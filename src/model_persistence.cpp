#include <aowis/db/model_persistence.h>

#include <QDataStream>
#include <QHash>
#include <QSet>
#include <QIODevice>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
constexpr quint32 root_magic = 0x414e5835; // ANX5
constexpr quint32 root_version = 1;

struct StoredRecord
{
    QUuid uuid;
    QString kind;
    QByteArray content;
};

struct NetworkIndex
{
    NetworkHydraulic metadata;
    QList<QUuid> pipe_materials;
    QList<QUuid> patterns_time;
    QList<QUuid> curves_tank_volume;
    QList<QUuid> curves_pump_head;
    QList<QUuid> curves_pump_efficiency;
    QList<QUuid> curves_valve_headloss;
    QList<QUuid> curves_valve_characteristic;
    QList<QUuid> curves_generic;
    QList<QUuid> nodes_reservoirs;
    QList<QUuid> nodes_junctions;
    QList<QUuid> nodes_tanks;
    QList<QUuid> demand_points;
    QList<QUuid> links_pipes;
    QList<QUuid> links_pumps;
    QList<QUuid> links_valves;
    QList<QUuid> controls_simple;
    QList<QUuid> controls_rules;
    QList<QUuid> map_labels;
};

void assignError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

QByteArray uuidBytes(const QUuid &uuid)
{
    return uuid.toRfc4122();
}

void clearLists(NetworkHydraulic &network)
{
    network.pipe_materials.clear();
    network.patterns_time.clear();
    network.curves_tank_volume.clear();
    network.curves_pump_head.clear();
    network.curves_pump_efficiency.clear();
    network.curves_valve_headloss.clear();
    network.curves_valve_characteristic.clear();
    network.curves_generic.clear();
    network.nodes_reservoirs.clear();
    network.nodes_junctions.clear();
    network.nodes_tanks.clear();
    network.demand_points.clear();
    network.links_pipes.clear();
    network.links_pumps.clear();
    network.links_valves.clear();
    network.controls_simple.clear();
    network.controls_rules.clear();
    network.map_labels.clear();
}

QByteArray encodeIndex(const NetworkIndex &index)
{
    QByteArray output;
    QDataStream stream(&output, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << root_magic << root_version << ModelCodec<NetworkHydraulic>::encode(index.metadata);
    stream << index.pipe_materials;
    stream << index.patterns_time;
    stream << index.curves_tank_volume;
    stream << index.curves_pump_head;
    stream << index.curves_pump_efficiency;
    stream << index.curves_valve_headloss;
    stream << index.curves_valve_characteristic;
    stream << index.curves_generic;
    stream << index.nodes_reservoirs;
    stream << index.nodes_junctions;
    stream << index.nodes_tanks;
    stream << index.demand_points;
    stream << index.links_pipes;
    stream << index.links_pumps;
    stream << index.links_valves;
    stream << index.controls_simple;
    stream << index.controls_rules;
    stream << index.map_labels;
    return stream.status() == QDataStream::Ok ? output : QByteArray();
}

std::optional<NetworkIndex> decodeIndex(const QByteArray &bytes, QString *error)
{
    if (bytes.size() > 256 * 1024 * 1024)
    {
        assignError(error, QStringLiteral("Network index too large"));
        return std::nullopt;
    }
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    QByteArray header;
    stream >> magic >> version >> header;
    if (stream.status() != QDataStream::Ok || magic != root_magic || version != root_version)
    {
        assignError(error, QStringLiteral("Unsupported network index format"));
        return std::nullopt;
    }
    std::optional<NetworkHydraulic> metadata = ModelCodec<NetworkHydraulic>::decode(header, error);
    if (!metadata)
        return std::nullopt;
    NetworkIndex result;
    result.metadata = std::move(*metadata);
    if (!result.metadata.pipe_materials.isEmpty() ||
        !result.metadata.patterns_time.isEmpty() ||
        !result.metadata.curves_tank_volume.isEmpty() ||
        !result.metadata.curves_pump_head.isEmpty() ||
        !result.metadata.curves_pump_efficiency.isEmpty() ||
        !result.metadata.curves_valve_headloss.isEmpty() ||
        !result.metadata.curves_valve_characteristic.isEmpty() ||
        !result.metadata.curves_generic.isEmpty() ||
        !result.metadata.nodes_reservoirs.isEmpty() ||
        !result.metadata.nodes_junctions.isEmpty() ||
        !result.metadata.nodes_tanks.isEmpty() ||
        !result.metadata.demand_points.isEmpty() ||
        !result.metadata.links_pipes.isEmpty() ||
        !result.metadata.links_pumps.isEmpty() ||
        !result.metadata.links_valves.isEmpty() ||
        !result.metadata.controls_simple.isEmpty() ||
        !result.metadata.controls_rules.isEmpty() ||
        !result.metadata.map_labels.isEmpty())
    {
        assignError(error, QStringLiteral("Network index metadata must not contain entity lists"));
        return std::nullopt;
    }
    stream >> result.pipe_materials;
    stream >> result.patterns_time;
    stream >> result.curves_tank_volume;
    stream >> result.curves_pump_head;
    stream >> result.curves_pump_efficiency;
    stream >> result.curves_valve_headloss;
    stream >> result.curves_valve_characteristic;
    stream >> result.curves_generic;
    stream >> result.nodes_reservoirs;
    stream >> result.nodes_junctions;
    stream >> result.nodes_tanks;
    stream >> result.demand_points;
    stream >> result.links_pipes;
    stream >> result.links_pumps;
    stream >> result.links_valves;
    stream >> result.controls_simple;
    stream >> result.controls_rules;
    stream >> result.map_labels;
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
    {
        assignError(error, QStringLiteral("Corrupted network object index"));
        return std::nullopt;
    }
    return result;
}

QSet<QUuid> referencedIds(const NetworkIndex &index)
{
    QSet<QUuid> ids;
    for (const QUuid &uuid : index.pipe_materials) ids.insert(uuid);
    for (const QUuid &uuid : index.patterns_time) ids.insert(uuid);
    for (const QUuid &uuid : index.curves_tank_volume) ids.insert(uuid);
    for (const QUuid &uuid : index.curves_pump_head) ids.insert(uuid);
    for (const QUuid &uuid : index.curves_pump_efficiency) ids.insert(uuid);
    for (const QUuid &uuid : index.curves_valve_headloss) ids.insert(uuid);
    for (const QUuid &uuid : index.curves_valve_characteristic) ids.insert(uuid);
    for (const QUuid &uuid : index.curves_generic) ids.insert(uuid);
    for (const QUuid &uuid : index.nodes_reservoirs) ids.insert(uuid);
    for (const QUuid &uuid : index.nodes_junctions) ids.insert(uuid);
    for (const QUuid &uuid : index.nodes_tanks) ids.insert(uuid);
    for (const QUuid &uuid : index.demand_points) ids.insert(uuid);
    for (const QUuid &uuid : index.links_pipes) ids.insert(uuid);
    for (const QUuid &uuid : index.links_pumps) ids.insert(uuid);
    for (const QUuid &uuid : index.links_valves) ids.insert(uuid);
    for (const QUuid &uuid : index.controls_simple) ids.insert(uuid);
    for (const QUuid &uuid : index.controls_rules) ids.insert(uuid);
    for (const QUuid &uuid : index.map_labels) ids.insert(uuid);
    return ids;
}

bool buildRecords(const NetworkHydraulic &network, NetworkIndex &index,
                  QVector<StoredRecord> &records, QString *error)
{
    if (network.uuid.isNull())
    {
        assignError(error, QStringLiteral("Hydraulic network UUID must not be null"));
        return false;
    }
    index.metadata = network;
    clearLists(index.metadata);
    QSet<QUuid> used{network.uuid};
    for (const HydraulicPipeMaterial &entity : network.pipe_materials)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: pipe_materials"));
            return false;
        }
        used.insert(entity.uuid);
        index.pipe_materials.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicPipeMaterial>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in pipe_materials"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.pipe_material"), data});
    }
    for (const HydraulicPatternTime &entity : network.patterns_time)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: patterns_time"));
            return false;
        }
        used.insert(entity.uuid);
        index.patterns_time.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicPatternTime>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in patterns_time"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.pattern_time"), data});
    }
    for (const HydraulicCurveTankVolume &entity : network.curves_tank_volume)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: curves_tank_volume"));
            return false;
        }
        used.insert(entity.uuid);
        index.curves_tank_volume.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicCurveTankVolume>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in curves_tank_volume"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.curve_tank_volume"), data});
    }
    for (const HydraulicCurvePumpHead &entity : network.curves_pump_head)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: curves_pump_head"));
            return false;
        }
        used.insert(entity.uuid);
        index.curves_pump_head.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicCurvePumpHead>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in curves_pump_head"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.curve_pump_head"), data});
    }
    for (const HydraulicCurvePumpEfficiency &entity : network.curves_pump_efficiency)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: curves_pump_efficiency"));
            return false;
        }
        used.insert(entity.uuid);
        index.curves_pump_efficiency.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicCurvePumpEfficiency>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in curves_pump_efficiency"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.curve_pump_efficiency"), data});
    }
    for (const HydraulicCurveValveHeadloss &entity : network.curves_valve_headloss)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: curves_valve_headloss"));
            return false;
        }
        used.insert(entity.uuid);
        index.curves_valve_headloss.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicCurveValveHeadloss>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in curves_valve_headloss"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.curve_valve_headloss"), data});
    }
    for (const HydraulicCurveValveCharacteristic &entity : network.curves_valve_characteristic)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: curves_valve_characteristic"));
            return false;
        }
        used.insert(entity.uuid);
        index.curves_valve_characteristic.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicCurveValveCharacteristic>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in curves_valve_characteristic"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.curve_valve_characteristic"), data});
    }
    for (const HydraulicCurveGeneric &entity : network.curves_generic)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: curves_generic"));
            return false;
        }
        used.insert(entity.uuid);
        index.curves_generic.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicCurveGeneric>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in curves_generic"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.curve_generic"), data});
    }
    for (const HydraulicNodeReservoir &entity : network.nodes_reservoirs)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: nodes_reservoirs"));
            return false;
        }
        used.insert(entity.uuid);
        index.nodes_reservoirs.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicNodeReservoir>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in nodes_reservoirs"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.reservoir"), data});
    }
    for (const HydraulicNodeJunction &entity : network.nodes_junctions)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: nodes_junctions"));
            return false;
        }
        used.insert(entity.uuid);
        index.nodes_junctions.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicNodeJunction>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in nodes_junctions"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.junction"), data});
    }
    for (const HydraulicNodeTank &entity : network.nodes_tanks)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: nodes_tanks"));
            return false;
        }
        used.insert(entity.uuid);
        index.nodes_tanks.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicNodeTank>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in nodes_tanks"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.tank"), data});
    }
    for (const HydraulicDemandPoint &entity : network.demand_points)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: demand_points"));
            return false;
        }
        used.insert(entity.uuid);
        index.demand_points.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicDemandPoint>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in demand_points"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.demand_point"), data});
    }
    for (const HydraulicLinkPipe &entity : network.links_pipes)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: links_pipes"));
            return false;
        }
        used.insert(entity.uuid);
        index.links_pipes.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicLinkPipe>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in links_pipes"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.pipe"), data});
    }
    for (const HydraulicLinkPump &entity : network.links_pumps)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: links_pumps"));
            return false;
        }
        used.insert(entity.uuid);
        index.links_pumps.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicLinkPump>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in links_pumps"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.pump"), data});
    }
    for (const HydraulicLinkValve &entity : network.links_valves)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: links_valves"));
            return false;
        }
        used.insert(entity.uuid);
        index.links_valves.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicLinkValve>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in links_valves"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.valve"), data});
    }
    for (const HydraulicControlSimple &entity : network.controls_simple)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: controls_simple"));
            return false;
        }
        used.insert(entity.uuid);
        index.controls_simple.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicControlSimple>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in controls_simple"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.control_simple"), data});
    }
    for (const HydraulicControlRule &entity : network.controls_rules)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: controls_rules"));
            return false;
        }
        used.insert(entity.uuid);
        index.controls_rules.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicControlRule>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in controls_rules"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.control_rule"), data});
    }
    for (const HydraulicMapLabel &entity : network.map_labels)
    {
        if (entity.uuid.isNull() || used.contains(entity.uuid))
        {
            assignError(error, QStringLiteral("Null or duplicate UUID in hydraulic network: map_labels"));
            return false;
        }
        used.insert(entity.uuid);
        index.map_labels.append(entity.uuid);
        const QByteArray data = ModelCodec<HydraulicMapLabel>::encode(entity);
        if (data.isEmpty())
        {
            assignError(error, QStringLiteral("Could not encode entity in map_labels"));
            return false;
        }
        records.append(StoredRecord{entity.uuid, QStringLiteral("hydraulic.map_label"), data});
    }
    const QByteArray root = encodeIndex(index);
    if (root.isEmpty())
    {
        assignError(error, QStringLiteral("Could not encode network index"));
        return false;
    }
    records.prepend(StoredRecord{network.uuid, QStringLiteral("hydraulic.network"), root});
    return true;
}

template<typename T>
bool restoreList(QList<T> &target, const QList<QUuid> &uuids,
                 const QString &kind, const QHash<QUuid, ModelObject> &rows,
                 QSet<QUuid> &seen, QString *error)
{
    for (const QUuid &uuid : uuids)
    {
        if (uuid.isNull() || seen.contains(uuid) || !rows.contains(uuid))
        {
            assignError(error, QStringLiteral("Missing or duplicated network entity UUID"));
            return false;
        }
        seen.insert(uuid);
        const ModelObject &row = rows[uuid];
        if (row.deleted || row.kind != kind)
        {
            assignError(error, QStringLiteral("Network entity type mismatch or deletion"));
            return false;
        }
        const std::optional<T> decoded = ModelCodec<T>::decode(row.content, error);
        if (!decoded || decoded->uuid != uuid)
        {
            assignError(error, QStringLiteral("Invalid entity UUID or encoding"));
            return false;
        }
        target.append(*decoded);
    }
    return true;
}

} // namespace

bool Database::saveHydraulicNetwork(const OperationContext &actor,
                                   const QUuid &branch_uuid,
                                   qint64 expected_generation,
                                   const NetworkHydraulic &network,
                                   QString *error)
{
    NetworkIndex index;
    QVector<StoredRecord> requested;
    if (!buildRecords(network, index, requested, error) || !beginWrite(error))
        return false;
    bool ok = false;
    do
    {
        if (!requirePermission(actor, QStringLiteral("project.model.write"),
                               AccessScope::branch(branch_uuid), error)) break;
        const std::optional<BranchInfo> branch_info = branchUnchecked(branch_uuid, error);
        const std::optional<RevisionInfo> revision = branch_info
            ? revisionUnchecked(branch_info->head_revision_uuid, error) : std::nullopt;
        if (!revision || revision->finalized || revision->generation != expected_generation)
        {
            assignError(error, QStringLiteral("Working revision generation conflict"));
            break;
        }
        const std::optional<int> mode = objectMode(revision->uuid, error);
        if (!mode) break;
        if (*mode == 0)
        {
            QSqlQuery legacy(this->database_);
            if (!legacy.prepare(QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM snapshots WHERE revision_uuid=?) OR "
                "EXISTS(SELECT 1 FROM operation_events WHERE revision_uuid=?)")))
            {
                assignError(error, legacy.lastError().text()); break;
            }
            legacy.addBindValue(uuidBytes(revision->uuid));
            legacy.addBindValue(uuidBytes(revision->uuid));
            if (!legacy.exec() || !legacy.next() || legacy.value(0).toBool())
            {
                assignError(error, QStringLiteral("Cannot mix legacy snapshot and typed model writes")); break;
            }
        }

        QHash<QUuid, ModelObject> existing;
        QSqlQuery query(this->database_);
        if (!query.prepare(QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects WHERE revision_uuid=?")))
        {
            assignError(error, query.lastError().text()); break;
        }
        query.addBindValue(uuidBytes(revision->uuid));
        if (!query.exec())
        {
            assignError(error, query.lastError().text()); break;
        }
        while (query.next())
        {
            const ModelObject item{QUuid::fromRfc4122(query.value(0).toByteArray()),
                query.value(1).toString(), query.value(2).toByteArray(),
                query.value(3).toLongLong(), query.value(4).toBool()};
            existing.insert(item.uuid, item);
        }
        query.finish();

        QSet<QUuid> old_ids;
        if (existing.contains(network.uuid) && !existing[network.uuid].deleted)
        {
            const ModelObject &old_root = existing[network.uuid];
            if (old_root.kind != QStringLiteral("hydraulic.network"))
            {
                assignError(error, QStringLiteral("UUID already used by another object type")); break;
            }
            const std::optional<NetworkIndex> previous = decodeIndex(old_root.content, error);
            if (!previous || previous->metadata.uuid != network.uuid) break;
            old_ids = referencedIds(*previous);
        }

        QSet<QUuid> new_ids;
        for (const StoredRecord &item : requested) new_ids.insert(item.uuid);

        // In a multi-network project, a model entity must not be reachable
        // from two independently editable network roots. Never steal an
        // existing entity UUID merely because its kind happens to match.
        bool foreign_reference = false;
        for (const ModelObject &stored : existing)
        {
            if (stored.deleted || stored.uuid == network.uuid ||
                stored.kind != QStringLiteral("hydraulic.network"))
                continue;
            const std::optional<NetworkIndex> foreign = decodeIndex(stored.content, error);
            if (!foreign || foreign->metadata.uuid != stored.uuid)
            {
                assignError(error, QStringLiteral("Corrupted foreign network root"));
                foreign_reference = true; break;
            }
            const QSet<QUuid> foreign_ids = referencedIds(*foreign);
            for (const QUuid &foreign_uuid : foreign_ids)
            {
                if (new_ids.contains(foreign_uuid) || foreign_uuid == network.uuid)
                {
                    assignError(error, QStringLiteral("Model object UUID is owned by another network"));
                    foreign_reference = true; break;
                }
            }
            if (foreign_reference) break;
        }
        if (foreign_reference) break;

        bool write_failed = false;
        for (const StoredRecord &item : requested)
        {
            const bool exists = existing.contains(item.uuid);
            const ModelObject prior = exists ? existing[item.uuid] : ModelObject{};
            if (exists && item.uuid != network.uuid && !old_ids.contains(item.uuid))
            {
                assignError(error, QStringLiteral("Model object UUID already used outside this network"));
                write_failed = true; break;
            }
            if (exists && prior.kind != item.kind)
            {
                assignError(error, QStringLiteral("Object UUID collision across model types"));
                write_failed = true; break;
            }
            if (exists && !prior.deleted && prior.content == item.content)
                continue;
            if (!modifyObjectInTransaction(actor, branch_uuid, item.uuid, item.kind,
                                           item.content, exists ? prior.version : -1, error))
            {
                write_failed = true; break;
            }
        }
        if (write_failed) break;
        for (const QUuid &old_uuid : old_ids)
        {
            if (new_ids.contains(old_uuid)) continue;
            if (!existing.contains(old_uuid) || existing[old_uuid].deleted ||
                !modifyObjectInTransaction(actor, branch_uuid, old_uuid, QString(),
                                           std::nullopt, existing[old_uuid].version, error))
            {
                assignError(error, QStringLiteral("Could not delete old network entity"));
                write_failed = true; break;
            }
        }
        if (write_failed) break;
        ok = true;
    } while (false);
    if (ok) ok = commit(error);
    if (!ok) rollback();
    return ok;
}

std::optional<NetworkHydraulic> Database::loadHydraulicNetwork(
    const OperationContext &actor, const QUuid &revision_uuid,
    const QUuid &network_uuid, QString *error) const
{
    const std::optional<RevisionInfo> revision = revisionUnchecked(revision_uuid, error);
    if (!revision || !requirePermission(actor, QStringLiteral("project.read"),
                                         AccessScope::branch(revision->branch_uuid), error))
        return std::nullopt;
    const QVector<ModelObject> visible = objects(actor, revision_uuid, error);
    QHash<QUuid, ModelObject> rows;
    for (const ModelObject &row : visible) rows.insert(row.uuid, row);
    if (!rows.contains(network_uuid) || rows[network_uuid].kind != QStringLiteral("hydraulic.network"))
    {
        assignError(error, QStringLiteral("Hydraulic network is not stored in this revision"));
        return std::nullopt;
    }
    const std::optional<NetworkIndex> index = decodeIndex(rows[network_uuid].content, error);
    if (!index || index->metadata.uuid != network_uuid)
    {
        assignError(error, QStringLiteral("Hydraulic network identity mismatch"));
        return std::nullopt;
    }
    NetworkHydraulic result = index->metadata;
    QSet<QUuid> seen{network_uuid};
    if (!restoreList(result.pipe_materials, index->pipe_materials, QStringLiteral("hydraulic.pipe_material"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.patterns_time, index->patterns_time, QStringLiteral("hydraulic.pattern_time"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.curves_tank_volume, index->curves_tank_volume, QStringLiteral("hydraulic.curve_tank_volume"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.curves_pump_head, index->curves_pump_head, QStringLiteral("hydraulic.curve_pump_head"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.curves_pump_efficiency, index->curves_pump_efficiency, QStringLiteral("hydraulic.curve_pump_efficiency"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.curves_valve_headloss, index->curves_valve_headloss, QStringLiteral("hydraulic.curve_valve_headloss"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.curves_valve_characteristic, index->curves_valve_characteristic, QStringLiteral("hydraulic.curve_valve_characteristic"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.curves_generic, index->curves_generic, QStringLiteral("hydraulic.curve_generic"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.nodes_reservoirs, index->nodes_reservoirs, QStringLiteral("hydraulic.reservoir"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.nodes_junctions, index->nodes_junctions, QStringLiteral("hydraulic.junction"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.nodes_tanks, index->nodes_tanks, QStringLiteral("hydraulic.tank"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.demand_points, index->demand_points, QStringLiteral("hydraulic.demand_point"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.links_pipes, index->links_pipes, QStringLiteral("hydraulic.pipe"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.links_pumps, index->links_pumps, QStringLiteral("hydraulic.pump"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.links_valves, index->links_valves, QStringLiteral("hydraulic.valve"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.controls_simple, index->controls_simple, QStringLiteral("hydraulic.control_simple"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.controls_rules, index->controls_rules, QStringLiteral("hydraulic.control_rule"),
                     rows, seen, error)) return std::nullopt;
    if (!restoreList(result.map_labels, index->map_labels, QStringLiteral("hydraulic.map_label"),
                     rows, seen, error)) return std::nullopt;
    return result;
}

} // namespace aowis::db
