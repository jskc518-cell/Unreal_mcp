// McpTool_ManagePCGGraph.cpp - manage_pcg_graph tool definition (~17 actions)
// Provides programmatic editing of UPCGGraph assets: nodes, pins, properties,
// user parameters, and asset-level operations (create graph, force regenerate).

#include "McpVersionCompatibility.h"
#include "MCP/McpToolDefinition.h"
#include "MCP/McpToolRegistry.h"
#include "MCP/McpSchemaBuilder.h"

class FMcpTool_ManagePCGGraph : public FMcpToolDefinition
{
public:
    FString GetName() const override { return TEXT("manage_pcg_graph"); }

    FString GetDescription() const override
    {
        return TEXT("Edit Procedural Content Generation (PCG) graphs: add/remove nodes, "
                    "connect pins, set node properties, manage user parameters, force "
                    "regeneration. Use for programmatic procedural content authoring "
                    "(residential blocks, foliage, spline scatterers).");
    }

    FString GetCategory() const override { return TEXT("core"); }

    TSharedPtr<FJsonObject> BuildInputSchema() const override
    {
        return FMcpSchemaBuilder()
            .StringEnum(TEXT("action"), {
                TEXT("list_node_types"),
                TEXT("get_graph_details"),
                TEXT("get_nodes"),
                TEXT("get_node_details"),
                TEXT("get_pin_details"),
                TEXT("create_node"),
                TEXT("delete_node"),
                TEXT("set_node_property"),
                TEXT("set_node_position"),
                TEXT("rename_node"),
                TEXT("connect_pins"),
                TEXT("disconnect_pins"),
                TEXT("break_pin_links"),
                TEXT("add_user_parameter"),
                TEXT("remove_user_parameter"),
                TEXT("create_pcg_graph"),
                TEXT("force_regenerate")
            }, TEXT("PCG graph action to perform."))
            .String(TEXT("assetPath"),
                TEXT("PCG graph asset path (e.g. /Game/PCG/Blocks/PCG_ResidentialBlock). "
                     "Required for all actions except list_node_types, create_pcg_graph, "
                     "force_regenerate."))
            .String(TEXT("path"),
                TEXT("Folder path for create_pcg_graph (e.g. /Game/PCG/Blocks)."))
            .String(TEXT("name"),
                TEXT("Asset name for create_pcg_graph; or new node title for rename_node."))
            .String(TEXT("nodeType"),
                TEXT("UPCGSettings subclass name or alias (SplineSampler, "
                     "StaticMeshSpawner, Difference, Transform, Filter, SelfPruning, "
                     "Subgraph, CreatePoints, CopyPoints, CreateSpline, GetActorData, "
                     "AttributeRemap, AttributeCast, or PCGSplineSamplerSettings etc)."))
            .String(TEXT("nodeId"),
                TEXT("Node identifier - object name, authored title, settings class name, "
                     "or the strings 'Input' / 'Output' for the graph I/O nodes."))
            .Number(TEXT("x"), TEXT("Node X position (graph editor units)."))
            .Number(TEXT("y"), TEXT("Node Y position (graph editor units)."))
            .FreeformObject(TEXT("properties"),
                TEXT("Optional property dictionary applied to UPCGSettings sub-object on create_node."))
            .String(TEXT("propertyName"),
                TEXT("Property name for set_node_property (resolved on UPCGSettings sub-object)."))
            .FreeformObject(TEXT("value"),
                TEXT("Property value for set_node_property / user parameter default."))
            .String(TEXT("fromNode"),
                TEXT("Source node identifier for connect_pins / disconnect_pins."))
            .String(TEXT("fromPin"),
                TEXT("Source pin label (case-insensitive). Omit on single-output nodes."))
            .String(TEXT("toNode"),
                TEXT("Target node identifier for connect_pins / disconnect_pins."))
            .String(TEXT("toPin"),
                TEXT("Target pin label (case-insensitive). Omit on single-input nodes."))
            .String(TEXT("pinName"),
                TEXT("Pin name for break_pin_links / get_pin_details."))
            .String(TEXT("parameterName"),
                TEXT("User parameter name for add_user_parameter / remove_user_parameter."))
            .StringEnum(TEXT("parameterType"), {
                TEXT("bool"),
                TEXT("int"),
                TEXT("int64"),
                TEXT("float"),
                TEXT("double"),
                TEXT("name"),
                TEXT("string"),
                TEXT("text")
            }, TEXT("User parameter primitive type."))
            .String(TEXT("actorName"),
                TEXT("Actor name (in current world) with UPCGComponent for force_regenerate."))
            .Required({TEXT("action")})
            .Build();
    }
};

MCP_REGISTER_TOOL(FMcpTool_ManagePCGGraph);
