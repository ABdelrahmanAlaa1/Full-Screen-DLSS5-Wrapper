#pragma once
#include "effects/real/com.h"
#include "infrastructure/bounded_string.h"
#include "interior/units.h"

namespace real {

// What a file's version resource calls its product, which is the one thing about a file its name does not say.
using ProductName = infra::BoundedString<wchar_t, 64>;

// A file Windows says is signed, held open so it stays the file that was checked: the handle is shared for
// reading only, so nothing may write to it, delete it or rename it while the session holds it.
struct TrustedFile
{
    UniqueHandle handle;
    ProductName product; // what the file calls its product, or nothing when it says: read once the file is held, so it is this file's
};

// Verifies the Authenticode signature and that the certificate names NVIDIA, and fails without both; only
// then is the product name read, which is for the caller to judge. Revocation is not chased, which would
// mean a network call on a path that has to work offline.
[[nodiscard]] infra::Result<TrustedFile, Error> OpenTrusted(const interior::FilePath& path) noexcept;

} // namespace real
