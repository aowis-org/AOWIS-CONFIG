# AOWIS-CONFIG

**Configuration storage for the AOWIS ecosystem.** AOWIS-CONFIG is a small,
independent **C++20 static library** for application-owned configuration:
validated owner roots, versioned JSON records, UUIDv7 identities, atomic
single-record updates, conflict detection, and imported WireGuard profiles.

**Current release:** `0.3.0` · **Current milestone:** Phase 3 complete on native
POSIX; Phase 4 (AOWIS-GUI integration) is next. This is a pre-1.0 project:
public APIs and schemas may change. There are **no productive installations**
requiring migration of AOWIS's experimental settings.

> **Boundary:** A shared *library* does not imply a shared *configuration
> directory*. `gui`, `gui-mobile`, `server-map`, `server-dvmdb` and other
> owners have separate stores. AOWIS-CONFIG persists profiles; AOWIS-NETWORK
> runs tunnels and reports their status.

## Start here

| I want to… | Read |
| --- | --- |
| Build and test the repository | [Quickstart](docs/quickstart.md) |
| Embed the library in an AOWIS component | [Consumer integration](docs/integration-guide.md) |
| Understand current C++ entry points | [Library API](docs/library-api.md), [Storage API](docs/storage-api.md) |
| Import and manage WireGuard profiles | [WireGuard profiles](docs/wireguard-profiles.md) |
| Understand boundaries and filesystem ownership | [Architecture](docs/architecture.md) |
| Back up, restore, or troubleshoot a configuration | [Operations and security](docs/operations.md) |
| See what is tested and what is not | [Verification guide](docs/testing.md) |
| Resume work or plan the next implementation | [Handoff](docs/handoff.md), [Roadmap](docs/roadmap.md) |
| Review settled and unsettled decisions | [Decision log](docs/decisions.md) |

## Build and test

Linux prerequisites: CMake 3.20+, Ninja, a C++20 compiler. Repository tooling
also needs Python 3, Bash, `zip` and `unzip`.

```bash
./compile_linux.sh
./compile_linux_tests.sh
python3 tests/repository_tooling_test.py
```

The public CMake target is **`aowis::config`**. It can be used with
`add_subdirectory(external/AOWIS-CONFIG)` or `find_package(AOWISConfig CONFIG
REQUIRED)` after installation. There are **no mandatory Qt, DVMDB,
AOWIS-NETWORK, or external JSON-library dependencies**. See the
[quickstart](docs/quickstart.md) for complete commands and examples.

## What's implemented

| Feature | State |
| --- | --- |
| Owner IDs and configuration-root resolution | Available (native POSIX and Windows path resolution) |
| Strict JSON-object validation and UUIDv7 IDs | Available |
| Private object collections; CRUD and revision tokens | **Native POSIX only** |
| Atomic per-record publication, collection locks, interruption tests | **Native POSIX only** |
| WireGuard profile import/edit/read/list/export/delete | **Native POSIX only** |
| Lossless imported `.conf` text (including keys) inside owned JSON records | Available via WireGuard profiles API |
| AOWIS-GUI persistence, connection references, and live status | **Not integrated yet (Phase 4)** |
| Preference store, Windows storage, browser storage | **Not implemented** |
| Cross-record transactions and hardware power-cut verification | **Not implemented / not established** |

## Owner-scoped storage

The native user layout is, for example:

```text
~/.config/aowis/
├── gui/
│   ├── wireguard/
│   │   ├── .lock
│   │   └── <uuidv7>.json
│   ├── connections/        # planned GUI-owned records, not integrated yet
│   └── unit-profiles/      # planned GUI-owned records, not integrated yet
├── gui-mobile/             # separate owner, when configured
├── server-map/             # separate owner, when configured
└── server-dvmdb/           # separate owner, when configured
```

Directories are created on demand. The POSIX collection backend stores each
object in **one** JSON file, including a WireGuard profile's metadata and its
original `.conf` text. Importing a `.conf` **copies its content into AOWIS**;
the original path is not retained. Copying the owning configuration tree can
transfer profiles and configuration references, but not DVMDB repositories,
working copies, runtime state, external secrets, or endpoint reachability.

**Secret handling:** stored WireGuard configurations contain private keys.
Owner/collection directories use restrictive permissions and files use `0600`;
this is **not encryption**. Treat copies, archives, and exports as sensitive.
See [operations and security](docs/operations.md).

## Repository patch and snapshot workflow

From `~/coding/aowis/AOWIS-CONFIG`:

```bash
./tools/apply_patch.sh ~/Downloads/aowis-config-description.zip
./tools/zip_config_project.sh
```

`apply_patch.sh` accepts `aowis-config-*.zip` with `AOWIS-CONFIG/` archive
entries, validates paths and symlinks, applies additions/replacements and an
optional `.aowis-patch-delete.txt` deletion manifest, and automatically
refreshes **`../config.zip`**. Updates are atomic **per file**, not per patch.
The source snapshot excludes `.git`, build products, and temporary files.
The patch script does not stage or commit changes.

## Project conventions

- **Apache-2.0** (`LICENSE`); standard **C++20** core, with no mandatory Qt.
- Do not use `auto` in C++ code; qualify member access with `this->`.
- Components that *own* persistent configuration depend on AOWIS-CONFIG;
  pure renderer/model libraries receive already-parsed values from callers.
- Do not add compatibility readers or dual writers for pre-production GUI INI
  formats. Remove obsolete implementations when each consumer is converted.
- Keep the [roadmap](docs/roadmap.md) and [handoff](docs/handoff.md) accurate
  as code lands; distinguish verified code from proposals.
