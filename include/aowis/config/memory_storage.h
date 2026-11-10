#pragma once

#include "aowis/config/result.h"

#include <memory>
#include <string>
#include <string_view>

namespace aowis::config {

namespace detail {
struct MemoryState;
}

// Non-persistent, in-process storage standing in for one owner root. Copies
// of a MemoryStorage share the same contents (handle semantics) and all
// operations are thread-safe. Use OwnerStore::openInMemory() to work with
// it. It is available on
// every platform, so consumer unit tests (e.g. GUI settings widgets) need no
// filesystem, and it is the reference implementation of the backend contract.
// Nothing is ever written to disk; contents vanish with the last handle.
class MemoryStorage final {
public:
    MemoryStorage();

    // Test-fixture helpers. writeRawEntry() stores bytes under an entry name
    // (e.g. "<uuidv7>.json" or "foreign.json") bypassing every validation, to
    // simulate damaged or foreign records. readRawEntry() returns stored bytes
    // verbatim. Collection names follow ObjectCollection rules.
    [[nodiscard]] Result<bool> writeRawEntry(std::string_view collection, std::string_view entry_name,
                                             std::string_view bytes) const;
    [[nodiscard]] Result<std::string> readRawEntry(std::string_view collection,
                                                   std::string_view entry_name) const;

private:
    std::shared_ptr<detail::MemoryState> state_;

    friend class OwnerStore;
};

} // namespace aowis::config
