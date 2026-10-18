# PostgreSQL integration tests (Docker Compose)

The normal `./compile_linux_tests.sh` now runs the existing PostgreSQL
backend-contract integration scenario **against a real PostgreSQL instance**
when Docker Engine and Docker Compose v2 are accessible. The test uses the
same public `Database` API as embedded SQLite. It covers schema constraints,
project/revision lifecycle, object history, undo/redo and hydraulic Model
save/load. It does not add another CTest executable: the live portion of
`aowis-db-backend-contract` is enabled by the test runner.

## Test container

The Compose definition lives in `tests/postgres/docker-compose.yaml`.

- PostgreSQL 17 Alpine (test-only fixed credentials and database name).
- Published **only to 127.0.0.1**, on a randomly allocated host port.
- Uses a temporary filesystem instead of a persistent volume.
- A health check runs before CTest can connect.
- The test script uses a unique Compose project per invocation and tears it
  down, including any volumes, on success or failure.
- Schema tests create disposable PostgreSQL schemas and remove them. No
  production database, application configuration, or shared port is used.

The first execution may download the official PostgreSQL image. The developer
must be able to execute `docker compose` without `sudo`. The script does not
silently escalate Docker privileges.

## Running

```bash
./compile_linux_tests.sh
```

Default `AOWIS_DB_TEST_POSTGRES=auto` means:

- Docker available: **start a real test database and run PostgreSQL tests**.
- Docker unavailable: warn explicitly and run the remaining SQLite/HTTP tests.
- Docker available but container startup or PostgreSQL tests fail: **fail the
  test run**; don't silently skip a broken PostgreSQL integration.

To require live PostgreSQL, for example in CI:

```bash
AOWIS_DB_TEST_POSTGRES=required ./compile_linux_tests.sh
```

For machines without Docker, to explicitly skip it:

```bash
AOWIS_DB_TEST_POSTGRES=off ./compile_linux_tests.sh
```

For an existing **dedicated disposable test database**, the test runner can
use it without starting Docker:

```bash
AOWIS_DB_TEST_PG_DB=aowis_test PGHOST=127.0.0.1 PGPORT=5432 \
PGUSER=aowis_test PGPASSWORD=your_test_password \
./compile_linux_tests.sh
```

The external test database user needs permission to create/drop schemas. Do
not point this at a production database. The backend contract only drops its
own randomly named schemas, but it exercises destructive rollback checks.

You can also run `--debug`, `--release` or `--both`. `--both` reuses the one
disposable container and automatically allocates distinct test schemas in each
run. Neither a database schema migration system nor a persistent PostgreSQL
Docker volume is introduced.

The test runner sets `AOWIS_DB_TEST_PG_REQUIRED=1` when live testing is
intended. The backend-contract test fails if the expected PostgreSQL database
name was not propagated to CTest, preventing a false green result from an
accidental skip.

## Default SQLite file

PostgreSQL testing doesn't modify normal SQLite configuration. On typical
Linux desktops, the existing Qt `QStandardPaths::AppDataLocation` default is
`~/.local/share/AOWIS-SERVER-DB/aowis.sqlite`, honoring `XDG_DATA_HOME`.
The INI `[Database] File` can override that path. Linux server-mode builds
instead default to `/var/lib/aowis-server-db/aowis.sqlite`.
