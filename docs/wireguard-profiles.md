# Phase 3 — WireGuard profiles

Status: **implemented in AOWIS-CONFIG v0.3.0 for the native POSIX object backend**. See [Consumer integration](integration-guide.md) and [Operations](operations.md) for application responsibilities and sensitive-file handling.
The GUI binding and AOWIS-NETWORK runtime integration belong to Phase 4.

## Storage and profile format

Each owner independently owns its profiles under `<owner>/wireguard/`. A
profile is one `0600` JSON object named `<uuidv7>.json`, using the atomic
versioned envelope defined in [storage-api.md](storage-api.md). There is no
external source file path in the payload. The entire imported `.conf` text is
stored *inside* the JSON record as a losslessly escaped string:

```json
{
  "schema_version": 1,
  "id": "019a1234-0000-7000-8000-000000000001",
  "revision": "0123456789abcdef0123456789abcdef",
  "payload": {
    "type": "wireguard-profile",
    "profile_version": 1,
    "name": "Office VPN",
    "description": "VPN for internal infrastructure",
    "configuration": "[Interface]\nPrivateKey = ...\n[Peer]\nPublicKey = ...\n"
  }
}
```

The keys above are *placeholders*, not functioning tunnel credentials. A
profile's UUID is in the storage envelope (not duplicated in the payload).
`profile_version` is independent of the envelope's `schema_version`.

The text round-trip preserves LF/CRLF, UTF-8, comments, blank lines, tabs,
trailing whitespace, line ordering, and private keys. Import accepts text in
memory, **not an external file path**. The GUI may use its platform file picker
to read `.conf` bytes and then invoke `importText()`. The library does not
retain the source filename or need the original file to reopen the profile.

Copy the owner's complete configuration directory to relocate profiles;
UUIDs and content survive the move. The copied tree is secret-bearing.
Imported configs containing script hooks, includes or references to external
files can still have dependencies *outside the copied tree*: the validation
API reports warnings, but does not rewrite or execute those directives.
No automatic cross-owner synchronization or keychain encryption is implied.

## Public API

`#include <aowis/config/wireguard_profiles.h>`

```cpp
// root is the caller's verified ConfigRoot; see library-api.md.
aowis::config::Result<aowis::config::WireGuardProfiles> opened =
    aowis::config::WireGuardProfiles::open(root);

aowis::config::WireGuardProfileInput input{
    "Office VPN", "VPN for infrastructure", imported_conf_text
};
aowis::config::WireGuardValidation validation =
    aowis::config::validateWireGuardConfiguration(input.configuration);
if (!validation.valid()) {
    // Present validation.issues in the editor; no file is written.
    return;
}
aowis::config::Result<aowis::config::WireGuardProfile> created =
    opened.value().importText(input);
```

| Operation | Contract |
| --- | --- |
| `validateWireGuardConfiguration(text)` | Returns line-numbered errors and warnings with no input values in diagnostic messages |
| `WireGuardProfiles::open(root)` | Opens private `wireguard` object collection for this owner |
| `importText(input)` | Validates and atomically creates a new UUIDv7 profile; does not take/keep source path |
| `list()` | Returns IDs, revisions, names, descriptions; **no private configuration text** |
| `read(id)` | Returns metadata, exact text, and current revision |
| `update(id, revision, input)` | Saves metadata and text together; preserves ID; rejects stale revisions |
| `exportText(id)` | Returns original text verbatim for explicit export; caller must protect exported secrets |
| `erase(id, revision, reference_count)` | Rejects missing/failed reference checks, nonzero usage and stale revisions |

`WireGuardProfile::issues` returns validation warnings on `importText`, `read`
and `update`, allowing GUI callers to display portability/runtime caveats even
if they did not call the standalone validator first. Diagnostics never echo
source lines, private keys, URLs, script arguments, or directive values.

### Delete guards

`WireGuardReferenceCount` is a mandatory `std::function` callback that takes
the profile UUID and returns `Result<std::size_t>` for the number of configured
service references. When the profile is used, `erase()` returns `profile_in_use`;
when lookup is missing, `reference_unavailable`; failed lookups propagate the
error. A stale revision returns `conflict` and nothing is deleted. Example:

```cpp
aowis::config::Result<bool> deleted = opened.value().erase(
    profile_id, known_revision,
    [&connections](const aowis::config::ObjectId &id) {
        return connections.countWireGuardReferences(id); // Application-owned catalogue.
    });
```

**Limitation:** The callback happens before the low-level collection deletion;
connection-catalogue changes are not locked in the same transaction. Phase 4
must coordinate creation/reassignment of references with deletion, or an
explicitly atomic cross-collection reference model must be designed. This
API makes accidental ungated deletion harder, but is **not** a proof of
cross-collection referential integrity under concurrency. Bare generic
`ObjectCollection::erase()` remains available to low-level callers.

## Validation scope and behavior

Accepted sections are `[Interface]` (one required) and zero or more `[Peer]`
sections. Nonempty `key = value` directives are required inside sections;
structural violations, invalid UTF-8/NUL, and configurations above 2 MiB are
errors and prevent saving. Missing keys, unrecognized directives, wg-quick
settings (such as `DNS`, `Address`, `MTU`), hook scripts and external key-file
references cause **warnings** and do not prevent import. Existing comments,
whitespace and directives are never removed. Up to 512 diagnostics are retained;
errors after the cap still make `valid()` return false.

This is a deliberately conservative **structure and portability check**, not
a complete WireGuard parser or a guarantee that AOWIS-NETWORK can activate the
result. It does not validate cryptographic key material, IP/CIDR syntax,
endpoint reachability or platform-specific transport support. Runtimes must
independently reject unsupported or unsafe directives. **Never execute hook
commands from imported configuration merely because storage accepts them.**

Metadata: nonblank UTF-8 name (at most 256 bytes), UTF-8 description (at most
4096 bytes). Names cannot contain CR, LF or NUL; descriptions cannot contain
NUL. The storage system validates JSON and restricts envelope size (8 MiB).

## Native security and tests

Imported secrets live in protected owner storage (`0700` directories, `0600`
files), with atomic file publication, expected revisions, cross-process
collection locking, and the interruption/recovery guarantees of Phase 2.
Permissions are not encryption at rest. Never print a full profile, JSON
payload, or exported text in diagnostics or logs.

`aowis-config-wireguard-profiles` tests import, read, edit, rename, list,
export, delete guards, schema rejection, secret-redacted warnings, CRLF,
UTF-8, and relocation/reopen. Existing Phase 2 tests cover underlying
filesystem security, stale concurrent edits and injected `SIGKILL` during
atomic writes. Tests do not establish power-cut resilience or Windows/WASM
persistence.

```bash
./compile_linux_tests.sh
python3 tests/repository_tooling_test.py
```

## Next integration boundary

AOWIS-GUI (Phase 4) should link `aowis::config`, open its `gui` owner root,
replace session-only in-memory profiles, pass imported file bytes to
`importText`, implement an authoritative reference counter/catalogue, and
resolve stable profile IDs in its service connection definitions. AOWIS-NETWORK
still owns tunnels, peer status and counters. The WebAssembly settings entry
remains disabled; this library does not enable embedded WireGuard in browsers.
