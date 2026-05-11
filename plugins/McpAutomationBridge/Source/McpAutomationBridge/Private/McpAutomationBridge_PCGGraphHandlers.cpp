// =============================================================================
// McpAutomationBridge_PCGGraphHandlers.cpp
// =============================================================================
// PCG (Procedural Content Generation) graph manipulation handlers.
//
// Handles: manage_pcg_graph (dispatcher with ~17 sub-actions).
//
// Sub-actions:
//   Inspection:
//     - list_node_types
//     - get_graph_details
//     - get_nodes
//     - get_node_details
//     - get_pin_details
//   Mutation (nodes):
//     - create_node
//     - delete_node
//     - set_node_property
//     - set_node_position
//     - rename_node
//   Mutation (edges):
//     - connect_pins
//     - disconnect_pins
//     - break_pin_links
//   Mutation (graph metadata):
//     - add_user_parameter
//     - remove_user_parameter
//   Asset-level:
//     - create_pcg_graph
//     - force_regenerate
//
// PCG ARCHITECTURE NOTE: PCG graphs use a bespoke runtime graph object
// (UPCGGraph) instead of UEdGraph. Each node's logic lives in an instanced
// UPCGSettings sub-object (Node->SettingsInterface / Node->GetSettings()).
// Property edits target the Settings sub-object, not the Node itself.
// =============================================================================

#include "McpVersionCompatibility.h"  // MUST BE FIRST
#include "McpHandlerUtils.h"
#include "McpAutomationBridgeGlobals.h"
#include "McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR && MCP_HAS_PCG_PLUGIN

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "PCGSettings.h"
#include "PCGComponent.h"
#include "PCGSubsystem.h"
#include "PCGCommon.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "StructUtils/PropertyBag.h"

#endif // WITH_EDITOR && MCP_HAS_PCG_PLUGIN

#if WITH_EDITOR && MCP_HAS_PCG_PLUGIN

namespace McpPcgHelpers
{
    // -----------------------------------------------------------------------
    // Alias map: friendly node-type names → UPCGSettings subclass name.
    // Falls through to UClass lookup if not matched.
    // -----------------------------------------------------------------------
    static UClass* ResolvePCGSettingsClass(const FString& InNodeType)
    {
        if (InNodeType.IsEmpty()) return nullptr;

        // Friendly aliases - most common nodes
        static const TMap<FString, FString> Aliases = {
            { TEXT("SplineSampler"),       TEXT("PCGSplineSamplerSettings") },
            { TEXT("StaticMeshSpawner"),   TEXT("PCGStaticMeshSpawnerSettings") },
            { TEXT("Difference"),          TEXT("PCGDifferenceSettings") },
            { TEXT("Transform"),           TEXT("PCGTransformPointsSettings") },
            { TEXT("TransformPoints"),     TEXT("PCGTransformPointsSettings") },
            { TEXT("Filter"),              TEXT("PCGAttributeFilterSettings") },
            { TEXT("AttributeFilter"),     TEXT("PCGAttributeFilterSettings") },
            { TEXT("SelfPruning"),         TEXT("PCGSelfPruningSettings") },
            { TEXT("Subgraph"),            TEXT("PCGSubgraphSettings") },
            { TEXT("CreatePoints"),        TEXT("PCGCreatePointsSettings") },
            { TEXT("CopyPoints"),          TEXT("PCGCopyPointsSettings") },
            { TEXT("CreateSpline"),        TEXT("PCGCreateSplineSettings") },
            { TEXT("GetActorData"),        TEXT("PCGGetActorDataSettings") },
            { TEXT("AttributeRemap"),      TEXT("PCGAttributeRemapSettings") },
            { TEXT("AttributeCast"),       TEXT("PCGAttributeCastSettings") }
        };

        FString CanonicalName = InNodeType;
        if (const FString* Alias = Aliases.Find(InNodeType))
        {
            CanonicalName = *Alias;
        }

        // Try direct UClass lookup (StaticFindObject)
        UClass* Found = FindFirstObject<UClass>(*CanonicalName, EFindFirstObjectOptions::None);
        if (Found && Found->IsChildOf(UPCGSettings::StaticClass()) &&
            !Found->HasAnyClassFlags(CLASS_Abstract))
        {
            return Found;
        }

        // Try with U prefix
        FString WithPrefix = FString::Printf(TEXT("U%s"), *CanonicalName);
        Found = FindFirstObject<UClass>(*WithPrefix, EFindFirstObjectOptions::None);
        if (Found && Found->IsChildOf(UPCGSettings::StaticClass()) &&
            !Found->HasAnyClassFlags(CLASS_Abstract))
        {
            return Found;
        }

        // Final fallback: iterate all UPCGSettings subclasses
        for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
        {
            UClass* Cls = *ClassIt;
            if (!Cls->IsChildOf(UPCGSettings::StaticClass()) ||
                Cls->HasAnyClassFlags(CLASS_Abstract))
            {
                continue;
            }
            if (Cls->GetName().Equals(CanonicalName, ESearchCase::IgnoreCase) ||
                Cls->GetName().Equals(InNodeType, ESearchCase::IgnoreCase))
            {
                return Cls;
            }
        }

        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Resolve a node by GUID / name / authored title / class name.
    // -----------------------------------------------------------------------
    static UPCGNode* FindNodeByIdentifier(UPCGGraph* Graph, const FString& Identifier)
    {
        if (!Graph || Identifier.IsEmpty()) return nullptr;

        const FString Needle = Identifier.TrimStartAndEnd();

        // Check input/output specials first
        if (Needle.Equals(TEXT("Input"), ESearchCase::IgnoreCase) ||
            Needle.Equals(TEXT("InputNode"), ESearchCase::IgnoreCase))
        {
            return Graph->GetInputNode();
        }
        if (Needle.Equals(TEXT("Output"), ESearchCase::IgnoreCase) ||
            Needle.Equals(TEXT("OutputNode"), ESearchCase::IgnoreCase))
        {
            return Graph->GetOutputNode();
        }

        // Walk all nodes including input/output
        TArray<UPCGNode*> AllNodes;
        if (UPCGNode* In = Graph->GetInputNode()) AllNodes.Add(In);
        if (UPCGNode* Out = Graph->GetOutputNode()) AllNodes.Add(Out);
        for (UPCGNode* N : Graph->GetNodes())
        {
            if (N) AllNodes.Add(N);
        }

        for (UPCGNode* Node : AllNodes)
        {
            if (!Node) continue;

            // Match by NodeGuid (UObject name, since UPCGNode doesn't expose explicit Guid)
            if (Node->GetName() == Needle) return Node;
            if (Node->GetPathName() == Needle) return Node;

            // Match by authored title FName
            if (Node->GetAuthoredTitleName().ToString() == Needle) return Node;

            // Match by display title text
            const FText TitleText = Node->GetNodeTitle(EPCGNodeTitleType::FullTitle);
            if (TitleText.ToString() == Needle) return Node;

            const FText ListTitle = Node->GetNodeTitle(EPCGNodeTitleType::ListView);
            if (ListTitle.ToString() == Needle) return Node;

            // Match by settings class name
            if (UPCGSettings* Settings = Node->GetSettings())
            {
                if (Settings->GetClass()->GetName() == Needle) return Node;
            }
        }

        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Serialize a UPCGNode to JSON.
    // -----------------------------------------------------------------------
    static TSharedPtr<FJsonObject> NodeToJson(UPCGNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Node) return Obj;

        Obj->SetStringField(TEXT("nodeId"), Node->GetName());
        Obj->SetStringField(TEXT("nodeName"), Node->GetAuthoredTitleName().ToString());
        Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());

        if (UPCGSettings* Settings = Node->GetSettings())
        {
            Obj->SetStringField(TEXT("settingsClass"), Settings->GetClass()->GetName());
        }

#if WITH_EDITORONLY_DATA
        Obj->SetNumberField(TEXT("x"), Node->PositionX);
        Obj->SetNumberField(TEXT("y"), Node->PositionY);
#endif

        // Pin summaries
        TArray<TSharedPtr<FJsonValue>> InputPinsJson;
        for (UPCGPin* Pin : Node->GetInputPins())
        {
            if (!Pin) continue;
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("name"), Pin->Properties.Label.ToString());
            PinObj->SetNumberField(TEXT("edgeCount"), Pin->EdgeCount());
            InputPinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        Obj->SetArrayField(TEXT("inputPins"), InputPinsJson);

        TArray<TSharedPtr<FJsonValue>> OutputPinsJson;
        for (UPCGPin* Pin : Node->GetOutputPins())
        {
            if (!Pin) continue;
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("name"), Pin->Properties.Label.ToString());
            PinObj->SetNumberField(TEXT("edgeCount"), Pin->EdgeCount());
            OutputPinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        Obj->SetArrayField(TEXT("outputPins"), OutputPinsJson);

        return Obj;
    }

    // -----------------------------------------------------------------------
    // Find a pin on a node by name (case-insensitive, exact match preferred).
    // -----------------------------------------------------------------------
    static UPCGPin* FindPinByName(UPCGNode* Node, const FString& PinName, bool bOutput)
    {
        if (!Node) return nullptr;

        const FName Needle(*PinName);

        // Exact match
        UPCGPin* Exact = bOutput ? Node->GetOutputPin(Needle) : Node->GetInputPin(Needle);
        if (Exact) return Exact;

        // Lenient: case-insensitive string compare
        const TArray<TObjectPtr<UPCGPin>>& Pins = bOutput ? Node->GetOutputPins() : Node->GetInputPins();
        for (UPCGPin* Pin : Pins)
        {
            if (!Pin) continue;
            if (Pin->Properties.Label.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }

        // If only one pin exists and name was empty / generic, return it (convenience).
        if (Pins.Num() == 1 && (PinName.IsEmpty() || PinName.Equals(TEXT("In"), ESearchCase::IgnoreCase) ||
                                 PinName.Equals(TEXT("Out"), ESearchCase::IgnoreCase)))
        {
            return Pins[0];
        }

        return nullptr;
    }
}

#endif // WITH_EDITOR && MCP_HAS_PCG_PLUGIN

// =============================================================================
// Handler: manage_pcg_graph
// =============================================================================

bool UMcpAutomationBridgeSubsystem::HandlePCGGraphAction(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> Socket)
{
    if (Action != TEXT("manage_pcg_graph"))
    {
        return false;
    }

#if !WITH_EDITOR || !MCP_HAS_PCG_PLUGIN
    SendAutomationError(Socket, RequestId,
        TEXT("PCG editing requires editor build with PCG plugin enabled."),
        TEXT("EDITOR_ONLY"));
    return true;
#else

    using namespace McpPcgHelpers;

    if (!Payload.IsValid())
    {
        SendAutomationError(Socket, RequestId, TEXT("Missing payload."), TEXT("INVALID_PAYLOAD"));
        return true;
    }

    FString SubAction;
    if (!Payload->TryGetStringField(TEXT("subAction"), SubAction) || SubAction.IsEmpty())
    {
        SendAutomationError(Socket, RequestId,
            TEXT("Missing 'subAction' for manage_pcg_graph."),
            TEXT("INVALID_ARGUMENT"));
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: list_node_types (global, no asset path needed)
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("list_node_types"))
    {
        TArray<TSharedPtr<FJsonValue>> NodeTypes;
        for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
        {
            UClass* Cls = *ClassIt;
            if (!Cls->IsChildOf(UPCGSettings::StaticClass()) ||
                Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                continue;
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("className"), Cls->GetName());
            Entry->SetStringField(TEXT("displayName"), Cls->GetDisplayNameText().ToString());
            NodeTypes.Add(MakeShared<FJsonValueObject>(Entry));
        }

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        Result->SetArrayField(TEXT("nodeTypes"), NodeTypes);
        Result->SetNumberField(TEXT("count"), NodeTypes.Num());
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("%d PCG node types available."), NodeTypes.Num()), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: create_pcg_graph (creates a new asset)
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("create_pcg_graph"))
    {
        FString FolderPath = SanitizeProjectRelativePath(
            McpHandlerUtils::GetOptionalString(Payload, TEXT("path")));
        FString AssetName = McpHandlerUtils::GetOptionalString(Payload, TEXT("name"));

        if (FolderPath.IsEmpty() || AssetName.IsEmpty())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("create_pcg_graph requires 'path' and 'name'."),
                TEXT("INVALID_ARGUMENT"));
            return true;
        }

        // Use AssetTools generic CreateAsset (no factory needed for simple UPCGGraph)
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        UObject* NewAsset = AssetToolsModule.Get().CreateAsset(
            AssetName, FolderPath, UPCGGraph::StaticClass(), nullptr);

        if (!NewAsset)
        {
            SendAutomationError(Socket, RequestId,
                TEXT("AssetTools::CreateAsset returned null for UPCGGraph."),
                TEXT("CREATE_FAILED"));
            return true;
        }

        SaveLoadedAssetThrottled(NewAsset, -1.0, true);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, NewAsset);
        Result->SetStringField(TEXT("assetPath"), NewAsset->GetPathName());
        Result->SetStringField(TEXT("name"), AssetName);
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("Created PCG graph '%s'."), *AssetName), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: force_regenerate (drives a PCGComponent on a named actor)
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("force_regenerate"))
    {
        FString ActorName = McpHandlerUtils::GetOptionalString(Payload, TEXT("actorName"));
        if (ActorName.IsEmpty())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("force_regenerate requires 'actorName'."),
                TEXT("INVALID_ARGUMENT"));
            return true;
        }

        AActor* Actor = McpHandlerUtils::FindActorByName(ActorName, true);
        if (!Actor)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Actor '%s' not found."), *ActorName),
                TEXT("ACTOR_NOT_FOUND"));
            return true;
        }

        UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
        if (!PCGComp)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Actor '%s' has no UPCGComponent."), *ActorName),
                TEXT("COMPONENT_NOT_FOUND"));
            return true;
        }

        PCGComp->Generate(/*bForce*/ true);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Actor);
        Result->SetBoolField(TEXT("regenerated"), true);
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("Regeneration triggered on '%s'."), *ActorName), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // All remaining sub-actions require an assetPath to an existing UPCGGraph.
    // -----------------------------------------------------------------------
    FString AssetPath;
    if (!Payload->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
    {
        SendAutomationError(Socket, RequestId,
            TEXT("Missing 'assetPath' for manage_pcg_graph."),
            TEXT("INVALID_ARGUMENT"));
        return true;
    }

    const FString SanitizedPath = SanitizeProjectRelativePath(AssetPath);
    if (SanitizedPath.IsEmpty())
    {
        SendAutomationError(Socket, RequestId,
            FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath),
            TEXT("INVALID_PATH"));
        return true;
    }

    UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *SanitizedPath);
    if (!Graph)
    {
        SendAutomationError(Socket, RequestId,
            FString::Printf(TEXT("Could not load UPCGGraph at: %s"), *SanitizedPath),
            TEXT("ASSET_NOT_FOUND"));
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: get_graph_details
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("get_graph_details"))
    {
        int32 EdgeCount = 0;
        for (UPCGNode* N : Graph->GetNodes())
        {
            if (!N) continue;
            for (UPCGPin* Pin : N->GetOutputPins())
            {
                if (Pin) EdgeCount += Pin->EdgeCount();
            }
        }
        // Include I/O nodes' outbound edges
        if (UPCGNode* In = Graph->GetInputNode())
        {
            for (UPCGPin* Pin : In->GetOutputPins())
            {
                if (Pin) EdgeCount += Pin->EdgeCount();
            }
        }

        const FInstancedPropertyBag* UserParams = Graph->GetUserParametersStruct();
        const int32 UserParamCount = (UserParams && UserParams->IsValid())
            ? UserParams->GetNumPropertiesInBag() : 0;

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("name"), Graph->GetName());
        Result->SetStringField(TEXT("path"), Graph->GetPathName());
        Result->SetNumberField(TEXT("nodeCount"), Graph->GetNodes().Num());
        Result->SetNumberField(TEXT("edgeCount"), EdgeCount);
        Result->SetNumberField(TEXT("userParameterCount"), UserParamCount);

        if (UPCGNode* In = Graph->GetInputNode())
        {
            Result->SetObjectField(TEXT("inputNode"), NodeToJson(In));
        }
        if (UPCGNode* Out = Graph->GetOutputNode())
        {
            Result->SetObjectField(TEXT("outputNode"), NodeToJson(Out));
        }

        SendAutomationResponse(Socket, RequestId, true, TEXT("PCG graph details."), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: get_nodes
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("get_nodes"))
    {
        TArray<TSharedPtr<FJsonValue>> NodesJson;

        if (UPCGNode* In = Graph->GetInputNode())
        {
            TSharedPtr<FJsonObject> NodeObj = NodeToJson(In);
            NodeObj->SetBoolField(TEXT("isInput"), true);
            NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
        }
        if (UPCGNode* Out = Graph->GetOutputNode())
        {
            TSharedPtr<FJsonObject> NodeObj = NodeToJson(Out);
            NodeObj->SetBoolField(TEXT("isOutput"), true);
            NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
        }
        for (UPCGNode* Node : Graph->GetNodes())
        {
            if (Node)
            {
                NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
            }
        }

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetArrayField(TEXT("nodes"), NodesJson);
        Result->SetNumberField(TEXT("count"), NodesJson.Num());
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("%d PCG node(s) in graph."), NodesJson.Num()), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: get_node_details
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("get_node_details"))
    {
        FString NodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeId"));
        UPCGNode* Node = FindNodeByIdentifier(Graph, NodeId);
        if (!Node)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node not found: %s"), *NodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        TSharedPtr<FJsonObject> Result = NodeToJson(Node);
        McpHandlerUtils::AddVerification(Result, Graph);

        // Detailed pin info with connections
        TArray<TSharedPtr<FJsonValue>> InputPinsDetail;
        for (UPCGPin* Pin : Node->GetInputPins())
        {
            if (!Pin) continue;
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("name"), Pin->Properties.Label.ToString());
            PinObj->SetBoolField(TEXT("allowMultiData"), Pin->Properties.bAllowMultipleData);
            PinObj->SetBoolField(TEXT("allowMultiConn"), Pin->AllowsMultipleConnections());
            PinObj->SetBoolField(TEXT("advanced"), Pin->Properties.IsAdvancedPin());
            PinObj->SetBoolField(TEXT("required"), Pin->Properties.IsRequiredPin());
            InputPinsDetail.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        Result->SetArrayField(TEXT("inputPinsDetail"), InputPinsDetail);

        TArray<TSharedPtr<FJsonValue>> OutputPinsDetail;
        for (UPCGPin* Pin : Node->GetOutputPins())
        {
            if (!Pin) continue;
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("name"), Pin->Properties.Label.ToString());
            PinObj->SetBoolField(TEXT("allowMultiData"), Pin->Properties.bAllowMultipleData);
            PinObj->SetBoolField(TEXT("allowMultiConn"), Pin->AllowsMultipleConnections());
            PinObj->SetBoolField(TEXT("advanced"), Pin->Properties.IsAdvancedPin());
            OutputPinsDetail.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        Result->SetArrayField(TEXT("outputPinsDetail"), OutputPinsDetail);

        // Settings property names (lightweight - just names, not values)
        if (UPCGSettings* Settings = Node->GetSettings())
        {
            TArray<TSharedPtr<FJsonValue>> SettingsProps;
            for (TFieldIterator<FProperty> PropIt(Settings->GetClass()); PropIt; ++PropIt)
            {
                FProperty* P = *PropIt;
                if (!P || !P->HasAnyPropertyFlags(CPF_Edit)) continue;
                TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
                PropObj->SetStringField(TEXT("name"), P->GetName());
                PropObj->SetStringField(TEXT("type"), P->GetCPPType());
                SettingsProps.Add(MakeShared<FJsonValueObject>(PropObj));
            }
            Result->SetArrayField(TEXT("settingsProperties"), SettingsProps);
        }

        SendAutomationResponse(Socket, RequestId, true, TEXT("PCG node details."), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: get_pin_details
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("get_pin_details"))
    {
        FString NodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeId"));
        FString PinName = McpHandlerUtils::GetOptionalString(Payload, TEXT("pinName"));

        UPCGNode* Node = FindNodeByIdentifier(Graph, NodeId);
        if (!Node)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node not found: %s"), *NodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        // Try input first, then output
        UPCGPin* Pin = FindPinByName(Node, PinName, /*bOutput*/ false);
        bool bOutput = false;
        if (!Pin)
        {
            Pin = FindPinByName(Node, PinName, /*bOutput*/ true);
            bOutput = true;
        }

        if (!Pin)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Pin '%s' not found on node '%s'."), *PinName, *NodeId),
                TEXT("PIN_NOT_FOUND"));
            return true;
        }

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("name"), Pin->Properties.Label.ToString());
        Result->SetStringField(TEXT("direction"), bOutput ? TEXT("output") : TEXT("input"));
        Result->SetBoolField(TEXT("allowMultiData"), Pin->Properties.bAllowMultipleData);
        Result->SetBoolField(TEXT("allowMultiConn"), Pin->AllowsMultipleConnections());
        Result->SetBoolField(TEXT("advanced"), Pin->Properties.IsAdvancedPin());
        Result->SetBoolField(TEXT("required"), Pin->Properties.IsRequiredPin());
        Result->SetNumberField(TEXT("edgeCount"), Pin->EdgeCount());

        // Enumerate connections (other endpoints of each edge)
        TArray<TSharedPtr<FJsonValue>> Connections;
        for (UPCGEdge* Edge : Pin->Edges)
        {
            if (!Edge) continue;
            // UPCGEdge stores OutputPin and InputPin (TWeakObjectPtr in some versions)
            // We resolve the *other* end of the edge.
            // The edge connects this pin to one other pin; we cannot rely on field names
            // without including the header, so walk the other-direction pins on every node.
            // Simpler: enumerate every node's pin lists looking for an edge that matches.
            // (This is O(N*M) but graphs are small.)
            for (UPCGNode* OtherNode : Graph->GetNodes())
            {
                if (!OtherNode || OtherNode == Node) continue;
                const TArray<TObjectPtr<UPCGPin>>& OtherPins = bOutput
                    ? OtherNode->GetInputPins() : OtherNode->GetOutputPins();
                for (UPCGPin* OtherPin : OtherPins)
                {
                    if (!OtherPin) continue;
                    if (OtherPin->Edges.Contains(Edge))
                    {
                        TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
                        Conn->SetStringField(TEXT("nodeId"), OtherNode->GetName());
                        Conn->SetStringField(TEXT("pinName"), OtherPin->Properties.Label.ToString());
                        Connections.Add(MakeShared<FJsonValueObject>(Conn));
                    }
                }
            }
        }
        Result->SetArrayField(TEXT("connections"), Connections);

        SendAutomationResponse(Socket, RequestId, true, TEXT("PCG pin details."), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: create_node
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("create_node"))
    {
        FString NodeType = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeType"));
        if (NodeType.IsEmpty())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("create_node requires 'nodeType'."), TEXT("INVALID_ARGUMENT"));
            return true;
        }

        UClass* SettingsClass = ResolvePCGSettingsClass(NodeType);
        if (!SettingsClass)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Unknown PCG node type: %s. Try a UPCGSettings subclass name like 'PCGSplineSamplerSettings' or an alias like 'SplineSampler'."), *NodeType),
                TEXT("UNKNOWN_TYPE"));
            return true;
        }

        const double XPos = McpHandlerUtils::GetOptionalFloat(Payload, TEXT("x"), 0.0);
        const double YPos = McpHandlerUtils::GetOptionalFloat(Payload, TEXT("y"), 0.0);

        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_CreatePCGNode", "Create PCG Node"));
        Graph->Modify();

        UPCGSettings* DefaultSettings = nullptr;
        UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, DefaultSettings);
        if (!NewNode)
        {
            SendAutomationError(Socket, RequestId,
                TEXT("AddNodeOfType returned null."), TEXT("CREATE_FAILED"));
            return true;
        }

#if WITH_EDITORONLY_DATA
        NewNode->Modify();
        NewNode->PositionX = (int32)XPos;
        NewNode->PositionY = (int32)YPos;
#endif

        // Optional initial properties: { "PropName": value, ... }
        const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
        if (Payload->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj && (*PropertiesObj).IsValid() && DefaultSettings)
        {
            DefaultSettings->Modify();
            for (const auto& Kvp : (*PropertiesObj)->Values)
            {
                McpHandlerUtils::FPropertyResolveResult Res =
                    McpHandlerUtils::ResolveProperty(DefaultSettings, Kvp.Key);
                if (!Res.IsValid()) continue;
                FString PropError;
                ApplyJsonValueToProperty(Res.Container, Res.Property, Kvp.Value, PropError);
            }
        }

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("nodeId"), NewNode->GetName());
        Result->SetStringField(TEXT("settingsClass"), SettingsClass->GetName());
        Result->SetStringField(TEXT("nodeName"), NewNode->GetAuthoredTitleName().ToString());
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("Created PCG node '%s' of type '%s'."),
                *NewNode->GetName(), *SettingsClass->GetName()), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: delete_node
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("delete_node"))
    {
        FString NodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeId"));
        UPCGNode* Node = FindNodeByIdentifier(Graph, NodeId);
        if (!Node)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node not found: %s"), *NodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("Cannot delete the graph input/output node."),
                TEXT("PROTECTED_NODE"));
            return true;
        }

        const FString DeletedId = Node->GetName();
        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_DeletePCGNode", "Delete PCG Node"));
        Graph->Modify();
        Graph->RemoveNode(Node);

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("nodeId"), DeletedId);
        Result->SetBoolField(TEXT("removed"), true);
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("Deleted PCG node '%s'."), *DeletedId), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: set_node_property
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("set_node_property"))
    {
        FString NodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeId"));
        FString PropertyName = McpHandlerUtils::GetOptionalString(Payload, TEXT("propertyName"));

        if (NodeId.IsEmpty() || PropertyName.IsEmpty())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("set_node_property requires 'nodeId' and 'propertyName'."),
                TEXT("INVALID_ARGUMENT"));
            return true;
        }

        UPCGNode* Node = FindNodeByIdentifier(Graph, NodeId);
        if (!Node)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node not found: %s"), *NodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        UPCGSettings* Settings = Node->GetSettings();
        if (!Settings)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node '%s' has no Settings object."), *NodeId),
                TEXT("INVALID_STATE"));
            return true;
        }

        TSharedPtr<FJsonValue> ValueField = Payload->TryGetField(TEXT("value"));
        if (!ValueField.IsValid())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("set_node_property requires 'value'."),
                TEXT("INVALID_ARGUMENT"));
            return true;
        }

        McpHandlerUtils::FPropertyResolveResult Res =
            McpHandlerUtils::ResolveProperty(Settings, PropertyName);
        if (!Res.IsValid())
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Could not resolve property '%s' on '%s': %s"),
                    *PropertyName, *Settings->GetClass()->GetName(), *Res.Error),
                TEXT("PROPERTY_NOT_FOUND"));
            return true;
        }

        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_SetPCGNodeProperty", "Set PCG Node Property"));
        Settings->Modify();
        FString PropError;
        const bool bApplied = ApplyJsonValueToProperty(Res.Container, Res.Property, ValueField, PropError);
        if (!bApplied)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Failed to apply value: %s"), *PropError),
                TEXT("APPLY_FAILED"));
            return true;
        }

        // Notify settings changed so dynamic pins / dependencies update.
        // UPCGSettings makes PostEditChangeProperty protected, so use the public
        // no-arg PostEditChange() from UObject — it fires the same change cascade.
        Settings->PostEditChange();

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("nodeId"), Node->GetName());
        Result->SetStringField(TEXT("propertyName"), PropertyName);
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("Set %s.%s."), *Node->GetName(), *PropertyName), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: set_node_position
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("set_node_position"))
    {
        FString NodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeId"));
        UPCGNode* Node = FindNodeByIdentifier(Graph, NodeId);
        if (!Node)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node not found: %s"), *NodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        const double XPos = McpHandlerUtils::GetOptionalFloat(Payload, TEXT("x"), 0.0);
        const double YPos = McpHandlerUtils::GetOptionalFloat(Payload, TEXT("y"), 0.0);

        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_SetPCGNodePosition", "Set PCG Node Position"));
        Node->Modify();
#if WITH_EDITORONLY_DATA
        Node->SetNodePosition((int32)XPos, (int32)YPos);
#endif

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("nodeId"), Node->GetName());
        Result->SetNumberField(TEXT("x"), XPos);
        Result->SetNumberField(TEXT("y"), YPos);
        SendAutomationResponse(Socket, RequestId, true,
            TEXT("Node position updated."), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: rename_node
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("rename_node"))
    {
        FString NodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeId"));
        FString NewName = McpHandlerUtils::GetOptionalString(Payload, TEXT("name"));
        if (NodeId.IsEmpty() || NewName.IsEmpty())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("rename_node requires 'nodeId' and 'name'."),
                TEXT("INVALID_ARGUMENT"));
            return true;
        }

        UPCGNode* Node = FindNodeByIdentifier(Graph, NodeId);
        if (!Node)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node not found: %s"), *NodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        const FString OldName = Node->GetAuthoredTitleName().ToString();
        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_RenamePCGNode", "Rename PCG Node"));
        Node->Modify();
        Node->SetNodeTitle(FName(*NewName), /*bApplySanitization*/ true);

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("nodeId"), Node->GetName());
        Result->SetStringField(TEXT("oldName"), OldName);
        Result->SetStringField(TEXT("newName"), NewName);
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("Renamed '%s' -> '%s'."), *OldName, *NewName), Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: connect_pins
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("connect_pins"))
    {
        FString FromNodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("fromNode"));
        FString FromPinName = McpHandlerUtils::GetOptionalString(Payload, TEXT("fromPin"));
        FString ToNodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("toNode"));
        FString ToPinName = McpHandlerUtils::GetOptionalString(Payload, TEXT("toPin"));

        UPCGNode* FromNode = FindNodeByIdentifier(Graph, FromNodeId);
        UPCGNode* ToNode = FindNodeByIdentifier(Graph, ToNodeId);
        if (!FromNode || !ToNode)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Could not resolve nodes: from='%s' to='%s'."),
                    *FromNodeId, *ToNodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        // Default pin names if omitted: assume the single output / single input.
        UPCGPin* FromPin = FindPinByName(FromNode, FromPinName, /*bOutput*/ true);
        UPCGPin* ToPin = FindPinByName(ToNode, ToPinName, /*bOutput*/ false);
        if (!FromPin || !ToPin)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Could not resolve pins: fromPin='%s' toPin='%s'."),
                    *FromPinName, *ToPinName),
                TEXT("PIN_NOT_FOUND"));
            return true;
        }

        const FName FromPinLabel = FromPin->Properties.Label;
        const FName ToPinLabel = ToPin->Properties.Label;

        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_ConnectPCGPins", "Connect PCG Pins"));
        Graph->Modify();
        UPCGNode* ChainResult = Graph->AddEdge(FromNode, FromPinLabel, ToNode, ToPinLabel);
        const bool bConnected = (ChainResult != nullptr);

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("fromNode"), FromNode->GetName());
        Result->SetStringField(TEXT("fromPin"), FromPinLabel.ToString());
        Result->SetStringField(TEXT("toNode"), ToNode->GetName());
        Result->SetStringField(TEXT("toPin"), ToPinLabel.ToString());
        Result->SetBoolField(TEXT("connected"), bConnected);

        if (bConnected)
        {
            SendAutomationResponse(Socket, RequestId, true, TEXT("PCG pins connected."), Result);
        }
        else
        {
            SendAutomationResponse(Socket, RequestId, false,
                TEXT("AddEdge returned null; types may be incompatible."), Result,
                TEXT("CONNECTION_FAILED"));
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: disconnect_pins
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("disconnect_pins"))
    {
        FString FromNodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("fromNode"));
        FString FromPinName = McpHandlerUtils::GetOptionalString(Payload, TEXT("fromPin"));
        FString ToNodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("toNode"));
        FString ToPinName = McpHandlerUtils::GetOptionalString(Payload, TEXT("toPin"));

        UPCGNode* FromNode = FindNodeByIdentifier(Graph, FromNodeId);
        UPCGNode* ToNode = FindNodeByIdentifier(Graph, ToNodeId);
        if (!FromNode || !ToNode)
        {
            SendAutomationError(Socket, RequestId,
                TEXT("Could not resolve nodes."), TEXT("NODE_NOT_FOUND"));
            return true;
        }

        UPCGPin* FromPin = FindPinByName(FromNode, FromPinName, /*bOutput*/ true);
        UPCGPin* ToPin = FindPinByName(ToNode, ToPinName, /*bOutput*/ false);
        if (!FromPin || !ToPin)
        {
            SendAutomationError(Socket, RequestId,
                TEXT("Could not resolve pins."), TEXT("PIN_NOT_FOUND"));
            return true;
        }

        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_DisconnectPCGPins", "Disconnect PCG Pins"));
        Graph->Modify();
        const bool bRemoved = Graph->RemoveEdge(FromNode,
            FromPin->Properties.Label, ToNode, ToPin->Properties.Label);

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetBoolField(TEXT("disconnected"), bRemoved);
        SendAutomationResponse(Socket, RequestId, true,
            bRemoved ? TEXT("PCG edge removed.") : TEXT("No matching edge to remove."),
            Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: break_pin_links
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("break_pin_links"))
    {
        FString NodeId = McpHandlerUtils::GetOptionalString(Payload, TEXT("nodeId"));
        FString PinName = McpHandlerUtils::GetOptionalString(Payload, TEXT("pinName"));

        UPCGNode* Node = FindNodeByIdentifier(Graph, NodeId);
        if (!Node)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Node not found: %s"), *NodeId),
                TEXT("NODE_NOT_FOUND"));
            return true;
        }

        UPCGPin* Pin = FindPinByName(Node, PinName, /*bOutput*/ false);
        if (!Pin) Pin = FindPinByName(Node, PinName, /*bOutput*/ true);
        if (!Pin)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Pin '%s' not found."), *PinName),
                TEXT("PIN_NOT_FOUND"));
            return true;
        }

        const int32 EdgeCountBefore = Pin->EdgeCount();
        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_BreakPCGPin", "Break PCG Pin Links"));
        Graph->Modify();
        TSet<UPCGNode*> Touched;
        Pin->BreakAllEdges(&Touched);

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("nodeId"), Node->GetName());
        Result->SetStringField(TEXT("pinName"), Pin->Properties.Label.ToString());
        Result->SetNumberField(TEXT("brokenCount"), EdgeCountBefore);
        SendAutomationResponse(Socket, RequestId, true,
            FString::Printf(TEXT("Broke %d edge(s) on pin '%s'."),
                EdgeCountBefore, *Pin->Properties.Label.ToString()),
            Result);
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: add_user_parameter
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("add_user_parameter"))
    {
        FString ParamName = McpHandlerUtils::GetOptionalString(Payload, TEXT("parameterName"));
        FString ParamType = McpHandlerUtils::GetOptionalString(Payload, TEXT("parameterType"));
        if (ParamName.IsEmpty() || ParamType.IsEmpty())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("add_user_parameter requires 'parameterName' and 'parameterType'."),
                TEXT("INVALID_ARGUMENT"));
            return true;
        }

        // Map common type strings to EPropertyBagPropertyType.
        EPropertyBagPropertyType BagType = EPropertyBagPropertyType::None;
        const FString TypeLower = ParamType.ToLower();
        if (TypeLower == TEXT("bool"))               BagType = EPropertyBagPropertyType::Bool;
        else if (TypeLower == TEXT("int") ||
                 TypeLower == TEXT("int32") ||
                 TypeLower == TEXT("integer"))        BagType = EPropertyBagPropertyType::Int32;
        else if (TypeLower == TEXT("int64"))          BagType = EPropertyBagPropertyType::Int64;
        else if (TypeLower == TEXT("float"))          BagType = EPropertyBagPropertyType::Float;
        else if (TypeLower == TEXT("double") ||
                 TypeLower == TEXT("number"))         BagType = EPropertyBagPropertyType::Double;
        else if (TypeLower == TEXT("name"))           BagType = EPropertyBagPropertyType::Name;
        else if (TypeLower == TEXT("string"))         BagType = EPropertyBagPropertyType::String;
        else if (TypeLower == TEXT("text"))           BagType = EPropertyBagPropertyType::Text;

        if (BagType == EPropertyBagPropertyType::None)
        {
            SendAutomationError(Socket, RequestId,
                FString::Printf(TEXT("Unsupported parameterType: %s. Supported: bool, int, int64, float, double, name, string, text."), *ParamType),
                TEXT("UNSUPPORTED_TYPE"));
            return true;
        }

        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_AddPCGUserParam", "Add PCG User Parameter"));
        Graph->Modify();

        TArray<FPropertyBagPropertyDesc> Descs;
        Descs.Emplace(FName(*ParamName), BagType);
        const EPropertyBagAlterationResult AddRes = Graph->AddUserParameters(Descs);

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("parameterName"), ParamName);
        Result->SetStringField(TEXT("parameterType"), ParamType);
        const bool bSuccess = (AddRes == EPropertyBagAlterationResult::Success);
        SendAutomationResponse(Socket, RequestId, bSuccess,
            bSuccess ? TEXT("User parameter added.")
                     : TEXT("AddUserParameters returned failure (parameter may already exist)."),
            Result, bSuccess ? FString() : TEXT("ADD_PARAM_FAILED"));
        return true;
    }

    // -----------------------------------------------------------------------
    // Sub-action: remove_user_parameter
    // -----------------------------------------------------------------------
    if (SubAction == TEXT("remove_user_parameter"))
    {
        FString ParamName = McpHandlerUtils::GetOptionalString(Payload, TEXT("parameterName"));
        if (ParamName.IsEmpty())
        {
            SendAutomationError(Socket, RequestId,
                TEXT("remove_user_parameter requires 'parameterName'."),
                TEXT("INVALID_ARGUMENT"));
            return true;
        }

        FScopedTransaction Tx(NSLOCTEXT("MCP", "MCP_RemovePCGUserParam", "Remove PCG User Parameter"));
        Graph->Modify();

        bool bRemoved = false;
        Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& Bag)
        {
            const FName NameToRemove(*ParamName);
            const EPropertyBagAlterationResult Res = Bag.RemovePropertyByName(NameToRemove);
            bRemoved = (Res == EPropertyBagAlterationResult::Success);
        });

        SaveLoadedAssetThrottled(Graph);

        TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
        McpHandlerUtils::AddVerification(Result, Graph);
        Result->SetStringField(TEXT("parameterName"), ParamName);
        Result->SetBoolField(TEXT("removed"), bRemoved);
        SendAutomationResponse(Socket, RequestId, true,
            bRemoved ? TEXT("User parameter removed.") : TEXT("Parameter not found."),
            Result);
        return true;
    }

    SendAutomationError(Socket, RequestId,
        FString::Printf(TEXT("Unknown subAction: %s"), *SubAction),
        TEXT("INVALID_SUBACTION"));
    return true;

#endif // WITH_EDITOR && MCP_HAS_PCG_PLUGIN
}
