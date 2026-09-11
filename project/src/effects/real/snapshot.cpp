#include "effects/real/snapshot.h"

#include "infrastructure/text.h"

#include <wincodec.h>

#include <array>
#include <cwchar>
#include <optional>
#include <ranges>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

constexpr std::uint32_t kMaxAttempts = 1000;
constexpr wchar_t kOriginalSuffix[] = L"_original.png";
constexpr wchar_t kProcessedSuffix[] = L"_processed.png";
constexpr wchar_t kReadbackName[] = L"Snapshot readback";

struct Files
{
    interior::FilePath original;
    interior::FilePath processed;
};

// How a texture's rows lie in the buffer they are copied to: each row padded to the alignment the copy needs.
struct Layout
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    std::uint64_t bytes;
};

// A texture copied out of the GPU, and where its rows are.
struct Readback
{
    Com<ID3D12Resource> buffer;
    Layout layout;
};

struct Readbacks
{
    Readback original;
    Readback processed;
};

struct Copied
{
    Readbacks readbacks;
    interior::FenceValue fence;
};

[[nodiscard]] interior::CaptureMoment MomentNow() noexcept
{
    SYSTEMTIME now{}; // WAIVER(R2): the answer of one query, read once after it.
    ::GetLocalTime(&now);
    return interior::CaptureMoment{ now.wMonth, now.wDay, now.wHour, now.wMinute };
}

// The folder is made when it is not there; one that is there already is what was wanted.
[[nodiscard]] Status<Error> EnsureFolder(const interior::DirectoryPath& folder) noexcept
{
    if (::CreateDirectoryW(folder.CString(), nullptr) != FALSE || ::GetLastError() == ERROR_ALREADY_EXISTS)
        return {};
    return Fail(LastError(ApiCall::CreateCaptureFolder));
}

[[nodiscard]] Result<interior::FilePath, Error> PathOf(const interior::DirectoryPath& folder, const interior::CaptureStem& stem, const wchar_t* suffix) noexcept
{
    const std::array<wchar_t, interior::CaptureStem::Capacity + 1> name = infra::WidenedChars<interior::CaptureStem::Capacity + 1>(stem.Get());
    std::array<wchar_t, interior::FilePath::Capacity + 1> path{}; // WAIVER(R2): a local buffer filled once, before use.
    if (::_snwprintf_s(path.data(), path.size(), _TRUNCATE, L"%s\\%s%s", folder.CString(), name.data(), suffix) < 0)
        return Fail(Error{ ApiCall::CapturePathTooLong, 0 });
    return interior::FilePath::Parse(path.data()).transform_error([](infra::StringTooLong) { return Error{ ApiCall::CapturePathTooLong, 0 }; });
}

[[nodiscard]] bool Exists(const interior::FilePath& path) noexcept
{
    return ::GetFileAttributesW(path.CString()) != INVALID_FILE_ATTRIBUTES;
}

// Both files of a capture take the same count, so the pair stays a pair: a count is free only when neither
// file with it is there. A name that cannot be made ends the search with its own error.
[[nodiscard]] Result<Files, Error> FreeNames(const interior::DirectoryPath& folder, const interior::CaptureStem& stem) noexcept
{
    static constexpr auto NamesAt = [] [[nodiscard]] (const interior::DirectoryPath& folder, const interior::CaptureStem& stem, std::uint32_t attempt) noexcept -> Result<Files, Error> {
        const interior::CaptureStem numbered = interior::NumberedStem(stem, attempt);
        return PathOf(folder, numbered, kOriginalSuffix).and_then([&](const interior::FilePath& original) {
            return PathOf(folder, numbered, kProcessedSuffix).transform([&](const interior::FilePath& processed) { return Files{ original, processed }; });
        });
    };

    static constexpr auto IsFree = [] [[nodiscard]] (const Result<Files, Error>& names) noexcept -> bool { return !names.has_value() || (!Exists(names->original) && !Exists(names->processed)); };
    const auto attempts = std::views::iota(std::uint32_t{ 1 }, kMaxAttempts + 1);
    const auto found = std::ranges::find_if(attempts, [&](std::uint32_t attempt) { return IsFree(NamesAt(folder, stem, attempt)); });
    if (found == attempts.end())
        return Fail(Error{ ApiCall::CaptureNameTaken, 0 });
    return NamesAt(folder, stem, *found);
}

[[nodiscard]] Layout LayoutOf(const GpuDevice& gpu, ID3D12Resource* texture) noexcept
{
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; // WAIVER(R2): answer records filled once by the query below.
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    return Layout{ footprint, total };
}

// The texture is handed to the copy and given back in the state the frame left it in, so the next frame's
// plan finds it where it expects. One already readable by a copy needs no handing over.
void RecordCopyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, ID3D12Resource* buffer, const Layout& layout, interior::ResourceState state) noexcept
{
    // WAIVER(R1): the two location records hold a union, which cannot be designated, so every field is assigned by name.
    static constexpr auto TextureLocation = [] [[nodiscard]] (ID3D12Resource * texture) noexcept -> D3D12_TEXTURE_COPY_LOCATION {
        D3D12_TEXTURE_COPY_LOCATION location{}; // WAIVER(R2): a request record filled once, before use.
        location.pResource = texture;
        location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        location.SubresourceIndex = 0;
        return location;
    };

    static constexpr auto BufferLocation = [] [[nodiscard]] (ID3D12Resource * buffer, const Layout& layout) noexcept -> D3D12_TEXTURE_COPY_LOCATION {
        D3D12_TEXTURE_COPY_LOCATION location{}; // WAIVER(R2): a request record filled once, before use.
        location.pResource = buffer;
        location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        location.PlacedFootprint = layout.footprint;
        return location;
    };

    static constexpr auto Handed = [](ID3D12GraphicsCommandList* list, ID3D12Resource* texture, interior::ResourceState from, interior::ResourceState to) noexcept -> void {
        if (from != to)
            RecordBarrier(list, texture, from, to);
    };
    const D3D12_TEXTURE_COPY_LOCATION source = TextureLocation(texture);
    const D3D12_TEXTURE_COPY_LOCATION destination = BufferLocation(buffer, layout);
    Handed(list, texture, state, interior::ResourceState::CopySource);
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Handed(list, texture, interior::ResourceState::CopySource, state);
}

[[nodiscard]] Result<Readback, Error> CopiedOut(const Gpu& gpu, const interior::ResourceId& id, const interior::StateTable& states) noexcept
{
    return Lookup(gpu.resources, id).and_then([&](ID3D12Resource* texture) {
        const Layout layout = LayoutOf(gpu.device, texture);
        return CreateBuffer(gpu.device, interior::ByteCountTag::Parse(static_cast<std::uint32_t>(layout.bytes)), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                            kReadbackName)
            .transform([&](const Com<ID3D12Resource>& buffer) {
                RecordCopyOut(gpu.list.Get(), texture, buffer.Get(), layout, interior::StateOf(states, id));
                return Readback{ buffer, layout };
            });
    });
}

// Both copies go into one list after the frame's own, and the list is waited for, so the buffers hold the
// pictures by the time they are read.
[[nodiscard]] Result<Copied, Error> CopiedBoth(const Gpu& gpu, const FrameContext& frame, const interior::FrameState& after) noexcept
{
    return OpenList(gpu, frame.slot)
        .and_then([&] { return CopiedOut(gpu, interior::SimpleId(interior::ResourceKind::ModelColor), after.states); })
        .and_then([&](const Readback& original) {
            return CopiedOut(gpu, interior::SimpleId(after.displaySource), after.states).transform([&](const Readback& processed) { return Readbacks{ original, processed }; });
        })
        .and_then([&](const Readbacks& readbacks) { return FlushList(gpu, frame.fence).transform([&readbacks](interior::FenceValue fence) { return Copied{ readbacks, fence }; }); });
}

// WAIVER(R12): DXGI's formats are an open set; the ones a picture of ours can be in are named, and the rest refused.
[[nodiscard]] Result<GUID, Error> WicFormatOf(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return GUID_WICPixelFormat32bppRGBA;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return GUID_WICPixelFormat32bppBGRA;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return GUID_WICPixelFormat64bppRGBAHalf;
    default: return Fail(Error{ ApiCall::SnapshotFormat, static_cast<std::uint32_t>(format) });
    }
}

// The rows as copied become a WIC bitmap, converted to plain 24-bit colour, and encoded as PNG at the
// encoder's own compression, which is what "standard" means to it.
[[nodiscard]] Status<Error> WritePng(IWICImagingFactory* wic, const Readback& readback, void* pixels, const interior::FilePath& path) noexcept
{
    static constexpr auto BitmapOf = [] [[nodiscard]] (IWICImagingFactory * wic, const Readback& r, void* pixels, const GUID& format) noexcept -> Result<Com<IWICBitmap>, Error> {
        const D3D12_SUBRESOURCE_FOOTPRINT& f = r.layout.footprint.Footprint;
        Com<IWICBitmap> bitmap; // WAIVER(R2): the answer of one call, read once after it.
        return Check(wic->CreateBitmapFromMemory(f.Width, f.Height, format, f.RowPitch, static_cast<UINT>(r.layout.bytes), static_cast<BYTE*>(pixels), &bitmap), ApiCall::WicCreateBitmap)
            .transform([&bitmap] { return bitmap; });
    };

    static constexpr auto ConvertedOf = [] [[nodiscard]] (IWICImagingFactory * wic, const Com<IWICBitmap>& bitmap) noexcept -> Result<Com<IWICFormatConverter>, Error> {
        Com<IWICFormatConverter> converter; // WAIVER(R2): the answer of one call, read once after it.
        return Check(wic->CreateFormatConverter(&converter), ApiCall::WicConvertPixels)
            .and_then(
                [&] { return Check(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), ApiCall::WicConvertPixels); })
            .transform([&converter] { return converter; });
    };

    static constexpr auto StreamOf = [] [[nodiscard]] (IWICImagingFactory * wic, const interior::FilePath& path) noexcept -> Result<Com<IWICStream>, Error> {
        Com<IWICStream> stream; // WAIVER(R2): the answer of one call, read once after it.
        return Check(wic->CreateStream(&stream), ApiCall::WicOpenFile)
            .and_then([&] { return Check(stream->InitializeFromFilename(path.CString(), GENERIC_WRITE), ApiCall::WicOpenFile); })
            .transform([&stream] { return stream; });
    };

    static constexpr auto EncoderOf = [] [[nodiscard]] (IWICImagingFactory * wic, const Com<IWICStream>& stream) noexcept -> Result<Com<IWICBitmapEncoder>, Error> {
        Com<IWICBitmapEncoder> encoder; // WAIVER(R2): the answer of one call, read once after it.
        return Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), ApiCall::WicCreateEncoder)
            .and_then([&] { return Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), ApiCall::WicCreateEncoder); })
            .transform([&encoder] { return encoder; });
    };

    static constexpr auto FrameWritten = [] [[nodiscard]] (const Com<IWICBitmapEncoder>& encoder, const Com<IWICFormatConverter>& picture, const Readback& r) noexcept -> Status<Error> {
        static constexpr auto FrameOf = [] [[nodiscard]] (const Com<IWICBitmapEncoder>& encoder) noexcept -> Result<Com<IWICBitmapFrameEncode>, Error> {
            Com<IWICBitmapFrameEncode> frame; // WAIVER(R2): the answers of one call, read once after it.
            Com<IPropertyBag2> options;
            return Check(encoder->CreateNewFrame(&frame, &options), ApiCall::WicWriteFrame)
                .and_then([&] { return Check(frame->Initialize(options.Get()), ApiCall::WicWriteFrame); })
                .transform([&frame] { return frame; });
        };

        static constexpr auto Described = [] [[nodiscard]] (const Com<IWICBitmapFrameEncode>& frame, const Readback& r) noexcept -> Status<Error> {
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR; // WAIVER(R2): the call may answer with the format it settled on.
            return Check(frame->SetSize(r.layout.footprint.Footprint.Width, r.layout.footprint.Footprint.Height), ApiCall::WicWriteFrame).and_then([&] {
                return Check(frame->SetPixelFormat(&format), ApiCall::WicWriteFrame);
            });
        };
        return FrameOf(encoder).and_then([&](const Com<IWICBitmapFrameEncode>& frame) {
            return Described(frame, r)
                .and_then([&] { return Check(frame->WriteSource(picture.Get(), nullptr), ApiCall::WicWriteFrame); })
                .and_then([&] { return Check(frame->Commit(), ApiCall::WicWriteFrame); })
                .and_then([&] { return Check(encoder->Commit(), ApiCall::WicWriteFrame); });
        });
    };
    return WicFormatOf(readback.layout.footprint.Footprint.Format).and_then([&](const GUID& format) {
        return BitmapOf(wic, readback, pixels, format).and_then([&](const Com<IWICBitmap>& bitmap) {
            return ConvertedOf(wic, bitmap).and_then([&](const Com<IWICFormatConverter>& picture) {
                return StreamOf(wic, path).and_then(
                    [&](const Com<IWICStream>& stream) { return EncoderOf(wic, stream).and_then([&](const Com<IWICBitmapEncoder>& encoder) { return FrameWritten(encoder, picture, readback); }); });
            });
        });
    });
}

// The buffer is mapped for as long as the file takes, and unmapped whether or not the file was written.
[[nodiscard]] Status<Error> WrittenOut(IWICImagingFactory* wic, const Readback& readback, const interior::FilePath& path) noexcept
{
    static constexpr auto WrittenThenUnmapped = [] [[nodiscard]] (IWICImagingFactory * wic, const Readback& readback, void* mapped, const interior::FilePath& path) noexcept -> Status<Error> {
        const Status<Error> written = WritePng(wic, readback, mapped, path);
        const D3D12_RANGE noWrite{ 0, 0 };
        readback.buffer->Unmap(0, &noWrite);
        return written;
    };
    void* mapped = nullptr; // WAIVER(R2): the answer of one call, read once after it.
    const D3D12_RANGE range{ 0, static_cast<SIZE_T>(readback.layout.bytes) };
    return Check(readback.buffer->Map(0, &range, &mapped), ApiCall::MapResource).and_then([&] { return WrittenThenUnmapped(wic, readback, mapped, path); });
}

[[nodiscard]] Result<Com<IWICImagingFactory>, Error> WicFactory() noexcept
{
    Com<IWICImagingFactory> factory; // WAIVER(R2): the answer of one call, read once after it.
    return Check(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), ApiCall::WicCreateFactory).transform([&factory] { return factory; });
}

} // namespace

Result<Snapshot, Error> SaveSnapshot(const Gpu& gpu, const FrameContext& frame, const interior::FrameState& after, const SnapshotOrder& order) noexcept
{
    static constexpr auto WrittenBoth = [] [[nodiscard]] (const Readbacks& r, const Files& files) noexcept -> Status<Error> {
        return WicFactory().and_then(
            [&](const Com<IWICImagingFactory>& wic) { return WrittenOut(wic.Get(), r.original, files.original).and_then([&] { return WrittenOut(wic.Get(), r.processed, files.processed); }); });
    };
    return EnsureFolder(order.folder)
        .and_then([&] { return FreeNames(order.folder, interior::CaptureStemOf(order.label, order.live, order.everything, MomentNow())); })
        .and_then([&](const Files& files) {
            return CopiedBoth(gpu, frame, after).and_then([&](const Copied& copied) {
                return WrittenBoth(copied.readbacks, files).transform([&] { return Snapshot{ files.original, files.processed, copied.fence }; });
            });
        });
}

} // namespace real
