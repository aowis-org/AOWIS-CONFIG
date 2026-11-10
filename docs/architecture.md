# Architecture and storage requirements

Status: **target design, not implemented**. Updated: 2026-11-10.

## 1. Purpose and responsibility

AOWIS-CONFIG supplies a reusable, native C++20 **configuration infrastructure**:
platform-aware configuration roots, validation, serializers, atomic storage,
structured object collections, stable identities and references, error
reporting, and (as needed) change notification. The applications own their
schemas where the meaning is application-specific. An optional Qt adapter may
be provided, but the core does not require Qt, DVMDB, MAP, or AOWIS-NETWORK.

This is **one library shared by source/dependency**, not **one shared physical
store**. AOWIS-GUI, AOWIS-GUI-Mobile, and independently deployed services each
own their persistent configurations. A shared schema or library implementation
does not create cross-application write access.

AOWIS-CONFIG should be integrated as a version-pinned Git submodule by the
applications/services that own persistent configuration; it should also build
standalone with its own CMake tests. Library components that merely consume
configuration, e.g. AOWIS-GUI-Render or AOWIS-SERVER-MODEL, should receive
values from callers rather than embedding a settings system of their own.

## 2. Storage ownership and paths

### Native user-scoped layout

Illustrative structure, with directories created lazily:

```text
${XDG_CONFIG_HOME:-~/.config}/aowis/
├── gui/
│   ├── preferences.json          # preferences representation TBD
│   ├── connections/
│   │   └── <connection-uuid>.json
│   ├── unit-profiles/
│   │   └── <profile-uuid>.json
│   └── wireguard/
│       └── <profile-uuid>/
│           ├── profile.json      # nonsecret name, description, ID
│           └── tunnel.conf       # full imported WireGuard text; secret
├── gui-mobile/
│   └── ...
├── server-map/
│   └── ...
├── server-dvmdb/
│   └── ...
├── server-hydrology/
│   └── ...
└── server-epanet/
    └── ...
```

The same schema can be used in more than one owner's root. For example, both
GUI and Mobile may store their *own* unit profiles and server connection
objects. Their stores are not automatically the same store. Other components
can use different collections without creating empty directories for all
hypothetical AOWIS modules.

The root is explicit in the storage API: resolved from a known component ID and
platform adapter or passed as an intentional override. The component ID must
be validated; arbitrary user text must not become a path. A service deployed
under systemd may instead read from `/etc/aowis/server-dvmdb/` (or another
explicit root), without depending on a GUI user's `HOME`. Consider read-only
service configuration and writable service state separately.

### Configuration is not application data

`~/.config/aowis/<component>/` holds portable configuration. Persistent
DVMDB repositories, working-copy data, tile data, caches, log files, and other
operational data do not belong there merely to be bundled by a settings copy.
Linux data, state, cache, and runtime paths should follow their relevant XDG
conventions. Do not break application-data roots by moving them indiscriminately.

### Portability invariant

Copying an owner's complete configuration directory should reproduce its
configured preferences, connection definitions, unit profiles, and imported
WireGuard profiles (including contained private keys), with no hidden
reference to the original `.conf` path. Internal references use stable IDs.

This **does not** guarantee that a remote endpoint remains reachable, that an
external command or path embedded in a WireGuard file is portable, that the
receiving platform supports the same capabilities, or that a database's
actual project data has been copied. The importer must detect or clearly warn
about external dependencies/unsupported directives; it must never silently
rewrite or drop their text.

## 3. Object and reference model

### Object identity

Use a distinct, stable ID for each AOWIS-owned configuration object, normally
UUIDv7 for new connections and profiles. Filenames derived from IDs are storage
details, not public references. Human-readable names may be changed without
rewriting consumers. Explicit `schema_version` supports validation and clear
unsupported-version errors, not automatic migration guarantees.

**Connection identity is different from a remote server's identity.** AOWIS may
have two configured endpoint definitions that reach the same DVMDB repository
using different URLs or transports. A connection entry therefore needs its
own `connection_id`, while `repository_id` (if known) is learned from the
remote repository. Neither is the WireGuard profile ID.

### Example connection definition (illustrative schema)

```json
{
  "schema_version": 1,
  "id": "<connection-uuidv7>",
  "name": "Office DVMDB",
  "service_type": "dvmdb",
  "endpoint": "http://10.8.0.10:8755",
  "repository_id": "<remote-dvmdb-repository-uuidv7-if-known>",
  "transport": {
    "type": "wireguard",
    "profile_id": "<wireguard-profile-uuidv7>"
  }
}
```

This JSON is a **design illustration**, not a frozen schema or implemented
serializer. `repository_id` may be absent when the repository has not yet been
probed. Direct connections can use a distinct transport type. Service-specific
fields can be defined by the owning component without teaching the storage
engine all DVMDB/MAP semantics.

### Reference resolution

References to another configuration object are resolved inside the current
owner's store unless the application explicitly imports/exports objects. The
storage machinery should expose typed lookup and referential-integrity helpers.
Do not persist derived values like "Used by 3 services" in the WireGuard
metadata: calculate it from referencing connection records.

Deleting a profile that is still referenced must fail with a useful list of
referrers. Renaming must not affect references. Concurrent changes and deletion
races need a defined policy before persistence is considered production-ready.

## 4. WireGuard as the first complete use case

### Managed profile storage

An imported WireGuard file is copied into the owner's configuration tree:

```text
<owner>/wireguard/<uuidv7>/profile.json
<owner>/wireguard/<uuidv7>/tunnel.conf
```

The metadata contains only the reusable identity and descriptive fields:

```json
{
  "schema_version": 1,
  "id": "<wireguard-profile-uuidv7>",
  "name": "Office VPN",
  "description": "Access to the internal infrastructure"
}
```

`tunnel.conf` contains the **complete** original text: `[Interface]`,
`[Peer]`, directives, comments, whitespace, and private key material. Import
reads the selected external `.conf`, validates/report issues, writes managed
content, and **does not retain the import path as the profile's authority**.
After import, edit and delete refer only to the stable profile ID.

AOWIS-GUI's existing WireGuard editor is a `QPlainTextEdit` with
`QSyntaxHighlighter` and a WASM-safe UI approach. Its currently implemented
in-memory GUI must later be connected to this library; the existence of the
editor does not mean CONFIG persistence has already been implemented.

### Configured usage versus running status

The settings entry is called **WireGuard VPN**. The list should show name,
configured usage ("Used by N services"), and live runtime state. Details
provide three views:

- **Configuration:** import/create/edit/delete, metadata, syntax-highlighted
  raw `.conf`, save/revert, validation, unsaved-change handling.
- **Used by:** enumerate configured service connections referencing this ID;
  allow navigation to their inspectors/settings. Counts are static references,
  not live sockets or tunnels.
- **Status:** running/starting/stopped/error, current active consumers,
  received/transmitted bytes, peers, endpoints, last handshakes, and error
  diagnostics supplied by AOWIS-NETWORK. Do not fabricate live data from stored
  configuration. A running WireGuard interface does not prove peer reachability.

One profile can be referenced by multiple service connections. Whether they
share one running tunnel and when it starts/stops is an AOWIS-NETWORK lifecycle
policy, not a storage mechanism. CONFIG must not include a tunnel engine.

In WebAssembly the `WireGuard VPN` settings navigation item is intentionally
**visible but disabled/greyed out**, with a tooltip explaining why native
UDP/TUN-based embedded WireGuard networking is unavailable in browsers.
External VPN-provided connectivity is separate from an embedded VPN client.

## 5. Secrets, correctness, and failure handling

### Secret-bearing profiles

A WireGuard `PrivateKey` is a secret. Current agreed first design favors
**managed protected files**, not required OS-keychain references, so a copied
configuration directory remains complete. On Linux:

- Secret-containing files (such as `tunnel.conf`): mode `0600`.
- Secret-containing directories and sensitive ancestors under application
  control: mode `0700`.
- Reject symlink/path-traversal attacks and ambiguous IDs/paths; do not follow
  untrusted external file targets while writing.
- Validate before installing; use same-filesystem temporary files, atomic
  rename, and durability steps appropriate to the OS; do not expose temporary
  files with weak permissions.
- Never write private keys/API credentials into logging, error dumps, test
  failure output, or nonsecret metadata.
- Treat backup/export archives as **secret-containing**; neither mode bits nor
  filesystem permissions amount to encryption at rest.

Do not promise OS-keychain encryption or seamless transfer of non-exportable
credentials. Such features would change the portability contract and require an
explicit additional design.

### Mutability and concurrency

The object collections are mutable, but their owning application remains in
control. Atomic replacement of a single file is not equivalent to a durable
multi-file transaction. For metadata-plus-secret updates and deletions, the
library needs a designed crash/restart recovery strategy (e.g. a serialized
bundle or a transaction journal) before claiming all-or-nothing behavior.

Two instances of **the same** application may still write the same owner root.
Use locking and conflict detection; do not assume unique process access simply
because application roots are separate. Avoid silent last-writer-wins where it
could lose user changes. Test power-loss/interrupted-write and multi-process
conflict cases where practical.

Do not mix transient status into durable configuration files. Active VPN
consumers, handshake age, byte counters and other live metrics belong in
memory in the network/runtime subsystem and flow into the GUI via an explicit
status interface.

## 6. Core API boundaries (conceptual)

The **shape** of the implementation is agreed, not exact C++ signatures:

```text
AOWIS applications/services
  ├── component-owned schemas (connection, map settings, units...)
  ├── AOWIS-CONFIG native C++20 facade
  │     ├── owner-root resolution / platform storage adapters
  │     ├── preferences (representation TBD)
  │     ├── JSON object collections and stable IDs
  │     ├── secure blob storage for imported .conf text
  │     ├── validation, errors, conflicts and notifications
  │     └── atomic I/O and recovery
  └── AOWIS-NETWORK runtime (for tunnels and diagnostics)
```

Potential operations include `list`, `read`, `create`, `update`, `delete`,
`import`, `resolveReferences`, and (possibly) `watch`. The precise naming,
errors, async model and CMake targets are pending. Core code should not require
Qt types in public signatures. Optional adapters may bridge Qt and WASM/browser
storage where appropriate without forcing them on server binaries.

## 7. Existing experimental GUI storage to replace

The current GUI snapshot uses:

- `aowis-server-gui.ini` under a legacy user-data directory.
- Scalar settings in INI (`interface`, `map_navigation`, `map_performance`,
  `shortcuts`, `symbology`).
- A large escaped JSON array in `[servers] profiles=...`.
- Qt-encoded JSON/byte-array unit profiles in `[units] custom_profiles=...`.
- A `wireguard_profile_path` referencing a file *outside* the managed tree.
- Plaintext map server API credentials alongside ordinary settings.
- Different native/browser persistence paths for some GUI settings.

We explicitly **do not** need backward-compatible readers or silent conversion
of these experimental formats. When the owning integration phase arrives,
remove legacy readers, writers, dead GUI code and old tests along with their
replacement. Keep tests verifying new behavior rather than shipping two
competing stores. This is a **separate integration phase**; this repo currently
contains no GUI changes or storage implementation.
