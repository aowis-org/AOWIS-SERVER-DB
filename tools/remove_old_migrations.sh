#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
# These files are intentionally removed from the development repository.
# ZIP extraction cannot delete pre-existing files, so run this once after
# applying the patch. It does not touch the SQLite databases themselves.
files=(
    src/sqlite_schema_migrations.cpp
    tests/sqlite_migration_test.cpp
    tests/legacy_sqlite_v1.h
    docs/backend-contract-v48.md
    docs/backend-contract-v49.md
    docs/backend-migrations-v50.md
    docs/backend-migrations-v51.md
    docs/backend-migrations-v52.md
    docs/backend-separation-v47.md
)
for file in "${files[@]}"; do
    rm -f -- "$project_root/$file"
done
printf 'Removed obsolete SQLite migration source, fixtures and notes.\n'
