#ifndef AOWIS_DB_MODEL_PERSISTENCE_H
#define AOWIS_DB_MODEL_PERSISTENCE_H

#include <aowis/db/database.h>
#include <aowis/db/model_codec.h>
#include <aowis/db/model_validation.h>

namespace aowis::db
{
// Typed edits to entities already referenced by a network root.
// Adding/removing entities must update the network index using saveHydraulicNetwork.
template<typename T> struct HydraulicObjectKind;
template<> struct HydraulicObjectKind<HydraulicPipeMaterial>
{
    static QString value() { return QStringLiteral("hydraulic.pipe_material"); }
};
template<> struct HydraulicObjectKind<HydraulicPatternTime>
{
    static QString value() { return QStringLiteral("hydraulic.pattern_time"); }
};
template<> struct HydraulicObjectKind<HydraulicCurveTankVolume>
{
    static QString value() { return QStringLiteral("hydraulic.curve_tank_volume"); }
};
template<> struct HydraulicObjectKind<HydraulicCurvePumpHead>
{
    static QString value() { return QStringLiteral("hydraulic.curve_pump_head"); }
};
template<> struct HydraulicObjectKind<HydraulicCurvePumpEfficiency>
{
    static QString value() { return QStringLiteral("hydraulic.curve_pump_efficiency"); }
};
template<> struct HydraulicObjectKind<HydraulicCurveValveHeadloss>
{
    static QString value() { return QStringLiteral("hydraulic.curve_valve_headloss"); }
};
template<> struct HydraulicObjectKind<HydraulicCurveValveCharacteristic>
{
    static QString value() { return QStringLiteral("hydraulic.curve_valve_characteristic"); }
};
template<> struct HydraulicObjectKind<HydraulicCurveGeneric>
{
    static QString value() { return QStringLiteral("hydraulic.curve_generic"); }
};
template<> struct HydraulicObjectKind<HydraulicNodeReservoir>
{
    static QString value() { return QStringLiteral("hydraulic.reservoir"); }
};
template<> struct HydraulicObjectKind<HydraulicNodeJunction>
{
    static QString value() { return QStringLiteral("hydraulic.junction"); }
};
template<> struct HydraulicObjectKind<HydraulicNodeTank>
{
    static QString value() { return QStringLiteral("hydraulic.tank"); }
};
template<> struct HydraulicObjectKind<HydraulicDemandPoint>
{
    static QString value() { return QStringLiteral("hydraulic.demand_point"); }
};
template<> struct HydraulicObjectKind<HydraulicLinkPipe>
{
    static QString value() { return QStringLiteral("hydraulic.pipe"); }
};
template<> struct HydraulicObjectKind<HydraulicLinkPump>
{
    static QString value() { return QStringLiteral("hydraulic.pump"); }
};
template<> struct HydraulicObjectKind<HydraulicLinkValve>
{
    static QString value() { return QStringLiteral("hydraulic.valve"); }
};
template<> struct HydraulicObjectKind<HydraulicControlSimple>
{
    static QString value() { return QStringLiteral("hydraulic.control_simple"); }
};
template<> struct HydraulicObjectKind<HydraulicControlRule>
{
    static QString value() { return QStringLiteral("hydraulic.control_rule"); }
};
template<> struct HydraulicObjectKind<HydraulicMapLabel>
{
    static QString value() { return QStringLiteral("hydraulic.map_label"); }
};

template<typename T>
bool putHydraulicEntity(Database &database, const OperationContext &actor,
                        const QUuid &branch_uuid, const T &entity,
                        qint64 expected_version, QString *error = nullptr)
{
    if (entity.uuid.isNull() || expected_version < 1)
    {
        if (error) *error = QStringLiteral("Entity UUID must be valid and an existing version is required");
        return false;
    }
    const QByteArray bytes = ModelCodec<T>::encode(entity);
    if (bytes.isEmpty())
    {
        if (error) *error = QStringLiteral("Entity serialization failed");
        return false;
    }
    return database.putObject(actor, branch_uuid, entity.uuid,
                              HydraulicObjectKind<T>::value(), bytes,
                              expected_version, error);
}

template<typename T>
std::optional<T> loadHydraulicEntity(const Database &database,
                                     const OperationContext &actor,
                                     const QUuid &revision_uuid,
                                     const QUuid &entity_uuid,
                                     QString *error = nullptr)
{
    const std::optional<ModelObject> stored = database.object(actor, revision_uuid, entity_uuid, error);
    if (!stored || stored->deleted || stored->kind != HydraulicObjectKind<T>::value())
    {
        if (error) *error = QStringLiteral("Entity missing, deleted, or has the wrong type");
        return std::nullopt;
    }
    std::optional<T> result = ModelCodec<T>::decode(stored->content, error);
    if (!result || result->uuid != entity_uuid)
    {
        if (error) *error = QStringLiteral("Entity payload UUID does not match its database key");
        return std::nullopt;
    }
    return result;
}

} // namespace aowis::db
#endif // AOWIS_DB_MODEL_PERSISTENCE_H
