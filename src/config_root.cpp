#include "aowis/config/config_root.h"

#include <cstdlib>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

#if defined(_WIN32)
// The wide API: getenv() returns the ANSI code page and mangles profile
// paths with characters outside it (e.g. C:\Users\Łukasz).
std::filesystem::path environmentAbsolutePath(const wchar_t *name)
{
    const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) return {};
    std::wstring value(size, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0 || written >= size) return {};
    value.resize(written);
    const std::filesystem::path path(value);
    return path.is_absolute() ? path : std::filesystem::path();
}
#elif !defined(__EMSCRIPTEN__)
std::filesystem::path environmentAbsolutePath(const char *name)
{
    const char *raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return {};
    }
    const std::filesystem::path path(raw);
    return path.is_absolute() ? path : std::filesystem::path();
}
#endif
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
    // Browser roots are key namespaces in the origin's localStorage, not
    // filesystem paths; every owner of the origin lives under "/aowis".
    return ConfigRoot::fromNamespaceRoot(std::move(owner), std::filesystem::path("/aowis"));
#elif defined(_WIN32)
    // LOCALAPPDATA, never the roaming APPDATA: owner roots hold private keys,
    // and roaming profiles are synchronized to domain file servers.
    const std::filesystem::path local_appdata = environmentAbsolutePath(L"LOCALAPPDATA");
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
