# Architecture and storage requirements

Status: **Phase 3 WireGuard native persistence implemented; Phase 4 consumer integration pending**. Updated: 2026-11-10. For executable examples see [Quickstart](quickstart.md); for GUI wiring see [Consumer integration](integration-guide.md).

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
│       └── <profile-uuid>.json  # private single-record envelope; metadata + imported text
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
platform-specific path logic or passed as an intentional override. The component ID must
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

Deleting a profile that is still referenced must fail; Phase 3 requires an
authoritative reference-count callback and fails closed when unavailable.
Renaming does not affect references. Phase 4 must provide the referrer list
for the GUI and coordinate cross-collection changes to close reference/delete
races; a callback alone is not an atomic transaction.

## 4. WireGuard as the first complete use case

### Managed profile storage

An imported WireGuard file is copied into the owner's managed
configuration collection as **one private atomic JSON object**:

```text
<owner>/wireguard/<uuidv7>.json
```

This supersedes the earlier illustrative two-file `profile.json` plus
`tunnel.conf` layout: a single atomic record avoids partial updates and crash
recovery ambiguity between two files. Its payload contains profile
metadata and the complete imported `.conf` text in a lossless JSON string,
including comments, whitespace and private key material. It is **not** a
path reference to the import source and **not** encryption. The exact
WireGuard payload schema, import and codec are implemented in **Phase 3**;
see [wireguard-profiles.md](wireguard-profiles.md). The generic envelope and
filesystem guarantees are documented in [storage-api.md](storage-api.md).

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

- Secret-containing files (including private JSON profile records): mode `0600`.
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
control. Phase 2 implements one atomic JSON record per object, so a
metadata-plus-secret payload can be saved or deleted together, not as two
independently mutable files. This intentionally does **not** guarantee atomic
changes across multiple object IDs or collections. Such use cases need a
separately designed cross-record transaction protocol.

Two instances of **the same** application may still write the same owner root.
Use locking and conflict detection; do not assume unique process access simply
because application roots are separate. Avoid silent last-writer-wins where it
could lose user changes. Test power-loss/interrupted-write and multi-process
conflict cases where practical.

Do not mix transient status into durable configuration files. Active VPN
consumers, handshake age, byte counters and other live metrics belong in
memory in the network/runtime subsystem and flow into the GUI via an explicit
status interface.

## 6. Implemented core and future integration boundaries

```text
AOWIS applications/services (own their domain schemas and references)
  ├── AOWIS-CONFIG 0.4.0 (native C++20 library, aowis::config)
  │     ├── OwnerId + ConfigRoot (owner-scoped path resolution)
  │     ├── ObjectCollection (JSON records, UUIDv7, revisions, POSIX I/O)
  │     ├── WireGuardProfiles (text import/CRUD/validation, reference hook)
  │     └── Result<T> / ErrorCode (checked failure handling)
  ├── Future consumer-specific preferences, connections and unit schemas
  └── AOWIS-NETWORK (WireGuard tunnels and live diagnostics)
```

The native core actually exposes `ObjectCollection::{open,list,read,create,
update,erase}`, `WireGuardProfiles::{open,list,read,importText,update,erase,
exportText}`, UUIDv7 and validation APIs. See [Library API](library-api.md),
[Storage API](storage-api.md) and [WireGuard profiles](wireguard-profiles.md)
for the verified C++ contracts. A generic preferences API, file watchers,
change notifications, cross-collection transactions, native Windows storage,
and browser storage are **not** implemented. Do not implement or assume a
hypothetical `resolveReferences` API: the deletion reference-count callback
is supplied by the consuming application's catalogue.

Platform adapters may be introduced later without imposing Qt on server
components. The GUI consumes the library; it does not own its storage logic.

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
competing stores. This is a **separate integration phase**: native storage and WireGuard
profiles are implemented here, but this repository does not change the GUI.
