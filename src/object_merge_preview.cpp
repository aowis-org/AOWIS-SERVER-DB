#include <aowis/db/database.h>
#include "object_merge_state.h"
#include "model_field_merge.h"
#include "content_store.h"
#include "revision_root.h"

#include <algorithm>
#include <limits>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QSet>

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

// Three-way merge comparisons use ordered UUID cursors. Finalized tips are
// authoritative Merkle snapshots; only editable tips use SQL. Immutable
// content is fetched in bounded batches, not one PostgreSQL call per object
// and not three entire model payloads resident at once.
struct LiveObjectCursor
{
    QSqlQuery query;
    const StorageBackend &backend_;
    const QSqlDatabase &database_;
    std::optional<ModelObject> current;
    QByteArray previous_id;
    bool sealed_ = false;
    QVector<merkle_tree::Entry> entries_;
    qsizetype next_entry_ = 0;
    QVector<MerkleObjectRecord> batch_;
    qsizetype next_batch_ = 0;

    LiveObjectCursor(const QSqlDatabase &database, const StorageBackend &backend)
        : query(database), backend_(backend), database_(database)
    {
        if (database.driverName() == QStringLiteral("QSQLITE"))
            this->query.setForwardOnly(true);
    }

    bool open(const QUuid &revision_uuid, bool finalized, const QByteArray &root,
              const QSet<QUuid> *selected, QString *error)
    {
        this->sealed_ = finalized;
        if (finalized)
        {
            if (root.size() != 32)
            {
                setError(error, QStringLiteral("Finalized merge revision has no valid Merkle root"));
                return false;
            }
            if (selected != nullptr)
            {
                // Both merge tips are finalized. The union of the two Merkle
                // differences against the base is a complete candidate set.
                // No unchanged blob payloads need to be loaded for this cursor.
                const std::optional<QHash<QUuid, QByteArray>> hashes =
                    merkle_tree::lookupMany(this->database_, root, *selected, error);
                if (!hashes)
                    return false;
                this->entries_.reserve(hashes->size());
                for (QHash<QUuid, QByteArray>::const_iterator it = hashes->cbegin();
                     it != hashes->cend(); ++it)
                    this->entries_.append(merkle_tree::Entry{it.key(), it.value()});
                std::sort(this->entries_.begin(), this->entries_.end(),
                    [](const merkle_tree::Entry &left, const merkle_tree::Entry &right) {
                        return left.key.toRfc4122() < right.key.toRfc4122();
                    });
            }
            else
            {
                // Editable tips still need a complete SQL cursor, so the
                // original full-scan behavior remains available for them.
                const std::optional<QVector<merkle_tree::Entry>> listed =
                    merkle_tree::entries(this->database_, root, error);
                if (!listed)
                    return false;
                this->entries_ = *listed;
            }
            return advance(error);
        }
        if (!this->query.prepare(QStringLiteral(
                "SELECT object_uuid,kind,content,version FROM revision_objects WHERE storage_uuid=(SELECT "
            "b.working_storage_uuid FROM branches b JOIN revisions r ON r.branch_uuid=b.uuid AND "
            "r.project_uuid=b.project_uuid WHERE r.uuid=? AND r.state='working') AND deleted=0 ORDER BY object_uuid")))
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
        if (this->sealed_)
        {
            while (true)
            {
                if (this->next_batch_ >= this->batch_.size())
                {
                    if (this->next_entry_ >= this->entries_.size())
                        return true;
                    const qsizetype count = qMin(qsizetype(256),
                        this->entries_.size() - this->next_entry_);
                    const QVector<merkle_tree::Entry> portion =
                        this->entries_.mid(this->next_entry_, count);
                    const std::optional<QVector<MerkleObjectRecord>> fetched =
                        readRevisionObjectEntries(this->database_, portion, error);
                    if (!fetched)
                        return false;
                    this->batch_ = *fetched;
                    this->next_batch_ = 0;
                    this->next_entry_ += count;
                }
                const MerkleObjectRecord &record = this->batch_.at(this->next_batch_++);
                if (record.deleted)
                    continue;
                const QByteArray raw_uuid = record.uuid.toRfc4122();
                if (record.uuid.isNull() ||
                    (!this->previous_id.isEmpty() && raw_uuid <= this->previous_id))
                {
                    setError(error, QStringLiteral("Invalid or duplicate UUID in Merkle merge stream"));
                    return false;
                }
                this->previous_id = raw_uuid;
                this->current = ModelObject{record.uuid, record.kind, record.content,
                    record.version, false};
                return true;
            }
        }
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
        this->entries_.clear();
        this->batch_.clear();
    }
};

bool sameContent(const std::optional<ModelObject> &a, const std::optional<ModelObject> &b)
{
    if (!a || !b)
        return !a && !b;
    return a->kind == b->kind && a->content == b->content;
}

} // namespace

std::optional<QByteArray> mergePhysicalObjectDigest(const QSqlDatabase &database,
    const StorageBackend &backend, const QUuid &revision_uuid,
    const QByteArray &sealed_root, QString *error)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto addField = [&hash](const QByteArray &value) {
        hash.addData(QByteArray::number(value.size()));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(value);
    };
    if (!sealed_root.isEmpty())
    {
        // A finalized left tip is immutable and cannot be a merge-commit
        // target. Its content-addressed root commits to all object records,
        // versions and tombstones, so a domain-separated root fingerprint is
        // sufficient for the transient preview token. Verify the stored root
        // node itself; candidate lookups verify every traversed descendant.
        // A complete audit of *all* descendants remains verifyProject()'s job.
        if (sealed_root.size() != 32 ||
            !content_store::get(database, content_store::Kind::Tree, sealed_root, error))
        {
            if (sealed_root.size() != 32)
                setError(error, QStringLiteral("Invalid finalized merge root digest"));
            return std::nullopt;
        }
        QByteArray preimage("AOWIS-MERGE-SEALED-ROOT-V1");
        preimage.append('\0');
        preimage.append(sealed_root);
        return QCryptographicHash::hash(preimage, QCryptographicHash::Sha256);
    }
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects WHERE storage_uuid=(SELECT "
            "b.working_storage_uuid FROM branches b JOIN revisions r ON r.branch_uuid=b.uuid AND "
            "r.project_uuid=b.project_uuid WHERE r.uuid=? AND r.state='working') ORDER BY object_uuid")))
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
    while (query.next())
    {
        const QUuid object_uuid = backend.readUuid(query.value(0));
        if (object_uuid.isNull())
        {
            setError(error, QStringLiteral("Invalid object UUID in physical merge digest"));
            return std::nullopt;
        }
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

    // Immutable tips support a cheaper three-way candidate scan: a content
    // hash change relative to the base is a superset of all logical changes
    // (version-only changes and tombstones can also differ). Bounded HTTP
    // previews can use it too, but must not accumulate an unbounded set of
    // UUIDs. If there are too many candidates, discard the partial set and
    // use the original bounded-memory full cursors. That fallback matters:
    // there may be more *physical* candidates than logical merge changes.
    // The unchanged payload and changed-entity limits still apply below.
    const bool finalized_tips = left_revision->finalized && right_revision->finalized;
    const bool bounded_preview =
        max_changes != std::numeric_limits<qsizetype>::max() ||
        max_changed_payload_bytes != std::numeric_limits<qsizetype>::max();
    const qsizetype candidate_limit = bounded_preview ?
        qMin(max_changes, qsizetype(4096)) : std::numeric_limits<qsizetype>::max();
    bool use_merkle_candidates = finalized_tips;
    QSet<QUuid> candidates;
    merkle_tree::DiffStats left_diff_stats;
    merkle_tree::DiffStats right_diff_stats;
    if (use_merkle_candidates)
    {
        bool candidate_overflow = false;
        const merkle_tree::DifferenceVisitor collect = [&candidates, &candidate_overflow,
                                                         candidate_limit](
            const merkle_tree::Difference &difference) {
            if (candidates.contains(difference.key))
                return true;
            if (candidates.size() >= candidate_limit)
            {
                candidate_overflow = true;
                return false;
            }
            candidates.insert(difference.key);
            return true;
        };
        // Use a private error string: stopping at the candidate cap is an
        // expected fallback, not a corrupt-tree or user-visible error.
        QString diff_error;
        if (!merkle_tree::diff(this->database_, base->object_root_sha256,
                               left_revision->object_root_sha256, collect,
                               &diff_error, &left_diff_stats) ||
            !merkle_tree::diff(this->database_, base->object_root_sha256,
                               right_revision->object_root_sha256, collect,
                               &diff_error, &right_diff_stats))
        {
            if (!candidate_overflow)
            {
                setError(error, diff_error);
                return fail();
            }
            use_merkle_candidates = false;
            candidates.clear();
        }
    }
    const QSet<QUuid> *selected = use_merkle_candidates ? &candidates : nullptr;
    LiveObjectCursor base_cursor(this->database_, *this->backend_);
    LiveObjectCursor left_cursor(this->database_, *this->backend_);
    LiveObjectCursor right_cursor(this->database_, *this->backend_);
    const auto failScan = [&]() -> std::optional<ObjectMergePreview> {
        base_cursor.finish();
        left_cursor.finish();
        right_cursor.finish();
        return fail();
    };
    if (!base_cursor.open(base_uuid, true, base->object_root_sha256, selected, error) ||
        !left_cursor.open(left_revision_uuid, left_revision->finalized,
                          left_revision->object_root_sha256, selected, error) ||
        !right_cursor.open(right_revision_uuid, right_revision->finalized,
                           right_revision->object_root_sha256, selected, error))
        return failScan();

    // For editable tips this scans all physical SQL rows in the same read
    // transaction as the preview. The merge commit rechecks those exact rows
    // under the write lock, catching even unjournaled modifications. Sealed
    // tips instead use an immutable Merkle-root fingerprint (no SQL state can
    // be merged into a finalized revision).
    const std::optional<QByteArray> physical = mergePhysicalObjectDigest(
        this->database_, *this->backend_, left_revision_uuid,
        left_revision->finalized ? left_revision->object_root_sha256 : QByteArray(), error);
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
    result.merkle_candidate_scan_used = use_merkle_candidates;
    if (use_merkle_candidates)
    {
        result.merkle_candidate_keys = static_cast<quint64>(candidates.size());
        result.merkle_tree_nodes_loaded = left_diff_stats.tree_nodes_loaded +
                                          right_diff_stats.tree_nodes_loaded;
        result.merkle_shared_subtrees_skipped = left_diff_stats.shared_subtrees_skipped +
                                                right_diff_stats.shared_subtrees_skipped;
    }
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
