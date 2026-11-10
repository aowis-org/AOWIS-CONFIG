# Implementation roadmap

Status: **Phase 3 WireGuard profiles implemented; Phase 4 next**. Updated: 2026-11-10. The [integration guide](integration-guide.md) and [verification matrix](testing.md) describe the handoff and current evidence.

This roadmap is written for **AOWIS-CONFIG**. It distinguishes the reusable
library from integrations into other repositories. Scope and turn counts are
planning aids, not promises. A phase is complete only after its code, tests,
and documentation have been reviewed and its acceptance conditions met.

## Current position

| Stage | Status | Deliverable |
| --- | --- | --- |
| 0. Repository tooling | **Complete** | `apply_patch.sh`, `zip_config_project.sh`, tooling tests |
| 1. Library skeleton | **Complete** | C++20 library, CMake, public API baseline, compile scripts, tests |
| 2. Persistence primitives | **Complete (native POSIX)** | Owner-scoped roots, JSON envelopes, atomic I/O, validation, concurrency/recovery |
| 3. WireGuard object storage | **Complete (native POSIX)** | Import, metadata, lossless managed text, CRUD, ID references |
| 4. GUI WireGuard integration | **Next** | Persistent GUI profiles; connection references; runtime diagnostics |
| 5. GUI connection / unit / preference consolidation | Not started | Remove JSON/Qt blobs from INI and retire legacy storage |
| 6. Other owner integrations | Not started | Mobile/server components opt in as needed |
| 7. Cross-platform hardening | Not started | Native + supported WASM adapters, permissions, portability testing |

The separate AOWIS-GUI repository **already has an experimental WireGuard VPN
settings UI and syntax highlighter**. Its current session-only GUI profiles
are *not* integrated with the completed Phase 3 persistence API. Its usage
and status views are not connected to actual configuration/runtime APIs.

## Phase 0 — tooling (done)

- [x] Define `aowis-config-*.zip` patch conventions.
- [x] Implement validating `tools/apply_patch.sh` and source snapshot refresh.
- [x] Implement `tools/zip_config_project.sh` producing `../config.zip`.
- [x] Add disposable-checkout regression tests.
- [x] Document the architecture, roadmap, decision log and handoff.

## Phase 1 — independent C++20 foundation

- [x] Establish CMake project, reusable build target, test target, install and
      submodule consumption model; Apache-2.0 license already present.
- [x] Add `compile_linux.sh` and `compile_linux_tests.sh` with clear, minimal
      dependencies and deterministic exits on test failure.
- [x] Define root namespace, explicit config-owner identifier, error model,
      result types, and minimal public headers.
- [x] Decide whether core includes JSON serialization via a small dependency
      or uses caller-provided serializers; avoid Qt in public/core interfaces.
- [x] Add smoke integration for a second standalone consumer, without forcing
      coupling to GUI, DVMDB, or AOWIS-NETWORK.
- [x] Document CMake targets and examples.

**Acceptance verified in the patch build:** Native library and 2 CTests pass
without Qt/DVMDB; independent link consumer passes. Installed-package CMake
export is exercised separately. No external services are required. See
[library-api.md](library-api.md).

## Phase 2 — storage engine and object collections

Native POSIX implementation completed. The chosen single-record envelope
avoids the need for multi-file `profile.json` + `tunnel.conf` transactions.
Cross-record transactions and reference integrity remain explicitly out of
scope until a consumer requires them. See [storage-api.md](storage-api.md).

- [x] Validate owner paths, collection names and UUIDv7 identities.
- [x] Define single-record JSON envelope (version, ID, revision, payload) and
      strict JSON syntax/UTF-8 validation.
- [x] Implement UUIDv7 generation, listing, reading, create, update, delete.
- [x] Enforce 0700 directories and 0600 files, reject symlinks and unsafe paths.
- [x] Implement temporary-file fsync, atomic publication and directory fsync;
      remove orphan temporary files on restart/open.
- [x] Implement cross-process collection locks and optimistic revisions to
      reject stale updates/deletes.
- [x] Test corruption, unsupported versions, permissions, invalid JSON,
      relocation/copy, conflicting edits and injected process termination.
- [ ] Test true power-loss durability on supported filesystems (Phase 7).
- [ ] Implement Windows and browser backends behind the backend contract
      (Android uses the POSIX backend with an explicit root).

**Acceptance on native POSIX:** Each object is a complete, durable record under
normal POSIX fsync/rename guarantees. `SIGKILL` injection and restart recovery
are tested, but are not equivalent to a physical power cut. Failures after
publication may have an uncertain outcome and must be followed by re-read.

## Phase 3 — WireGuard profiles and secret-bearing import

Implemented on native POSIX using the Phase 2 atomic object store. See
[wireguard-profiles.md](wireguard-profiles.md) for exact API and validation
scope, including limitations of cross-collection reference guarding.

- [x] Define versioned typed profile payload and lossless `.conf` JSON codec.
- [x] Import text into owner storage; never retain original import path.
- [x] Preserve original comments, whitespace, CRLF/LF and private keys.
- [x] Validate structure, warn about unknown/wg-quick/external directives.
- [x] Implement list/read/create/edit/rename/delete/export by stable UUIDv7.
- [x] Require a consumer-provided reference count lookup before deletion;
      reject missing/failed lookups and profiles with configured consumers.
- [x] Document copy/relocation, protected files, runtime separation and limits.
- [x] Test reopening edited profiles, copying roots, redacted diagnostics,
      revision conflicts and reference guards.
- [ ] Close cross-collection deletion/reference-creation races in the owning
      GUI catalogue (Phase 4; not solved by a callback alone).
- [ ] Windows, Android and WASM/platform storage hardening (Phase 7).

**Acceptance:** Native imported profiles survive restart and owner-root copy
without external source paths. Profiles with references cannot be deleted
when the authoritative reference callback returns nonzero; an absent or
failed callback fails closed. Structural validation blocks malformed text;
portability warnings remain visible but do not rewrite stored configuration.
No live tunnel status is stored by CONFIG.

### Phase 3 hardening (v0.5.0, pre-Phase 4 review) — done

- [x] Accept symlinked ancestors above the owner directory; keep `O_NOFOLLOW`
      and privacy checks from the owner directory down.
- [x] `list()`/`readAll()` report damaged entries in `unreadable` instead of
      failing; `eraseCorrupt()` / `eraseUnreadable()` remove them safely.
- [x] Single locked pass for `WireGuardProfiles::list()`.
- [x] Lenient WireGuard reads (`WireGuardProfile::validation`); strict writes.
- [x] wg(8)-compatible case-insensitive keys/sections and `#` comments; no
      external-reference warning for base64 keys starting with `/`.
- [x] Time-limited collection locks (`lock_timeout`); orphan recovery only on
      `open()` and writes.
- [x] Pluggable storage backend interface (v0.5.0): private contract,
      native POSIX and in-memory backends, shared contract tests; Windows and
      browser use a placeholder returning `unsupported_platform`.
- [ ] Browser backend **before or with Phase 5** (GUI settings must persist
      in WASM); Windows backend before Phase 4 ships on the Standalone build.

## Phase 4 — AOWIS-GUI integration (separate repository)

- [ ] Pin `AOWIS-CONFIG` as a submodule in AOWIS-GUI and link the native core.
- [ ] Replace the WireGuard VPN settings widget's session-only profile store
      with the owner-scoped persistent store.
- [ ] Replace `wireguard_profile_path` in connection records with stable
      profile IDs and a transport specification.
- [ ] Bind configured `Used by N services` to the GUI's actual catalog of
      DVMDB, map and subsequent server connection definitions.
- [ ] Integrate the runtime `Status` view with AOWIS-NETWORK, not CONFIG:
      state, consumers, peer handshakes, traffic, errors.
- [ ] Handle edit/delete while active, save failures, unavailable profiles,
      and reference conflicts with useful GUI diagnostics.
- [ ] Preserve the agreed disabled `WireGuard VPN` entry with tooltip in WASM;
      do not claim embedded WG networking in browsers.
- [ ] Remove now-obsolete transient storage/adapters and dead code.
- [ ] Add native GUI integration and regression tests; run full GUI CTest.

**Acceptance:** Imported profile is persistent; multiple configured services
can reference one profile by stable ID; deleting a referenced profile is
blocked; real runtime status is from the network engine; no legacy file path
is needed.

## Phase 5 — other GUI configuration and cleanup

- [ ] Replace `[servers] profiles="[...]"` with independent structured
      connection objects; separate connection identity from remote repository
      UUID and support different service types.
- [ ] Replace `[units] custom_profiles=@ByteArray(...)` with versioned unit
      profile records; make storage shared in *code*, not disk ownership.
- [ ] Resolve simple preference format (INI adapter vs JSON) and store under
      `aowis/gui`, not legacy `aowis-server-gui` paths.
- [ ] Move map API secrets out of ordinary preference fields, maintaining a
      portable protected-storage policy.
- [ ] Remove legacy readers, dual writes, import converters, migration tests,
      unused settings objects, and outdated documents. No production
      installation requires legacy continuity.
- [ ] Validate that UI language/theme changes still apply without restart and
      that consumers react to configuration changes rather than stale caches.

**Acceptance:** AOWIS-GUI has one authoritative configuration mechanism for
each concept; all historical compatibility paths are deleted.

## Phase 6 — other AOWIS owners, deliberately

Integrate one component at a time only where it owns persistent configuration:

- [ ] AOWIS-GUI-Mobile: independently owned preferences and profiles.
- [ ] AOWIS-SERVER-MAP: service config with user/system deployment roots.
- [ ] AOWIS-SERVER-DVMDB: service config; keep repository/data storage separate.
- [ ] AOWIS-SERVER-HYDROLOGY, EPANET and other persistent services as needed.
- [ ] Standalone: define which owning component's config it embeds/starts; do
      not silently merge owners' physical stores.

Do not add config-file I/O to AOWIS-GUI-Render or AOWIS-SERVER-MODEL merely
because they are AOWIS projects. Pure libraries receive settings from callers.

**Acceptance:** Independent components do not collide in the filesystem and
share the same tested implementation without new ad-hoc format parsers.

## Phase 7 — platforms and robustness

- [ ] Windows and Android native storage roots and permission behavior.
- [ ] Explicit, limited WASM/browser adapter for *supported* non-WireGuard
      settings; appropriate browser persistence/host-provided config.
- [ ] Copy/restore exercises across fresh machines/processes and relocation
      to a different home directory.
- [ ] Fault injection, multi-process tests and security review across the
      supported filesystems and platforms.
- [x] Native developer quickstart, integration guide, API references, and
      operations/troubleshooting documentation (documentation-only pass).
- [ ] Per-platform deployment docs and release checklist once those backends
      exist.

**Acceptance:** Portability/security claims are backed by platform-specific
checks; unsupported features report limitations clearly.

## Implementation discipline

- Each turn should deliver a small, reviewable `aowis-config-*.zip` patch,
  tests, documented acceptance, commands and a copyable Git commit command.
- Run the AOWIS-CONFIG test suite after each change. Run downstream suites
  when integration changes their behavior.
- Don't call a proposed feature implemented merely because a header,
  placeholder widget or mock status value exists.
- No `auto` in new C++ code; use `this->` for class member access, following
  the AOWIS conventions.
- Keep the roadmap updated after each phase. Remove superseded proposals rather
  than accumulating contradictory historical design documents.
