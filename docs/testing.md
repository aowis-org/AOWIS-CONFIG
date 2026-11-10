# Verification and test matrix

This page describes **the tests present in AOWIS-CONFIG v0.3.0**, not tests
that have necessarily run on every downstream AOWIS component. To verify a
new checkout, run:

```bash
cd ~/coding/aowis/AOWIS-CONFIG
./compile_linux_tests.sh
python3 tests/repository_tooling_test.py
```

The native helper invokes CMake and CTest with `--output-on-failure` and fails
if compilation or a test fails. You can run a specific CTest target with:

```bash
ctest --test-dir build-linux-tests --output-on-failure \
  -R '^aowis-config-wireguard-profiles$'
```

## Implemented suites

| CTest target | Source | Coverage focus |
| --- | --- | --- |
| `aowis-config-core` | `tests/config_core_test.cpp` | Owner IDs, roots, Result, version and core API |
| `aowis-config-consumer-smoke` | `tests/consumer/main.cpp` | Public-header and target linkage, no private includes |
| `aowis-config-object-store` | `tests/object_store_test.cpp` | JSON, UUIDv7, CRUD, revisions, filesystem protections, conflicts, crash injections |
| `aowis-config-wireguard-profiles` | `tests/wireguard_profiles_test.cpp` | Import, exact text, metadata, validation, list/read/export, deletion guards, relocation |

Outside CTest, `tests/repository_tooling_test.py` exercises the ZIP/source
snapshot and `apply_patch.sh` workflow (9 tooling checks at the Phase 3
checkpoint). The installed-package consumer under
`tests/installed_consumer/` is a **separate install/consume exercise**, not a
fifth standard CTest target.

## Verify installation as a consumer

```bash
./compile_linux.sh
cmake --install build-linux --prefix /tmp/aowis-config-install
cmake -S tests/installed_consumer -B build-installed-consumer -G Ninja \
  -DCMAKE_PREFIX_PATH=/tmp/aowis-config-install
cmake --build build-installed-consumer
./build-installed-consumer/aowis-config-installed-consumer
```

This checks the exported `AOWISConfig` package and `aowis::config` link from
an independently configured project. For an in-tree parent integration, also
verify `add_subdirectory(external/AOWIS-CONFIG)`.

## What is *not* proven

- **Power loss:** controlled `SIGKILL` fault injection and restart recovery
  test interrupted processes, **not** actual power removal on ext4/xfs/etc.
- **All platforms:** the persistent object store is implemented for native
  POSIX. Windows paths resolving does not imply a Windows storage backend;
  no Emscripten/browser persistence backend exists.
- **Whole-application integration:** AOWIS-GUI's WireGuard GUI is not yet
  backed by this library; usage/status are not live, and old connection
  paths have not been removed.
- **Cross-collection referential integrity:** the WireGuard deletion callback
  cannot atomically exclude new references created in another collection.
- **Secrets encrypted at rest:** POSIX file modes protect against other
  ordinary users, not processes with the same privileges or physical access.
- **Remote VPN operation:** importing a structurally valid `.conf` does not
  prove it can start, handshake, route packets, or reach a service.

## When changing the library

1. Add or update a test that exercises the public behavior and an adverse
   case (invalid input, stale revision, interruption, or access error).
2. Build the native library and run all CTest targets and tooling checks.
3. For storage changes, test open after restart and after copying the owner
   root; confirm secret files remain private.
4. If the public CMake interface changes, rerun the installed consumer.
5. Update API docs, [decisions](decisions.md) where a contract changed,
   [roadmap](roadmap.md), and [handoff](handoff.md). Do not mark a milestone
   complete based only on documentation or a single happy-path run.

See [Operations](operations.md) for response to storage errors and
[Roadmap](roadmap.md) for planned hardening.
