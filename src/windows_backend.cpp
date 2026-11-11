// Native Windows storage backend. Selected in CMakeLists.txt for WIN32
// builds. Implements the contract in storage_backend.h:
//
//  - Paths are used in their "\\?\" form (no MAX_PATH limit, no Win32 name
//    normalization). Ancestors above the owner root belong to the user's
//    profile layout and are followed (and in read_write mode created).
//  - The owner directory and every collection directory are held open for
//    the whole session without FILE_SHARE_DELETE, so they cannot be renamed,
//    deleted or replaced while the session uses their paths. NTFS also
//    refuses to rename an ancestor while a descendant is open.
//  - Nothing at or below the owner root may be a reparse point (symlink,
//    junction, mount point, cloud placeholder).
//  - Exclusive sessions: LockFileEx on <owner>\.lock, polled until the
//    configured timeout. Byte-range locks are per handle, so they exclude
//    other threads of this process as well as other processes.
//  - Writes: ".tmp-<uuidv7>" in the collection, FlushFileBuffers, then
//    MoveFileExW(MOVEFILE_WRITE_THROUGH), with REPLACE_EXISTING for updates
//    and without it for create_only (fails if the target exists). Deletions
//    flush the collection directory where the filesystem supports it.
//  - Access policy (replaces POSIX modes): the owner of every object must be
//    a trusted principal (this user, SYSTEM, Administrators). New
//    directories and files get a protected DACL granting only this user.
//      read_write: no access-allowed ACE for any other principal.
//      read_only:  other principals may read, never write (administrator-
//                  managed roots, e.g. under ProgramData).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Handle-based directory enumeration (FILE_FULL_DIR_INFO) needs Vista+.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "storage_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aowis::config::detail {
namespace {
class Handle final {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
    ~Handle() { this->reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    Handle &operator=(Handle &&other) noexcept
    {
        if (this != &other) {
            this->reset();
            this->handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return this->handle_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return this->handle_ != INVALID_HANDLE_VALUE && this->handle_ != nullptr;
    }

private:
    void reset() noexcept
    {
        if (this->valid()) ::CloseHandle(this->handle_);
        this->handle_ = INVALID_HANDLE_VALUE;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// Frees memory returned by the security APIs (LocalAlloc'ed).
class LocalMemory final {
public:
    explicit LocalMemory(void *memory = nullptr) : memory_(memory) {}
    ~LocalMemory() { if (this->memory_ != nullptr) ::LocalFree(this->memory_); }
    LocalMemory(const LocalMemory &) = delete;
    LocalMemory &operator=(const LocalMemory &) = delete;
    [[nodiscard]] void *get() const noexcept { return this->memory_; }

private:
    void *memory_;
};

Error operatingError(std::string_view operation, DWORD error)
{
    const ErrorCode code = (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
                               ? ErrorCode::permission_denied : ErrorCode::io_failure;
    return {code, std::string(operation) + " failed (Windows error " + std::to_string(error) + ")"};
}

bool missing(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Antivirus scanners and the search indexer briefly open files without
// FILE_SHARE_DELETE; a rename or delete then fails transiently.
bool transient(DWORD error)
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
}

constexpr int transient_retries = 50;
constexpr std::chrono::milliseconds transient_delay(20);

std::wstring widen(std::string_view ascii)
{
    // Collection names and object IDs are validated ASCII before they get here.
    return std::wstring(ascii.begin(), ascii.end());
}

std::string narrow(const wchar_t *text, std::size_t length)
{
    if (length == 0) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string("?");
    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), result.data(), size, nullptr, nullptr);
    return result;
}

// ---------------------------------------------------------------------------
// Identity and access policy
// ---------------------------------------------------------------------------

using SidBytes = std::vector<unsigned char>;

PSID sid(const SidBytes &bytes)
{
    return const_cast<unsigned char *>(bytes.data());
}

Result<SidBytes> wellKnownSid(WELL_KNOWN_SID_TYPE type)
{
    SidBytes bytes(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(bytes.size());
    if (!::CreateWellKnownSid(type, nullptr, bytes.data(), &size)) {
        return Result<SidBytes>::failure(operatingError("Create well-known SID", ::GetLastError()));
    }
    bytes.resize(size);
    return Result<SidBytes>::success(std::move(bytes));
}

// The process user and the principals that may always own or access an
// owner root. Security descriptors for new objects are prepared once.
class Identity final {
public:
    static Result<std::shared_ptr<const Identity>> current()
    {
        HANDLE raw_token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
            return failure(operatingError("Open process token", ::GetLastError()));
        }
        Handle token(raw_token);
        DWORD needed = 0;
        ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed);
        if (needed == 0) return failure(operatingError("Query token user", ::GetLastError()));
        std::vector<unsigned char> buffer(needed);
        if (!::GetTokenInformation(token.get(), TokenUser, buffer.data(), needed, &needed)) {
            return failure(operatingError("Query token user", ::GetLastError()));
        }
        const PSID user_sid = reinterpret_cast<const TOKEN_USER *>(buffer.data())->User.Sid;
        if (!::IsValidSid(user_sid)) return failure({ErrorCode::io_failure, "Process token has no valid user SID"});

        std::shared_ptr<Identity> identity(new Identity());
        identity->user_.resize(::GetLengthSid(user_sid));
        if (!::CopySid(static_cast<DWORD>(identity->user_.size()), identity->user_.data(), user_sid)) {
            return failure(operatingError("Copy user SID", ::GetLastError()));
        }
        for (WELL_KNOWN_SID_TYPE type : {WinLocalSystemSid, WinBuiltinAdministratorsSid, WinCreatorOwnerRightsSid}) {
            Result<SidBytes> known = wellKnownSid(type);
            if (!known) return failure(known.error());
            identity->trusted_.push_back(std::move(known.value()));
        }

        LPWSTR text = nullptr;
        if (!::ConvertSidToStringSidW(user_sid, &text)) {
            return failure(operatingError("Format user SID", ::GetLastError()));
        }
        const LocalMemory text_guard(text);
        const std::wstring user(text);
        // Protected DACLs: nothing is inherited from %LOCALAPPDATA% or any
        // other ancestor; directories pass the single ACE on to children.
        Result<bool> directories = identity->directory_.build(L"O:" + user + L"D:P(A;OICI;FA;;;" + user + L")");
        if (!directories) return failure(directories.error());
        Result<bool> files = identity->file_.build(L"O:" + user + L"D:P(A;;FA;;;" + user + L")");
        if (!files) return failure(files.error());
        return Result<std::shared_ptr<const Identity>>::success(std::move(identity));
    }

    [[nodiscard]] SECURITY_ATTRIBUTES *directoryAttributes() const { return this->directory_.attributes(); }
    [[nodiscard]] SECURITY_ATTRIBUTES *fileAttributes() const { return this->file_.attributes(); }

    [[nodiscard]] bool trusted(PSID principal) const
    {
        if (::EqualSid(principal, sid(this->user_))) return true;
        return std::any_of(this->trusted_.begin(), this->trusted_.end(),
                           [principal](const SidBytes &known) { return ::EqualSid(principal, sid(known)) != FALSE; });
    }

private:
    class Descriptor final {
    public:
        Descriptor() = default;
        ~Descriptor() { if (this->descriptor_ != nullptr) ::LocalFree(this->descriptor_); }
        Descriptor(const Descriptor &) = delete;
        Descriptor &operator=(const Descriptor &) = delete;

        Result<bool> build(const std::wstring &sddl)
        {
            if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(), SDDL_REVISION_1, &this->descriptor_, nullptr)) {
                return Result<bool>::failure(operatingError("Build private security descriptor", ::GetLastError()));
            }
            this->attributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
            this->attributes_.lpSecurityDescriptor = this->descriptor_;
            this->attributes_.bInheritHandle = FALSE;
            return Result<bool>::success(true);
        }

        [[nodiscard]] SECURITY_ATTRIBUTES *attributes() const
        {
            return const_cast<SECURITY_ATTRIBUTES *>(&this->attributes_);
        }

    private:
        PSECURITY_DESCRIPTOR descriptor_ = nullptr;
        SECURITY_ATTRIBUTES attributes_{};
    };

    Identity() = default;

    static Result<std::shared_ptr<const Identity>> failure(Error error)
    {
        return Result<std::shared_ptr<const Identity>>::failure(std::move(error));
    }

    SidBytes user_;
    std::vector<SidBytes> trusted_;
    Descriptor directory_;
    Descriptor file_;
};

constexpr DWORD write_rights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES
                               | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER
                               | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;

// The Windows counterpart of the POSIX owner/mode check. The handle must
// have READ_CONTROL. Returns false for a policy violation; true when safe.
Result<bool> acceptable(HANDLE object, BackendMode mode, const Identity &identity)
{
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    const DWORD queried = ::GetSecurityInfo(object, SE_FILE_OBJECT,
                                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                            &owner, nullptr, &dacl, nullptr, &raw_descriptor);
    if (queried != ERROR_SUCCESS) return Result<bool>::failure(operatingError("Inspect access control", queried));
    const LocalMemory descriptor_guard(raw_descriptor);
    if (owner == nullptr || !identity.trusted(owner)) return Result<bool>::success(false);
    // A NULL DACL grants everyone full access.
    if (dacl == nullptr) return Result<bool>::success(false);
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void *entry = nullptr;
        if (!::GetAce(dacl, index, &entry)) return Result<bool>::failure(operatingError("Read access control entry", ::GetLastError()));
        const ACE_HEADER *header = static_cast<const ACE_HEADER *>(entry);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0) continue; // Applies to children only.
        switch (header->AceType) {
        case ACCESS_DENIED_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
            continue;
        case ACCESS_ALLOWED_ACE_TYPE: {
            ACCESS_ALLOWED_ACE *allowed = static_cast<ACCESS_ALLOWED_ACE *>(entry);
            const PSID principal = reinterpret_cast<PSID>(&allowed->SidStart);
            if (identity.trusted(principal)) continue;
            const DWORD forbidden = mode == BackendMode::read_write ? ~DWORD{0} : write_rights;
            if ((allowed->Mask & forbidden) != 0) return Result<bool>::success(false);
            continue;
        }
        default:
            // Conditional/object grants cannot be evaluated here: refuse.
            return Result<bool>::success(false);
        }
    }
    return Result<bool>::success(true);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool usableSegment(const std::wstring &segment)
{
    if (segment.empty() || segment == L"." || segment == L"..") return false;
    // ':' would address an alternate data stream; trailing dots and spaces
    // name different files in "\\?\" form than in Explorer.
    if (segment.find_first_of(L":*?\"<>|") != std::wstring::npos) return false;
    if (std::any_of(segment.begin(), segment.end(), [](wchar_t character) { return character < 0x20; })) return false;
    return segment.back() != L'.' && segment.back() != L' ';
}

struct OwnerPath {
    std::wstring root;                  // "\\?\C:\" or "\\?\UNC\server\share\"
    std::vector<std::wstring> segments; // Below the root; the last one is the owner.

    [[nodiscard]] std::wstring prefix(std::size_t count) const
    {
        std::wstring path = this->root;
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) path += L'\\';
            path += this->segments[index];
        }
        return path;
    }
};

Result<OwnerPath> splitOwnerPath(const std::filesystem::path &input)
{
    std::wstring text = input.wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    const auto unsafe = []() {
        return Result<OwnerPath>::failure({ErrorCode::unsafe_path, "Configuration root must be a local drive or UNC path"});
    };
    const auto starts = [&text](std::wstring_view head) { return std::wstring_view(text).substr(0, head.size()) == head; };
    OwnerPath path;
    std::wstring rest;
    const auto unc = [&](std::size_t offset) {
        const std::size_t server_end = text.find(L'\\', offset);
        if (server_end == std::wstring::npos || server_end == offset) return false;
        const std::size_t share_end = text.find(L'\\', server_end + 1);
        const std::size_t share_stop = share_end == std::wstring::npos ? text.size() : share_end;
        if (share_stop == server_end + 1) return false;
        path.root = L"\\\\?\\UNC\\" + text.substr(offset, share_stop - offset) + L"\\";
        rest = share_end == std::wstring::npos ? std::wstring() : text.substr(share_end + 1);
        return true;
    };
    const auto drive = [&](std::size_t offset) {
        if (text.size() < offset + 3 || text[offset + 1] != L':' || text[offset + 2] != L'\\') return false;
        const wchar_t letter = text[offset];
        if (!((letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z'))) return false;
        path.root = L"\\\\?\\" + text.substr(offset, 3);
        rest = text.substr(offset + 3);
        return true;
    };
    if (starts(L"\\\\?\\UNC\\")) {
        if (!unc(8)) return unsafe();
    } else if (starts(L"\\\\?\\") || starts(L"\\\\.\\")) {
        if (!drive(4)) return unsafe(); // Device paths other than drives are refused.
    } else if (starts(L"\\\\")) {
        if (!unc(2)) return unsafe();
    } else if (!drive(0)) {
        return unsafe();
    }
    std::size_t position = 0;
    while (position <= rest.size()) {
        const std::size_t next = rest.find(L'\\', position);
        const std::size_t stop = next == std::wstring::npos ? rest.size() : next;
        std::wstring segment = rest.substr(position, stop - position);
        if (!segment.empty()) {
            if (!usableSegment(segment)) {
                return Result<OwnerPath>::failure({ErrorCode::unsafe_path, "Invalid configuration root component"});
            }
            path.segments.push_back(std::move(segment));
        }
        if (next == std::wstring::npos) break;
        position = next + 1;
    }
    if (path.segments.empty()) {
        return Result<OwnerPath>::failure({ErrorCode::unsafe_path, "A drive or share root cannot be an owner directory"});
    }
    return Result<OwnerPath>::success(std::move(path));
}

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

constexpr DWORD directory_access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
// Others may read and add entries; nobody may delete or rename the
// directory while it is held.
constexpr DWORD directory_share = FILE_SHARE_READ | FILE_SHARE_WRITE;

struct HeldDirectory {
    Handle handle;
    std::wstring path;
};

// Opens (read_write: creates first) a directory at or below the owner
// boundary. Missing directories are reported as not_found so read_only
// callers can treat a missing collection as empty.
Result<HeldDirectory> directoryAt(const std::wstring &path, BackendMode mode, const Identity &identity)
{
    if (mode == BackendMode::read_write && !::CreateDirectoryW(path.c_str(), identity.directoryAttributes())) {
        const DWORD failure = ::GetLastError();
        if (failure != ERROR_ALREADY_EXISTS) {
            return Result<HeldDirectory>::failure(operatingError("Create configuration directory", failure));
        }
    }
    Handle directory(::CreateFileW(path.c_str(), directory_access, directory_share, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directory.valid()) {
        const DWORD failure = ::GetLastError();
        if (missing(failure)) {
            return Result<HeldDirectory>::failure({ErrorCode::not_found, "Configuration directory does not exist"});
        }
        return Result<HeldDirectory>::failure(operatingError("Open configuration directory", failure));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(directory.get(), &information)) {
        return Result<HeldDirectory>::failure(operatingError("Inspect configuration directory", ::GetLastError()));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Result<HeldDirectory>::failure({ErrorCode::unsafe_path,
            "Configuration owner or collection directory is a reparse point or not a directory"});
    }
    Result<bool> safe = acceptable(directory.get(), mode, identity);
    if (!safe) return Result<HeldDirectory>::failure(safe.error());
    if (!safe.value()) {
        return Result<HeldDirectory>::failure({ErrorCode::permission_denied,
            mode == BackendMode::read_write
                ? "Configuration directory must be owned by this user and grant access to nobody else"
                : "Read-only configuration directory must have a trusted owner and grant write access to nobody else"});
    }
    return Result<HeldDirectory>::success(HeldDirectory{std::move(directory), path});
}

Result<HeldDirectory> ownerDirectory(const OwnerPath &path, BackendMode mode, const Identity &identity)
{
    // Ancestors (e.g. a redirected or junctioned profile folder) are part of
    // the user's layout: followed and created, not permission-checked. The
    // owner directory is the security boundary.
    for (std::size_t count = 1; count < path.segments.size(); ++count) {
        const std::wstring ancestor = path.prefix(count);
        const DWORD attributes = ::GetFileAttributesW(ancestor.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return Result<HeldDirectory>::failure({ErrorCode::unsafe_path, "Configuration ancestor is not a directory"});
            }
            continue;
        }
        const DWORD failure = ::GetLastError();
        if (!missing(failure)) {
            return Result<HeldDirectory>::failure(operatingError("Inspect configuration ancestor directory", failure));
        }
        if (mode == BackendMode::read_only) {
            return Result<HeldDirectory>::failure({ErrorCode::not_found, "Configuration root does not exist"});
        }
        if (!::CreateDirectoryW(ancestor.c_str(), identity.directoryAttributes())
            && ::GetLastError() != ERROR_ALREADY_EXISTS) {
            return Result<HeldDirectory>::failure(
                operatingError("Create configuration ancestor directory", ::GetLastError()));
        }
    }
    Result<HeldDirectory> owner = directoryAt(path.prefix(path.segments.size()), mode, identity);
    if (!owner && owner.error().code == ErrorCode::not_found) {
        return Result<HeldDirectory>::failure({ErrorCode::not_found, "Configuration root does not exist"});
    }
    return owner;
}

// Enumerates the directory behind a held handle (not by path), the
// counterpart of fdopendir(3).
template <typename Visit>
Result<bool> forEachEntry(HANDLE directory, Visit &&visit)
{
    alignas(8) unsigned char buffer[64 * 1024];
    FILE_INFO_BY_HANDLE_CLASS request = FileFullDirectoryRestartInfo;
    while (true) {
        if (!::GetFileInformationByHandleEx(directory, request, buffer, sizeof(buffer))) {
            const DWORD failure = ::GetLastError();
            if (failure == ERROR_NO_MORE_FILES) return Result<bool>::success(true);
            return Result<bool>::failure(operatingError("Enumerate collection", failure));
        }
        request = FileFullDirectoryInfo;
        std::size_t offset = 0;
        while (true) {
            const FILE_FULL_DIR_INFO *entry = reinterpret_cast<const FILE_FULL_DIR_INFO *>(buffer + offset);
            const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
            Result<bool> visited = visit(name, entry->FileAttributes);
            if (!visited) return visited;
            if (entry->NextEntryOffset == 0) break;
            offset += entry->NextEntryOffset;
        }
    }
}

// Flushes directory metadata (an erase, a recovery). Filesystems without
// directory flush support report that as success.
Result<bool> flushDirectory(const std::wstring &path)
{
    Handle directory(::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                   nullptr));
    if (!directory.valid()) return Result<bool>::failure(operatingError("Open collection for flush", ::GetLastError()));
    if (!::FlushFileBuffers(directory.get())) {
        const DWORD failure = ::GetLastError();
        if (failure == ERROR_INVALID_FUNCTION || failure == ERROR_NOT_SUPPORTED) return Result<bool>::success(true);
        return Result<bool>::failure(operatingError("Sync collection directory", failure));
    }
    return Result<bool>::success(true);
}

bool temporaryName(std::wstring_view name)
{
    constexpr std::wstring_view prefix = L".tmp-";
    if (name.substr(0, prefix.size()) != prefix) return false;
    const std::wstring_view rest = name.substr(prefix.size());
    if (std::any_of(rest.begin(), rest.end(), [](wchar_t character) { return character > 0x7f; })) return false;
    return ObjectId::parse(std::string(rest.begin(), rest.end())).hasValue();
}

Result<bool> removeOrphanTemps(const HeldDirectory &collection, const Identity &identity)
{
    std::vector<std::wstring> orphans;
    Result<bool> listed = forEachEntry(collection.handle.get(), [&orphans](std::wstring_view name, DWORD) {
        if (temporaryName(name)) orphans.emplace_back(name);
        return Result<bool>::success(true);
    });
    if (!listed) return listed;
    for (const std::wstring &name : orphans) {
        const std::wstring path = collection.path + L"\\" + name;
        Handle file(::CreateFileW(path.c_str(), DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!file.valid()) {
            const DWORD failure = ::GetLastError();
            if (missing(failure)) continue;
            return Result<bool>::failure(operatingError("Open orphan temporary file", failure));
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!::GetFileInformationByHandle(file.get(), &information)) {
            return Result<bool>::failure(operatingError("Inspect orphan temporary file", ::GetLastError()));
        }
        Result<bool> safe = acceptable(file.get(), BackendMode::read_write, identity);
        if (!safe) return safe;
        if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
            || !safe.value()) {
            return Result<bool>::failure({ErrorCode::unsafe_path, "Unsafe temporary entry in configuration collection"});
        }
        // Deleting through the verified handle leaves no window to swap it.
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (!::SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
            return Result<bool>::failure(operatingError("Remove orphan temporary file", ::GetLastError()));
        }
    }
    if (orphans.empty()) return Result<bool>::success(false);
    Result<bool> flushed = flushDirectory(collection.path);
    if (!flushed) return flushed;
    return Result<bool>::success(true);
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

Result<std::string> readFile(const std::wstring &path, BackendMode mode, const Identity &identity)
{
    Handle file;
    for (int attempt = 0;; ++attempt) {
        // FILE_SHARE_DELETE lets a writer replace the entry while it is read.
        file = Handle(::CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.valid()) break;
        const DWORD failure = ::GetLastError();
        if (missing(failure)) return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            return Result<std::string>::failure({ErrorCode::unsafe_path, "Object file is a directory or reparse point"});
        }
        if (!transient(failure) || attempt >= transient_retries) {
            return Result<std::string>::failure(operatingError("Open object", failure));
        }
        std::this_thread::sleep_for(transient_delay);
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(file.get(), &information)) {
        return Result<std::string>::failure(operatingError("Inspect object", ::GetLastError()));
    }
    Result<bool> safe = acceptable(file.get(), mode, identity);
    if (!safe) return Result<std::string>::failure(safe.error());
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
        || ::GetFileType(file.get()) != FILE_TYPE_DISK || !safe.value()) {
        return Result<std::string>::failure({ErrorCode::unsafe_path,
            mode == BackendMode::read_write
                ? "Object file must be a regular file owned by this user and private to it"
                : "Read-only object file must be regular, have a trusted owner and not be writable by others"});
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) return Result<std::string>::failure(operatingError("Inspect object", ::GetLastError()));
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_record_bytes) {
        return Result<std::string>::failure({ErrorCode::object_too_large, "Stored object exceeds size limit"});
    }
    std::string text;
    char buffer[8192];
    while (true) {
        DWORD count = 0;
        if (!::ReadFile(file.get(), buffer, sizeof(buffer), &count, nullptr)) {
            return Result<std::string>::failure(operatingError("Read object", ::GetLastError()));
        }
        if (count == 0) break;
        if (text.size() + count > max_record_bytes) {
            return Result<std::string>::failure({ErrorCode::object_too_large, "Stored object exceeds size limit"});
        }
        text.append(buffer, count);
    }
    return Result<std::string>::success(std::move(text));
}

Result<bool> writeFileFully(HANDLE file, std::string_view contents)
{
    std::size_t consumed = 0;
    while (consumed < contents.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size() - consumed, 1U << 20));
        DWORD count = 0;
        if (!::WriteFile(file, contents.data() + consumed, chunk, &count, nullptr) || count == 0) {
            return Result<bool>::failure(operatingError("Write object", ::GetLastError()));
        }
        consumed += count;
    }
    if (!::FlushFileBuffers(file)) return Result<bool>::failure(operatingError("Sync object", ::GetLastError()));
    return Result<bool>::success(true);
}

Result<bool> storeRecord(const std::wstring &directory, const ObjectId &id, std::string_view encoded,
                         bool create_only, const Identity &identity)
{
    Result<ObjectId> temp_id = ObjectId::generate();
    if (!temp_id) return Result<bool>::failure(temp_id.error());
    const std::wstring temporary = directory + L"\\.tmp-" + widen(temp_id.value().value());
    const std::wstring target = directory + L"\\" + widen(id.value()) + L".json";
    {
        Handle file(::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, identity.fileAttributes(), CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid()) return Result<bool>::failure(operatingError("Create temporary object", ::GetLastError()));
        Result<bool> written = writeFileFully(file.get(), encoded);
        if (!written) {
            file = Handle();
            ::DeleteFileW(temporary.c_str());
            return written;
        }
    } // Contents are flushed; the handle is closed before publication.
    // Without REPLACE_EXISTING the rename fails if the target exists, which
    // makes create_only atomic without a hard link.
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (create_only ? 0 : MOVEFILE_REPLACE_EXISTING);
    for (int attempt = 0;; ++attempt) {
        if (::MoveFileExW(temporary.c_str(), target.c_str(), flags)) return Result<bool>::success(true);
        const DWORD failure = ::GetLastError();
        if (failure == ERROR_ALREADY_EXISTS || failure == ERROR_FILE_EXISTS) {
            ::DeleteFileW(temporary.c_str());
            if (create_only) return Result<bool>::failure({ErrorCode::conflict, "Object ID already exists"});
            return Result<bool>::failure(operatingError("Install object", failure));
        }
        if (!transient(failure) || attempt >= transient_retries) {
            ::DeleteFileW(temporary.c_str());
            return Result<bool>::failure(operatingError("Install object", failure));
        }
        std::this_thread::sleep_for(transient_delay);
    }
}

Result<bool> removeFile(const std::wstring &directory, const ObjectId &id)
{
    const std::wstring target = directory + L"\\" + widen(id.value()) + L".json";
    for (int attempt = 0;; ++attempt) {
        // DeleteFileW removes a reparse point itself, never its target.
        if (::DeleteFileW(target.c_str())) break;
        const DWORD failure = ::GetLastError();
        if (missing(failure)) return Result<bool>::failure({ErrorCode::not_found, "Object does not exist"});
        if (!transient(failure) || attempt >= transient_retries) {
            return Result<bool>::failure(operatingError("Delete object", failure));
        }
        std::this_thread::sleep_for(transient_delay);
    }
    return flushDirectory(directory);
}

Result<bool> readOnlyWrite()
{
    return Result<bool>::failure({ErrorCode::permission_denied, "Configuration store is opened read-only"});
}

// One collection inside a held owner session. present_ is false for a
// collection that does not exist in read-only mode: it then reads as empty.
class WindowsCollection final : public CollectionSession {
public:
    WindowsCollection(HeldDirectory directory, bool present, BackendMode mode, std::shared_ptr<const Identity> identity)
        : directory_(std::move(directory)), present_(present), mode_(mode), identity_(std::move(identity)) {}

    [[nodiscard]] Result<std::vector<std::string>> entryNames() const override
    {
        std::vector<std::string> names;
        if (!this->present_) return Result<std::vector<std::string>>::success(std::move(names));
        constexpr std::wstring_view suffix = L".json";
        Result<bool> listed = forEachEntry(this->directory_.handle.get(), [&names, suffix](std::wstring_view name, DWORD) {
            // ".", ".." and ".tmp-*" never end in ".json".
            if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
                names.push_back(narrow(name.data(), name.size()));
            }
            return Result<bool>::success(true);
        });
        if (!listed) return Result<std::vector<std::string>>::failure(listed.error());
        return Result<std::vector<std::string>>::success(std::move(names));
    }

    [[nodiscard]] Result<std::string> readEntry(const ObjectId &id) const override
    {
        if (!this->present_) return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        return readFile(this->directory_.path + L"\\" + widen(id.value()) + L".json", this->mode_, *this->identity_);
    }

    [[nodiscard]] Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) override
    {
        if (this->mode_ == BackendMode::read_only) return readOnlyWrite();
        return storeRecord(this->directory_.path, id, bytes, create_only, *this->identity_);
    }

    [[nodiscard]] Result<bool> removeEntry(const ObjectId &id) override
    {
        if (this->mode_ == BackendMode::read_only) return readOnlyWrite();
        return removeFile(this->directory_.path, id);
    }

private:
    HeldDirectory directory_;
    bool present_;
    BackendMode mode_;
    std::shared_ptr<const Identity> identity_;
};

// Holds LockFileEx on the owner's .lock file; released on destruction.
class OwnerLock final {
public:
    OwnerLock() = default;
    explicit OwnerLock(Handle file) : file_(std::move(file)) {}
    ~OwnerLock() { this->release(); }
    OwnerLock(OwnerLock &&) noexcept = default;
    OwnerLock &operator=(OwnerLock &&other) noexcept
    {
        if (this != &other) {
            this->release();
            this->file_ = std::move(other.file_);
        }
        return *this;
    }
    OwnerLock(const OwnerLock &) = delete;
    OwnerLock &operator=(const OwnerLock &) = delete;

private:
    void release() noexcept
    {
        if (this->file_.valid()) {
            OVERLAPPED region{};
            ::UnlockFileEx(this->file_.get(), 0, 1, 0, &region);
            this->file_ = Handle();
        }
    }

    Handle file_;
};

class WindowsOwnerSession final : public OwnerSessionImpl {
public:
    WindowsOwnerSession(HeldDirectory owner, OwnerLock lock, BackendMode mode, std::shared_ptr<const Identity> identity)
        : owner_(std::move(owner)), lock_(std::move(lock)), mode_(mode), identity_(std::move(identity)) {}

    [[nodiscard]] Result<std::unique_ptr<CollectionSession>> collection(const std::string &name, bool recover) override
    {
        Result<HeldDirectory> directory = directoryAt(this->owner_.path + L"\\" + widen(name), this->mode_, *this->identity_);
        if (!directory) {
            if (this->mode_ == BackendMode::read_only && directory.error().code == ErrorCode::not_found) {
                return Result<std::unique_ptr<CollectionSession>>::success(
                    std::make_unique<WindowsCollection>(HeldDirectory{}, false, this->mode_, this->identity_));
            }
            return Result<std::unique_ptr<CollectionSession>>::failure(directory.error());
        }
        if (recover && this->mode_ == BackendMode::read_write) {
            Result<bool> recovered = removeOrphanTemps(directory.value(), *this->identity_);
            if (!recovered) return Result<std::unique_ptr<CollectionSession>>::failure(recovered.error());
        }
        return Result<std::unique_ptr<CollectionSession>>::success(
            std::make_unique<WindowsCollection>(std::move(directory.value()), true, this->mode_, this->identity_));
    }

private:
    HeldDirectory owner_;
    OwnerLock lock_; // Empty in read_only mode.
    BackendMode mode_;
    std::shared_ptr<const Identity> identity_;
};

Result<OwnerLock> acquireOwnerLock(const HeldDirectory &owner, std::chrono::milliseconds timeout, const Identity &identity)
{
    const std::wstring path = owner.path + L"\\.lock";
    // No FILE_SHARE_DELETE: the lock file cannot be removed or replaced
    // while anyone has it open.
    Handle file(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, identity.fileAttributes(), OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid()) return Result<OwnerLock>::failure(operatingError("Open owner lock", ::GetLastError()));
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(file.get(), &information)) {
        return Result<OwnerLock>::failure(operatingError("Inspect owner lock", ::GetLastError()));
    }
    Result<bool> safe = acceptable(file.get(), BackendMode::read_write, identity);
    if (!safe) return Result<OwnerLock>::failure(safe.error());
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 || !safe.value()) {
        return Result<OwnerLock>::failure({ErrorCode::unsafe_path, "Unsafe owner lock file"});
    }
    // Never block indefinitely: a hung second instance or a stalled network
    // share would otherwise freeze the caller's thread.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
    while (true) {
        OVERLAPPED region{};
        if (::LockFileEx(file.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region)) break;
        const DWORD failure = ::GetLastError();
        if (failure != ERROR_LOCK_VIOLATION && failure != ERROR_IO_PENDING) {
            return Result<OwnerLock>::failure(operatingError("Acquire owner lock", failure));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Result<OwnerLock>::failure(
                {ErrorCode::lock_timeout, "Configuration is locked by another process; retry later"});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return Result<OwnerLock>::success(OwnerLock(std::move(file)));
}

class WindowsBackend final : public OwnerBackend {
public:
    WindowsBackend(OwnerPath owner_path, BackendMode mode, std::chrono::milliseconds lock_timeout,
                   std::shared_ptr<const Identity> identity)
        : owner_path_(std::move(owner_path)), mode_(mode), lock_timeout_(lock_timeout), identity_(std::move(identity)) {}

    [[nodiscard]] Result<std::unique_ptr<OwnerSessionImpl>> lock() const override
    {
        // Re-opened and re-verified for every session, so a directory
        // swapped between sessions is detected rather than trusted.
        Result<HeldDirectory> owner = ownerDirectory(this->owner_path_, this->mode_, *this->identity_);
        if (!owner) return Result<std::unique_ptr<OwnerSessionImpl>>::failure(owner.error());
        OwnerLock held;
        if (this->mode_ == BackendMode::read_write) {
            Result<OwnerLock> locked = acquireOwnerLock(owner.value(), this->lock_timeout_, *this->identity_);
            if (!locked) return Result<std::unique_ptr<OwnerSessionImpl>>::failure(locked.error());
            held = std::move(locked.value());
        }
        return Result<std::unique_ptr<OwnerSessionImpl>>::success(std::make_unique<WindowsOwnerSession>(
            std::move(owner.value()), std::move(held), this->mode_, this->identity_));
    }

    [[nodiscard]] BackendMode mode() const noexcept override { return this->mode_; }

private:
    OwnerPath owner_path_;
    BackendMode mode_;
    std::chrono::milliseconds lock_timeout_;
    std::shared_ptr<const Identity> identity_;
};
} // namespace

Result<std::shared_ptr<const OwnerBackend>> openNativeBackend(
    const std::filesystem::path &owner_root, BackendMode mode, std::chrono::milliseconds lock_timeout)
{
    Result<OwnerPath> path = splitOwnerPath(owner_root);
    if (!path) return Result<std::shared_ptr<const OwnerBackend>>::failure(path.error());
    Result<std::shared_ptr<const Identity>> identity = Identity::current();
    if (!identity) return Result<std::shared_ptr<const OwnerBackend>>::failure(identity.error());
    return Result<std::shared_ptr<const OwnerBackend>>::success(
        std::make_shared<const WindowsBackend>(std::move(path.value()), mode, lock_timeout, std::move(identity.value())));
}

} // namespace aowis::config::detail
