// Runs one behavioral contract against every storage backend available on
// this platform: always the in-memory backend, plus native POSIX where it
// exists. Shared ObjectCollection/WireGuardProfiles logic is therefore
// exercised on Windows and Emscripten builds too, and a future persistent
// backend only needs to be added to backends() to be held to the same rules.
#include <aowis/config/memory_storage.h>
#include <aowis/config/object_store.h>
#include <aowis/config/wireguard_profiles.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

namespace {
using aowis::config::AccessMode;
using aowis::config::ErrorCode;
using aowis::config::OpenOptions;
using aowis::config::OwnerSession;
using aowis::config::OwnerStore;
using aowis::config::MemoryStorage;
using aowis::config::ObjectCollection;
using aowis::config::ObjectId;
using aowis::config::ObjectListing;
using aowis::config::ObjectSnapshot;
using aowis::config::Result;
using aowis::config::StoredObject;
using aowis::config::WireGuardProfile;
using aowis::config::WireGuardProfileInput;
using aowis::config::WireGuardProfileListing;
using aowis::config::WireGuardProfiles;
using aowis::config::WireGuardReferenceCount;

void require(bool valid, const std::string &message)
{
    if (!valid) throw std::runtime_error(message);
}

// One owner root of one backend: opens collections and plants raw bytes
// under an entry name to simulate damage, as a crash or a user edit would.
struct Fixture {
    std::string backend;
    std::function<Result<OwnerStore>(OpenOptions)> store;
    std::function<void(const std::string &, const std::string &, const std::string &)> plant;

    [[nodiscard]] Result<ObjectCollection> open(const std::string &name, std::chrono::milliseconds timeout) const
    {
        Result<OwnerStore> opened = this->store(OpenOptions{AccessMode::read_write, timeout});
        if (!opened) return Result<ObjectCollection>::failure(opened.error());
        return opened.value().collection(name);
    }
};

ObjectCollection opened(const Fixture &fixture, const std::string &name)
{
    Result<ObjectCollection> collection = fixture.open(name, aowis::config::default_lock_timeout);
    require(collection.hasValue(), fixture.backend + ": cannot open collection");
    return collection.value();
}

void crudAndRevisions(const Fixture &fixture)
{
    const std::string tag = fixture.backend + ": ";
    ObjectCollection stored = opened(fixture, "items");
    require(stored.list() && stored.list().value().objects.empty(), tag + "new collection not empty");
    Result<StoredObject> first = stored.create("{\"n\":1}");
    Result<StoredObject> second = stored.create("{\"n\":2}");
    require(first && second, tag + "create failed");
    Result<ObjectListing> listed = stored.list();
    require(listed && listed.value().objects.size() == 2 && listed.value().unreadable.empty(), tag + "list size");
    require(listed.value().objects[0].id.value() < listed.value().objects[1].id.value(), tag + "list not sorted");
    Result<ObjectSnapshot> all = stored.readAll();
    require(all && all.value().objects.size() == 2, tag + "readAll size");
    Result<StoredObject> stale = stored.update(first.value().info.id, "{\"n\":9}", second.value().info.revision);
    require(!stale && stale.error().code == ErrorCode::conflict, tag + "stale update accepted");
    Result<StoredObject> updated = stored.update(first.value().info.id, "{\"n\":3}", first.value().info.revision);
    require(updated && updated.value().info.revision != first.value().info.revision, tag + "update failed");
    require(stored.read(first.value().info.id).value().json == "{\"n\":3}", tag + "update not visible");
    require(!stored.erase(first.value().info.id, first.value().info.revision), tag + "stale erase accepted");
    require(stored.erase(first.value().info.id, updated.value().info.revision).hasValue(), tag + "erase failed");
    Result<StoredObject> missing = stored.read(first.value().info.id);
    require(!missing && missing.error().code == ErrorCode::not_found, tag + "erased record readable");
    require(!stored.create("{bad"), tag + "invalid JSON stored");
    require(!fixture.open("../x", aowis::config::default_lock_timeout), tag + "collection traversal accepted");
    ObjectCollection other = opened(fixture, "others");
    require(other.list().value().objects.empty(), tag + "collections share entries");
}

void damagedEntries(const Fixture &fixture)
{
    const std::string tag = fixture.backend + ": ";
    ObjectCollection stored = opened(fixture, "damage");
    Result<StoredObject> healthy = stored.create("{\"ok\":true}");
    Result<StoredObject> victim = stored.create("{\"ok\":false}");
    Result<StoredObject> future = stored.create("{\"future\":true}");
    require(healthy && victim && future, tag + "cannot seed damage test");
    fixture.plant("damage", victim.value().info.id.value() + ".json", "{corrupt");
    fixture.plant("damage", future.value().info.id.value() + ".json",
                  "{\"envelope_version\":2,\"id\":\"" + future.value().info.id.value() + "\",\"revision\":\""
                      + future.value().info.revision + "\",\"payload\":{}}");
    fixture.plant("damage", "foreign.json", "{}");
    fixture.plant("damage", "notes.txt", "ignored: not a .json entry");
    Result<ObjectListing> listed = stored.list();
    require(listed && listed.value().objects.size() == 1, tag + "damage hid healthy record");
    require(listed.value().unreadable.size() == 3, tag + "damaged entries not all reported");
    require(listed.value().unreadable[0].filename < listed.value().unreadable[1].filename, tag + "unreadable unsorted");
    for (const aowis::config::UnreadableObject &entry : listed.value().unreadable) {
        if (entry.filename == "foreign.json") {
            require(!entry.id.has_value() && entry.error.code == ErrorCode::corrupt_object, tag + "foreign entry");
        } else if (entry.id->value() == future.value().info.id.value()) {
            require(entry.error.code == ErrorCode::unsupported_schema, tag + "future schema classification");
        } else {
            require(entry.error.code == ErrorCode::corrupt_object, tag + "corrupt classification");
        }
    }
    Result<bool> readable = stored.eraseCorrupt(healthy.value().info.id);
    require(!readable && readable.error().code == ErrorCode::conflict, tag + "eraseCorrupt removed readable");
    Result<bool> newer = stored.eraseCorrupt(future.value().info.id);
    require(!newer && newer.error().code == ErrorCode::unsupported_schema, tag + "eraseCorrupt removed newer schema");
    require(stored.eraseCorrupt(victim.value().info.id).hasValue(), tag + "eraseCorrupt failed");
    Result<bool> again = stored.eraseCorrupt(victim.value().info.id);
    require(!again && again.error().code == ErrorCode::not_found, tag + "eraseCorrupt not idempotent-safe");
    require(stored.list().value().unreadable.size() == 2, tag + "corrupt entry still listed");
}

void exclusiveSessions(const Fixture &fixture)
{
    // Many threads race to update the same revision: the lock plus revision
    // check must let exactly one win and turn every other into conflict.
    const std::string tag = fixture.backend + ": ";
    ObjectCollection stored = opened(fixture, "race");
    Result<StoredObject> seed = stored.create("{\"v\":0}");
    require(seed.hasValue(), tag + "cannot seed race");
    constexpr int contenders = 8;
    std::atomic<int> winners{0};
    std::atomic<int> conflicts{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < contenders; ++index) {
        threads.emplace_back([&fixture, &seed, &winners, &conflicts, &failures, index]() {
            Result<ObjectCollection> own = fixture.open("race", std::chrono::milliseconds(10000));
            if (!own) { ++failures; return; }
            Result<StoredObject> result = own.value().update(
                seed.value().info.id, "{\"v\":" + std::to_string(index + 1) + "}", seed.value().info.revision);
            if (result) ++winners;
            else if (result.error().code == ErrorCode::conflict) ++conflicts;
            else ++failures;
        });
    }
    for (std::thread &thread : threads) thread.join();
    require(failures == 0 && winners == 1 && conflicts == contenders - 1, tag + "lost update or deadlock in race");
}

void lockTimeout(const Fixture &fixture)
{
    // A writer thread keeps the collection busy with back-to-back updates.
    // A zero-timeout handle must then either succeed or report lock_timeout,
    // never block and never fail otherwise. (The deterministic "lock held by
    // another process" case is covered by the native object-store tests.)
    const std::string tag = fixture.backend + ": ";
    ObjectCollection stored = opened(fixture, "busy");
    Result<StoredObject> seed = stored.create("{\"v\":0}");
    require(seed.hasValue(), tag + "cannot seed busy test");
    std::atomic<bool> stop{false};
    std::thread writer([&stored, &seed, &stop]() {
        std::string revision = seed.value().info.revision;
        int value = 0;
        while (!stop) {
            Result<StoredObject> next = stored.update(seed.value().info.id, "{\"v\":" + std::to_string(++value) + "}", revision);
            if (next) revision = next.value().info.revision;
        }
    });
    Result<ObjectCollection> impatient = fixture.open("busy", std::chrono::milliseconds(0));
    int other_errors = 0;
    if (impatient) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            Result<StoredObject> read = impatient.value().read(seed.value().info.id);
            if (!read && read.error().code != ErrorCode::lock_timeout) ++other_errors;
        }
    } else if (impatient.error().code != ErrorCode::lock_timeout) {
        ++other_errors;
    }
    stop = true;
    writer.join();
    require(other_errors == 0, tag + "zero-timeout access failed with an error other than lock_timeout");
}

void wireGuardOnBackend(const Fixture &fixture)
{
    const std::string tag = fixture.backend + ": ";
    Result<OwnerStore> store = fixture.store(OpenOptions{});
    require(store.hasValue(), tag + "store failed");
    Result<WireGuardProfiles> opened_profiles = WireGuardProfiles::open(store.value());
    require(opened_profiles.hasValue(), tag + "WireGuardProfiles::open failed");
    const WireGuardProfiles &profiles = opened_profiles.value();
    const std::string text = "[Interface]\nPrivateKey = abc\n[Peer]\nPublicKey = def\n";
    Result<WireGuardProfile> created = profiles.importText(WireGuardProfileInput{"VPN", "", text});
    Result<WireGuardProfile> broken = profiles.importText(WireGuardProfileInput{"Broken", "", text});
    require(created && broken, tag + "import failed");
    fixture.plant("wireguard", broken.value().summary.info.id.value() + ".json", "{garbage");
    Result<WireGuardProfileListing> listed = profiles.list();
    require(listed && listed.value().profiles.size() == 1 && listed.value().unreadable.size() == 1,
            tag + "WireGuard listing on backend");
    require(profiles.exportText(created.value().summary.info.id).value() == text, tag + "text not lossless");
    const WireGuardReferenceCount unused = [](const OwnerSession &, const ObjectId &) {
        return Result<std::size_t>::success(0);
    };
    require(profiles.eraseUnreadable(broken.value().summary.info.id, unused).hasValue(), tag + "eraseUnreadable");
    require(profiles.erase(created.value().summary.info.id, created.value().summary.info.revision, unused).hasValue(),
            tag + "erase");
    require(profiles.list().value().profiles.empty(), tag + "profiles remain");
}

void ownerSessions(const Fixture &fixture)
{
    // One held read-write session excludes every other read-write session of
    // the owner, for every collection, until it is released. Read-only
    // sessions are never blocked.
    const std::string tag = fixture.backend + ": ";
    Result<OwnerStore> store = fixture.store(OpenOptions{});
    require(store.hasValue(), tag + "store failed");
    const ObjectCollection items = store.value().collection("items").value();
    Result<OwnerSession> held = store.value().lock();
    require(held.hasValue(), tag + "lock failed");
    Result<StoredObject> inside = items.create(held.value(), "{\"in\":\"session\"}");
    require(inside.hasValue(), tag + "write inside session failed");
    std::atomic<int> blocked{0};
    std::atomic<int> readable{0};
    std::thread other([&fixture, &blocked, &readable, &inside]() {
        // A persistent backend verifies the root under the lock in open(),
        // so the second writer may already be refused there.
        Result<OwnerStore> second = fixture.store(OpenOptions{AccessMode::read_write, std::chrono::milliseconds(50)});
        if (!second) {
            if (second.error().code == ErrorCode::lock_timeout) ++blocked;
        } else {
            Result<OwnerSession> lock = second.value().lock();
            if (!lock && lock.error().code == ErrorCode::lock_timeout) ++blocked;
        }
        Result<OwnerStore> reader = fixture.store(OpenOptions{AccessMode::read_only, std::chrono::milliseconds(0)});
        if (!reader) return;
        Result<ObjectCollection> items_ro = reader.value().collection("items");
        if (items_ro && items_ro.value().read(inside.value().info.id)) ++readable;
    });
    other.join();
    require(blocked == 1, tag + "held session did not exclude another writer");
    require(readable == 1, tag + "read-only access blocked or missed committed write");
    held = Result<OwnerSession>::failure({ErrorCode::not_found, "released"}); // Releases the lock.
    Result<OwnerStore> later = fixture.store(OpenOptions{AccessMode::read_write, std::chrono::milliseconds(0)});
    require(later && later.value().lock().hasValue(), tag + "lock not released with session");
}

void readOnlyContract(const Fixture &fixture)
{
    const std::string tag = fixture.backend + ": ";
    ObjectCollection writer = opened(fixture, "ro");
    Result<StoredObject> seed = writer.create("{\"seed\":1}");
    require(seed.hasValue(), tag + "cannot seed read-only test");
    Result<OwnerStore> store = fixture.store(OpenOptions{AccessMode::read_only});
    require(store && store.value().mode() == AccessMode::read_only, tag + "read-only store failed");
    Result<ObjectCollection> reader = store.value().collection("ro");
    require(reader && reader.value().read(seed.value().info.id).hasValue(), tag + "read-only read failed");
    Result<StoredObject> write = reader.value().create("{}");
    require(!write && write.error().code == ErrorCode::permission_denied, tag + "read-only create accepted");
    Result<StoredObject> update = reader.value().update(seed.value().info.id, "{}", seed.value().info.revision);
    require(!update && update.error().code == ErrorCode::permission_denied, tag + "read-only update accepted");
    Result<ObjectCollection> missing = store.value().collection("never-created");
    require(missing && missing.value().list().value().objects.empty(), tag + "missing read-only collection not empty");
}

void runContract(const Fixture &fixture)
{
    crudAndRevisions(fixture);
    damagedEntries(fixture);
    exclusiveSessions(fixture);
    lockTimeout(fixture);
    wireGuardOnBackend(fixture);
    ownerSessions(fixture);
    readOnlyContract(fixture);
    std::cout << "contract passed: " << fixture.backend << '\n';
}

void memorySpecifics()
{
    MemoryStorage storage;
    MemoryStorage shared = storage; // Handle semantics: copies share contents.
    Result<ObjectCollection> first = OwnerStore::openInMemory(storage).value().collection("items");
    Result<ObjectCollection> second = OwnerStore::openInMemory(shared).value().collection("items");
    require(first && second, "memory: open failed");
    Result<StoredObject> created = first.value().create("{\"x\":1}");
    require(created && second.value().read(created.value().info.id).hasValue(), "memory: copies do not share");
    Result<std::string> raw = storage.readRawEntry("items", created.value().info.id.value() + ".json");
    require(raw && raw.value().find("\"payload\":{\"x\":1}") != std::string::npos, "memory: raw entry not envelope");
    MemoryStorage isolated;
    require(OwnerStore::openInMemory(isolated).value().collection("items").value().list().value().objects.empty(),
            "memory: separate storages share contents");
    require(!storage.writeRawEntry("../x", "a.json", "{}"), "memory: invalid raw collection accepted");
    require(!storage.writeRawEntry("items", "a/b.json", "{}"), "memory: raw entry path accepted");
    require(!OwnerStore::openInMemory(storage).value().collection("Bad Name"), "memory: invalid collection accepted");
}

std::vector<Fixture> backends(MemoryStorage &memory, const std::filesystem::path &native_root)
{
    std::vector<Fixture> fixtures;
    fixtures.push_back({"memory",
        [memory](OpenOptions options) { return OwnerStore::openInMemory(memory, options); },
        [memory](const std::string &collection, const std::string &entry, const std::string &bytes) {
            require(memory.writeRawEntry(collection, entry, bytes).hasValue(), "memory: cannot plant entry");
        }});
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    fixtures.push_back({"native-posix",
        [native_root](OpenOptions options) {
            Result<aowis::config::OwnerId> owner = aowis::config::OwnerId::parse("gui");
            Result<aowis::config::ConfigRoot> root = aowis::config::ConfigRoot::fromOwnerRoot(owner.value(), native_root);
            return OwnerStore::open(root.value(), options);
        },
        [native_root](const std::string &collection, const std::string &entry, const std::string &bytes) {
            const std::filesystem::path file = native_root / collection / entry;
            {
                std::ofstream output(file, std::ios::binary | std::ios::trunc);
                output << bytes;
            }
            std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        }});
#else
    static_cast<void>(native_root);
#endif
    return fixtures;
}
} // namespace

int main()
{
    std::filesystem::path native_root;
    try {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        char name[] = "/tmp/aowis-config-contract-XXXXXX";
        const char *made = ::mkdtemp(name);
        require(made != nullptr, "cannot create native test root");
        native_root = std::filesystem::path(made) / "aowis" / "gui";
#endif
        MemoryStorage memory;
        memorySpecifics();
        for (const Fixture &fixture : backends(memory, native_root)) runContract(fixture);
        if (!native_root.empty()) std::filesystem::remove_all(native_root.parent_path().parent_path());
        std::cout << "AOWIS-CONFIG backend contract tests passed\n";
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << "FAILED: " << failure.what() << '\n';
        if (!native_root.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(native_root.parent_path().parent_path(), ignored);
        }
        return 1;
    }
}
