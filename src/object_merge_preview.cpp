#include <aowis/db/database.h>
#include "object_merge_state.h"
#include "model_field_merge.h"

#include <algorithm>
#include <limits>

#include <QByteArrayView>
#include <QCryptographicHash>

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

// Stream the three object revisions in stable UUID order. Holding only one
// row per revision avoids retaining every unchanged network entity in memory.
// Tombstones are deliberately absent from semantic comparison; their physical
// rows are still covered by the separate left-side generation digest.
struct LiveObjectCursor
{
    QSqlQuery query;
    const StorageBackend &backend_;
    std::optional<ModelObject> current;
    QByteArray previous_id;

    LiveObjectCursor(const QSqlDatabase &database, const StorageBackend &backend)
        : query(database), backend_(backend)
    {
        // SQLite supports interleaved forward-only statements on the same
        // connection. PostgreSQL cannot safely use that access pattern.
        if (database.driverName() == QStringLiteral("QSQLITE"))
            this->query.setForwardOnly(true);
    }

    bool open(const QUuid &revision_uuid, QString *error)
    {
        if (!this->query.prepare(QStringLiteral(
                "SELECT object_uuid,kind,content,version FROM revision_objects "
                "WHERE revision_uuid=? AND deleted=0 ORDER BY object_uuid")))
        {
            setError(error, this->query.lastError().text());
            return false;
        }
        this->query.addBindValue(this->backend_.bindUuid(revision_uuid));
        if (!this->query.exec())
        {
            setError(error, this->query.lastError().text());
            return false;
        }
        return advance(error);
    }

    bool advance(QString *error)
    {
        this->current.reset();
        if (!this->query.next())
        {
            if (this->query.lastError().isValid())
            {
                setError(error, this->query.lastError().text());
                return false;
            }
            this->query.finish();
            return true;
        }
        const QUuid uuid = this->backend_.readUuid(this->query.value(0));
        const QByteArray raw_uuid = uuid.toRfc4122();
        if (uuid.isNull() ||
            (!this->previous_id.isEmpty() && raw_uuid <= this->previous_id))
        {
            setError(error, QStringLiteral("Invalid or duplicate object UUID in revision"));
            return false;
        }
        this->previous_id = raw_uuid;
        this->current = ModelObject{uuid, this->query.value(1).toString(),
            this->query.value(2).toByteArray(), this->query.value(3).toLongLong(), false};
        return true;
    }

    void finish()
    {
        this->current.reset();
        this->query.finish();
    }
};

bool sameContent(const std::optional<ModelObject> &a, const std::optional<ModelObject> &b)
{
    if (!a || !b)
        return !a && !b;
    return a->kind == b->kind && a->content == b->content;
}

} // namespace

std::optional<QByteArray> mergePhysicalObjectDigest(const QSqlDatabase &database, const StorageBackend &backend,
    const QUuid &revision_uuid, QString *error)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? ORDER BY object_uuid")))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.addBindValue(backend.bindUuid(revision_uuid));
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Length-prefixed fields prevent ambiguous concatenations. All physical
    // rows are included (even deleted ones), unlike the semantic preview.
    const auto addField = [&hash](const QByteArray &value) {
        hash.addData(QByteArray::number(value.size()));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(value);
    };
    while (query.next())
    {
        const QUuid object_uuid = backend.readUuid(query.value(0));
        if (object_uuid.isNull())
        {
            setError(error, QStringLiteral("Invalid object UUID in physical merge digest"));
            return std::nullopt;
        }
        // The digest remains the same RFC-4122 byte stream, not the SQL
        // driver's UUID representation. SQLite's historical digest is stable.
        addField(object_uuid.toRfc4122());
        addField(query.value(1).toString().toUtf8());
        addField(query.value(2).toByteArray());
        addField(QByteArray::number(query.value(3).toLongLong()));
        addField(QByteArray::number(query.value(4).toInt()));
    }
    if (query.lastError().isValid())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    return hash.result();
}

std::optional<ObjectMergePreview> Database::previewObjectMerge(const OperationContext &actor,
    const QUuid &left_revision_uuid, const QUuid &right_revision_uuid, QString *error) const
{
    return previewObjectMergeBounded(actor, left_revision_uuid, right_revision_uuid,
        std::numeric_limits<qsizetype>::max(), std::numeric_limits<qsizetype>::max(), error);
}

std::optional<ObjectMergePreview> Database::previewObjectMergeBounded(const OperationContext &actor,
    const QUuid &left_revision_uuid, const QUuid &right_revision_uuid,
    qsizetype max_changes, qsizetype max_changed_payload_bytes, QString *error) const
{
    if (max_changes < 1 || max_changed_payload_bytes < 1)
    {
        setError(error, QStringLiteral("Merge preview limits must be positive"));
        return std::nullopt;
    }
    // A single read transaction produces a coherent SQLite snapshot even if
    // another connection modifies either editable working head concurrently.
    QSqlQuery transaction(this->database_);
    if (!transaction.exec(QStringLiteral("BEGIN")))
    {
        setError(error, transaction.lastError().text());
        return std::nullopt;
    }
    const auto fail = [this]() -> std::optional<ObjectMergePreview> {
        QSqlQuery rollback(this->database_);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return std::nullopt;
    };

    const std::optional<RevisionComparison> ancestry = compareRevisions(
        actor, left_revision_uuid, right_revision_uuid, error);
    if (!ancestry)
        return fail();
    if (ancestry->merge_base_revision_uuids.size() != 1)
    {
        setError(error, QStringLiteral(
            "Object merge preview requires exactly one finalized merge base; "
            "multiple bases need recursive merging"));
        return fail();
    }
    const QUuid base_uuid = ancestry->merge_base_revision_uuids.first();

    // compareRevisions checks both tips; the base might belong to a third
    // branch with independent read permissions. Do not leak its contents.
    const std::optional<RevisionInfo> base = revision(actor, base_uuid, error);
    if (!base || !base->finalized || base->project_uuid != ancestry->project_uuid)
    {
        if (base && error != nullptr)
            *error = QStringLiteral("Merge base is not a finalized revision in this project");
        return fail();
    }
    const std::optional<RevisionInfo> left_revision = revision(actor, left_revision_uuid, error);
    const std::optional<RevisionInfo> right_revision = revision(actor, right_revision_uuid, error);
    if (!left_revision || !right_revision)
        return fail();
    const std::optional<int> base_mode = objectMode(base_uuid, error);
    const std::optional<int> left_mode = objectMode(left_revision_uuid, error);
    const std::optional<int> right_mode = objectMode(right_revision_uuid, error);
    if (!base_mode || !left_mode || !right_mode)
        return fail();
    if (*base_mode != 1 || *left_mode != 1 || *right_mode != 1)
    {
        setError(error, QStringLiteral(
            "Object merge preview requires object-mode base and source revisions; "
            "legacy opaque snapshots cannot be compared by entity"));
        return fail();
    }

    LiveObjectCursor base_cursor(this->database_, *this->backend_);
    LiveObjectCursor left_cursor(this->database_, *this->backend_);
    LiveObjectCursor right_cursor(this->database_, *this->backend_);
    const auto failScan = [&]() -> std::optional<ObjectMergePreview> {
        base_cursor.finish();
        left_cursor.finish();
        right_cursor.finish();
        return fail();
    };
    if (!base_cursor.open(base_uuid, error) ||
        !left_cursor.open(left_revision_uuid, error) ||
        !right_cursor.open(right_revision_uuid, error))
        return failScan();

    // Captured inside the same read transaction as the three-way comparison:
    // an unjournaled update cannot silently alter the working head between
    // this preview and a subsequent write transaction.
    const std::optional<QByteArray> physical = mergePhysicalObjectDigest(
        this->database_, *this->backend_, left_revision_uuid, error);
    if (!physical)
        return failScan();

    ObjectMergePreview result;
    result.left_physical_sha256 = *physical;
    result.project_uuid = ancestry->project_uuid;
    result.base_revision_uuid = base_uuid;
    result.left_revision_uuid = left_revision_uuid;
    result.right_revision_uuid = right_revision_uuid;
    result.left_generation = left_revision->generation;
    result.right_generation = right_revision->generation;
    qsizetype retained_payload_bytes = 0;
    while (base_cursor.current || left_cursor.current || right_cursor.current)
    {
        // All three cursors use ORDER BY object_uuid on the stored BLOB UUID.
        // This is the same RFC4122 byte ordering used by earlier previews.
        QByteArray next_id;
        const auto includeId = [&next_id](const std::optional<ModelObject> &object) {
            if (object)
            {
                const QByteArray candidate = object->uuid.toRfc4122();
                if (next_id.isEmpty() || candidate < next_id)
                    next_id = candidate;
            }
        };
        includeId(base_cursor.current);
        includeId(left_cursor.current);
        includeId(right_cursor.current);
        const QUuid uuid = QUuid::fromRfc4122(next_id);
        const bool advance_base = base_cursor.current && base_cursor.current->uuid == uuid;
        const bool advance_left = left_cursor.current && left_cursor.current->uuid == uuid;
        const bool advance_right = right_cursor.current && right_cursor.current->uuid == uuid;
        // Moving the current row means only changed rows can remain retained
        // in the result; all other model payloads are promptly discarded.
        const std::optional<ModelObject> baseline = advance_base ?
            std::move(base_cursor.current) : std::nullopt;
        const std::optional<ModelObject> left = advance_left ?
            std::move(left_cursor.current) : std::nullopt;
        const std::optional<ModelObject> right = advance_right ?
            std::move(right_cursor.current) : std::nullopt;
        if ((advance_base && !base_cursor.advance(error)) ||
            (advance_left && !left_cursor.advance(error)) ||
            (advance_right && !right_cursor.advance(error)))
            return failScan();
        const bool left_changed = !sameContent(baseline, left);
        const bool right_changed = !sameContent(baseline, right);
        if (!left_changed && !right_changed)
            continue;

        // Refuse oversized previews before copying changed entities or
        // running the field-level codec. These limits apply only to the
        // bounded API; the existing C++ commit path remains unrestricted.
        if (result.changes.size() >= max_changes)
        {
            setError(error, QStringLiteral("Merge preview exceeds changed-entity limit"));
            return failScan();
        }
        qsizetype this_payload_bytes = 0;
        for (const std::optional<ModelObject> *object : {&baseline, &left, &right})
        {
            if (*object)
            {
                const qsizetype length = (*object)->content.size();
                if (length > max_changed_payload_bytes - retained_payload_bytes - this_payload_bytes)
                {
                    setError(error, QStringLiteral("Merge preview exceeds changed-payload limit"));
                    return failScan();
                }
                this_payload_bytes += length;
            }
        }
        retained_payload_bytes += this_payload_bytes;

        ObjectMergeKind kind = ObjectMergeKind::Conflict;
        if (!left_changed)
            kind = ObjectMergeKind::RightOnly;
        else if (!right_changed)
            kind = ObjectMergeKind::LeftOnly;
        else if (sameContent(left, right))
            kind = ObjectMergeKind::BothSame;
        ObjectMergeChange change{uuid, kind, baseline, left, right, {}, std::nullopt};
        if (kind == ObjectMergeKind::Conflict && baseline && left && right &&
            baseline->kind == left->kind && left->kind == right->kind)
        {
            const std::optional<ModelFieldMerge> fields = mergeModelFields(
                uuid, left->kind, baseline->content, left->content, right->content);
            if (fields)
            {
                change.conflicting_fields = fields->conflicting_fields;
                if (fields->conflicting_fields.isEmpty())
                {
                    kind = ObjectMergeKind::FieldsMerged;
                    change.kind = kind;
                    change.automatically_merged = ModelObject{
                        uuid, left->kind, fields->content, left->version, false};
                }
            }
        }
        if (kind == ObjectMergeKind::Conflict)
            ++result.conflict_count;
        result.changes.append(std::move(change));
    }
    base_cursor.finish();
    left_cursor.finish();
    right_cursor.finish();
    QSqlQuery commit(this->database_);
    if (!commit.exec(QStringLiteral("COMMIT")))
    {
        setError(error, commit.lastError().text());
        return failScan();
    }
    return result;
}

} // namespace aowis::db
