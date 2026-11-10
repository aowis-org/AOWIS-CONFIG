# AOWIS-CONFIG

**AOWIS-CONFIG** is an independent C++20 library for persistent
configuration machinery across AOWIS applications and services. Its purpose is
to share the *implementation and formats* of configuration management, **not**
to make independent applications write to one shared configuration directory.

> **Implementation status (2026-11-10):** Phase 1 foundation is implemented:
> CMake library, public owner/root/error/result API, Linux build scripts,
> independent consumer smoke and tests. **There is no persistent storage engine,
> WireGuard import, or GUI integration yet.** See [library API](docs/library-api.md).

## Read first

- [Architecture and requirements](docs/architecture.md) — ownership, paths,
  persistence, WireGuard, security, portability, and API boundaries.
- [Decisions and open questions](docs/decisions.md) — what was agreed and what
  remains a design choice.
- [Implementation roadmap](docs/roadmap.md) — staged work, tests, acceptance
  criteria, and explicit removal of legacy storage.
- [Development handoff](docs/handoff.md) — current state and precise next steps
  for continuing in a new thread or checkout.
- [Library API / consumption](docs/library-api.md) — implemented Phase 1 interface,
  build scripts, CMake submodule and installed-package examples.

## Core rules

1. One reusable C++20 configuration **library**; separate physical stores owned
   by `gui`, `gui-mobile`, `server-map`, `server-dvmdb`, etc.
2. No universal writable `shared/` configuration directory. Components may use
   common schemas without silently sharing mutable objects.
3. Persist reusable configuration objects by stable IDs, not paths to files
   outside the AOWIS-owned configuration root.
4. Import WireGuard `.conf` files into the **owning application's** storage,
   preserving their text, including secret keys. A copied configuration tree
   should contain everything AOWIS needs from that imported profile.
5. Treat credentials as secrets, preserve restrictive permissions, and never
   log them. No external keychain dependency is required for profile portability.
6. AOWIS-CONFIG owns persistence, not network sessions. AOWIS-NETWORK owns
   tunnel lifecycle, handshake/traffic status, and network transport behavior.
7. Use deliberate breaking changes now: there are **no productive installations**
   requiring migration from existing experimental settings formats. Replace
   obsolete storage paths and remove dead compatibility implementations.
8. Library consumers must opt in because they own persistent configuration.
   Pure rendering, model, and compute libraries should receive typed values
   rather than reading configuration files by themselves.

## Intended native layout (illustrative)

```text
~/.config/aowis/
├── gui/
│   ├── preferences.json             # final preference format to be decided
│   ├── connections/<uuid>.json
│   ├── unit-profiles/<uuid>.json
│   └── wireguard/<uuid>/
│       ├── profile.json
│       └── tunnel.conf
├── gui-mobile/
├── server-map/
└── server-dvmdb/
```

These directories are **independently owned**. They are created only as needed.
System services may be configured with a separate explicit root such as
`/etc/aowis/server-dvmdb/`; a GUI user's home directory is not a systemd
service's configuration location. OS-specific roots are resolved by a storage
adapter, not hard-coded into feature widgets.

Do not confuse configuration with persistent operational data: DVMDB project
repositories, working copies, caches, and other data have separate roots and
are **not** migrated simply by copying `~/.config/aowis/`.

## Build and test (Phase 1 implemented)

```bash
./compile_linux.sh
./compile_linux_tests.sh
python3 tests/repository_tooling_test.py
```

CMake 3.20+, Ninja and a C++20 compiler are required for the scripts; Python
3, Bash and ZIP utilities are used for repository tooling. The public CMake
target is `aowis::config` (`add_subdirectory` or installed `AOWISConfig`
package). See [library API](docs/library-api.md).

## Repository tooling (implemented)

### Apply source patches

```bash
cd ~/coding/aowis/AOWIS-CONFIG
./tools/apply_patch.sh ~/Downloads/aowis-config-description.zip
```

Patches must have a filename `aowis-config-*.zip` and all members must live
under the `AOWIS-CONFIG/` archive prefix. Replacements and additions are
supported. Optional deletion manifest:
`AOWIS-CONFIG/.aowis-patch-delete.txt` (one project-relative file path per
line). The script validates entries, rejects unsafe paths and symlinks, updates
file timestamps, and refreshes `../config.zip` after applying changes. File
replacement is atomic **per file**, not across an entire patch. It does not
stage or commit Git changes.

### Create or refresh the source snapshot

```bash
cd ~/coding/aowis/AOWIS-CONFIG
./tools/zip_config_project.sh
```

This creates `~/coding/aowis/config.zip` from the repository source, with an
`AOWIS-CONFIG/` top-level entry. Git metadata, build outputs, caches, and
temporary files are excluded. The old snapshot is retained if creation fails.

### Run tooling tests

```bash
python3 tests/repository_tooling_test.py
```

The repository tooling has nine standalone regression tests; the C++ library
has its own CTest suite. Tooling requires Bash, Python 3, `zip`, and `unzip`.

## Source policy

License: Apache-2.0 (see `LICENSE`). C++ core: C++20 with no mandatory
Qt, DVMDB, or AOWIS-NETWORK dependencies. Follow the AOWIS source conventions:
do not introduce `auto` for C++ local declarations, and use `this->` when
accessing class members.
