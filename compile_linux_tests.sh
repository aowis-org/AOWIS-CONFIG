#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cmake -S "$project_root" -B "$project_root/build-linux-tests" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DAOWIS_CONFIG_BUILD_TESTS=ON
cmake --build "$project_root/build-linux-tests" --parallel
ctest --test-dir "$project_root/build-linux-tests" --output-on-failure
