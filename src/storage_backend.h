#pragma once

#include "aowis/config/object_id.h"
#include "aowis/config/result.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aowis::config::detail {

// Upper bound for one stored entry (envelope + payload), shared by every
// backend so a record accepted on one platform is accepted on all of them.
inline constexpr std::size_t max_record_bytes = 8U * 1024U * 1024U;

// ---------------------------------------------------------------------------
// Storage backend contract
//
// OwnerStore/ObjectCollection own everything platform-independent: the JSON
// envelope, payload validation, revision checks, listing/unreadable
// classification and the erase/eraseCorrupt rules. A backend only moves
// opaque bytes inside ONE owner root. Every backend MUST guarantee:
//
//  1. Exclusive owner sessions (read-write mode). lock() returns a session
//     that excludes every other read-write session on the same owner root,
//     across threads and (where processes share storage) across processes.
//     One lock covers all collections of the owner, so a caller can check
//     references in one collection and change another without a race.
//     lock() waits at most the configured timeout, then fails with
//     ErrorCode::lock_timeout; it never blocks indefinitely.
//  2. Atomic single-entry replacement. After writeEntry() succeeds a reader
//     sees the complete new bytes; after a failure or crash it sees the
//     complete old or the complete new bytes, never a mix. create_only=true
//     fails with ErrorCode::conflict if the entry exists and never replaces.
//     Because of this, read-only sessions need no lock.
//  3. Durability before success, under the backend's documented failure
//     model. An error after publication may leave the change applied.
//  4. Faithful enumeration: every stored "*.json" entry, including names
//     that are not object IDs; backend bookkeeping never appears.
//  5. Per-entry errors (not_found, unsafe_path, object_too_large,
//     permission_denied, io_failure) without stored bytes in messages.
//  6. Owner isolation: a backend instance can only address its owner root,
//     and collection names are validated before they reach the backend.
//  7. Crash residue: collection(name, recover=true) removes the backend's
//     own leftovers of interrupted writes in that collection; leftovers are
//     invisible to readers.
//  8. Read-only mode: never creates, locks, recovers or writes anything;
//     write calls fail with permission_denied. A missing collection reads as
//     empty. Ownership rules may be relaxed for administrator-managed roots
//     (e.g. root-owned, group-readable /etc), never for writability.
//
// Sessions and their collection handles are used on the thread that created
// them and must not outlive the OwnerSessionImpl they came from.
// ---------------------------------------------------------------------------

enum class BackendMode { read_write, read_only };

class CollectionSession {
public:
    virtual ~CollectionSession() = default;

    [[nodiscard]] virtual Result<std::vector<std::string>> entryNames() const = 0;
    [[nodiscard]] virtual Result<std::string> readEntry(const ObjectId &id) const = 0;
    [[nodiscard]] virtual Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) = 0;
    [[nodiscard]] virtual Result<bool> removeEntry(const ObjectId &id) = 0;
};

class OwnerSessionImpl {
public:
    virtual ~OwnerSessionImpl() = default;

    // Read-write: creates and verifies the collection; recover=true removes
    // crash residue (item 7). Read-only: opens it if present (item 8).
    [[nodiscard]] virtual Result<std::unique_ptr<CollectionSession>> collection(
        const std::string &name, bool recover) = 0;
};

class OwnerBackend {
public:
    virtual ~OwnerBackend() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<OwnerSessionImpl>> lock() const = 0;
    [[nodiscard]] virtual BackendMode mode() const noexcept = 0;
};

// The persistent backend of the current platform, selected in
// CMakeLists.txt: native POSIX, native Windows, or the browser backend on
// Emscripten.
[[nodiscard]] Result<std::shared_ptr<const OwnerBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, BackendMode mode, std::chrono::milliseconds lock_timeout);

} // namespace aowis::config::detail
