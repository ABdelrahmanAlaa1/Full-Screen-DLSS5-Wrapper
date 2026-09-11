#include "effects/real/trust.h"

#include <softpub.h>
#include <wintrust.h>
#include <winver.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string_view>
#include <vector>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

// WINTRUST_ACTION_GENERIC_VERIFY_V2 is a macro over an initialiser, so it is named once here. The call
// takes a mutable pointer to it and does not write through it.
GUID kVerifyAction = WINTRUST_ACTION_GENERIC_VERIFY_V2; // WAIVER(R2): a constant the API insists on being handed by non-const pointer.
constexpr std::wstring_view kSigner = L"NVIDIA";
constexpr std::size_t kNameCapacity = 256;
constexpr std::size_t kKeyCapacity = 64;

// Each check has a call for each of the files, so a refusal names the file it is about.
[[nodiscard]] ApiCall OpenCall(ModelKind kind) noexcept
{
    return kind == ModelKind::NeuralRendering ? ApiCall::OpenModelFile : ApiCall::OpenUpscalerFile;
}

[[nodiscard]] ApiCall NotSignedCall(ModelKind kind) noexcept
{
    return kind == ModelKind::NeuralRendering ? ApiCall::ModelNotSigned : ApiCall::UpscalerNotSigned;
}

[[nodiscard]] ApiCall NotFromNvidiaCall(ModelKind kind) noexcept
{
    return kind == ModelKind::NeuralRendering ? ApiCall::ModelNotFromNvidia : ApiCall::UpscalerNotFromNvidia;
}

// One entry of a version resource's translation table: which language its strings are kept under.
struct Translation
{
    WORD language;
    WORD codePage;
};

// What the file's version resource calls its product. Read by path once the file is held, which is safe
// because the handle's share mode keeps the file from being written, deleted or renamed under the path.
[[nodiscard]] ProductName ProductNameOf(const wchar_t* path) noexcept
{
    static constexpr auto VersionBlock = [] [[nodiscard]] (const wchar_t* path) noexcept -> std::vector<std::byte> {
        DWORD ignored = 0; // WAIVER(R2): an out-parameter the API insists on, which it always sets to zero.
        const DWORD size = ::GetFileVersionInfoSizeW(path, &ignored);
        if (size == 0)
            return {};
        std::vector<std::byte> block(size); // WAIVER(R2): a buffer the API fills once, before use.
        if (::GetFileVersionInfoW(path, 0, size, block.data()) == FALSE)
            return {};
        return block;
    };

    // The strings are kept per language; the first language listed is the one asked.
    static constexpr auto FirstTranslation = [] [[nodiscard]] (const std::vector<std::byte>& block) noexcept -> std::optional<Translation> {
        void* found = nullptr; // WAIVER(R2): the answer of one query, read once after it.
        UINT length = 0;
        if (block.empty() || ::VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", &found, &length) == FALSE || length < sizeof(Translation))
            return std::nullopt;
        Translation first{ 0, 0 }; // WAIVER(R2): copied out of the block, whose alignment is the API's to promise.
        std::memcpy(&first, found, sizeof(Translation));
        return first;
    };

    static constexpr auto NamedProduct = [] [[nodiscard]] (const std::vector<std::byte>& block, const Translation& translation) noexcept -> ProductName {
        static constexpr auto KeyOf = [] [[nodiscard]] (const Translation& translation) noexcept -> std::array<wchar_t, kKeyCapacity> {
            std::array<wchar_t, kKeyCapacity> key{}; // WAIVER(R2): a local buffer filled once, before use.
            (void)::_snwprintf_s(key.data(), key.size(), _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\ProductName", static_cast<unsigned int>(translation.language),
                                 static_cast<unsigned int>(translation.codePage));
            return key;
        };
        void* found = nullptr; // WAIVER(R2): the answer of one query, read once after it.
        UINT length = 0;
        if (::VerQueryValueW(block.data(), KeyOf(translation).data(), &found, &length) == FALSE || length == 0)
            return ProductName{};
        const wchar_t* text = static_cast<const wchar_t*>(found);
        return ProductName::Parse(std::wstring_view(text, ::wcsnlen(text, length))).value_or(ProductName{});
    };
    const std::vector<std::byte> block = VersionBlock(path);
    const std::optional<Translation> translation = FirstTranslation(block);
    if (!translation.has_value())
        return ProductName{};
    return NamedProduct(block, *translation);
}

} // namespace

Result<TrustedFile, Error> OpenTrusted(const interior::FilePath& path, ModelKind kind) noexcept
{
    // Shared for reading only, so nothing else may write to the file, delete it or rename it while it is held.
    static constexpr auto OpenForReading = [] [[nodiscard]] (const wchar_t* path, ModelKind kind) noexcept -> Result<UniqueHandle, Error> {
        void* handle = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return Fail(LastError(OpenCall(kind)));
        return UniqueHandle(handle);
    };

    static constexpr auto Verified = [] [[nodiscard]] (const wchar_t* path, void* handle, ModelKind kind) noexcept -> Status<Error> {
        static constexpr auto FileInfoFor = [] [[nodiscard]] (const wchar_t* path, void* handle) noexcept -> WINTRUST_FILE_INFO {
            return WINTRUST_FILE_INFO{ .cbStruct = sizeof(WINTRUST_FILE_INFO), .pcwszFilePath = path, .hFile = handle, .pgKnownSubject = nullptr };
        };

        // Asked of the handle already held rather than of the path, so it cannot be answered about another file.
        // WAIVER(R1): every field is named, including the ones that want nothing, which is the point of it.
        static constexpr auto RequestFor = [] [[nodiscard]] (WINTRUST_FILE_INFO * file) noexcept -> WINTRUST_DATA {
            WINTRUST_DATA request{}; // WAIVER(R2): a request record filled once, before it is asked.
            request.cbStruct = sizeof(WINTRUST_DATA);
            request.pPolicyCallbackData = nullptr;
            request.pSIPClientData = nullptr;
            request.dwUIChoice = WTD_UI_NONE;
            request.fdwRevocationChecks = WTD_REVOKE_NONE;
            request.dwUnionChoice = WTD_CHOICE_FILE;
            request.pFile = file;
            request.dwStateAction = WTD_STATEACTION_VERIFY;
            request.hWVTStateData = nullptr;
            request.pwszURLReference = nullptr;
            request.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;
            request.dwUIContext = 0;
            return request;
        };

        static constexpr auto Answered = [] [[nodiscard]] (WINTRUST_DATA & request, ModelKind kind) noexcept -> Status<Error> {
            static constexpr auto SignedByNvidia = [] [[nodiscard]] (HANDLE state, LONG verdict, ModelKind kind) noexcept -> Status<Error> {
                static constexpr auto FromNvidia = [] [[nodiscard]] (HANDLE state, ModelKind kind) noexcept -> Status<Error> {
                    static constexpr auto NamesNvidia = [] [[nodiscard]] (HANDLE state) noexcept -> bool {
                        static constexpr auto SigningCertificate = [] [[nodiscard]] (HANDLE state) noexcept -> const CERT_CONTEXT* {
                            static constexpr auto SignerOf = [] [[nodiscard]] (CRYPT_PROVIDER_DATA * provider) noexcept -> CRYPT_PROVIDER_SGNR* {
                                return provider == nullptr ? nullptr : ::WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
                            };

                            static constexpr auto CertificateOf = [] [[nodiscard]] (CRYPT_PROVIDER_SGNR * signer) noexcept -> CRYPT_PROVIDER_CERT* {
                                return signer == nullptr ? nullptr : ::WTHelperGetProvCertFromChain(signer, 0);
                            };
                            CRYPT_PROVIDER_CERT* certificate = CertificateOf(SignerOf(::WTHelperProvDataFromStateData(state)));
                            return certificate == nullptr ? nullptr : certificate->pCert;
                        };

                        static constexpr auto NameOfCertificate = [] [[nodiscard]] (const CERT_CONTEXT* certificate) noexcept -> std::array<wchar_t, kNameCapacity> {
                            std::array<wchar_t, kNameCapacity> name{}; // WAIVER(R2): a local buffer filled once, before use.
                            (void)::CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), kNameCapacity);
                            return name;
                        };
                        const CERT_CONTEXT* certificate = SigningCertificate(state);
                        if (certificate == nullptr)
                            return false;
                        const std::array<wchar_t, kNameCapacity> name = NameOfCertificate(certificate);
                        return std::wstring_view(name.data()).starts_with(kSigner);
                    };
                    if (!NamesNvidia(state))
                        return Fail(Error{ NotFromNvidiaCall(kind), 0 });
                    return {};
                };
                if (verdict != ERROR_SUCCESS)
                    return Fail(Error{ NotSignedCall(kind), static_cast<std::uint32_t>(verdict) });
                return FromNvidia(state, kind);
            };

            // The verification allocates state that has to be given back whatever the answer was.
            static constexpr auto CloseVerification = [](WINTRUST_DATA& request) noexcept -> void {
                request.dwStateAction = WTD_STATEACTION_CLOSE;
                (void)::WinVerifyTrust(nullptr, &kVerifyAction, &request);
            };
            const LONG verdict = ::WinVerifyTrust(nullptr, &kVerifyAction, &request);
            const Status<Error> answer = SignedByNvidia(request.hWVTStateData, verdict, kind);
            CloseVerification(request);
            return answer;
        };
        WINTRUST_FILE_INFO file = FileInfoFor(path, handle);
        WINTRUST_DATA request = RequestFor(&file); // WAIVER(R2): the call writes its state into the record it is given.
        return Answered(request, kind);
    };
    // The product name is read only of a file that has passed, and says nothing about whether it passed.
    return OpenForReading(path.CString(), kind).and_then([&path, kind](UniqueHandle handle) {
        return Verified(path.CString(), handle.get(), kind).transform([&path, &handle] { return TrustedFile{ std::move(handle), ProductNameOf(path.CString()) }; });
    });
}

} // namespace real
