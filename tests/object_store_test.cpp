#include "aowis/config/config_root.h"
#include "aowis/config/json.h"
#include "aowis/config/object_id.h"
#include "aowis/config/object_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using aowis::config::ConfigRoot;
using aowis::config::ErrorCode;
using aowis::config::ObjectCollection;
using aowis::config::ObjectId;
using aowis::config::OwnerId;
using aowis::config::ObjectListing;
using aowis::config::ObjectSnapshot;
using aowis::config::AccessMode;
using aowis::config::OpenOptions;
using aowis::config::OwnerSession;
using aowis::config::OwnerStore;
using aowis::config::Result;
using aowis::config::StoredObject;

void require(bool valid, const char *message)
{
    if (!valid) throw std::runtime_error(message);
}

void jsonTests()
{
    const std::string_view valid[] = {
        "{}", "{\"name\":\"VPN \\uD83D\\uDD10\",\"addresses\":[\"10.0.0.1/32\",null,true,1.25e-2]}",
        "{\"nested\":{\"a\":1,\"b\":[]}}", "{\"utf8\":\"grüße\"}"
    };
    for (std::string_view value : valid) require(aowis::config::validateJsonObject(value).hasValue(), "valid JSON rejected");
    const std::string_view invalid[] = {
        "[]", "{} garbage", "{", "{\"x\":}", "{\"x\":01}", "{\"x\":-}",
        "{\"x\":1.}", "{\"x\":1e+}", "{\"x\":\"\\x\"}", "{\"x\":\"\\uD800\"}",
        "{\"x\":\"\\uDC00\"}", "{\"x\":\"\\uD800\\u0000\"}",
        "{\"x\":true,}", "{\"x\":\"\x01\"}", "{\"x\":\"\xc0\xaf\"}",
        "{\"x\":\"\xed\xa0\x80\"}", "{\"x\":\"\xf4\x90\x80\x80\"}"
    };
    for (std::string_view value : invalid) require(!aowis::config::validateJsonObject(value), "invalid JSON accepted");
    std::string deeply_nested(70, '[');
    deeply_nested.append("0");
    deeply_nested.append(70, ']');
    require(!aowis::config::validateJsonObject("{\"deep\":" + deeply_nested + "}"), "overdeep JSON accepted");
}

void idTests()
{
    Result<ObjectId> first = ObjectId::generate();
    Result<ObjectId> second = ObjectId::generate();
    require(first && second && first.value().value() != second.value().value(), "UUID generation failed");
    require(ObjectId::parse(first.value().value()).hasValue(), "UUIDv7 did not parse");
    require(!ObjectId::parse("../../foo"), "path traversal accepted as UUID");
    require(!ObjectId::parse("01a18fe5-99c4-760f-7d36-63e35e1fe608"), "UUIDv7 parser accepted wrong variant");
    require(!ObjectId::parse("01A18FE5-99C4-760F-BD36-63E35E1FE608"), "uppercase UUID accepted");
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        char name[] = "/tmp/aowis-config-storage-XXXXXX";
        char *path = ::mkdtemp(name);
        require(path != nullptr, "cannot create test directory");
        this->path_ = path;
    }
    ~TemporaryDirectory() { std::error_code failure; std::filesystem::remove_all(this->path_, failure); }
    [[nodiscard]] const std::filesystem::path &path() const { return this->path_; }
private:
    std::filesystem::path path_;
};

ConfigRoot ownerRoot(const std::filesystem::path &path)
{
    Result<OwnerId> owner = OwnerId::parse("gui");
    require(owner.hasValue(), "cannot construct test owner");
    Result<ConfigRoot> root = ConfigRoot::fromOwnerRoot(owner.value(), path);
    require(root.hasValue(), "cannot construct test root");
    return root.value();
}

Result<ObjectCollection> openCollection(const ConfigRoot &root, std::string_view name,
                                       std::chrono::milliseconds timeout = aowis::config::default_lock_timeout,
                                       AccessMode mode = AccessMode::read_write)
{
    Result<OwnerStore> store = OwnerStore::open(root, OpenOptions{mode, timeout});
    if (!store) return Result<ObjectCollection>::failure(store.error());
    return store.value().collection(name);
}

ObjectCollection collection(const std::filesystem::path &path, std::string_view name = "wireguard")
{
    Result<ObjectCollection> opened = openCollection(ownerRoot(path), name);
    require(opened.hasValue(), "cannot open test collection");
    return opened.value();
}

void requireMode(const std::filesystem::path &file, mode_t expected)
{
    struct stat status{};
    require(::lstat(file.c_str(), &status) == 0, "cannot stat secure file");
    require((status.st_mode & 0777) == expected, "incorrect security mode");
}

void basicCrud()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "aowis" / "gui";
    ObjectCollection stored = collection(root);
    requireMode(root, 0700);
    requireMode(root / "wireguard", 0700);
    requireMode(root / ".lock", 0600);
    require(!std::filesystem::exists(root / "wireguard" / ".lock"), "per-collection lock still created");
    require(stored.list() && stored.list().value().objects.empty(), "new collection not empty");
    Result<StoredObject> created = stored.create("{\"conf\":\"[Interface]\\nPrivateKey = EXAMPLE\\n\",\"name\":\"Test\"}");
    require(created.hasValue(), "cannot create test object");
    const ObjectId id = created.value().info.id;
    requireMode(root / "wireguard" / (id.value() + ".json"), 0600);
    require(stored.list() && stored.list().value().objects.size() == 1, "listed record missing");
    require(stored.list().value().objects[0].id.value() == id.value(), "listed wrong record ID");
    Result<ObjectSnapshot> snapshot = stored.readAll();
    require(snapshot && snapshot.value().objects.size() == 1
            && snapshot.value().objects[0].json == created.value().json, "readAll lost payload");
    Result<StoredObject> reloaded = stored.read(id);
    require(reloaded && reloaded.value().json == created.value().json, "payload lost on read");
    require(reloaded.value().info.revision == created.value().info.revision, "revision changed on read");
    Result<ObjectCollection> reopened = openCollection(ownerRoot(root), "wireguard");
    require(reopened && reopened.value().read(id).hasValue(), "reopen failed");
    require(!stored.update(id, "{}", "bad") && stored.update(id, "{}", "bad").error().code == ErrorCode::conflict,
            "stale edit accepted");
    Result<StoredObject> updated = stored.update(id, "{\"name\":\"Updated\"}", created.value().info.revision);
    require(updated && updated.value().info.revision != created.value().info.revision,
            "valid update failed or did not change revision");
    Result<StoredObject> stale = reopened.value().update(id, "{\"name\":\"Stale\"}", created.value().info.revision);
    require(!stale && stale.error().code == ErrorCode::conflict, "lost-update protection failed");
    require(!stored.erase(id, created.value().info.revision)
            && stored.erase(id, created.value().info.revision).error().code == ErrorCode::conflict,
            "stale deletion accepted");
    require(stored.read(id).value().json == updated.value().json, "stale write changed live record");
    require(stored.erase(id, updated.value().info.revision).hasValue(), "valid deletion failed");
    Result<StoredObject> missing = stored.read(id);
    require(!missing && missing.error().code == ErrorCode::not_found, "deleted record remains");
    require(stored.list() && stored.list().value().objects.empty(), "deleted record still listed");
    require(!stored.erase(id, updated.value().info.revision), "deleted twice");
}

void validationAndCorruption()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "gui";
    ObjectCollection stored = collection(root);
    Result<StoredObject> bad = stored.create("{\"bad\":}");
    require(!bad && bad.error().code == ErrorCode::invalid_json, "invalid data was stored");
    require(!openCollection(ownerRoot(root), "../bad"), "collection path traversal accepted");
    Result<StoredObject> created = stored.create("{\"name\":\"a\"}");
    require(created.hasValue(), "cannot seed corruption test");
    const std::filesystem::path file = root / "wireguard" / (created.value().info.id.value() + ".json");
    {
        std::ofstream output(file, std::ios::trunc);
        output << "{corrupt";
    }
    require(!stored.read(created.value().info.id)
            && stored.read(created.value().info.id).error().code == ErrorCode::corrupt_object,
            "corruption not detected");
    // One damaged record must not hide the readable ones, and it must stay
    // visible (not silently dropped) so a UI can offer to remove it.
    Result<StoredObject> healthy = stored.create("{\"name\":\"b\"}");
    require(healthy.hasValue(), "cannot seed healthy neighbour");
    Result<ObjectListing> listed = stored.list();
    require(listed && listed.value().objects.size() == 1
            && listed.value().objects[0].id.value() == healthy.value().info.id.value(),
            "corrupt record hid readable records");
    require(listed.value().unreadable.size() == 1 && listed.value().unreadable[0].id.has_value()
            && listed.value().unreadable[0].id->value() == created.value().info.id.value()
            && listed.value().unreadable[0].error.code == ErrorCode::corrupt_object,
            "corrupt record silently excluded from list");
    Result<ObjectSnapshot> snapshot = stored.readAll();
    require(snapshot && snapshot.value().objects.size() == 1 && snapshot.value().unreadable.size() == 1,
            "readAll did not report corrupt record");
    Result<bool> readable = stored.eraseCorrupt(healthy.value().info.id);
    require(!readable && readable.error().code == ErrorCode::conflict, "eraseCorrupt deleted a readable record");
    require(stored.read(healthy.value().info.id).hasValue(), "readable record removed");
    require(stored.eraseCorrupt(created.value().info.id).hasValue(), "corrupt record could not be removed");
    require(!std::filesystem::exists(file), "corrupt record file remains");
    require(stored.list().value().unreadable.empty(), "removed corrupt record still listed");
    Result<bool> gone = stored.eraseCorrupt(created.value().info.id);
    require(!gone && gone.error().code == ErrorCode::not_found, "missing corrupt record not reported");

    Result<StoredObject> future_record = stored.create("{\"name\":\"c\"}");
    require(future_record.hasValue(), "cannot seed schema test");
    const std::filesystem::path future_file = root / "wireguard" / (future_record.value().info.id.value() + ".json");
    {
        std::ofstream output(future_file, std::ios::trunc);
        output << "{\"envelope_version\":2,\"id\":\"" << future_record.value().info.id.value()
               << "\",\"revision\":\"" << future_record.value().info.revision
               << "\",\"payload\":{}}";
    }
    require(!stored.read(future_record.value().info.id)
            && stored.read(future_record.value().info.id).error().code == ErrorCode::unsupported_schema,
            "unsupported reader schema accepted");
    Result<bool> future_erase = stored.eraseCorrupt(future_record.value().info.id);
    require(!future_erase && future_erase.error().code == ErrorCode::unsupported_schema,
            "eraseCorrupt deleted a record from a newer schema");
    require(std::filesystem::exists(future_file), "newer-schema record removed");
}

void attacksAndPermissions()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "real" / "gui";
    ObjectCollection stored = collection(root);
    const std::filesystem::path link = directory.path() / "link";
    std::filesystem::create_symlink(directory.path() / "real", link);
    // A symlinked *ancestor* (/home -> /var/home, ~/.config -> dotfiles) is
    // legitimate and reaches the same private owner directory.
    Result<ObjectCollection> via_ancestor = openCollection(ownerRoot(link / "gui"), "wireguard");
    require(via_ancestor.hasValue(), "symlinked ancestor rejected");
    Result<ObjectCollection> new_via_ancestor = openCollection(ownerRoot(link / "fresh" / "gui"), "wireguard");
    require(new_via_ancestor.hasValue() && std::filesystem::is_directory(directory.path() / "real" / "fresh" / "gui"),
            "missing ancestor below a symlink not created");
    // The owner directory itself is the security boundary and must not be a symlink.
    std::filesystem::create_directory_symlink(root, directory.path() / "real" / "gui-link");
    Result<ObjectCollection> owner_link = openCollection(ownerRoot(directory.path() / "real" / "gui-link"), "wireguard");
    require(!owner_link && owner_link.error().code == ErrorCode::unsafe_path, "symlink owner root accepted");
    std::filesystem::create_symlink(root / "wireguard", root / "evil");
    require(!openCollection(ownerRoot(root), "evil"), "symlink collection accepted");
    Result<StoredObject> created = stored.create("{}");
    require(created.hasValue(), "cannot seed symlink test");
    const std::filesystem::path file = root / "wireguard" / (created.value().info.id.value() + ".json");
    std::filesystem::remove(file);
    std::filesystem::create_symlink("/etc/passwd", file);
    Result<StoredObject> result = stored.read(created.value().info.id);
    require(!result && result.error().code == ErrorCode::unsafe_path, "symlink object followed");
    Result<ObjectListing> listed = stored.list();
    require(listed && listed.value().objects.empty() && listed.value().unreadable.size() == 1
            && listed.value().unreadable[0].error.code == ErrorCode::unsafe_path,
            "list followed or hid malicious symlink");
    Result<bool> symlink_erase = stored.eraseCorrupt(created.value().info.id);
    require(!symlink_erase && symlink_erase.error().code == ErrorCode::unsafe_path
            && std::filesystem::is_symlink(file), "eraseCorrupt touched an unsafe entry");
    std::filesystem::remove(file);
    require(::chmod((root / "wireguard").c_str(), 0755) == 0, "cannot alter test directory permissions");
    require(!openCollection(ownerRoot(root), "wireguard"), "insecure collection accepted");
    require(::chmod((root / "wireguard").c_str(), 0700) == 0, "cannot restore permissions");
    require(::chmod(root.c_str(), 0777) == 0, "cannot alter owner permissions");
    require(!openCollection(ownerRoot(root), "wireguard"), "insecure owner accepted");
}

void crashRecovery()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "gui";
    ObjectCollection stored = collection(root);
    Result<StoredObject> created = stored.create("{\"version\":1}");
    require(created.hasValue(), "cannot seed crash test");
    const ObjectId id = created.value().info.id;
    const std::string original_revision = created.value().info.revision;
    const pid_t child = ::fork();
    require(child >= 0, "cannot fork crash test");
    if (child == 0) {
        ::setenv("AOWIS_CONFIG_TEST_CRASH_AT", "after-temp-fsync", 1);
        static_cast<void>(stored.update(id, "{\"version\":2}", original_revision));
        ::_exit(80);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "pre-publish crash point not reached");
    require(stored.read(id) && stored.read(id).value().json == "{\"version\":1}",
            "pre-publish crash damaged last committed object");
    // Readers skip orphans; the next open() or write removes them.
    require(stored.list() && stored.list().value().objects.size() == 1 && stored.list().value().unreadable.empty(),
            "orphan temp visible to readers");
    require(openCollection(ownerRoot(root), "wireguard").hasValue(), "reopen after crash failed");
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(root / "wireguard")) {
        require(!entry.path().filename().string().starts_with(".tmp-"), "orphan temp not recovered");
    }
    const pid_t later = ::fork();
    require(later >= 0, "cannot fork post-publish crash test");
    if (later == 0) {
        ::setenv("AOWIS_CONFIG_TEST_CRASH_AT", "after-publish", 1);
        static_cast<void>(stored.update(id, "{\"version\":3}", original_revision));
        ::_exit(81);
    }
    require(::waitpid(later, &status, 0) == later && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "post-publish crash point not reached");
    require(stored.read(id) && stored.read(id).value().json == "{\"version\":3}",
            "post-publish crash returned a torn object");
}

void processConflicts()
{
    TemporaryDirectory directory;
    ObjectCollection stored = collection(directory.path() / "gui");
    Result<StoredObject> created = stored.create("{\"value\":1}");
    require(created.hasValue(), "cannot seed process conflict test");
    int signal_pipe[2]{};
    require(::pipe(signal_pipe) == 0, "cannot make process sync pipe");
    const pid_t child = ::fork();
    require(child >= 0, "cannot fork conflict test");
    if (child == 0) {
        ::close(signal_pipe[1]);
        char signal = 0;
        if (::read(signal_pipe[0], &signal, 1) != 1) ::_exit(70);
        const Result<StoredObject> stale = stored.update(created.value().info.id,
            "{\"value\":999}", created.value().info.revision);
        ::_exit(!stale && stale.error().code == ErrorCode::conflict ? 0 : 71);
    }
    ::close(signal_pipe[0]);
    Result<StoredObject> updated = stored.update(created.value().info.id,
        "{\"value\":2}", created.value().info.revision);
    require(updated.hasValue(), "parent update failed");
    require(::write(signal_pipe[1], "x", 1) == 1, "cannot release child");
    ::close(signal_pipe[1]);
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "cross-process stale update was not rejected");
    require(stored.read(created.value().info.id).value().json == "{\"value\":2}",
            "cross-process conflict overwrote committed data");
}

void copyAndOwnerIsolation()
{
    TemporaryDirectory directory;
    const std::filesystem::path original = directory.path() / "origin" / "aowis" / "gui";
    ObjectCollection stored = collection(original);
    Result<StoredObject> record = stored.create("{\"private_data\":\"line1\\nline2\\n\"}");
    require(record.hasValue(), "cannot seed copy test");
    const std::filesystem::path destination = directory.path() / "relocated" / "aowis" / "gui";
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy(original, destination, std::filesystem::copy_options::recursive);
    ObjectCollection copied = collection(destination);
    require(copied.read(record.value().info.id)
                && copied.read(record.value().info.id).value().json == record.value().json,
            "relocated owner lost payload or stable ID");
    const std::filesystem::path mobile_root = directory.path() / "origin" / "aowis" / "gui-mobile";
    ObjectCollection mobile = collection(mobile_root);
    require(mobile.list() && mobile.list().value().objects.empty(), "different owners share storage");
    require(stored.list() && stored.list().value().objects.size() == 1, "other owner affected original storage");
}

void additionalCorruptionAndSecurity()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "gui";
    ObjectCollection stored = collection(root);
    Result<StoredObject> created = stored.create("{}");
    require(created.hasValue(), "cannot create additional corruption fixture");
    const std::filesystem::path file = root / "wireguard" / (created.value().info.id.value() + ".json");
    require(::chmod(file.c_str(), 0644) == 0, "cannot change object permissions");
    Result<StoredObject> unsafe = stored.read(created.value().info.id);
    require(!unsafe && unsafe.error().code == ErrorCode::unsafe_path, "public object file accepted");
    require(::chmod(file.c_str(), 0600) == 0, "cannot restore object permissions");
    Result<ObjectId> unrelated = ObjectId::generate();
    require(unrelated.hasValue(), "cannot generate wrong identity");
    {
        std::ofstream output(file, std::ios::trunc);
        output << "{\"envelope_version\":1,\"id\":\"" << unrelated.value().value()
               << "\",\"revision\":\"" << created.value().info.revision
               << "\",\"payload\":{}}";
    }
    require(!stored.read(created.value().info.id)
            && stored.read(created.value().info.id).error().code == ErrorCode::corrupt_object,
            "wrong record identity accepted");
    std::filesystem::remove(file);
    {
        std::ofstream output(root / "wireguard" / "not-a-uuid.json");
        output << "{}";
    }
    Result<ObjectListing> listed = stored.list();
    require(listed && listed.value().unreadable.size() == 1
            && listed.value().unreadable[0].filename == "not-a-uuid.json"
            && !listed.value().unreadable[0].id.has_value()
            && listed.value().unreadable[0].error.code == ErrorCode::corrupt_object,
            "unrecognized object file was silently skipped");
}

void readOnlyMode()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "etc" / "aowis" / "server-dvmdb";
    Result<OwnerStore> missing = OwnerStore::open(ownerRoot(root), OpenOptions{AccessMode::read_only});
    require(!missing && missing.error().code == ErrorCode::not_found, "missing read-only root not reported");
    require(!std::filesystem::exists(directory.path() / "etc"), "read-only open created directories");

    ObjectCollection writer = collection(root, "settings");
    Result<StoredObject> created = writer.create("{\"listen\":\"0.0.0.0:8755\"}");
    require(created.hasValue(), "cannot seed read-only test");
    std::filesystem::remove(root / ".lock");
    // Administrator layout: group-readable, never group-writable.
    require(::chmod(root.c_str(), 0750) == 0 && ::chmod((root / "settings").c_str(), 0750) == 0
            && ::chmod((root / "settings" / (created.value().info.id.value() + ".json")).c_str(), 0640) == 0,
            "cannot prepare administrator permissions");
    Result<ObjectCollection> rejected = openCollection(ownerRoot(root), "settings");
    require(!rejected, "read-write mode accepted group-readable store");

    Result<OwnerStore> store = OwnerStore::open(ownerRoot(root), OpenOptions{AccessMode::read_only});
    require(store && store.value().mode() == AccessMode::read_only, "read-only open failed");
    Result<ObjectCollection> settings = store.value().collection("settings");
    require(settings.hasValue(), "read-only collection failed");
    Result<StoredObject> loaded = settings.value().read(created.value().info.id);
    require(loaded && loaded.value().json == created.value().json, "read-only read failed");
    require(settings.value().list() && settings.value().list().value().objects.size() == 1, "read-only list failed");
    Result<StoredObject> write = settings.value().create("{}");
    require(!write && write.error().code == ErrorCode::permission_denied, "read-only write accepted");
    Result<bool> removal = settings.value().erase(created.value().info.id, created.value().info.revision);
    require(!removal && removal.error().code == ErrorCode::permission_denied, "read-only erase accepted");
    require(!std::filesystem::exists(root / ".lock"), "read-only access created a lock file");
    Result<ObjectCollection> absent = store.value().collection("connections");
    require(absent && absent.value().list() && absent.value().list().value().objects.empty(),
            "missing read-only collection not empty");
    require(!std::filesystem::exists(root / "connections"), "read-only access created a collection");

    require(::chmod((root / "settings").c_str(), 0770) == 0, "cannot make collection group-writable");
    require(!store.value().collection("settings"), "group-writable read-only collection accepted");
    require(::chmod((root / "settings").c_str(), 0750) == 0, "cannot restore collection");
    std::filesystem::create_directory_symlink(root / "settings", root / "linked");
    Result<ObjectCollection> linked = store.value().collection("linked");
    require(!linked && linked.error().code == ErrorCode::unsafe_path, "read-only mode followed symlink");
}

void lockTimeout()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "gui";
    ObjectCollection stored = collection(root);
    Result<StoredObject> created = stored.create("{\"value\":1}");
    require(created.hasValue(), "cannot seed lock test");
    int ready[2]{};
    int release[2]{};
    require(::pipe(ready) == 0 && ::pipe(release) == 0, "cannot make lock sync pipes");
    const pid_t child = ::fork();
    require(child >= 0, "cannot fork lock holder");
    if (child == 0) {
        // Simulates a hung second instance holding the collection lock.
        ::close(ready[0]);
        ::close(release[1]);
        const int lock = ::open((root / ".lock").c_str(), O_RDWR | O_CLOEXEC);
        if (lock < 0 || ::flock(lock, LOCK_EX) < 0) ::_exit(60);
        if (::write(ready[1], "x", 1) != 1) ::_exit(61);
        char signal = 0;
        if (::read(release[0], &signal, 1) != 1) ::_exit(62);
        ::_exit(0);
    }
    ::close(ready[1]);
    ::close(release[0]);
    char signal = 0;
    require(::read(ready[0], &signal, 1) == 1, "lock holder did not start");
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    Result<ObjectCollection> blocked = openCollection(ownerRoot(root), "wireguard", std::chrono::milliseconds(100));
    const std::chrono::steady_clock::duration waited = std::chrono::steady_clock::now() - started;
    require(!blocked && blocked.error().code == ErrorCode::lock_timeout, "held lock did not time out");
    require(waited >= std::chrono::milliseconds(100) && waited < std::chrono::seconds(3), "lock timeout not honoured");
    Result<ObjectCollection> immediate = openCollection(ownerRoot(root), "wireguard", std::chrono::milliseconds(0));
    require(!immediate && immediate.error().code == ErrorCode::lock_timeout, "zero timeout blocked or succeeded");
    // The lock is per owner: every other collection of the owner is blocked too.
    Result<ObjectCollection> other = openCollection(ownerRoot(root), "connections", std::chrono::milliseconds(0));
    require(!other && other.error().code == ErrorCode::lock_timeout, "owner lock did not cover other collections");
    // Read-only access takes no lock and is not blocked by a writer.
    Result<ObjectCollection> reader = openCollection(ownerRoot(root), "wireguard", std::chrono::milliseconds(0),
                                                     AccessMode::read_only);
    require(reader && reader.value().read(created.value().info.id).hasValue(), "read-only access blocked by lock");
    require(::write(release[1], "x", 1) == 1, "cannot release lock holder");
    ::close(release[1]);
    ::close(ready[0]);
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "lock holder failed");
    require(stored.read(created.value().info.id).hasValue(), "lock not usable after release");
}

#endif
} // namespace

int main()
{
    try {
        jsonTests();
        idTests();
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        basicCrud();
        validationAndCorruption();
        attacksAndPermissions();
        crashRecovery();
        processConflicts();
        copyAndOwnerIsolation();
        additionalCorruptionAndSecurity();
        lockTimeout();
        readOnlyMode();
#endif
        std::cout << "AOWIS-CONFIG object store tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
