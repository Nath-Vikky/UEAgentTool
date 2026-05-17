// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentEditorToolCatalog.h"

namespace UEAgentEditorToolCatalogPrivate
{
	static FUEAgentEditorToolDefinition MakeTool(
		const FName ToolName,
		const FString& OperationType,
		const FString& Description,
		const FString& Category,
		const TArray<FString>& RequiredFields,
		const TArray<FString>& OptionalFields)
	{
		FUEAgentEditorToolDefinition Definition;
		Definition.ToolName = ToolName;
		Definition.OperationType = OperationType;
		Definition.Description = Description;
		Definition.Category = Category;
		Definition.SideEffectLevel = TEXT("confirmed_write");
		Definition.RequiredFields = RequiredFields;
		Definition.OptionalFields = OptionalFields;
		Definition.bEnabled = true;
		return Definition;
	}
}

TArray<FUEAgentEditorToolDefinition> FUEAgentEditorToolCatalog::BuildCoreEditorOperationDefinitions()
{
	TArray<FUEAgentEditorToolDefinition> Definitions;
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("rename_asset")),
		TEXT("rename_selected_asset"),
		TEXT("Rename one selected/content-browser asset after backend Proposal confirmation."),
		TEXT("asset"),
		{ TEXT("asset_path"), TEXT("new_name") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_static_mesh_settings")),
		TEXT("apply_static_mesh_basic_settings"),
		TEXT("Apply whitelisted Static Mesh settings after backend Proposal confirmation."),
		TEXT("asset"),
		{ TEXT("asset_path"), TEXT("settings") },
		{ TEXT("before_snapshot"), TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("create_blueprint")),
		TEXT("create_blueprint_asset"),
		TEXT("Create one Blueprint asset under /Game after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("parent_class"), TEXT("target_folder"), TEXT("asset_name") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("add_blueprint_variable")),
		TEXT("add_blueprint_variable"),
		TEXT("Add one member variable to a Blueprint after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path"), TEXT("variable_name"), TEXT("variable_type") },
		{ TEXT("category"), TEXT("default_value"), TEXT("editable"), TEXT("expose_on_spawn") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("add_blueprint_component")),
		TEXT("add_blueprint_component"),
		TEXT("Add one component node to a Blueprint after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path"), TEXT("component_name"), TEXT("component_class") },
		{ TEXT("attach_to"), TEXT("transform") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("add_blueprint_event")),
		TEXT("create_blueprint_event_stub"),
		TEXT("Create a small Blueprint event stub after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path"), TEXT("event_name") },
		{ TEXT("graph_name"), TEXT("node_comment") }));
	return Definitions;
}

FUEAgentEditorToolRegistry FUEAgentEditorToolCatalog::BuildMetadataRegistry()
{
	FUEAgentEditorToolRegistry Registry;
	for (const FUEAgentEditorToolDefinition& Definition : BuildCoreEditorOperationDefinitions())
	{
		Registry.RegisterTool(Definition);
	}
	return Registry;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolCatalog::BuildToolsListJson()
{
	return BuildMetadataRegistry().ToJsonObject();
}
