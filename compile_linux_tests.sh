#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
    cat <<'HELP'
Usage: ./compile_linux_tests.sh [--debug | --release | --both]

  (no option)  Use build-linux-tests with its existing CMake configuration.
  --debug      Build and test Debug in build-linux-tests-debug.
  --release    Build and test Release in build-linux-tests-release.
  --both       Run both configurations; fail if either one fails.

Only AOWIS DB test executables are built, and only tests with the exact
'aowis' label are executed. A missing/empty test registration fails.
HELP
}

run_suite() {
    local build_dir="$1"
    local build_type="$2"
    local cmake_args=( -S "${PROJECT_DIR}" -B "${build_dir}" -G Ninja -DBUILD_TESTING=ON
        "-DAOWIS_MODEL_SOURCE_DIR=${PROJECT_DIR}/external/AOWIS-SERVER-MODEL" )
    if [[ -n "$build_type" ]]; then
        cmake_args+=( "-DCMAKE_BUILD_TYPE=${build_type}" )
    fi

    printf '=== AOWIS DB Linux tests (%s) ===\n' "${build_type:-existing configuration}"
    cmake "${cmake_args[@]}"
    # Build the aggregate target generated from every DB test registered in CMake.
    # Never hardcode individual test targets here: new registrations must be built.
    # Ninja's dependency graph handles incremental builds. Do not force a
    # full clean when source contents change; patch ZIPs carry fresh mtimes.
    cmake --build "$build_dir" --target aowis-db-tests --parallel
    local registered
    registered="$(ctest --test-dir "$build_dir" -N -L '^aowis$')"
    if ! grep -Eq 'Total Tests: [1-9][0-9]*' <<< "$registered"; then
        printf 'ERROR: no AOWIS tests registered in %s\n%s\n' "$build_dir" "$registered" >&2
        return 1
    fi
    ctest --test-dir "$build_dir" --output-on-failure -L '^aowis$'
}

if [[ ! -f "${PROJECT_DIR}/external/AOWIS-SERVER-MODEL/CMakeLists.txt" ]]; then
    printf 'Model submodule is missing; run ./git_submodule_init.sh first.\n' >&2
    exit 1
fi

case "${1:-}" in
    '') [[ $# -eq 0 ]] || { usage >&2; exit 2; }; run_suite "${PROJECT_DIR}/build-linux-tests" '' ;;
    --debug) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; run_suite "${PROJECT_DIR}/build-linux-tests-debug" Debug ;;
    --release) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; run_suite "${PROJECT_DIR}/build-linux-tests-release" Release ;;
    --both) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; run_suite "${PROJECT_DIR}/build-linux-tests-debug" Debug; run_suite "${PROJECT_DIR}/build-linux-tests-release" Release ;;
    --help|-h) usage ;;
    *) usage >&2; exit 2 ;;
esac
