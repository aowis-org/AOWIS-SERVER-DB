# V34: one-way session token redaction

The session table is append-only with respect to identity, creation, expiration,
and revocation metadata. V13 of the SQLite schema allows **one additional
one-way operation**: replacing the 32-byte token digest of a spent session
with the public, UUID-derived `AOWIS-REDACT-V1!` marker (16 bytes) followed by
the session UUID (16 bytes). The marker cannot authenticate as a bearer token.

A session may be redacted only after expiry or revocation, and only when its
expiry/revocation time is at or before the selected cutoff. Ordinary SQL is
restricted by the V13 trigger: active session digests cannot be redacted,
and any redacted digest cannot be restored. In particular, **session rows are
not deleted**. UUIDs, actors, timestamps, and the immutable SHA-256 security
audit chain are preserved.

## Trusted local maintenance

```sh
./build-linux/aowis-db-admin redact-sessions DATABASE.sqlite ADMIN_UUID [MIN_AGE_DAYS [LIMIT]]
```

By default, `MIN_AGE_DAYS=30` and `LIMIT=100` (maximum 500). The cutoff is
computed from the local machine's clock in UTC milliseconds. Run repeatedly
until it reports zero. A cutoff of zero days is supported for test/local
administration purposes; ordinary deployments should use an explicit retention
period appropriate to their policy.

The operation requires system-level `security.manage`, takes a SQLite write
transaction, and writes a separate `session.token.redact` security audit event
for each affected session. All redactions and their audit events commit as one
transaction, or none do. No HTTP endpoint is exposed, and no background task
is introduced. The database operator controls retention policy and backups.

**Limitation:** SQLite WAL files and existing backups may retain earlier bytes.
For physical data sanitization, operational procedures must address backups,
WAL checkpointing and storage-level encryption separately. The project revision
and model integrity formats are unchanged.
