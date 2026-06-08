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
		FName(TEXT("duplicate_asset")),
		TEXT("duplicate_asset"),
		TEXT("Duplicate one asset to a new /Game path after backend Proposal confirmation."),
		TEXT("asset"),
		{ TEXT("source_asset_path"), TEXT("new_name") },
		{ TEXT("target_folder"), TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("fixup_redirectors")),
		TEXT("fixup_redirectors"),
		TEXT("Fix redirectors under one bounded /Game folder after backend Proposal confirmation."),
		TEXT("asset"),
		{ TEXT("folder_path") },
		{ TEXT("recursive"), TEXT("max_redirectors"), TEXT("reason"), TEXT("source_task_id") }));
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
		FName(TEXT("add_blueprint_node_template")),
		TEXT("add_blueprint_node_template"),
		TEXT("Add one whitelisted Blueprint node template after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path"), TEXT("template_id") },
		{ TEXT("graph_name"), TEXT("message"), TEXT("duration"), TEXT("delay_seconds"), TEXT("print_to_screen"), TEXT("print_to_log"), TEXT("entry_event"), TEXT("input_action_path"), TEXT("node_position"), TEXT("node_comment"), TEXT("compile_after_edit") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("connect_blueprint_nodes")),
		TEXT("connect_blueprint_nodes"),
		TEXT("Connect two explicit Blueprint pins in one graph after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path"), TEXT("graph_name"), TEXT("source_node_id"), TEXT("source_pin_name"), TEXT("target_node_id"), TEXT("target_pin_name") },
		{ TEXT("compile_after_edit"), TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("compile_blueprint")),
		TEXT("compile_blueprint"),
		TEXT("Compile one Blueprint after backend Proposal confirmation."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path") },
		{ TEXT("compile_mode") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_editor_context")),
		TEXT("get_editor_context"),
		TEXT("Read lightweight live Unreal Editor context through the optional TCP tool server."),
		TEXT("editor"),
		{},
		{},
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_selected_assets")),
		TEXT("get_selected_assets"),
		TEXT("Read currently selected Content Browser assets and Static Mesh detail through the optional TCP tool server."),
		TEXT("asset"),
		{},
		{},
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_static_mesh_details")),
		TEXT("get_static_mesh_details"),
		TEXT("Read Static Mesh Nanite, LOD, collision, lightmap, and material slot details by path, query, or current selection."),
		TEXT("asset"),
		{},
		{ TEXT("static_mesh_path"), TEXT("asset_path"), TEXT("query") },
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_selected_actors")),
		TEXT("get_selected_actors"),
		TEXT("Read currently selected Level Actors through the optional TCP tool server."),
		TEXT("level"),
		{},
		{},
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_level_actors")),
		TEXT("get_level_actors"),
		TEXT("Read current level Actors with optional query, class, tag, folder, and limit filters."),
		TEXT("level"),
		{},
		{ TEXT("query"), TEXT("class_contains"), TEXT("tag"), TEXT("folder_path"), TEXT("limit") },
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_level_actor_details")),
		TEXT("get_level_actor_details"),
		TEXT("Read one live Level Actor's transform, tags, folder, class, and components through the optional TCP tool server."),
		TEXT("level"),
		{ TEXT("actor_reference") },
		{ TEXT("query"), TEXT("actor_label"), TEXT("actor_name"), TEXT("actor_path") },
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_blueprint_graph")),
		TEXT("get_blueprint_graph"),
		TEXT("Read Blueprint graph metadata through the optional TCP tool server."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path") },
		{},
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_blueprint_node_details")),
		TEXT("get_blueprint_node_details"),
		TEXT("Read one Blueprint graph node's live title, class, pins, links, and graph metadata through the optional TCP tool server."),
		TEXT("blueprint"),
		{ TEXT("blueprint_path"), TEXT("node_query") },
		{ TEXT("graph_name"), TEXT("node_id"), TEXT("node_name"), TEXT("node_title"), TEXT("target_node"), TEXT("query") },
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
		FName(TEXT("get_widget_details")),
		TEXT("get_widget_details"),
		TEXT("Read one UMG Widget's live properties, parent, children, and slot metadata through the optional TCP tool server."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name") },
		{ TEXT("blueprint_path"), TEXT("target_widget"), TEXT("query") },
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("get_material_instance_parameters")),
		TEXT("get_material_instance_parameters"),
		TEXT("Read Material Instance parameters through the optional TCP tool server."),
		TEXT("material"),
		{},
		{ TEXT("material_instance_path") },
		TEXT("read_only")));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("add_umg_widget")),
		TEXT("add_umg_widget"),
		TEXT("Add one simple Widget to a Widget Blueprint after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("widget_class") },
		{ TEXT("parent_widget_name"), TEXT("text"), TEXT("is_variable") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("duplicate_umg_widget")),
		TEXT("duplicate_umg_widget"),
		TEXT("Duplicate one existing non-panel UMG widget under the same parent after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("new_widget_name") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("delete_umg_widget")),
		TEXT("delete_umg_widget"),
		TEXT("Remove one existing non-root non-panel UMG widget after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_umg_widget_text")),
		TEXT("set_umg_widget_text"),
		TEXT("Set text on one TextBlock in a Widget Blueprint after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("text") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_umg_widget_layout")),
		TEXT("set_umg_widget_layout"),
		TEXT("Set CanvasPanelSlot layout fields on one UMG widget after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("layout") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_umg_slot_layout_v2")),
		TEXT("set_umg_slot_layout_v2"),
		TEXT("Set HorizontalBox, VerticalBox, or Overlay slot layout fields after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("slot_type"), TEXT("layout") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("reparent_umg_widget")),
		TEXT("reparent_umg_widget"),
		TEXT("Move one existing UMG widget under another existing panel widget after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("new_parent_name") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_umg_widget_visibility")),
		TEXT("set_umg_widget_visibility"),
		TEXT("Set visibility on one UMG widget after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("visibility") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_umg_widget_appearance")),
		TEXT("set_umg_widget_appearance"),
		TEXT("Set safe appearance fields on one UMG widget after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("appearance") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_umg_widget_brush")),
		TEXT("set_umg_widget_brush"),
		TEXT("Set Image or Border brush resource on one UMG widget after backend Proposal confirmation."),
		TEXT("umg"),
		{ TEXT("widget_blueprint_path"), TEXT("widget_name"), TEXT("brush") },
		{ TEXT("reason"), TEXT("source_task_id") }));
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
		FName(TEXT("set_actor_metadata")),
		TEXT("set_actor_metadata"),
		TEXT("Update one Actor label, folder, or tags in the current editor level after backend Proposal confirmation."),
		TEXT("level"),
		{ TEXT("actor_reference"), TEXT("metadata") },
		{ TEXT("reason"), TEXT("source_task_id") }));
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("arrange_actors_pattern")),
		TEXT("arrange_actors_pattern"),
		TEXT("Arrange a bounded Actor set with line, grid, or circle placement templates after backend Proposal confirmation."),
		TEXT("level"),
		{ TEXT("actor_references"), TEXT("pattern") },
		{ TEXT("reason"), TEXT("source_task_id") }));
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
	Definitions.Add(UEAgentEditorToolCatalogPrivate::MakeTool(
		FName(TEXT("set_material_instance_static_switch")),
		TEXT("set_material_instance_static_switch"),
		TEXT("Set one static switch parameter on a Material Instance after backend Proposal confirmation."),
		TEXT("material"),
		{ TEXT("material_instance_path"), TEXT("parameter_name"), TEXT("value") },
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
