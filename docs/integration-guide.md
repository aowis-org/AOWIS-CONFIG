# Integration guide — an AOWIS component as configuration owner

**Target audience:** developers integrating AOWIS-CONFIG into AOWIS-GUI or a
standalone AOWIS service. **Implemented library API:** v0.4.0 on native POSIX.
**Planned GUI wiring:** Phase 4; the steps below are an integration contract,
not a claim that GUI code has already been changed.

## 1. Choose ownership before choosing a path

Each application/service owns its own `OwnerId` and collection names:

| Component | Example owner | Intended responsibility |
| --- | --- | --- |
| AOWIS-GUI | `gui` | User's server definitions, WireGuard profiles, unit profiles |
| AOWIS-GUI-Mobile | `gui-mobile` | Mobile's independent settings and applicable profiles |
| AOWIS-SERVER-MAP | `server-map` | Map-service configuration |
| AOWIS-SERVER-DVMDB | `server-dvmdb` | DVMDB service configuration, **not** project data |

**No global mutable `shared/` directory.** Reuse schemas and library code,
not physical stores. A single owner may reuse one WireGuard profile for many
connection entries; a second owner does not acquire that profile implicitly.

Use `ConfigRoot::forCurrentUser(owner)` for normal native user applications,
`fromOwnerRoot(owner, absolute_root)` for explicit service/test roots, or
`fromNamespaceRoot(owner, absolute_aowis_root)` when the caller chooses an
entire AOWIS namespace. These resolve paths only; a collection's `open()`
performs the actual secure native filesystem access.

## 2. Integrate the CMake target

As a version-pinned Git submodule (example for a parent repository):

```bash
git submodule add https://github.com/aowis-org/AOWIS-CONFIG.git external/AOWIS-CONFIG
```

```cmake
add_subdirectory(external/AOWIS-CONFIG)
target_link_libraries(my-component PRIVATE aowis::config)
```

The imported target has a transitive C++20 requirement and public include
path. In a parent build, tests are off unless
`AOWIS_CONFIG_BUILD_TESTS=ON` is explicitly enabled. Do not directly include
`src/` or invent a Qt dependency. The equivalent installed-package path is
`find_package(AOWISConfig CONFIG REQUIRED)` plus the same target.

## 3. Use stable IDs and compare-and-save revisions

A `WireGuardProfileSummary` contains metadata, `ObjectInfo::id`, and
`ObjectInfo::revision`, but **no private configuration text**. Use `list()` for
profile selection and `read()` only when the user opens an editor or needs to
start a tunnel. On update, supply the exact revision from the last successful
read; a stale revision returns `ErrorCode::conflict`, which should trigger a
reload/compare prompt—not an unconditional overwrite.

One proposed **GUI-owned connection** representation (not yet implemented):

```json
{
  "name": "Office DVMDB",
  "service_type": "dvmdb",
  "endpoint": "http://10.8.0.10:8755",
  "transport": {
    "type": "wireguard",
    "profile_id": "<wireguard-profile-uuidv7>"
  }
}
```

The connection has its **own** identity, distinct from the identity reported
by the remote DVMDB repository and from the WireGuard profile ID. Its eventual
stored object also has the versioned envelope defined by the object store.
Do **not** copy the illustrative JSON as a ratified connection schema.

## 4. Import, edit and validate WireGuard text

Suggested UI flow:

1. Use the native Qt file picker only to read the user's `.conf`; pass the
   complete UTF-8 text into `WireGuardProfiles::importText()`.
2. Preserve text, comments and line endings. Never persist the source path.
3. Use `validateWireGuardConfiguration()` for line-numbered issues. A warning
   is *not* an error, but it also is *not* proof of runtime compatibility.
4. Use `list()` to populate the Settings list; use `read()` on selection.
   Show `listing.unreadable` entries as damaged rows and offer
   `eraseUnreadable()` after confirmation, so one broken file never hides the
   others. Show `profile.validation` issues in the editor even for stored
   profiles: an older profile may fail newer rules and must stay repairable.
5. Use `update(id, expected_revision, input)` for edits and rename; reload on
   `conflict`. Refresh the list and consumers only after a successful save.
6. Do not log full `WireGuardProfile`, exported configuration, JSON payloads,
   or editor contents. The native store protects files but does not encrypt.

The GUI's current syntax-highlighted `QPlainTextEdit` can remain the editor.
AOWIS-CONFIG is the persistence library, not a replacement editor or a
WireGuard configuration executor.

## 5. Build 'Used by' from authoritative references

The label **“Used by N services”** means *N configured service connection
entries referencing that profile ID*—not N TCP connections, WireGuard peers,
or active tunnels. Compute the count and detailed referring-connection list
from the owning application's connection catalogue. Do not persist a derived
`used_by_count` in the profile record.

`WireGuardProfiles::erase(id, revision, reference_count)` **requires** a
reference resolver and rejects nonzero usage, unavailable/failed lookups and
stale revisions. An example resolver signature is:

```cpp
aowis::config::WireGuardReferenceCount count_references =
    [&connections](const aowis::config::ObjectId &profile_id)
        -> aowis::config::Result<std::size_t> {
        return connections.countWireGuardReferences(profile_id);
    };
```

`connections.countWireGuardReferences()` above is **illustrative application
code**; AOWIS-CONFIG does not provide that method or a connection catalogue.

**Critical race:** the resolver and low-level deletion are not one transaction
with the connection collection. During Phase 4, enforce a single authoritative
GUI coordinator that serializes reference creation/reassignment and deletion
(or design a real cross-record transaction). Merely supplying the callback is
not enough to prove referential integrity under concurrent writers.

## 6. Keep WireGuard runtime out of the configuration store

| Question | Owning subsystem |
| --- | --- |
| Which profile text is stored? | AOWIS-CONFIG |
| Which service connections reference it? | GUI/service connection catalogue |
| Is a tunnel running, starting, stopped, or in error? | AOWIS-NETWORK |
| How many currently active consumers use it? | AOWIS-NETWORK / runtime coordinator |
| Last handshake, peer endpoint and byte counters? | AOWIS-NETWORK |

One profile may be reused by multiple configured connections and—depending on
network-layer policy—one running tunnel. Interface state is not peer reachability.
Status views must show **real** runtime information or an explicit unavailable
state; never fabricate traffic/handshake data from a stored profile.

## 7. Threading, locking and platform roots

All calls are synchronous filesystem I/O with `fsync`. Run them off the Qt
event-loop thread (or accept a short stall), and choose a `lock_timeout` the
UI can tolerate; `lock_timeout` errors mean another instance is holding the
collection and nothing changed.

`ConfigRoot::forCurrentUser()` relies on `XDG_CONFIG_HOME`/`HOME`, which
Android does not provide meaningfully. AOWIS-GUI-Mobile must pass an explicit
root, e.g. `ConfigRoot::fromNamespaceRoot(owner, <QStandardPaths::AppConfigLocation>/aowis)`.

## 8. Platform and integration checklist

- [ ] WireGuard GUI uses an explicit `gui` owner root and linked `aowis::config`.
- [ ] Import/rename/edit/delete persist and survive restart; no path references.
- [ ] All network consumers use profile IDs, not `wireguard_profile_path`.
- [ ] References are counted across *all configured* supported service types.
- [ ] Delete/reference creation is coordinated; on error it fails closed.
- [ ] Running status and active consumers come from AOWIS-NETWORK.
- [ ] The **WireGuard VPN** settings entry remains visible, greyed out and
      explained with a tooltip in WebAssembly; no embedded browser VPN claimed.
- [ ] Obsolete experimental GUI-only persistence and dead code are removed.
- [ ] Tests exercise restart, two consumers/one profile, stale edits, delete
      guard, missing references, platform disabled UI and error handling.

See the [Phase 4 roadmap](roadmap.md) and [handoff](handoff.md) before editing
the GUI repository. Windows and Emscripten persistence are not delivered by
simply linking the existing native POSIX code.
