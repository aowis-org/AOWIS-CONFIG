#include <aowis/config/wireguard_profiles.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using aowis::config::ConfigRoot;
using aowis::config::ErrorCode;
using aowis::config::MemoryStorage;
using aowis::config::ObjectCollection;
using aowis::config::ObjectId;
using aowis::config::OwnerId;
using aowis::config::OwnerSession;
using aowis::config::OwnerStore;
using aowis::config::Result;
using aowis::config::StoredObject;
using aowis::config::WireGuardProfile;
using aowis::config::WireGuardProfileInput;
using aowis::config::WireGuardProfileListing;
using aowis::config::WireGuardProfiles;
using aowis::config::WireGuardReferenceCount;

void require(bool valid, const char *message)
{
    if (!valid) throw std::runtime_error(message);
}

constexpr std::string_view secret = "k5yQ1m2b3c4d5e6f7g8h9i0j1k2l3m4n5o6p7q8r9s0=";
constexpr std::string_view sample =
    "# AOWIS VPN \xe2\x80\x94 original comment\r\n"
    "[Interface]\r\n"
    "PrivateKey = k5yQ1m2b3c4d5e6f7g8h9i0j1k2l3m4n5o6p7q8r9s0=\r\n"
    "Address = 10.77.0.2/32\r\n"
    "\r\n"
    "[Peer]\r\n"
    "PublicKey = V0hERXJ4MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg=\r\n"
    "Endpoint = vpn.example.invalid:51820\r\n"
    "AllowedIPs = 10.77.0.0/24\r\n";

const WireGuardReferenceCount unused = [](const OwnerSession &, const ObjectId &) {
    return Result<std::size_t>::success(0);
};

WireGuardProfiles memoryProfiles(const MemoryStorage &storage)
{
    Result<OwnerStore> store = OwnerStore::openInMemory(storage);
    require(store.hasValue(), "memory store failed");
    Result<WireGuardProfiles> profiles = WireGuardProfiles::open(store.value());
    require(profiles.hasValue(), "memory profiles failed");
    return profiles.value();
}

void textIntegrity()
{
    // Storage checks integrity only; WireGuard syntax is AOWIS-NETWORK's job.
    const MemoryStorage storage;
    const WireGuardProfiles profiles = memoryProfiles(storage);
    WireGuardProfileInput input{"VPN", "", "not WireGuard syntax at all"};
    require(profiles.importText(input).hasValue(), "storage interpreted WireGuard syntax");
    input.configuration.clear();
    Result<WireGuardProfile> empty = profiles.importText(input);
    require(!empty && empty.error().code == ErrorCode::invalid_wireguard_profile, "empty text accepted");
    input.configuration = std::string("[Interface]\nA\0B", 15);
    require(!profiles.importText(input), "NUL accepted");
    input.configuration = "[Interface]\nKey = \xff\n";
    require(!profiles.importText(input), "invalid UTF-8 accepted");
    input.configuration = std::string(aowis::config::max_wireguard_configuration_bytes + 1U, 'x');
    require(!profiles.importText(input), "oversized text accepted");
    input.configuration = std::string(sample);
    input.name = "";
    require(!profiles.importText(input), "empty name accepted");
    input.name = "line\nbreak";
    require(!profiles.importText(input), "multi-line name accepted");
    input.name = "VPN";
    input.description = std::string("broken \xff", 8);
    require(!profiles.importText(input), "invalid description UTF-8 accepted");
}

void lifecycle(const WireGuardProfiles &profiles)
{
    WireGuardProfileInput input{"Office VPN \xc3\xa9", "Private remote connection", std::string(sample)};
    Result<WireGuardProfile> created = profiles.importText(input);
    require(created.hasValue(), "import failed");
    const ObjectId id = created.value().summary.info.id;
    const std::string version_one = created.value().summary.info.revision;
    require(created.value().configuration == sample, "import changed formatting");
    Result<WireGuardProfileListing> listed = profiles.list();
    require(listed && listed.value().profiles.size() == 1 && listed.value().unreadable.empty(), "list failed");
    require(listed.value().profiles.front().name == input.name, "summary lost name");
    require(profiles.exportText(id).value() == sample, "export changed formatting");

    input.name = "Office VPN renamed";
    input.description = "Changed \"description\" with \\ backslash and \n newline";
    input.configuration += "# final comment\n";
    Result<WireGuardProfile> updated = profiles.update(id, version_one, input);
    require(updated && updated.value().summary.info.revision != version_one, "edit failed");
    require(updated.value().configuration == input.configuration, "edit changed text");
    Result<WireGuardProfile> stale = profiles.update(id, version_one, input);
    require(!stale && stale.error().code == ErrorCode::conflict, "stale update accepted");

    const std::string current = updated.value().summary.info.revision;
    Result<bool> unguarded = profiles.erase(id, current, WireGuardReferenceCount{});
    require(!unguarded && unguarded.error().code == ErrorCode::reference_unavailable, "missing resolver allowed delete");
    Result<bool> failed = profiles.erase(id, current, [](const OwnerSession &, const ObjectId &) {
        return Result<std::size_t>::failure({ErrorCode::io_failure, "Catalogue unavailable"});
    });
    require(!failed && failed.error().code == ErrorCode::io_failure, "failed lookup allowed delete");
    Result<bool> used = profiles.erase(id, current, [](const OwnerSession &, const ObjectId &) {
        return Result<std::size_t>::success(3);
    });
    require(!used && used.error().code == ErrorCode::profile_in_use, "referenced profile deleted");
    Result<bool> stale_delete = profiles.erase(id, version_one, unused);
    require(!stale_delete && stale_delete.error().code == ErrorCode::conflict, "stale delete accepted");
    require(profiles.erase(id, current, unused).hasValue(), "unreferenced delete failed");
    require(!profiles.read(id) && profiles.list().value().profiles.empty(), "deleted profile remains");
}

void referencesInOneSession()
{
    // The reference count reads the connection catalogue through the same
    // owner session as the delete, so no reference can appear in between.
    const MemoryStorage storage;
    Result<OwnerStore> store = OwnerStore::openInMemory(storage);
    require(store.hasValue(), "store failed");
    const WireGuardProfiles profiles = WireGuardProfiles::open(store.value()).value();
    const ObjectCollection connections = store.value().collection("connections").value();
    Result<WireGuardProfile> profile = profiles.importText({"VPN", "", std::string(sample)});
    require(profile.hasValue(), "import failed");
    const ObjectId id = profile.value().summary.info.id;
    const WireGuardReferenceCount count_connections =
        [&connections](const OwnerSession &session, const ObjectId &profile_id) {
            Result<aowis::config::ObjectSnapshot> all = connections.readAll(session);
            if (!all) return Result<std::size_t>::failure(all.error());
            std::size_t count = 0;
            for (const StoredObject &record : all.value().objects) {
                if (record.json.find(profile_id.value()) != std::string::npos) ++count;
            }
            return Result<std::size_t>::success(count);
        };
    Result<StoredObject> connection = connections.create(
        "{\"name\":\"Office DVMDB\",\"transport\":{\"type\":\"wireguard\",\"profile_id\":\"" + id.value() + "\"}}");
    require(connection.hasValue(), "connection create failed");
    Result<bool> blocked = profiles.erase(id, profile.value().summary.info.revision, count_connections);
    require(!blocked && blocked.error().code == ErrorCode::profile_in_use, "referenced profile deleted");

    // Reassign, then delete, both inside one held session.
    Result<OwnerSession> session = store.value().lock();
    require(session.hasValue(), "session failed");
    require(connections.update(session.value(), connection.value().info.id, "{\"name\":\"Office DVMDB\"}",
                               connection.value().info.revision).hasValue(), "reassign in session failed");
    require(profiles.erase(session.value(), id, profile.value().summary.info.revision, count_connections).hasValue(),
            "delete after reassignment failed");

    // A session of another store is rejected.
    const MemoryStorage other_storage;
    Result<OwnerStore> other = OwnerStore::openInMemory(other_storage);
    Result<OwnerSession> foreign = other.value().lock();
    Result<WireGuardProfileListing> wrong = profiles.list(foreign.value());
    require(!wrong && wrong.error().code == ErrorCode::session_mismatch, "foreign session accepted");
}

void unreadableRecords()
{
    const MemoryStorage storage;
    const WireGuardProfiles profiles = memoryProfiles(storage);
    Result<WireGuardProfile> healthy = profiles.importText({"A", "", std::string(sample)});
    Result<WireGuardProfile> broken = profiles.importText({"B", "", std::string(sample)});
    Result<WireGuardProfile> wrong_type = profiles.importText({"C", "", std::string(sample)});
    Result<WireGuardProfile> newer = profiles.importText({"D", "", std::string(sample)});
    require(healthy && broken && wrong_type && newer, "cannot seed unreadable test");
    require(storage.writeRawEntry("wireguard", broken.value().summary.info.id.value() + ".json", "{garbage").hasValue(),
            "cannot plant corrupt envelope");
    const ObjectCollection &generic = profiles.collection();
    require(generic.update(wrong_type.value().summary.info.id,
                           "{\"type\":\"not-wireguard\",\"profile_version\":1,\"name\":\"X\",\"description\":\"\","
                           "\"configuration\":\"x\"}",
                           wrong_type.value().summary.info.revision).hasValue(), "cannot plant wrong type");
    require(generic.update(newer.value().summary.info.id,
                           "{\"type\":\"wireguard-profile\",\"profile_version\":2,\"name\":\"D\",\"description\":\"\","
                           "\"configuration\":\"x\"}",
                           newer.value().summary.info.revision).hasValue(), "cannot plant newer version");
    Result<WireGuardProfileListing> listed = profiles.list();
    require(listed && listed.value().profiles.size() == 1 && listed.value().unreadable.size() == 3,
            "damaged records hid healthy profile or were dropped");
    Result<bool> readable = profiles.eraseUnreadable(healthy.value().summary.info.id, unused);
    require(!readable && readable.error().code == ErrorCode::conflict, "eraseUnreadable removed readable profile");
    Result<bool> referenced = profiles.eraseUnreadable(broken.value().summary.info.id,
        [](const OwnerSession &, const ObjectId &) { return Result<std::size_t>::success(1); });
    require(!referenced && referenced.error().code == ErrorCode::profile_in_use, "referenced unreadable removed");
    require(profiles.eraseUnreadable(broken.value().summary.info.id, unused).hasValue(), "corrupt envelope kept");
    require(profiles.eraseUnreadable(wrong_type.value().summary.info.id, unused).hasValue(), "wrong type kept");
    Result<bool> future = profiles.eraseUnreadable(newer.value().summary.info.id, unused);
    require(!future && future.error().code == ErrorCode::unsupported_schema, "newer profile version deleted");
    require(profiles.list().value().unreadable.size() == 1, "removed records still listed");
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        char name[] = "/tmp/aowis-wireguard-test-XXXXXX";
        char *directory = ::mkdtemp(name);
        require(directory != nullptr, "mkdtemp failed");
        this->path_ = directory;
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(this->path_, ignored); }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return this->path_; }
private:
    std::filesystem::path path_;
};

WireGuardProfiles nativeProfiles(const std::filesystem::path &path)
{
    Result<OwnerId> owner = OwnerId::parse("gui");
    Result<ConfigRoot> root = ConfigRoot::fromOwnerRoot(owner.value(), path);
    Result<OwnerStore> store = OwnerStore::open(root.value());
    require(store.hasValue(), "native store failed");
    Result<WireGuardProfiles> profiles = WireGuardProfiles::open(store.value());
    require(profiles.hasValue(), "native profiles failed");
    return profiles.value();
}

void requireMode(const std::filesystem::path &path, mode_t expected)
{
    struct stat state{};
    require(::lstat(path.c_str(), &state) == 0, "stat failed");
    require((state.st_mode & 0777) == expected, "insecure permissions");
}

void nativePersistence()
{
    TemporaryDirectory directory;
    const std::filesystem::path one = directory.path() / "original" / "aowis" / "gui";
    const WireGuardProfiles profiles = nativeProfiles(one);
    lifecycle(profiles);
    Result<WireGuardProfile> kept = profiles.importText({"Kept", "", std::string(sample)});
    require(kept.hasValue(), "import failed");
    const ObjectId id = kept.value().summary.info.id;
    const std::filesystem::path record = one / "wireguard" / (id.value() + ".json");
    requireMode(one, 0700);
    requireMode(one / "wireguard", 0700);
    requireMode(record, 0600);
    {
        std::ifstream file(record, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        require(bytes.find(secret) != std::string::npos, "imported secret not in managed record");
    }
    require(nativeProfiles(one).read(id).value().configuration == sample, "reopen changed text");
    const std::filesystem::path copy = directory.path() / "moved" / "aowis" / "gui";
    std::filesystem::create_directories(copy.parent_path());
    std::filesystem::copy(one, copy, std::filesystem::copy_options::recursive);
    Result<WireGuardProfile> relocated = nativeProfiles(copy).read(id);
    require(relocated && relocated.value().configuration == sample, "relocation broke profile");
}
#endif
} // namespace

int main()
{
    try {
        textIntegrity();
        {
            const MemoryStorage storage;
            lifecycle(memoryProfiles(storage));
        }
        referencesInOneSession();
        unreadableRecords();
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        nativePersistence();
#endif
        std::cout << "WireGuard profile storage tests passed\n";
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << "WireGuard profile test failed: " << failure.what() << '\n';
        return 1;
    }
}
