#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cmake -S "$project_root" -B "$project_root/build-linux" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DAOWIS_CONFIG_BUILD_TESTS=OFF
cmake --build "$project_root/build-linux" --parallel
