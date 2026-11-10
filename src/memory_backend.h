#pragma once

#include "storage_backend.h"

namespace aowis::config::detail {

struct MemoryState;

[[nodiscard]] std::shared_ptr<const CollectionBackend> openMemoryBackend(
    std::shared_ptr<MemoryState> state, const std::string &collection,
    std::chrono::milliseconds lock_timeout);

} // namespace aowis::config::detail
