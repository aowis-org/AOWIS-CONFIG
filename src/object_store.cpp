#include "aowis/config/object_store.h"

#include "aowis/config/json.h"
#include "json_internal.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace aowis::config {
namespace {
constexpr std::size_t max_record_bytes = 8U * 1024U * 1024U;

bool validRevision(std::string_view revision)
{
    if (revision.size() != 32) return false;
    for (char value : revision) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
    }
    return true;
}

Result<bool> validatePayload(std::string_view payload, std::uint32_t schema_version)
{
    if (schema_version != 1) {
        return Result<bool>::failure({ErrorCode::unsupported_schema, "Only object schema version 1 is supported"});
    }
    if (payload.size() > max_record_bytes - 512) {
        return Result<bool>::failure({ErrorCode::object_too_large, "Object payload exceeds size limit"});
    }
    return validateJsonObject(payload);
}

std::string encodeRecord(const StoredObject &record)
{
    std::string text;
    text.reserve(record.json.size() + 160);
    text += "{\"schema_version\":1,\"id\":\"";
    text += record.info.id.value();
    text += "\",\"revision\":\"";
    text += record.info.revision;
    text += "\",\"payload\":";
    text += record.json;
    text += "}\n";
    return text;
}

Result<StoredObject> decodeRecord(std::string_view text, const ObjectId &requested)
{
    Result<std::vector<detail::JsonMember>> members = detail::parseJsonObject(text);
    if (!members) {
        if (members.error().code == ErrorCode::object_too_large) {
            return Result<StoredObject>::failure(members.error());
        }
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object is not valid JSON"});
    }
    std::string_view schema;
    std::string_view id;
    std::string_view revision;
    std::string_view payload;
    for (const detail::JsonMember &member : members.value()) {
        std::string_view *destination = nullptr;
        if (member.key == "schema_version") destination = &schema;
        else if (member.key == "id") destination = &id;
        else if (member.key == "revision") destination = &revision;
        else if (member.key == "payload") destination = &payload;
        if (destination == nullptr || !destination->empty()) {
            return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object has invalid or duplicate fields"});
        }
        *destination = member.value;
    }
    if (schema.empty() || id.size() < 2 || revision.size() < 2 || payload.empty()) {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object is missing required fields"});
    }
    if (schema != "1") {
        // Recognize syntactically valid integer versions, so future formats
        // fail explicitly instead of being mistaken for damaged records.
        std::uint32_t parsed_version = 0;
        const std::from_chars_result parsed = std::from_chars(
            schema.data(), schema.data() + schema.size(), parsed_version);
        if (parsed.ec == std::errc{} && parsed.ptr == schema.data() + schema.size()) {
            return Result<StoredObject>::failure({ErrorCode::unsupported_schema, "Stored object uses an unsupported schema version"});
        }
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object has an invalid schema version"});
    }
    if (id.front() != '"' || id.back() != '"' || revision.front() != '"'
        || revision.back() != '"') {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object identity fields are not strings"});
    }
    id.remove_prefix(1); id.remove_suffix(1);
    revision.remove_prefix(1); revision.remove_suffix(1);
    Result<ObjectId> parsed_id = ObjectId::parse(id);
    if (!parsed_id || parsed_id.value().value() != requested.value() || !validRevision(revision)) {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object identity or revision is invalid"});
    }
    if (payload.front() != '{') {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object payload must be a JSON object"});
    }
    return Result<StoredObject>::success({{parsed_id.value(), 1, std::string(revision)}, std::string(payload)});
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
class FileDescriptor final {
public:
    explicit FileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
    ~FileDescriptor() { if (this->descriptor_ >= 0) ::close(this->descriptor_); }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept
    {
        if (this != &other) {
            if (this->descriptor_ >= 0) ::close(this->descriptor_);
            this->descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return this->descriptor_; }
    [[nodiscard]] bool valid() const noexcept { return this->descriptor_ >= 0; }

private:
    int descriptor_;
};

Error operatingError(std::string_view operation, int error)
{
    const ErrorCode code = (error == EACCES || error == EPERM)
                               ? ErrorCode::permission_denied : ErrorCode::io_failure;
    return {code, std::string(operation) + " failed (errno " + std::to_string(error) + ")"};
}

Result<FileDescriptor> directoryAt(int parent, const std::string &name, bool restricted)
{
    const int created = ::mkdirat(parent, name.c_str(), 0700);
    if (created < 0 && errno != EEXIST) {
        return Result<FileDescriptor>::failure(operatingError("Create configuration directory", errno));
    }
    if (created == 0 && ::fsync(parent) < 0) {
        return Result<FileDescriptor>::failure(operatingError("Sync parent after directory creation", errno));
    }
    FileDescriptor directory(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory.valid()) {
        return Result<FileDescriptor>::failure(
            {ErrorCode::unsafe_path, "Configuration path contains a symlink or a non-directory component"});
    }
    struct stat status{};
    if (::fstat(directory.get(), &status) < 0) {
        return Result<FileDescriptor>::failure(operatingError("Inspect configuration directory", errno));
    }
    if (restricted && (status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0
                       || (status.st_mode & S_IWUSR) == 0)) {
        return Result<FileDescriptor>::failure(
            {ErrorCode::permission_denied, "Configuration owner directory must belong to this user and be private (0700)"});
    }
    return Result<FileDescriptor>::success(std::move(directory));
}

Result<FileDescriptor> collectionDirectory(const std::filesystem::path &root_path,
                                          const std::string &collection_name)
{
    FileDescriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current.valid()) return Result<FileDescriptor>::failure(operatingError("Open filesystem root", errno));
    std::vector<std::string> segments;
    for (const std::filesystem::path &component : root_path) {
        const std::string segment = component.string();
        if (segment == "/" || segment.empty()) continue;
        if (segment == "." || segment == ".." || segment.find('\0') != std::string::npos) {
            return Result<FileDescriptor>::failure({ErrorCode::unsafe_path, "Invalid configuration root component"});
        }
        segments.push_back(segment);
    }
    if (segments.empty()) {
        return Result<FileDescriptor>::failure({ErrorCode::unsafe_path, "Filesystem root cannot be an owner directory"});
    }
    for (std::size_t index = 0; index < segments.size(); ++index) {
        Result<FileDescriptor> next = directoryAt(current.get(), segments[index], index + 1 == segments.size());
        if (!next) return next;
        current = std::move(next.value());
    }
    return directoryAt(current.get(), collection_name, true);
}

bool temporaryName(std::string_view name)
{
    if (!name.starts_with(".tmp-")) return false;
    return ObjectId::parse(name.substr(5)).hasValue();
}

Result<bool> removeOrphanTemps(int directory)
{
    const int copied = ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (copied < 0) return Result<bool>::failure(operatingError("Duplicate directory for recovery", errno));
    DIR *stream = ::fdopendir(copied);
    if (stream == nullptr) {
        const int failure = errno;
        ::close(copied);
        return Result<bool>::failure(operatingError("Read directory for recovery", failure));
    }
    bool removed = false;
    while (true) {
        errno = 0;
        const dirent *entry = ::readdir(stream);
        if (entry == nullptr) break;
        const std::string name(entry->d_name);
        if (!temporaryName(name)) continue;
        struct stat status{};
        if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;
            const int failure = errno;
            ::closedir(stream);
            return Result<bool>::failure(operatingError("Inspect orphan temporary file", failure));
        }
        if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid()) {
            ::closedir(stream);
            return Result<bool>::failure({ErrorCode::unsafe_path, "Unsafe temporary entry in configuration collection"});
        }
        if (::unlinkat(directory, name.c_str(), 0) < 0) {
            const int failure = errno;
            ::closedir(stream);
            return Result<bool>::failure(operatingError("Remove orphan temporary file", failure));
        }
        removed = true;
    }
    const int read_failure = errno;
    ::closedir(stream);
    if (read_failure != 0) return Result<bool>::failure(operatingError("Enumerate recovery directory", read_failure));
    if (removed && ::fsync(directory) < 0) {
        return Result<bool>::failure(operatingError("Sync recovery directory", errno));
    }
    return Result<bool>::success(removed);
}

class Session final {
public:
    static Result<Session> acquire(const std::filesystem::path &root, const std::string &collection)
    {
        Result<FileDescriptor> directory = collectionDirectory(root, collection);
        if (!directory) return Result<Session>::failure(directory.error());
        FileDescriptor lock(::openat(directory.value().get(), ".lock",
                                     O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
        if (!lock.valid()) return Result<Session>::failure(operatingError("Open collection lock", errno));
        struct stat status{};
        if (::fstat(lock.get(), &status) < 0 || !S_ISREG(status.st_mode)
            || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
            return Result<Session>::failure({ErrorCode::unsafe_path, "Unsafe collection lock file"});
        }
        if (::flock(lock.get(), LOCK_EX) < 0) {
            return Result<Session>::failure(operatingError("Acquire collection lock", errno));
        }
        Result<bool> recovered = removeOrphanTemps(directory.value().get());
        if (!recovered) return Result<Session>::failure(recovered.error());
        return Result<Session>::success(Session(std::move(directory.value()), std::move(lock)));
    }

    [[nodiscard]] int directory() const noexcept { return this->directory_.get(); }

private:
    Session(FileDescriptor directory, FileDescriptor lock)
        : directory_(std::move(directory)), lock_(std::move(lock)) {}
    FileDescriptor directory_;
    FileDescriptor lock_;
};

Result<std::string> readFile(int directory, const ObjectId &id)
{
    const std::string filename = id.value() + ".json";
    FileDescriptor file(::openat(directory, filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!file.valid()) {
        if (errno == ENOENT) return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        if (errno == ELOOP) return Result<std::string>::failure({ErrorCode::unsafe_path, "Object file is a symlink"});
        return Result<std::string>::failure(operatingError("Open object", errno));
    }
    struct stat status{};
    if (::fstat(file.get(), &status) < 0) return Result<std::string>::failure(operatingError("Inspect object", errno));
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
        return Result<std::string>::failure({ErrorCode::unsafe_path, "Object file must be regular, owned and private (0600)"});
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > max_record_bytes) {
        return Result<std::string>::failure({ErrorCode::object_too_large, "Stored object exceeds size limit"});
    }
    std::string text;
    char buffer[8192];
    while (true) {
        const ssize_t count = ::read(file.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return Result<std::string>::failure(operatingError("Read object", errno));
        if (count == 0) break;
        if (text.size() + static_cast<std::size_t>(count) > max_record_bytes) {
            return Result<std::string>::failure({ErrorCode::object_too_large, "Stored object exceeds size limit"});
        }
        text.append(buffer, static_cast<std::size_t>(count));
    }
    return Result<std::string>::success(std::move(text));
}

Result<StoredObject> loadRecord(int directory, const ObjectId &id)
{
    Result<std::string> text = readFile(directory, id);
    if (!text) return Result<StoredObject>::failure(text.error());
    return decodeRecord(text.value(), id);
}

Result<bool> writeFileFully(int descriptor, std::string_view contents)
{
    std::size_t consumed = 0;
    while (consumed < contents.size()) {
        const ssize_t count = ::write(descriptor, contents.data() + consumed, contents.size() - consumed);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return Result<bool>::failure(operatingError("Write object", errno));
        consumed += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) < 0) return Result<bool>::failure(operatingError("Sync object", errno));
    return Result<bool>::success(true);
}

#if defined(AOWIS_CONFIG_TEST_FAULT_INJECTION)
void testCrashAt(std::string_view point)
{
    const char *requested = std::getenv("AOWIS_CONFIG_TEST_CRASH_AT");
    if (requested != nullptr && std::string_view(requested) == point) {
        ::raise(SIGKILL);
    }
}
#endif

Result<bool> storeRecord(int directory, const StoredObject &record, bool create_only)
{
    Result<ObjectId> temp_id = ObjectId::generate();
    if (!temp_id) return Result<bool>::failure(temp_id.error());
    const std::string temporary_name = ".tmp-" + temp_id.value().value();
    const std::string filename = record.info.id.value() + ".json";
    FileDescriptor temporary(::openat(directory, temporary_name.c_str(),
                                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!temporary.valid()) return Result<bool>::failure(operatingError("Create temporary object", errno));
    const std::string encoded = encodeRecord(record);
    Result<bool> written = writeFileFully(temporary.get(), encoded);
    if (!written) {
        ::unlinkat(directory, temporary_name.c_str(), 0);
        return written;
    }
#if defined(AOWIS_CONFIG_TEST_FAULT_INJECTION)
    testCrashAt("after-temp-fsync");
#endif
    // File contents have been fsynced; this closes the handle before install.
    temporary = FileDescriptor();
    int installed = 0;
    if (create_only) {
        installed = ::linkat(directory, temporary_name.c_str(), directory, filename.c_str(), 0);
    } else {
        installed = ::renameat(directory, temporary_name.c_str(), directory, filename.c_str());
    }
    if (installed < 0) {
        const int failure = errno;
        ::unlinkat(directory, temporary_name.c_str(), 0);
        if (failure == EEXIST) {
            return Result<bool>::failure({ErrorCode::conflict, "Object ID already exists"});
        }
        return Result<bool>::failure(operatingError("Install object", failure));
    }
    if (create_only && ::unlinkat(directory, temporary_name.c_str(), 0) < 0) {
        return Result<bool>::failure(operatingError("Remove installed object temporary link", errno));
    }
#if defined(AOWIS_CONFIG_TEST_FAULT_INJECTION)
    testCrashAt("after-publish");
#endif
    if (::fsync(directory) < 0) return Result<bool>::failure(operatingError("Sync collection directory", errno));
    return Result<bool>::success(true);
}

Result<std::string> newRevision()
{
    Result<ObjectId> generated = ObjectId::generate();
    if (!generated) return Result<std::string>::failure(generated.error());
    std::string revision;
    for (char value : generated.value().value()) {
        if (value != '-') revision += value;
    }
    return Result<std::string>::success(std::move(revision));
}

#else
Result<std::string> newRevision()
{
    return Result<std::string>::failure({ErrorCode::unsupported_platform, "Native storage backend unavailable"});
}
#endif
} // namespace

Result<ObjectCollection> ObjectCollection::open(const ConfigRoot &root, std::string_view collection)
{
    Result<OwnerId> checked = OwnerId::parse(collection);
    if (!checked) {
        return Result<ObjectCollection>::failure({ErrorCode::invalid_collection, "Invalid collection name"});
    }
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    static_cast<void>(root);
    return Result<ObjectCollection>::failure({ErrorCode::unsupported_platform,
                                               "Native POSIX storage backend unavailable on this platform"});
#else
    Result<Session> session = Session::acquire(root.path(), checked.value().value());
    if (!session) return Result<ObjectCollection>::failure(session.error());
    return Result<ObjectCollection>::success(ObjectCollection(root.path(), checked.value().value()));
#endif
}

Result<std::vector<ObjectInfo>> ObjectCollection::list() const
{
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    return Result<std::vector<ObjectInfo>>::failure({ErrorCode::unsupported_platform, "Native storage unavailable"});
#else
    Result<Session> session = Session::acquire(this->root_path_, this->name_);
    if (!session) return Result<std::vector<ObjectInfo>>::failure(session.error());
    const int copied = ::openat(session.value().directory(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (copied < 0) return Result<std::vector<ObjectInfo>>::failure(operatingError("Duplicate collection directory", errno));
    DIR *stream = ::fdopendir(copied);
    if (stream == nullptr) {
        const int failure = errno;
        ::close(copied);
        return Result<std::vector<ObjectInfo>>::failure(operatingError("Enumerate collection", failure));
    }
    std::vector<ObjectInfo> records;
    while (true) {
        errno = 0;
        const dirent *entry = ::readdir(stream);
        if (entry == nullptr) break;
        const std::string name(entry->d_name);
        if (name == "." || name == ".." || name == ".lock") continue;
        if (!name.ends_with(".json")) continue;
        Result<ObjectId> id = ObjectId::parse(std::string_view(name).substr(0, name.size() - 5));
        if (!id) {
            ::closedir(stream);
            return Result<std::vector<ObjectInfo>>::failure({ErrorCode::corrupt_object, "Collection contains an invalid JSON object filename"});
        }
        Result<StoredObject> record = loadRecord(session.value().directory(), id.value());
        if (!record) {
            ::closedir(stream);
            return Result<std::vector<ObjectInfo>>::failure(record.error());
        }
        records.push_back(record.value().info);
    }
    const int iteration_error = errno;
    ::closedir(stream);
    if (iteration_error != 0) return Result<std::vector<ObjectInfo>>::failure(operatingError("Enumerate collection", iteration_error));
    std::sort(records.begin(), records.end(), [](const ObjectInfo &left, const ObjectInfo &right) {
        return left.id.value() < right.id.value();
    });
    return Result<std::vector<ObjectInfo>>::success(std::move(records));
#endif
}

Result<StoredObject> ObjectCollection::read(const ObjectId &id) const
{
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    static_cast<void>(id);
    return Result<StoredObject>::failure({ErrorCode::unsupported_platform, "Native storage unavailable"});
#else
    Result<Session> session = Session::acquire(this->root_path_, this->name_);
    if (!session) return Result<StoredObject>::failure(session.error());
    return loadRecord(session.value().directory(), id);
#endif
}

Result<StoredObject> ObjectCollection::create(std::string_view json, std::uint32_t schema_version) const
{
    Result<bool> checked = validatePayload(json, schema_version);
    if (!checked) return Result<StoredObject>::failure(checked.error());
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    return Result<StoredObject>::failure({ErrorCode::unsupported_platform, "Native storage unavailable"});
#else
    Result<ObjectId> id = ObjectId::generate();
    if (!id) return Result<StoredObject>::failure(id.error());
    Result<std::string> revision = newRevision();
    if (!revision) return Result<StoredObject>::failure(revision.error());
    StoredObject record{{id.value(), schema_version, revision.value()}, std::string(json)};
    Result<Session> session = Session::acquire(this->root_path_, this->name_);
    if (!session) return Result<StoredObject>::failure(session.error());
    Result<bool> stored = storeRecord(session.value().directory(), record, true);
    if (!stored) return Result<StoredObject>::failure(stored.error());
    return Result<StoredObject>::success(std::move(record));
#endif
}

Result<StoredObject> ObjectCollection::update(const ObjectId &id, std::string_view json,
                                                std::string_view expected_revision,
                                                std::uint32_t schema_version) const
{
    Result<bool> checked = validatePayload(json, schema_version);
    if (!checked) return Result<StoredObject>::failure(checked.error());
    if (!validRevision(expected_revision)) {
        return Result<StoredObject>::failure({ErrorCode::conflict, "A valid expected revision is required"});
    }
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    return Result<StoredObject>::failure({ErrorCode::unsupported_platform, "Native storage unavailable"});
#else
    Result<Session> session = Session::acquire(this->root_path_, this->name_);
    if (!session) return Result<StoredObject>::failure(session.error());
    Result<StoredObject> previous = loadRecord(session.value().directory(), id);
    if (!previous) return Result<StoredObject>::failure(previous.error());
    if (previous.value().info.revision != expected_revision) {
        return Result<StoredObject>::failure({ErrorCode::conflict, "Object changed since it was read"});
    }
    Result<std::string> revision = newRevision();
    if (!revision) return Result<StoredObject>::failure(revision.error());
    StoredObject updated{{id, schema_version, revision.value()}, std::string(json)};
    Result<bool> stored = storeRecord(session.value().directory(), updated, false);
    if (!stored) return Result<StoredObject>::failure(stored.error());
    return Result<StoredObject>::success(std::move(updated));
#endif
}

Result<bool> ObjectCollection::erase(const ObjectId &id, std::string_view expected_revision) const
{
    if (!validRevision(expected_revision)) {
        return Result<bool>::failure({ErrorCode::conflict, "A valid expected revision is required"});
    }
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    return Result<bool>::failure({ErrorCode::unsupported_platform, "Native storage unavailable"});
#else
    Result<Session> session = Session::acquire(this->root_path_, this->name_);
    if (!session) return Result<bool>::failure(session.error());
    Result<StoredObject> previous = loadRecord(session.value().directory(), id);
    if (!previous) return Result<bool>::failure(previous.error());
    if (previous.value().info.revision != expected_revision) {
        return Result<bool>::failure({ErrorCode::conflict, "Object changed since it was read"});
    }
    const std::string filename = id.value() + ".json";
    if (::unlinkat(session.value().directory(), filename.c_str(), 0) < 0) {
        return Result<bool>::failure(operatingError("Delete object", errno));
    }
    if (::fsync(session.value().directory()) < 0) {
        return Result<bool>::failure(operatingError("Sync collection after deletion", errno));
    }
    return Result<bool>::success(true);
#endif
}

} // namespace aowis::config
