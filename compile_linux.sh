#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
BUILD_DIR="${PROJECT_DIR}/build-linux"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "${0##*/}" >&2
    exit 2
fi

if [[ ! -f "${PROJECT_DIR}/external/AOWIS-SERVER-MODEL/CMakeLists.txt" ]]; then
    printf 'Model submodule is missing; run ./git_submodule_init.sh first.\n' >&2
    exit 1
fi

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -G Ninja -DBUILD_TESTING=OFF \
    -DAOWIS_MODEL_SOURCE_DIR="${PROJECT_DIR}/external/AOWIS-SERVER-MODEL"
# Ninja handles incremental rebuilds from the source/header dependency graph.
# Patch ZIP files must provide fresh mtimes on changed source files; never
# clean all build objects merely because the source tree has changed.
# BUILD_TESTING=OFF above keeps this target set small: core library, local
# administration CLI and the optional HTTP server. Ninja remains incremental.
cmake --build "${BUILD_DIR}" --parallel
printf 'AOWIS DB library and enabled tools built successfully.\n' 
