#pragma once

#include "storage_backend.h"

namespace aowis::config::detail {

struct MemoryState;

[[nodiscard]] std::shared_ptr<const OwnerBackend> openMemoryBackend(
    std::shared_ptr<MemoryState> state, BackendMode mode, std::chrono::milliseconds lock_timeout);

} // namespace aowis::config::detail
