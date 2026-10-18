# Configuration (V68)

The library exposes `aowis::db::Configuration::load()` using Qt's
`QSettings` **INI** format. The optional `aowis-db-server` executable reads
this configuration automatically. If no INI file exists, defaults are used;
reading it never writes or creates an INI.

Normal builds use `QStandardPaths::AppDataLocation` for the configuration and
SQLite file and `AppLocalDataLocation` for the log file. On a typical Linux
system with application name `AOWIS-SERVER-DB`, these are:

- `~/.local/share/AOWIS-SERVER-DB/config.ini`
- `~/.local/share/AOWIS-SERVER-DB/aowis.sqlite`
- `~/.local/share/AOWIS-SERVER-DB/logs/server.log`

Qt chooses the appropriate directory on macOS and Windows, and respects
`XDG_DATA_HOME` on Linux. The embedding application controls its Qt
application name; paths in the core therefore use the embedding app's data
location.

Building with `-DAOWIS_DB_SERVER_MODE=ON` uses these defaults on Linux:

- `/etc/aowis-server-db/config.ini` (configuration; not writable by the service)
- `/var/lib/aowis-server-db/aowis.sqlite` (writable database)
- `/var/log/aowis-server-db/server.log` (writable log)

On non-Linux systems the server-mode option keeps Qt platform paths.
Provision writable `/var/lib` and `/var/log` directories with appropriate
permissions when running as a dedicated service account.

## INI options

```ini
[Database]
File=/path/to/aowis.sqlite

[Logging]
File=/path/to/logs/server.log

[HTTP]
Port=8080
```

All options are optional. File overrides must be absolute paths; relative paths
and invalid ports are rejected. Database and log directories are created when
`aowis-db-server` starts, if the service account has permission. The log file
records Qt diagnostic messages and service startup/failures, in addition to
stderr. It is appended to, not truncated. Configure log rotation externally.

The server remains bound to localhost; there is no configurable remote bind
address in this patch. Credentials do **not** belong in the INI. PostgreSQL
continues to use libpq environment/service configuration and the
`Database::openPostgres()` API, not this SQLite-file setting.

`aowis-db-server` can now be started with no arguments. The existing
`aowis-db-server SQLITE_FILE [PORT]` command remains valid; positional
arguments override the matching INI options. `--config /absolute/path.ini`
selects another INI for development or service management.

No database migrations or schema versions are introduced.
