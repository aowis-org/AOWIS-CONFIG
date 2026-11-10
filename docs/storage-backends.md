# Storage backends

Status: **v0.5.0 — backend interface, native POSIX backend and in-memory
backend implemented; Windows and browser backends not implemented.**
Updated: 2026-11-10. See [Storage API](storage-api.md) for the public
`ObjectCollection` contract that every backend serves.

## Layering

```text
ObjectCollection / WireGuardProfiles     (public, platform-independent)
  │  envelope encode/decode, payload validation, revisions,
  │  list/readAll + unreadable classification, erase/eraseCorrupt rules
  ▼
detail::CollectionBackend / CollectionSession   (src/storage_backend.h, private)
  ├── posix_backend.cpp        native POSIX (Linux, Android, macOS, BSD)
  ├── memory_backend.cpp       MemoryStorage, every platform
  └── unsupported_backend.cpp  Windows + Emscripten placeholder
```

A backend moves opaque bytes for **one collection of one owner**. Everything
that decides *what* a record means stays in `ObjectCollection`, so damaged
entries, schema versions, revision conflicts and WireGuard rules behave
identically on every backend. Exactly one persistent backend is compiled per
platform; the selection is in `CMakeLists.txt`.

The interface is private (`src/`), not public API: backends are implemented
inside this library, never by consumers. Consumers choose only between
`ObjectCollection::open(root, …)` (the platform's persistent backend) and
`ObjectCollection::openInMemory(storage, …)`.

## Backend contract

Every backend must guarantee (authoritative text: `src/storage_backend.h`):

| # | Guarantee | POSIX | Memory |
| --- | --- | --- | --- |
| 1 | Exclusive sessions across threads and processes; bounded wait, then `lock_timeout` | `flock` polled until deadline | `std::mutex` polled until deadline |
| 2 | Atomic single-entry replace; `create_only` never replaces (`conflict`) | fsynced temp + `linkat`/`renameat` | map assignment under the lock |
| 3 | Success only once durable under the backend's failure model | file + directory `fsync` | lifetime of the last `MemoryStorage` handle |
| 4 | Enumerate every `*.json` entry, including foreign names; hide bookkeeping | `readdir`, skips `.lock`/`.tmp-*` | map keys ending in `.json` |
| 5 | Per-entry errors (`not_found`, `unsafe_path`, `object_too_large`, …) without payload text | symlink/permission/size checks | size check |
| 6 | Bound to one owner root and validated collection name | descriptor walk from owner root | per-`MemoryStorage` map |
| 7 | `lock(recover=true)` removes own crash residue; residue invisible to readers | orphan `.tmp-<uuidv7>` cleanup | nothing to recover |

`tests/backend_contract_test.cpp` runs the same behavioral suite against
every backend available on the build platform (CRUD and revisions, damaged
and foreign entries, newer-schema protection, an 8-thread lost-update race,
zero-timeout behavior under contention, WireGuard profiles end to end). A new
backend is added to `backends()` there and must pass unchanged.

## MemoryStorage

`#include <aowis/config/memory_storage.h>` — available on every platform.

```cpp
aowis::config::MemoryStorage storage;           // copies share contents
aowis::config::Result<aowis::config::ObjectCollection> collection =
    aowis::config::ObjectCollection::openInMemory(storage, "wireguard");
aowis::config::Result<aowis::config::WireGuardProfiles> profiles =
    aowis::config::WireGuardProfiles::fromCollection(collection.value());
```

Intended for consumer unit tests (e.g. AOWIS-GUI settings widgets without a
filesystem) and as the reference implementation of the contract. It is
thread-safe and never touches disk. `writeRawEntry()`/`readRawEntry()` are
fixture helpers that bypass validation to simulate damaged or foreign
entries. Do not use it as a production store: nothing persists.

## Adding the Windows or browser backend

1. Create `src/windows_backend.cpp` / `src/browser_backend.cpp` implementing
   `detail::openNativeBackend()` and the two interfaces.
2. Replace `unsupported_backend.cpp` for that platform in `CMakeLists.txt`.
3. Add the backend to `backends()` in the contract test; it must pass as is.
4. Document its failure model (item 3) and residue handling (item 7) in this
   page and in [Operations](operations.md).

Design notes for the open backends (not decided; see
[decisions](decisions.md) open questions 5 and 7):

- **Windows:** `%APPDATA%\aowis\<owner>`, `ReplaceFileW`/`MoveFileExW` with
  `MOVEFILE_WRITE_THROUGH`, `LockFileEx` with a polled deadline, owner-only
  ACLs instead of POSIX modes, reparse-point rejection below the owner root.
- **Browser (Emscripten):** a key-value store such as IndexedDB or
  `localStorage` keyed by owner/collection/entry; item 1 needs a cross-tab
  lock (e.g. Web Locks API) or an explicit single-tab policy; item 3 is the
  browser's persistence guarantee, which is weaker than `fsync`. WireGuard
  stays disabled in WASM regardless.
