# Native library API and consumption (Phases 1–3)

Status: **v0.3.0; native C++20 foundation, POSIX object storage and WireGuard profile API implemented**. For a runnable example see [Quickstart](quickstart.md); for consumers see [Integration guide](integration-guide.md).

AOWIS-CONFIG is a C++20 static library that builds standalone without Qt,
DVMDB, MAP or AOWIS-NETWORK. Public headers are in `include/aowis/config/`.

## Commands

```bash
./compile_linux.sh
./compile_linux_tests.sh
python3 tests/repository_tooling_test.py
```

The first command builds only the library in `build-linux/`. The second
configures `build-linux-tests/`, builds the library and smoke consumers, and
runs CTest, failing if any test fails. The Python suite checks patch/snapshot
tooling separately. CMake 3.20+, C++20 compiler and Ninja are required by
these scripts. The library itself has no third-party dependencies.
Native POSIX object storage is described in [storage-api.md](storage-api.md).
WireGuard import and profile management are described in [wireguard-profiles.md](wireguard-profiles.md).

## Public symbols

| Header | API | Behavior |
| --- | --- | --- |
| `error.h` | `ErrorCode`, `Error` | Typed error code and nonsecret message |
| `result.h` | `Result<T>` | Success/failure result for value-returning calls; wrong alternative access throws `std::bad_variant_access` |
| `owner_id.h` | `OwnerId::parse(text)` | Validates application owner IDs (`gui`, `gui-mobile`, `server-dvmdb`) |
| `config_root.h` | `ConfigRoot::fromNamespaceRoot(owner, absolute_path)` | Appends the validated owner to the explicit `aowis` namespace root |
| `config_root.h` | `ConfigRoot::fromOwnerRoot(owner, absolute_path)` | Uses an explicit absolute owner root for e.g. `/etc/aowis/server-dvmdb` |
| `config_root.h` | `ConfigRoot::forCurrentUser(owner)` | Resolves native user config root without writing to disk |
| `version.h` | `version_major/minor/patch` | Current source API version: 0.3.0 |
| `object_store.h` | `ObjectCollection` | Native POSIX atomic JSON object CRUD and revision checks |
| `wireguard_profiles.h` | `WireGuardProfiles`, validator | Typed, owner-scoped WireGuard profiles and diagnostics; see linked API |

`OwnerId` is a restricted path component: 1–63 ASCII characters, starts with
`a`–`z`, followed by lowercase ASCII letters/digits, or single interior
hyphens. `.` and `/` are not allowed. The path constructors reject empty or
relative roots and lexically normalize paths. The owner never comes from
unvalidated filesystem input.

Native user-root resolution:

- Linux/other native POSIX: absolute `XDG_CONFIG_HOME/aowis/<owner>`, falling
  back to absolute `HOME/.config/aowis/<owner>` if XDG is absent or relative.
- Windows: absolute `APPDATA/aowis/<owner>`.
- Emscripten: returns `unsupported_platform`; a future browser adapter must
  implement browser persistence rather than assuming a native disk.

An explicit owner root is available for system services and tests. Resolving
a `ConfigRoot` only names a path; `ObjectCollection::open()` on native POSIX
performs the secure directory creation/checks before using that path. The
Windows and Emscripten storage backends are not implemented.

```cpp
#include <aowis/config/config_root.h>

aowis::config::Result<aowis::config::OwnerId> owner =
    aowis::config::OwnerId::parse("gui");
if (!owner) {
    // Inspect owner.error().code and owner.error().message.
    return;
}
aowis::config::Result<aowis::config::ConfigRoot> root =
    aowis::config::ConfigRoot::forCurrentUser(owner.value());
if (!root) {
    return;
}
// root.value().path() names the location; no files have been written.
```

## Consume as a submodule

Add this repository to a component's source tree, for example as
`external/AOWIS-CONFIG`. From the parent's CMake project:

```cmake
add_subdirectory(external/AOWIS-CONFIG)
target_link_libraries(my-component PRIVATE aowis::config)
```

Tests are off by default when included via `add_subdirectory`; the parent can
set `AOWIS_CONFIG_BUILD_TESTS=ON` deliberately. The public target is
`aowis::config`. It supplies its C++20 requirement and include directory
transitively, so consumers do not reference the library's private `src/`.

## Consume as an installed package

```bash
cmake --install build-linux --prefix /tmp/aowis-config-install
cmake -S tests/installed_consumer -B build-installed-consumer -G Ninja \
  -DCMAKE_PREFIX_PATH=/tmp/aowis-config-install
cmake --build build-installed-consumer
./build-installed-consumer/aowis-config-installed-consumer
```

The installed CMake package is `AOWISConfig` with exported target
`aowis::config`. `tests/consumer/main.cpp` is used by both the in-tree
consumer smoke and the external installed-package sample. Both are independent
of AOWIS-GUI and Qt.

## Deliberately not implemented yet

- AOWIS-GUI WireGuard persistence integration, fully transactional service
  reference coordination, or WireGuard runtime diagnostics.
- Simple preference storage format, Qt adapters and GUI/server integrations.
- Windows/Android/browser persistence adapters and cross-object transactions.

**Serialization boundary:** The core validates strict JSON and the versioned
storage envelope. It does not interpret the application-owned JSON payload;
component schemas; lossless WireGuard `.conf` serialization is
implemented in the optional typed WireGuard layer described above.
See [storage-api.md](storage-api.md).

API is pre-1.0 and may change during the remaining foundation work. There is
no historical data migration or ABI compatibility promise.
