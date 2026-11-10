#pragma once

#include "storage_backend.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aowis::config::detail {

// Cross-context writer lease for one owner root. In a browser every tab is a
// separate context sharing one localStorage; the lease (Web Locks API, held
// for the page's lifetime) makes exactly one tab the writer.
enum class LeaseState {
    held,        // This context may write.
    pending,     // Another context holds it, or it has not been granted yet.
    unavailable, // No cross-context locking: writes are refused (fail closed).
};

// A flat, string-keyed store with atomic single-key writes (browser
// localStorage, or a fake in tests). Keys and values are UTF-8.
class KeyValueStore {
public:
    virtual ~KeyValueStore() = default;

    [[nodiscard]] virtual Result<std::optional<std::string>> get(const std::string &key) const = 0;
    // create_only: conflict if the key exists. Quota exhaustion: io_failure.
    [[nodiscard]] virtual Result<bool> put(const std::string &key, std::string_view value, bool create_only) = 0;
    [[nodiscard]] virtual Result<bool> remove(const std::string &key) = 0; // not_found if absent
    [[nodiscard]] virtual Result<std::vector<std::string>> keys(const std::string &prefix) const = 0;
    // Starts acquisition on first call for a root and reports the state.
    [[nodiscard]] virtual LeaseState lease(const std::string &root) = 0;

    // In-process exclusion between threads of this context, per owner root.
    [[nodiscard]] std::shared_ptr<std::mutex> ownerMutex(const std::string &root);

private:
    std::mutex guard_;
    std::map<std::string, std::shared_ptr<std::mutex>> mutexes_;
};

// OwnerBackend over a KeyValueStore. Key layout:
//   aowis-config:<owner root>/<collection>/<entry>
// e.g. aowis-config:/aowis/gui/connections/<uuidv7>.json
[[nodiscard]] std::shared_ptr<const OwnerBackend> openKeyValueBackend(
    std::shared_ptr<KeyValueStore> store, std::string owner_root, BackendMode mode,
    std::chrono::milliseconds lock_timeout);

} // namespace aowis::config::detail
