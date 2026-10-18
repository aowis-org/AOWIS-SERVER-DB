# Configuration and startup

Both `aowis-server-db` and `aowis-db-admin` use the same `QSettings` INI
configuration. If the file does not exist, Qt platform-specific defaults
apply. Use `--config /absolute/path/config.ini` to load a different INI.

## Default locations

In a desktop build, Qt `QStandardPaths::AppDataLocation` supplies the INI and
SQLite database directory, while `AppLocalDataLocation` supplies the log
directory. On a normal Linux desktop both are under `~/.local/share` unless
`XDG_DATA_HOME` overrides that. The usual defaults are:

| Resource | Linux desktop |
| --- | --- |
| INI | `~/.local/share/AOWIS-SERVER-DB/config.ini` |
| Database | `~/.local/share/AOWIS-SERVER-DB/aowis.sqlite` |
| Log | `~/.local/share/AOWIS-SERVER-DB/logs/server.log` |

Build with `-DAOWIS_DB_SERVER_MODE=ON` on Linux to use:

| Resource | Linux system server |
| --- | --- |
| INI | `/etc/aowis-server-db/config.ini` |
| Database | `/var/lib/aowis-server-db/aowis.sqlite` |
| Log | `/var/log/aowis-server-db/server.log` |

Windows and macOS use their own Qt application data locations; Linux system
paths are never hardcoded on those platforms.

## SQLite configuration

```ini
[Database]
Backend=sqlite
File=/absolute/path/aowis.sqlite

[Logging]
File=/absolute/path/server.log

[HTTP]
Port=8080
```

All these keys can be omitted to use defaults. Configured file paths must
be absolute, and the HTTP port must be 1 through 65535. The application
creates writable SQLite and log directories when permitted.

## PostgreSQL configuration

```ini
[Database]
Backend=postgres
Name=aowis
Schema=aowis

[HTTP]
Port=8080
```

The PostgreSQL database and schema must already exist; AOWIS initializes its
current tables inside an empty selected schema. Use libpq configuration and
credentials (`PGHOST`, `PGPORT`, `PGUSER`, `.pgpass`, etc.). A PostgreSQL config
cannot include `Database/File`, and SQLite cannot include `Name` or `Schema`.

## Command line

```bash
./build-linux/aowis-server-db
./build-linux/aowis-server-db --config /absolute/path/config.ini
./build-linux/aowis-server-db /path/to/another.sqlite
./build-linux/aowis-db-admin bootstrap administrator
./build-linux/aowis-db-admin --config /absolute/path/config.ini audit-verify ADMIN_UUID
```

The server's optional positional SQLite filename overrides the INI *for this
run only*. It is rejected in PostgreSQL mode. `aowis-db-admin` does not accept
a positional database filename; switch databases through `--config`. The
listener binds to `127.0.0.1`; no positional HTTP port is supported.

The INI is **not** a secret store. Protect log and database access with normal
OS permissions. System service data and logs should be writable by the service
account, not the entire system.
