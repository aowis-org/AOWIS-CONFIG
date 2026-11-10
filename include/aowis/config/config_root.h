#pragma once

#include "aowis/config/owner_id.h"

#include <filesystem>
#include <utility>

namespace aowis::config {

// Resolves an owner's *configuration location only*. These methods perform
// no filesystem writes, symlink checks, locks or permission changes; those
// happen in OwnerStore::open().
class ConfigRoot final {
public:
    // namespace_root is the absolute path to the 'aowis' directory.
    [[nodiscard]] static Result<ConfigRoot> fromNamespaceRoot(
        OwnerId owner, const std::filesystem::path &namespace_root);

    // Explicit absolute owner root, useful for system services (/etc/aowis/...)
    // or isolated tests. No additional 'aowis/<owner>' suffix is appended.
    [[nodiscard]] static Result<ConfigRoot> fromOwnerRoot(
        OwnerId owner, const std::filesystem::path &owner_root);

    // Native user root: XDG_CONFIG_HOME/aowis/<owner> or HOME/.config/aowis/<owner>
    // on POSIX, and LOCALAPPDATA/aowis/<owner> on Windows (never the roaming
    // APPDATA: roots hold private keys). Never creates directories. Android
    // has no meaningful HOME/XDG: pass an explicit root from QStandardPaths.
    [[nodiscard]] static Result<ConfigRoot> forCurrentUser(OwnerId owner);

    [[nodiscard]] const OwnerId &owner() const noexcept { return this->owner_; }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return this->path_; }

private:
    ConfigRoot(OwnerId owner, std::filesystem::path path)
        : owner_(std::move(owner)), path_(std::move(path))
    {
    }

    OwnerId owner_;
    std::filesystem::path path_;
};

} // namespace aowis::config
