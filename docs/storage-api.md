# Phase 2 — native object storage API

Status: **implemented on native POSIX (including Linux)**, C++20, dependency-free.
Windows, Android-specific policy and Emscripten/browser persistence remain later
platform work. This is **configuration object storage**, not a WireGuard importer,
GUI integration, preferences implementation or multi-object transaction engine.

## On-disk contract

Each application/service chooses an independent `ConfigRoot` (`aowis/gui`,
`aowis/gui-mobile`, etc.), with lazily created collections:

```text
~/.config/aowis/gui/
  wireguard/
    .lock
    <uuidv7>.json
  connections/
    .lock
    <uuidv7>.json
```

A record is written atomically as one **private file** with an envelope:

```json
{
  "schema_version": 1,
  "id": "019a1234-0000-7000-8000-000000000001",
  "revision": "0123456789abcdef0123456789abcdef",
  "payload": { "name": "Example", "example_value": "opaque to CONFIG" }
}
```

The example ID/revision above illustrate the format, not actual generated
values. The top-level `schema_version` is the **storage envelope** version.
The `payload` is any valid JSON **object** interpreted and validated by the
owning component; CONFIG validates JSON syntax, UTF-8, nesting limits and the
envelope but not domain-specific fields. Components may additionally version
their own payloads.

**Single-record atomicity is intentional.** For the WireGuard use case, Phase
3 can store imported configuration text as an escaped JSON string within the
profile's payload alongside metadata. JSON escaping is lossless and the file
contains the full imported text and private key. This avoids a partially
installed `profile.json`/`tunnel.conf` pair. There is no stand-alone `.conf`
file in the adopted core layout. The importer will define exact metadata and
encoding; neither encryption nor WireGuard interpretation is provided yet.

## C++ API

```cpp
#include <aowis/config/config_root.h>
#include <aowis/config/object_store.h>

// Use checked Result<T> values at each step in real code.
aowis::config::Result<aowis::config::OwnerId> owner =
    aowis::config::OwnerId::parse("gui");
aowis::config::Result<aowis::config::ConfigRoot> root =
    aowis::config::ConfigRoot::forCurrentUser(owner.value());
aowis::config::Result<aowis::config::ObjectCollection> collection =
    aowis::config::ObjectCollection::open(root.value(), "wireguard");
aowis::config::Result<aowis::config::StoredObject> created =
    collection.value().create("{\"name\":\"Office VPN\"}");

aowis::config::Result<aowis::config::StoredObject> updated =
    collection.value().update(created.value().info.id,
                              "{\"name\":\"Office VPN (updated)\"}",
                              created.value().info.revision);
```

Public functions:

| Function | Behavior |
| --- | --- |
| `ObjectId::parse(text)` | Accepts lowercase RFC 9562 UUIDv7 with correct variant and canonical hyphens |
| `ObjectId::generate()` | Creates UUIDv7 from wall-clock milliseconds and OS randomness (`getrandom` on Linux) |
| `validateJsonObject(text)` | Strict JSON object syntax and UTF-8 check; payload semantics are caller-owned |
| `ObjectCollection::open(root, name)` | Validates collection name; opens/creates private owner and collection directories |
| `list()` | Sorted IDs, schema versions, revisions, **no payload or secrets** |
| `read(id)` | Returns the complete payload and revision token |
| `create(json, schema_version=1)` | Validates and stores a new UUIDv7 record |
| `update(id, json, expected_revision, schema_version=1)` | Replaces record only if revision still matches |
| `erase(id, expected_revision)` | Deletes record only if revision still matches |

Revision tokens are opaque and change on every successful update. Always
read a revision before editing or deleting. A missing object reports
`not_found`, a stale/missing revision `conflict`, invalid JSON `invalid_json`,
and unknown envelope versions `unsupported_schema`. A `list()` encountering
corrupt stored data returns an error, **not a silently shortened catalogue**.
Never include secret JSON in logging/error strings.

The public core has no Qt types and no third-party runtime dependencies. Each
object is capped at 8 MiB including envelope; JSON nesting depth is capped at
64. The current JSON parser validates syntax, not canonical representations or
application-specific JSON-schema constraints.

## Native protection, recovery and concurrent access

On native POSIX:

- Traversal from `/` uses directory descriptors and `openat`/`mkdirat` with
  `O_NOFOLLOW`; symlink components, collection-name traversal, unsafe types,
  owner mismatches, and owner/collection directory permissions broader than
  `0700` are rejected.
- Owner and collection directories are created mode `0700`; the lock, object
  and temporary files are created mode `0600`. Existing weak permissions are
  rejected, not silently changed. No keychain encryption is implied.
- Each collection has an advisory `flock` lock held around operations; writers
  use expected revision tokens to prevent silent lost updates between
  cooperating processes. Calls are synchronous; no background worker or file
  watcher is implemented.
- New contents are written to an exclusive temporary file in the same
  directory, `fsync`ed, then published with a non-replacing link (create)
  or atomic rename (update), followed by directory `fsync`. Delete similarly
  `unlink`s the object and `fsync`s the directory.
- On the next collection access, an interrupted pre-publication write's
  `.tmp-<uuidv7>` orphan is checked and removed. No recovery process rewrites
  already committed object data.
- An I/O or sync failure after publication can have an **uncertain outcome**.
  Callers must re-read the record before retrying an operation. File-system
  correctness assumes normal POSIX rename/link and `fsync` behavior; testing
  process termination is **not proof of hardware power-cut durability**.

Atomicity is per record, not across two different objects or collections.
**There is deliberately no cross-record transaction API.** For a future
operation that must update multiple objects atomically, design a separate
journal/transaction facility rather than claiming this one provides it.
An application must enforce referential integrity (e.g. WireGuard `Used by`)
at its owning catalogue layer; bare `erase()` cannot discover component-specific
references.

## Test coverage

```bash
./compile_linux_tests.sh
python3 tests/repository_tooling_test.py
```

CTest covers JSON/UTF-8 validation, UUIDv7 parsing and generation, CRUD,
reopen, relocation/copy, owner isolation, permissions, symlinks, corruption,
unsupported schemas, stale edits/deletions and a forked cross-process conflict.
A test-only compiled fault hook delivers `SIGKILL` immediately after temp-file
`fsync` or publication, verifying that interrupted operations leave complete
records and that temporary files are recovered. It is only compiled in native
**top-level test builds**; it does not exist in ordinary library builds.
No physical power-cut, Windows backend or Android permission test is claimed.

Future phases must implement application-owned typed schemas, imported WG text
serialization, reference guards, GUI integration and platform-specific
backends without weakening these invariants.
