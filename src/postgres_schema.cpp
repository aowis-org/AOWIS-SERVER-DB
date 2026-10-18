#include "postgres_backend.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

namespace aowis::db
{
namespace
{
bool run(const QSqlDatabase &database, const QString &statement, QString *error)
{
    QSqlQuery query(database);
    if (query.exec(statement))
        return true;
    if (error != nullptr)
        *error = query.lastError().text() + QStringLiteral(" [DDL: %1]").arg(statement.left(140));
    return false;
}

// PostgreSQL's native uuid and bytea columns require a different schema from
// SQLite's BLOBs.  No migrations: this is the one current development schema.
const char *const statements[] = {
    R"SQL(CREATE TABLE branches (
    uuid UUID PRIMARY KEY NOT NULL,
    project_uuid UUID NOT NULL,
    name TEXT NOT NULL,
    head_revision_uuid UUID,
    created_by_uuid UUID,
    UNIQUE(project_uuid, name),
    UNIQUE(uuid, project_uuid)
))SQL",
    R"SQL(CREATE TABLE group_memberships (
    uuid UUID PRIMARY KEY NOT NULL,
    group_uuid UUID NOT NULL,
    user_uuid UUID NOT NULL,
    created_by_uuid UUID NOT NULL,
    created_at_ms BIGINT NOT NULL,
    revoked_at_ms BIGINT,
    revoked_by_uuid UUID,
    CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
))SQL",
    R"SQL(CREATE TABLE groups (
    uuid UUID PRIMARY KEY NOT NULL,
    name TEXT NOT NULL  CHECK(length(name)>0),
    created_at_ms BIGINT NOT NULL,
    disabled_at_ms BIGINT
))SQL",
    R"SQL(CREATE TABLE hydraulic_network_members (
    revision_uuid UUID NOT NULL,
    network_uuid UUID NOT NULL,
    entity_uuid UUID NOT NULL,
    PRIMARY KEY(revision_uuid,entity_uuid)
))SQL",
    R"SQL(CREATE TABLE hydraulic_network_references (
    revision_uuid UUID NOT NULL,
    network_uuid UUID NOT NULL,
    source_uuid UUID NOT NULL,
    target_uuid UUID NOT NULL,
    field TEXT NOT NULL,
    PRIMARY KEY(revision_uuid,network_uuid,source_uuid,target_uuid,field)
))SQL",
    R"SQL(CREATE TABLE object_events (
    uuid UUID PRIMARY KEY NOT NULL,
    revision_uuid UUID NOT NULL,
    object_uuid UUID NOT NULL,
    actor_uuid UUID NOT NULL,
    sequence BIGINT NOT NULL CHECK(sequence>=1),
    occurred_at_ms BIGINT NOT NULL,
    kind TEXT NOT NULL,
    action TEXT NOT NULL CHECK(action IN ('put','delete')),
    before_version BIGINT NOT NULL CHECK(before_version>=-1),
    after_version BIGINT NOT NULL CHECK(after_version>=1),
    before_content BYTEA,
    after_content BYTEA,
    before_deleted BIGINT NOT NULL CHECK(before_deleted IN (0,1)),
    UNIQUE(revision_uuid,sequence),
    CHECK((before_version=-1 AND after_version=1) OR
                  (before_version>=1 AND after_version=before_version+1)),
    CHECK((action='put' AND after_content IS NOT NULL) OR
                  (action='delete' AND after_content IS NULL)),
    CHECK((before_version=-1 AND before_content IS NULL AND before_deleted=1) OR
                  before_version>=1)
))SQL",
    R"SQL(CREATE TABLE object_group_events (
    group_uuid UUID NOT NULL,
    position BIGINT NOT NULL CHECK(position>=0),
    object_event_uuid UUID NOT NULL UNIQUE,
    PRIMARY KEY(group_uuid,position)
))SQL",
    R"SQL(CREATE TABLE object_groups (
    uuid UUID PRIMARY KEY NOT NULL,
    revision_uuid UUID NOT NULL,
    actor_uuid UUID NOT NULL,
    action TEXT NOT NULL CHECK(action IN ('edit','undo','redo')),
    target_group_uuid UUID,
    first_sequence BIGINT NOT NULL CHECK(first_sequence>=1),
    last_sequence BIGINT NOT NULL CHECK(last_sequence>=first_sequence),
    occurred_at_ms BIGINT NOT NULL,
    UNIQUE(revision_uuid,first_sequence),
    CHECK((action='edit' AND target_group_uuid IS NULL) OR
                   (action!='edit' AND target_group_uuid IS NOT NULL))
))SQL",
    R"SQL(CREATE TABLE operation_events (
    uuid UUID PRIMARY KEY NOT NULL,
    project_uuid UUID NOT NULL,
    branch_uuid UUID NOT NULL,
    revision_uuid UUID NOT NULL,
    sequence BIGINT NOT NULL CHECK(sequence>=1),
    actor_uuid UUID NOT NULL,
    occurred_at_ms BIGINT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('replace','undo','redo')),
    target_event_uuid UUID,
    before_present BIGINT NOT NULL CHECK(before_present IN (0,1)),
    before_snapshot BYTEA,
    after_snapshot BYTEA,
    after_present BIGINT NOT NULL CHECK(after_present IN (0,1)),
    UNIQUE(revision_uuid,sequence),
    CHECK((before_present=1 AND before_snapshot IS NOT NULL) OR
                  (before_present=0 AND before_snapshot IS NULL)),
    CHECK((kind='replace' AND target_event_uuid IS NULL) OR
                  (kind IN ('undo','redo') AND target_event_uuid IS NOT NULL)),
    CHECK((after_present=1 AND after_snapshot IS NOT NULL) OR
                  (after_present=0 AND after_snapshot IS NULL)),
    CHECK(kind='undo' OR after_present=1)
))SQL",
    R"SQL(CREATE TABLE password_recovery_tokens (
    uuid UUID PRIMARY KEY NOT NULL,
    user_uuid UUID NOT NULL,
    token_sha256 BYTEA NOT NULL UNIQUE CHECK(length(token_sha256)=32),
    issued_by_uuid UUID NOT NULL,
    issued_at_ms BIGINT NOT NULL,
    expires_at_ms BIGINT NOT NULL CHECK(expires_at_ms>issued_at_ms),
    used_at_ms BIGINT,
    invalidated_at_ms BIGINT,
    CHECK(used_at_ms IS NULL OR used_at_ms>=issued_at_ms),
    CHECK(invalidated_at_ms IS NULL OR invalidated_at_ms>=issued_at_ms),
    CHECK(used_at_ms IS NULL OR invalidated_at_ms IS NULL)
))SQL",
    R"SQL(CREATE TABLE permissions (
    uuid UUID PRIMARY KEY NOT NULL,
    key TEXT NOT NULL UNIQUE,
    builtin BIGINT NOT NULL DEFAULT 0 CHECK(builtin IN (0,1))
))SQL",
    R"SQL(CREATE TABLE projects (
    uuid UUID PRIMARY KEY NOT NULL,
    name TEXT NOT NULL CHECK(length(name)>0),
    created_at_ms BIGINT NOT NULL,
    archived_at_ms BIGINT,
    created_by_uuid UUID
))SQL",
    R"SQL(CREATE TABLE revision_objects (
    revision_uuid UUID NOT NULL,
    object_uuid UUID NOT NULL,
    kind TEXT NOT NULL CHECK(length(kind)>0 AND length(kind)<=128),
    content BYTEA,
    version BIGINT NOT NULL CHECK(version>=1),
    deleted BIGINT NOT NULL CHECK(deleted IN (0,1)),
    PRIMARY KEY(revision_uuid,object_uuid),
    CHECK((deleted=1 AND content IS NULL) OR (deleted=0 AND content IS NOT NULL))
))SQL",
    R"SQL(CREATE TABLE revision_parents (
    revision_uuid UUID NOT NULL,
    parent_position BIGINT NOT NULL CHECK(parent_position>=0),
    parent_revision_uuid UUID NOT NULL,
    PRIMARY KEY(revision_uuid,parent_position),
    UNIQUE(revision_uuid,parent_revision_uuid)
))SQL",
    R"SQL(CREATE TABLE revisions (
    uuid UUID PRIMARY KEY NOT NULL,
    project_uuid UUID NOT NULL,
    branch_uuid UUID NOT NULL,
    number BIGINT NOT NULL CHECK(number>=1),
    state TEXT NOT NULL CHECK(state IN ('working','finalized')),
    generation BIGINT NOT NULL DEFAULT 0 CHECK(generation>=0),
    created_at_ms BIGINT NOT NULL,
    finalized_at_ms BIGINT,
    title TEXT NOT NULL DEFAULT '',
    message TEXT NOT NULL DEFAULT '',
    snapshot_sha256 BYTEA CHECK(snapshot_sha256 IS NULL OR length(snapshot_sha256)=32),
    chain_sha256 BYTEA CHECK(chain_sha256 IS NULL OR length(chain_sha256)=32),
    created_by_uuid UUID,
    finalized_by_uuid UUID,
    hash_format_version BIGINT NOT NULL DEFAULT 1
            CHECK(hash_format_version IN (1,2)),
    operation_history_sha256 BYTEA
            CHECK(operation_history_sha256 IS NULL OR length(operation_history_sha256)=32),
    object_mode BIGINT NOT NULL DEFAULT 0
             CHECK(object_mode IN (0,1)),
    object_journal_version BIGINT NOT NULL DEFAULT 1
             CHECK(object_journal_version IN (1,2)),
    UNIQUE(branch_uuid, number),
    CHECK(state!='finalized' OR (finalized_at_ms IS NOT NULL AND snapshot_sha256 IS NOT NULL AND chain_sha256 IS NOT NULL))
))SQL",
    R"SQL(CREATE TABLE role_assignments (
    uuid UUID PRIMARY KEY NOT NULL,
    principal_kind TEXT NOT NULL CHECK(principal_kind IN ('user','group')),
    principal_uuid UUID NOT NULL,
    role_uuid UUID NOT NULL,
    scope_kind TEXT NOT NULL CHECK(scope_kind IN ('system','project','branch')),
    scope_uuid UUID,
    created_by_uuid UUID NOT NULL,
    created_at_ms BIGINT NOT NULL,
    revoked_at_ms BIGINT,
    revoked_by_uuid UUID,
    CHECK((scope_kind='system' AND scope_uuid IS NULL) OR
              (scope_kind IN ('project','branch') AND scope_uuid IS NOT NULL)),
    CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
))SQL",
    R"SQL(CREATE TABLE role_permissions (
    uuid UUID PRIMARY KEY NOT NULL,
    role_uuid UUID NOT NULL,
    permission_uuid UUID NOT NULL,
    revoked_at_ms BIGINT,
    revoked_by_uuid UUID,
    CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
))SQL",
    R"SQL(CREATE TABLE roles (
    uuid UUID PRIMARY KEY NOT NULL,
    key TEXT NOT NULL UNIQUE,
    builtin BIGINT NOT NULL DEFAULT 0 CHECK(builtin IN (0,1))
))SQL",
    R"SQL(CREATE TABLE security_audit_events (
    sequence BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
    uuid UUID NOT NULL UNIQUE,
    actor_uuid UUID NOT NULL,
    subject_uuid UUID NOT NULL,
    action TEXT NOT NULL CHECK(length(action)>0),
    occurred_at_ms BIGINT NOT NULL,
    previous_sha256 BYTEA NOT NULL CHECK(length(previous_sha256)=32),
    chain_sha256 BYTEA NOT NULL CHECK(length(chain_sha256)=32)
))SQL",
    R"SQL(CREATE TABLE snapshots (
    revision_uuid UUID PRIMARY KEY NOT NULL,
    format_version BIGINT NOT NULL DEFAULT 1 CHECK(format_version=1),
    content BYTEA NOT NULL
))SQL",
    R"SQL(CREATE TABLE user_password_credentials (
    user_uuid UUID PRIMARY KEY NOT NULL,
    salt BYTEA NOT NULL CHECK(length(salt)=16),
    verifier BYTEA NOT NULL CHECK(length(verifier)=32),
    iterations BIGINT NOT NULL CHECK(iterations>=100000 AND iterations<=2000000),
    changed_at_ms BIGINT NOT NULL,
    changed_by_uuid UUID NOT NULL,
    failed_attempts BIGINT NOT NULL DEFAULT 0 CHECK(failed_attempts>=0 AND failed_attempts<=5),
    locked_until_ms BIGINT NOT NULL DEFAULT 0
))SQL",
    R"SQL(CREATE TABLE user_sessions (
    uuid UUID PRIMARY KEY NOT NULL,
    user_uuid UUID NOT NULL,
    token_sha256 BYTEA NOT NULL UNIQUE CHECK(length(token_sha256)=32),
    created_by_uuid UUID NOT NULL,
    created_at_ms BIGINT NOT NULL,
    expires_at_ms BIGINT NOT NULL CHECK(expires_at_ms>created_at_ms),
    revoked_at_ms BIGINT,
    CHECK(revoked_at_ms IS NULL OR revoked_at_ms>=created_at_ms)
))SQL",
    R"SQL(CREATE TABLE users (
    uuid UUID PRIMARY KEY NOT NULL,
    username TEXT NOT NULL  CHECK(length(username)>0),
    created_at_ms BIGINT NOT NULL,
    disabled_at_ms BIGINT
))SQL",
    R"SQL(ALTER TABLE branches ADD CONSTRAINT aowis_branches_fk_1 FOREIGN KEY (project_uuid) REFERENCES projects(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE branches ADD CONSTRAINT aowis_branches_fk_2 FOREIGN KEY (head_revision_uuid) REFERENCES revisions(uuid) DEFERRABLE INITIALLY DEFERRED)SQL",
    R"SQL(ALTER TABLE branches ADD CONSTRAINT aowis_branches_fk_3 FOREIGN KEY (created_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE group_memberships ADD CONSTRAINT aowis_group_memberships_fk_1 FOREIGN KEY (group_uuid) REFERENCES groups(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE group_memberships ADD CONSTRAINT aowis_group_memberships_fk_2 FOREIGN KEY (user_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE group_memberships ADD CONSTRAINT aowis_group_memberships_fk_3 FOREIGN KEY (created_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE group_memberships ADD CONSTRAINT aowis_group_memberships_fk_4 FOREIGN KEY (revoked_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE hydraulic_network_members ADD CONSTRAINT aowis_hydraulic_network_members_fk_1 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE hydraulic_network_members ADD CONSTRAINT aowis_hydraulic_network_members_fk_2 FOREIGN KEY(revision_uuid,network_uuid) REFERENCES revision_objects(revision_uuid,object_uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE hydraulic_network_members ADD CONSTRAINT aowis_hydraulic_network_members_fk_3 FOREIGN KEY(revision_uuid,entity_uuid) REFERENCES revision_objects(revision_uuid,object_uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE hydraulic_network_references ADD CONSTRAINT aowis_hydraulic_network_references_fk_1 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE hydraulic_network_references ADD CONSTRAINT aowis_hydraulic_network_references_fk_2 FOREIGN KEY(revision_uuid,network_uuid) REFERENCES revision_objects(revision_uuid,object_uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE object_events ADD CONSTRAINT aowis_object_events_fk_1 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE object_events ADD CONSTRAINT aowis_object_events_fk_2 FOREIGN KEY (actor_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE object_group_events ADD CONSTRAINT aowis_object_group_events_fk_1 FOREIGN KEY (group_uuid) REFERENCES object_groups(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE object_group_events ADD CONSTRAINT aowis_object_group_events_fk_2 FOREIGN KEY (object_event_uuid) REFERENCES object_events(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE object_groups ADD CONSTRAINT aowis_object_groups_fk_1 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE object_groups ADD CONSTRAINT aowis_object_groups_fk_2 FOREIGN KEY (actor_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE object_groups ADD CONSTRAINT aowis_object_groups_fk_3 FOREIGN KEY (target_group_uuid) REFERENCES object_groups(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE operation_events ADD CONSTRAINT aowis_operation_events_fk_1 FOREIGN KEY (project_uuid) REFERENCES projects(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE operation_events ADD CONSTRAINT aowis_operation_events_fk_2 FOREIGN KEY (branch_uuid) REFERENCES branches(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE operation_events ADD CONSTRAINT aowis_operation_events_fk_3 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE operation_events ADD CONSTRAINT aowis_operation_events_fk_4 FOREIGN KEY (actor_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE operation_events ADD CONSTRAINT aowis_operation_events_fk_5 FOREIGN KEY (target_event_uuid) REFERENCES operation_events(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE password_recovery_tokens ADD CONSTRAINT aowis_password_recovery_tokens_fk_1 FOREIGN KEY (user_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE password_recovery_tokens ADD CONSTRAINT aowis_password_recovery_tokens_fk_2 FOREIGN KEY (issued_by_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE projects ADD CONSTRAINT aowis_projects_fk_1 FOREIGN KEY (created_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE revision_objects ADD CONSTRAINT aowis_revision_objects_fk_1 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE revision_parents ADD CONSTRAINT aowis_revision_parents_fk_1 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE revision_parents ADD CONSTRAINT aowis_revision_parents_fk_2 FOREIGN KEY (parent_revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE revisions ADD CONSTRAINT aowis_revisions_fk_1 FOREIGN KEY (created_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE revisions ADD CONSTRAINT aowis_revisions_fk_2 FOREIGN KEY (finalized_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE revisions ADD CONSTRAINT aowis_revisions_fk_3 FOREIGN KEY (branch_uuid, project_uuid) REFERENCES branches(uuid, project_uuid))SQL",
    R"SQL(ALTER TABLE role_assignments ADD CONSTRAINT aowis_role_assignments_fk_1 FOREIGN KEY (role_uuid) REFERENCES roles(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE role_assignments ADD CONSTRAINT aowis_role_assignments_fk_2 FOREIGN KEY (created_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE role_assignments ADD CONSTRAINT aowis_role_assignments_fk_3 FOREIGN KEY (revoked_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE role_permissions ADD CONSTRAINT aowis_role_permissions_fk_1 FOREIGN KEY (role_uuid) REFERENCES roles(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE role_permissions ADD CONSTRAINT aowis_role_permissions_fk_2 FOREIGN KEY (permission_uuid) REFERENCES permissions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE role_permissions ADD CONSTRAINT aowis_role_permissions_fk_3 FOREIGN KEY (revoked_by_uuid) REFERENCES users(uuid))SQL",
    R"SQL(ALTER TABLE security_audit_events ADD CONSTRAINT aowis_security_audit_events_fk_1 FOREIGN KEY (actor_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE snapshots ADD CONSTRAINT aowis_snapshots_fk_1 FOREIGN KEY (revision_uuid) REFERENCES revisions(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE user_password_credentials ADD CONSTRAINT aowis_user_password_credentials_fk_1 FOREIGN KEY (user_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE user_password_credentials ADD CONSTRAINT aowis_user_password_credentials_fk_2 FOREIGN KEY (changed_by_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE user_sessions ADD CONSTRAINT aowis_user_sessions_fk_1 FOREIGN KEY (user_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(ALTER TABLE user_sessions ADD CONSTRAINT aowis_user_sessions_fk_2 FOREIGN KEY (created_by_uuid) REFERENCES users(uuid) ON DELETE RESTRICT)SQL",
    R"SQL(CREATE UNIQUE INDEX group_membership_active_unique
        ON group_memberships(group_uuid,user_uuid) WHERE revoked_at_ms IS NULL)SQL",
    R"SQL(CREATE INDEX hydraulic_members_by_network ON hydraulic_network_members(revision_uuid,network_uuid))SQL",
    R"SQL(CREATE INDEX hydraulic_references_by_target ON hydraulic_network_references(revision_uuid,target_uuid))SQL",
    R"SQL(CREATE INDEX object_groups_by_revision
              ON object_groups(revision_uuid,last_sequence))SQL",
    R"SQL(CREATE UNIQUE INDEX one_working_revision_per_branch
            ON revisions(branch_uuid) WHERE state='working')SQL",
    R"SQL(CREATE INDEX operation_events_by_project ON operation_events(project_uuid,occurred_at_ms))SQL",
    R"SQL(CREATE INDEX recovery_tokens_by_user ON password_recovery_tokens(user_uuid))SQL",
    R"SQL(CREATE INDEX revision_objects_by_kind
              ON revision_objects(revision_uuid,kind,object_uuid))SQL",
    R"SQL(CREATE INDEX revisions_by_project ON revisions(project_uuid))SQL",
    R"SQL(CREATE UNIQUE INDEX role_assignment_active_unique ON role_assignments (
        principal_kind,principal_uuid,role_uuid,scope_kind,COALESCE(scope_uuid::text,''))
        WHERE revoked_at_ms IS NULL)SQL",
    R"SQL(CREATE UNIQUE INDEX role_permission_active_unique
        ON role_permissions(role_uuid,permission_uuid) WHERE revoked_at_ms IS NULL)SQL",
    R"SQL(CREATE INDEX security_audit_by_actor ON security_audit_events(actor_uuid,sequence))SQL",
    R"SQL(CREATE INDEX user_sessions_by_user ON user_sessions(user_uuid))SQL",
    R"SQL(CREATE UNIQUE INDEX users_username_nocase_unique ON users(lower(username)))SQL",
    R"SQL(CREATE UNIQUE INDEX groups_name_nocase_unique ON groups(lower(name)))SQL",
    R"SQL(CREATE FUNCTION aowis_guard_branch_creator_required() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.created_by_uuid IS NULL THEN
        RAISE EXCEPTION 'branch creator required' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER branch_creator_required BEFORE INSERT ON branches FOR EACH ROW EXECUTE FUNCTION aowis_guard_branch_creator_required())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_branch_head_must_be_working() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.head_revision_uuid IS NOT NULL AND NOT EXISTS (
                SELECT 1 FROM revisions WHERE uuid=NEW.head_revision_uuid
                AND branch_uuid=NEW.uuid AND state='working') THEN
        RAISE EXCEPTION 'branch head must be its working revision' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER branch_head_must_be_working BEFORE UPDATE OF head_revision_uuid ON branches FOR EACH ROW EXECUTE FUNCTION aowis_guard_branch_head_must_be_working())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalize_actor_required() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.state='finalized' AND (NEW.hash_format_version!=2 OR
                NEW.finalized_by_uuid IS NULL) THEN
        RAISE EXCEPTION 'Finalization actor required' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalize_actor_required BEFORE UPDATE OF state ON revisions FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalize_actor_required())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalize_requires_history_hash() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.state='finalized' AND NEW.operation_history_sha256 IS NULL THEN
        RAISE EXCEPTION 'operation history seal required' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalize_requires_history_hash BEFORE UPDATE OF state ON revisions FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalize_requires_history_hash())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalize_requires_snapshot() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.state='finalized'
            AND NOT EXISTS(SELECT 1 FROM snapshots WHERE revision_uuid=NEW.uuid) THEN
        RAISE EXCEPTION 'cannot finalize without a snapshot' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalize_requires_snapshot BEFORE UPDATE OF state ON revisions FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalize_requires_snapshot())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_hydraulic_member_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized network membership is immutable' USING ERRCODE = '23514';
    END IF;
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_hydraulic_member_delete BEFORE DELETE ON hydraulic_network_members FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_hydraulic_member_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_hydraulic_member_insert() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized network membership is immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_hydraulic_member_insert BEFORE INSERT ON hydraulic_network_members FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_hydraulic_member_insert())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_hydraulic_reference_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized hydraulic references are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_hydraulic_reference_delete BEFORE DELETE ON hydraulic_network_references FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_hydraulic_reference_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_hydraulic_reference_insert() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized hydraulic references are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_hydraulic_reference_insert BEFORE INSERT ON hydraulic_network_references FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_hydraulic_reference_insert())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_object_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized objects are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_object_delete BEFORE DELETE ON revision_objects FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_object_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_object_insert() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized objects are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_object_insert BEFORE INSERT ON revision_objects FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_object_insert())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_object_update() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized objects are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_object_update BEFORE UPDATE ON revision_objects FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_object_update())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_parents_readonly_insert() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized revision parents are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_parents_readonly_insert BEFORE INSERT ON revision_parents FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_parents_readonly_insert())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_revision_readonly_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF OLD.state='finalized' THEN
        RAISE EXCEPTION 'finalized revisions cannot be deleted' USING ERRCODE = '23514';
    END IF;
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_revision_readonly_delete BEFORE DELETE ON revisions FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_revision_readonly_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_revision_readonly_update() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF OLD.state='finalized' THEN
        RAISE EXCEPTION 'finalized revisions are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_revision_readonly_update BEFORE UPDATE ON revisions FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_revision_readonly_update())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_snapshot_readonly_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized snapshots cannot be deleted' USING ERRCODE = '23514';
    END IF;
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_snapshot_readonly_delete BEFORE DELETE ON snapshots FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_snapshot_readonly_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_snapshot_readonly_insert() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized snapshots cannot be inserted' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_snapshot_readonly_insert BEFORE INSERT ON snapshots FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_snapshot_readonly_insert())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_finalized_snapshot_readonly_update() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized' THEN
        RAISE EXCEPTION 'finalized snapshots are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER finalized_snapshot_readonly_update BEFORE UPDATE ON snapshots FOR EACH ROW EXECUTE FUNCTION aowis_guard_finalized_snapshot_readonly_update())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_group_membership_no_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'membership history cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER group_membership_no_delete BEFORE DELETE ON group_memberships FOR EACH ROW EXECUTE FUNCTION aowis_guard_group_membership_no_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_group_membership_revoke_only() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NOT DISTINCT FROM NEW.uuid AND OLD.group_uuid IS NOT DISTINCT FROM NEW.group_uuid
              AND OLD.user_uuid IS NOT DISTINCT FROM NEW.user_uuid AND OLD.created_by_uuid IS NOT DISTINCT FROM NEW.created_by_uuid
              AND OLD.created_at_ms IS NOT DISTINCT FROM NEW.created_at_ms) THEN
        RAISE EXCEPTION 'membership can only be revoked once' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER group_membership_revoke_only BEFORE UPDATE ON group_memberships FOR EACH ROW EXECUTE FUNCTION aowis_guard_group_membership_revoke_only())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_hydraulic_member_update_block() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'network membership must be replaced transactionally' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER hydraulic_member_update_block BEFORE UPDATE ON hydraulic_network_members FOR EACH ROW EXECUTE FUNCTION aowis_guard_hydraulic_member_update_block())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_hydraulic_reference_update_block() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'hydraulic references cannot be updated' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER hydraulic_reference_update_block BEFORE UPDATE ON hydraulic_network_references FOR EACH ROW EXECUTE FUNCTION aowis_guard_hydraulic_reference_update_block())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_immutable_object_event_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'object events cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER immutable_object_event_delete BEFORE DELETE ON object_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_immutable_object_event_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_immutable_object_event_update() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'object events are immutable' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER immutable_object_event_update BEFORE UPDATE ON object_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_immutable_object_event_update())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_immutable_revision_identity() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF OLD.uuid!=NEW.uuid
                OR OLD.project_uuid!=NEW.project_uuid OR OLD.branch_uuid!=NEW.branch_uuid
                OR OLD.number!=NEW.number THEN
        RAISE EXCEPTION 'revision identity and ancestry are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER immutable_revision_identity BEFORE UPDATE ON revisions FOR EACH ROW EXECUTE FUNCTION aowis_guard_immutable_revision_identity())SQL",
    // The application validates a complete new hydraulic network before
    // entering this function. It does not bypass the normal row triggers:
    // revision generation and each immutable journal event still advance
    // together, one object at a time. A JSON array merely amortizes QPSQL
    // client/server round trips; all writes share the caller's transaction.
    R"SQL(CREATE FUNCTION aowis_insert_fresh_network_objects(
        p_revision_uuid UUID, p_actor_uuid UUID, p_rows JSONB)
    RETURNS BIGINT LANGUAGE plpgsql AS $aowis$
DECLARE
    item RECORD;
    content_bytes BYTEA;
    next_generation BIGINT;
    inserted_count BIGINT := 0;
BEGIN
    IF jsonb_typeof(p_rows) IS DISTINCT FROM 'array' THEN
        RAISE EXCEPTION 'expected fresh-network JSON array' USING ERRCODE = '22023';
    END IF;
    FOR item IN
        SELECT value FROM jsonb_array_elements(p_rows) WITH ORDINALITY AS incoming(value, ordinal)
        ORDER BY ordinal
    LOOP
        content_bytes := decode(item.value->>'content_b64', 'base64');
        INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted)
        VALUES (p_revision_uuid,(item.value->>'object_uuid')::UUID,
                item.value->>'kind',content_bytes,1,0);
        UPDATE revisions SET generation=generation+1
        WHERE uuid=p_revision_uuid AND state='working'
        RETURNING generation INTO next_generation;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'working revision generation conflict' USING ERRCODE = '23514';
        END IF;
        INSERT INTO object_events(uuid,revision_uuid,object_uuid,actor_uuid,
            sequence,occurred_at_ms,kind,action,before_version,after_version,
            before_content,after_content,before_deleted)
        VALUES ((item.value->>'event_uuid')::UUID,p_revision_uuid,
                (item.value->>'object_uuid')::UUID,p_actor_uuid,next_generation,
                (item.value->>'occurred_at_ms')::BIGINT,item.value->>'kind',
                'put',-1,1,NULL,content_bytes,1);
        inserted_count := inserted_count+1;
    END LOOP;
    RETURN inserted_count;
END
$aowis$)SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_event_insert_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM revisions r WHERE r.uuid=NEW.revision_uuid
               AND r.state='working' AND r.object_mode=1 AND r.generation=NEW.sequence) THEN
        RAISE EXCEPTION 'object event must match current revision' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_event_insert_guard BEFORE INSERT ON object_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_event_insert_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_group_delete_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'object groups are immutable' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_group_delete_guard BEFORE DELETE ON object_groups FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_group_delete_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_group_insert_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT EXISTS(SELECT 1 FROM revisions WHERE uuid=NEW.revision_uuid
                AND state='working' AND object_mode=1 AND generation=NEW.last_sequence)
                OR (NEW.target_group_uuid IS NOT NULL AND NOT EXISTS (
                  SELECT 1 FROM object_groups prior WHERE prior.uuid=NEW.target_group_uuid
                    AND prior.revision_uuid=NEW.revision_uuid AND prior.action='edit')) THEN
        RAISE EXCEPTION 'invalid object group or target' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_group_insert_guard BEFORE INSERT ON object_groups FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_group_insert_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_group_member_delete_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'object group members are immutable' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_group_member_delete_guard BEFORE DELETE ON object_group_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_group_member_delete_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_group_member_insert_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT EXISTS(
                SELECT 1 FROM object_groups g JOIN object_events e ON e.uuid=NEW.object_event_uuid
                JOIN revisions r ON r.uuid=g.revision_uuid
                WHERE g.uuid=NEW.group_uuid AND e.revision_uuid=g.revision_uuid
                  AND e.actor_uuid=g.actor_uuid AND r.state='working'
                  AND e.sequence BETWEEN g.first_sequence AND g.last_sequence) THEN
        RAISE EXCEPTION 'object event/group mismatch' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_group_member_insert_guard BEFORE INSERT ON object_group_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_group_member_insert_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_group_member_update_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'object group members are immutable' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_group_member_update_guard BEFORE UPDATE ON object_group_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_group_member_update_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_group_update_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'object groups are immutable' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_group_update_guard BEFORE UPDATE ON object_groups FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_group_update_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_object_identity_readonly() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF OLD.revision_uuid!=NEW.revision_uuid OR OLD.object_uuid!=NEW.object_uuid
                 OR OLD.kind!=NEW.kind THEN
        RAISE EXCEPTION 'object identity and type are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER object_identity_readonly BEFORE UPDATE ON revision_objects FOR EACH ROW EXECUTE FUNCTION aowis_guard_object_identity_readonly())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_operation_events_delete_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'operation history cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER operation_events_delete_guard BEFORE DELETE ON operation_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_operation_events_delete_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_operation_events_insert_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT EXISTS (
                SELECT 1 FROM revisions r WHERE r.uuid=NEW.revision_uuid
                AND r.project_uuid=NEW.project_uuid AND r.branch_uuid=NEW.branch_uuid
                AND r.state='working' AND r.generation=NEW.sequence)
              OR (NEW.kind IN ('undo','redo') AND NOT EXISTS (
                SELECT 1 FROM operation_events prior WHERE prior.uuid=NEW.target_event_uuid
                AND prior.revision_uuid=NEW.revision_uuid AND prior.kind='replace')) THEN
        RAISE EXCEPTION 'operation must match current working revision and generation' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER operation_events_insert_guard BEFORE INSERT ON operation_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_operation_events_insert_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_operation_events_update_guard() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'operation history is immutable' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER operation_events_update_guard BEFORE UPDATE ON operation_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_operation_events_update_guard())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_parent_must_be_finalized_in_same_project() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT EXISTS (
                SELECT 1 FROM revisions child JOIN revisions parent
                    ON parent.uuid=NEW.parent_revision_uuid
                WHERE child.uuid=NEW.revision_uuid AND parent.state='finalized'
                    AND parent.project_uuid=child.project_uuid) THEN
        RAISE EXCEPTION 'parent must be finalized in same project' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER parent_must_be_finalized_in_same_project BEFORE INSERT ON revision_parents FOR EACH ROW EXECUTE FUNCTION aowis_guard_parent_must_be_finalized_in_same_project())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_parents_readonly_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'revision parent edges cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER parents_readonly_delete BEFORE DELETE ON revision_parents FOR EACH ROW EXECUTE FUNCTION aowis_guard_parents_readonly_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_parents_readonly_update() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'revision parent edges are immutable' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER parents_readonly_update BEFORE UPDATE ON revision_parents FOR EACH ROW EXECUTE FUNCTION aowis_guard_parents_readonly_update())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_project_creator_required() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.created_by_uuid IS NULL THEN
        RAISE EXCEPTION 'project creator required' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER project_creator_required BEFORE INSERT ON projects FOR EACH ROW EXECUTE FUNCTION aowis_guard_project_creator_required())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_recovery_tokens_close_only() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid
               OR NEW.token_sha256!=OLD.token_sha256 OR NEW.issued_by_uuid!=OLD.issued_by_uuid
               OR NEW.issued_at_ms!=OLD.issued_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms
               OR OLD.used_at_ms IS DISTINCT FROM NEW.used_at_ms AND OLD.used_at_ms IS NOT NULL
               OR OLD.invalidated_at_ms IS DISTINCT FROM NEW.invalidated_at_ms AND OLD.invalidated_at_ms IS NOT NULL THEN
        RAISE EXCEPTION 'recovery identity and closure are immutable' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER recovery_tokens_close_only BEFORE UPDATE ON password_recovery_tokens FOR EACH ROW EXECUTE FUNCTION aowis_guard_recovery_tokens_close_only())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_recovery_tokens_no_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'recovery tokens cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER recovery_tokens_no_delete BEFORE DELETE ON password_recovery_tokens FOR EACH ROW EXECUTE FUNCTION aowis_guard_recovery_tokens_no_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_revision_creator_required() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.created_by_uuid IS NULL THEN
        RAISE EXCEPTION 'revision creator required' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER revision_creator_required BEFORE INSERT ON revisions FOR EACH ROW EXECUTE FUNCTION aowis_guard_revision_creator_required())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_role_assignment_no_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'role assignment history cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER role_assignment_no_delete BEFORE DELETE ON role_assignments FOR EACH ROW EXECUTE FUNCTION aowis_guard_role_assignment_no_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_role_assignment_revoke_only() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NOT DISTINCT FROM NEW.uuid AND OLD.principal_kind IS NOT DISTINCT FROM NEW.principal_kind
              AND OLD.principal_uuid IS NOT DISTINCT FROM NEW.principal_uuid AND OLD.role_uuid IS NOT DISTINCT FROM NEW.role_uuid
              AND OLD.scope_kind IS NOT DISTINCT FROM NEW.scope_kind AND OLD.scope_uuid IS NOT DISTINCT FROM NEW.scope_uuid
              AND OLD.created_by_uuid IS NOT DISTINCT FROM NEW.created_by_uuid
              AND OLD.created_at_ms IS NOT DISTINCT FROM NEW.created_at_ms) THEN
        RAISE EXCEPTION 'role assignment can only be revoked once' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER role_assignment_revoke_only BEFORE UPDATE ON role_assignments FOR EACH ROW EXECUTE FUNCTION aowis_guard_role_assignment_revoke_only())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_role_assignment_valid() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT (
            (NEW.principal_kind='user' AND EXISTS(SELECT 1 FROM users WHERE uuid=NEW.principal_uuid)) OR
            (NEW.principal_kind='group' AND EXISTS(SELECT 1 FROM groups WHERE uuid=NEW.principal_uuid)))
          OR NOT (
            NEW.scope_kind='system' OR
            (NEW.scope_kind='project' AND EXISTS(SELECT 1 FROM projects WHERE uuid=NEW.scope_uuid)) OR
            (NEW.scope_kind='branch' AND EXISTS(SELECT 1 FROM branches WHERE uuid=NEW.scope_uuid))) THEN
        RAISE EXCEPTION 'invalid principal or scope' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER role_assignment_valid BEFORE INSERT ON role_assignments FOR EACH ROW EXECUTE FUNCTION aowis_guard_role_assignment_valid())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_role_permission_no_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'role permission history cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER role_permission_no_delete BEFORE DELETE ON role_permissions FOR EACH ROW EXECUTE FUNCTION aowis_guard_role_permission_no_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_role_permission_revoke_only() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NOT DISTINCT FROM NEW.uuid AND OLD.role_uuid IS NOT DISTINCT FROM NEW.role_uuid
              AND OLD.permission_uuid IS NOT DISTINCT FROM NEW.permission_uuid) THEN
        RAISE EXCEPTION 'role permission can only be revoked once' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER role_permission_revoke_only BEFORE UPDATE ON role_permissions FOR EACH ROW EXECUTE FUNCTION aowis_guard_role_permission_revoke_only())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_security_audit_readonly_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'security audit events cannot be deleted' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER security_audit_readonly_delete BEFORE DELETE ON security_audit_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_security_audit_readonly_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_security_audit_readonly_update() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'security audit events are immutable' USING ERRCODE = '23514';
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER security_audit_readonly_update BEFORE UPDATE ON security_audit_events FOR EACH ROW EXECUTE FUNCTION aowis_guard_security_audit_readonly_update())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_user_sessions_immutable_delete() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
        RAISE EXCEPTION 'sessions are append-only' USING ERRCODE = '23514';
    RETURN OLD;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER user_sessions_immutable_delete BEFORE DELETE ON user_sessions FOR EACH ROW EXECUTE FUNCTION aowis_guard_user_sessions_immutable_delete())SQL",
    R"SQL(CREATE FUNCTION aowis_guard_user_sessions_immutable_identity() RETURNS trigger LANGUAGE plpgsql AS $aowis$
BEGIN
    IF NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid
                OR NEW.created_by_uuid!=OLD.created_by_uuid
                OR NEW.created_at_ms!=OLD.created_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms
                OR (OLD.revoked_at_ms IS NOT NULL AND NEW.revoked_at_ms IS DISTINCT FROM OLD.revoked_at_ms)
                OR (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
                    AND NEW.revoked_at_ms<OLD.created_at_ms)
                OR (NEW.token_sha256!=OLD.token_sha256 AND NOT (
                    NEW.token_sha256=(decode('414f5749532d5245444143542d563121', 'hex') || decode(replace(OLD.uuid::text, '-', ''), 'hex'))
                    AND (OLD.revoked_at_ms IS NOT NULL OR OLD.expires_at_ms<=(floor(extract(epoch from clock_timestamp())*1000))::bigint)
                )) THEN
        RAISE EXCEPTION 'session identity is immutable; spent credentials may only be redacted' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END
$aowis$)SQL",
    R"SQL(CREATE TRIGGER user_sessions_immutable_identity BEFORE UPDATE ON user_sessions FOR EACH ROW EXECUTE FUNCTION aowis_guard_user_sessions_immutable_identity())SQL",
};

bool checkObjects(const QSqlDatabase &database,
                  const QString &query_sql, const QStringList &expected, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(query_sql))
    {
        if (error != nullptr) *error = query.lastError().text();
        return false;
    }
    QStringList present;
    while (query.next())
        present.append(query.value(0).toString());
    query.finish();
    for (const QString &name : expected)
    {
        if (!present.contains(name))
        {
            if (error != nullptr) *error = QStringLiteral("PostgreSQL schema missing: %1").arg(name);
            return false;
        }
    }
    return true;
}
} // namespace

bool PostgresBackend::createSchema(const QSqlDatabase &database, QString *error) const
{
    // Called by the core within an exclusive write transaction.
    for (const char *statement : statements)
    {
        if (!run(database, QString::fromUtf8(statement), error))
            return false;
    }
    return true;
}

bool PostgresBackend::verifySchema(const QSqlDatabase &database, QString *error) const
{
    const QStringList tables = {
        QStringLiteral("branches"),
        QStringLiteral("group_memberships"),
        QStringLiteral("groups"),
        QStringLiteral("hydraulic_network_members"),
        QStringLiteral("hydraulic_network_references"),
        QStringLiteral("object_events"),
        QStringLiteral("object_group_events"),
        QStringLiteral("object_groups"),
        QStringLiteral("operation_events"),
        QStringLiteral("password_recovery_tokens"),
        QStringLiteral("permissions"),
        QStringLiteral("projects"),
        QStringLiteral("revision_objects"),
        QStringLiteral("revision_parents"),
        QStringLiteral("revisions"),
        QStringLiteral("role_assignments"),
        QStringLiteral("role_permissions"),
        QStringLiteral("roles"),
        QStringLiteral("security_audit_events"),
        QStringLiteral("snapshots"),
        QStringLiteral("user_password_credentials"),
        QStringLiteral("user_sessions"),
        QStringLiteral("users")
    };
    const QStringList indexes = {
        QStringLiteral("group_membership_active_unique"),
        QStringLiteral("hydraulic_members_by_network"),
        QStringLiteral("hydraulic_references_by_target"),
        QStringLiteral("object_groups_by_revision"),
        QStringLiteral("one_working_revision_per_branch"),
        QStringLiteral("operation_events_by_project"),
        QStringLiteral("recovery_tokens_by_user"),
        QStringLiteral("revision_objects_by_kind"),
        QStringLiteral("revisions_by_project"),
        QStringLiteral("role_assignment_active_unique"),
        QStringLiteral("role_permission_active_unique"),
        QStringLiteral("security_audit_by_actor"),
        QStringLiteral("user_sessions_by_user"),
        QStringLiteral("users_username_nocase_unique"),
        QStringLiteral("groups_name_nocase_unique")
    };
    const QStringList foreign_keys = {
        QStringLiteral("aowis_branches_fk_1"),
        QStringLiteral("aowis_branches_fk_2"),
        QStringLiteral("aowis_branches_fk_3"),
        QStringLiteral("aowis_group_memberships_fk_1"),
        QStringLiteral("aowis_group_memberships_fk_2"),
        QStringLiteral("aowis_group_memberships_fk_3"),
        QStringLiteral("aowis_group_memberships_fk_4"),
        QStringLiteral("aowis_hydraulic_network_members_fk_1"),
        QStringLiteral("aowis_hydraulic_network_members_fk_2"),
        QStringLiteral("aowis_hydraulic_network_members_fk_3"),
        QStringLiteral("aowis_hydraulic_network_references_fk_1"),
        QStringLiteral("aowis_hydraulic_network_references_fk_2"),
        QStringLiteral("aowis_object_events_fk_1"),
        QStringLiteral("aowis_object_events_fk_2"),
        QStringLiteral("aowis_object_group_events_fk_1"),
        QStringLiteral("aowis_object_group_events_fk_2"),
        QStringLiteral("aowis_object_groups_fk_1"),
        QStringLiteral("aowis_object_groups_fk_2"),
        QStringLiteral("aowis_object_groups_fk_3"),
        QStringLiteral("aowis_operation_events_fk_1"),
        QStringLiteral("aowis_operation_events_fk_2"),
        QStringLiteral("aowis_operation_events_fk_3"),
        QStringLiteral("aowis_operation_events_fk_4"),
        QStringLiteral("aowis_operation_events_fk_5"),
        QStringLiteral("aowis_password_recovery_tokens_fk_1"),
        QStringLiteral("aowis_password_recovery_tokens_fk_2"),
        QStringLiteral("aowis_projects_fk_1"),
        QStringLiteral("aowis_revision_objects_fk_1"),
        QStringLiteral("aowis_revision_parents_fk_1"),
        QStringLiteral("aowis_revision_parents_fk_2"),
        QStringLiteral("aowis_revisions_fk_1"),
        QStringLiteral("aowis_revisions_fk_2"),
        QStringLiteral("aowis_revisions_fk_3"),
        QStringLiteral("aowis_role_assignments_fk_1"),
        QStringLiteral("aowis_role_assignments_fk_2"),
        QStringLiteral("aowis_role_assignments_fk_3"),
        QStringLiteral("aowis_role_permissions_fk_1"),
        QStringLiteral("aowis_role_permissions_fk_2"),
        QStringLiteral("aowis_role_permissions_fk_3"),
        QStringLiteral("aowis_security_audit_events_fk_1"),
        QStringLiteral("aowis_snapshots_fk_1"),
        QStringLiteral("aowis_user_password_credentials_fk_1"),
        QStringLiteral("aowis_user_password_credentials_fk_2"),
        QStringLiteral("aowis_user_sessions_fk_1"),
        QStringLiteral("aowis_user_sessions_fk_2")
    };
    const QStringList triggers = {
        QStringLiteral("branch_creator_required"),
        QStringLiteral("branch_head_must_be_working"),
        QStringLiteral("finalize_actor_required"),
        QStringLiteral("finalize_requires_history_hash"),
        QStringLiteral("finalize_requires_snapshot"),
        QStringLiteral("finalized_hydraulic_member_delete"),
        QStringLiteral("finalized_hydraulic_member_insert"),
        QStringLiteral("finalized_hydraulic_reference_delete"),
        QStringLiteral("finalized_hydraulic_reference_insert"),
        QStringLiteral("finalized_object_delete"),
        QStringLiteral("finalized_object_insert"),
        QStringLiteral("finalized_object_update"),
        QStringLiteral("finalized_parents_readonly_insert"),
        QStringLiteral("finalized_revision_readonly_delete"),
        QStringLiteral("finalized_revision_readonly_update"),
        QStringLiteral("finalized_snapshot_readonly_delete"),
        QStringLiteral("finalized_snapshot_readonly_insert"),
        QStringLiteral("finalized_snapshot_readonly_update"),
        QStringLiteral("group_membership_no_delete"),
        QStringLiteral("group_membership_revoke_only"),
        QStringLiteral("hydraulic_member_update_block"),
        QStringLiteral("hydraulic_reference_update_block"),
        QStringLiteral("immutable_object_event_delete"),
        QStringLiteral("immutable_object_event_update"),
        QStringLiteral("immutable_revision_identity"),
        QStringLiteral("object_event_insert_guard"),
        QStringLiteral("object_group_delete_guard"),
        QStringLiteral("object_group_insert_guard"),
        QStringLiteral("object_group_member_delete_guard"),
        QStringLiteral("object_group_member_insert_guard"),
        QStringLiteral("object_group_member_update_guard"),
        QStringLiteral("object_group_update_guard"),
        QStringLiteral("object_identity_readonly"),
        QStringLiteral("operation_events_delete_guard"),
        QStringLiteral("operation_events_insert_guard"),
        QStringLiteral("operation_events_update_guard"),
        QStringLiteral("parent_must_be_finalized_in_same_project"),
        QStringLiteral("parents_readonly_delete"),
        QStringLiteral("parents_readonly_update"),
        QStringLiteral("project_creator_required"),
        QStringLiteral("recovery_tokens_close_only"),
        QStringLiteral("recovery_tokens_no_delete"),
        QStringLiteral("revision_creator_required"),
        QStringLiteral("role_assignment_no_delete"),
        QStringLiteral("role_assignment_revoke_only"),
        QStringLiteral("role_assignment_valid"),
        QStringLiteral("role_permission_no_delete"),
        QStringLiteral("role_permission_revoke_only"),
        QStringLiteral("security_audit_readonly_delete"),
        QStringLiteral("security_audit_readonly_update"),
        QStringLiteral("user_sessions_immutable_delete"),
        QStringLiteral("user_sessions_immutable_identity")
    };
    // Check every expected name, not merely the aggregate count: a damaged
    // database must not silently lose a security or immutability guard.
    return checkObjects(database, QStringLiteral(
        "SELECT proname FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace "
        "WHERE n.nspname=current_schema()"),
        {QStringLiteral("aowis_insert_fresh_network_objects")}, error) &&
           checkObjects(database, QStringLiteral(
        "SELECT tablename FROM pg_catalog.pg_tables WHERE schemaname=current_schema()"), tables, error) &&
           checkObjects(database, QStringLiteral(
        "SELECT indexname FROM pg_catalog.pg_indexes WHERE schemaname=current_schema()"), indexes, error) &&
           checkObjects(database, QStringLiteral(
        "SELECT con.conname FROM pg_catalog.pg_constraint con "
        "JOIN pg_catalog.pg_namespace n ON n.oid=con.connamespace "
        "WHERE n.nspname=current_schema() AND con.contype='f'"), foreign_keys, error) &&
           checkObjects(database, QStringLiteral(
        "SELECT t.tgname FROM pg_catalog.pg_trigger t "
        "JOIN pg_catalog.pg_class c ON c.oid=t.tgrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname=current_schema() AND NOT t.tgisinternal"), triggers, error);
}

} // namespace aowis::db
