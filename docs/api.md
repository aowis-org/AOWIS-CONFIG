# API reference and integration

Status: **v0.6.0**, C++20, namespace `aowis::config`, CMake target
`aowis::config`. Design background: [architecture.md](architecture.md).

All fallible calls return `Result<T>`: `hasValue()` / `explicit operator bool`,
`value()`, `error()` (`Error{ErrorCode code; std::string message}`). Accessing
the wrong alternative throws `std::bad_variant_access`. Error messages never
contain stored payloads, configuration text or keys.

## Headers

| Header | Types |
| --- | --- |
| `owner_id.h` | `OwnerId` |
| `config_root.h` | `ConfigRoot` |
| `object_id.h` | `ObjectId` (UUIDv7) |
| `object_store.h` | `OwnerStore`, `OwnerSession`, `ObjectCollection`, `OpenOptions`, `AccessMode`, listings |
| `memory_storage.h` | `MemoryStorage` |
| `wireguard_profiles.h` | `WireGuardProfiles`, inputs, summaries, `WireGuardReferenceCount` |
| `json.h` | `validateJsonObject()` |
| `error.h`, `result.h`, `version.h` | `ErrorCode`, `Error`, `Result<T>`, `version_major/minor/patch` (0.6.0) |

## Identities and roots

- `OwnerId::parse(text)`: 1–63 chars, `a`–`z` first, then lowercase letters,
  digits or single interior hyphens (`gui`, `gui-mobile`, `server-dvmdb`).
  Collection names follow the same rule.
- `ObjectId::parse(text)` accepts lowercase RFC 9562 UUIDv7 only;
  `ObjectId::generate()` uses the OS random source.
- `ConfigRoot::forCurrentUser(owner)`: `$XDG_CONFIG_HOME/aowis/<owner>`, else
  `$HOME/.config/aowis/<owner>`; Windows `%LOCALAPPDATA%\aowis\<owner>`;
  Emscripten `unsupported_platform`. **Android:** no meaningful HOME/XDG —
  use `ConfigRoot::fromNamespaceRoot(owner, <QStandardPaths::AppConfigLocation>/aowis)`.
- `ConfigRoot::fromNamespaceRoot(owner, abs)` appends the owner;
  `fromOwnerRoot(owner, abs)` uses the path as is (e.g. `/etc/aowis/server-dvmdb`).
  Roots only name paths; nothing touches the disk before `OwnerStore::open`.

## OwnerStore and OwnerSession

```cpp
struct OpenOptions { AccessMode mode = AccessMode::read_write;
                     std::chrono::milliseconds lock_timeout = default_lock_timeout; }; // 5 s
Result<OwnerStore> OwnerStore::open(const ConfigRoot &root, OpenOptions = {});
Result<OwnerStore> OwnerStore::openInMemory(const MemoryStorage &storage, OpenOptions = {});
Result<ObjectCollection> OwnerStore::collection(std::string_view name) const;
Result<OwnerSession> OwnerStore::lock() const;
AccessMode OwnerStore::mode() const noexcept;
```

| Mode | Behavior |
| --- | --- |
| `read_write` | Creates missing directories (0700); one exclusive owner lock (`<owner>/.lock`) per operation or session; entries must be owned by the user and private; `open()` and `collection()` verify everything up front and remove crash residue |
| `read_only` | Creates, locks, recovers and writes nothing; writes return `permission_denied`; owner root must exist (`not_found`); a missing collection lists empty; entries may be owned by root and group-readable, never group/other-writable |

Ancestors above the owner directory may be symlinks; the owner directory and
everything below must not be (`unsafe_path`).

`OwnerSession` holds the owner lock until destroyed (move-only). Pass it to
the session overloads to run several operations — on any collections of the
same store — without another instance interleaving. A session of another store
yields `session_mismatch`. Use it on the creating thread; do not call
session-less operations of the same store while holding it (they would wait
for your own lock and end in `lock_timeout`). Read-only sessions hold no lock.
Not a crash-atomic transaction.

`lock_timeout`: the lock is polled until the deadline, then
`ErrorCode::lock_timeout`; 0 means one attempt. Calls are synchronous with
two `fsync`s per write — run them off the Qt event-loop thread.

## ObjectCollection

Every operation exists twice: without a session (takes and releases the owner
lock itself) and with `const OwnerSession &` as first parameter.

| Call | Behavior |
| --- | --- |
| `list()` | `ObjectListing{objects (id, revision; no payload), unreadable}` |
| `readAll()` | `ObjectSnapshot{objects (with payload), unreadable}`, one session |
| `read(id)` | `StoredObject{info, json}` |
| `create(json)` | Validates a JSON object (≤ ~8 MiB, depth ≤ 64), new UUIDv7 + revision |
| `update(id, json, expected_revision)` | Replaces only if the revision matches, else `conflict` |
| `erase(id, expected_revision)` | Deletes only if the revision matches |
| `eraseCorrupt(id)` | Deletes only while the record is `corrupt_object`; readable → `conflict`; `unsupported_schema`, `unsafe_path` etc. are returned and nothing is deleted |
| `name()`, `store()` | Collection name; its `OwnerStore` (e.g. to take a session) |

`UnreadableObject{filename, optional id, error}` covers corrupt JSON, wrong
identity, unsafe permissions or symlinks, records of a newer envelope version
(`unsupported_schema`) and `*.json` names that are not UUIDv7. Listings fail
as a whole only for store-level problems (lock, directory).

After an I/O error *after* publication the write may have taken effect:
re-read before retrying.

## WireGuard profiles

```cpp
Result<WireGuardProfiles> WireGuardProfiles::open(const OwnerStore &store); // "wireguard" collection
struct WireGuardProfileInput { std::string name, description, configuration; };
using WireGuardReferenceCount =
    std::function<Result<std::size_t>(const OwnerSession &, const ObjectId &)>;
```

| Call (also with `OwnerSession`) | Contract |
| --- | --- |
| `importText(input)` | Stores the exact text with name/description; new UUIDv7 |
| `list()` | `WireGuardProfileListing{profiles (id, revision, name, description; no text), unreadable}` |
| `read(id)` / `exportText(id)` | Metadata + exact text / exact text only |
| `update(id, revision, input)` | Replaces metadata and text together; stale → `conflict` |
| `erase(id, revision, reference_count)` | Missing callback → `reference_unavailable`; failed → its error; nonzero → `profile_in_use`; stale → `conflict` |
| `eraseUnreadable(id, reference_count)` | Removes a corrupt or wrongly typed record with the same guard; readable → `conflict`; newer `profile_version` → `unsupported_schema`, kept |
| `collection()` | The underlying `ObjectCollection` |

Input checks (`invalid_wireguard_profile`): name nonblank UTF-8 ≤ 256 bytes
without CR/LF/NUL; description UTF-8 ≤ 4096 bytes without NUL; configuration
nonempty, ≤ 2 MiB (`max_wireguard_configuration_bytes`), UTF-8, no NUL.
**No WireGuard syntax is checked here** — call AOWIS-NETWORK's
`parseWireGuardConf()` in the editor and refuse to save on its errors. Reads
never re-apply input rules, so stored profiles stay readable and repairable
when rules tighten.

The reference callback runs inside the deleting session and must read the
catalogue through it:

```cpp
const aowis::config::WireGuardReferenceCount count =
    [&connections](const aowis::config::OwnerSession &session, const aowis::config::ObjectId &profile) {
        aowis::config::Result<aowis::config::ObjectSnapshot> all = connections.readAll(session);
        if (!all) return aowis::config::Result<std::size_t>::failure(all.error());
        std::size_t used = 0;
        for (const aowis::config::StoredObject &record : all.value().objects) {
            if (referencesProfile(record.json, profile)) ++used; // application parses its own schema
        }
        return aowis::config::Result<std::size_t>::success(used);
    };
```

Reassign-then-delete in one session:

```cpp
aowis::config::Result<aowis::config::OwnerSession> session = store.lock();
connections.update(session.value(), connection_id, json_without_profile, connection_revision);
profiles.erase(session.value(), profile_id, profile_revision, count);
```

## MemoryStorage

Non-persistent owner storage on every platform; copies share contents;
thread-safe. `OwnerStore::openInMemory(storage, options)` gives the same
semantics as a persistent store, including read-only mode. Fixture helpers
`writeRawEntry(collection, entry_name, bytes)` / `readRawEntry(...)` bypass
validation to simulate damaged or foreign entries. Use it for GUI unit tests;
never as a production store.

## Integration recipe (AOWIS-GUI, Phase 4)

1. Submodule `external/AOWIS-CONFIG`, `add_subdirectory`, link `aowis::config`.
2. At startup: `OwnerStore::open(ConfigRoot::forCurrentUser(gui))`; keep the
   store; open `WireGuardProfiles` and the `connections` collection.
3. Settings list from `list()`; show `unreadable` rows with a remove action
   (`eraseUnreadable` after confirmation).
4. Import/edit: read the file bytes, run `parseWireGuardConf()` for
   diagnostics, refuse to save on errors, then `importText`/`update`; reload on
   `conflict`. Never keep the source path; never log text or keys.
5. Connections reference profiles by `transport.profile_id`; "Used by" and the
   delete guard count connection records inside the session.
6. Tunnel start: `read(id)` → `parseWireGuardConf(text)` → `WireGuardClient`.
7. Status comes from AOWIS-NETWORK only. WASM: entry disabled with tooltip.
8. Remove `servers/profiles`, `wireguard_profile_path`, `readProfile()` and
   their localStorage code in the same phase; no migration.

## Backend contract

Private (`src/storage_backend.h`); implemented inside this library only.
Every backend guarantees:

| # | Guarantee | POSIX | Memory |
| --- | --- | --- | --- |
| 1 | Exclusive read-write owner sessions across threads/processes; bounded wait → `lock_timeout` | `flock` on `<owner>/.lock`, polled | `std::mutex`, polled |
| 2 | Atomic single-entry replacement; `create_only` never replaces (`conflict`) | fsynced temp + `linkat`/`renameat` | map assignment under lock |
| 3 | Success only once durable under the backend's failure model | file + directory `fsync` | lifetime of the last handle |
| 4 | Enumerate every `*.json` entry incl. foreign names; hide bookkeeping | `readdir`, skips `.lock`/`.tmp-*` | map keys |
| 5 | Per-entry errors without stored bytes | symlink/permission/size checks | size check |
| 6 | Bound to one owner root; names validated before reaching the backend | descriptor walk | one `MemoryStorage` |
| 7 | `collection(name, recover)` removes own crash residue | `.tmp-<uuidv7>` cleanup | none |
| 8 | Read-only mode: no creation/lock/recovery/writes; missing collection = empty | relaxed ownership, never writability | same |

`tests/backend_contract_test.cpp` runs one behavioral suite against every
backend of the build platform; a new backend is added to `backends()` there
and must pass unchanged. Adding a backend: implement `openNativeBackend()` in
`src/<platform>_backend.cpp`, select it in `CMakeLists.txt` instead of
`unsupported_backend.cpp`, add it to the contract test, document its failure
model in [operations.md](operations.md).
