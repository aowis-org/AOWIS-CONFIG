#include <aowis/config/config_root.h>
#include <aowis/config/version.h>

#include <filesystem>

int main()
{
    aowis::config::Result<aowis::config::OwnerId> owner =
        aowis::config::OwnerId::parse("gui-mobile");
    if (!owner) {
        return 1;
    }
    aowis::config::Result<aowis::config::ConfigRoot> root =
        aowis::config::ConfigRoot::fromNamespaceRoot(owner.value(), "/tmp/aowis-consumer/aowis");
    return root && root.value().path() == std::filesystem::path("/tmp/aowis-consumer/aowis/gui-mobile")
               && aowis::config::version_major == 0 ? 0 : 2;
}
