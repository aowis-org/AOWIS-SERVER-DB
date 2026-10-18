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

PostgreSQL integration tests run automatically using a disposable local
Docker Compose container when Docker Compose is available. Configure using:
  AOWIS_DB_TEST_POSTGRES=auto     Use Docker if available (default)
  AOWIS_DB_TEST_POSTGRES=required Require a working PostgreSQL test service
  AOWIS_DB_TEST_POSTGRES=off      Explicitly skip live PostgreSQL tests

When AOWIS_DB_TEST_PG_DB is set, use that existing, dedicated test database
instead of Docker, with libpq PGHOST/PGPORT/PGUSER/PGPASSWORD settings.

Only AOWIS DB test executables are built, and only tests with the exact
'aowis' label are executed. A missing/empty test registration fails.
HELP
}

# This project is unique per invocation. Docker picks an unused host port and
# PostgreSQL data lives only in container memory. Cleanup also runs when a
# compilation or test fails, so no development database is modified.
postgres_compose_file="${PROJECT_DIR}/tests/postgres/docker-compose.yaml"
postgres_compose_project="aowis-db-pgtest-$$"
postgres_started=false

cleanup_postgres() {
    if [[ "$postgres_started" == true ]]; then
        printf '=== Stopping disposable AOWIS PostgreSQL test container ===\n'
        docker compose -f "$postgres_compose_file" -p "$postgres_compose_project" \
            down --volumes --remove-orphans || \
            printf 'WARNING: Could not clean up PostgreSQL container (%s).\n' "$postgres_compose_project" >&2
    fi
}

start_postgres_tests() {
    local mode="${AOWIS_DB_TEST_POSTGRES:-auto}"
    case "$mode" in
        auto|required|off) ;;
        *) printf 'Invalid AOWIS_DB_TEST_POSTGRES=%s (auto, required, off)\n' "$mode" >&2; return 2 ;;
    esac
    if [[ "$mode" == off ]]; then
        # Even an inherited test database must not be used in explicit off mode.
        unset AOWIS_DB_TEST_PG_DB AOWIS_DB_TEST_PG_REQUIRED
        printf '=== PostgreSQL integration SKIPPED (explicitly disabled) ===\n'
        return 0
    fi
    if [[ -n "${AOWIS_DB_TEST_PG_DB:-}" ]]; then
        export AOWIS_DB_TEST_PG_REQUIRED=1
        printf '=== PostgreSQL integration ENABLED (external dedicated database: %s) ===\n' "$AOWIS_DB_TEST_PG_DB"
        return 0
    fi

    if ! command -v docker >/dev/null 2>&1 || ! docker compose version >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
        if [[ "$mode" == required ]]; then
            printf 'ERROR: Docker Engine with Compose v2 is required for PostgreSQL tests.\n' >&2
            return 1
        fi
        printf 'WARNING: PostgreSQL integration SKIPPED (Docker Engine/Compose unavailable).\n' >&2
        printf '         Use AOWIS_DB_TEST_POSTGRES=required to make this a hard failure.\n' >&2
        unset AOWIS_DB_TEST_PG_REQUIRED
        return 0
    fi

    printf '=== Starting disposable PostgreSQL test container ===\n'
    # Set cleanup flag BEFORE 'up' in case image extraction/startup fails midway.
    postgres_started=true
    docker compose -f "$postgres_compose_file" -p "$postgres_compose_project" up -d postgres
    local container_id
    container_id="$(docker compose -f "$postgres_compose_file" -p "$postgres_compose_project" ps -q postgres)"
    if [[ -z "$container_id" ]]; then
        printf 'ERROR: PostgreSQL test container not found after startup.\n' >&2
        return 1
    fi

    local status=''
    local attempt
    for (( attempt=0; attempt<90; ++attempt )); do
        status="$(docker inspect --format '{{.State.Health.Status}}' "$container_id")"
        if [[ "$status" == healthy ]]; then
            break
        fi
        if [[ "$status" == unhealthy ]]; then
            printf 'ERROR: PostgreSQL test container failed its health check.\n' >&2
            docker compose -f "$postgres_compose_file" -p "$postgres_compose_project" logs --tail=60 postgres >&2 || true
            return 1
        fi
        sleep 1
    done
    if [[ "$status" != healthy ]]; then
        printf 'ERROR: PostgreSQL test container did not become healthy within 90 seconds.\n' >&2
        docker compose -f "$postgres_compose_file" -p "$postgres_compose_project" logs --tail=60 postgres >&2 || true
        return 1
    fi

    local endpoint
    endpoint="$(docker compose -f "$postgres_compose_file" -p "$postgres_compose_project" port postgres 5432)"
    local port="${endpoint##*:}"
    if [[ ! "$port" =~ ^[1-9][0-9]*$ ]] || (( port > 65535 )); then
        printf 'ERROR: Could not determine the Docker PostgreSQL host port: %s\n' "$endpoint" >&2
        return 1
    fi

    export PGHOST=127.0.0.1 PGPORT="$port" PGUSER=aowis_test
    export PGPASSWORD=aowis_test_only_local PGSSLMODE=disable
    export AOWIS_DB_TEST_PG_DB=aowis_test AOWIS_DB_TEST_PG_REQUIRED=1
    printf '=== PostgreSQL integration ENABLED (Docker, localhost:%s) ===\n' "$port"
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
    # full clean; tools/apply_patch.sh refreshes source timestamps locally.
    cmake --build "$build_dir" --target aowis-db-tests --parallel
    local registered
    registered="$(ctest --test-dir "$build_dir" -N -L '^aowis$')"
    if ! grep -Eq 'Total Tests: [1-9][0-9]*' <<< "$registered"; then
        printf 'ERROR: no AOWIS tests registered in %s\n%s\n' "$build_dir" "$registered" >&2
        return 1
    fi
    # SQLite tests each use QTemporaryDir, while the backend-contract test
    # uses its own disposable PostgreSQL schemas when enabled. PBKDF2 strength
    # is unchanged. Limit test concurrency for smaller machines.
    local jobs="${AOWIS_DB_TEST_JOBS:-4}"
    if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]] || (( jobs > 32 )); then
        printf 'Invalid AOWIS_DB_TEST_JOBS=%s (expected integer 1..32)\n' "$jobs" >&2
        return 2
    fi
    ctest --test-dir "$build_dir" --parallel "$jobs" --output-on-failure -L '^aowis$'
}

if [[ ! -f "${PROJECT_DIR}/external/AOWIS-SERVER-MODEL/CMakeLists.txt" ]]; then
    printf 'Model submodule is missing; run ./git_submodule_init.sh first.\n' >&2
    exit 1
fi

# --help has no side effects. Initialize Docker only for actual test runs.
if [[ "${1:-}" == '--help' || "${1:-}" == '-h' ]]; then
    usage
    exit 0
fi
if (( $# > 1 )) || [[ -n "${1:-}" && "${1:-}" != '--debug' &&
                      "${1:-}" != '--release' && "${1:-}" != '--both' ]]; then
    usage >&2
    exit 2
fi

trap cleanup_postgres EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
start_postgres_tests

case "${1:-}" in
    '') run_suite "${PROJECT_DIR}/build-linux-tests" '' ;;
    --debug) run_suite "${PROJECT_DIR}/build-linux-tests-debug" Debug ;;
    --release) run_suite "${PROJECT_DIR}/build-linux-tests-release" Release ;;
    --both) run_suite "${PROJECT_DIR}/build-linux-tests-debug" Debug; run_suite "${PROJECT_DIR}/build-linux-tests-release" Release ;;
esac
