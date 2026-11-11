# Architecture, decisions and roadmap

Status: **v0.8.0**. Updated: 2026-11-11. API details: [api.md](api.md);
security and operations: [operations.md](operations.md).

## 1. Purpose and boundaries

AOWIS-CONFIG is the reusable storage machinery for configuration that an AOWIS
application or service *owns*: owner-scoped roots, record identity and
revisions, atomic writes, locking, damaged-entry handling, and a typed store
for imported WireGuard profiles. Applications own the meaning of their
records (schemas); the library owns how records are stored.

| Concern | Owner |
| --- | --- |
| Where/how records are stored, locked, versioned, recovered | AOWIS-CONFIG |
| Record schemas (connections, unit profiles, preferences) | The owning application |
| WireGuard `.conf` syntax, runtime support, tunnels, status | AOWIS-NETWORK |
| Which connections reference which WireGuard profile | The owning application's connection catalogue |
| DVMDB repositories, working copies, tiles, caches, logs | Their own data/state/cache locations, never the config root |

Only components that own persistent configuration link the library. Pure
libraries (AOWIS-GUI-Render, AOWIS-SERVER-MODEL) receive values from callers.

## 2. Owners and roots

```text
${XDG_CONFIG_HOME:-~/.config}/aowis/        (Windows: %LOCALAPPDATA%\aowis\)
├── gui/
│   ├── .lock
│   ├── wireguard/<uuidv7>.json
│   ├── connections/<uuidv7>.json      (Phase 4)
│   └── unit-profiles/<uuidv7>.json    (Phase 5)
├── gui-mobile/                        (separate owner; explicit root on Android)
├── server-map/
└── server-dvmdb/
/etc/aowis/server-dvmdb/               (administrator-managed, opened read-only)
```

One owner = one root = one lock. Owners never share a physical store, even
when they use the same schemas; moving objects between owners is an explicit
export/import. Directories are created on demand in read-write mode only.

**Portability:** copying a stopped owner's root reproduces its configuration,
including imported WireGuard profiles and their private keys; references use
stable IDs, never paths. It does not copy repositories, runtime state or
external dependencies inside imported text (hooks, key files), and it does not
make remote endpoints reachable.

## 3. Records and references

- One record = one JSON file: a library-internal envelope
  (`envelope_version`, `id`, `revision`, `payload`) around the application's
  JSON object. Metadata and secrets in one record change together.
- Identity: UUIDv7, never derived from names; renaming changes nothing else.
- Revisions: a new opaque token on every write; update/erase require the last
  read token (`conflict` otherwise). No silent last-writer-wins.
- Versioning: the envelope version is internal. Payloads carry their own
  version field when they need one (`profile_version` in WireGuard records).
- Damaged entries never hide the rest: listings return readable records plus
  `unreadable` entries; corrupt records can be removed deliberately, records
  of a newer version never are.
- Connection identity ≠ remote identity: a connection record has its own ID;
  a DVMDB `repository_id` learned from the server is a separate field; a
  WireGuard profile is referenced by `transport.profile_id`.

Illustrative Phase 4 connection payload (not yet a ratified schema):

```json
{
  "connection_version": 1,
  "name": "Office DVMDB",
  "service_type": "dvmdb",
  "endpoint": "http://10.8.0.10:8755",
  "repository_id": "<learned-repository-uuidv7, optional>",
  "transport": { "type": "wireguard", "profile_id": "<wireguard-profile-uuidv7>" }
}
```

**Referential integrity** comes from the owner lock: an `OwnerSession` holds
it across collections, so "count references in `connections`, then delete the
WireGuard profile" (or "reassign, then delete") cannot interleave with another
instance. It is serialization, not a crash-atomic transaction: callers order
writes so every intermediate state is valid. "Used by N" is computed from the
connection records, never stored.

## 4. WireGuard

- Importing copies the complete `.conf` text into one owner record (exact
  bytes, comments, CRLF, keys); the source path is never kept.
- CONFIG checks text integrity only (nonempty, ≤ 2 MiB, UTF-8, no NUL). There
  is exactly one WireGuard parser: AOWIS-NETWORK's `parseWireGuardConf()`,
  which decides what the embedded engine supports. The GUI runs it on import
  and edit, shows its diagnostics, and refuses to save on errors.
- One profile can be referenced by many connections of the same owner.
- Runtime state (running, handshakes, counters, consumers) lives in
  AOWIS-NETWORK and is never written to configuration.
- In WebAssembly the `WireGuard VPN` settings entry stays visible but disabled
  with an explanatory tooltip: browsers cannot run the embedded tunnel.

## 5. Backends

```text
OwnerStore / ObjectCollection / WireGuardProfiles   public, platform-independent
        │  envelope, validation, revisions, listing, erase rules, sessions
        ▼
detail::OwnerBackend → OwnerSessionImpl → CollectionSession   private contract
  ├── posix_backend.cpp        Linux, Android, macOS, BSD
  ├── memory_backend.cpp       MemoryStorage, every platform
  ├── key_value_backend.cpp    generic logic over a flat key-value store
  │     └── browser_backend.cpp  Emscripten: localStorage + Web Locks writer lease
  └── windows_backend.cpp      Windows: DACL policy, LockFileEx, write-through renames
```

Exactly one persistent backend is compiled per platform. The contract
(src/storage_backend.h, summarized in [api.md](api.md#backend-contract)) is
enforced by one shared test suite run against every available backend.

## 6. Decision log

Status: **A** agreed and implemented, **S** superseded (kept for history).

| ID | Decision | Consequence |
| --- | --- | --- |
| D01 A | Independent repo, reusable C++20 library `aowis::config` | One storage implementation for all owners |
| D02 A | No mandatory Qt in the core | Services do not pull in GUI frameworks |
| D03 A | No shared mutable `aowis/shared/` directory | Apps cannot overwrite each other's configuration |
| D04 A | Owner-specific roots inside an `aowis` namespace | `~/.config/aowis/gui/`, `/etc/aowis/server-dvmdb/` |
| D05 A | Shared schemas ≠ shared stored instances | Explicit export/import between owners |
| D06 A | Only configuration owners link the library | Render/MODEL receive values |
| D07 A | Stable UUIDv7 identities | Renames never break references |
| D08 A | WireGuard text is imported, never referenced by path | Self-contained owner root |
| D09 A | One WireGuard profile, many connections (same owner) | No duplicated secrets |
| D11 A | Private keys stay in the owner root; no mandatory keychain | Copying the root copies profiles; not encryption |
| D12 A | Secrets: strict permissions, atomic writes, redacted errors | See operations.md |
| D13 A | Tunnels and runtime status belong to AOWIS-NETWORK | CONFIG never runs or reports tunnels |
| D14 A | UI entry `WireGuard VPN`, disabled with tooltip in WASM | No browser VPN claim |
| D15 A | WireGuard UI: CRUD, import, text editor, `Used by`, `Status` | References ≠ runtime consumers |
| D16 A | No migration of pre-production GUI formats | Replace and delete legacy storage |
| D17 A | Portability by copying an owner root | Stable IDs, no hidden paths |
| D18 A | System services use administrator roots, opened read-only | Implemented by D38 |
| D19 A | Connection identity separate from remote repository identity | Fixes GUI's `ProjectServerProfile::id` in Phase 4 |
| D21 A | Namespace/target `aowis::config`, explicit `ConfigRoot` | No implicit shared directory |
| D22 A | Internal JSON parser, no JSON dependency, no public JSON value API | Applications parse payloads with their own JSON library (Qt: QJsonDocument) |
| D23 A | One record = one atomic JSON file | No multi-file update ambiguity |
| D25 A | Durability per record; no crash-atomic multi-record transactions | Order writes so intermediate states are valid |
| D26 A | Reject unsafe paths, symlinks and permissions at/below the owner root | Private 0700/0600 in read-write mode |
| D27 A | WireGuard record: versioned payload with exact escaped text | Text, metadata and key update together |
| D30 A | Ancestors above the owner root are trusted and may be symlinks | Silverblue `/home`, macOS `/var`, dotfile `~/.config` work |
| D31 A | Listings report damaged entries; only corrupt ones are removable | Newer-version records are never deleted |
| D33 A | Locks are time-limited (`lock_timeout`, default 5 s) | A hung instance cannot block forever |
| D34 A | Private backend contract; one persistent backend per platform | Windows/browser add a file, not a second path |
| D35 A | Public `MemoryStorage` on every platform, same contract tests | Filesystem-free consumer tests |
| D36 A | One lock per owner (not per collection) plus `OwnerSession`; reference callbacks receive the session | Closes the reference/delete race inside the library; no GUI coordinator needed |
| D37 A | Envelope version is internal; no caller `schema_version` parameter | Payloads version themselves |
| D38 A | `AccessMode::read_only`: no lock, no creation, no writes; root-owned, group-readable, never group/other-writable entries accepted | `/etc/aowis/<owner>` with `0640 root:aowis` works |
| D39 A | Windows roots under `%LOCALAPPDATA%`, never roaming `%APPDATA%` | Keys are not synced to domain servers |
| D40 A | CONFIG does not parse WireGuard; AOWIS-NETWORK owns the single `.conf` parser | No diverging parsers; storage checks integrity only |
| D41 A | Browser backend on `localStorage` (synchronous, ~5 MB/origin), one key per record, root `/aowis/<owner>` as key namespace | No Asyncify/JSPI in consumers; WireGuard is disabled in browsers, so no keys are stored there |
| D42 A | One writer tab per owner root: a Web Locks lease requested by the first read-write store and held for the page's lifetime; other tabs read; no Web Locks → read-only (fail closed) | Cross-tab lost updates impossible; a waiting tab takes over when the writer closes |
| D43 A | Windows backend: `\\?\` paths; owner/collection directories held without `FILE_SHARE_DELETE`; `LockFileEx` on `<owner>\.lock`; temp + `FlushFileBuffers` + `MoveFileExW(MOVEFILE_WRITE_THROUGH)` (no `ReplaceFileW`: it merges attributes/ACLs and is not atomic); no reparse points at/below the owner root | Same contract as POSIX without `openat`; handle-based enumeration and orphan deletion |
| D44 A | Windows access policy: owner ∈ {user, SYSTEM, Administrators}; read-write: no allow entry for anyone else; read-only: no write rights for anyone else; new objects get a protected user-only DACL | Counterpart of 0700/0600 and D38; Explorer's default reset stays accepted |
| D10 S | ~~Distinguish parse validity from runtime support in CONFIG~~ | Superseded by D40 |
| D20 S | ~~Caller-visible schema version on every record~~ | Superseded by D37 |
| D24 S | ~~Per-collection `flock`~~ | Superseded by D36 |
| D28/D32 S | ~~WireGuard validation in CONFIG~~ | Superseded by D40 |
| D29 S | ~~Reference callback without a shared lock~~ | Superseded by D36 |

### Open questions (decide when the phase starts)

1. **Preferences format** (Phase 5): versioned JSON records vs. a small
   key/value preferences API; scalar preferences work today and are last.
2. **Browser administrator config:** whether the webroot `.ini` the WASM GUI
   reads today becomes a read-only browser root (fetched records) or stays a
   GUI concern (Phase 5).
3. **Cross-owner transfer UX:** export/import and ID collisions.
4. **Optional encryption at rest / key rotation:** never a mandatory keychain
   (portability, D11).

## 7. Roadmap

| Step | State | Content |
| --- | --- | --- |
| Phases 0–3 | Done | Tooling, library, POSIX storage, WireGuard storage |
| Hardening + backends | Done (0.4–0.5) | Damaged entries, lenient reads, lock timeout, backend contract, `MemoryStorage` |
| Design fixes | Done (0.6) | Owner lock + `OwnerSession`, read-only mode, internal envelope version, `%LOCALAPPDATA%`, WireGuard parsing moved to NETWORK, docs merged |
| NETWORK parser | Done | `parseWireGuardConf()` in AOWIS-NETWORK with tests (separate repo) |
| Browser backend | Done (0.7) | localStorage + Web Locks writer lease; C++ logic in the contract suite, JS glue under Node; first real-browser run happens with Phase 4 |
| Phase 4 | Done | GUI connections + WireGuard: new connection schema, profiles in CONFIG, probe/reader/writer by profile ID, `Used by` from connections, delete `servers/profiles`, `wireguard_profile_path`, `readProfile()` and the localStorage path |
| Windows backend | Done (0.8) | D43/D44; syntax-checked only, first compile and run in the Windows build |
| Map-server client | Done (GUI) | `service_type "map-server"` record in the GUI's `connections` collection with base URL and API keys; former INI section deleted without migration (D16); direct transport only, others fail closed |
| Phase 5 | **Next** | Unit profiles (swap behind `UnitProfileStorage`), then preferences |
| Phase 6 | Planned | Mobile and servers opt in one at a time (servers via read-only roots) |
| Phase 7 | Planned | Cross-platform security review, power-loss testing, release checklist |
