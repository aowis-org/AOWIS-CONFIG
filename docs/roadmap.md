# Implementation roadmap

Status: **Phase 1 implemented; Phase 2 next**. Updated: 2026-11-10.

This roadmap is written for **AOWIS-CONFIG**. It distinguishes the reusable
library from integrations into other repositories. Scope and turn counts are
planning aids, not promises. A phase is complete only after its code, tests,
and documentation have been reviewed and its acceptance conditions met.

## Current position

| Stage | Status | Deliverable |
| --- | --- | --- |
| 0. Repository tooling | **Complete** | `apply_patch.sh`, `zip_config_project.sh`, tooling tests |
| 1. Library skeleton | **Complete** | C++20 library, CMake, public API baseline, compile scripts, tests |
| 2. Persistence primitives | **Next** | Owner-scoped roots, JSON, atomic I/O, validation, concurrency/recovery |
| 3. WireGuard object storage | Not started | Import, metadata, secure managed text, CRUD, ID references |
| 4. GUI WireGuard integration | Not started | Persistent GUI profiles; connection references; runtime diagnostics |
| 5. GUI connection / unit / preference consolidation | Not started | Remove JSON/Qt blobs from INI and retire legacy storage |
| 6. Other owner integrations | Not started | Mobile/server components opt in as needed |
| 7. Cross-platform hardening | Not started | Native + supported WASM adapters, permissions, portability testing |

The separate AOWIS-GUI repository **already has an experimental WireGuard VPN
settings UI and syntax highlighter**. That does *not* count as Stage 3 or 4,
because its profile objects currently have session-only storage and its usage
and status views are not connected to the final configuration/runtime APIs.

## Phase 0 — tooling (done)

- [x] Define `aowis-config-*.zip` patch conventions.
- [x] Implement validating `tools/apply_patch.sh` and source snapshot refresh.
- [x] Implement `tools/zip_config_project.sh` producing `../config.zip`.
- [x] Add disposable-checkout regression tests.
- [x] Document the planned library architecture and roadmap (this patch).

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

Phase 1 supplies **path resolution only**, not secure directory access. Before
storing credentials, the next phase must establish a safe file-opening/locking
and recovery design. Code that merely returns a `ConfigRoot` is not a secure
filesystem backend.

- [ ] Resolve the owner-scoped native config root from platform conventions or
      an explicit override; enforce valid owner IDs and path containment.
- [ ] Define durable file layout, JSON schema/version checks, structured
      errors, object listing, reading, writing and deletion.
- [ ] Generate stable UUIDv7 IDs for new objects; separate storage path from ID.
- [ ] Implement restrictive permissions for secret-bearing files/directories,
      symlink protection, and error-safe atomic replacement.
- [ ] Define and implement concurrency/conflict semantics for multiple
      processes sharing the **same owner** root.
- [ ] Establish crash-recovery behavior for operations involving multiple
      files, including create/update/delete of metadata plus secret payload.
- [ ] Test corruption, unsupported schemas, power interruption/failure
      injection, conflicting edits, permissions, path attacks and idempotent
      cleanup.

**Acceptance:** Atomicity/durability claims have tests; existing objects never
silently disappear or lose updates; errors are observable and actionable.

## Phase 3 — WireGuard profiles and secret-bearing import

- [ ] Finalize profile metadata schema and managed raw `.conf` blob API.
- [ ] Import `.conf` text into owner storage; discard original file path.
- [ ] Preserve original comments/whitespace and private keys exactly; never
      convert it into a lossy field-by-field serialization.
- [ ] Validate known file structure and report unsupported directives and
      nonportable external file/script references without silently removing
      them.
- [ ] Implement create/list/edit/rename/delete/read by UUIDv7 ID.
- [ ] Forbid deletion while referenced (based on consumer-provided catalogue
      or typed reference resolver); avoid hard-coded DVMDB-only assumptions.
- [ ] Define export/copy semantics for self-contained owner configurations.
- [ ] Test a fresh instance re-opening imported and edited profiles; ensure
      copied owner roots do not depend on the original import location.
- [ ] Assert private material absent from logs, errors and plaintext metadata;
      enforce the documented filesystem protection model.

**Acceptance:** A managed WireGuard profile survives restart and a copied
owner root contains the complete stored configuration, without an external
`.conf` file being required.

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
- [ ] Comprehensive public API docs, deployment docs and release checklist.

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
