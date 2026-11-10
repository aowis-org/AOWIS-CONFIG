// Placeholder persistent backend for Windows and Emscripten, selected in
// CMakeLists.txt. A real backend replaces this file per platform and must
// meet the contract in storage_backend.h. MemoryStorage works everywhere.
#include "storage_backend.h"

namespace aowis::config::detail {

Result<std::shared_ptr<const OwnerBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, BackendMode mode, std::chrono::milliseconds lock_timeout)
{
    static_cast<void>(owner_root);
    static_cast<void>(mode);
    static_cast<void>(lock_timeout);
    return Result<std::shared_ptr<const OwnerBackend>>::failure(
        {ErrorCode::unsupported_platform, "No persistent storage backend is implemented for this platform"});
}

} // namespace aowis::config::detail
