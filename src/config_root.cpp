#include "aowis/config/config_root.h"

#include <cstdlib>
#include <string>
#include <utility>

namespace aowis::config {

namespace {
Result<std::filesystem::path> absoluteRoot(const std::filesystem::path &input)
{
    if (input.empty() || !input.is_absolute()) {
        return Result<std::filesystem::path>::failure(
            {ErrorCode::invalid_root, "Configuration root must be an absolute path"});
    }
    return Result<std::filesystem::path>::success(input.lexically_normal());
}

std::filesystem::path environmentAbsolutePath(const char *name)
{
    const char *raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return {};
    }
    const std::filesystem::path path(raw);
    return path.is_absolute() ? path : std::filesystem::path();
}
} // namespace

Result<ConfigRoot> ConfigRoot::fromOwnerRoot(
    OwnerId owner, const std::filesystem::path &owner_root)
{
    Result<std::filesystem::path> resolved = absoluteRoot(owner_root);
    if (!resolved) {
        return Result<ConfigRoot>::failure(resolved.error());
    }
    return Result<ConfigRoot>::success(ConfigRoot(std::move(owner), resolved.value()));
}

Result<ConfigRoot> ConfigRoot::fromNamespaceRoot(
    OwnerId owner, const std::filesystem::path &namespace_root)
{
    Result<std::filesystem::path> resolved = absoluteRoot(namespace_root);
    if (!resolved) {
        return Result<ConfigRoot>::failure(resolved.error());
    }
    const std::filesystem::path owner_path = resolved.value() / owner.value();
    return Result<ConfigRoot>::success(ConfigRoot(std::move(owner), owner_path));
}

Result<ConfigRoot> ConfigRoot::forCurrentUser(OwnerId owner)
{
#if defined(__EMSCRIPTEN__)
    return Result<ConfigRoot>::failure({ErrorCode::unsupported_platform,
                                       "Browser storage requires an explicit WebAssembly adapter"});
#elif defined(_WIN32)
    // LOCALAPPDATA, never the roaming APPDATA: owner roots hold private keys,
    // and roaming profiles are synchronized to domain file servers.
    const std::filesystem::path local_appdata = environmentAbsolutePath("LOCALAPPDATA");
    if (local_appdata.empty()) {
        return Result<ConfigRoot>::failure({ErrorCode::missing_environment,
                                           "LOCALAPPDATA is unset or not an absolute path"});
    }
    return ConfigRoot::fromNamespaceRoot(std::move(owner), local_appdata / "aowis");
#else
    std::filesystem::path config_base = environmentAbsolutePath("XDG_CONFIG_HOME");
    if (config_base.empty()) {
        const std::filesystem::path home = environmentAbsolutePath("HOME");
        if (home.empty()) {
            return Result<ConfigRoot>::failure({ErrorCode::missing_environment,
                                               "Neither absolute XDG_CONFIG_HOME nor absolute HOME is available"});
        }
        config_base = home / ".config";
    }
    return ConfigRoot::fromNamespaceRoot(std::move(owner), config_base / "aowis");
#endif
}

} // namespace aowis::config
