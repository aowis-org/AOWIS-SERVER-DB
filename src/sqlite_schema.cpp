#include "sqlite_backend.h"

#include <QSqlError>
#include <QSqlQuery>

namespace aowis::db
{
bool SqliteBackend::createSchema(const QSqlDatabase &database, QString *error) const
{
    // One current SQLite schema. This is a development database: no upgrades
    // from earlier schemas, compatibility shims, or data backfills.
    static const char *const statements[] = {
        R"SQL(CREATE TABLE branches (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            project_uuid BLOB NOT NULL REFERENCES projects(uuid) ON DELETE RESTRICT,
            name TEXT NOT NULL,
            head_revision_uuid BLOB REFERENCES revisions(uuid) DEFERRABLE INITIALLY DEFERRED, created_by_uuid BLOB REFERENCES users(uuid),
            UNIQUE(project_uuid, name),
            UNIQUE(uuid, project_uuid)
        ))SQL",
        R"SQL(CREATE TABLE group_memberships (
        uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
        group_uuid BLOB NOT NULL REFERENCES groups(uuid) ON DELETE RESTRICT,
        user_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
        created_by_uuid BLOB NOT NULL REFERENCES users(uuid),
        created_at_ms INTEGER NOT NULL,
        revoked_at_ms INTEGER,
        revoked_by_uuid BLOB REFERENCES users(uuid),
        CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
    ))SQL",
        R"SQL(CREATE TABLE groups (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            name TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(name)>0),
            created_at_ms INTEGER NOT NULL,
            disabled_at_ms INTEGER
        ))SQL",
        R"SQL(CREATE TABLE hydraulic_network_members (revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,network_uuid BLOB NOT NULL,entity_uuid BLOB NOT NULL,PRIMARY KEY(revision_uuid,entity_uuid),FOREIGN KEY(revision_uuid,network_uuid) REFERENCES revision_objects(revision_uuid,object_uuid) ON DELETE RESTRICT,FOREIGN KEY(revision_uuid,entity_uuid) REFERENCES revision_objects(revision_uuid,object_uuid) ON DELETE RESTRICT) WITHOUT ROWID)SQL",
        R"SQL(CREATE TABLE hydraulic_network_references (revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,network_uuid BLOB NOT NULL,source_uuid BLOB NOT NULL CHECK(length(source_uuid)=16),target_uuid BLOB NOT NULL CHECK(length(target_uuid)=16),field TEXT NOT NULL,PRIMARY KEY(revision_uuid,network_uuid,source_uuid,target_uuid,field),FOREIGN KEY(revision_uuid,network_uuid) REFERENCES revision_objects(revision_uuid,object_uuid) ON DELETE RESTRICT) WITHOUT ROWID)SQL",
        R"SQL(CREATE TABLE object_events (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            object_uuid BLOB NOT NULL CHECK(length(object_uuid)=16),
            actor_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            sequence INTEGER NOT NULL CHECK(sequence>=1),
            occurred_at_ms INTEGER NOT NULL,
            kind TEXT NOT NULL,
            action TEXT NOT NULL CHECK(action IN ('put','delete')),
            before_version INTEGER NOT NULL CHECK(before_version>=-1),
            after_version INTEGER NOT NULL CHECK(after_version>=1),
            before_content BLOB,
            after_content BLOB,
            before_deleted INTEGER NOT NULL CHECK(before_deleted IN (0,1)),
            UNIQUE(revision_uuid,sequence),
            CHECK((before_version=-1 AND after_version=1) OR
                  (before_version>=1 AND after_version=before_version+1)),
            CHECK((action='put' AND after_content IS NOT NULL) OR
                  (action='delete' AND after_content IS NULL)),
            CHECK((before_version=-1 AND before_content IS NULL AND before_deleted=1) OR
                  before_version>=1)
        ))SQL",
        R"SQL(CREATE TABLE object_group_events (
              group_uuid BLOB NOT NULL REFERENCES object_groups(uuid) ON DELETE RESTRICT,
              position INTEGER NOT NULL CHECK(position>=0),
              object_event_uuid BLOB NOT NULL UNIQUE REFERENCES object_events(uuid) ON DELETE RESTRICT,
              PRIMARY KEY(group_uuid,position)
        ))SQL",
        R"SQL(CREATE TABLE object_groups (
             uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
             revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
             actor_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
             action TEXT NOT NULL CHECK(action IN ('edit','undo','redo')),
             target_group_uuid BLOB REFERENCES object_groups(uuid) ON DELETE RESTRICT,
             first_sequence INTEGER NOT NULL CHECK(first_sequence>=1),
             last_sequence INTEGER NOT NULL CHECK(last_sequence>=first_sequence),
             occurred_at_ms INTEGER NOT NULL,
             UNIQUE(revision_uuid,first_sequence),
             CHECK((action='edit' AND target_group_uuid IS NULL) OR
                   (action!='edit' AND target_group_uuid IS NOT NULL))
        ))SQL",
        R"SQL(CREATE TABLE operation_events (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            project_uuid BLOB NOT NULL REFERENCES projects(uuid) ON DELETE RESTRICT,
            branch_uuid BLOB NOT NULL REFERENCES branches(uuid) ON DELETE RESTRICT,
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            sequence INTEGER NOT NULL CHECK(sequence>=1),
            actor_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            occurred_at_ms INTEGER NOT NULL,
            kind TEXT NOT NULL CHECK(kind IN ('replace','undo','redo')),
            target_event_uuid BLOB REFERENCES operation_events(uuid) ON DELETE RESTRICT,
            before_present INTEGER NOT NULL CHECK(before_present IN (0,1)),
            before_snapshot BLOB,
            after_snapshot BLOB,
            after_present INTEGER NOT NULL CHECK(after_present IN (0,1)),
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
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            user_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            token_sha256 BLOB NOT NULL UNIQUE CHECK(length(token_sha256)=32),
            issued_by_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            issued_at_ms INTEGER NOT NULL,
            expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms>issued_at_ms),
            used_at_ms INTEGER,
            invalidated_at_ms INTEGER,
            CHECK(used_at_ms IS NULL OR used_at_ms>=issued_at_ms),
            CHECK(invalidated_at_ms IS NULL OR invalidated_at_ms>=issued_at_ms),
            CHECK(used_at_ms IS NULL OR invalidated_at_ms IS NULL)
        ))SQL",
        R"SQL(CREATE TABLE permissions (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            key TEXT NOT NULL UNIQUE,
            builtin INTEGER NOT NULL DEFAULT 0 CHECK(builtin IN (0,1))
        ))SQL",
        R"SQL(CREATE TABLE projects (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            name TEXT NOT NULL CHECK(length(name)>0),
            created_at_ms INTEGER NOT NULL,
            archived_at_ms INTEGER
        , created_by_uuid BLOB REFERENCES users(uuid)))SQL",
        R"SQL(CREATE TABLE revision_objects (
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            object_uuid BLOB NOT NULL CHECK(length(object_uuid)=16),
            kind TEXT NOT NULL CHECK(length(kind)>0 AND length(kind)<=128),
            content BLOB,
            version INTEGER NOT NULL CHECK(version>=1),
            deleted INTEGER NOT NULL CHECK(deleted IN (0,1)),
            PRIMARY KEY(revision_uuid,object_uuid),
            CHECK((deleted=1 AND content IS NULL) OR (deleted=0 AND content IS NOT NULL))
        ))SQL",
        R"SQL(CREATE TABLE revision_parents (
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            parent_position INTEGER NOT NULL CHECK(parent_position>=0),
            parent_revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            PRIMARY KEY(revision_uuid,parent_position),
            UNIQUE(revision_uuid,parent_revision_uuid)
        ))SQL",
        R"SQL(CREATE TABLE revisions (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            project_uuid BLOB NOT NULL,
            branch_uuid BLOB NOT NULL,
            number INTEGER NOT NULL CHECK(number>=1),
            state TEXT NOT NULL CHECK(state IN ('working','finalized')),
            generation INTEGER NOT NULL DEFAULT 0 CHECK(generation>=0),
            created_at_ms INTEGER NOT NULL,
            finalized_at_ms INTEGER,
            title TEXT NOT NULL DEFAULT '',
            message TEXT NOT NULL DEFAULT '',
            snapshot_sha256 BLOB CHECK(snapshot_sha256 IS NULL OR length(snapshot_sha256)=32),
            chain_sha256 BLOB CHECK(chain_sha256 IS NULL OR length(chain_sha256)=32), created_by_uuid BLOB REFERENCES users(uuid), finalized_by_uuid BLOB REFERENCES users(uuid), hash_format_version INTEGER NOT NULL DEFAULT 1
            CHECK(hash_format_version IN (1,2)), operation_history_sha256 BLOB
            CHECK(operation_history_sha256 IS NULL OR length(operation_history_sha256)=32), object_mode INTEGER NOT NULL DEFAULT 0
             CHECK(object_mode IN (0,1)), object_journal_version INTEGER NOT NULL DEFAULT 1
             CHECK(object_journal_version IN (1,2)),
            FOREIGN KEY (branch_uuid, project_uuid) REFERENCES branches(uuid, project_uuid),
            UNIQUE(branch_uuid, number),
            CHECK(state!='finalized' OR (finalized_at_ms IS NOT NULL AND snapshot_sha256 IS NOT NULL AND chain_sha256 IS NOT NULL))
        ))SQL",
        R"SQL(CREATE TABLE role_assignments (
        uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
        principal_kind TEXT NOT NULL CHECK(principal_kind IN ('user','group')),
        principal_uuid BLOB NOT NULL CHECK(length(principal_uuid)=16),
        role_uuid BLOB NOT NULL REFERENCES roles(uuid) ON DELETE RESTRICT,
        scope_kind TEXT NOT NULL CHECK(scope_kind IN ('system','project','branch')),
        scope_uuid BLOB CHECK(scope_uuid IS NULL OR length(scope_uuid)=16),
        created_by_uuid BLOB NOT NULL REFERENCES users(uuid),
        created_at_ms INTEGER NOT NULL,
        revoked_at_ms INTEGER,
        revoked_by_uuid BLOB REFERENCES users(uuid),
        CHECK((scope_kind='system' AND scope_uuid IS NULL) OR
              (scope_kind IN ('project','branch') AND scope_uuid IS NOT NULL)),
        CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
    ))SQL",
        R"SQL(CREATE TABLE role_permissions (
        uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
        role_uuid BLOB NOT NULL REFERENCES roles(uuid) ON DELETE RESTRICT,
        permission_uuid BLOB NOT NULL REFERENCES permissions(uuid) ON DELETE RESTRICT,
        revoked_at_ms INTEGER,
        revoked_by_uuid BLOB REFERENCES users(uuid),
        CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
    ))SQL",
        R"SQL(CREATE TABLE roles (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            key TEXT NOT NULL UNIQUE,
            builtin INTEGER NOT NULL DEFAULT 0 CHECK(builtin IN (0,1))
        ))SQL",
        R"SQL(CREATE TABLE security_audit_events (
            sequence INTEGER PRIMARY KEY AUTOINCREMENT,
            uuid BLOB NOT NULL UNIQUE CHECK(length(uuid)=16),
            actor_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            subject_uuid BLOB NOT NULL CHECK(length(subject_uuid)=16),
            action TEXT NOT NULL CHECK(length(action)>0),
            occurred_at_ms INTEGER NOT NULL,
            previous_sha256 BLOB NOT NULL CHECK(length(previous_sha256)=32),
            chain_sha256 BLOB NOT NULL CHECK(length(chain_sha256)=32)
        ))SQL",
        R"SQL(CREATE TABLE snapshots (
            revision_uuid BLOB PRIMARY KEY NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            format_version INTEGER NOT NULL DEFAULT 1 CHECK(format_version=1),
            content BLOB NOT NULL
        ))SQL",
        R"SQL(CREATE TABLE user_password_credentials (
        user_uuid BLOB PRIMARY KEY NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
        salt BLOB NOT NULL CHECK(length(salt)=16),
        verifier BLOB NOT NULL CHECK(length(verifier)=32),
        iterations INTEGER NOT NULL CHECK(iterations>=100000 AND iterations<=2000000),
        changed_at_ms INTEGER NOT NULL,
        changed_by_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
        failed_attempts INTEGER NOT NULL DEFAULT 0 CHECK(failed_attempts>=0 AND failed_attempts<=5),
        locked_until_ms INTEGER NOT NULL DEFAULT 0
    ))SQL",
        R"SQL(CREATE TABLE user_sessions (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            user_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            token_sha256 BLOB NOT NULL UNIQUE CHECK(length(token_sha256)=32),
            created_by_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            created_at_ms INTEGER NOT NULL,
            expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms>created_at_ms),
            revoked_at_ms INTEGER,
            CHECK(revoked_at_ms IS NULL OR revoked_at_ms>=created_at_ms)
        ))SQL",
        R"SQL(CREATE TABLE users (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            username TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(username)>0),
            created_at_ms INTEGER NOT NULL,
            disabled_at_ms INTEGER
        ))SQL",
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
        principal_kind,principal_uuid,role_uuid,scope_kind,COALESCE(hex(scope_uuid),''))
        WHERE revoked_at_ms IS NULL)SQL",
        R"SQL(CREATE UNIQUE INDEX role_permission_active_unique
        ON role_permissions(role_uuid,permission_uuid) WHERE revoked_at_ms IS NULL)SQL",
        R"SQL(CREATE INDEX security_audit_by_actor ON security_audit_events(actor_uuid,sequence))SQL",
        R"SQL(CREATE INDEX user_sessions_by_user ON user_sessions(user_uuid))SQL",
        R"SQL(CREATE TRIGGER branch_creator_required BEFORE INSERT ON branches
            WHEN NEW.created_by_uuid IS NULL OR length(NEW.created_by_uuid)!=16
            BEGIN SELECT RAISE(ABORT,'branch creator required'); END)SQL",
        R"SQL(CREATE TRIGGER branch_head_must_be_working
            BEFORE UPDATE OF head_revision_uuid ON branches
            WHEN NEW.head_revision_uuid IS NOT NULL AND NOT EXISTS (
                SELECT 1 FROM revisions WHERE uuid=NEW.head_revision_uuid
                AND branch_uuid=NEW.uuid AND state='working')
            BEGIN SELECT RAISE(ABORT, 'branch head must be its working revision'); END)SQL",
        R"SQL(CREATE TRIGGER finalize_actor_required BEFORE UPDATE OF state ON revisions
            WHEN NEW.state='finalized' AND (NEW.hash_format_version!=2 OR
                NEW.finalized_by_uuid IS NULL OR length(NEW.finalized_by_uuid)!=16)
            BEGIN SELECT RAISE(ABORT,'Finalization actor required'); END)SQL",
        R"SQL(CREATE TRIGGER finalize_requires_history_hash BEFORE UPDATE OF state ON revisions
            WHEN NEW.state='finalized' AND NEW.operation_history_sha256 IS NULL
            BEGIN SELECT RAISE(ABORT,'operation history seal required'); END)SQL",
        R"SQL(CREATE TRIGGER finalize_requires_snapshot
            BEFORE UPDATE OF state ON revisions WHEN NEW.state='finalized'
            AND NOT EXISTS(SELECT 1 FROM snapshots WHERE revision_uuid=NEW.uuid)
            BEGIN SELECT RAISE(ABORT, 'cannot finalize without a snapshot'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_hydraulic_member_delete BEFORE DELETE ON hydraulic_network_members
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT,'finalized network membership is immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_hydraulic_member_insert BEFORE INSERT ON hydraulic_network_members
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT,'finalized network membership is immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_hydraulic_reference_delete
            BEFORE DELETE ON hydraulic_network_references
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT,'finalized hydraulic references are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_hydraulic_reference_insert
            BEFORE INSERT ON hydraulic_network_references
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT,'finalized hydraulic references are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_object_delete BEFORE DELETE ON revision_objects
             WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
             BEGIN SELECT RAISE(ABORT,'finalized objects are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_object_insert BEFORE INSERT ON revision_objects
             WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
             BEGIN SELECT RAISE(ABORT,'finalized objects are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_object_update BEFORE UPDATE ON revision_objects
             WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
             BEGIN SELECT RAISE(ABORT,'finalized objects are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_parents_readonly_insert
            BEFORE INSERT ON revision_parents
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revision parents are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_revision_readonly_delete
            BEFORE DELETE ON revisions WHEN OLD.state='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revisions cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_revision_readonly_update
            BEFORE UPDATE ON revisions WHEN OLD.state='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revisions are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_snapshot_readonly_delete
            BEFORE DELETE ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_snapshot_readonly_insert
            BEFORE INSERT ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots cannot be inserted'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_snapshot_readonly_update
            BEFORE UPDATE ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER group_membership_no_delete BEFORE DELETE ON group_memberships
        BEGIN SELECT RAISE(ABORT,'membership history cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER group_membership_revoke_only BEFORE UPDATE ON group_memberships
        WHEN NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NEW.uuid AND OLD.group_uuid IS NEW.group_uuid
              AND OLD.user_uuid IS NEW.user_uuid AND OLD.created_by_uuid IS NEW.created_by_uuid
              AND OLD.created_at_ms IS NEW.created_at_ms)
        BEGIN SELECT RAISE(ABORT,'membership can only be revoked once'); END)SQL",
        R"SQL(CREATE TRIGGER hydraulic_member_update_block BEFORE UPDATE ON hydraulic_network_members
            BEGIN SELECT RAISE(ABORT,'network membership must be replaced transactionally'); END)SQL",
        R"SQL(CREATE TRIGGER hydraulic_reference_update_block
            BEFORE UPDATE ON hydraulic_network_references
            BEGIN SELECT RAISE(ABORT,'hydraulic references cannot be updated'); END)SQL",
        R"SQL(CREATE TRIGGER immutable_object_event_delete BEFORE DELETE ON object_events
             BEGIN SELECT RAISE(ABORT,'object events cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER immutable_object_event_update BEFORE UPDATE ON object_events
             BEGIN SELECT RAISE(ABORT,'object events are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER immutable_revision_identity
            BEFORE UPDATE ON revisions WHEN OLD.uuid!=NEW.uuid
                OR OLD.project_uuid!=NEW.project_uuid OR OLD.branch_uuid!=NEW.branch_uuid
                OR OLD.number!=NEW.number
            BEGIN SELECT RAISE(ABORT, 'revision identity and ancestry are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER object_event_insert_guard BEFORE INSERT ON object_events
             WHEN NOT EXISTS (SELECT 1 FROM revisions r WHERE r.uuid=NEW.revision_uuid
               AND r.state='working' AND r.object_mode=1 AND r.generation=NEW.sequence)
             BEGIN SELECT RAISE(ABORT,'object event must match current revision'); END)SQL",
        R"SQL(CREATE TRIGGER object_group_delete_guard BEFORE DELETE ON object_groups
              BEGIN SELECT RAISE(ABORT,'object groups are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER object_group_insert_guard BEFORE INSERT ON object_groups
              WHEN NOT EXISTS(SELECT 1 FROM revisions WHERE uuid=NEW.revision_uuid
                AND state='working' AND object_mode=1 AND generation=NEW.last_sequence)
                OR (NEW.target_group_uuid IS NOT NULL AND NOT EXISTS (
                  SELECT 1 FROM object_groups prior WHERE prior.uuid=NEW.target_group_uuid
                    AND prior.revision_uuid=NEW.revision_uuid AND prior.action='edit'))
              BEGIN SELECT RAISE(ABORT,'invalid object group or target'); END)SQL",
        R"SQL(CREATE TRIGGER object_group_member_delete_guard BEFORE DELETE ON object_group_events
              BEGIN SELECT RAISE(ABORT,'object group members are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER object_group_member_insert_guard BEFORE INSERT ON object_group_events
              WHEN NOT EXISTS(
                SELECT 1 FROM object_groups g JOIN object_events e ON e.uuid=NEW.object_event_uuid
                JOIN revisions r ON r.uuid=g.revision_uuid
                WHERE g.uuid=NEW.group_uuid AND e.revision_uuid=g.revision_uuid
                  AND e.actor_uuid=g.actor_uuid AND r.state='working'
                  AND e.sequence BETWEEN g.first_sequence AND g.last_sequence)
              BEGIN SELECT RAISE(ABORT,'object event/group mismatch'); END)SQL",
        R"SQL(CREATE TRIGGER object_group_member_update_guard BEFORE UPDATE ON object_group_events
              BEGIN SELECT RAISE(ABORT,'object group members are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER object_group_update_guard BEFORE UPDATE ON object_groups
              BEGIN SELECT RAISE(ABORT,'object groups are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER object_identity_readonly BEFORE UPDATE ON revision_objects
             WHEN OLD.revision_uuid!=NEW.revision_uuid OR OLD.object_uuid!=NEW.object_uuid
                 OR OLD.kind!=NEW.kind
             BEGIN SELECT RAISE(ABORT,'object identity and type are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER operation_events_delete_guard BEFORE DELETE ON operation_events
            BEGIN SELECT RAISE(ABORT,'operation history cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER operation_events_insert_guard BEFORE INSERT ON operation_events
            WHEN NOT EXISTS (
                SELECT 1 FROM revisions r WHERE r.uuid=NEW.revision_uuid
                AND r.project_uuid=NEW.project_uuid AND r.branch_uuid=NEW.branch_uuid
                AND r.state='working' AND r.generation=NEW.sequence)
              OR (NEW.kind IN ('undo','redo') AND NOT EXISTS (
                SELECT 1 FROM operation_events prior WHERE prior.uuid=NEW.target_event_uuid
                AND prior.revision_uuid=NEW.revision_uuid AND prior.kind='replace'))
            BEGIN SELECT RAISE(ABORT,'operation must match current working revision and generation'); END)SQL",
        R"SQL(CREATE TRIGGER operation_events_update_guard BEFORE UPDATE ON operation_events
            BEGIN SELECT RAISE(ABORT,'operation history is immutable'); END)SQL",
        R"SQL(CREATE TRIGGER parent_must_be_finalized_in_same_project
            BEFORE INSERT ON revision_parents
            WHEN NOT EXISTS (
                SELECT 1 FROM revisions child JOIN revisions parent
                    ON parent.uuid=NEW.parent_revision_uuid
                WHERE child.uuid=NEW.revision_uuid AND parent.state='finalized'
                    AND parent.project_uuid=child.project_uuid)
            BEGIN SELECT RAISE(ABORT, 'parent must be finalized in same project'); END)SQL",
        R"SQL(CREATE TRIGGER parents_readonly_delete
            BEFORE DELETE ON revision_parents
            BEGIN SELECT RAISE(ABORT, 'revision parent edges cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER parents_readonly_update
            BEFORE UPDATE ON revision_parents
            BEGIN SELECT RAISE(ABORT, 'revision parent edges are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER project_creator_required BEFORE INSERT ON projects
            WHEN NEW.created_by_uuid IS NULL OR length(NEW.created_by_uuid)!=16
            BEGIN SELECT RAISE(ABORT,'project creator required'); END)SQL",
        R"SQL(CREATE TRIGGER recovery_tokens_close_only BEFORE UPDATE ON password_recovery_tokens
            WHEN NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid
               OR NEW.token_sha256!=OLD.token_sha256 OR NEW.issued_by_uuid!=OLD.issued_by_uuid
               OR NEW.issued_at_ms!=OLD.issued_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms
               OR OLD.used_at_ms IS NOT NEW.used_at_ms AND OLD.used_at_ms IS NOT NULL
               OR OLD.invalidated_at_ms IS NOT NEW.invalidated_at_ms AND OLD.invalidated_at_ms IS NOT NULL
            BEGIN SELECT RAISE(ABORT,'recovery identity and closure are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER recovery_tokens_no_delete BEFORE DELETE ON password_recovery_tokens
            BEGIN SELECT RAISE(ABORT,'recovery tokens cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER revision_creator_required BEFORE INSERT ON revisions
            WHEN NEW.created_by_uuid IS NULL OR length(NEW.created_by_uuid)!=16
            BEGIN SELECT RAISE(ABORT,'revision creator required'); END)SQL",
        R"SQL(CREATE TRIGGER role_assignment_no_delete BEFORE DELETE ON role_assignments
        BEGIN SELECT RAISE(ABORT,'role assignment history cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER role_assignment_revoke_only BEFORE UPDATE ON role_assignments
        WHEN NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NEW.uuid AND OLD.principal_kind IS NEW.principal_kind
              AND OLD.principal_uuid IS NEW.principal_uuid AND OLD.role_uuid IS NEW.role_uuid
              AND OLD.scope_kind IS NEW.scope_kind AND OLD.scope_uuid IS NEW.scope_uuid
              AND OLD.created_by_uuid IS NEW.created_by_uuid
              AND OLD.created_at_ms IS NEW.created_at_ms)
        BEGIN SELECT RAISE(ABORT,'role assignment can only be revoked once'); END)SQL",
        R"SQL(CREATE TRIGGER role_assignment_valid BEFORE INSERT ON role_assignments
        WHEN NOT (
            (NEW.principal_kind='user' AND EXISTS(SELECT 1 FROM users WHERE uuid=NEW.principal_uuid)) OR
            (NEW.principal_kind='group' AND EXISTS(SELECT 1 FROM groups WHERE uuid=NEW.principal_uuid)))
          OR NOT (
            NEW.scope_kind='system' OR
            (NEW.scope_kind='project' AND EXISTS(SELECT 1 FROM projects WHERE uuid=NEW.scope_uuid)) OR
            (NEW.scope_kind='branch' AND EXISTS(SELECT 1 FROM branches WHERE uuid=NEW.scope_uuid)))
        BEGIN SELECT RAISE(ABORT,'invalid principal or scope'); END)SQL",
        R"SQL(CREATE TRIGGER role_permission_no_delete BEFORE DELETE ON role_permissions
        BEGIN SELECT RAISE(ABORT,'role permission history cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER role_permission_revoke_only BEFORE UPDATE ON role_permissions
        WHEN NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NEW.uuid AND OLD.role_uuid IS NEW.role_uuid
              AND OLD.permission_uuid IS NEW.permission_uuid)
        BEGIN SELECT RAISE(ABORT,'role permission can only be revoked once'); END)SQL",
        R"SQL(CREATE TRIGGER security_audit_readonly_delete
            BEFORE DELETE ON security_audit_events
            BEGIN SELECT RAISE(ABORT,'security audit events cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER security_audit_readonly_update
            BEFORE UPDATE ON security_audit_events
            BEGIN SELECT RAISE(ABORT,'security audit events are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER user_sessions_immutable_delete BEFORE DELETE ON user_sessions
            BEGIN SELECT RAISE(ABORT,'sessions are append-only'); END)SQL",
        R"SQL(CREATE TRIGGER user_sessions_immutable_identity BEFORE UPDATE ON user_sessions
            WHEN NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid
                OR NEW.created_by_uuid!=OLD.created_by_uuid
                OR NEW.created_at_ms!=OLD.created_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms
                OR (OLD.revoked_at_ms IS NOT NULL AND NEW.revoked_at_ms IS NOT OLD.revoked_at_ms)
                OR (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
                    AND NEW.revoked_at_ms<OLD.created_at_ms)
                OR (NEW.token_sha256!=OLD.token_sha256 AND NOT (
                    NEW.token_sha256=CAST(x'414f5749532d5245444143542d563121' || OLD.uuid AS BLOB)
                    AND (OLD.revoked_at_ms IS NOT NULL OR OLD.expires_at_ms<=CAST((julianday('now')-2440587.5)*86400000 AS INTEGER))
                ))
            BEGIN SELECT RAISE(ABORT,'session identity is immutable; spent credentials may only be redacted'); END)SQL",
    };
    for (const char *statement : statements)
    {
        QSqlQuery query(database);
        if (!query.exec(QString::fromUtf8(statement)))
        {
            if (error != nullptr)
                *error = query.lastError().text();
            return false;
        }
    }
    return true;
}
}
