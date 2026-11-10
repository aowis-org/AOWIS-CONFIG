#include "aowis/config/config_root.h"
#include "aowis/config/error.h"
#include "aowis/config/result.h"
#include "aowis/config/version.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testOwnerIds()
{
    const std::string_view valid[] = {"gui", "gui-mobile", "server-map", "server-dvmdb", "a1", "a"};
    for (std::string_view name : valid) {
        aowis::config::Result<aowis::config::OwnerId> owner = aowis::config::OwnerId::parse(name);
        require(owner.hasValue(), "valid owner rejected");
        require(owner.value().value() == name, "owner name altered");
    }
    const std::string_view invalid[] = {
        "", "a/../b", "..", "GUI", "1gui", "-gui", "gui-", "gui--mobile",
        "gui_mobile", "gui.mobile", "gui\\mobile", "gui/mobile", "gui\nname",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    };
    for (std::string_view name : invalid) {
        aowis::config::Result<aowis::config::OwnerId> owner = aowis::config::OwnerId::parse(name);
        require(!owner, "invalid owner accepted");
        require(owner.error().code == aowis::config::ErrorCode::invalid_owner_id,
                "incorrect owner validation error");
    }
}

void testRootPaths()
{
    aowis::config::Result<aowis::config::OwnerId> owner = aowis::config::OwnerId::parse("server-map");
    require(owner.hasValue(), "test owner could not be created");
    aowis::config::Result<aowis::config::ConfigRoot> namespaced =
        aowis::config::ConfigRoot::fromNamespaceRoot(owner.value(), "/tmp/aowis-test-base/aowis");
    require(namespaced.hasValue(), "absolute namespace root rejected");
    require(namespaced.value().path() == std::filesystem::path("/tmp/aowis-test-base/aowis/server-map"),
            "namespace root did not append owner");
    require(namespaced.value().owner().value() == "server-map", "owner identity lost");

    aowis::config::Result<aowis::config::ConfigRoot> explicit_root =
        aowis::config::ConfigRoot::fromOwnerRoot(owner.value(), "/etc/aowis/server-map");
    require(explicit_root.hasValue(), "absolute explicit owner root rejected");
    require(explicit_root.value().path() == std::filesystem::path("/etc/aowis/server-map"),
            "explicit owner root was modified");

    aowis::config::Result<aowis::config::ConfigRoot> relative =
        aowis::config::ConfigRoot::fromNamespaceRoot(owner.value(), "relative/path");
    require(!relative && relative.error().code == aowis::config::ErrorCode::invalid_root,
            "relative namespace root should fail");
    aowis::config::Result<aowis::config::ConfigRoot> empty =
        aowis::config::ConfigRoot::fromOwnerRoot(owner.value(), {});
    require(!empty, "empty owner root should fail");

    aowis::config::Result<aowis::config::ConfigRoot> normalized =
        aowis::config::ConfigRoot::fromNamespaceRoot(owner.value(), "/tmp/aowis/../aowis");
    require(normalized && normalized.value().path() == std::filesystem::path("/tmp/aowis/server-map"),
            "lexical root normalization failed");
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
class EnvironmentGuard final {
public:
    explicit EnvironmentGuard(const char *name) : name_(name)
    {
        const char *value = std::getenv(name);
        if (value != nullptr) {
            this->previous_ = std::string(value);
        }
    }

    ~EnvironmentGuard()
    {
        if (this->previous_) {
            setenv(this->name_.c_str(), this->previous_->c_str(), 1);
        } else {
            unsetenv(this->name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

void testUserRootResolution()
{
    EnvironmentGuard xdg("XDG_CONFIG_HOME");
    EnvironmentGuard home("HOME");
    aowis::config::Result<aowis::config::OwnerId> owner =
        aowis::config::OwnerId::parse("gui");
    require(owner.hasValue(), "test owner unavailable");

    require(setenv("XDG_CONFIG_HOME", "/tmp/aowis-config-phase1-xdg", 1) == 0,
            "cannot set XDG_CONFIG_HOME");
    require(setenv("HOME", "/tmp/aowis-config-phase1-home", 1) == 0,
            "cannot set HOME");
    aowis::config::Result<aowis::config::ConfigRoot> xdg_root =
        aowis::config::ConfigRoot::forCurrentUser(owner.value());
    require(xdg_root && xdg_root.value().path()
                            == std::filesystem::path("/tmp/aowis-config-phase1-xdg/aowis/gui"),
            "XDG_CONFIG_HOME was not respected");

    require(setenv("XDG_CONFIG_HOME", "relative/xdg", 1) == 0,
            "cannot set relative XDG_CONFIG_HOME");
    aowis::config::Result<aowis::config::ConfigRoot> home_root =
        aowis::config::ConfigRoot::forCurrentUser(owner.value());
    require(home_root && home_root.value().path()
                            == std::filesystem::path("/tmp/aowis-config-phase1-home/.config/aowis/gui"),
            "relative XDG_CONFIG_HOME must fall back to HOME");

    require(unsetenv("XDG_CONFIG_HOME") == 0 && unsetenv("HOME") == 0,
            "cannot clear environment");
    aowis::config::Result<aowis::config::ConfigRoot> missing =
        aowis::config::ConfigRoot::forCurrentUser(owner.value());
    require(!missing && missing.error().code == aowis::config::ErrorCode::missing_environment,
            "missing environment must return an explicit error");
}
#endif

void testResultContract()
{
    aowis::config::Result<int> success = aowis::config::Result<int>::success(42);
    require(success && success.value() == 42, "success result incorrect");
    aowis::config::Result<int> failure = aowis::config::Result<int>::failure(
        {aowis::config::ErrorCode::invalid_root, "sample error"});
    require(!failure && failure.error().message == "sample error", "failure result incorrect");
    bool wrong_access_threw = false;
    try {
        static_cast<void>(failure.value());
    } catch (const std::bad_variant_access &) {
        wrong_access_threw = true;
    }
    require(wrong_access_threw, "invalid result access should throw");
    require(aowis::config::version_major == 0 && aowis::config::version_minor == 7,
            "version header incorrect");
}
} // namespace

int main()
{
    try {
        testOwnerIds();
        testRootPaths();
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        testUserRootResolution();
#endif
        testResultContract();
        std::cout << "AOWIS-CONFIG core tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
