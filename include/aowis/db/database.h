#ifndef AOWIS_DB_DATABASE_H
#define AOWIS_DB_DATABASE_H

#include <aowis/db/identity.h>
#include <aowis/model/hydraulic/network_hydraulic.h>

#include <memory>
#include <optional>
#include <functional>

#include <QByteArray>
#include <QSqlDatabase>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

namespace aowis::db
{
class StorageBackend;

// Bearer token is returned exactly once and never stored in plaintext.
struct IssuedSession
{
    QUuid uuid;
    QUuid user_uuid;
    QByteArray bearer_token; // 64 lowercase hexadecimal characters (256 bits)
    qint64 expires_at_ms = 0;
};

// Introspection of one active bearer session; never contains credentials.
// One-time reset authority issued by a trusted administrator. The secret is
// returned once; only a domain-separated SHA-256 digest is stored.
struct IssuedRecoveryToken
{
    QUuid uuid;
    QUuid user_uuid;
    QByteArray token; // 64 lowercase hex characters (256 random bits)
    qint64 expires_at_ms = 0;
};

struct ActiveSession
{
    QUuid uuid;
    QUuid user_uuid;
    qint64 expires_at_ms = 0;
};

// Safe self-service session metadata. Never contains bearer tokens, hashes,
// user-controlled labels, or another user's sessions.
struct OwnSessionInfo
{
    QUuid uuid;
    qint64 created_at_ms = 0;
    qint64 expires_at_ms = 0;
    bool current = false;
};

// An append-only administration event. The hash includes the previous event
// hash, so audit history is verified independently of project revisions.
struct SecurityAuditEvent
{
    QUuid uuid;
    QUuid actor_uuid;
    QUuid subject_uuid;
    QString action;
    qint64 sequence = 0;
    qint64 occurred_at_ms = 0;
    QByteArray previous_sha256;
    QByteArray chain_sha256;
};

struct SecurityAuditReport
{
    bool valid = false;
    qint64 checked_events = 0;
    qint64 failed_sequence = 0;
    QString error;
};

struct ProjectInfo
{
    QUuid uuid;
    QString name;
    QUuid created_by_uuid; // Creator identity.
};

struct ProjectCreation
{
    QUuid project_uuid;
    QUuid main_branch_uuid;
    QUuid working_revision_uuid;
};

struct BranchInfo
{
    QUuid uuid;
    QUuid project_uuid;
    QString name;
    QUuid head_revision_uuid;
    QUuid created_by_uuid; // Creator identity.
};

struct RevisionInfo
{
    QUuid uuid;
    QUuid project_uuid;
    QUuid branch_uuid;
    // Ordered parent list. Multiple parents allow future merge commits.
    QVector<QUuid> parent_revision_uuids;
    qint64 number = 0;
    qint64 created_at_ms = 0;
    bool finalized = false;
    qint64 generation = 0;
    QByteArray snapshot_sha256;
    QByteArray chain_sha256;
    QUuid created_by_uuid;
    QUuid finalized_by_uuid;
    int hash_version = 1;
};

// Read-only DAG comparison. Merge bases are the maximal common ancestors:
// several are possible for criss-cross merges. Only finalized revisions are
// eligible as merge bases. No merge or model mutation is performed.
// Ancestor relationships include equality, so equal revisions set both flags.
struct RevisionComparison
{
    QUuid project_uuid;
    QUuid left_revision_uuid;
    QUuid right_revision_uuid;
    bool left_is_ancestor_of_right = false;
    bool right_is_ancestor_of_left = false;
    QVector<QUuid> merge_base_revision_uuids;
};

struct Finalization
{
    QUuid finalized_revision_uuid;
    QByteArray snapshot_sha256;
    QByteArray chain_sha256;
    QUuid new_working_revision_uuid;
};

struct HistoryEvent
{
    QUuid uuid;
    QUuid revision_uuid;
    QUuid actor_uuid;
    QUuid target_event_uuid; // Populated for undo/redo events.
    qint64 sequence = 0;
    qint64 occurred_at_ms = 0;
    QString kind; // "replace", "undo", "redo"
    bool had_previous_snapshot = false;
    QByteArray before_sha256;
    QByteArray after_sha256;
};

// Per-object version is independent of the branch-wide revision generation.
// Deleted objects remain as tombstones to reject stale recreate operations.
struct ModelObject
{
    QUuid uuid;
    QString kind;
    QByteArray content;
    qint64 version = 0;
    bool deleted = false;
};

// A read-only three-way comparison of canonical object states. Each entry
// represents an entity changed relative to the single finalized merge base.
// Deleted objects are represented by std::nullopt (tombstones are not live
// entities). Object version counters are not part of content equality.
enum class ObjectMergeKind
{
    LeftOnly,   // Only left changed relative to the base.
    RightOnly,  // Only right changed relative to the base.
    BothSame,   // Both changed to identical canonical state (including delete).
    Conflict,   // Incompatible edits; caller must explicitly resolve them.
    FieldsMerged // Independent top-level field edits; synthesized canonical result.
};

struct ObjectMergeChange
{
    QUuid object_uuid;
    ObjectMergeKind kind = ObjectMergeKind::Conflict;
    std::optional<ModelObject> base;
    std::optional<ModelObject> left;
    std::optional<ModelObject> right;
    // Populated only for supported typed entities changed on both sides.
    // Typed metadata subfields are compared independently. Distinct appended
    // metadata tags may merge; other collections remain atomic.
    QStringList conflicting_fields;
    std::optional<ModelObject> automatically_merged;
};

struct ObjectMergePreview
{
    QUuid project_uuid;
    QUuid base_revision_uuid;
    QUuid left_revision_uuid;
    QUuid right_revision_uuid;
    // Captured from the same read transaction as the entity states. A future
    // merge operation must check these generations again under its write lock.
    qint64 left_generation = 0;
    qint64 right_generation = 0;
    // Transient SHA-256 of every physical left object row, including version
    // and tombstone data. Captured with the preview's SQLite read snapshot.
    QByteArray left_physical_sha256;
    QVector<ObjectMergeChange> changes; // Sorted by UUID bytes.
    qsizetype conflict_count = 0;
};

struct ObjectChange
{
    QUuid uuid;
    QUuid revision_uuid;
    QUuid object_uuid;
    QUuid actor_uuid;
    QString kind;
    QString action; // put / delete
    qint64 sequence = 0;
    qint64 occurred_at_ms = 0;
    qint64 before_version = -1;
    qint64 after_version = 0;
};

// A batch is one user-visible action. Edits are applied in vector order, all
// within a single SQLite transaction; all expected versions must match.
// For deletion, set content to std::nullopt (kind may be empty).
// A conflict can select one entire side or give a choice for every conflicting
// top-level field. Nonconflicting fields are always merged automatically when
// using field choices. FieldsMerged changes require no manual resolution.
enum class ObjectMergeSide { Left, Right };

struct ObjectMergeFieldResolution
{
    QString field_name; // Exact (possibly dotted) name from conflicting_fields.
    ObjectMergeSide side = ObjectMergeSide::Left;
};

struct ObjectMergeResolution
{
    QUuid object_uuid;
    ObjectMergeSide side = ObjectMergeSide::Left; // Whole entity, when field_resolutions is empty.
    // When nonempty, must cover each conflicting field exactly once; side is
    // ignored and all independent changes from both branches are preserved.
    QVector<ObjectMergeFieldResolution> field_resolutions;
};

struct ObjectEdit
{
    QUuid object_uuid;
    QString kind;
    std::optional<QByteArray> content;
    qint64 expected_version = -1;
};

struct ObjectGroup
{
    QUuid uuid;
    QUuid revision_uuid;
    QUuid actor_uuid;
    QUuid target_group_uuid; // For undo/redo; null for forward edits.
    QString action; // edit / undo / redo
    qint64 first_sequence = 0;
    qint64 last_sequence = 0;
    qint64 occurred_at_ms = 0;
    QVector<QUuid> object_event_uuids;
};

struct IntegrityReport
{
    bool valid = false;
    qsizetype checked_revisions = 0;
    QUuid failed_revision_uuid;
    QString error;
};

// A connection is bound to the constructing thread, like QSqlDatabase itself.
// A single Database is not shared across threads. Each thread needs its own
// connection, while SQLite serializes the writers using BEGIN IMMEDIATE.
class Database final
{
public:
    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;

    bool openSqlite(const QString &file_path, QString *error = nullptr);
    // Open a PostgreSQL database using QPSQL/libpq credentials. schema_name
    // must name an existing schema; the default deployment schema is "public".
    // The same domain operations and permission rules are used as for SQLite.
    bool openPostgres(const QString &database_name, const QString &schema_name,
                      QString *error = nullptr);
    void close();
    bool isOpen() const;

    // Explicit local-only one-time bootstrap. An HTTP route must never call
    // this method. Refuses any subsequent call, even by an administrator.
    std::optional<UserInfo> bootstrapAdministrator(const QString &username, QString *error = nullptr);

    // Initial password enrollment and administrative resets require a trusted
    // administrator. Only a password verifier, random salt and work factor are
    // stored, never plaintext. Resets revoke all existing user sessions.
    bool setUserPassword(const OperationContext &admin, const QUuid &user_uuid,
                         const QByteArray &password, QString *error = nullptr);
    // Change the holder's own password. Both the authenticated bearer token
    // and their CURRENT password are required. On success all user sessions,
    // including the caller's, are revoked in the audited transaction.
    // No arbitrary user UUID can be supplied by this self-service API.
    bool changeOwnPassword(const QByteArray &bearer_token, const QByteArray &current_password,
                           const QByteArray &new_password, QString *error = nullptr);
    // Credentials are verified before a new revocable bearer session is issued.
    // Never return a distinct error for an unknown user or bad password.
    std::optional<IssuedSession> loginWithPassword(const QString &username,
        const QByteArray &password, qint64 lifetime_seconds = 3600, QString *error = nullptr);

    // Recovery tokens require system-level security.manage to issue. They are
    // valid once and for at most one hour. Delivery and real-world identity
    // checks must be performed by the trusted operator out of band.
    std::optional<IssuedRecoveryToken> issuePasswordRecovery(const OperationContext &admin,
        const QUuid &user_uuid, qint64 lifetime_seconds = 900, QString *error = nullptr);
    // Unauthenticated redemption: possession of the unguessable recovery
    // token authorizes replacing that one user's password. Revokes sessions.
    bool redeemPasswordRecovery(const QByteArray &token, const QByteArray &new_password,
        QString *error = nullptr);

    // Each user may hold up to 32 concurrent unexpired, unrevoked sessions.
    // This quota applies to both password login and administrator issuance.
    // Session issuance/revocation are restricted to system administrators.
    // No HTTP route exposes these operations. A trusted local host can
    // provision the first credentials after administrator bootstrap.
    std::optional<IssuedSession> issueSession(const OperationContext &admin,
        const QUuid &user_uuid, qint64 lifetime_seconds, QString *error = nullptr);
    bool revokeSession(const OperationContext &admin, const QUuid &session_uuid,
        QString *error = nullptr);
    // Retain session identity and history, but erase spent bearer-token digests.
    // cutoff_ms must be a past UTC millisecond timestamp; max_count is 1..500.
    // Every scrubbed session receives its own immutable security audit event.
    // An active session is never scrubbed. Local administrator operation only.
    std::optional<int> redactSpentSessionTokens(const OperationContext &admin,
        qint64 cutoff_ms, int max_count = 100, QString *error = nullptr);
    bool disableUser(const OperationContext &admin, const QUuid &user_uuid,
        QString *error = nullptr);
    bool enableUser(const OperationContext &admin, const QUuid &user_uuid,
        QString *error = nullptr);
    bool removeGroupMember(const OperationContext &admin, const QUuid &membership_uuid,
        QString *error = nullptr);
    bool revokeRolePermission(const OperationContext &admin, const QUuid &grant_uuid,
        QString *error = nullptr);
    bool revokeRoleAssignment(const OperationContext &admin, const QUuid &assignment_uuid,
        QString *error = nullptr);
    // Bearer tokens are credentials; resolve to a context, never trust an
    // HTTP-supplied user UUID. Do not return error details to remote clients.
    std::optional<OperationContext> authenticateSession(const QByteArray &bearer_token,
        QString *error = nullptr) const;
    // Self-service session metadata and logout. Tokens remain confidential;
    // a caller can revoke only the session represented by their own token.
    std::optional<ActiveSession> activeSession(const QByteArray &bearer_token,
        QString *error = nullptr) const;
    bool logoutSession(const QByteArray &bearer_token, QString *error = nullptr);
    // Only sessions belonging to the bearer holder; includes only unrevoked,
    // unexpired sessions. Keyset pagination is by the immutable session UUID.
    // Pass a null after_uuid to begin. A valid empty result is not an error.
    QVector<OwnSessionInfo> ownSessions(const QByteArray &bearer_token,
        const QUuid &after_uuid = {}, int limit = 100, QString *error = nullptr) const;
    // Revoke only one of the bearer's own sessions, including their current
    // session. Ownership is rechecked inside the same transaction as auditing.
    bool revokeOwnSession(const QByteArray &bearer_token, const QUuid &session_uuid,
        QString *error = nullptr);
    // Atomically revoke all OTHER active sessions belonging to the bearer
    // holder. The current session stays usable. Returns the number revoked,
    // or nullopt on authentication, storage, or audit failure.
    std::optional<qint64> revokeOtherSessions(const QByteArray &bearer_token,
        QString *error = nullptr);

    // Requires security.audit.read at system scope. Pagination is by durable
    // sequence (exclusive lower bound), not by mutable offsets.
    QVector<SecurityAuditEvent> securityAudit(const OperationContext &actor,
        qint64 after_sequence, int limit, QString *error = nullptr) const;
    SecurityAuditReport verifySecurityAudit(const OperationContext &actor) const;

    // Administrative catalog: stable UUID keyset pages, sorted by UUID.
    // Empty results are valid; use error to distinguish them from failures.
    // All require the system-level security.manage permission. Only active
    // memberships/grants are shown unless include_inactive is requested.
    QVector<UserInfo> securityUsers(const OperationContext &actor,
        const SecurityListOptions &options = {}, QString *error = nullptr) const;
    QVector<GroupInfo> securityGroups(const OperationContext &actor,
        const SecurityListOptions &options = {}, QString *error = nullptr) const;
    QVector<RoleInfo> securityRoles(const OperationContext &actor,
        const SecurityListOptions &options = {}, QString *error = nullptr) const;
    QVector<PermissionInfo> securityPermissions(const OperationContext &actor,
        const SecurityListOptions &options = {}, QString *error = nullptr) const;
    QVector<GroupMembershipInfo> securityGroupMembers(const OperationContext &actor,
        const QUuid &group_uuid, const SecurityListOptions &options = {},
        QString *error = nullptr) const;
    QVector<RolePermissionGrantInfo> securityRolePermissions(const OperationContext &actor,
        const QUuid &role_uuid, const SecurityListOptions &options = {},
        QString *error = nullptr) const;
    QVector<RoleAssignmentInfo> securityRoleAssignments(const OperationContext &actor,
        PrincipalKind kind, const QUuid &principal_uuid,
        const SecurityListOptions &options = {}, QString *error = nullptr) const;

    std::optional<UserInfo> createUser(const OperationContext &actor, const QString &username, QString *error = nullptr);
    std::optional<GroupInfo> createGroup(const OperationContext &actor, const QString &name, QString *error = nullptr);
    std::optional<RoleInfo> createRole(const OperationContext &actor, const QString &key, QString *error = nullptr);
    std::optional<PermissionInfo> createPermission(const OperationContext &actor, const QString &key, QString *error = nullptr);
    std::optional<RoleInfo> role(const OperationContext &actor, const QString &key, QString *error = nullptr) const;
    std::optional<PermissionInfo> permission(const OperationContext &actor, const QString &key, QString *error = nullptr) const;
    std::optional<QUuid> addGroupMember(const OperationContext &actor, const QUuid &group_uuid, const QUuid &user_uuid, QString *error = nullptr);
    std::optional<QUuid> grantRolePermission(const OperationContext &actor, const QUuid &role_uuid, const QUuid &permission_uuid, QString *error = nullptr);
    std::optional<QUuid> assignRole(const OperationContext &actor, PrincipalKind kind, const QUuid &principal_uuid,
                                   const QUuid &role_uuid, const AccessScope &scope, QString *error = nullptr);
    bool hasPermission(const OperationContext &actor, const QString &permission_key,
                       const AccessScope &scope, QString *error = nullptr) const;

    std::optional<ProjectCreation> createProject(const OperationContext &actor, const QString &name, QString *error = nullptr);
    std::optional<ProjectInfo> project(const OperationContext &actor, const QUuid &project_uuid, QString *error = nullptr) const;
    QVector<ProjectInfo> projects(const OperationContext &actor, QString *error = nullptr) const;
    QVector<BranchInfo> branches(const OperationContext &actor, const QUuid &project_uuid, QString *error = nullptr) const;
    QVector<RevisionInfo> revisions(const OperationContext &actor, const QUuid &branch_uuid, QString *error = nullptr) const;
    std::optional<BranchInfo> createBranch(const OperationContext &actor, const QUuid &project_uuid,
                                           const QString &name, const QUuid &base_finalized_revision_uuid,
                                           QString *error = nullptr);
    std::optional<BranchInfo> branch(const OperationContext &actor, const QUuid &branch_uuid, QString *error = nullptr) const;
    std::optional<RevisionInfo> revision(const OperationContext &actor, const QUuid &revision_uuid, QString *error = nullptr) const;
    // Compare two revisions in one project (working heads are allowed).
    // Requires project.read on BOTH source branches. Returns all maximal
    // FINALIZED common ancestors, not an arbitrary single merge base.
    std::optional<RevisionComparison> compareRevisions(const OperationContext &actor,
        const QUuid &left_revision_uuid, const QUuid &right_revision_uuid,
        QString *error = nullptr) const;

    // Preview a three-way merge of canonical object states without modifying
    // revisions. Requires exactly one finalized common ancestor and object
    // mode on all three revisions. The caller needs project.read on left,
    // right AND the base branch (which might be a third private branch).
    // Supports preview of field conflicts; multiple merge bases remain unsupported.
    std::optional<ObjectMergePreview> previewObjectMerge(const OperationContext &actor,
        const QUuid &left_revision_uuid, const QUuid &right_revision_uuid,
        QString *error = nullptr) const;

    // Memory-bounded three-way preview for remote clients. Streams unchanged
    // entities without retaining them, rejects once either limit is reached,
    // and returns no partial result. The payload budget counts canonical
    // content from all three states of each changed entity. This does not
    // change the unrestricted core merge-commit behavior.
    std::optional<ObjectMergePreview> previewObjectMergeBounded(const OperationContext &actor,
        const QUuid &left_revision_uuid, const QUuid &right_revision_uuid,
        qsizetype max_changes, qsizetype max_changed_payload_bytes,
        QString *error = nullptr) const;

    // Finalize an entity-level merge on a canonical working revision, including
    // one with existing local edits. Its sole finalized parent becomes the
    // first merge parent; the working head's current objects are the left
    // comparison input. The right input is finalized on another branch.
    // Local journal/groups are preserved and any merge edits form a new group.
    // Conflicts require explicit whole-entity or per-field choices. Both head UUID and generation
    // are required for compare-and-swap; no local edit is silently overwritten.
    std::optional<Finalization> mergeObjectRevisions(const OperationContext &actor,
        const QUuid &target_branch_uuid, const QUuid &expected_working_revision_uuid,
        qint64 expected_generation, const QUuid &right_finalized_revision_uuid,
        const QVector<ObjectMergeResolution> &resolutions,
        const QString &title, const QString &message, QString *error = nullptr);

    // Legacy opaque whole-snapshot path; object-mode revisions use typed
    // persistence and the separate grouped object journal.
    bool replaceWorkingSnapshot(const OperationContext &actor, const QUuid &branch_uuid,
                                qint64 expected_generation, const QByteArray &canonical_snapshot,
                                QString *error = nullptr);
    std::optional<QByteArray> snapshot(const OperationContext &actor, const QUuid &revision_uuid,
                                       QString *error = nullptr) const;
    std::optional<Finalization> finalize(const OperationContext &actor, const QUuid &branch_uuid,
                                         qint64 expected_generation, const QString &title,
                                         const QString &message, QString *error = nullptr);
    IntegrityReport verifyProject(const OperationContext &actor, const QUuid &project_uuid) const;

    // Atomic, generation-checked complete network synchronization. Each changed
    // object produces an immutable object-history event. Network root
    // stores the ordered entity UUID lists for lossless reconstruction.
    bool saveHydraulicNetwork(const OperationContext &actor, const QUuid &branch_uuid,
                              qint64 expected_generation, const NetworkHydraulic &network,
                              QString *error = nullptr);
    std::optional<NetworkHydraulic> loadHydraulicNetwork(const OperationContext &actor,
                              const QUuid &revision_uuid, const QUuid &network_uuid,
                              QString *error = nullptr) const;

    // Low-level object persistence accepts canonical bytes. Typed AOWIS Model
    // codecs and grouped object history are provided in this library.
    // expected_version=-1 means the UUID must not have been used in this revision.
    // Object mutations are serialized by SQLite, but do not require the caller
    // to supply the branch-wide revision generation.
    std::optional<ModelObject> object(const OperationContext &actor, const QUuid &revision_uuid,
                                      const QUuid &object_uuid, QString *error = nullptr) const;
    QVector<ModelObject> objects(const OperationContext &actor, const QUuid &revision_uuid,
                                  QString *error = nullptr) const;
    bool putObject(const OperationContext &actor, const QUuid &branch_uuid,
                   const QUuid &object_uuid, const QString &kind, const QByteArray &canonical_bytes,
                   qint64 expected_version, QString *error = nullptr);
    bool deleteObject(const OperationContext &actor, const QUuid &branch_uuid,
                      const QUuid &object_uuid, qint64 expected_version, QString *error = nullptr);
    // Full atomic batch with branch-generation CAS. Returns the durable
    // action UUID. New edits after undo invalidate the redo stack.
    std::optional<QUuid> commitObjectGroup(const OperationContext &actor,
                             const QUuid &branch_uuid, qint64 expected_generation,
                             const QVector<ObjectEdit> &edits, QString *error = nullptr);
    QVector<ObjectGroup> objectGroupsForRevision(const OperationContext &actor,
                             const QUuid &revision_uuid, QString *error = nullptr) const;
    std::optional<QUuid> undoObjectGroup(const OperationContext &actor,
                             const QUuid &branch_uuid, qint64 expected_generation,
                             QString *error = nullptr);
    std::optional<QUuid> redoObjectGroup(const OperationContext &actor,
                             const QUuid &branch_uuid, qint64 expected_generation,
                             QString *error = nullptr);

    QVector<ObjectChange> objectHistoryForRevision(const OperationContext &actor,
                         const QUuid &revision_uuid, QString *error = nullptr) const;

    // Durable, append-only working revision history. Each change (including
    // undo and redo) is persisted atomically with the working snapshot.
    QVector<HistoryEvent> history(const OperationContext &actor, const QUuid &branch_uuid,
                                  QString *error = nullptr) const;
    QVector<HistoryEvent> historyForRevision(const OperationContext &actor, const QUuid &revision_uuid,
                                             QString *error = nullptr) const;
    // Undo/redo return the UUID of the original change they compensated,
    // not the UUID of the newly appended compensating history event.
    std::optional<QUuid> undoLastChange(const OperationContext &actor, const QUuid &branch_uuid,
                                       qint64 expected_generation, QString *error = nullptr);
    std::optional<QUuid> redoLastChange(const OperationContext &actor, const QUuid &branch_uuid,
                                       qint64 expected_generation, QString *error = nullptr);

private:
    bool exec(const QString &sql, QString *error = nullptr) const;
    bool beginWrite(QString *error = nullptr) const;
    bool commit(QString *error = nullptr) const;
    void rollback() const;
    bool initializeSchema(QString *error);
    bool seedBuiltinSecurity(QString *error);
    bool writeSecurityAuditEvent(const QUuid &actor_uuid, const QString &action,
                                 const QUuid &subject_uuid, QString *error);
    bool auditedSecurityMutation(const QUuid &actor_uuid, const QString &action,
                                 const QUuid &subject_uuid, const std::function<bool()> &mutation,
                                 QString *error);
    std::optional<QUuid> compensateObjectGroup(const OperationContext &actor,
                      const QUuid &branch_uuid, qint64 expected_generation, bool redo,
                      QString *error);
    // The manifest includes the exact object-change journal digest, so it is
    // covered by the finalized SHA-256 revision chain.
    // Internal reads do not require project.read: an actor authorized to edit
    // must still have their resulting hydraulic graph validated atomically.
    std::optional<NetworkHydraulic> loadHydraulicNetworkUnchecked(
        const QUuid &revision_uuid, const QUuid &network_uuid, QString *error) const;
    bool validateTouchedHydraulicNetworks(const QUuid &revision_uuid,
                                          const QSet<QUuid> &touched_uuids,
                                          QString *error) const;
    std::optional<QByteArray> objectManifest(const QUuid &revision_uuid, QString *error) const;
    std::optional<int> objectMode(const QUuid &revision_uuid, QString *error) const;
    bool copyObjects(const QUuid &from_revision, const QUuid &to_revision, QString *error);
    bool replaceNetworkMembership(const QUuid &revision_uuid, const QUuid &network_uuid,
                                  const QSet<QUuid> &entity_uuids, QString *error) const;
    bool replaceHydraulicReferences(const QUuid &revision_uuid,
                                    const NetworkHydraulic &network, QString *error) const;
    bool canSkipHydraulicRevalidation(const QUuid &revision_uuid,
                                      const QUuid &entity_uuid, QString *error) const;
    std::optional<QUuid> writeObjectGroupInTransaction(const RevisionInfo &revision,
                            const OperationContext &actor, const QString &action,
                            const QUuid &target_uuid, qint64 before_generation,
                            QString *error);
    bool modifyObject(const OperationContext &actor, const QUuid &branch_uuid,
                      const QUuid &object_uuid, const QString &kind,
                      const std::optional<QByteArray> &bytes, qint64 expected_version,
                      QString *error);
    // Must be called inside an existing BEGIN IMMEDIATE transaction.
    bool modifyObjectInTransaction(const OperationContext &actor, const QUuid &branch_uuid,
                      const QUuid &object_uuid, const QString &kind,
                      const std::optional<QByteArray> &bytes, qint64 expected_version,
                      QString *error);
    bool applySnapshotOperation(const RevisionInfo &revision, const QUuid &actor_uuid,
                                const std::optional<QByteArray> &new_snapshot,
                                const QString &kind, const QUuid &target_uuid,
                                QString *error);
    std::optional<QByteArray> historyDigest(const QUuid &revision_uuid,
                                             QString *error) const;
    bool requirePermission(const OperationContext &actor, const QString &key,
                           const AccessScope &scope, QString *error) const;

    // Only internally accessible, after authorization at the public boundary.
    std::optional<ProjectCreation> createProjectUnchecked(const QUuid &actor_uuid, const QString &name, QString *error);
    std::optional<ProjectInfo> projectUnchecked(const QUuid &project_uuid, QString *error) const;
    QVector<ProjectInfo> projectsUnchecked(QString *error) const;
    QVector<BranchInfo> branchesUnchecked(const QUuid &project_uuid, QString *error) const;
    QVector<RevisionInfo> revisionsUnchecked(const QUuid &branch_uuid, QString *error) const;
    std::optional<BranchInfo> createBranchUnchecked(const QUuid &actor_uuid, const QUuid &project_uuid,
                                                   const QString &name, const QUuid &base_revision_uuid, QString *error);
    std::optional<BranchInfo> branchUnchecked(const QUuid &branch_uuid, QString *error) const;
    std::optional<RevisionInfo> revisionUnchecked(const QUuid &revision_uuid, QString *error) const;
    bool replaceWorkingSnapshotUnchecked(const QUuid &actor_uuid, const QUuid &branch_uuid,
                                         qint64 expected_generation,
                                         const QByteArray &snapshot, QString *error);
    std::optional<QByteArray> snapshotUnchecked(const QUuid &revision_uuid, QString *error) const;
    // Caller holds a BEGIN IMMEDIATE transaction; never commits or rolls back.
    std::optional<Finalization> finalizeInWriteTransaction(const QUuid &actor_uuid,
        const QUuid &branch_uuid, qint64 expected_generation, const QString &title,
        const QString &message, QString *error);
    std::optional<Finalization> finalizeUnchecked(const QUuid &actor_uuid, const QUuid &branch_uuid,
                                                 qint64 expected_generation, const QString &title,
                                                 const QString &message, QString *error);
    IntegrityReport verifyProjectUnchecked(const QUuid &project_uuid) const;

    bool openWithBackend(std::unique_ptr<StorageBackend> backend,
                         const QString &location, QString *error);

    QString connection_name_;
    QSqlDatabase database_;
    std::unique_ptr<StorageBackend> backend_;
};

} // namespace aowis::db

#endif // AOWIS_DB_DATABASE_H
