#ifndef AOWIS_DB_BUILTIN_IDENTITY_IDS_H
#define AOWIS_DB_BUILTIN_IDENTITY_IDS_H

namespace aowis::db::internal
{
// Built-in UUIDs are persisted identities, shared by fresh databases and
// fresh databases. Keep these identifiers stable between installations.
inline constexpr char security_audit_read_permission_uuid[] =
    "01a14202-2800-731d-a688-db84f1d900b5";
}

#endif // AOWIS_DB_BUILTIN_IDENTITY_IDS_H
