# Building and testing

The standard Linux test script configures/builds the registered CTest programs
and runs the `aowis`-labelled suite. Use the checked-out Model submodule,
Qt 6 Core/Sql, OpenSSL and a C++20 compiler. The HTTP tests additionally need
Qt Network/HttpServer. See the root [README](../README.md) for dependencies.

```bash
./git_submodule_init.sh
./compile_linux.sh
./compile_linux_tests.sh
AOWIS_DB_TEST_POSTGRES=required ./compile_linux_tests.sh
```

Use `./compile_linux_tests.sh --debug`, `--release` or `--both` for separate
build configurations. The test target is `aowis-db-tests`. No new build work
from Ninja is expected when source timestamps and contents have not changed.

## Live PostgreSQL

The test runner uses `tests/postgres/docker-compose.yaml` to launch a disposable
PostgreSQL instance, wait for readiness, expose it only on a random localhost
port, execute the normal test suite and remove the container/volumes. The
backend-contract test detects `AOWIS_DB_TEST_PG_DB` and runs the PostgreSQL
integration scenarios. A skipped PostgreSQL scenario is not evidence of
PostgreSQL compatibility.

| Environment | Behavior |
| --- | --- |
| `AOWIS_DB_TEST_POSTGRES=auto` (default) | Use Docker when available, otherwise report an explicit skip |
| `AOWIS_DB_TEST_POSTGRES=required` | Fail if Docker or PostgreSQL testing is unavailable |
| `AOWIS_DB_TEST_POSTGRES=off` | Skip PostgreSQL explicitly |
| `AOWIS_DB_TEST_PG_DB=name` | Connect to a dedicated existing test DB via libpq instead of Docker |
| `AOWIS_DB_TEST_JOBS=4` | Configure concurrent CTest jobs |

The external PostgreSQL database must be disposable and permit test schema
creation and deletion. Never point the tests at a production database.

## Source patch workflow

From the repository root:

```bash
./tools/apply_patch.sh ~/Downloads/PATCH_FILENAME.zip
./tools/zip_db.sh
AOWIS_DB_TEST_POSTGRES=required ./compile_linux_tests.sh
```

`apply_patch.sh` refreshes the modification timestamps of extracted files and
directories to ensure Ninja rebuilds them. `zip_db.sh` creates `../db.zip`
with the current source tree and Model submodule; it excludes Git metadata and
ignored build files. Keep source archives and test logs together when asking
for subsequent changes.
