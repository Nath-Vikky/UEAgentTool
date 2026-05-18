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
		const TArray<FString>& OptionalFields,
		const FString& SideEffectLevel = TEXT("confirmed_write"))
	{
		FUEAgentEditorToolDefinition Definition;
		Definition.ToolName = ToolName;
		Definition.OperationType = OperationType;
		Definition.Description = Description;
		Definition.Category = Category;
		Definition.SideEffectLevel = SideEffectLevel;
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
		FName(TEXT("batch_rename_assets")),
		TEXT("batch_rename_assets"),
		TEXT("Rename multiple assets in one confirmed backend Proposal."),
		TEXT("asset"),
		{ TEXT("renames") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("move_assets")),
		TEXT("move_assets"),
		TEXT("Move multiple assets to one folder after backend Proposal confirmation."),
		TEXT("asset"),
		{ TEXT("asset_paths"), TEXT("target_folder") },
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
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("compile_blueprint")),
		TEXT("compile_blueprint"),
		TEXT("Compile one Blueprint after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path") },
		{ TEXT("compile_mode") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_blueprint_graph")),
		TEXT("get_blueprint_graph"),
		TEXT("Read Blueprint graph metadata through the optional TCP tool server."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path") },
		{},
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_widget_tree")),
		TEXT("get_widget_tree"),
		TEXT("Read UMG Widget Blueprint tree metadata through the optional TCP tool server."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path") },
		{},
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("add_umg_widget")),
		TEXT("add_umg_widget"),
		TEXT("Add one simple Widget to a Widget Blueprint after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("widget_class") },
		{ TEXT("parent_widget_name"), TEXT("text"), TEXT("is_variable") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("place_actor_in_level")),
		TEXT("place_actor_in_level"),
		TEXT("Place one Actor in the current editor level after backend Proposal confirmation."),
		TEXT("level"),
		{ TEXT("actor_class") },
		{ TEXT("actor_label"), TEXT("transform"), TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_actor_transform")),
		TEXT("set_actor_transform"),
		TEXT("Modify one existing Actor transform in the current editor level after backend Proposal confirmation."),
		TEXT("level"),
		{ TEXT("actor_reference"), TEXT("transform_mode") },
		{ TEXT("actor_name"), TEXT("actor_label"), TEXT("transform"), TEXT("transform_delta"), TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_material_instance_parameter")),
		TEXT("set_material_instance_parameter"),
		TEXT("Set one scalar or vector parameter on a Material Instance after backend Proposal confirmation."),
		TEXT("material"),
		{ TEXT("material_instance_path"), TEXT("parameter_name"), TEXT("parameter_type"), TEXT("value") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_material_instance_texture_parameter")),
		TEXT("set_material_instance_texture_parameter"),
		TEXT("Set one texture parameter on a Material Instance after backend Proposal confirmation."),
		TEXT("material"),
		{ TEXT("material_instance_path"), TEXT("parameter_name"), TEXT("texture_path") },
		{ TEXT("reason"), TEXT("source_task_id") }));
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
