// Native POSIX storage backend (Linux, Android, macOS, BSD). Selected in
// CMakeLists.txt for every non-Windows, non-Emscripten build. Implements the
// contract in storage_backend.h with descriptor-based traversal, a
// time-limited flock(2) lock on <owner>/.lock covering all collections,
// fsynced temporary files and atomic link/rename publication.
//
// Two access policies:
//  - read_write: the owner directory and everything below must belong to the
//    effective user and be private (0700 directories, 0600 files). Missing
//    directories are created.
//  - read_only: nothing is created, locked or written. Entries may belong to
//    the effective user or to root and may be group/other-readable (e.g.
//    0640 root:aowis under /etc), but must never be group/other-writable.
// In both modes nothing at or below the owner directory may be a symlink.
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


// Ownership/permission policy for everything at or below the owner root.
bool acceptable(const struct stat &status, BackendMode mode, bool directory)
{
    if (mode == BackendMode::read_write) {
        if (status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) return false;
        return !directory || (status.st_mode & S_IWUSR) != 0;
    }
    // Administrator-managed roots: root-owned is fine, readable by the
    // service group is fine, writable by anyone but the owner never is.
    if (status.st_uid != ::geteuid() && status.st_uid != 0) return false;
    return (status.st_mode & 0022) == 0;
}

// Opens (read_write: creates first) a directory below the owner boundary.
// ENOENT in read_only mode is reported as not_found so callers can treat a
// missing collection as empty.
Result<FileDescriptor> directoryAt(int parent, const std::string &name, BackendMode mode)
{
    if (mode == BackendMode::read_write) {
        const int created = ::mkdirat(parent, name.c_str(), 0700);
        if (created < 0 && errno != EEXIST) {
            return Result<FileDescriptor>::failure(operatingError("Create configuration directory", errno));
        }
        if (created == 0 && ::fsync(parent) < 0) {
            return Result<FileDescriptor>::failure(operatingError("Sync parent after directory creation", errno));
        }
    }
    FileDescriptor directory(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory.valid()) {
        if (errno == ENOENT) return Result<FileDescriptor>::failure({ErrorCode::not_found, "Configuration directory does not exist"});
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
    if (!acceptable(status, mode, true)) {
        return Result<FileDescriptor>::failure({ErrorCode::permission_denied,
            mode == BackendMode::read_write
                ? "Configuration directory must belong to this user and be private (0700)"
                : "Read-only configuration directory must belong to this user or root and not be group/other-writable"});
    }
    return Result<FileDescriptor>::success(std::move(directory));
}

Result<FileDescriptor> ownerDirectory(const std::filesystem::path &root_path, BackendMode mode)
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
    // legitimate. They are followed (and in read_write mode created), but not
    // permission-checked. The owner directory is the security boundary.
    for (std::size_t index = 0; index + 1 < segments.size(); ++index) {
        if (mode == BackendMode::read_write) {
            const int created = ::mkdirat(current.get(), segments[index].c_str(), 0700);
            if (created < 0 && errno != EEXIST) {
                return Result<FileDescriptor>::failure(operatingError("Create configuration ancestor directory", errno));
            }
            if (created == 0 && ::fsync(current.get()) < 0) {
                return Result<FileDescriptor>::failure(operatingError("Sync parent after directory creation", errno));
            }
        }
        FileDescriptor next(::openat(current.get(), segments[index].c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!next.valid()) {
            if (errno == ENOENT) {
                return Result<FileDescriptor>::failure({ErrorCode::not_found, "Configuration root does not exist"});
            }
            return Result<FileDescriptor>::failure(operatingError("Open configuration ancestor directory", errno));
        }
        current = std::move(next);
    }
    Result<FileDescriptor> owner = directoryAt(current.get(), segments.back(), mode);
    if (!owner && owner.error().code == ErrorCode::not_found) {
        return Result<FileDescriptor>::failure({ErrorCode::not_found, "Configuration root does not exist"});
    }
    return owner;
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


Result<std::string> readFile(int directory, const ObjectId &id, BackendMode mode)
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
    if (!S_ISREG(status.st_mode) || !acceptable(status, mode, false)) {
        return Result<std::string>::failure({ErrorCode::unsafe_path,
            mode == BackendMode::read_write
                ? "Object file must be regular, owned and private (0600)"
                : "Read-only object file must be regular, owned by this user or root and not group/other-writable"});
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


Result<bool> readOnlyWrite()
{
    return Result<bool>::failure({ErrorCode::permission_denied, "Configuration store is opened read-only"});
}

// One collection inside a held owner session. directory_ is invalid for a
// collection that does not exist in read-only mode: it then reads as empty.
class PosixCollection final : public CollectionSession {
public:
    PosixCollection(FileDescriptor directory, BackendMode mode)
        : directory_(std::move(directory)), mode_(mode) {}

    [[nodiscard]] Result<std::vector<std::string>> entryNames() const override
    {
        std::vector<std::string> names;
        if (!this->directory_.valid()) return Result<std::vector<std::string>>::success(std::move(names));
        const int copied = ::openat(this->directory_.get(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (copied < 0) return Result<std::vector<std::string>>::failure(operatingError("Duplicate collection directory", errno));
        DIR *stream = ::fdopendir(copied);
        if (stream == nullptr) {
            const int failure = errno;
            ::close(copied);
            return Result<std::vector<std::string>>::failure(operatingError("Enumerate collection", failure));
        }
        while (true) {
            errno = 0;
            const dirent *entry = ::readdir(stream);
            if (entry == nullptr) break;
            std::string name(entry->d_name);
            // ".", ".." and ".tmp-*" never end in ".json".
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
        if (!this->directory_.valid()) {
            return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        }
        return readFile(this->directory_.get(), id, this->mode_);
    }

    [[nodiscard]] Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) override
    {
        if (this->mode_ == BackendMode::read_only) return readOnlyWrite();
        return storeRecord(this->directory_.get(), id, bytes, create_only);
    }

    [[nodiscard]] Result<bool> removeEntry(const ObjectId &id) override
    {
        if (this->mode_ == BackendMode::read_only) return readOnlyWrite();
        const std::string filename = id.value() + ".json";
        if (::unlinkat(this->directory_.get(), filename.c_str(), 0) < 0) {
            if (errno == ENOENT) return Result<bool>::failure({ErrorCode::not_found, "Object does not exist"});
            return Result<bool>::failure(operatingError("Delete object", errno));
        }
        if (::fsync(this->directory_.get()) < 0) {
            return Result<bool>::failure(operatingError("Sync collection after deletion", errno));
        }
        return Result<bool>::success(true);
    }

private:
    FileDescriptor directory_;
    BackendMode mode_;
};

class PosixOwnerSession final : public OwnerSessionImpl {
public:
    PosixOwnerSession(FileDescriptor owner, FileDescriptor lock, BackendMode mode)
        : owner_(std::move(owner)), lock_(std::move(lock)), mode_(mode) {}

    [[nodiscard]] Result<std::unique_ptr<CollectionSession>> collection(const std::string &name, bool recover) override
    {
        Result<FileDescriptor> directory = directoryAt(this->owner_.get(), name, this->mode_);
        if (!directory) {
            if (this->mode_ == BackendMode::read_only && directory.error().code == ErrorCode::not_found) {
                return Result<std::unique_ptr<CollectionSession>>::success(
                    std::make_unique<PosixCollection>(FileDescriptor(), this->mode_));
            }
            return Result<std::unique_ptr<CollectionSession>>::failure(directory.error());
        }
        if (recover && this->mode_ == BackendMode::read_write) {
            Result<bool> recovered = removeOrphanTemps(directory.value().get());
            if (!recovered) return Result<std::unique_ptr<CollectionSession>>::failure(recovered.error());
        }
        return Result<std::unique_ptr<CollectionSession>>::success(
            std::make_unique<PosixCollection>(std::move(directory.value()), this->mode_));
    }

private:
    FileDescriptor owner_;
    FileDescriptor lock_; // Holds flock(LOCK_EX) in read_write mode; invalid in read_only mode.
    BackendMode mode_;
};

Result<FileDescriptor> acquireOwnerLock(int owner, std::chrono::milliseconds timeout)
{
    FileDescriptor lock(::openat(owner, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    if (!lock.valid()) return Result<FileDescriptor>::failure(operatingError("Open owner lock", errno));
    struct stat status{};
    if (::fstat(lock.get(), &status) < 0 || !S_ISREG(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
        return Result<FileDescriptor>::failure({ErrorCode::unsafe_path, "Unsafe owner lock file"});
    }
    // Never block indefinitely: a hung second instance or a stalled network
    // filesystem would otherwise freeze the caller's thread.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
    while (::flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK) return Result<FileDescriptor>::failure(operatingError("Acquire owner lock", errno));
        if (std::chrono::steady_clock::now() >= deadline) {
            return Result<FileDescriptor>::failure(
                {ErrorCode::lock_timeout, "Configuration is locked by another process; retry later"});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return Result<FileDescriptor>::success(std::move(lock));
}

class PosixBackend final : public OwnerBackend {
public:
    PosixBackend(std::filesystem::path owner_root, BackendMode mode, std::chrono::milliseconds lock_timeout)
        : owner_root_(std::move(owner_root)), mode_(mode), lock_timeout_(lock_timeout) {}

    [[nodiscard]] Result<std::unique_ptr<OwnerSessionImpl>> lock() const override
    {
        // The path is re-walked and re-verified for every session, so a
        // directory swapped between calls is detected rather than trusted.
        Result<FileDescriptor> owner = ownerDirectory(this->owner_root_, this->mode_);
        if (!owner) return Result<std::unique_ptr<OwnerSessionImpl>>::failure(owner.error());
        FileDescriptor held;
        if (this->mode_ == BackendMode::read_write) {
            Result<FileDescriptor> locked = acquireOwnerLock(owner.value().get(), this->lock_timeout_);
            if (!locked) return Result<std::unique_ptr<OwnerSessionImpl>>::failure(locked.error());
            held = std::move(locked.value());
        }
        return Result<std::unique_ptr<OwnerSessionImpl>>::success(
            std::make_unique<PosixOwnerSession>(std::move(owner.value()), std::move(held), this->mode_));
    }

    [[nodiscard]] BackendMode mode() const noexcept override { return this->mode_; }

private:
    std::filesystem::path owner_root_;
    BackendMode mode_;
    std::chrono::milliseconds lock_timeout_;
};
} // namespace

Result<std::shared_ptr<const OwnerBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, BackendMode mode, std::chrono::milliseconds lock_timeout)
{
    return Result<std::shared_ptr<const OwnerBackend>>::success(
        std::make_shared<const PosixBackend>(owner_root, mode, lock_timeout));
}

} // namespace aowis::config::detail
