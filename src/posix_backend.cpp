// Native POSIX storage backend (Linux, Android, macOS, BSD). Selected in
// CMakeLists.txt for every non-Windows, non-Emscripten build. Implements the
// contract in storage_backend.h with descriptor-based traversal, private
// permissions, a time-limited flock(2) collection lock, fsynced temporary
// files and atomic link/rename publication.
#include "storage_backend.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aowis::config::detail {
namespace {
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
        if (errno == ELOOP || errno == ENOTDIR) {
            return Result<FileDescriptor>::failure(
                {ErrorCode::unsafe_path, "Configuration owner or collection directory is a symlink or not a directory"});
        }
        return Result<FileDescriptor>::failure(operatingError("Open configuration directory", errno));
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
    // Ancestors of the owner directory belong to the user's or system's
    // filesystem layout, not to AOWIS: /home -> /var/home (Silverblue),
    // /var -> /private/var (macOS) or a dotfile-managed ~/.config symlink are
    // legitimate. They are followed and created on demand, but not
    // permission-checked. The security boundary is the owner directory: it
    // and everything below it are opened with O_NOFOLLOW and must be owned
    // by the effective user with mode 0700, so a redirected ancestor can only
    // lead to a directory that is already private to this user.
    for (std::size_t index = 0; index + 1 < segments.size(); ++index) {
        const int created = ::mkdirat(current.get(), segments[index].c_str(), 0700);
        if (created < 0 && errno != EEXIST) {
            return Result<FileDescriptor>::failure(operatingError("Create configuration ancestor directory", errno));
        }
        if (created == 0 && ::fsync(current.get()) < 0) {
            return Result<FileDescriptor>::failure(operatingError("Sync parent after directory creation", errno));
        }
        FileDescriptor next(::openat(current.get(), segments[index].c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!next.valid()) {
            return Result<FileDescriptor>::failure(operatingError("Open configuration ancestor directory", errno));
        }
        current = std::move(next);
    }
    Result<FileDescriptor> owner = directoryAt(current.get(), segments.back(), true);
    if (!owner) return owner;
    return directoryAt(owner.value().get(), collection_name, true);
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

class LockedDirectory final {
public:
    // recover=true removes orphan temporaries left by a crashed writer. Only
    // mutating operations and open() need it: readers skip ".tmp-*" entries,
    // so a leftover temporary is harmless until the next write.
    static Result<LockedDirectory> acquire(const std::filesystem::path &root, const std::string &collection,
                                   std::chrono::milliseconds timeout, bool recover)
    {
        Result<FileDescriptor> directory = collectionDirectory(root, collection);
        if (!directory) return Result<LockedDirectory>::failure(directory.error());
        FileDescriptor lock(::openat(directory.value().get(), ".lock",
                                     O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
        if (!lock.valid()) return Result<LockedDirectory>::failure(operatingError("Open collection lock", errno));
        struct stat status{};
        if (::fstat(lock.get(), &status) < 0 || !S_ISREG(status.st_mode)
            || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
            return Result<LockedDirectory>::failure({ErrorCode::unsafe_path, "Unsafe collection lock file"});
        }
        // Never block indefinitely: a hung second instance or a stalled
        // network filesystem would otherwise freeze the caller's thread.
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
        while (::flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
            if (errno == EINTR) continue;
            if (errno != EWOULDBLOCK) {
                return Result<LockedDirectory>::failure(operatingError("Acquire collection lock", errno));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return Result<LockedDirectory>::failure(
                    {ErrorCode::lock_timeout, "Collection is locked by another process; retry later"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (recover) {
            Result<bool> recovered = removeOrphanTemps(directory.value().get());
            if (!recovered) return Result<LockedDirectory>::failure(recovered.error());
        }
        return Result<LockedDirectory>::success(LockedDirectory(std::move(directory.value()), std::move(lock)));
    }

    [[nodiscard]] int directory() const noexcept { return this->directory_.get(); }

private:
    LockedDirectory(FileDescriptor directory, FileDescriptor lock)
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

Result<bool> storeRecord(int directory, const ObjectId &id, std::string_view encoded, bool create_only)
{
    Result<ObjectId> temp_id = ObjectId::generate();
    if (!temp_id) return Result<bool>::failure(temp_id.error());
    const std::string temporary_name = ".tmp-" + temp_id.value().value();
    const std::string filename = id.value() + ".json";
    FileDescriptor temporary(::openat(directory, temporary_name.c_str(),
                                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!temporary.valid()) return Result<bool>::failure(operatingError("Create temporary object", errno));
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

class PosixSession final : public CollectionSession {
public:
    explicit PosixSession(LockedDirectory locked) : locked_(std::move(locked)) {}

    [[nodiscard]] Result<std::vector<std::string>> entryNames() const override
    {
        const int copied = ::openat(this->locked_.directory(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (copied < 0) return Result<std::vector<std::string>>::failure(operatingError("Duplicate collection directory", errno));
        DIR *stream = ::fdopendir(copied);
        if (stream == nullptr) {
            const int failure = errno;
            ::close(copied);
            return Result<std::vector<std::string>>::failure(operatingError("Enumerate collection", failure));
        }
        std::vector<std::string> names;
        while (true) {
            errno = 0;
            const dirent *entry = ::readdir(stream);
            if (entry == nullptr) break;
            std::string name(entry->d_name);
            // ".", "..", ".lock" and ".tmp-*" never end in ".json".
            if (name.ends_with(".json")) names.push_back(std::move(name));
        }
        const int iteration_error = errno;
        ::closedir(stream);
        if (iteration_error != 0) {
            return Result<std::vector<std::string>>::failure(operatingError("Enumerate collection", iteration_error));
        }
        return Result<std::vector<std::string>>::success(std::move(names));
    }

    [[nodiscard]] Result<std::string> readEntry(const ObjectId &id) const override
    {
        return readFile(this->locked_.directory(), id);
    }

    [[nodiscard]] Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) override
    {
        return storeRecord(this->locked_.directory(), id, bytes, create_only);
    }

    [[nodiscard]] Result<bool> removeEntry(const ObjectId &id) override
    {
        const std::string filename = id.value() + ".json";
        if (::unlinkat(this->locked_.directory(), filename.c_str(), 0) < 0) {
            if (errno == ENOENT) return Result<bool>::failure({ErrorCode::not_found, "Object does not exist"});
            return Result<bool>::failure(operatingError("Delete object", errno));
        }
        if (::fsync(this->locked_.directory()) < 0) {
            return Result<bool>::failure(operatingError("Sync collection after deletion", errno));
        }
        return Result<bool>::success(true);
    }

private:
    LockedDirectory locked_;
};

class PosixBackend final : public CollectionBackend {
public:
    PosixBackend(std::filesystem::path owner_root, std::string collection, std::chrono::milliseconds lock_timeout)
        : owner_root_(std::move(owner_root)), collection_(std::move(collection)), lock_timeout_(lock_timeout) {}

    [[nodiscard]] Result<std::unique_ptr<CollectionSession>> lock(bool recover) const override
    {
        // The path is re-walked and re-verified for every session, so a
        // directory swapped between calls is detected rather than trusted.
        Result<LockedDirectory> locked = LockedDirectory::acquire(
            this->owner_root_, this->collection_, this->lock_timeout_, recover);
        if (!locked) return Result<std::unique_ptr<CollectionSession>>::failure(locked.error());
        return Result<std::unique_ptr<CollectionSession>>::success(
            std::make_unique<PosixSession>(std::move(locked.value())));
    }

private:
    std::filesystem::path owner_root_;
    std::string collection_;
    std::chrono::milliseconds lock_timeout_;
};
} // namespace

Result<std::shared_ptr<const CollectionBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, const std::string &collection,
    std::chrono::milliseconds lock_timeout)
{
    return Result<std::shared_ptr<const CollectionBackend>>::success(
        std::make_shared<const PosixBackend>(owner_root, collection, lock_timeout));
}

} // namespace aowis::config::detail
