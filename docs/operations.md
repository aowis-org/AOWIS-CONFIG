# Operations, security and verification

Status: **v0.7.0**: native POSIX and browser backends; the Windows backend
does not exist yet (`unsupported_platform`). API: [api.md](api.md).

## Layout on disk

```text
~/.config/aowis/gui/          0700, owned by the user
├── .lock                     0600, owner lock (read-write mode only)
└── wireguard/                0700
    ├── <uuidv7>.json         0600, one record each
    └── .tmp-<uuidv7>         only after an interrupted write; removed on next open/write
```

A record is `{"envelope_version":1,"id":…,"revision":…,"payload":{…}}`. A
WireGuard payload contains the complete imported `.conf` text **including
`PrivateKey`**; the source path is never stored. A record is not a runnable
`wg-quick` file — use `exportText()` for an explicit export.

Administrator roots (opened `read_only`), e.g.:

```bash
sudo install -d -o root -g aowis -m 0750 /etc/aowis/server-dvmdb /etc/aowis/server-dvmdb/settings
sudo install -o root -g aowis -m 0640 <record>.json /etc/aowis/server-dvmdb/settings/
```

Group/other-writable entries and symlinks at or below the owner root are
rejected in every mode.

## Browser storage

Each record is one `localStorage` item of the page's origin:
`aowis-config:/aowis/<owner>/<collection>/<uuidv7>.json` → the same JSON
record as on disk. Consequences:

- Clearing the site's data, private-browsing session ends, or browser storage
  eviction delete the configuration; there is no `fsync`-level guarantee.
- Every origin (scheme + host + port) has its own store; moving the
  deployment to another host starts empty.
- Only one tab per owner writes (Web Locks lease); others read and get
  `lock_timeout` on writes until the writer tab closes.
- Readable by any script of the same origin. WireGuard is disabled in
  browsers, so no private keys are stored there; do not store other secrets
  there either.
- Backup/inspection: the browser's developer tools list the items.

## Security model

| Aspect | Behavior |
| --- | --- |
| Permissions (read-write) | Owner root and collections `0700`, records and lock `0600`, owned by the effective user; weaker permissions are rejected, never repaired |
| Permissions (read-only) | Owned by the user or root; group/other-readable allowed, writable never |
| Paths | Descriptor walk from `/`; ancestors above the owner root may be symlinks; at/below it `O_NOFOLLOW`, traversal and wrong file types rejected |
| Writes | Exclusive temp file, `fsync`, `linkat` (create) / `renameat` (update), directory `fsync` |
| Concurrency | One `flock` per owner, polled up to `lock_timeout`; revision tokens reject stale edits; `OwnerSession` serializes multi-step operations |
| Reads in read-only mode | No lock: every write is an atomic whole-record replacement |
| Secrets in diagnostics | Error messages never contain payloads, text or keys; callers must not log profiles, payloads or exports |
| Encryption at rest | **None.** Mode bits do not protect against the same user, administrators or physical access |
| Windows | Roots under `%LOCALAPPDATA%`, never roaming `%APPDATA%` (keys must not sync to domain servers) |
| Hook commands in imported text | Never executed by CONFIG; AOWIS-NETWORK ignores them |

## Backup, restore, transfer

Copy the **whole owner root while the owner is stopped**:

```bash
mkdir -p /path/to/private-backup/aowis
cp -a "$HOME/.config/aowis/gui" /path/to/private-backup/aowis/
```

A copy is a **secret-bearing backup**: encrypt it outside a trusted machine.
Restore as a complete tree into a stopped owner, then reopen and check names
and IDs. Not included: DVMDB repositories, working copies, tiles, caches,
logs, and anything external referenced by imported text (key files, hook
scripts); endpoints may be unreachable from the destination.

## Errors and safe responses

| `ErrorCode` | Meaning → response |
| --- | --- |
| `invalid_owner_id`, `invalid_collection`, `invalid_root` | Caller bug: check identifiers and roots |
| `missing_environment`, `unsupported_platform` | No usable root/backend: choose an explicit root or supported platform |
| `not_found` | Record or (read-only) root missing: re-list |
| `conflict` | Changed since read, or ID collision: reload and compare, never force |
| `lock_timeout` | Another instance holds the owner lock: nothing changed, retry later |
| `session_mismatch` | Session of a different store passed: caller bug |
| `permission_denied` | Wrong owner/mode, or a write on a read-only store |
| `unsafe_path` | Symlink, wrong file type or unsafe permissions: inspect, do not bypass |
| `invalid_json`, `invalid_wireguard_profile`, `object_too_large` | Bad input: show the message, let the user correct it |
| `corrupt_object` | Damaged record: listed in `unreadable`; keep a backup, then `eraseCorrupt`/`eraseUnreadable` |
| `unsupported_schema` | Written by a newer release: keep it; never deleted by the erase helpers |
| `profile_in_use`, `reference_unavailable` | Delete blocked by references or missing catalogue check |
| `io_failure` | Filesystem/sync error; after publication the write may have happened — re-read before retrying |

## Tests

```bash
./compile_linux_tests.sh                 # all CTest suites, fails on any failure
python3 tests/repository_tooling_test.py # patch/snapshot tooling (9 tests)
ctest --test-dir build-linux-tests --output-on-failure -R '^aowis-config-backend-contract$'
```

| CTest target | Coverage |
| --- | --- |
| `aowis-config-core` | Owner IDs, roots, `Result`, version |
| `aowis-config-consumer-smoke` | Public headers/target only; `MemoryStorage` end to end on every platform |
| `aowis-config-object-store` | POSIX: CRUD, revisions, permissions, symlinked ancestors vs. symlinked owner, damaged entries, `SIGKILL` crash injection before/after publish, cross-process conflicts, owner lock held by another process (all collections blocked, read-only not), read-only mode (`/etc`-style permissions, no lock/creation, writes refused) |
| `aowis-config-wireguard-profiles` | Text integrity only (no syntax), lifecycle, reference guard, reference count and reassign-then-delete inside one `OwnerSession`, foreign session, unreadable/wrong-type/newer-version records, POSIX permissions, relocation |
| `aowis-config-browser-glue` | Node (when installed): the EM_JS bodies of `browser_backend.cpp` against fake `localStorage`/`navigator.locks` — get/put/create-only/remove/keys, quota, unavailable storage, writer lease pending → held, second tab waits and takes over, independent roots, no Web Locks → fail closed |
| `aowis-config-backend-contract` | Same suite on every backend (memory, browser key-value logic on a fake store, POSIX): CRUD, damaged/foreign/newer entries, 8-thread lost-update race, zero-timeout contention, held session excludes writers but not readers, read-only contract, WireGuard; clean under ThreadSanitizer |

Installed-package check (not a CTest target):

```bash
./compile_linux.sh
cmake --install build-linux --prefix /tmp/aowis-config-install
cmake -S tests/installed_consumer -B build-installed-consumer -G Ninja -DCMAKE_PREFIX_PATH=/tmp/aowis-config-install
cmake --build build-installed-consumer && ./build-installed-consumer/aowis-config-installed-consumer
```

**Not proven:** real power-loss durability (only process kills), the browser
backend inside a real browser and an Emscripten build (no toolchain in the
development sandbox; first run with Phase 4), Windows persistence (no backend), GUI integration (Phase 4), encryption at
rest (none), tunnel operation (AOWIS-NETWORK).

When changing the library: add a test for the behavior and an adverse case;
run all suites, the tooling tests and (for CMake changes) the installed
consumer; update [architecture.md](architecture.md) decisions and roadmap.
