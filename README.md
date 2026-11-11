# AOWIS-CONFIG

**Configuration storage for the AOWIS ecosystem.** A small, independent
**C++20 static library** (`aowis::config`) for application-owned
configuration: validated owner roots, versioned JSON records with UUIDv7
identities, atomic single-record updates with revision conflicts, one lock per
owner for multi-step operations, a read-only mode for administrator-managed
roots, and storage of imported WireGuard profiles.

**Current release:** `0.8.0` (pre-1.0: APIs and on-disk format may change;
there are no productive installations to migrate).

> **Boundaries.** A shared *library* is not a shared *store*: `gui`,
> `gui-mobile`, `server-map`, `server-dvmdb` … each own a separate root.
> CONFIG stores configuration; AOWIS-NETWORK parses WireGuard configurations
> and runs tunnels; pure libraries (Render, MODEL) receive values from callers.

## Documentation

| Document | Contents |
| --- | --- |
| [Architecture](docs/architecture.md) | Purpose, ownership, record and reference model, backends, decision log, roadmap |
| [API](docs/api.md) | Every public type and call, integration recipe, backend contract, WireGuard storage |
| [Operations](docs/operations.md) | Security, layout on disk, backup/restore, error handling, tests and what they prove |

## Build and test

Linux prerequisites: CMake 3.20+, Ninja, a C++20 compiler; tooling tests also
need Python 3, Bash, `zip`, `unzip`.

```bash
./compile_linux.sh
./compile_linux_tests.sh
python3 tests/repository_tooling_test.py
```

Consume with `add_subdirectory(external/AOWIS-CONFIG)` or
`find_package(AOWISConfig CONFIG REQUIRED)`, then link `aowis::config`. No Qt,
DVMDB, AOWIS-NETWORK or third-party JSON dependency.

```cpp
aowis::config::Result<aowis::config::OwnerStore> store =
    aowis::config::OwnerStore::open(aowis::config::ConfigRoot::forCurrentUser(
        aowis::config::OwnerId::parse("gui").value()).value());
aowis::config::Result<aowis::config::WireGuardProfiles> profiles =
    aowis::config::WireGuardProfiles::open(store.value());
```

## State

| Feature | State |
| --- | --- |
| Owner roots (POSIX XDG/HOME, Windows `%LOCALAPPDATA%`, explicit roots) | Available |
| JSON records, UUIDv7, revisions, damaged-entry reporting | Available |
| Owner lock + `OwnerSession` for multi-collection operations | Available |
| Read-only mode for administrator roots (`/etc/aowis/...`, `ProgramData`) | Available (POSIX, Windows) |
| Native POSIX backend; in-memory backend (`MemoryStorage`) | Available |
| Native Windows backend (private DACLs, `LockFileEx`, write-through renames) | Available (syntax-checked only; first run in the Windows build) |
| Browser backend (localStorage, one writer tab via Web Locks) | Available (Emscripten; JS tested under Node, not yet in a browser) |
| WireGuard profile storage (exact text, name, description, reference guard) | Available |
| AOWIS-GUI integration (connections, WireGuard profiles) | Available |

## Repository workflow

From `~/coding/aowis/AOWIS-CONFIG`:

```bash
./tools/apply_patch.sh ~/Downloads/aowis-config-<description>.zip
./tools/zip_config_project.sh
```

`apply_patch.sh` accepts only `aowis-config-*.zip` with `AOWIS-CONFIG/`
entries, validates paths and symlinks, applies additions/replacements and an
optional `.aowis-patch-delete.txt` deletion manifest, and refreshes
`../config.zip`. Updates are atomic per file, not per patch; nothing is staged
or committed.

## Conventions

- Apache-2.0; standard C++20; no mandatory Qt.
- No `auto`; qualify member variables with `this->`, never member functions.
- No compatibility readers or dual writers for pre-production formats.
- Keep [architecture](docs/architecture.md) (decisions, roadmap) accurate as
  code lands; distinguish implemented behavior from plans.
