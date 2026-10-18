#include "storage_backend.h"

#include <aowis/db/model_persistence.h>
#include <aowis/db/model_validation.h>
#include <aowis/model/uuid.h>

#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QIODevice>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>

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

// Read only objects relevant to one incoming network rather than scanning
// all objects in the revision. The composite revision/object primary key is
// used for each bounded IN batch (well below older SQLite variable limits).
// Call only inside the save's BEGIN IMMEDIATE transaction.
bool fetchObjectsForNetworkSave(const StorageBackend &backend, const QSqlDatabase &database,
                                const QUuid &revision_uuid,
                                const QSet<QUuid> &wanted,
                                QHash<QUuid, ModelObject> &result,
                                QString *error)
{
    constexpr qsizetype batch_size = 300;
    const QList<QUuid> ids = wanted.values();
    for (qsizetype offset = 0; offset < ids.size(); offset += batch_size)
    {
        const qsizetype count = qMin(batch_size, ids.size() - offset);
        QString placeholders;
        for (qsizetype i = 0; i < count; ++i)
            placeholders += i == 0 ? QStringLiteral("?") : QStringLiteral(",?");
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral(
                "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
                "WHERE revision_uuid=? AND object_uuid IN (%1)").arg(placeholders)))
        {
            assignError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(backend.bindUuid(revision_uuid));
        for (qsizetype i = 0; i < count; ++i)
            query.addBindValue(backend.bindUuid(ids.at(offset + i)));
        if (!query.exec())
        {
            assignError(error, query.lastError().text());
            return false;
        }
        while (query.next())
        {
            const ModelObject item{backend.readUuid(query.value(0)),
                                   query.value(1).toString(), query.value(2).toByteArray(),
                                   query.value(3).toLongLong(), query.value(4).toBool()};
            result.insert(item.uuid, item);
        }
    }
    return true;
}

// The first insertion of a previously unknown network has no old object
// versions to read or reconcile. The caller has already checked permissions,
// revision generation, all incoming UUIDs and network-wide validation inside
// a write transaction. Use reusable statements for the same writes produced
// by modifyObjectInTransaction: one object row, one generation increment and
// one immutable journal event per entity, in exactly the input order.
//
// Do not use this path for replacements, deletions, tombstones or UUIDs
// already present in the revision. Those retain the checked general path.
bool insertFreshNetworkObjects(const StorageBackend &backend,
                               const QSqlDatabase &database,
                               const RevisionInfo &revision,
                               const OperationContext &actor,
                               const QVector<StoredRecord> &records,
                               bool enable_object_mode,
                               QString *error)
{
    const QVariant revision_id = backend.bindUuid(revision.uuid);
    const QVariant actor_id = backend.bindUuid(actor.actor_uuid);
    if (enable_object_mode)
    {
        QSqlQuery mode(database);
        if (!mode.prepare(QStringLiteral(
                "UPDATE revisions SET object_mode=1 WHERE uuid=? AND object_mode=0")))
        {
            assignError(error, mode.lastError().text());
            return false;
        }
        mode.addBindValue(revision_id);
        if (!mode.exec() || mode.numRowsAffected() != 1)
        {
            assignError(error, mode.lastError().text().isEmpty()
                ? QStringLiteral("Cannot enable object mode") : mode.lastError().text());
            return false;
        }
    }

    QSqlQuery insert(database);
    QSqlQuery generation(database);
    QSqlQuery event(database);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
            "VALUES (?,?,?,?,1,0)")) ||
        !generation.prepare(QStringLiteral(
            "UPDATE revisions SET generation=generation+1 WHERE uuid=? AND state='working'")) ||
        !event.prepare(QStringLiteral(
            "INSERT INTO object_events(uuid,revision_uuid,object_uuid,actor_uuid,sequence,occurred_at_ms,kind,action,"
            "before_version,after_version,before_content,after_content,before_deleted) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)")))
    {
        const QString detail = !insert.lastError().text().isEmpty() ? insert.lastError().text()
            : !generation.lastError().text().isEmpty() ? generation.lastError().text()
            : event.lastError().text();
        assignError(error, detail);
        return false;
    }
    generation.bindValue(0, revision_id);
    insert.bindValue(0, revision_id);
    event.bindValue(1, revision_id);
    event.bindValue(3, actor_id);
    event.bindValue(7, QStringLiteral("put"));
    event.bindValue(8, -1);
    event.bindValue(9, 1);
    event.bindValue(10, QVariant());
    event.bindValue(12, 1);
    qint64 sequence = revision.generation;
    for (const StoredRecord &record : records)
    {
        const QVariant object_id = backend.bindUuid(record.uuid);
        insert.bindValue(1, object_id);
        insert.bindValue(2, record.kind);
        insert.bindValue(3, record.content);
        if (!insert.exec())
        {
            assignError(error, insert.lastError().text());
            return false;
        }
        if (!generation.exec() || generation.numRowsAffected() != 1)
        {
            assignError(error, generation.lastError().text().isEmpty()
                ? QStringLiteral("Working revision generation conflict") : generation.lastError().text());
            return false;
        }
        ++sequence;
        event.bindValue(0, backend.bindUuid(createUuidV7()));
        event.bindValue(2, object_id);
        event.bindValue(4, sequence);
        event.bindValue(5, QDateTime::currentMSecsSinceEpoch());
        event.bindValue(6, record.kind);
        event.bindValue(11, record.content);
        if (!event.exec())
        {
            assignError(error, event.lastError().text());
            return false;
        }
    }
    return true;
}

// Derived ownership is indexed by (revision_uuid, entity_uuid). Checking all
// incoming UUIDs (including the root) also catches accidental collisions with
// another network's entity, even if the object payload was tombstoned.
bool checkIncomingNetworkOwnership(const StorageBackend &backend, const QSqlDatabase &database,
                                   const QUuid &revision_uuid,
                                   const QUuid &network_uuid,
                                   const QSet<QUuid> &incoming,
                                   QString *error)
{
    constexpr qsizetype batch_size = 300;
    const QList<QUuid> ids = incoming.values();
    for (qsizetype offset = 0; offset < ids.size(); offset += batch_size)
    {
        const qsizetype count = qMin(batch_size, ids.size() - offset);
        QString placeholders;
        for (qsizetype i = 0; i < count; ++i)
            placeholders += i == 0 ? QStringLiteral("?") : QStringLiteral(",?");
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral(
                "SELECT network_uuid FROM hydraulic_network_members "
                "WHERE revision_uuid=? AND entity_uuid IN (%1)").arg(placeholders)))
        {
            assignError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(backend.bindUuid(revision_uuid));
        for (qsizetype i = 0; i < count; ++i)
            query.addBindValue(backend.bindUuid(ids.at(offset + i)));
        if (!query.exec())
        {
            assignError(error, query.lastError().text());
            return false;
        }
        while (query.next())
        {
            if (backend.readUuid(query.value(0)) != network_uuid)
            {
                assignError(error, QStringLiteral("Model object UUID is owned by another network"));
                return false;
            }
        }
    }
    return true;
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

// References are derived from the validated Model graph, not authoritative
// revision content. Preserve unchanged rows when a save only modifies scalar
// properties (or when a multi-object edit changes other parts of the graph).
//
// A key encodes fixed-width source/target UUIDs followed by the UTF-8 field
// path. This is unambiguous even when field paths contain arbitrary Unicode.
// All work is performed inside the caller's BEGIN IMMEDIATE transaction.
bool Database::replaceHydraulicReferences(const QUuid &revision_uuid,
                                          const NetworkHydraulic &network,
                                          QString *error) const
{
    const QVariant revision_value = this->backend_->bindUuid(revision_uuid);
    const QVariant network_value = this->backend_->bindUuid(network.uuid);
    QSet<QByteArray> wanted;
    for (const HydraulicReference &reference : hydraulicReferences(network))
    {
        QByteArray key = uuidBytes(reference.source_uuid);
        key.append(uuidBytes(reference.target_uuid));
        key.append(reference.field.toUtf8());
        wanted.insert(key);
    }

    QSet<QByteArray> previous;
    QSqlQuery existing(this->database_);
    if (!existing.prepare(QStringLiteral(
            "SELECT source_uuid,target_uuid,field FROM hydraulic_network_references "
            "WHERE revision_uuid=? AND network_uuid=?")))
    {
        assignError(error, existing.lastError().text());
        return false;
    }
    existing.addBindValue(revision_value);
    existing.addBindValue(network_value);
    if (!existing.exec())
    {
        assignError(error, existing.lastError().text());
        return false;
    }
    while (existing.next())
    {
        QByteArray key = this->backend_->readUuid(existing.value(0)).toRfc4122();
        key.append(this->backend_->readUuid(existing.value(1)).toRfc4122());
        key.append(existing.value(2).toString().toUtf8());
        previous.insert(key);
    }
    existing.finish();

    if (wanted == previous)
        return true;

    QSqlQuery remove(this->database_);
    if (!remove.prepare(QStringLiteral(
            "DELETE FROM hydraulic_network_references WHERE revision_uuid=? "
            "AND network_uuid=? AND source_uuid=? AND target_uuid=? AND field=?")))
    {
        assignError(error, remove.lastError().text());
        return false;
    }
    for (const QByteArray &key : previous)
    {
        if (wanted.contains(key))
            continue;
        remove.bindValue(0, revision_value);
        remove.bindValue(1, network_value);
        remove.bindValue(2, this->backend_->bindUuid(QUuid::fromRfc4122(key.left(16))));
        remove.bindValue(3, this->backend_->bindUuid(QUuid::fromRfc4122(key.mid(16, 16))));
        remove.bindValue(4, QString::fromUtf8(key.constData() + 32, key.size() - 32));
        if (!remove.exec())
        {
            assignError(error, remove.lastError().text());
            return false;
        }
    }

    QSqlQuery insert(this->database_);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO hydraulic_network_references "
            "(revision_uuid,network_uuid,source_uuid,target_uuid,field) VALUES(?,?,?,?,?)")))
    {
        assignError(error, insert.lastError().text());
        return false;
    }
    for (const QByteArray &key : wanted)
    {
        if (previous.contains(key))
            continue;
        insert.bindValue(0, revision_value);
        insert.bindValue(1, network_value);
        insert.bindValue(2, this->backend_->bindUuid(QUuid::fromRfc4122(key.left(16))));
        insert.bindValue(3, this->backend_->bindUuid(QUuid::fromRfc4122(key.mid(16, 16))));
        insert.bindValue(4, QString::fromUtf8(key.constData() + 32, key.size() - 32));
        if (!insert.exec())
        {
            assignError(error, insert.lastError().text());
            return false;
        }
    }
    return true;
}

// Must be invoked inside an existing write transaction. The primary key
// enforces exactly one owning root per entity, including multi-root groups.
bool Database::replaceNetworkMembership(const QUuid &revision_uuid,
                                        const QUuid &network_uuid,
                                        const QSet<QUuid> &entity_uuids,
                                        QString *error) const
{
    QSqlQuery remove(this->database_);
    if (!remove.prepare(QStringLiteral(
            "DELETE FROM hydraulic_network_members WHERE revision_uuid=? AND network_uuid=?")))
    {
        assignError(error, remove.lastError().text());
        return false;
    }
    remove.addBindValue(this->backend_->bindUuid(revision_uuid));
    remove.addBindValue(this->backend_->bindUuid(network_uuid));
    if (!remove.exec())
    {
        assignError(error, remove.lastError().text());
        return false;
    }
    QSqlQuery insert(this->database_);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO hydraulic_network_members(revision_uuid,network_uuid,entity_uuid) "
            "VALUES(?,?,?)")))
    {
        assignError(error, insert.lastError().text());
        return false;
    }
    for (const QUuid &member : entity_uuids)
    {
        insert.bindValue(0, this->backend_->bindUuid(revision_uuid));
        insert.bindValue(1, this->backend_->bindUuid(network_uuid));
        insert.bindValue(2, this->backend_->bindUuid(member));
        if (!insert.exec())
        {
            assignError(error, insert.lastError().text());
            return false;
        }
    }
    return true;
}

bool Database::saveHydraulicNetwork(const OperationContext &actor,
                                   const QUuid &branch_uuid,
                                   qint64 expected_generation,
                                   const NetworkHydraulic &network,
                                   QString *error)
{
    const bool profile = qEnvironmentVariableIsSet("AOWIS_DB_PROFILE_SAVE");
    QElapsedTimer profile_clock;
    if (profile)
        profile_clock.start();
    const ModelValidationReport validation = validateHydraulicNetwork(network);
    if (!validation.valid())
    {
        assignError(error, validation.summary());
        return false;
    }
    NetworkIndex index;
    QVector<StoredRecord> requested;
    if (!buildRecords(network, index, requested, error))
        return false;
    const qint64 prepared_ms = profile ? profile_clock.elapsed() : 0;
    if (!beginWrite(error))
        return false;
    qint64 looked_up_ms = prepared_ms;
    qint64 written_ms = prepared_ms;
    qint64 indexed_ms = prepared_ms;
    qint64 grouped_ms = prepared_ms;
    bool used_fresh_insert = false;
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
            legacy.addBindValue(this->backend_->bindUuid(revision->uuid));
            legacy.addBindValue(this->backend_->bindUuid(revision->uuid));
            if (!legacy.exec() || !legacy.next() || legacy.value(0).toBool())
            {
                assignError(error, QStringLiteral("Cannot mix legacy snapshot and typed model writes")); break;
            }
        }

        // Inspect only this root to determine its previous member UUIDs.
        // All subsequent object and ownership lookups use indexed primary keys.
        QSet<QUuid> old_ids;
        QSqlQuery root(this->database_);
        if (!root.prepare(QStringLiteral(
                "SELECT kind,content FROM revision_objects "
                "WHERE revision_uuid=? AND object_uuid=? AND deleted=0")))
        {
            assignError(error, root.lastError().text()); break;
        }
        root.addBindValue(this->backend_->bindUuid(revision->uuid));
        root.addBindValue(this->backend_->bindUuid(network.uuid));
        if (!root.exec())
        {
            assignError(error, root.lastError().text()); break;
        }
        if (root.next())
        {
            if (root.value(0).toString() != QStringLiteral("hydraulic.network"))
            {
                assignError(error, QStringLiteral("UUID already used by another object type")); break;
            }
            const std::optional<NetworkIndex> previous = decodeIndex(root.value(1).toByteArray(), error);
            if (!previous || previous->metadata.uuid != network.uuid)
            {
                if (previous)
                    assignError(error, QStringLiteral("Corrupted hydraulic network identity"));
                break;
            }
            old_ids = referencedIds(*previous);
        }
        root.finish();

        QSet<QUuid> new_ids;
        for (const StoredRecord &item : requested)
            new_ids.insert(item.uuid);
        if (!checkIncomingNetworkOwnership(*this->backend_, this->database_, revision->uuid,
                                           network.uuid, new_ids, error))
            break;

        // Include previous members for version-checked deletions, even when
        // they are absent from the new incoming root. Objects from unrelated
        // networks (and unrelated generic records) are never deserialized.
        QSet<QUuid> relevant_ids = new_ids;
        relevant_ids.unite(old_ids);
        QHash<QUuid, ModelObject> existing;
        if (!fetchObjectsForNetworkSave(*this->backend_, this->database_, revision->uuid,
                                        relevant_ids, existing, error))
            break;

        if (profile)
            looked_up_ms = profile_clock.elapsed();
        const bool fresh_network = old_ids.isEmpty() && existing.isEmpty();
        used_fresh_insert = fresh_network;
        if (fresh_network)
        {
            // Mode 0 was checked for legacy snapshots above. Switching it on
            // once here is equivalent to the first individual object insert.
            if (!insertFreshNetworkObjects(*this->backend_, this->database_, *revision,
                                           actor, requested, *mode == 0, error))
                break;
        }
        else
        {
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
        }
        if (profile)
            written_ms = profile_clock.elapsed();
        const std::optional<RevisionInfo> after = revisionUnchecked(revision->uuid, error);
        if (!after) break;
        if (after->generation != expected_generation)
        {
            // saveHydraulicNetwork has already validated the complete typed
            // input; keep the derived ownership index in the same transaction.
            // Membership is derived from the root UUID lists. Scalar edits
            // and list reordering do not alter ownership, so avoid deleting
            // and reinserting every member of a large network on those saves.
            const QSet<QUuid> new_members = referencedIds(index);
            if (old_ids != new_members &&
                !replaceNetworkMembership(revision->uuid, network.uuid, new_members, error))
                break;
            if (!replaceHydraulicReferences(revision->uuid, network, error)) break;
            if (profile)
                indexed_ms = profile_clock.elapsed();
            if (!writeObjectGroupInTransaction(*revision, actor, QStringLiteral("edit"),
                                               QUuid(), expected_generation, error)) break;
        }
        if (profile)
        {
            if (after->generation == expected_generation)
                indexed_ms = profile_clock.elapsed();
            grouped_ms = profile_clock.elapsed();
        }
        ok = true;
    } while (false);
    if (ok) ok = commit(error);
    if (!ok) rollback();
    if (ok && profile)
    {
        const qint64 total_ms = profile_clock.elapsed();
        // Opt-in diagnostics, emitted once per save, never per object.
        qInfo().noquote().nospace()
            << "aowis_network_save_profile"
            << " objects=" << requested.size()
            << " fresh_insert=" << (used_fresh_insert ? 1 : 0)
            << " validation_encode_ms=" << prepared_ms
            << " lookup_ms=" << (looked_up_ms - prepared_ms)
            << " writes_ms=" << (written_ms - looked_up_ms)
            << " index_ms=" << (indexed_ms - written_ms)
            << " group_ms=" << (grouped_ms - indexed_ms)
            << " commit_ms=" << (total_ms - grouped_ms)
            << " total_ms=" << total_ms;
    }
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
    return loadHydraulicNetworkUnchecked(revision_uuid, network_uuid, error);
}

std::optional<NetworkHydraulic> Database::loadHydraulicNetworkUnchecked(
    const QUuid &revision_uuid, const QUuid &network_uuid, QString *error) const
{
    // Fetch only the root, then the rows belonging to this network. Other
    // networks in the same revision are never deserialized on a local edit.
    QSqlQuery root(this->database_);
    if (!root.prepare(QStringLiteral(
            "SELECT content FROM revision_objects WHERE revision_uuid=? AND object_uuid=? "
            "AND kind='hydraulic.network' AND deleted=0")))
    {
        assignError(error, root.lastError().text());
        return std::nullopt;
    }
    root.addBindValue(this->backend_->bindUuid(revision_uuid));
    root.addBindValue(this->backend_->bindUuid(network_uuid));
    if (!root.exec())
    {
        assignError(error, root.lastError().text());
        return std::nullopt;
    }
    if (!root.next())
    {
        assignError(error, QStringLiteral("Hydraulic network is not stored in this revision"));
        return std::nullopt;
    }
    const std::optional<NetworkIndex> index = decodeIndex(root.value(0).toByteArray(), error);
    root.finish();
    if (!index || index->metadata.uuid != network_uuid)
    {
        if (index)
            assignError(error, QStringLiteral("Hydraulic network identity mismatch"));
        return std::nullopt;
    }
    const QSet<QUuid> references = referencedIds(*index);
    QHash<QUuid, ModelObject> rows;
    QSqlQuery members(this->database_);
    if (!members.prepare(QStringLiteral(
            "SELECT o.object_uuid,o.kind,o.content,o.version,o.deleted "
            "FROM hydraulic_network_members m JOIN revision_objects o "
            "ON o.revision_uuid=m.revision_uuid AND o.object_uuid=m.entity_uuid "
            "WHERE m.revision_uuid=? AND m.network_uuid=?")))
    {
        assignError(error, members.lastError().text());
        return std::nullopt;
    }
    members.addBindValue(this->backend_->bindUuid(revision_uuid));
    members.addBindValue(this->backend_->bindUuid(network_uuid));
    if (!members.exec())
    {
        assignError(error, members.lastError().text());
        return std::nullopt;
    }
    while (members.next())
    {
        const QUuid uuid = this->backend_->readUuid(members.value(0));
        if (!references.contains(uuid))
            continue; // Root changed, membership index not committed yet.
        rows.insert(uuid, ModelObject{uuid, members.value(1).toString(),
                    members.value(2).toByteArray(), members.value(3).toLongLong(),
                    members.value(4).toBool()});
    }
    members.finish();
    // Only root edits may introduce new referenced UUIDs before the membership
    // index is synchronized. Indexed edits normally need zero extra queries.
    QSqlQuery newly_referenced(this->database_);
    if (!newly_referenced.prepare(QStringLiteral(
            "SELECT kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? AND object_uuid=?")))
    {
        assignError(error, newly_referenced.lastError().text());
        return std::nullopt;
    }
    for (const QUuid &uuid : references)
    {
        if (rows.contains(uuid))
            continue;
        newly_referenced.bindValue(0, this->backend_->bindUuid(revision_uuid));
        newly_referenced.bindValue(1, this->backend_->bindUuid(uuid));
        if (!newly_referenced.exec())
        {
            assignError(error, newly_referenced.lastError().text());
            return std::nullopt;
        }
        if (!newly_referenced.next())
        {
            assignError(error, QStringLiteral("Missing referenced hydraulic entity"));
            return std::nullopt;
        }
        rows.insert(uuid, ModelObject{uuid, newly_referenced.value(0).toString(),
                    newly_referenced.value(1).toByteArray(),
                    newly_referenced.value(2).toLongLong(), newly_referenced.value(3).toBool()});
        newly_referenced.finish();
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

// Compare the fields which the Model reference validator actually observes,
// without reconstructing every entity in the network. The partial network is
// deliberately used ONLY to collect outgoing references, never to judge their
// validity (targets are not loaded). Root-owned options and MSX are excluded.
template<typename T>
std::optional<QVector<HydraulicReference>> outgoingReferences(
    const QByteArray &content, const QUuid &entity_uuid,
    QList<T> NetworkHydraulic::*collection)
{
    const std::optional<T> value = ModelCodec<T>::decode(content);
    if (!value || value->uuid != entity_uuid)
        return std::nullopt;
    NetworkHydraulic partial;
    (partial.*collection).append(*value);
    QVector<HydraulicReference> result;
    for (const HydraulicReference &reference : hydraulicReferences(partial))
        if (reference.source_uuid == entity_uuid)
            result.append(reference);
    return result;
}

bool sameOutgoingReferences(const QVector<HydraulicReference> &before,
                            const QVector<HydraulicReference> &after)
{
    if (before.size() != after.size())
        return false;
    for (qsizetype i = 0; i < before.size(); ++i)
    {
        if (before[i].source_uuid != after[i].source_uuid ||
            before[i].target_uuid != after[i].target_uuid ||
            before[i].field != after[i].field)
            return false;
    }
    return true;
}

template<typename T>
bool unchangedValidationFields(const QByteArray &before, const QByteArray &after,
                               const QUuid &entity_uuid,
                               QList<T> NetworkHydraulic::*collection)
{
    const std::optional<QVector<HydraulicReference>> old_references =
        outgoingReferences(before, entity_uuid, collection);
    const std::optional<QVector<HydraulicReference>> new_references =
        outgoingReferences(after, entity_uuid, collection);
    return old_references && new_references &&
           sameOutgoingReferences(*old_references, *new_references);
}

// An assigned-junction demand point additionally depends on the attachment
// mode and pipe endpoints. Those endpoints cannot change in the fast path,
// and the mode must remain the same. Pipe position itself is validated here.
bool unchangedDemandPointConstraints(const QByteArray &before,
                                     const QByteArray &after,
                                     const QUuid &entity_uuid)
{
    const std::optional<HydraulicDemandPoint> old_point =
        ModelCodec<HydraulicDemandPoint>::decode(before);
    const std::optional<HydraulicDemandPoint> new_point =
        ModelCodec<HydraulicDemandPoint>::decode(after);
    if (!old_point || !new_point || old_point->uuid != entity_uuid ||
        new_point->uuid != entity_uuid ||
        old_point->attachment.type != new_point->attachment.type ||
        old_point->attachment.pipe_allocation_mode != new_point->attachment.pipe_allocation_mode)
        return false;
    if (new_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
    {
        if (!std::isfinite(new_point->attachment.pipe_position) ||
            new_point->attachment.pipe_position < 0.0 ||
            new_point->attachment.pipe_position > 1.0)
            return false;
    }
    return unchangedValidationFields(before, after, entity_uuid,
                                      &NetworkHydraulic::demand_points);
}

// No fast-path for roots, insertions, deletions, or rule premises (their
// node-vs-link discriminator can change the expected target type). For other
// existing objects, stable IDs and outgoing references preserve all graph
// constraints checked by NetworkReferenceValidator. Unknown kinds fall back.
bool referenceNeutralEntityEdit(const QString &kind, const QByteArray &before,
                                const QByteArray &after, const QUuid &entity_uuid)
{
    if (kind == QStringLiteral("hydraulic.demand_point"))
        return unchangedDemandPointConstraints(before, after, entity_uuid);
#define AOWIS_CHECK_ENTITY(TYPE, FIELD) \
    if (kind == HydraulicObjectKind<TYPE>::value()) \
        return unchangedValidationFields(before, after, entity_uuid, &NetworkHydraulic::FIELD);
    AOWIS_CHECK_ENTITY(HydraulicPipeMaterial, pipe_materials)
    AOWIS_CHECK_ENTITY(HydraulicPatternTime, patterns_time)
    AOWIS_CHECK_ENTITY(HydraulicCurveTankVolume, curves_tank_volume)
    AOWIS_CHECK_ENTITY(HydraulicCurvePumpHead, curves_pump_head)
    AOWIS_CHECK_ENTITY(HydraulicCurvePumpEfficiency, curves_pump_efficiency)
    AOWIS_CHECK_ENTITY(HydraulicCurveValveHeadloss, curves_valve_headloss)
    AOWIS_CHECK_ENTITY(HydraulicCurveValveCharacteristic, curves_valve_characteristic)
    AOWIS_CHECK_ENTITY(HydraulicCurveGeneric, curves_generic)
    AOWIS_CHECK_ENTITY(HydraulicNodeReservoir, nodes_reservoirs)
    AOWIS_CHECK_ENTITY(HydraulicNodeJunction, nodes_junctions)
    AOWIS_CHECK_ENTITY(HydraulicNodeTank, nodes_tanks)
    AOWIS_CHECK_ENTITY(HydraulicLinkPipe, links_pipes)
    AOWIS_CHECK_ENTITY(HydraulicLinkPump, links_pumps)
    AOWIS_CHECK_ENTITY(HydraulicLinkValve, links_valves)
    AOWIS_CHECK_ENTITY(HydraulicControlSimple, controls_simple)
    AOWIS_CHECK_ENTITY(HydraulicMapLabel, map_labels)
#undef AOWIS_CHECK_ENTITY
    return false;
}

// After an edit, the latest immutable object event supplies the exact prior
// bytes. This includes undo and redo; a missing or malformed event is always
// a reason to run full validation, never a reason to accept an edit.
bool Database::canSkipHydraulicRevalidation(const QUuid &revision_uuid,
                                            const QUuid &entity_uuid,
                                            QString *error) const
{
    QSqlQuery current(this->database_);
    if (!current.prepare(QStringLiteral(
            "SELECT kind,content,deleted FROM revision_objects "
            "WHERE revision_uuid=? AND object_uuid=?")))
    {
        assignError(error, current.lastError().text());
        return false;
    }
    current.addBindValue(this->backend_->bindUuid(revision_uuid));
    current.addBindValue(this->backend_->bindUuid(entity_uuid));
    if (!current.exec())
    {
        assignError(error, current.lastError().text());
        return false;
    }
    if (!current.next() || current.value(2).toBool())
        return false;
    const QString kind = current.value(0).toString();
    const QByteArray updated = current.value(1).toByteArray();
    current.finish();
    QSqlQuery previous(this->database_);
    if (!previous.prepare(QStringLiteral(
            "SELECT before_content,before_deleted FROM object_events "
            "WHERE revision_uuid=? AND object_uuid=? ORDER BY sequence DESC LIMIT 1")))
    {
        assignError(error, previous.lastError().text());
        return false;
    }
    previous.addBindValue(this->backend_->bindUuid(revision_uuid));
    previous.addBindValue(this->backend_->bindUuid(entity_uuid));
    if (!previous.exec())
    {
        assignError(error, previous.lastError().text());
        return false;
    }
    if (!previous.next() || previous.value(1).toBool() || previous.value(0).isNull())
        return false;
    return referenceNeutralEntityEdit(kind, previous.value(0).toByteArray(),
                                      updated, entity_uuid);
}

// A deliberately bounded structural fast path for existing links' endpoints,
// typed link resources (materials, curves and patterns), and demand-point
// attachments. Every new link reference must resolve to an object of the
// expected kind in the same indexed hydraulic network. The old reverse-
// reference index identifies untouched demand points that may depend on a
// moved PIPE endpoint; pump/valve resource changes have no such dependency.
enum class LocalStructureCheck { NotApplicable, Accepted, SqlError };

struct LocalStructureEdit
{
    QUuid uuid;
    QUuid network_uuid;
    QString kind;
    QByteArray before;
    QByteArray after;
};

bool locallyOwnedObject(const StorageBackend &backend, const QSqlDatabase &database, const QUuid &revision_uuid,
                        const QUuid &network_uuid, const QUuid &object_uuid,
                        QString &kind, QByteArray &content, QString *error)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT o.kind,o.content FROM hydraulic_network_members AS m "
            "JOIN revision_objects AS o ON o.revision_uuid=m.revision_uuid "
            "AND o.object_uuid=m.entity_uuid "
            "WHERE m.revision_uuid=? AND m.network_uuid=? AND m.entity_uuid=? "
            "AND o.deleted=0")))
    {
        assignError(error, query.lastError().text());
        return false;
    }
    query.addBindValue(backend.bindUuid(revision_uuid));
    query.addBindValue(backend.bindUuid(network_uuid));
    query.addBindValue(backend.bindUuid(object_uuid));
    if (!query.exec())
    {
        assignError(error, query.lastError().text());
        return false;
    }
    if (!query.next())
        return false;
    kind = query.value(0).toString();
    content = query.value(1).toByteArray();
    return true;
}

bool localTargetType(const StorageBackend &backend, const QSqlDatabase &database, const QUuid &revision_uuid,
                     const QUuid &network_uuid, const QUuid &target_uuid,
                     const QSet<QString> &allowed, QString *error)
{
    if (target_uuid.isNull())
        return true; // Incomplete draft references are explicitly supported.
    QString kind;
    QByteArray content;
    return locallyOwnedObject(backend, database, revision_uuid, network_uuid,
                              target_uuid, kind, content, error) && allowed.contains(kind);
}

bool localDemandPointConstraints(const StorageBackend &backend, const QSqlDatabase &database,
                                 const QUuid &revision_uuid, const QUuid &network_uuid,
                                 const HydraulicDemandPoint &point, QString *error)
{
    const HydraulicDemandPointAttachment &attachment = point.attachment;
    if (attachment.type == HydraulicDemandPointAttachmentType::Junction)
        return localTargetType(backend, database, revision_uuid, network_uuid,
                               attachment.junction_uuid, {QStringLiteral("hydraulic.junction")}, error);
    if (attachment.type != HydraulicDemandPointAttachmentType::Pipe)
        return true;
    if (!std::isfinite(attachment.pipe_position) || attachment.pipe_position < 0.0 ||
        attachment.pipe_position > 1.0)
        return false;
    if (!localTargetType(backend, database, revision_uuid, network_uuid, attachment.pipe_uuid,
                         {QStringLiteral("hydraulic.pipe")}, error))
        return false;
    if (attachment.pipe_allocation_mode != HydraulicDemandPointPipeAllocationMode::AssignedJunction)
        return true;
    if (!localTargetType(backend, database, revision_uuid, network_uuid,
                         attachment.pipe_assigned_junction_uuid,
                         {QStringLiteral("hydraulic.junction")}, error))
        return false;
    if (attachment.pipe_uuid.isNull() || attachment.pipe_assigned_junction_uuid.isNull())
        return true;
    QString pipe_kind;
    QByteArray pipe_content;
    if (!locallyOwnedObject(backend, database, revision_uuid, network_uuid, attachment.pipe_uuid,
                            pipe_kind, pipe_content, error))
        return false;
    const std::optional<HydraulicLinkPipe> pipe = ModelCodec<HydraulicLinkPipe>::decode(pipe_content);
    return pipe && pipe->uuid == attachment.pipe_uuid &&
           (pipe->node_uuid_from == attachment.pipe_assigned_junction_uuid ||
            pipe->node_uuid_to == attachment.pipe_assigned_junction_uuid);
}

// Only the attachment references are allowed to change on the local point path.
// Demand-pattern references must stay identical (and were already validated).
QVector<HydraulicReference> nonAttachmentReferences(const QVector<HydraulicReference> &references)
{
    QVector<HydraulicReference> result;
    for (const HydraulicReference &reference : references)
        if (!reference.field.contains(QStringLiteral(".attachment.")))
            result.append(reference);
    return result;
}

// The reference-delta writer, scoped to one touched source instead of reconstructing
// a network-sized desired reference set. Only call after ALL local validation
// succeeds and within the caller's ongoing write transaction.
bool updateLocalReferenceSource(const StorageBackend &backend, const QSqlDatabase &database, const QUuid &revision_uuid,
                                const QUuid &network_uuid, const QUuid &source_uuid,
                                const QVector<HydraulicReference> &desired,
                                QString *error)
{
    QSet<QByteArray> wanted;
    for (const HydraulicReference &ref : desired)
    {
        QByteArray key = uuidBytes(ref.target_uuid);
        key.append(ref.field.toUtf8());
        wanted.insert(key);
    }
    QSqlQuery existing(database);
    if (!existing.prepare(QStringLiteral(
            "SELECT target_uuid,field FROM hydraulic_network_references "
            "WHERE revision_uuid=? AND network_uuid=? AND source_uuid=?")))
    {
        assignError(error, existing.lastError().text());
        return false;
    }
    existing.addBindValue(backend.bindUuid(revision_uuid));
    existing.addBindValue(backend.bindUuid(network_uuid));
    existing.addBindValue(backend.bindUuid(source_uuid));
    if (!existing.exec())
    {
        assignError(error, existing.lastError().text());
        return false;
    }
    QSet<QByteArray> previous;
    while (existing.next())
    {
        QByteArray key = backend.readUuid(existing.value(0)).toRfc4122();
        key.append(existing.value(1).toString().toUtf8());
        previous.insert(key);
    }
    existing.finish();
    if (previous == wanted)
        return true;
    QSqlQuery remove(database);
    QSqlQuery insert(database);
    if (!remove.prepare(QStringLiteral(
            "DELETE FROM hydraulic_network_references WHERE revision_uuid=? "
            "AND network_uuid=? AND source_uuid=? AND target_uuid=? AND field=?")) ||
        !insert.prepare(QStringLiteral(
            "INSERT INTO hydraulic_network_references "
            "(revision_uuid,network_uuid,source_uuid,target_uuid,field) "
            "VALUES(?,?,?,?,?)")))
    {
        assignError(error, remove.lastError().isValid() ? remove.lastError().text()
                                                         : insert.lastError().text());
        return false;
    }
    for (const QByteArray &key : previous)
    {
        if (wanted.contains(key))
            continue;
        remove.bindValue(0, backend.bindUuid(revision_uuid));
        remove.bindValue(1, backend.bindUuid(network_uuid));
        remove.bindValue(2, backend.bindUuid(source_uuid));
        remove.bindValue(3, backend.bindUuid(QUuid::fromRfc4122(key.left(16))));
        remove.bindValue(4, QString::fromUtf8(key.constData() + 16, key.size() - 16));
        if (!remove.exec())
        {
            assignError(error, remove.lastError().text());
            return false;
        }
    }
    for (const QByteArray &key : wanted)
    {
        if (previous.contains(key))
            continue;
        insert.bindValue(0, backend.bindUuid(revision_uuid));
        insert.bindValue(1, backend.bindUuid(network_uuid));
        insert.bindValue(2, backend.bindUuid(source_uuid));
        insert.bindValue(3, backend.bindUuid(QUuid::fromRfc4122(key.left(16))));
        insert.bindValue(4, QString::fromUtf8(key.constData() + 16, key.size() - 16));
        if (!insert.exec())
        {
            assignError(error, insert.lastError().text());
            return false;
        }
    }
    return true;
}

LocalStructureCheck checkLocalPipeAndAttachments(const StorageBackend &backend, const QSqlDatabase &database,
                                                 const QUuid &revision_uuid,
                                                 const QSet<QUuid> &touched_uuids,
                                                 QString *error)
{
    QVector<LocalStructureEdit> edits;
    QSet<QUuid> changed_pipes;
    QSqlQuery state(database);
    QSqlQuery owner(database);
    QSqlQuery previous(database);
    if (!state.prepare(QStringLiteral(
            "SELECT kind,content,deleted FROM revision_objects "
            "WHERE revision_uuid=? AND object_uuid=?")) ||
        !owner.prepare(QStringLiteral(
            "SELECT network_uuid FROM hydraulic_network_members "
            "WHERE revision_uuid=? AND entity_uuid=?")) ||
        !previous.prepare(QStringLiteral(
            "SELECT before_content,before_deleted FROM object_events "
            "WHERE revision_uuid=? AND object_uuid=? ORDER BY sequence DESC LIMIT 1")))
    {
        assignError(error, QStringLiteral("Cannot prepare local structural validation queries"));
        return LocalStructureCheck::SqlError;
    }
    for (const QUuid &id : touched_uuids)
    {
        state.bindValue(0, backend.bindUuid(revision_uuid));
        state.bindValue(1, backend.bindUuid(id));
        owner.bindValue(0, backend.bindUuid(revision_uuid));
        owner.bindValue(1, backend.bindUuid(id));
        previous.bindValue(0, backend.bindUuid(revision_uuid));
        previous.bindValue(1, backend.bindUuid(id));
        if (!state.exec() || !owner.exec() || !previous.exec())
        {
            assignError(error, QStringLiteral("Cannot query local structural validation data"));
            return LocalStructureCheck::SqlError;
        }
        if (!state.next() || state.value(2).toBool() || !owner.next() ||
            !previous.next() || previous.value(1).toBool() || previous.value(0).isNull())
            return LocalStructureCheck::NotApplicable;
        const QString kind = state.value(0).toString();
        if (kind != QStringLiteral("hydraulic.pipe") &&
            kind != QStringLiteral("hydraulic.pump") &&
            kind != QStringLiteral("hydraulic.valve") &&
            kind != QStringLiteral("hydraulic.demand_point"))
            return LocalStructureCheck::NotApplicable;
        LocalStructureEdit edit{id, backend.readUuid(owner.value(0)),
                                kind, previous.value(0).toByteArray(), state.value(1).toByteArray()};
        if (kind == QStringLiteral("hydraulic.pipe"))
        {
            const std::optional<HydraulicLinkPipe> before = ModelCodec<HydraulicLinkPipe>::decode(edit.before);
            const std::optional<HydraulicLinkPipe> after = ModelCodec<HydraulicLinkPipe>::decode(edit.after);
            if (!before || !after || before->uuid != id || after->uuid != id)
                return LocalStructureCheck::NotApplicable;
            // Material changes do not affect an attached demand point's
            // assigned endpoint; only endpoint moves require reverse scans.
            if (before->node_uuid_from != after->node_uuid_from ||
                before->node_uuid_to != after->node_uuid_to)
                changed_pipes.insert(id);
        }
        else if (kind == QStringLiteral("hydraulic.pump"))
        {
            const std::optional<HydraulicLinkPump> before = ModelCodec<HydraulicLinkPump>::decode(edit.before);
            const std::optional<HydraulicLinkPump> after = ModelCodec<HydraulicLinkPump>::decode(edit.after);
            if (!before || !after || before->uuid != id || after->uuid != id)
                return LocalStructureCheck::NotApplicable;
        }
        else if (kind == QStringLiteral("hydraulic.valve"))
        {
            const std::optional<HydraulicLinkValve> before = ModelCodec<HydraulicLinkValve>::decode(edit.before);
            const std::optional<HydraulicLinkValve> after = ModelCodec<HydraulicLinkValve>::decode(edit.after);
            if (!before || !after || before->uuid != id || after->uuid != id)
                return LocalStructureCheck::NotApplicable;
        }
        else
        {
            const std::optional<HydraulicDemandPoint> before = ModelCodec<HydraulicDemandPoint>::decode(edit.before);
            const std::optional<HydraulicDemandPoint> after = ModelCodec<HydraulicDemandPoint>::decode(edit.after);
            if (!before || !after || before->uuid != id || after->uuid != id)
                return LocalStructureCheck::NotApplicable;
            const std::optional<QVector<HydraulicReference>> old_refs =
                outgoingReferences(edit.before, id, &NetworkHydraulic::demand_points);
            const std::optional<QVector<HydraulicReference>> new_refs =
                outgoingReferences(edit.after, id, &NetworkHydraulic::demand_points);
            if (!old_refs || !new_refs ||
                !sameOutgoingReferences(nonAttachmentReferences(*old_refs),
                                        nonAttachmentReferences(*new_refs)))
                return LocalStructureCheck::NotApplicable;
        }
        edits.append(edit);
        state.finish();
        owner.finish();
        previous.finish();
    }

    const QSet<QString> node_kinds{QStringLiteral("hydraulic.junction"),
                                   QStringLiteral("hydraulic.reservoir"),
                                   QStringLiteral("hydraulic.tank")};
    const QSet<QString> material_kinds{QStringLiteral("hydraulic.pipe_material")};
    const QSet<QString> pattern_kinds{QStringLiteral("hydraulic.pattern_time")};
    const QSet<QString> pump_head_kinds{QStringLiteral("hydraulic.curve_pump_head")};
    const QSet<QString> pump_efficiency_kinds{QStringLiteral("hydraulic.curve_pump_efficiency")};
    const QSet<QString> valve_headloss_kinds{QStringLiteral("hydraulic.curve_valve_headloss")};
    const QSet<QString> valve_characteristic_kinds{QStringLiteral("hydraulic.curve_valve_characteristic")};
    for (const LocalStructureEdit &edit : edits)
    {
        if (edit.kind == QStringLiteral("hydraulic.pipe"))
        {
            const std::optional<HydraulicLinkPipe> pipe = ModelCodec<HydraulicLinkPipe>::decode(edit.after);
            if (!pipe || pipe->uuid != edit.uuid ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pipe->node_uuid_from, node_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pipe->node_uuid_to, node_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pipe->material_uuid, material_kinds, error))
                return LocalStructureCheck::NotApplicable;
        }
        else if (edit.kind == QStringLiteral("hydraulic.pump"))
        {
            const std::optional<HydraulicLinkPump> pump = ModelCodec<HydraulicLinkPump>::decode(edit.after);
            if (!pump || pump->uuid != edit.uuid ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pump->node_uuid_from, node_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pump->node_uuid_to, node_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pump->head_curve_uuid, pump_head_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pump->speed_pattern_uuid, pattern_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pump->efficiency_curve_uuid, pump_efficiency_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 pump->price_pattern_uuid, pattern_kinds, error))
                return LocalStructureCheck::NotApplicable;
        }
        else if (edit.kind == QStringLiteral("hydraulic.valve"))
        {
            const std::optional<HydraulicLinkValve> valve = ModelCodec<HydraulicLinkValve>::decode(edit.after);
            if (!valve || valve->uuid != edit.uuid ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 valve->node_uuid_from, node_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 valve->node_uuid_to, node_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 valve->head_loss_curve_uuid, valve_headloss_kinds, error) ||
                !localTargetType(backend, database, revision_uuid, edit.network_uuid,
                                 valve->characteristic_curve_uuid, valve_characteristic_kinds, error))
                return LocalStructureCheck::NotApplicable;
        }
        else
        {
            const std::optional<HydraulicDemandPoint> point = ModelCodec<HydraulicDemandPoint>::decode(edit.after);
            if (!point || !localDemandPointConstraints(backend, database, revision_uuid,
                                                        edit.network_uuid, *point, error))
                return LocalStructureCheck::NotApplicable;
        }
    }

    // Previously indexed demand points attached to a changed pipe must still
    // be valid in the *final* transactional state, even when they weren't part
    // of this group. This closes the endpoint/assigned-junction dependency.
    QSqlQuery dependents(database);
    if (!dependents.prepare(QStringLiteral(
            "SELECT DISTINCT r.source_uuid,r.network_uuid FROM hydraulic_network_references AS r "
            "WHERE r.revision_uuid=? AND r.target_uuid=? "
            "AND r.field LIKE 'demand_point%attachment.pipe_uuid'")))
    {
        assignError(error, dependents.lastError().text());
        return LocalStructureCheck::SqlError;
    }
    for (const QUuid &pipe_uuid : changed_pipes)
    {
        dependents.bindValue(0, backend.bindUuid(revision_uuid));
        dependents.bindValue(1, backend.bindUuid(pipe_uuid));
        if (!dependents.exec())
        {
            assignError(error, dependents.lastError().text());
            return LocalStructureCheck::SqlError;
        }
        while (dependents.next())
        {
            const QUuid point_uuid = backend.readUuid(dependents.value(0));
            const QUuid network_uuid = backend.readUuid(dependents.value(1));
            QString kind;
            QByteArray content;
            if (!locallyOwnedObject(backend, database, revision_uuid, network_uuid, point_uuid,
                                    kind, content, error) || kind != QStringLiteral("hydraulic.demand_point"))
                return LocalStructureCheck::NotApplicable;
            const std::optional<HydraulicDemandPoint> point = ModelCodec<HydraulicDemandPoint>::decode(content);
            if (!point || point->uuid != point_uuid ||
                !localDemandPointConstraints(backend, database, revision_uuid, network_uuid, *point, error))
                return LocalStructureCheck::NotApplicable;
        }
        dependents.finish();
    }

    // No broad index rebuild: update only the touched source UUIDs. An SQL
    // failure aborts the whole transaction via the caller's rollback path.
    for (const LocalStructureEdit &edit : edits)
    {
        std::optional<QVector<HydraulicReference>> refs;
        if (edit.kind == QStringLiteral("hydraulic.pipe"))
            refs = outgoingReferences(edit.after, edit.uuid, &NetworkHydraulic::links_pipes);
        else if (edit.kind == QStringLiteral("hydraulic.pump"))
            refs = outgoingReferences(edit.after, edit.uuid, &NetworkHydraulic::links_pumps);
        else if (edit.kind == QStringLiteral("hydraulic.valve"))
            refs = outgoingReferences(edit.after, edit.uuid, &NetworkHydraulic::links_valves);
        else
            refs = outgoingReferences(edit.after, edit.uuid, &NetworkHydraulic::demand_points);
        if (!refs || !updateLocalReferenceSource(backend, database, revision_uuid,
                                                  edit.network_uuid, edit.uuid, *refs, error))
            return LocalStructureCheck::SqlError;
    }
    return LocalStructureCheck::Accepted;
}

// Called with a BEGIN IMMEDIATE transaction already active, after the final
// object state has been written. The caller rolls back *all* object changes and
// journal records when this validation fails.
bool Database::validateTouchedHydraulicNetworks(const QUuid &revision_uuid,
                                                 const QSet<QUuid> &touched_uuids,
                                                 QString *error) const
{
    if (touched_uuids.isEmpty())
        return true;

    // A lookup by (revision,entity) finds the owning root in O(log N),
    // without scanning and deserializing every root in the project.
    QSqlQuery owner(this->database_);
    if (!owner.prepare(QStringLiteral(
            "SELECT network_uuid FROM hydraulic_network_members "
            "WHERE revision_uuid=? AND entity_uuid=?")))
    {
        assignError(error, owner.lastError().text());
        return false;
    }
    QSqlQuery touched_row(this->database_);
    if (!touched_row.prepare(QStringLiteral(
            "SELECT kind,content,deleted FROM revision_objects "
            "WHERE revision_uuid=? AND object_uuid=?")))
    {
        assignError(error, touched_row.lastError().text());
        return false;
    }
    QHash<QUuid, QSet<QUuid>> changed_roots;
    QSet<QUuid> affected;
    for (const QUuid &uuid : touched_uuids)
    {
        touched_row.bindValue(0, this->backend_->bindUuid(revision_uuid));
        touched_row.bindValue(1, this->backend_->bindUuid(uuid));
        if (!touched_row.exec())
        {
            assignError(error, touched_row.lastError().text());
            return false;
        }
        if (touched_row.next() &&
            touched_row.value(0).toString() == QStringLiteral("hydraulic.network"))
        {
            QSet<QUuid> new_members;
            if (!touched_row.value(2).toBool())
            {
                const std::optional<NetworkIndex> index =
                    decodeIndex(touched_row.value(1).toByteArray(), error);
                if (!index || index->metadata.uuid != uuid)
                {
                    if (index)
                        assignError(error, QStringLiteral("Corrupted hydraulic network identity"));
                    return false;
                }
                new_members = referencedIds(*index);
            }
            changed_roots.insert(uuid, new_members);
            affected.insert(uuid);
        }
        touched_row.finish();
        owner.bindValue(0, this->backend_->bindUuid(revision_uuid));
        owner.bindValue(1, this->backend_->bindUuid(uuid));
        if (!owner.exec())
        {
            assignError(error, owner.lastError().text());
            return false;
        }
        if (owner.next())
            affected.insert(this->backend_->readUuid(owner.value(0)));
        owner.finish();
    }

    // A root-index rewrite may detach entities only when they are tombstoned
    // by the same transaction. The previously committed membership table is
    // authoritative for the old root index and survives failed edits.
    QSqlQuery old_members(this->database_);
    if (!old_members.prepare(QStringLiteral(
            "SELECT entity_uuid FROM hydraulic_network_members "
            "WHERE revision_uuid=? AND network_uuid=?")))
    {
        assignError(error, old_members.lastError().text());
        return false;
    }
    QSqlQuery entity(this->database_);
    if (!entity.prepare(QStringLiteral(
            "SELECT deleted FROM revision_objects WHERE revision_uuid=? AND object_uuid=?")))
    {
        assignError(error, entity.lastError().text());
        return false;
    }
    for (QHash<QUuid, QSet<QUuid>>::const_iterator root = changed_roots.cbegin();
         root != changed_roots.cend(); ++root)
    {
        old_members.bindValue(0, this->backend_->bindUuid(revision_uuid));
        old_members.bindValue(1, this->backend_->bindUuid(root.key()));
        if (!old_members.exec())
        {
            assignError(error, old_members.lastError().text());
            return false;
        }
        while (old_members.next())
        {
            const QUuid member = this->backend_->readUuid(old_members.value(0));
            if (root.value().contains(member))
                continue;
            // Atomic transfer between two changed roots is allowed; the
            // final membership uniqueness constraint remains authoritative.
            bool transferred = false;
            for (QHash<QUuid, QSet<QUuid>>::const_iterator other = changed_roots.cbegin();
                 other != changed_roots.cend(); ++other)
            {
                if (other.key() != root.key() && other.value().contains(member))
                {
                    transferred = true;
                    break;
                }
            }
            if (transferred)
                continue;
            entity.bindValue(0, this->backend_->bindUuid(revision_uuid));
            entity.bindValue(1, this->backend_->bindUuid(member));
            if (!entity.exec())
            {
                assignError(error, entity.lastError().text());
                return false;
            }
            if (entity.next() && !entity.value(0).toBool())
            {
                assignError(error, QStringLiteral(
                    "Cannot remove an entity from the network index without deleting it"));
                return false;
            }
            entity.finish();
        }
        old_members.finish();
    }

    // The unchanged parts of the model were validated when last committed.
    // Existing objects whose validation-sensitive fields did not change cannot
    // invalidate those constraints, even when multiple such edits are grouped.
    // All touched IDs must belong to an indexed network; the conservative
    // full validator handles any root/structural edit or unknown object kind.
    // Past a small batch, one full-network pass is cheaper than 2 SQL reads
    // and a typed mini-validator per touched entity.
    constexpr qsizetype max_incremental_entities = 32;
    if (changed_roots.isEmpty() && !affected.isEmpty() &&
        touched_uuids.size() <= max_incremental_entities)
    {
        bool all_reference_neutral = true;
        for (const QUuid &entity_uuid : touched_uuids)
        {
            owner.bindValue(0, this->backend_->bindUuid(revision_uuid));
            owner.bindValue(1, this->backend_->bindUuid(entity_uuid));
            if (!owner.exec())
            {
                assignError(error, owner.lastError().text());
                return false;
            }
            const bool indexed = owner.next();
            owner.finish();
            if (!indexed || !canSkipHydraulicRevalidation(revision_uuid, entity_uuid, error))
            {
                all_reference_neutral = false;
                break;
            }
        }
        if (all_reference_neutral)
            return true;
        const LocalStructureCheck local = checkLocalPipeAndAttachments(
            *this->backend_, this->database_, revision_uuid, touched_uuids, error);
        if (local == LocalStructureCheck::Accepted)
            return true;
        if (local == LocalStructureCheck::SqlError)
            return false;
    }

    // Validate the final transactional state, not transient intermediate edits.
    for (const QUuid &network_uuid : affected)
    {
        if (changed_roots.contains(network_uuid))
        {
            QSqlQuery state(this->database_);
            if (!state.prepare(QStringLiteral(
                    "SELECT deleted FROM revision_objects WHERE revision_uuid=? AND object_uuid=?")))
            {
                assignError(error, state.lastError().text());
                return false;
            }
            state.addBindValue(this->backend_->bindUuid(revision_uuid));
            state.addBindValue(this->backend_->bindUuid(network_uuid));
            if (!state.exec() || !state.next())
            {
                assignError(error, QStringLiteral("Changed network root missing"));
                return false;
            }
            if (state.value(0).toBool())
                continue;
        }
        const std::optional<NetworkHydraulic> network =
            loadHydraulicNetworkUnchecked(revision_uuid, network_uuid, error);
        if (!network)
            return false;
        const ModelValidationReport report = validateHydraulicNetwork(*network);
        if (!report.valid())
        {
            assignError(error, QStringLiteral("Hydraulic network %1: %2")
                .arg(network_uuid.toString(QUuid::WithoutBraces), report.summary()));
            return false;
        }
        if (!replaceHydraulicReferences(revision_uuid, *network, error))
            return false;
    }

    // Delete references for roots removed in this transaction as well.
    QSqlQuery delete_references(this->database_);
    if (!delete_references.prepare(QStringLiteral(
            "DELETE FROM hydraulic_network_references WHERE revision_uuid=? AND network_uuid=?")))
    {
        assignError(error, delete_references.lastError().text());
        return false;
    }
    for (QHash<QUuid, QSet<QUuid>>::const_iterator root = changed_roots.cbegin();
         root != changed_roots.cend(); ++root)
    {
        if (!root.value().isEmpty())
            continue;
        QSqlQuery state(this->database_);
        if (!state.prepare(QStringLiteral(
                "SELECT deleted FROM revision_objects WHERE revision_uuid=? AND object_uuid=?")))
        {
            assignError(error, state.lastError().text());
            return false;
        }
        state.addBindValue(this->backend_->bindUuid(revision_uuid));
        state.addBindValue(this->backend_->bindUuid(root.key()));
        if (!state.exec() || !state.next())
        {
            assignError(error, QStringLiteral("Changed network root missing"));
            return false;
        }
        if (state.value(0).toBool())
        {
            delete_references.bindValue(0, this->backend_->bindUuid(revision_uuid));
            delete_references.bindValue(1, this->backend_->bindUuid(root.key()));
            if (!delete_references.exec())
            {
                assignError(error, delete_references.lastError().text());
                return false;
            }
        }
    }

    // Clear all changed roots first, so moving an entity between two roots
    // in one atomic group has no artificial transient UNIQUE violation.
    QSqlQuery remove(this->database_);
    if (!remove.prepare(QStringLiteral(
            "DELETE FROM hydraulic_network_members WHERE revision_uuid=? AND network_uuid=?")))
    {
        assignError(error, remove.lastError().text());
        return false;
    }
    for (QHash<QUuid, QSet<QUuid>>::const_iterator root = changed_roots.cbegin();
         root != changed_roots.cend(); ++root)
    {
        remove.bindValue(0, this->backend_->bindUuid(revision_uuid));
        remove.bindValue(1, this->backend_->bindUuid(root.key()));
        if (!remove.exec())
        {
            assignError(error, remove.lastError().text());
            return false;
        }
    }
    for (QHash<QUuid, QSet<QUuid>>::const_iterator root = changed_roots.cbegin();
         root != changed_roots.cend(); ++root)
    {
        if (!replaceNetworkMembership(revision_uuid, root.key(), root.value(), error))
            return false;
    }
    return true;
}

} // namespace aowis::db
