#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::OperationContext;
using aowis::db::ModelValidationReport;
using aowis::db::validateHydraulicNetwork;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    Database db;
    REQUIRE(db.openSqlite(temp.filePath(QStringLiteral("references.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(actor, QStringLiteral("Reference validation"), &error);
    REQUIRE(project.has_value());

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeJunction junction;
    junction.uuid = createUuidV7();
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    network.nodes_junctions.append(junction);
    network.nodes_reservoirs.append(reservoir);
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = junction.uuid;
    network.links_pipes.append(pipe);
    HydraulicDemandPoint point;
    point.uuid = createUuidV7();
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = pipe.uuid;
    point.attachment.pipe_position = 0.75;
    point.attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = junction.uuid;
    network.demand_points.append(point);
    REQUIRE(validateHydraulicNetwork(network).valid());
    REQUIRE(db.saveHydraulicNetwork(actor, project->main_branch_uuid, 0, network, &error));
    const qint64 baseline = db.revision(actor, project->working_revision_uuid, &error)->generation;
    const qsizetype groups = db.objectGroupsForRevision(actor, project->working_revision_uuid, &error).size();
    const QByteArray original = db.object(actor, project->working_revision_uuid, pipe.uuid, &error)->content;

    // A dangling endpoint is rejected atomically: no partial change, no history event.
    NetworkHydraulic invalid = network;
    invalid.links_pipes[0].node_uuid_to = createUuidV7();
    const ModelValidationReport missing = validateHydraulicNetwork(invalid);
    REQUIRE(!missing.valid());
    REQUIRE(missing.summary().contains(QStringLiteral("node_uuid_to")));
    REQUIRE(!db.saveHydraulicNetwork(actor, project->main_branch_uuid, baseline, invalid, &error));
    REQUIRE(db.revision(actor, project->working_revision_uuid, &error)->generation == baseline);
    REQUIRE(db.objectGroupsForRevision(actor, project->working_revision_uuid, &error).size() == groups);
    REQUIRE(db.object(actor, project->working_revision_uuid, pipe.uuid, &error)->content == original);

    // Same UUID, wrong type: pipe reference points to a reservoir.
    invalid = network;
    invalid.demand_points[0].attachment.pipe_uuid = reservoir.uuid;
    REQUIRE(!validateHydraulicNetwork(invalid).valid());
    REQUIRE(!db.saveHydraulicNetwork(actor, project->main_branch_uuid, baseline, invalid, &error));

    // Assigned-junction semantics: only a junction at the attached pipe's end.
    invalid = network;
    HydraulicNodeJunction other;
    other.uuid = createUuidV7();
    invalid.nodes_junctions.append(other);
    invalid.demand_points[0].attachment.pipe_assigned_junction_uuid = other.uuid;
    REQUIRE(!validateHydraulicNetwork(invalid).valid());
    invalid = network;
    invalid.demand_points[0].attachment.pipe_position = 1.1;
    REQUIRE(!validateHydraulicNetwork(invalid).valid());

    // Pattern, material, MSX and control references use different namespaces.
    invalid = network;
    invalid.links_pipes[0].material_uuid = junction.uuid;
    REQUIRE(!validateHydraulicNetwork(invalid).valid());
    invalid = network;
    HydraulicControlSimple control;
    control.uuid = createUuidV7();
    control.link_uuid = reservoir.uuid;
    invalid.controls_simple.append(control);
    REQUIRE(!validateHydraulicNetwork(invalid).valid());
    invalid = network;
    MultiSpeciesSpecies species;
    species.uuid = createUuidV7();
    invalid.multi_species.species.append(species);
    MultiSpeciesPipeInitialQuality quality;
    quality.pipe_uuid = junction.uuid;
    quality.species_uuid = species.uuid;
    invalid.multi_species.initial_quality_pipes.append(quality);
    REQUIRE(!validateHydraulicNetwork(invalid).valid());

    invalid = network;
    HydraulicDemand extra_demand;
    extra_demand.pattern_uuid = createUuidV7();
    invalid.nodes_junctions[0].demands.append(extra_demand);
    REQUIRE(!validateHydraulicNetwork(invalid).valid());
    invalid = network;
    invalid.options_report.selection_links.mode = HydraulicSimulationReportSelectionMode::Selected;
    invalid.options_report.selection_links.uuids.append(junction.uuid);
    REQUIRE(!validateHydraulicNetwork(invalid).valid());
    invalid = network;
    MultiSpeciesNodeSource source;
    source.node_uuid = junction.uuid;
    source.species_uuid = createUuidV7();
    invalid.multi_species.sources.append(source);
    REQUIRE(!validateHydraulicNetwork(invalid).valid());

    // Clearing an endpoint referenced by an AssignedJunction demand point is
    // invalid even in a draft: the assignment would no longer be an endpoint.
    NetworkHydraulic disconnected_assignment = network;
    disconnected_assignment.links_pipes[0].node_uuid_to = QUuid();
    const ModelValidationReport disconnected_report = validateHydraulicNetwork(disconnected_assignment);
    REQUIRE(!disconnected_report.valid());
    REQUIRE(disconnected_report.summary().contains(QStringLiteral("attachment.pipe_assigned_junction_uuid")));

    // Editing incomplete drafts must remain possible. Clear the *other*
    // endpoint, preserving the junction explicitly assigned to the demand
    // point. A null endpoint is incomplete rather than a dangling reference.
    NetworkHydraulic draft = network;
    draft.links_pipes[0].node_uuid_from = QUuid();
    const ModelValidationReport draft_report = validateHydraulicNetwork(draft);
    if (!draft_report.valid())
        error = draft_report.summary();
    REQUIRE(draft_report.valid());
    REQUIRE(db.saveHydraulicNetwork(actor, project->main_branch_uuid, baseline, draft, &error));
    const qint64 after = db.revision(actor, project->working_revision_uuid, &error)->generation;
    REQUIRE(after > baseline);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    // Finalization keeps the original model state immutable and verifiable.
    REQUIRE(db.finalize(actor, project->main_branch_uuid, after, QStringLiteral("Draft"), {}, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
