#include <aowis/config/config_root.h>
#include <aowis/config/json.h>
#include <aowis/config/memory_storage.h>
#include <aowis/config/object_id.h>
#include <aowis/config/object_store.h>
#include <aowis/config/version.h>
#include <aowis/config/wireguard_profiles.h>

#include <filesystem>

// Links through exactly the public target and headers. Uses MemoryStorage so
// it runs identically on every platform without touching the filesystem.
int main()
{
    aowis::config::Result<aowis::config::OwnerId> owner =
        aowis::config::OwnerId::parse("gui-mobile");
    if (!owner) {
        return 1;
    }
    aowis::config::Result<aowis::config::ConfigRoot> root =
        aowis::config::ConfigRoot::fromNamespaceRoot(owner.value(), "/tmp/aowis-consumer/aowis");
    aowis::config::Result<aowis::config::ObjectId> identifier = aowis::config::ObjectId::generate();
    if (!identifier || !aowis::config::validateJsonObject("{\"success\":true}")) {
        return 3;
    }
    const aowis::config::MemoryStorage storage;
    aowis::config::Result<aowis::config::OwnerStore> store = aowis::config::OwnerStore::openInMemory(storage);
    if (!store) {
        return 4;
    }
    aowis::config::Result<aowis::config::WireGuardProfiles> profiles =
        aowis::config::WireGuardProfiles::open(store.value());
    if (!profiles || !profiles.value().importText({"VPN", "", "[Interface]\nPrivateKey = example\n"})) {
        return 5;
    }
    return root && root.value().path() == std::filesystem::path("/tmp/aowis-consumer/aowis/gui-mobile")
               && aowis::config::version_major == 0 ? 0 : 2;
}
