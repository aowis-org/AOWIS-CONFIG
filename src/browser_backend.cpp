// Browser (Emscripten) persistent backend: the key-value backend over
// window.localStorage, with a Web Locks API lease making exactly one tab per
// owner root the writer. Selected in CMakeLists.txt for Emscripten builds.
//
// Why localStorage: the library API is synchronous, and localStorage is the
// only synchronous persistent browser storage (IndexedDB/OPFS would need
// Asyncify/JSPI in every consumer). Configuration records are small; the
// usual ~5 MB per-origin quota is ample (exhaustion is reported as
// io_failure). WireGuard is disabled in browsers, so no keys are stored here.
//
// Why the lease is held for the page's lifetime: navigator.locks is
// asynchronous, but sessions are synchronous. The first read-write store of a
// tab requests the lock; once granted (after an event-loop turn) the tab
// keeps it until it is closed, and a waiting tab takes over automatically.
// Until then writes fail with lock_timeout and reads work. Without Web Locks
// (non-secure context; threaded Qt WASM always runs cross-origin isolated,
// hence secure) writes are refused: fail closed.
//
// localStorage and navigator.locks exist only on the browser main thread. In
// threaded builds every access is proxied there synchronously. The JS bodies
// below are also executed by tests/browser_storage_glue_test.mjs under Node
// with fake browser APIs; keep them self-contained.
#include "key_value_backend.h"

#include <emscripten.h>
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif

#include <cstdlib>
#include <functional>
#include <string>
#include <utility>

// Results are malloc'ed UTF-8: first byte 'v' + value, 'e' + error kind, or
// a null pointer when the key does not exist.
EM_JS(char *, aowis_config_ls_get, (const char *key), {
    try {
        const value = globalThis.localStorage.getItem(UTF8ToString(key));
        return value === null ? 0 : stringToNewUTF8("v" + value);
    } catch (error) {
        return stringToNewUTF8("eunavailable");
    }
});

// 0 stored, 1 exists (create_only), 2 quota exceeded, 3 storage unavailable.
EM_JS(int, aowis_config_ls_put, (const char *key, const char *value, int create_only), {
    try {
        const name = UTF8ToString(key);
        if (create_only && globalThis.localStorage.getItem(name) !== null) {
            return 1;
        }
        globalThis.localStorage.setItem(name, UTF8ToString(value));
        return 0;
    } catch (error) {
        return error && (error.name === "QuotaExceededError" || error.code === 22) ? 2 : 3;
    }
});

// 0 removed, 3 storage unavailable, 4 not found.
EM_JS(int, aowis_config_ls_remove, (const char *key), {
    try {
        const name = UTF8ToString(key);
        if (globalThis.localStorage.getItem(name) === null) {
            return 4;
        }
        globalThis.localStorage.removeItem(name);
        return 0;
    } catch (error) {
        return 3;
    }
});

// 'v' + keys starting with prefix, separated by U+001F; keys containing
// U+001F are skipped (never produced by this library).
EM_JS(char *, aowis_config_ls_keys, (const char *prefix), {
    try {
        const wanted = UTF8ToString(prefix);
        const storage = globalThis.localStorage;
        const found = [];
        for (let index = 0; index < storage.length; ++index) {
            const name = storage.key(index);
            if (name !== null && name.startsWith(wanted) && name.indexOf("\u001f") < 0) {
                found.push(name);
            }
        }
        return stringToNewUTF8("v" + found.join("\u001f"));
    } catch (error) {
        return stringToNewUTF8("eunavailable");
    }
});

// 0 held, 1 pending, 2 unavailable. Requests the lock on first call per root
// and keeps it until the page is closed (the callback's promise never settles).
EM_JS(int, aowis_config_lease, (const char *root), {
    const name = UTF8ToString(root);
    const leases = globalThis.__aowisConfigLeases || (globalThis.__aowisConfigLeases = {});
    if (!(name in leases)) {
        const locks = globalThis.navigator && globalThis.navigator.locks;
        if (!locks || typeof locks.request !== "function") {
            leases[name] = 2;
        } else {
            leases[name] = 1;
            try {
                locks.request("aowis-config:" + name, { mode: "exclusive" }, () => {
                    leases[name] = 0;
                    return new Promise(() => {});
                }).catch(() => { leases[name] = 2; });
            } catch (error) {
                leases[name] = 2;
            }
        }
    }
    return leases[name];
});

namespace aowis::config::detail {
namespace {

#if defined(__EMSCRIPTEN_PTHREADS__)
void runTask(void *task)
{
    (*static_cast<std::function<void()> *>(task))();
}
#endif

// Runs a JS accessor on the browser main thread and waits for it.
bool onMainThread(std::function<void()> task)
{
#if defined(__EMSCRIPTEN_PTHREADS__)
    if (!emscripten_is_main_runtime_thread()) {
        return emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                                     runTask, &task) == 1;
    }
#endif
    task();
    return true;
}

Error unavailable()
{
    return {ErrorCode::io_failure, "Browser localStorage is unavailable"};
}

// Takes ownership of a malloc'ed tagged result.
Result<std::optional<std::string>> tagged(char *raw)
{
    if (raw == nullptr) return Result<std::optional<std::string>>::success(std::nullopt);
    std::string text(raw);
    std::free(raw);
    if (text.empty() || text.front() != 'v') return Result<std::optional<std::string>>::failure(unavailable());
    return Result<std::optional<std::string>>::success(text.substr(1));
}

class LocalStorageStore final : public KeyValueStore {
public:
    [[nodiscard]] Result<std::optional<std::string>> get(const std::string &key) const override
    {
        char *raw = nullptr;
        if (!onMainThread([&key, &raw]() { raw = aowis_config_ls_get(key.c_str()); })) {
            return Result<std::optional<std::string>>::failure(unavailable());
        }
        return tagged(raw);
    }

    [[nodiscard]] Result<bool> put(const std::string &key, std::string_view value, bool create_only) override
    {
        const std::string text(value);
        int status = 3;
        if (!onMainThread([&key, &text, create_only, &status]() {
                status = aowis_config_ls_put(key.c_str(), text.c_str(), create_only ? 1 : 0);
            })) {
            return Result<bool>::failure(unavailable());
        }
        if (status == 0) return Result<bool>::success(true);
        if (status == 1) return Result<bool>::failure({ErrorCode::conflict, "Object ID already exists"});
        if (status == 2) return Result<bool>::failure({ErrorCode::io_failure, "Browser storage quota exceeded"});
        return Result<bool>::failure(unavailable());
    }

    [[nodiscard]] Result<bool> remove(const std::string &key) override
    {
        int status = 3;
        if (!onMainThread([&key, &status]() { status = aowis_config_ls_remove(key.c_str()); })) {
            return Result<bool>::failure(unavailable());
        }
        if (status == 0) return Result<bool>::success(true);
        if (status == 4) return Result<bool>::failure({ErrorCode::not_found, "Object does not exist"});
        return Result<bool>::failure(unavailable());
    }

    [[nodiscard]] Result<std::vector<std::string>> keys(const std::string &prefix) const override
    {
        char *raw = nullptr;
        if (!onMainThread([&prefix, &raw]() { raw = aowis_config_ls_keys(prefix.c_str()); })) {
            return Result<std::vector<std::string>>::failure(unavailable());
        }
        Result<std::optional<std::string>> joined = tagged(raw);
        if (!joined || !joined.value().has_value()) {
            return Result<std::vector<std::string>>::failure(unavailable());
        }
        std::vector<std::string> names;
        const std::string &text = *joined.value();
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = text.find('\x1f', start);
            names.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return Result<std::vector<std::string>>::success(std::move(names));
    }

    [[nodiscard]] LeaseState lease(const std::string &root) override
    {
        int state = 2;
        if (!onMainThread([&root, &state]() { state = aowis_config_lease(root.c_str()); })) {
            return LeaseState::unavailable;
        }
        if (state == 0) return LeaseState::held;
        if (state == 1) return LeaseState::pending;
        return LeaseState::unavailable;
    }
};

std::shared_ptr<KeyValueStore> localStorageStore()
{
    // One store per page: in-tab owner mutexes must be shared by every
    // OwnerStore of the same root.
    static const std::shared_ptr<KeyValueStore> store = std::make_shared<LocalStorageStore>();
    return store;
}
} // namespace

Result<std::shared_ptr<const OwnerBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, BackendMode mode, std::chrono::milliseconds lock_timeout)
{
    return Result<std::shared_ptr<const OwnerBackend>>::success(
        openKeyValueBackend(localStorageStore(), owner_root.generic_string(), mode, lock_timeout));
}

} // namespace aowis::config::detail
