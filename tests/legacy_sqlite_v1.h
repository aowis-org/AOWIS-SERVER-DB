// Exact schema-v1 DDL, retained solely to test migration without the new schema.
#pragma once

namespace legacy_sqlite_v1
{
static constexpr const char *statements[] = {
    R"V1(CREATE TABLE projects (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            name TEXT NOT NULL CHECK(length(name)>0),
            created_at_ms INTEGER NOT NULL,
            archived_at_ms INTEGER
        ))V1",
    R"V1(CREATE TABLE branches (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            project_uuid BLOB NOT NULL REFERENCES projects(uuid) ON DELETE RESTRICT,
            name TEXT NOT NULL,
            head_revision_uuid BLOB REFERENCES revisions(uuid) DEFERRABLE INITIALLY DEFERRED,
            UNIQUE(project_uuid, name),
            UNIQUE(uuid, project_uuid)
        ))V1",
    R"V1(CREATE TABLE revisions (
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
            chain_sha256 BLOB CHECK(chain_sha256 IS NULL OR length(chain_sha256)=32),
            FOREIGN KEY (branch_uuid, project_uuid) REFERENCES branches(uuid, project_uuid),
            UNIQUE(branch_uuid, number),
            CHECK(state!='finalized' OR (finalized_at_ms IS NOT NULL AND snapshot_sha256 IS NOT NULL AND chain_sha256 IS NOT NULL))
        ))V1",
    R"V1(CREATE TABLE revision_parents (
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            parent_position INTEGER NOT NULL CHECK(parent_position>=0),
            parent_revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            PRIMARY KEY(revision_uuid,parent_position),
            UNIQUE(revision_uuid,parent_revision_uuid)
        ))V1",
    R"V1(CREATE UNIQUE INDEX one_working_revision_per_branch
            ON revisions(branch_uuid) WHERE state='working')V1",
    R"V1(CREATE INDEX revisions_by_project ON revisions(project_uuid))V1",
    R"V1(CREATE TABLE snapshots (
            revision_uuid BLOB PRIMARY KEY NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            format_version INTEGER NOT NULL DEFAULT 1 CHECK(format_version=1),
            content BLOB NOT NULL
        ))V1",
    R"V1(CREATE TRIGGER finalized_revision_readonly_update
            BEFORE UPDATE ON revisions WHEN OLD.state='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revisions are immutable'); END)V1",
    R"V1(CREATE TRIGGER finalized_revision_readonly_delete
            BEFORE DELETE ON revisions WHEN OLD.state='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revisions cannot be deleted'); END)V1",
    R"V1(CREATE TRIGGER immutable_revision_identity
            BEFORE UPDATE ON revisions WHEN OLD.uuid!=NEW.uuid
                OR OLD.project_uuid!=NEW.project_uuid OR OLD.branch_uuid!=NEW.branch_uuid
                OR OLD.number!=NEW.number
            BEGIN SELECT RAISE(ABORT, 'revision identity and ancestry are immutable'); END)V1",
    R"V1(CREATE TRIGGER finalized_parents_readonly_insert
            BEFORE INSERT ON revision_parents
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revision parents are immutable'); END)V1",
    R"V1(CREATE TRIGGER parents_readonly_update
            BEFORE UPDATE ON revision_parents
            BEGIN SELECT RAISE(ABORT, 'revision parent edges are immutable'); END)V1",
    R"V1(CREATE TRIGGER parents_readonly_delete
            BEFORE DELETE ON revision_parents
            BEGIN SELECT RAISE(ABORT, 'revision parent edges cannot be deleted'); END)V1",
    R"V1(CREATE TRIGGER parent_must_be_finalized_in_same_project
            BEFORE INSERT ON revision_parents
            WHEN NOT EXISTS (
                SELECT 1 FROM revisions child JOIN revisions parent
                    ON parent.uuid=NEW.parent_revision_uuid
                WHERE child.uuid=NEW.revision_uuid AND parent.state='finalized'
                    AND parent.project_uuid=child.project_uuid)
            BEGIN SELECT RAISE(ABORT, 'parent must be finalized in same project'); END)V1",
    R"V1(CREATE TRIGGER finalized_snapshot_readonly_update
            BEFORE UPDATE ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots are immutable'); END)V1",
    R"V1(CREATE TRIGGER finalized_snapshot_readonly_delete
            BEFORE DELETE ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots cannot be deleted'); END)V1",
    R"V1(CREATE TRIGGER finalized_snapshot_readonly_insert
            BEFORE INSERT ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots cannot be inserted'); END)V1",
    R"V1(CREATE TRIGGER branch_head_must_be_working
            BEFORE UPDATE OF head_revision_uuid ON branches
            WHEN NEW.head_revision_uuid IS NOT NULL AND NOT EXISTS (
                SELECT 1 FROM revisions WHERE uuid=NEW.head_revision_uuid
                AND branch_uuid=NEW.uuid AND state='working')
            BEGIN SELECT RAISE(ABORT, 'branch head must be its working revision'); END)V1",
    R"V1(CREATE TRIGGER finalize_requires_snapshot
            BEFORE UPDATE OF state ON revisions WHEN NEW.state='finalized'
            AND NOT EXISTS(SELECT 1 FROM snapshots WHERE revision_uuid=NEW.uuid)
            BEGIN SELECT RAISE(ABORT, 'cannot finalize without a snapshot'); END)V1",
};
} // namespace legacy_sqlite_v1
