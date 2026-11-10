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
// ObjectCollection owns everything platform-independent: the JSON envelope,
// payload validation, revision checks, listing/unreadable classification and
// the erase/eraseCorrupt rules. A backend only moves opaque bytes for one
// collection of one owner. Every backend MUST guarantee:
//
//  1. Exclusive sessions. lock() returns a session holding a lock that
//     excludes every other session on the same collection, across threads
//     and (where the platform has processes sharing storage) across
//     processes. lock() waits at most the backend's configured timeout and
//     then fails with ErrorCode::lock_timeout; it never blocks indefinitely.
//  2. Atomic single-entry replacement. After writeEntry() returns success a
//     reader sees the complete new bytes; after a failure or a crash it sees
//     either the complete old bytes or the complete new bytes, never a mix.
//     create_only=true must fail with ErrorCode::conflict if the entry exists
//     and must never replace it.
//  3. Durability before success. writeEntry()/removeEntry() return success
//     only once the change survives the backend's documented failure model
//     (process crash for native files; the platform's persistence guarantee
//     for browser storage). An error after publication may leave the change
//     applied; callers re-read.
//  4. Faithful enumeration. entryNames() returns every stored entry whose
//     name ends in ".json", including names that are not valid object IDs,
//     so damaged/foreign entries are reported, never silently hidden.
//     Backend bookkeeping (locks, temporaries) never appears in the result.
//  5. Per-entry errors. readEntry() reports not_found, unsafe_path,
//     object_too_large, permission_denied or io_failure for one entry; the
//     caller turns these into UnreadableObject rows. Messages never contain
//     stored bytes.
//  6. Owner isolation. A backend instance is bound to one owner root and one
//     validated collection name; it can never address another owner.
//  7. Crash residue. lock(recover=true) removes the backend's own leftovers
//     of interrupted writes. Leftovers must be invisible to readers.
// ---------------------------------------------------------------------------

class CollectionSession {
public:
    virtual ~CollectionSession() = default;

    [[nodiscard]] virtual Result<std::vector<std::string>> entryNames() const = 0;
    [[nodiscard]] virtual Result<std::string> readEntry(const ObjectId &id) const = 0;
    [[nodiscard]] virtual Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) = 0;
    [[nodiscard]] virtual Result<bool> removeEntry(const ObjectId &id) = 0;
};

class CollectionBackend {
public:
    virtual ~CollectionBackend() = default;

    // recover=true additionally removes crash residue (contract item 7);
    // ObjectCollection requests it for open() and every mutating call.
    [[nodiscard]] virtual Result<std::unique_ptr<CollectionSession>> lock(bool recover) const = 0;
};

// The persistent backend of the current platform. Native POSIX today; on
// Windows and Emscripten this returns unsupported_platform until a backend
// meeting the contract above is implemented (selected in CMakeLists.txt).
[[nodiscard]] Result<std::shared_ptr<const CollectionBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, const std::string &collection,
    std::chrono::milliseconds lock_timeout);

} // namespace aowis::config::detail
