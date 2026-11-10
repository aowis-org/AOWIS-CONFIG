#include <aowis/config/wireguard_profiles.h>

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <iostream>
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
using aowis::config::ObjectId;
using aowis::config::OwnerId;
using aowis::config::Result;
using aowis::config::WireGuardIssueCode;
using aowis::config::WireGuardProfile;
using aowis::config::WireGuardProfileInput;
using aowis::config::WireGuardProfileListing;
using aowis::config::WireGuardProfiles;
using aowis::config::WireGuardReferenceCount;
using aowis::config::WireGuardValidation;

void require(bool valid, const char *message)
{
    if (!valid) throw std::runtime_error(message);
}

bool containsIssue(const WireGuardValidation &validation, WireGuardIssueCode code)
{
    for (const aowis::config::WireGuardIssue &issue : validation.issues) {
        if (issue.code == code) return true;
    }
    return false;
}

// Artificial secret must never occur in diagnostics and other metadata.
constexpr std::string_view secret = "k5yQ1m2b3c4d5e6f7g8h9i0j1k2l3m4n5o6p7q8r9s0=";
constexpr std::string_view sample =
    "# AOWIS VPN — original comment\r\n"
    "[Interface]\r\n"
    "PrivateKey = k5yQ1m2b3c4d5e6f7g8h9i0j1k2l3m4n5o6p7q8r9s0=\r\n"
    "Address = 10.77.0.2/32\r\n"
    "DNS = 10.77.0.1\r\n"
    "\r\n"
    "[Peer]\r\n"
    "PublicKey = V0hERXJ4MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg=\r\n"
    "Endpoint = vpn.example.invalid:51820\r\n"
    "AllowedIPs = 10.77.0.0/24\r\n"
    "PersistentKeepalive = 25\r\n";

void validation()
{
    const WireGuardValidation valid = aowis::config::validateWireGuardConfiguration(sample);
    require(valid.valid(), "valid config not accepted");
    require(containsIssue(valid, WireGuardIssueCode::requires_wg_quick), "wg-quick warning absent");
    for (const aowis::config::WireGuardIssue &issue : valid.issues) {
        require(issue.message.find(secret) == std::string::npos, "diagnostic contains secret");
    }
    const WireGuardValidation multiple = aowis::config::validateWireGuardConfiguration(
        "[Interface]\nPrivateKey = abc\n[Peer]\nPublicKey = first\n[Peer]\nPublicKey = second\n");
    require(multiple.valid(), "multiple peers rejected");
    const WireGuardValidation hooks = aowis::config::validateWireGuardConfiguration(
        "[Interface]\nPrivateKey = ./keyfile\nPostUp = /usr/bin/sh /home/example/start.sh\nInclude = /tmp/x\n"
        "[Peer]\nPublicKey = key\nPresharedKey = @/home/example/psk\n");
    require(hooks.valid(), "unsupported directives must give warnings rather than silently changing content");
    require(containsIssue(hooks, WireGuardIssueCode::external_command), "external command warning missing");
    require(containsIssue(hooks, WireGuardIssueCode::external_reference), "external reference warning missing");
    require(containsIssue(hooks, WireGuardIssueCode::unknown_directive), "unknown directive warning missing");
    const WireGuardValidation invalid = aowis::config::validateWireGuardConfiguration(
        "PrivateKey = secret\n[Interface]\nAddress\n[Interface]\nNoValue = \n[Other]\nkey = value\n");
    require(!invalid.valid(), "invalid sections/lines accepted");
    require(containsIssue(invalid, WireGuardIssueCode::duplicate_interface), "duplicate interface missed");
    require(containsIssue(invalid, WireGuardIssueCode::directive_outside_section), "directive outside section missed");
    require(containsIssue(invalid, WireGuardIssueCode::invalid_line), "malformed directive missed");
    require(containsIssue(invalid, WireGuardIssueCode::empty_value), "empty directive missed");
    require(containsIssue(invalid, WireGuardIssueCode::unexpected_section), "unknown section missed");
    require(!aowis::config::validateWireGuardConfiguration("garbage").valid(), "garbage accepted");
    require(!aowis::config::validateWireGuardConfiguration("[Peer]\nPublicKey = foo\n").valid(), "missing interface accepted");
    require(!aowis::config::validateWireGuardConfiguration("[Interface]\nKey = \xff\n").valid(), "invalid UTF-8 accepted");
    require(!aowis::config::validateWireGuardConfiguration(std::string("[Interface]\nA\0B=X", 17)).valid(), "NUL accepted");
    std::string overflow = "[Interface]\nPrivateKey = abc\n";
    for (int index = 0; index < 700; ++index) overflow += "UnknownDirective = x\n";
    overflow += "[Illegal]\n";
    const WireGuardValidation truncated = aowis::config::validateWireGuardConfiguration(overflow);
    require(truncated.issues.size() == 512 && !truncated.valid(),
            "issue limit hid a later configuration error");
    require(!aowis::config::validateWireGuardConfiguration(std::string(2U * 1024U * 1024U + 1U, 'x')).valid(),
            "oversized configuration accepted");
    const std::string bom = "\xef\xbb\xbf[Interface]\r\nPrivateKey = abc\r\n";
    require(aowis::config::validateWireGuardConfiguration(bom).valid(), "BOM rejected");

    // '/' is a base64 character: a real key starting with it is not a path.
    const WireGuardValidation slash_key = aowis::config::validateWireGuardConfiguration(
        "[Interface]\nPrivateKey = /BCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq=\n"
        "[Peer]\nPublicKey = +BCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq=\n"
        "PresharedKey = /BCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq=\n");
    require(slash_key.valid() && slash_key.issues.empty(), "base64 key starting with '/' flagged as file path");
    require(containsIssue(aowis::config::validateWireGuardConfiguration(
                "[Interface]\nPrivateKey = /etc/wireguard/private.key\n"),
            WireGuardIssueCode::external_reference), "absolute key path no longer flagged");

    // wg(8)/wg-quick accept any case, blanks in headers and trailing comments.
    const WireGuardValidation relaxed = aowis::config::validateWireGuardConfiguration(
        "[interface]\nprivatekey = abc # note\naddress = 10.0.0.2/32\n"
        "[ PEER ] # office\npublickey = def\nendpoint = vpn.example.invalid:51820\n");
    require(relaxed.valid(), "case-insensitive WireGuard syntax rejected");
    require(!containsIssue(relaxed, WireGuardIssueCode::missing_private_key), "lowercase PrivateKey not recognized");
    require(!containsIssue(relaxed, WireGuardIssueCode::missing_peer_public_key), "lowercase PublicKey not recognized");
    require(!containsIssue(relaxed, WireGuardIssueCode::unknown_directive), "lowercase standard directive unknown");
    require(containsIssue(relaxed, WireGuardIssueCode::requires_wg_quick), "lowercase wg-quick directive not classified");
    require(containsIssue(aowis::config::validateWireGuardConfiguration(
                "[Interface]\nPrivateKey = abc\npostup = /bin/true\n"),
            WireGuardIssueCode::external_command), "lowercase hook not flagged");
    require(containsIssue(aowis::config::validateWireGuardConfiguration(
                "[Interface]\nPrivateKey = abc\nPrivateKey = # only a comment\n"),
            WireGuardIssueCode::empty_value), "comment-only value accepted");
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

ConfigRoot root(const std::filesystem::path &path)
{
    Result<OwnerId> owner = OwnerId::parse("gui");
    require(owner.hasValue(), "owner parse failed");
    Result<ConfigRoot> created = ConfigRoot::fromOwnerRoot(owner.value(), path);
    require(created.hasValue(), "root parse failed");
    return created.value();
}

WireGuardProfiles open(const std::filesystem::path &path)
{
    Result<WireGuardProfiles> profiles = WireGuardProfiles::open(root(path));
    require(profiles.hasValue(), "open failed");
    return profiles.value();
}

void requireMode(const std::filesystem::path &path, mode_t expected)
{
    struct stat state{};
    require(::lstat(path.c_str(), &state) == 0, "stat failed");
    require((state.st_mode & 0777) == expected, "insecure permissions");
}

void lifecycle()
{
    TemporaryDirectory directory;
    const std::filesystem::path one = directory.path() / "original" / "aowis" / "gui";
    WireGuardProfiles profiles = open(one);
    WireGuardProfileInput input{"Office VPN \xc3\xa9", "Private remote connection", std::string(sample)};
    Result<WireGuardProfile> created = profiles.importText(input);
    require(created.hasValue(), "import failed");
    const ObjectId id = created.value().summary.info.id;
    const std::string version_one = created.value().summary.info.revision;
    require(created.value().configuration == sample, "import changed formatting");
    require(!created.value().validation.issues.empty(), "import hid wg-quick portability warnings");
    require(created.value().summary.name == input.name, "import changed name");
    Result<WireGuardProfileListing> summaries = profiles.list();
    require(summaries && summaries.value().profiles.size() == 1 && summaries.value().unreadable.empty(), "list failed");
    require(summaries.value().profiles.front().info.id.value() == id.value(), "summary lost id");
    require(summaries.value().profiles.front().name == input.name, "summary lost name");
    requireMode(one, 0700);
    requireMode(one / "wireguard", 0700);
    const std::filesystem::path record = one / "wireguard" / (id.value() + ".json");
    requireMode(record, 0600);
    {
        std::ifstream file(record, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        require(bytes.find(secret) != std::string::npos, "imported secret not present in managed record");
        require(bytes.find("/home/samuel") == std::string::npos, "external source path persisted");
    }
    require(profiles.exportText(id).value() == sample, "export changed formatting");
    WireGuardProfiles reopened = open(one);
    Result<WireGuardProfile> loaded = reopened.read(id);
    require(loaded && loaded.value().configuration == sample, "reopened profile is not identical");
    require(loaded.value().summary.info.revision == version_one, "reopen changed revision");
    input.name = "Office VPN renamed";
    input.description = "Changed \"description\" with \\ backslash and \n newline";
    input.configuration += "# final comment\n";
    Result<WireGuardProfile> updated = reopened.update(id, version_one, input);
    require(updated.hasValue(), "edit/rename failed");
    require(updated.value().summary.info.id.value() == id.value(), "rename changed id");
    require(updated.value().summary.info.revision != version_one, "edit did not update revision");
    require(updated.value().configuration == input.configuration, "edit changed config text");
    Result<WireGuardProfile> stale = profiles.update(id, version_one, input);
    require(!stale && stale.error().code == ErrorCode::conflict, "stale update accepted");
    const std::filesystem::path copy = directory.path() / "moved" / "aowis" / "gui";
    std::filesystem::create_directories(copy.parent_path());
    std::filesystem::copy(one, copy, std::filesystem::copy_options::recursive);
    WireGuardProfiles relocated = open(copy);
    Result<WireGuardProfile> relocated_profile = relocated.read(id);
    require(relocated_profile && relocated_profile.value().configuration == input.configuration,
            "relocation broke config");
    require(relocated_profile.value().summary.info.id.value() == id.value(), "relocation changed ID");
    const std::string current = updated.value().summary.info.revision;
    Result<bool> missing_resolver = reopened.erase(id, current, WireGuardReferenceCount{});
    require(!missing_resolver && missing_resolver.error().code == ErrorCode::reference_unavailable,
            "missing resolver allowed deletion");
    Result<bool> resolver_failure = reopened.erase(id, current, [](const ObjectId &) {
        return Result<std::size_t>::failure({ErrorCode::io_failure, "Catalogue unavailable"});
    });
    require(!resolver_failure && resolver_failure.error().code == ErrorCode::io_failure,
            "reference lookup failure allowed deletion");
    Result<bool> used = reopened.erase(id, current, [](const ObjectId &) {
        return Result<std::size_t>::success(3);
    });
    require(!used && used.error().code == ErrorCode::profile_in_use, "referenced profile deleted");
    require(reopened.read(id).hasValue(), "guard removed profile");
    Result<bool> stale_delete = reopened.erase(id, version_one, [](const ObjectId &) {
        return Result<std::size_t>::success(0);
    });
    require(!stale_delete && stale_delete.error().code == ErrorCode::conflict,
            "stale delete accepted");
    Result<bool> deleted = reopened.erase(id, current, [](const ObjectId &) {
        return Result<std::size_t>::success(0);
    });
    require(deleted && deleted.value(), "unreferenced profile deletion failed");
    require(!reopened.read(id) && reopened.list().value().profiles.empty(), "deleted profile remains");
    require(relocated.read(id).hasValue(), "deletion in one root affected copied root");
}

void errorsAndSchema()
{
    TemporaryDirectory directory;
    const std::filesystem::path location = directory.path() / "gui";
    WireGuardProfiles profiles = open(location);
    WireGuardProfileInput invalid{"", "", std::string(sample)};
    Result<WireGuardProfile> rejected = profiles.importText(invalid);
    require(!rejected && rejected.error().code == ErrorCode::invalid_wireguard_profile, "empty name accepted");
    invalid.name = "VPN";
    invalid.configuration = "not WireGuard";
    rejected = profiles.importText(invalid);
    require(!rejected && rejected.error().code == ErrorCode::invalid_wireguard_profile, "invalid config accepted");
    invalid.configuration = std::string(sample);
    invalid.description = std::string("broken \xff", 8);
    require(!profiles.importText(invalid), "invalid metadata UTF-8 accepted");
    invalid.description.clear();
    invalid.name = "\"quoted\" name";
    Result<WireGuardProfile> created = profiles.importText(invalid);
    require(created.hasValue(), "quoted name rejected");
    const ObjectId id = created.value().summary.info.id;
    require(profiles.read(id).value().summary.name == invalid.name, "quoted name changed");
    const std::filesystem::path filename = location / "wireguard" / (id.value() + ".json");
    {
        // A stored syntactically valid but incorrect typed payload must not be
        // misinterpreted as a valid profile. Phase 2 owns envelope validation.
        aowis::config::ObjectCollection generic = aowis::config::ObjectCollection::open(root(location), "wireguard").value();
        require(generic.update(id, "{\"type\":\"not-wireguard\",\"profile_version\":1,\"name\":\"X\",\"description\":\"\",\"configuration\":\"[Interface]\\n\"}",
                               created.value().summary.info.revision).hasValue(), "cannot seed schema test");
    }
    Result<WireGuardProfile> wrong_type = profiles.read(id);
    require(!wrong_type && wrong_type.error().code == ErrorCode::corrupt_object, "wrong typed schema accepted");
    Result<WireGuardProfileListing> listed = profiles.list();
    require(listed && listed.value().profiles.empty() && listed.value().unreadable.size() == 1
            && listed.value().unreadable[0].id.has_value() && listed.value().unreadable[0].id->value() == id.value()
            && listed.value().unreadable[0].error.code == ErrorCode::corrupt_object,
            "catalogue silently dropped invalid profile");
    requireMode(filename, 0600);

    // Unreadable records are removable, with the same reference guard as erase().
    Result<bool> unguarded = profiles.eraseUnreadable(id, WireGuardReferenceCount{});
    require(!unguarded && unguarded.error().code == ErrorCode::reference_unavailable, "unguarded unreadable delete");
    Result<bool> referenced = profiles.eraseUnreadable(id, [](const ObjectId &) {
        return Result<std::size_t>::success(1);
    });
    require(!referenced && referenced.error().code == ErrorCode::profile_in_use, "referenced unreadable deleted");
    const WireGuardReferenceCount unused = [](const ObjectId &) { return Result<std::size_t>::success(0); };
    require(profiles.eraseUnreadable(id, unused).hasValue(), "wrong-type record not removable");
    require(!std::filesystem::exists(filename), "wrong-type record remains");

    // A corrupt envelope is removable too; a readable profile is not.
    Result<WireGuardProfile> broken = profiles.importText(invalid);
    Result<WireGuardProfile> healthy = profiles.importText(invalid);
    require(broken && healthy, "cannot seed corrupt envelope test");
    const std::filesystem::path broken_file = location / "wireguard" / (broken.value().summary.info.id.value() + ".json");
    {
        std::ofstream output(broken_file, std::ios::trunc);
        output << "{garbage";
    }
    listed = profiles.list();
    require(listed && listed.value().profiles.size() == 1 && listed.value().unreadable.size() == 1,
            "corrupt envelope hid healthy profile");
    Result<bool> readable = profiles.eraseUnreadable(healthy.value().summary.info.id, unused);
    require(!readable && readable.error().code == ErrorCode::conflict, "eraseUnreadable removed readable profile");
    require(profiles.eraseUnreadable(broken.value().summary.info.id, unused).hasValue(), "corrupt envelope not removable");
    require(profiles.list().value().unreadable.empty(), "removed corrupt envelope still listed");
}

void lenientRead()
{
    // A stored profile which the *current* validator rejects (e.g. after the
    // rules were tightened) stays readable and repairable.
    TemporaryDirectory directory;
    const std::filesystem::path location = directory.path() / "gui";
    WireGuardProfiles profiles = open(location);
    WireGuardProfileInput input{"VPN", "", std::string(sample)};
    Result<WireGuardProfile> created = profiles.importText(input);
    require(created.hasValue(), "cannot seed lenient read test");
    const ObjectId id = created.value().summary.info.id;
    {
        aowis::config::ObjectCollection generic = aowis::config::ObjectCollection::open(root(location), "wireguard").value();
        require(generic.update(id, "{\"type\":\"wireguard-profile\",\"profile_version\":1,\"name\":\"VPN\","
                                   "\"description\":\"\",\"configuration\":\"no longer valid\"}",
                               created.value().summary.info.revision).hasValue(), "cannot seed outdated profile");
    }
    Result<WireGuardProfile> loaded = profiles.read(id);
    require(loaded.hasValue(), "profile failing current validation became unreadable");
    require(loaded.value().validation.has_errors && !loaded.value().validation.issues.empty(),
            "validation errors of stored text not reported");
    Result<WireGuardProfileListing> listed = profiles.list();
    require(listed && listed.value().profiles.size() == 1 && listed.value().unreadable.empty(),
            "outdated profile missing from list");
    require(profiles.exportText(id).value() == "no longer valid", "outdated profile not exportable");
    Result<WireGuardProfile> repaired = profiles.update(id, loaded.value().summary.info.revision, input);
    require(repaired && repaired.value().validation.valid(), "outdated profile not repairable");
    input.configuration = "still invalid";
    require(!profiles.update(id, repaired.value().summary.info.revision, input), "invalid write accepted");
}
#endif
} // namespace

int main()
{
    try {
        validation();
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        lifecycle();
        errorsAndSchema();
        lenientRead();
#endif
        std::cout << "WireGuard profile persistence tests passed\n";
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << "WireGuard profile test failed: " << failure.what() << '\n';
        return 1;
    }
}
