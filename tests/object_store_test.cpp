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
#include <csignal>
#include <cstdlib>
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

ObjectCollection collection(const std::filesystem::path &path, std::string_view name = "wireguard")
{
    Result<ObjectCollection> opened = ObjectCollection::open(ownerRoot(path), name);
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
    requireMode(root / "wireguard" / ".lock", 0600);
    require(stored.list() && stored.list().value().empty(), "new collection not empty");
    Result<StoredObject> created = stored.create("{\"conf\":\"[Interface]\\nPrivateKey = EXAMPLE\\n\",\"name\":\"Test\"}");
    require(created.hasValue(), "cannot create test object");
    const ObjectId id = created.value().info.id;
    requireMode(root / "wireguard" / (id.value() + ".json"), 0600);
    require(stored.list() && stored.list().value().size() == 1, "listed record missing");
    require(stored.list().value()[0].id.value() == id.value(), "listed wrong record ID");
    Result<StoredObject> reloaded = stored.read(id);
    require(reloaded && reloaded.value().json == created.value().json, "payload lost on read");
    require(reloaded.value().info.revision == created.value().info.revision, "revision changed on read");
    Result<ObjectCollection> reopened = ObjectCollection::open(ownerRoot(root), "wireguard");
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
    require(stored.list() && stored.list().value().empty(), "deleted record still listed");
    require(!stored.erase(id, updated.value().info.revision), "deleted twice");
}

void validationAndCorruption()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "gui";
    ObjectCollection stored = collection(root);
    Result<StoredObject> bad = stored.create("{\"bad\":}");
    require(!bad && bad.error().code == ErrorCode::invalid_json, "invalid data was stored");
    Result<StoredObject> future = stored.create("{}", 2);
    require(!future && future.error().code == ErrorCode::unsupported_schema, "unsupported writer schema accepted");
    require(!ObjectCollection::open(ownerRoot(root), "../bad"), "collection path traversal accepted");
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
    require(!stored.list() && stored.list().error().code == ErrorCode::corrupt_object,
            "corrupt object silently excluded from list");
    {
        std::ofstream output(file, std::ios::trunc);
        output << "{\"schema_version\":2,\"id\":\"" << created.value().info.id.value()
               << "\",\"revision\":\"" << created.value().info.revision
               << "\",\"payload\":{}}";
    }
    require(!stored.read(created.value().info.id)
            && stored.read(created.value().info.id).error().code == ErrorCode::unsupported_schema,
            "unsupported reader schema accepted");
}

void attacksAndPermissions()
{
    TemporaryDirectory directory;
    const std::filesystem::path root = directory.path() / "real" / "gui";
    ObjectCollection stored = collection(root);
    const std::filesystem::path link = directory.path() / "link";
    std::filesystem::create_symlink(directory.path() / "real", link);
    require(!ObjectCollection::open(ownerRoot(link / "gui"), "wireguard"), "symlink root accepted");
    std::filesystem::create_symlink(root / "wireguard", root / "evil");
    require(!ObjectCollection::open(ownerRoot(root), "evil"), "symlink collection accepted");
    Result<StoredObject> created = stored.create("{}");
    require(created.hasValue(), "cannot seed symlink test");
    const std::filesystem::path file = root / "wireguard" / (created.value().info.id.value() + ".json");
    std::filesystem::remove(file);
    std::filesystem::create_symlink("/etc/passwd", file);
    Result<StoredObject> result = stored.read(created.value().info.id);
    require(!result && result.error().code == ErrorCode::unsafe_path, "symlink object followed");
    require(!stored.list(), "list followed malicious symlink");
    std::filesystem::remove(file);
    require(::chmod((root / "wireguard").c_str(), 0755) == 0, "cannot alter test directory permissions");
    require(!ObjectCollection::open(ownerRoot(root), "wireguard"), "insecure collection accepted");
    require(::chmod((root / "wireguard").c_str(), 0700) == 0, "cannot restore permissions");
    require(::chmod(root.c_str(), 0777) == 0, "cannot alter owner permissions");
    require(!ObjectCollection::open(ownerRoot(root), "wireguard"), "insecure owner accepted");
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
    require(mobile.list() && mobile.list().value().empty(), "different owners share storage");
    require(stored.list() && stored.list().value().size() == 1, "other owner affected original storage");
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
        output << "{\"schema_version\":1,\"id\":\"" << unrelated.value().value()
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
    require(!stored.list() && stored.list().error().code == ErrorCode::corrupt_object,
            "unrecognized object file was silently skipped");
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
#endif
        std::cout << "AOWIS-CONFIG object store tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
