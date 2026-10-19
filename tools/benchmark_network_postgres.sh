#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
COMPOSE_FILE="${PROJECT_DIR}/tests/postgres/docker-compose.yaml"
COMPOSE_PROJECT="aowis-db-perf-$$"
STARTED=false

usage() {
    cat <<'HELP'
Usage: ./tools/benchmark_network_postgres.sh [PIPE_COUNT] [--revisions N] [--memory] [--deep-verify] [--profile]

Run the opt-in AOWIS network-persistence benchmark against a disposable
PostgreSQL 17 Docker Compose instance. Defaults to 1000 pipes. The container,
test schema and all benchmark data are destroyed at exit. By default the
benchmark finalizes one revision; --revisions N permits 1..100 revisions.
--memory records AOWIS client RSS checkpoints and high-water mark (Linux).
--deep-verify replays object journals against finalized Merkle snapshots.
--profile emits opt-in detailed storage phase timings to stderr.
PostgreSQL container memory is NOT included in the client RSS measurements.

Set AOWIS_DB_BENCH_BUILD_DIR to use a different configured CMake build
folder (default: build-linux-tests). No CTest tests are run.
HELP
}

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    usage
    exit 0
fi
count=1000
count_seen=false
revision_seen=false
revision_args=()
extra_args=()
while (( $# > 0 )); do
    case "$1" in
        --revisions)
            if [[ "$revision_seen" == true ]] || (( $# < 2 )) ||
               [[ ! "$2" =~ ^[1-9][0-9]*$ ]] || (( ${#2} > 3 )) ||
               (( 10#$2 > 100 )); then
                usage >&2
                exit 2
            fi
            revision_seen=true
            revision_args=(--revisions "$2")
            shift 2
            ;;
        --memory|--deep-verify|--profile)
            for existing in "${extra_args[@]}"; do
                if [[ "$existing" == "$1" ]]; then
                    usage >&2
                    exit 2
                fi
            done
            extra_args+=("$1")
            shift
            ;;
        *)
            if [[ "$count_seen" == true ]] ||
               [[ ! "$1" =~ ^[1-9][0-9]*$ ]] || (( ${#1} > 6 )) ||
               (( 10#$1 > 100000 )); then
                usage >&2
                exit 2
            fi
            count="$1"
            count_seen=true
            shift
            ;;
    esac
done
if ! command -v docker >/dev/null 2>&1 || ! docker compose version >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    printf 'Docker Engine and Compose v2 are required for the PostgreSQL benchmark.\n' >&2
    exit 1
fi

cleanup() {
    if [[ "$STARTED" == true ]]; then
        printf '=== Removing disposable PostgreSQL benchmark container ===\n'
        docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" down --volumes --remove-orphans || \
            printf 'WARNING: PostgreSQL cleanup failed: %s\n' "$COMPOSE_PROJECT" >&2
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf '=== Starting disposable PostgreSQL benchmark container ===\n'
STARTED=true
docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" up -d postgres
container_id="$(docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" ps -q postgres)"
if [[ -z "$container_id" ]]; then
    printf 'PostgreSQL benchmark container did not start.\n' >&2
    exit 1
fi
status=''
for (( attempt=0; attempt<90; ++attempt )); do
    status="$(docker inspect --format '{{.State.Health.Status}}' "$container_id")"
    if [[ "$status" == healthy ]]; then
        break
    fi
    if [[ "$status" == unhealthy ]]; then
        printf 'PostgreSQL benchmark container is unhealthy.\n' >&2
        exit 1
    fi
    sleep 1
done
if [[ "$status" != healthy ]]; then
    printf 'PostgreSQL benchmark container did not become healthy.\n' >&2
    exit 1
fi
endpoint="$(docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" port postgres 5432)"
port="${endpoint##*:}"
if [[ ! "$port" =~ ^[1-9][0-9]*$ ]] || (( port > 65535 )); then
    printf 'Invalid PostgreSQL host port: %s\n' "$endpoint" >&2
    exit 1
fi

export PGHOST=127.0.0.1 PGPORT="$port" PGUSER=aowis_test
export PGPASSWORD=aowis_test_only_local PGSSLMODE=disable
export AOWIS_DB_TEST_PG_DB=aowis_test
build_dir="${AOWIS_DB_BENCH_BUILD_DIR:-${PROJECT_DIR}/build-linux-tests}"
if [[ "$build_dir" != /* ]]; then
    build_dir="${PROJECT_DIR}/${build_dir}"
fi
if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    printf 'CMake build directory not configured: %s\n' "$build_dir" >&2
    printf 'Run ./compile_linux_tests.sh once, or set AOWIS_DB_BENCH_BUILD_DIR.\n' >&2
    exit 1
fi
cmake --build "$build_dir" --target aowis-db-network-benchmark
printf '=== PostgreSQL network benchmark: %s pipes ===\n' "$count"
"${build_dir}/aowis-db-network-benchmark" "$count" "${revision_args[@]}" "${extra_args[@]}" --postgres
