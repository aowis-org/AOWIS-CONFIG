#pragma once

#include "aowis/config/object_store.h"
#include "storage_backend.h"

#include <memory>
#include <utility>

namespace aowis::config::detail {

// Library-internal access to OwnerStore's private constructor, so internal
// backends without a public entry point (the key-value backend on a fake
// store) can be held to the backend contract tests.
struct OwnerStoreAccess {
    [[nodiscard]] static OwnerStore make(std::shared_ptr<const OwnerBackend> backend)
    {
        return OwnerStore(std::move(backend));
    }
};

} // namespace aowis::config::detail
