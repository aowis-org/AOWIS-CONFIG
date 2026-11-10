// Placeholder persistent backend for Windows and Emscripten, selected in
// CMakeLists.txt. A real backend replaces this file per platform and must
// meet the contract in storage_backend.h. MemoryStorage works everywhere.
#include "storage_backend.h"

namespace aowis::config::detail {

Result<std::shared_ptr<const CollectionBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, const std::string &collection,
    std::chrono::milliseconds lock_timeout)
{
    static_cast<void>(owner_root);
    static_cast<void>(collection);
    static_cast<void>(lock_timeout);
    return Result<std::shared_ptr<const CollectionBackend>>::failure(
        {ErrorCode::unsupported_platform, "No persistent storage backend is implemented for this platform"});
}

} // namespace aowis::config::detail
