# Architecture decisions and open questions

Status: **architecture decisions; Phase 3 WireGuard native storage added**. Updated: 2026-11-10.

This is the authoritative short record of decisions established during the
initial implementation of AOWIS-CONFIG. It distinguishes policy from features
that have not yet been integrated into consuming applications. See [architecture.md](architecture.md) for implications,
[roadmap.md](roadmap.md) for the order of implementation and
[integration-guide.md](integration-guide.md) for consumer responsibilities.

## Agreed decisions

| ID | Decision | Rationale / consequence |
| --- | --- | --- |
| D01 | Create independent `AOWIS-CONFIG` repo and reusable C++20 library | One storage machinery implementation reused across applications and services |
| D02 | No mandatory Qt dependency in the core | Native services must not pull in GUI frameworks merely to load settings; optional Qt integration may exist separately |
| D03 | No universal mutable `aowis/shared/` directory | Independent apps should not overwrite, delete, or silently upgrade each other's configurations |
| D04 | Component-specific roots inside an AOWIS namespace | E.g. `~/.config/aowis/gui/` and `~/.config/aowis/server-map/` |
| D05 | Common library and schemas do not imply shared stored instances | Each owner has its own profiles and references; explicit import/export for movement between owners |
| D06 | Only persistent-config owners use the library directly | Renderers, MODEL, and other pure libraries consume supplied values |
| D07 | Reusable objects have stable IDs and typed schemas | Rename should not break references; use UUIDv7 for newly generated AOWIS-owned profile and connection IDs |
| D08 | WireGuard `.conf` contents are **imported**, not referenced by external path | Self-contained owned storage; path of source file discarded after import |
| D09 | WireGuard profile can be reused by multiple configured server/service connections within its owner | One profile, many referencing configurations; no duplication of secrets |
| D10 | Store original WireGuard configuration text, with comments and formatting | Text editor is authoritative; distinguish parse validity from runtime support |
| D11 | Keep WireGuard configuration and private key inside the owner's managed configuration tree | Copying the tree copies the imported profile; no mandatory external keychain |
| D12 | Secret-containing files require strict permissions, atomic writes, and log redaction | Portability without claiming encryption at rest |
| D13 | WireGuard runtime is AOWIS-NETWORK's responsibility | AOWIS-CONFIG does not start tunnels, compute handshakes, or persist live traffic counters |
| D14 | WireGuard UI entry is `WireGuard VPN` | Disabled/greyed with explanatory tooltip in WebAssembly, where embedded tunnel networking is unavailable |
| D15 | WireGuard UI provides CRUD, import, syntax-highlighted text editor, `Used by`, and `Status` | Configuration references are separate from runtime consumers/handshake status |
| D16 | No legacy compatibility/migration is required for the current experimental installations | Replace JSON-in-INI and external WireGuard path mechanisms directly; remove old storage code |
| D17 | Configuration portability is a design requirement | No hidden absolute dependencies inside managed profile references; transfer by copying the owner's configuration tree, subject to external endpoint and operational-data caveats |
| D18 | System services are not forced into desktop-user configuration directories | Allow explicit system-managed roots such as `/etc/aowis/server-dvmdb/` |
| D19 | Separate configured connection identity from remote repository/service identity | Two connection definitions may point to the same repository through different endpoints/transports |
| D20 | Include schema version in structured objects | Validate and reject unsupported versions; this is not a commitment to maintain legacy readers |
| D21 | Phase 1 API uses `aowis::config`, CMake target `aowis::config`, and owner-scoped `ConfigRoot` | Native C++20 without Qt; explicit roots for system services, no implicit shared mutable directory |
| D22 | Keep the core free of mandatory JSON dependencies | Phase 2 validates JSON using a small internal parser; applications own payload schemas |
| D23 | Store each object as one private, atomic JSON envelope | Metadata and future imported WG text can update together; no two-file transaction ambiguity |
| D24 | Native POSIX concurrency: collection `flock` plus expected revision tokens | Cooperating instances serialize I/O and reject stale edits/deletes |
| D25 | Single-record durability scope, no multi-object transaction guarantee | `fsync` file, atomic publish, `fsync` directory; cross-record transactions require a separate design |
| D26 | Reject insecure paths, symlinks and file permissions | Private 0700 owner/collection directories and 0600 files; no encryption implied |
| D27 | WireGuard typed profile records use versioned JSON, with complete escaped `.conf` text in one record | Exact text, metadata and secrets update together; import path is not retained |
| D28 | WireGuard validation distinguishes blocking structural errors from portability/runtime warnings | Storage preserves original directives; AOWIS-NETWORK must independently validate activation safety |
| D29 | Profile deletion requires a reference-count callback and rejects missing/failed lookups | Avoids ungated UI deletes; cross-collection reference/create races still require GUI coordination |

## Clarifications that matter

**Sharing:** AOWIS-GUI can share *one* WireGuard profile across its own DVMDB,
map, and other server connection definitions. That is **not** the same as
sharing on-disk profile files with AOWIS-GUI-Mobile, system servers, or other
processes. If users need to move an object between applications, export/import
is preferable to two owners editing the same file.

**"Used by N services":** The count is of **configured AOWIS connection
entries** that reference the profile, independent of reachability. It is not
the number of live TCP/HTTP connections, tunnel instances, or peer handshakes.
The UI must separately expose runtime usage and WireGuard tunnel diagnostics.

**WireGuard tunnel state:** WireGuard is connectionless. A running interface is
not proof that a peer is reachable. Show `Starting`, `Running`, `Stopped`, or
`Error` separately from handshake age and traffic counters. Multiple configured
connections may share one running tunnel; lifecycle/refcounting belongs to
AOWIS-NETWORK, not to the config store.

**Breaking changes:** No compatibility shims, dual writes, or old-format
migration framework for the current pre-production data. This does not remove
the need for safe atomic writes, schema validation, backups, or test isolation.

## Design questions intentionally not settled yet

Do **not** silently invent answers to these while implementing an unrelated
phase. Resolve them explicitly when that phase begins.

1. **Preferences file format:** versioned JSON for a uniform cross-platform
   core versus continuing small native INI/QSettings preferences behind a
   generic adapter. Structured profiles are JSON regardless; the diagram's
   `preferences.json` is an *example*, not a ratified final format.
2. **API after Phase 2:** synchronous native object collections now exist;
   change notifications, watchers and optional async façade remain undecided.
3. **Additional concurrency:** application-level reference integrity across
   multiple collections and optional change notifications are still needed.
4. **Cross-record transactions:** single record atomics are implemented;
   if one operation must change several records, a journal or different
   transaction design must be explicitly added.
5. **Paths on Windows, Android, and WASM:** platform adapters and browser
   persistence where appropriate, without weakening ownership boundaries.
6. **WireGuard runtime validation:** Phase 3 stores the text intact and
   reports structural errors vs runtime-portability warnings; AOWIS-NETWORK
   must define the actual supported directives and enforcement at activation.
7. **Protected secret files:** key-rotation/permission/error behavior and
   optional encryption/export are separate decisions; *do not make a keychain
   mandatory* or silently pretend filesystem permissions equal encryption.
8. **Cross-application profile transfer:** optional import/export UX and
   collision handling when a copied object ID already exists at destination.
9. **Host/connection schema evolution:** generic shared definitions versus
   service-owned extension fields, without coupling CONFIG to DVMDB or MAP.
10. **System-wide policy:** installation/deployment model and ownership for
    `/etc/aowis/<component>/`; avoid assuming an ordinary user can write there.

## Out of scope for AOWIS-CONFIG

- DVMDB repositories, network models, revisions, personal working copies, or
  the DVMDB storage engine.
- WireGuard UDP/TUN implementations, sockets, routing, handshake logic, or
  network health monitoring (AOWIS-NETWORK owns these).
- A Qt UI framework, dialogs, text editors, renderer state, or map scenes.
- Automatically synchronized profile storage across multiple AOWIS apps.
- Compatibility with the experimental pre-production GUI INI format.
