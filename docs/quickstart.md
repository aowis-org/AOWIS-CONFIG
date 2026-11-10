# Quickstart — build, inspect and use the library

This guide applies to **AOWIS-CONFIG 0.5.0** and the **native POSIX** object
store. Root *resolution* has a Windows code path, but object persistence does
not yet have a Windows or Emscripten/browser backend.

## Prerequisites and builds

On Linux, install a C++20 toolchain, CMake 3.20+, Ninja, Bash, Python 3, `zip`
and `unzip`. No Qt or database server is necessary.

```bash
cd ~/coding/aowis/AOWIS-CONFIG
./compile_linux.sh                    # release library: build-linux/
./compile_linux_tests.sh              # debug library + all CTest targets
python3 tests/repository_tooling_test.py
```

To inspect the registered tests:

```bash
ctest --test-dir build-linux-tests -N
ctest --test-dir build-linux-tests --output-on-failure
```

For a clean, explicit CMake configuration (rather than the helper scripts):

```bash
cmake -S . -B build-linux-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DAOWIS_CONFIG_BUILD_TESTS=ON
cmake --build build-linux-tests --parallel
ctest --test-dir build-linux-tests --output-on-failure
```

## Use the storage API in an isolated directory

This small example creates an owner-scoped collection, writes one JSON object,
reads it, and removes it using the returned revision token. It does **not**
modify your regular AOWIS user settings.

Create `demo.cpp`:

```cpp
#include <aowis/config/config_root.h>
#include <aowis/config/object_store.h>

#include <filesystem>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "Usage: demo <absolute-owner-root>\n";
        return 2;
    }

    aowis::config::Result<aowis::config::OwnerId> owner =
        aowis::config::OwnerId::parse("gui");
    if (!owner) return 1;

    aowis::config::Result<aowis::config::ConfigRoot> root =
        aowis::config::ConfigRoot::fromOwnerRoot(
            owner.value(), std::filesystem::path(argv[1]));
    if (!root) return 1;

    aowis::config::Result<aowis::config::ObjectCollection> collection =
        aowis::config::ObjectCollection::open(root.value(), "examples");
    if (!collection) return 1;

    aowis::config::Result<aowis::config::StoredObject> saved =
        collection.value().create(R"({"name":"Hello AOWIS"})");
    if (!saved) return 1;

    aowis::config::Result<aowis::config::StoredObject> loaded =
        collection.value().read(saved.value().info.id);
    if (!loaded) return 1;

    std::cout << "ID: " << loaded.value().info.id.value() << '\n';
    std::cout << "Payload: " << loaded.value().json << '\n';

    aowis::config::Result<bool> removed = collection.value().erase(
        loaded.value().info.id, loaded.value().info.revision);
    return removed && removed.value() ? 0 : 1;
}
```

Link this through a consuming CMake project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(config-demo LANGUAGES CXX)
find_package(AOWISConfig CONFIG REQUIRED)
add_executable(config-demo demo.cpp)
target_link_libraries(config-demo PRIVATE aowis::config)
```

Install the library from its repository, then build the external project:

```bash
# Run in AOWIS-CONFIG:
cmake --install build-linux --prefix /tmp/aowis-config-install

# Run in a separate directory containing demo.cpp and CMakeLists.txt:
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/tmp/aowis-config-install
cmake --build build
owner_root="$(mktemp -d)/gui"
./build/config-demo "$owner_root"
```

The demo leaves an empty private `examples/` collection and lock file under
its temporary owner root. Delete the disposable test directory when finished.
**Do not print `StoredObject::json` for real WireGuard or credential objects:**
that string may contain secrets.

## Use the typed WireGuard API

AOWIS-CONFIG accepts the **contents** of a `.conf`, not its original filename.
A UI or command-line caller reads the input and passes a `WireGuardProfileInput`
into `WireGuardProfiles::importText()`. See the full
[WireGuard API](wireguard-profiles.md) and the
[consumer integration guide](integration-guide.md) for deletion references,
validation and runtime responsibilities. Saving a profile does **not** start a
WireGuard tunnel.

## Where next?

- [Storage API](storage-api.md): full CRUD contract, revision tokens and limits.
- [Operations and security](operations.md): copying owner roots, permissions,
  errors and recovery.
- [Verification guide](testing.md): exactly what the tests establish.
- [Handoff](handoff.md): Phase 4 integration entry point.
