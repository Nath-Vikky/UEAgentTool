// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentEditorToolServer.h"

#include "AgentEditorToolCatalog.h"
#include "Modules/ModuleManager.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Async/Async.h"
#include "Blueprint/WidgetTree.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/Selection.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/SceneComponent.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Engine/Texture.h"
#include "Common/TcpListener.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAgentEditorToolServer, Log, All);

namespace UEAgentEditorToolServerPrivate
{
	static constexpr int32 MaxReadChunkBytes = 64 * 1024;
	static constexpr int32 SocketWaitSeconds = 2;

	static FString NormalizeAssetPackagePath(FString AssetPath);
	static FString ToObjectPath(const FString& PackagePath);
	static UStaticMesh* LoadStaticMeshAsset(const FString& StaticMeshPath);
	static UStaticMesh* FindSelectedStaticMeshAsset(FString& OutResolvedPath);
	static UStaticMesh* FindStaticMeshAssetByQuery(const FString& Query, FString& OutResolvedPath);
	static TSharedPtr<FJsonObject> MakeToolErrorObject(const FString& Reason, const FString& Message);

	static TArray<TSharedPtr<FJsonValue>> StringsToJsonArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	static FString CollisionTraceFlagToString(const ECollisionTraceFlag CollisionFlag)
	{
		switch (CollisionFlag)
		{
		case CTF_UseDefault:
			return TEXT("project_default");
		case CTF_UseSimpleAndComplex:
			return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex:
			return TEXT("use_simple_as_complex");
		case CTF_UseComplexAsSimple:
			return TEXT("use_complex_as_simple");
		default:
			return TEXT("unknown");
		}
	}

	static void AddStaticMeshSelectedAssetDetails(const UStaticMesh* StaticMesh, const TSharedPtr<FJsonObject>& AssetObject)
	{
		if (StaticMesh == nullptr || !AssetObject.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> StaticMeshObject = MakeShared<FJsonObject>();
#if WITH_EDITORONLY_DATA
		StaticMeshObject->SetBoolField(TEXT("nanite_enabled"), StaticMesh->NaniteSettings.bEnabled);
#endif
		StaticMeshObject->SetNumberField(TEXT("lod_count"), StaticMesh->GetNumLODs());
		StaticMeshObject->SetNumberField(TEXT("lightmap_resolution"), StaticMesh->GetLightMapResolution());

		if (const UBodySetup* BodySetup = StaticMesh->GetBodySetup())
		{
			StaticMeshObject->SetStringField(TEXT("collision_complexity"), CollisionTraceFlagToString(BodySetup->CollisionTraceFlag));
		}

		constexpr int32 MaxMaterialSlotsReturned = 32;
		const TArray<FStaticMaterial>& StaticMaterials = StaticMesh->GetStaticMaterials();
		TArray<TSharedPtr<FJsonValue>> MaterialSlotValues;
		for (const FStaticMaterial& StaticMaterial : StaticMaterials)
		{
			TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
			SlotObject->SetStringField(TEXT("slot_name"), StaticMaterial.MaterialSlotName.ToString());
			if (StaticMaterial.MaterialInterface != nullptr)
			{
				SlotObject->SetStringField(TEXT("material_path"), StaticMaterial.MaterialInterface->GetPathName());
			}
			MaterialSlotValues.Add(MakeShared<FJsonValueObject>(SlotObject));
			if (MaterialSlotValues.Num() >= MaxMaterialSlotsReturned)
			{
				break;
			}
		}

		StaticMeshObject->SetNumberField(TEXT("material_slot_count"), StaticMaterials.Num());
		StaticMeshObject->SetNumberField(TEXT("max_material_slots_returned"), MaxMaterialSlotsReturned);
		StaticMeshObject->SetArrayField(TEXT("material_slots"), MaterialSlotValues);
		AssetObject->SetObjectField(TEXT("static_mesh"), StaticMeshObject);
	}

	static TSharedPtr<FJsonObject> BuildStaticMeshDetailsSnapshot(const FString& StaticMeshPathOrQuery, const FString& ServerStatus)
	{
		FString ResolvedPath;
		UStaticMesh* StaticMesh = nullptr;
		const FString Query = StaticMeshPathOrQuery.TrimStartAndEnd();
		if (!Query.IsEmpty())
		{
			StaticMesh = FindStaticMeshAssetByQuery(Query, ResolvedPath);
		}
		if (StaticMesh == nullptr)
		{
			StaticMesh = FindSelectedStaticMeshAsset(ResolvedPath);
		}
		if (StaticMesh == nullptr)
		{
			return MakeToolErrorObject(TEXT("static_mesh_not_found"), TEXT("No Static Mesh matched the provided path/query and no Static Mesh is selected."));
		}

		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("static_mesh_schema_version"), TEXT("ue_agent_static_mesh_details_v1"));
		SnapshotObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
		SnapshotObject->SetStringField(TEXT("server_status"), ServerStatus);
		SnapshotObject->SetStringField(TEXT("static_mesh_name"), StaticMesh->GetName());
		SnapshotObject->SetStringField(TEXT("static_mesh_path"), !ResolvedPath.IsEmpty() ? ResolvedPath : StaticMesh->GetPathName());
		SnapshotObject->SetStringField(TEXT("resolved_from"), !Query.IsEmpty() ? TEXT("query_or_path") : TEXT("selected_content_browser_asset"));
		AddStaticMeshSelectedAssetDetails(StaticMesh, SnapshotObject);
		return SnapshotObject;
	}

	static TSharedPtr<FJsonObject> MakeInputSchema(const TArray<FString>& RequiredFields, const TArray<FString>& OptionalFields)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		for (const FString& FieldName : RequiredFields)
		{
			Properties->SetObjectField(FieldName, MakeShared<FJsonObject>());
		}
		for (const FString& FieldName : OptionalFields)
		{
			if (!Properties->HasField(FieldName))
			{
				Properties->SetObjectField(FieldName, MakeShared<FJsonObject>());
			}
		}
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		Schema->SetArrayField(TEXT("required"), StringsToJsonArray(RequiredFields));
		return Schema;
	}

	static TSharedPtr<FJsonObject> MakeMcpToolObject(const FUEAgentEditorToolDefinition& Definition)
	{
		TSharedPtr<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), Definition.OperationType);
		ToolObject->SetStringField(TEXT("description"), Definition.Description);
		ToolObject->SetObjectField(TEXT("inputSchema"), MakeInputSchema(Definition.RequiredFields, Definition.OptionalFields));

		TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetStringField(TEXT("tool_name"), Definition.ToolName.ToString());
		Annotations->SetStringField(TEXT("operation_type"), Definition.OperationType);
		Annotations->SetStringField(TEXT("category"), Definition.Category);
		Annotations->SetStringField(TEXT("side_effect_level"), Definition.SideEffectLevel);
		Annotations->SetBoolField(TEXT("requires_confirmation"), Definition.SideEffectLevel != TEXT("read_only"));
		Annotations->SetStringField(TEXT("execution_policy"), TEXT("confirmed_write_tools_must_use_http_proposal"));
		ToolObject->SetObjectField(TEXT("annotations"), Annotations);
		return ToolObject;
	}

	static TSharedPtr<FJsonObject> MakeReadOnlyCatalogToolObject()
	{
		TSharedPtr<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), TEXT("ue_agent_tools_list"));
		ToolObject->SetStringField(TEXT("description"), TEXT("Return UEAgentTool editor tool metadata without executing editor writes."));
		ToolObject->SetObjectField(TEXT("inputSchema"), MakeInputSchema(TArray<FString>(), TArray<FString>()));

		TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetStringField(TEXT("category"), TEXT("diagnostics"));
		Annotations->SetStringField(TEXT("side_effect_level"), TEXT("read_only"));
		Annotations->SetBoolField(TEXT("requires_confirmation"), false);
		ToolObject->SetObjectField(TEXT("annotations"), Annotations);
		return ToolObject;
	}

	static FString GetEnvVar(const TCHAR* Name)
	{
		return FPlatformMisc::GetEnvironmentVariable(Name).TrimStartAndEnd();
	}

	static bool IsTruthy(const FString& Value)
	{
		return Value.Equals(TEXT("1")) || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("on"), ESearchCase::IgnoreCase);
	}

	static FString BlueprintStatusToString(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown:
			return TEXT("unknown");
		case BS_Dirty:
			return TEXT("dirty");
		case BS_Error:
			return TEXT("error");
		case BS_UpToDate:
			return TEXT("up_to_date");
		case BS_BeingCreated:
			return TEXT("being_created");
		case BS_UpToDateWithWarnings:
			return TEXT("up_to_date_with_warnings");
		default:
			return TEXT("unknown");
		}
	}

	static FString NormalizeAssetPackagePath(FString AssetPath)
	{
		AssetPath = AssetPath.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
		if (AssetPath.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
		{
			AssetPath.LeftChopInline(7);
		}
		if (AssetPath.Contains(TEXT(".")) && AssetPath.StartsWith(TEXT("/")))
		{
			FString PackagePath;
			FString ObjectName;
			if (AssetPath.Split(TEXT("."), &PackagePath, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
				&& !ObjectName.IsEmpty()
				&& PackagePath.EndsWith(FString::Printf(TEXT("/%s"), *ObjectName)))
			{
				AssetPath = PackagePath;
			}
		}
		if (!AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RemoveFromStart(TEXT("/"));
			AssetPath.RemoveFromEnd(TEXT("/"));
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);
		}
		while (AssetPath.Contains(TEXT("//")))
		{
			AssetPath = AssetPath.Replace(TEXT("//"), TEXT("/"));
		}
		return AssetPath;
	}

	static FString GetAssetNameFromPackagePath(const FString& PackagePath)
	{
		int32 SlashIndex = INDEX_NONE;
		return PackagePath.FindLastChar(TEXT('/'), SlashIndex) ? PackagePath.Mid(SlashIndex + 1) : PackagePath;
	}

	static FString ToObjectPath(const FString& PackagePath)
	{
		if (PackagePath.Contains(TEXT(".")))
		{
			return PackagePath;
		}
		const FString AssetName = GetAssetNameFromPackagePath(PackagePath);
		return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	}

	static UBlueprint* LoadBlueprintAsset(const FString& BlueprintPath)
	{
		const FString PackagePath = NormalizeAssetPackagePath(BlueprintPath);
		return Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
	}

	static UMaterialInstance* LoadMaterialInstanceAsset(const FString& MaterialInstancePath)
	{
		if (MaterialInstancePath.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		const FString PackagePath = NormalizeAssetPackagePath(MaterialInstancePath);
		return Cast<UMaterialInstance>(StaticLoadObject(UMaterialInstance::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
	}

	static UStaticMesh* LoadStaticMeshAsset(const FString& StaticMeshPath)
	{
		if (StaticMeshPath.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		const FString PackagePath = NormalizeAssetPackagePath(StaticMeshPath);
		return Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
	}

	static UStaticMesh* FindSelectedStaticMeshAsset(FString& OutResolvedPath)
	{
		TArray<FAssetData> SelectedAssets;
		if (!FModuleManager::Get().ModuleExists(TEXT("ContentBrowser")))
		{
			return nullptr;
		}
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
		for (const FAssetData& AssetData : SelectedAssets)
		{
			if (!AssetData.IsValid())
			{
				continue;
			}
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
			{
				OutResolvedPath = AssetData.GetSoftObjectPath().ToString();
				return StaticMesh;
			}
		}
		return nullptr;
	}

	static UStaticMesh* FindStaticMeshAssetByQuery(const FString& Query, FString& OutResolvedPath)
	{
		const FString Needle = Query.TrimStartAndEnd();
		if (Needle.IsEmpty())
		{
			return nullptr;
		}
		if (Needle.StartsWith(TEXT("/")))
		{
			if (UStaticMesh* StaticMesh = LoadStaticMeshAsset(Needle))
			{
				OutResolvedPath = ToObjectPath(NormalizeAssetPackagePath(Needle));
				return StaticMesh;
			}
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FARFilter Filter;
		Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AssetDataList;
		AssetRegistryModule.Get().GetAssets(Filter, AssetDataList);
		for (const FAssetData& AssetData : AssetDataList)
		{
			const FString AssetName = AssetData.AssetName.ToString();
			const FString ObjectPath = AssetData.GetSoftObjectPath().ToString();
			if (AssetName.Equals(Needle, ESearchCase::IgnoreCase)
				|| AssetName.Contains(Needle, ESearchCase::IgnoreCase)
				|| ObjectPath.Contains(Needle, ESearchCase::IgnoreCase))
			{
				if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
				{
					OutResolvedPath = ObjectPath;
					return StaticMesh;
				}
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> MakeMaterialColorObject(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> ColorObject = MakeShared<FJsonObject>();
		ColorObject->SetNumberField(TEXT("r"), Color.R);
		ColorObject->SetNumberField(TEXT("g"), Color.G);
		ColorObject->SetNumberField(TEXT("b"), Color.B);
		ColorObject->SetNumberField(TEXT("a"), Color.A);
		return ColorObject;
	}

	static TSharedPtr<FJsonObject> MakeMaterialParameterObject(const FMaterialParameterInfo& ParameterInfo, const FString& ParameterType)
	{
		TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
		ParameterObject->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
		ParameterObject->SetStringField(TEXT("parameter_name"), ParameterInfo.Name.ToString());
		ParameterObject->SetStringField(TEXT("parameter_type"), ParameterType);
		return ParameterObject;
	}

	static void AddScalarMaterialParameters(
		const UMaterialInstance* MaterialInstance,
		TArray<TSharedPtr<FJsonValue>>& ParameterValues,
		TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllScalarParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			float Value = 0.0f;
			TSharedPtr<FJsonObject> ParameterObject = MakeMaterialParameterObject(ParameterInfo, TEXT("scalar"));
			if (MaterialInstance->GetScalarParameterValue(ParameterInfo, Value))
			{
				ParameterObject->SetNumberField(TEXT("value"), Value);
			}
			ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			AllParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			if (ParameterValues.Num() >= 128)
			{
				break;
			}
		}
	}

	static void AddVectorMaterialParameters(
		const UMaterialInstance* MaterialInstance,
		TArray<TSharedPtr<FJsonValue>>& ParameterValues,
		TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllVectorParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			FLinearColor Value = FLinearColor::White;
			TSharedPtr<FJsonObject> ParameterObject = MakeMaterialParameterObject(ParameterInfo, TEXT("vector"));
			if (MaterialInstance->GetVectorParameterValue(ParameterInfo, Value))
			{
				ParameterObject->SetObjectField(TEXT("value"), MakeMaterialColorObject(Value));
			}
			ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			AllParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			if (ParameterValues.Num() >= 128)
			{
				break;
			}
		}
	}

	static void AddTextureMaterialParameters(
		const UMaterialInstance* MaterialInstance,
		TArray<TSharedPtr<FJsonValue>>& ParameterValues,
		TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllTextureParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			UTexture* Value = nullptr;
			TSharedPtr<FJsonObject> ParameterObject = MakeMaterialParameterObject(ParameterInfo, TEXT("texture"));
			if (MaterialInstance->GetTextureParameterValue(ParameterInfo, Value) && Value != nullptr)
			{
				ParameterObject->SetStringField(TEXT("texture_path"), Value->GetPathName());
				ParameterObject->SetStringField(TEXT("value"), Value->GetPathName());
			}
			ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			AllParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			if (ParameterValues.Num() >= 128)
			{
				break;
			}
		}
	}

	static void AddStaticSwitchMaterialParameters(
		const UMaterialInstance* MaterialInstance,
		TArray<TSharedPtr<FJsonValue>>& ParameterValues,
		TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllStaticSwitchParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			bool bValue = false;
			FGuid ExpressionGuid;
			TSharedPtr<FJsonObject> ParameterObject = MakeMaterialParameterObject(ParameterInfo, TEXT("static_switch"));
			if (MaterialInstance->GetStaticSwitchParameterValue(ParameterInfo, bValue, ExpressionGuid))
			{
				ParameterObject->SetBoolField(TEXT("value"), bValue);
			}
			ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			AllParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			if (ParameterValues.Num() >= 128)
			{
				break;
			}
		}
	}

	static UMaterialInstance* ResolveMaterialInstanceAsset(
		const FString& MaterialInstancePath,
		FString& OutResolvedFrom,
		FString& OutResolvedPath,
		int32& OutSelectedAssetCount)
	{
		OutResolvedFrom = TEXT("");
		OutResolvedPath = TEXT("");
		OutSelectedAssetCount = 0;
		if (!MaterialInstancePath.TrimStartAndEnd().IsEmpty())
		{
			if (UMaterialInstance* LoadedMaterialInstance = LoadMaterialInstanceAsset(MaterialInstancePath))
			{
				OutResolvedFrom = TEXT("argument");
				OutResolvedPath = LoadedMaterialInstance->GetPathName();
				return LoadedMaterialInstance;
			}
		}

		if (!FModuleManager::Get().ModuleExists(TEXT("ContentBrowser")))
		{
			return nullptr;
		}

		TArray<FAssetData> SelectedAssets;
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
		OutSelectedAssetCount = SelectedAssets.Num();
		for (const FAssetData& AssetData : SelectedAssets)
		{
			if (!AssetData.IsValid())
			{
				continue;
			}
			UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(AssetData.GetAsset());
			if (MaterialInstance != nullptr)
			{
				OutResolvedFrom = TEXT("content_browser_selection");
				OutResolvedPath = AssetData.GetSoftObjectPath().ToString();
				return MaterialInstance;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> MakeToolErrorObject(const FString& Reason, const FString& Message)
	{
		TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetBoolField(TEXT("isError"), true);
		TArray<TSharedPtr<FJsonValue>> ContentValues;
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), Message);
		ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
		ResultObject->SetArrayField(TEXT("content"), ContentValues);
		TSharedPtr<FJsonObject> StructuredObject = MakeShared<FJsonObject>();
		StructuredObject->SetStringField(TEXT("reason"), Reason);
		StructuredObject->SetStringField(TEXT("message"), Message);
		ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
		return ResultObject;
	}

	static FString PinDirectionToString(const EEdGraphPinDirection Direction)
	{
		switch (Direction)
		{
		case EGPD_Input:
			return TEXT("input");
		case EGPD_Output:
			return TEXT("output");
		default:
			return TEXT("unknown");
		}
	}

	static FString PinTypeToSnapshotString(const FEdGraphPinType& PinType)
	{
		FString TypeText = PinType.PinCategory.ToString();
		if (!PinType.PinSubCategory.IsNone())
		{
			TypeText += FString::Printf(TEXT(":%s"), *PinType.PinSubCategory.ToString());
		}
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeText += FString::Printf(TEXT(":%s"), *PinType.PinSubCategoryObject->GetPathName());
		}
		if (PinType.IsArray())
		{
			TypeText += TEXT("[]");
		}
		else if (PinType.IsSet())
		{
			TypeText += TEXT("<set>");
		}
		else if (PinType.IsMap())
		{
			TypeText += TEXT("<map>");
		}
		return TypeText;
	}

	static void AddPinSummaries(
		const UEdGraphNode* Node,
		TArray<TSharedPtr<FJsonValue>>& OutPinValues,
		int32& OutInputPinCount,
		int32& OutOutputPinCount,
		int32& OutLinkCount)
	{
		if (Node == nullptr)
		{
			return;
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr)
			{
				continue;
			}

			if (Pin->Direction == EGPD_Input)
			{
				++OutInputPinCount;
			}
			else if (Pin->Direction == EGPD_Output)
			{
				++OutOutputPinCount;
			}

			TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
			PinObject->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
			PinObject->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
			PinObject->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
			PinObject->SetStringField(TEXT("pin_type"), PinTypeToSnapshotString(Pin->PinType));
			PinObject->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());

			TArray<TSharedPtr<FJsonValue>> LinkValues;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin == nullptr || LinkedPin->GetOwningNode() == nullptr)
				{
					continue;
				}
				++OutLinkCount;
				TSharedPtr<FJsonObject> LinkObject = MakeShared<FJsonObject>();
				LinkObject->SetStringField(TEXT("linked_node_id"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				LinkObject->SetStringField(TEXT("linked_node_name"), LinkedPin->GetOwningNode()->GetName());
				LinkObject->SetStringField(TEXT("linked_pin_id"), LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
				LinkObject->SetStringField(TEXT("linked_pin_name"), LinkedPin->PinName.ToString());
				LinkValues.Add(MakeShared<FJsonValueObject>(LinkObject));
				if (LinkValues.Num() >= 8)
				{
					break;
				}
			}
			PinObject->SetArrayField(TEXT("linked_to"), LinkValues);
			OutPinValues.Add(MakeShared<FJsonValueObject>(PinObject));
			if (OutPinValues.Num() >= 32)
			{
				break;
			}
		}
	}

	static void AddGraphSummaries(
		const TArray<UEdGraph*>& Graphs,
		const FString& GraphType,
		TArray<TSharedPtr<FJsonValue>>& OutGraphValues,
		int32& OutTotalNodeCount,
		int32& OutTotalPinCount,
		int32& OutTotalLinkCount)
	{
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
			GraphObject->SetStringField(TEXT("graph_id"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
			GraphObject->SetStringField(TEXT("graph_name"), Graph->GetName());
			GraphObject->SetStringField(TEXT("graph_type"), GraphType);
			GraphObject->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

			TArray<TSharedPtr<FJsonValue>> NodeValues;
			int32 GraphPinCount = 0;
			int32 GraphLinkCount = 0;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node == nullptr)
				{
					continue;
				}
				TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
				NodeObject->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				NodeObject->SetStringField(TEXT("node_name"), Node->GetName());
				NodeObject->SetStringField(TEXT("node_class"), Node->GetClass() != nullptr ? Node->GetClass()->GetName() : TEXT("unknown"));
				NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
				NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);
				if (!Node->NodeComment.IsEmpty())
				{
					NodeObject->SetStringField(TEXT("comment"), Node->NodeComment);
				}

				TArray<TSharedPtr<FJsonValue>> PinValues;
				int32 InputPinCount = 0;
				int32 OutputPinCount = 0;
				int32 NodeLinkCount = 0;
				AddPinSummaries(Node, PinValues, InputPinCount, OutputPinCount, NodeLinkCount);
				NodeObject->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
				NodeObject->SetNumberField(TEXT("input_pin_count"), InputPinCount);
				NodeObject->SetNumberField(TEXT("output_pin_count"), OutputPinCount);
				NodeObject->SetNumberField(TEXT("link_count"), NodeLinkCount);
				NodeObject->SetArrayField(TEXT("pins"), PinValues);

				GraphPinCount += Node->Pins.Num();
				GraphLinkCount += NodeLinkCount;
				NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));
				if (NodeValues.Num() >= 64)
				{
					break;
				}
			}
			GraphObject->SetNumberField(TEXT("pin_count"), GraphPinCount);
			GraphObject->SetNumberField(TEXT("link_count"), GraphLinkCount);
			GraphObject->SetArrayField(TEXT("nodes"), NodeValues);
			OutTotalNodeCount += Graph->Nodes.Num();
			OutTotalPinCount += GraphPinCount;
			OutTotalLinkCount += GraphLinkCount;
			OutGraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
			if (OutGraphValues.Num() >= 64)
			{
				break;
			}
		}
	}


	static TSharedPtr<FJsonObject> BuildVectorSnapshot(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Value.X);
		Object->SetNumberField(TEXT("y"), Value.Y);
		Object->SetNumberField(TEXT("z"), Value.Z);
		return Object;
	}

	static TSharedPtr<FJsonObject> BuildVector2DSnapshot(const FVector2D& Value)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Value.X);
		Object->SetNumberField(TEXT("y"), Value.Y);
		return Object;
	}

	static TSharedPtr<FJsonObject> BuildRotatorSnapshot(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("pitch"), Value.Pitch);
		Object->SetNumberField(TEXT("yaw"), Value.Yaw);
		Object->SetNumberField(TEXT("roll"), Value.Roll);
		return Object;
	}

	static TSharedPtr<FJsonObject> BuildTransformSnapshot(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetObjectField(TEXT("location"), BuildVectorSnapshot(Value.GetLocation()));
		Object->SetObjectField(TEXT("rotation"), BuildRotatorSnapshot(Value.Rotator()));
		Object->SetObjectField(TEXT("scale"), BuildVectorSnapshot(Value.GetScale3D()));
		return Object;
	}

	static FString WidgetVisibilityToString(const ESlateVisibility Visibility)
	{
		switch (Visibility)
		{
		case ESlateVisibility::Visible:
			return TEXT("Visible");
		case ESlateVisibility::Collapsed:
			return TEXT("Collapsed");
		case ESlateVisibility::Hidden:
			return TEXT("Hidden");
		case ESlateVisibility::HitTestInvisible:
			return TEXT("HitTestInvisible");
		case ESlateVisibility::SelfHitTestInvisible:
			return TEXT("SelfHitTestInvisible");
		default:
			return TEXT("Unknown");
		}
	}

	static TSharedPtr<FJsonObject> BuildAnchorsSnapshot(const FAnchors& Anchors)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetObjectField(TEXT("minimum"), BuildVector2DSnapshot(Anchors.Minimum));
		Object->SetObjectField(TEXT("maximum"), BuildVector2DSnapshot(Anchors.Maximum));
		return Object;
	}

	static TSharedPtr<FJsonObject> BuildWidgetTransformSnapshot(const FWidgetTransform& Transform)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetObjectField(TEXT("translation"), BuildVector2DSnapshot(Transform.Translation));
		Object->SetObjectField(TEXT("scale"), BuildVector2DSnapshot(Transform.Scale));
		Object->SetObjectField(TEXT("shear"), BuildVector2DSnapshot(Transform.Shear));
		Object->SetNumberField(TEXT("angle"), Transform.Angle);
		return Object;
	}

	static void AddWidgetSlotSnapshot(const UWidget* Widget, const TSharedPtr<FJsonObject>& WidgetObject)
	{
		if (Widget == nullptr || WidgetObject == nullptr || Widget->Slot == nullptr)
		{
			return;
		}

		TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
		SlotObject->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass() != nullptr ? Widget->Slot->GetClass()->GetPathName() : FString());
		if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("CanvasPanelSlot"));
			SlotObject->SetObjectField(TEXT("position"), BuildVector2DSnapshot(CanvasSlot->GetPosition()));
			SlotObject->SetObjectField(TEXT("size"), BuildVector2DSnapshot(CanvasSlot->GetSize()));
			SlotObject->SetObjectField(TEXT("alignment"), BuildVector2DSnapshot(CanvasSlot->GetAlignment()));
			SlotObject->SetObjectField(TEXT("anchors"), BuildAnchorsSnapshot(CanvasSlot->GetAnchors()));
			SlotObject->SetBoolField(TEXT("auto_size"), CanvasSlot->GetAutoSize());
			SlotObject->SetNumberField(TEXT("z_order"), CanvasSlot->GetZOrder());
		}
		WidgetObject->SetObjectField(TEXT("slot"), SlotObject);
	}

	static void AddWidgetTypeSpecificSnapshot(const UWidget* Widget, const TSharedPtr<FJsonObject>& WidgetObject)
	{
		if (Widget == nullptr || WidgetObject == nullptr)
		{
			return;
		}

		if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
		{
			TSharedPtr<FJsonObject> TextObject = MakeShared<FJsonObject>();
			TextObject->SetStringField(TEXT("text"), TextBlock->GetText().ToString());
			TextObject->SetNumberField(TEXT("font_size"), TextBlock->GetFont().Size);
			WidgetObject->SetObjectField(TEXT("text_block"), TextObject);
			return;
		}

		if (const UImage* Image = Cast<UImage>(Widget))
		{
			TSharedPtr<FJsonObject> ImageObject = MakeShared<FJsonObject>();
			const FSlateBrush& Brush = Image->GetBrush();
			ImageObject->SetObjectField(TEXT("image_size"), BuildVector2DSnapshot(Brush.ImageSize));
			if (Brush.GetResourceObject() != nullptr)
			{
				ImageObject->SetStringField(TEXT("resource_path"), Brush.GetResourceObject()->GetPathName());
				ImageObject->SetStringField(TEXT("resource_name"), Brush.GetResourceObject()->GetName());
			}
			WidgetObject->SetObjectField(TEXT("image"), ImageObject);
		}
	}

	static TSharedPtr<FJsonObject> BuildWidgetSnapshot(const UWidget* Widget)
	{
		TSharedPtr<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
		if (Widget == nullptr)
		{
			return WidgetObject;
		}

		WidgetObject->SetStringField(TEXT("widget_name"), Widget->GetName());
		WidgetObject->SetStringField(TEXT("widget_class"), Widget->GetClass() != nullptr ? Widget->GetClass()->GetPathName() : TEXT("unknown"));
		WidgetObject->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
		WidgetObject->SetStringField(TEXT("visibility"), WidgetVisibilityToString(Widget->GetVisibility()));
		WidgetObject->SetObjectField(TEXT("render_transform"), BuildWidgetTransformSnapshot(Widget->GetRenderTransform()));
		WidgetObject->SetObjectField(TEXT("render_transform_pivot"), BuildVector2DSnapshot(Widget->GetRenderTransformPivot()));
		if (const UPanelWidget* ParentWidget = Widget->GetParent())
		{
			WidgetObject->SetStringField(TEXT("parent_widget"), ParentWidget->GetName());
			WidgetObject->SetStringField(TEXT("parent_widget_class"), ParentWidget->GetClass() != nullptr ? ParentWidget->GetClass()->GetPathName() : FString());
		}
		if (Widget->Slot != nullptr)
		{
			WidgetObject->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
		}
		AddWidgetSlotSnapshot(Widget, WidgetObject);
		AddWidgetTypeSpecificSnapshot(Widget, WidgetObject);
		return WidgetObject;
	}

	static TSharedPtr<FJsonObject> BuildActorComponentSnapshot(const UActorComponent* Component)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (Component == nullptr)
		{
			return Object;
		}

		Object->SetStringField(TEXT("component_name"), Component->GetName());
		Object->SetStringField(TEXT("component_class"), Component->GetClass() != nullptr ? Component->GetClass()->GetPathName() : FString());
		Object->SetBoolField(TEXT("is_registered"), Component->IsRegistered());

		const USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
		Object->SetBoolField(TEXT("is_scene_component"), SceneComponent != nullptr);
		if (SceneComponent != nullptr)
		{
			Object->SetObjectField(TEXT("relative_location"), BuildVectorSnapshot(SceneComponent->GetRelativeLocation()));
			Object->SetObjectField(TEXT("relative_rotation"), BuildRotatorSnapshot(SceneComponent->GetRelativeRotation()));
			Object->SetObjectField(TEXT("relative_scale"), BuildVectorSnapshot(SceneComponent->GetRelativeScale3D()));
			if (SceneComponent->GetAttachParent() != nullptr)
			{
				Object->SetStringField(TEXT("attach_parent"), SceneComponent->GetAttachParent()->GetName());
			}
		}
		return Object;
	}
	static TSharedPtr<FJsonObject> BuildEditorContextSnapshot(const FString& ServerStatus)
	{
		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("context_schema_version"), TEXT("ue_agent_editor_context_v1"));
		SnapshotObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
		SnapshotObject->SetStringField(TEXT("server_status"), ServerStatus);
		SnapshotObject->SetBoolField(TEXT("http_proposal_flow_required_for_writes"), true);

		const TArray<FUEAgentEditorToolDefinition> Definitions = FUEAgentEditorToolCatalog::BuildCoreEditorOperationDefinitions();
		int32 ReadOnlyToolCount = 0;
		int32 ConfirmedWriteToolCount = 0;
		TMap<FString, int32> CategoryCounts;
		TArray<FString> ReadOnlyToolNames;
		for (const FUEAgentEditorToolDefinition& Definition : Definitions)
		{
			if (Definition.SideEffectLevel == TEXT("read_only"))
			{
				++ReadOnlyToolCount;
				ReadOnlyToolNames.Add(Definition.OperationType);
			}
			else
			{
				++ConfirmedWriteToolCount;
			}
			int32& Count = CategoryCounts.FindOrAdd(Definition.Category);
			++Count;
		}

		TSharedPtr<FJsonObject> CategoryCountsObject = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Item : CategoryCounts)
		{
			CategoryCountsObject->SetNumberField(Item.Key, Item.Value);
		}

		TSharedPtr<FJsonObject> ToolSummaryObject = MakeShared<FJsonObject>();
		ToolSummaryObject->SetNumberField(TEXT("tool_count"), Definitions.Num());
		ToolSummaryObject->SetNumberField(TEXT("read_only_tool_count"), ReadOnlyToolCount);
		ToolSummaryObject->SetNumberField(TEXT("confirmed_write_tool_count"), ConfirmedWriteToolCount);
		ToolSummaryObject->SetArrayField(TEXT("read_only_tools"), StringsToJsonArray(ReadOnlyToolNames));
		ToolSummaryObject->SetObjectField(TEXT("category_counts"), CategoryCountsObject);
		SnapshotObject->SetObjectField(TEXT("tool_summary"), ToolSummaryObject);

		TSharedPtr<FJsonObject> EditorWorldObject = MakeShared<FJsonObject>();
		EditorWorldObject->SetBoolField(TEXT("editor_available"), GEditor != nullptr);
		EditorWorldObject->SetNumberField(TEXT("selected_actor_count"), GEditor != nullptr ? GEditor->GetSelectedActorCount() : 0);
		UWorld* EditorWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (EditorWorld != nullptr)
		{
			EditorWorldObject->SetStringField(TEXT("world_name"), EditorWorld->GetName());
			EditorWorldObject->SetStringField(TEXT("map_name"), EditorWorld->GetMapName());
			if (EditorWorld->GetCurrentLevel() != nullptr)
			{
				EditorWorldObject->SetStringField(TEXT("current_level_name"), EditorWorld->GetCurrentLevel()->GetOuter()->GetName());
			}
			EditorWorldObject->SetBoolField(TEXT("is_game_world"), EditorWorld->IsGameWorld());
		}
		else
		{
			EditorWorldObject->SetStringField(TEXT("world_name"), TEXT(""));
			EditorWorldObject->SetStringField(TEXT("map_name"), TEXT(""));
		}
		SnapshotObject->SetObjectField(TEXT("editor_world"), EditorWorldObject);
		return SnapshotObject;
	}



	static TSharedPtr<FJsonObject> BuildSelectedAssetsSnapshot(const FString& ServerStatus)
	{
		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("asset_selection_schema_version"), TEXT("ue_agent_selected_assets_v1"));
		SnapshotObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
		SnapshotObject->SetStringField(TEXT("server_status"), ServerStatus);

		constexpr int32 MaxAssetsReturned = 64;
		TArray<FAssetData> SelectedAssets;
		const bool bContentBrowserAvailable = FModuleManager::Get().ModuleExists(TEXT("ContentBrowser"));
		SnapshotObject->SetBoolField(TEXT("content_browser_available"), bContentBrowserAvailable);
		if (bContentBrowserAvailable)
		{
			FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
			ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
		}

		TArray<TSharedPtr<FJsonValue>> AssetValues;
		for (const FAssetData& AssetData : SelectedAssets)
		{
			if (!AssetData.IsValid())
			{
				continue;
			}

			TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
			AssetObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
			AssetObject->SetStringField(TEXT("asset_path"), AssetData.GetSoftObjectPath().ToString());
			AssetObject->SetStringField(TEXT("asset_type"), AssetData.AssetClassPath.GetAssetName().ToString());
			AssetObject->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
			AssetObject->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
			if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
			{
				AddStaticMeshSelectedAssetDetails(StaticMesh, AssetObject);
			}
			AssetValues.Add(MakeShared<FJsonValueObject>(AssetObject));
			if (AssetValues.Num() >= MaxAssetsReturned)
			{
				break;
			}
		}

		SnapshotObject->SetNumberField(TEXT("selected_asset_count"), SelectedAssets.Num());
		SnapshotObject->SetNumberField(TEXT("max_assets_returned"), MaxAssetsReturned);
		SnapshotObject->SetArrayField(TEXT("assets"), AssetValues);
		return SnapshotObject;
	}
	static TSharedPtr<FJsonObject> BuildSelectedActorsSnapshot(const FString& ServerStatus)
	{
		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("selection_schema_version"), TEXT("ue_agent_selected_actors_v1"));
		SnapshotObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
		SnapshotObject->SetStringField(TEXT("server_status"), ServerStatus);
		SnapshotObject->SetBoolField(TEXT("editor_available"), GEditor != nullptr);

		UWorld* EditorWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (EditorWorld != nullptr)
		{
			SnapshotObject->SetStringField(TEXT("world_name"), EditorWorld->GetName());
			SnapshotObject->SetStringField(TEXT("map_name"), EditorWorld->GetMapName());
		}

		constexpr int32 MaxActorsReturned = 64;
		TArray<TSharedPtr<FJsonValue>> ActorValues;
		USelection* Selection = GEditor != nullptr ? GEditor->GetSelectedActors() : nullptr;
		if (Selection != nullptr)
		{
			for (FSelectionIterator It(*Selection); It; ++It)
			{
				AActor* Actor = Cast<AActor>(*It);
				if (Actor == nullptr)
				{
					continue;
				}

				TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
				ActorObject->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
				ActorObject->SetStringField(TEXT("actor_name"), Actor->GetName());
				ActorObject->SetStringField(TEXT("actor_path"), Actor->GetPathName());
				ActorObject->SetStringField(TEXT("actor_class"), Actor->GetClass() != nullptr ? Actor->GetClass()->GetPathName() : FString());
				ActorObject->SetObjectField(TEXT("transform"), BuildTransformSnapshot(Actor->GetActorTransform()));
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				TArray<TSharedPtr<FJsonValue>> ComponentValues;
				for (UActorComponent* Component : Components)
				{
					if (Component == nullptr)
					{
						continue;
					}
					ComponentValues.Add(MakeShared<FJsonValueObject>(BuildActorComponentSnapshot(Component)));
					if (ComponentValues.Num() >= 32)
					{
						break;
					}
				}
				ActorObject->SetNumberField(TEXT("component_count"), Components.Num());
				ActorObject->SetArrayField(TEXT("components"), ComponentValues);
				ActorValues.Add(MakeShared<FJsonValueObject>(ActorObject));
				if (ActorValues.Num() >= MaxActorsReturned)
				{
					break;
				}
			}
		}

		SnapshotObject->SetNumberField(TEXT("selected_actor_count"), Selection != nullptr ? Selection->Num() : 0);
		SnapshotObject->SetNumberField(TEXT("max_actors_returned"), MaxActorsReturned);
		SnapshotObject->SetArrayField(TEXT("actors"), ActorValues);
		return SnapshotObject;
	}

	static TArray<TSharedPtr<FJsonValue>> ActorTagsToJsonArray(const AActor* Actor)
	{
		TArray<TSharedPtr<FJsonValue>> TagValues;
		if (Actor == nullptr)
		{
			return TagValues;
		}
		for (const FName& Tag : Actor->Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			if (TagValues.Num() >= 32)
			{
				break;
			}
		}
		return TagValues;
	}

	static FString ActorFolderPathText(const AActor* Actor)
	{
		if (Actor == nullptr)
		{
			return FString();
		}
#if WITH_EDITOR
		return Actor->GetFolderPath().ToString();
#else
		return FString();
#endif
	}

	static bool ActorHasTagText(const AActor* Actor, const FString& TagText)
	{
		if (Actor == nullptr || TagText.IsEmpty())
		{
			return true;
		}
		for (const FName& Tag : Actor->Tags)
		{
			if (Tag.ToString().Equals(TagText, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool ActorMatchesLevelActorFilters(
		const AActor* Actor,
		const FString& Query,
		const FString& ClassContains,
		const FString& Tag,
		const FString& FolderPath)
	{
		if (Actor == nullptr)
		{
			return false;
		}
		const FString ActorLabel = Actor->GetActorLabel();
		const FString ActorName = Actor->GetName();
		const FString ActorClass = Actor->GetClass() != nullptr ? Actor->GetClass()->GetPathName() : FString();
		const FString ActorFolder = ActorFolderPathText(Actor);

		if (!ClassContains.IsEmpty() && !ActorClass.Contains(ClassContains, ESearchCase::IgnoreCase))
		{
			return false;
		}
		if (!FolderPath.IsEmpty() && !ActorFolder.Contains(FolderPath, ESearchCase::IgnoreCase))
		{
			return false;
		}
		if (!ActorHasTagText(Actor, Tag))
		{
			return false;
		}
		if (Query.IsEmpty())
		{
			return true;
		}
		if (ActorLabel.Contains(Query, ESearchCase::IgnoreCase)
			|| ActorName.Contains(Query, ESearchCase::IgnoreCase)
			|| ActorClass.Contains(Query, ESearchCase::IgnoreCase)
			|| ActorFolder.Contains(Query, ESearchCase::IgnoreCase))
		{
			return true;
		}
		for (const FName& ActorTag : Actor->Tags)
		{
			if (ActorTag.ToString().Contains(Query, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildLevelActorSnapshot(AActor* Actor)
	{
		TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
		if (Actor == nullptr)
		{
			return ActorObject;
		}
		ActorObject->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		ActorObject->SetStringField(TEXT("actor_name"), Actor->GetName());
		ActorObject->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		ActorObject->SetStringField(TEXT("actor_class"), Actor->GetClass() != nullptr ? Actor->GetClass()->GetPathName() : FString());
		ActorObject->SetStringField(TEXT("folder_path"), ActorFolderPathText(Actor));
		ActorObject->SetArrayField(TEXT("tags"), ActorTagsToJsonArray(Actor));
		ActorObject->SetObjectField(TEXT("transform"), BuildTransformSnapshot(Actor->GetActorTransform()));

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		TArray<TSharedPtr<FJsonValue>> ComponentValues;
		for (UActorComponent* Component : Components)
		{
			if (Component == nullptr)
			{
				continue;
			}
			ComponentValues.Add(MakeShared<FJsonValueObject>(BuildActorComponentSnapshot(Component)));
			if (ComponentValues.Num() >= 16)
			{
				break;
			}
		}
		ActorObject->SetNumberField(TEXT("component_count"), Components.Num());
		ActorObject->SetNumberField(TEXT("max_components_returned"), 16);
		ActorObject->SetArrayField(TEXT("components"), ComponentValues);
		return ActorObject;
	}

	static TSharedPtr<FJsonObject> BuildLevelActorsSnapshot(const FString& ServerStatus, const TSharedPtr<FJsonObject>& ArgumentsObject)
	{
		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("level_actor_schema_version"), TEXT("ue_agent_level_actors_v1"));
		SnapshotObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
		SnapshotObject->SetStringField(TEXT("server_status"), ServerStatus);
		SnapshotObject->SetBoolField(TEXT("editor_available"), GEditor != nullptr);

		FString Query;
		FString ClassContains;
		FString Tag;
		FString FolderPath;
		double LimitNumber = 100.0;
		if (ArgumentsObject.IsValid())
		{
			ArgumentsObject->TryGetStringField(TEXT("query"), Query);
			ArgumentsObject->TryGetStringField(TEXT("class_contains"), ClassContains);
			if (ClassContains.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("actor_class"), ClassContains);
			}
			ArgumentsObject->TryGetStringField(TEXT("tag"), Tag);
			ArgumentsObject->TryGetStringField(TEXT("folder_path"), FolderPath);
			ArgumentsObject->TryGetNumberField(TEXT("limit"), LimitNumber);
		}
		const int32 MaxActorsReturned = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 300);

		UWorld* EditorWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (EditorWorld != nullptr)
		{
			SnapshotObject->SetStringField(TEXT("world_name"), EditorWorld->GetName());
			SnapshotObject->SetStringField(TEXT("map_name"), EditorWorld->GetMapName());
		}

		TSharedPtr<FJsonObject> FiltersObject = MakeShared<FJsonObject>();
		FiltersObject->SetStringField(TEXT("query"), Query);
		FiltersObject->SetStringField(TEXT("class_contains"), ClassContains);
		FiltersObject->SetStringField(TEXT("tag"), Tag);
		FiltersObject->SetStringField(TEXT("folder_path"), FolderPath);
		FiltersObject->SetNumberField(TEXT("limit"), MaxActorsReturned);
		SnapshotObject->SetObjectField(TEXT("filters"), FiltersObject);

		TArray<TSharedPtr<FJsonValue>> ActorValues;
		int32 TotalActorCount = 0;
		int32 MatchedActorCount = 0;
		if (EditorWorld != nullptr)
		{
			for (TActorIterator<AActor> It(EditorWorld); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor == nullptr || Actor->IsTemplate())
				{
					continue;
				}
				++TotalActorCount;
				if (!ActorMatchesLevelActorFilters(Actor, Query, ClassContains, Tag, FolderPath))
				{
					continue;
				}
				++MatchedActorCount;
				if (ActorValues.Num() < MaxActorsReturned)
				{
					ActorValues.Add(MakeShared<FJsonValueObject>(BuildLevelActorSnapshot(Actor)));
				}
			}
		}

		SnapshotObject->SetNumberField(TEXT("total_actor_count"), TotalActorCount);
		SnapshotObject->SetNumberField(TEXT("matched_actor_count"), MatchedActorCount);
		SnapshotObject->SetNumberField(TEXT("max_actors_returned"), MaxActorsReturned);
		SnapshotObject->SetArrayField(TEXT("actors"), ActorValues);
		return SnapshotObject;
	}

	static TSharedPtr<FJsonObject> BuildBlueprintGraphSnapshot(const FString& BlueprintPath)
	{
		const FString NormalizedPath = NormalizeAssetPackagePath(BlueprintPath);
		if (NormalizedPath.IsEmpty())
		{
			return MakeToolErrorObject(TEXT("missing_blueprint_path"), TEXT("blueprint_path is required."));
		}
		UBlueprint* Blueprint = LoadBlueprintAsset(NormalizedPath);
		if (Blueprint == nullptr)
		{
			return MakeToolErrorObject(TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *NormalizedPath));
		}

		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("graph_schema_version"), TEXT("blueprint_graph_snapshot_v2"));
		SnapshotObject->SetStringField(TEXT("blueprint_path"), NormalizedPath);
		SnapshotObject->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
		SnapshotObject->SetStringField(TEXT("status"), BlueprintStatusToString(Blueprint->Status));
		SnapshotObject->SetBoolField(TEXT("is_dirty"), Blueprint->GetPackage() != nullptr && Blueprint->GetPackage()->IsDirty());
		SnapshotObject->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint));
		if (Blueprint->ParentClass != nullptr)
		{
			SnapshotObject->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		int32 TotalNodeCount = 0;
		int32 TotalPinCount = 0;
		int32 TotalLinkCount = 0;
#if WITH_EDITORONLY_DATA
		AddGraphSummaries(Blueprint->UbergraphPages, TEXT("event"), GraphValues, TotalNodeCount, TotalPinCount, TotalLinkCount);
		AddGraphSummaries(Blueprint->FunctionGraphs, TEXT("function"), GraphValues, TotalNodeCount, TotalPinCount, TotalLinkCount);
		AddGraphSummaries(Blueprint->MacroGraphs, TEXT("macro"), GraphValues, TotalNodeCount, TotalPinCount, TotalLinkCount);
#endif
		TSharedPtr<FJsonObject> GraphMetricsObject = MakeShared<FJsonObject>();
		GraphMetricsObject->SetNumberField(TEXT("graph_count"), GraphValues.Num());
		GraphMetricsObject->SetNumberField(TEXT("node_count"), TotalNodeCount);
		GraphMetricsObject->SetNumberField(TEXT("pin_count"), TotalPinCount);
		GraphMetricsObject->SetNumberField(TEXT("link_count"), TotalLinkCount);
		GraphMetricsObject->SetNumberField(TEXT("max_graphs_returned"), 64);
		GraphMetricsObject->SetNumberField(TEXT("max_nodes_per_graph"), 64);
		GraphMetricsObject->SetNumberField(TEXT("max_pins_per_node"), 32);
		GraphMetricsObject->SetNumberField(TEXT("max_links_per_pin"), 8);
		SnapshotObject->SetObjectField(TEXT("graph_metrics"), GraphMetricsObject);
		SnapshotObject->SetArrayField(TEXT("graphs"), GraphValues);

		TArray<TSharedPtr<FJsonValue>> VariableValues;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
			VariableObject->SetStringField(TEXT("variable_name"), Variable.VarName.ToString());
			VariableObject->SetStringField(TEXT("variable_type"), Variable.VarType.PinCategory.ToString());
			VariableObject->SetStringField(TEXT("category"), Variable.Category.ToString());
			VariableValues.Add(MakeShared<FJsonValueObject>(VariableObject));
			if (VariableValues.Num() >= 64)
			{
				break;
			}
		}
		SnapshotObject->SetArrayField(TEXT("variables"), VariableValues);

		TArray<TSharedPtr<FJsonValue>> ComponentValues;
		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node == nullptr)
				{
					continue;
				}
				TSharedPtr<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
				ComponentObject->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
				if (Node->ComponentTemplate != nullptr)
				{
					ComponentObject->SetStringField(TEXT("component_class"), Node->ComponentTemplate->GetClass()->GetPathName());
				}
				ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
				if (ComponentValues.Num() >= 64)
				{
					break;
				}
			}
		}
		SnapshotObject->SetArrayField(TEXT("components"), ComponentValues);
		return SnapshotObject;
	}

	static UWidgetBlueprint* LoadWidgetBlueprintAsset(const FString& WidgetBlueprintPath)
	{
		const FString PackagePath = NormalizeAssetPackagePath(WidgetBlueprintPath);
		return Cast<UWidgetBlueprint>(StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
	}

	static TSharedPtr<FJsonObject> BuildWidgetTreeSnapshot(const FString& WidgetBlueprintPath)
	{
		const FString NormalizedPath = NormalizeAssetPackagePath(WidgetBlueprintPath);
		if (NormalizedPath.IsEmpty())
		{
			return MakeToolErrorObject(TEXT("missing_widget_blueprint_path"), TEXT("widget_blueprint_path is required."));
		}
		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(NormalizedPath);
		if (WidgetBlueprint == nullptr)
		{
			return MakeToolErrorObject(TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint not found: %s"), *NormalizedPath));
		}

		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("widget_blueprint_path"), NormalizedPath);
		SnapshotObject->SetStringField(TEXT("widget_blueprint_name"), WidgetBlueprint->GetName());
		SnapshotObject->SetStringField(TEXT("status"), BlueprintStatusToString(WidgetBlueprint->Status));
		SnapshotObject->SetBoolField(TEXT("is_dirty"), WidgetBlueprint->GetPackage() != nullptr && WidgetBlueprint->GetPackage()->IsDirty());
		if (WidgetBlueprint->ParentClass != nullptr)
		{
			SnapshotObject->SetStringField(TEXT("parent_class"), WidgetBlueprint->ParentClass->GetPathName());
		}
		if (WidgetBlueprint->WidgetTree == nullptr)
		{
			SnapshotObject->SetStringField(TEXT("root_widget"), TEXT(""));
			SnapshotObject->SetNumberField(TEXT("widget_count"), 0);
			SnapshotObject->SetArrayField(TEXT("widgets"), TArray<TSharedPtr<FJsonValue>>());
			return SnapshotObject;
		}

		if (WidgetBlueprint->WidgetTree->RootWidget != nullptr)
		{
			SnapshotObject->SetStringField(TEXT("root_widget"), WidgetBlueprint->WidgetTree->RootWidget->GetName());
			SnapshotObject->SetStringField(TEXT("root_widget_class"), WidgetBlueprint->WidgetTree->RootWidget->GetClass()->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> WidgetValues;
		WidgetBlueprint->WidgetTree->ForEachWidget([&WidgetValues](UWidget* Widget)
		{
			if (Widget == nullptr || WidgetValues.Num() >= 128)
			{
				return;
			}
			WidgetValues.Add(MakeShared<FJsonValueObject>(BuildWidgetSnapshot(Widget)));
		});
		SnapshotObject->SetNumberField(TEXT("widget_count"), WidgetValues.Num());
		SnapshotObject->SetArrayField(TEXT("widgets"), WidgetValues);
		return SnapshotObject;
	}

	static TSharedPtr<FJsonObject> BuildWidgetDetailsSnapshot(const FString& WidgetBlueprintPath, const FString& WidgetName)
	{
		const FString NormalizedPath = NormalizeAssetPackagePath(WidgetBlueprintPath);
		const FString TargetWidgetName = WidgetName.TrimStartAndEnd();
		if (NormalizedPath.IsEmpty())
		{
			return MakeToolErrorObject(TEXT("missing_widget_blueprint_path"), TEXT("widget_blueprint_path is required."));
		}
		if (TargetWidgetName.IsEmpty())
		{
			return MakeToolErrorObject(TEXT("missing_widget_name"), TEXT("widget_name is required."));
		}

		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(NormalizedPath);
		if (WidgetBlueprint == nullptr)
		{
			return MakeToolErrorObject(TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint not found: %s"), *NormalizedPath));
		}
		if (WidgetBlueprint->WidgetTree == nullptr)
		{
			return MakeToolErrorObject(TEXT("widget_tree_missing"), FString::Printf(TEXT("Widget Tree not found: %s"), *NormalizedPath));
		}

		UWidget* MatchedWidget = nullptr;
		WidgetBlueprint->WidgetTree->ForEachWidget([&MatchedWidget, &TargetWidgetName](UWidget* Widget)
		{
			if (Widget != nullptr && MatchedWidget == nullptr && Widget->GetName().Equals(TargetWidgetName, ESearchCase::IgnoreCase))
			{
				MatchedWidget = Widget;
			}
		});
		if (MatchedWidget == nullptr)
		{
			return MakeToolErrorObject(TEXT("widget_not_found"), FString::Printf(TEXT("Widget not found: %s in %s"), *TargetWidgetName, *NormalizedPath));
		}

		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("widget_detail_schema_version"), TEXT("ue_agent_widget_details_v1"));
		SnapshotObject->SetStringField(TEXT("widget_blueprint_path"), NormalizedPath);
		SnapshotObject->SetStringField(TEXT("widget_blueprint_name"), WidgetBlueprint->GetName());
		SnapshotObject->SetStringField(TEXT("requested_widget_name"), TargetWidgetName);
		SnapshotObject->SetStringField(TEXT("widget_name"), MatchedWidget->GetName());
		SnapshotObject->SetObjectField(TEXT("widget"), BuildWidgetSnapshot(MatchedWidget));

		TArray<TSharedPtr<FJsonValue>> ChildValues;
		if (const UPanelWidget* PanelWidget = Cast<UPanelWidget>(MatchedWidget))
		{
			for (int32 Index = 0; Index < PanelWidget->GetChildrenCount(); ++Index)
			{
				if (UWidget* ChildWidget = PanelWidget->GetChildAt(Index))
				{
					ChildValues.Add(MakeShared<FJsonValueObject>(BuildWidgetSnapshot(ChildWidget)));
				}
				if (ChildValues.Num() >= 64)
				{
					break;
				}
			}
		}
		SnapshotObject->SetNumberField(TEXT("child_count"), ChildValues.Num());
		SnapshotObject->SetArrayField(TEXT("children"), ChildValues);
		if (WidgetBlueprint->WidgetTree->RootWidget != nullptr)
		{
			SnapshotObject->SetStringField(TEXT("root_widget"), WidgetBlueprint->WidgetTree->RootWidget->GetName());
		}
		return SnapshotObject;
	}

	static TSharedPtr<FJsonObject> BuildMaterialInstanceParametersSnapshot(const FString& MaterialInstancePath, const FString& ServerStatus)
	{
		FString ResolvedFrom;
		FString ResolvedPath;
		int32 SelectedAssetCount = 0;
		UMaterialInstance* MaterialInstance = ResolveMaterialInstanceAsset(
			MaterialInstancePath,
			ResolvedFrom,
			ResolvedPath,
			SelectedAssetCount);
		if (MaterialInstance == nullptr)
		{
			const FString ErrorMessage = MaterialInstancePath.TrimStartAndEnd().IsEmpty()
				? FString(TEXT("No Material Instance path was provided and no selected Content Browser Material Instance was found."))
				: FString::Printf(TEXT("Material Instance not found: %s"), *MaterialInstancePath);
			return MakeToolErrorObject(
				TEXT("material_instance_not_found"),
				ErrorMessage);
		}

		TArray<TSharedPtr<FJsonValue>> AllParameterValues;
		TArray<TSharedPtr<FJsonValue>> ScalarValues;
		TArray<TSharedPtr<FJsonValue>> VectorValues;
		TArray<TSharedPtr<FJsonValue>> TextureValues;
		TArray<TSharedPtr<FJsonValue>> StaticSwitchValues;
		AddScalarMaterialParameters(MaterialInstance, ScalarValues, AllParameterValues);
		AddVectorMaterialParameters(MaterialInstance, VectorValues, AllParameterValues);
		AddTextureMaterialParameters(MaterialInstance, TextureValues, AllParameterValues);
		AddStaticSwitchMaterialParameters(MaterialInstance, StaticSwitchValues, AllParameterValues);

		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("material_instance_schema_version"), TEXT("ue_agent_material_instance_parameters_v1"));
		SnapshotObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
		SnapshotObject->SetStringField(TEXT("server_status"), ServerStatus);
		SnapshotObject->SetStringField(TEXT("requested_material_instance_path"), MaterialInstancePath);
		SnapshotObject->SetStringField(TEXT("resolved_from"), ResolvedFrom);
		SnapshotObject->SetStringField(TEXT("material_instance_path"), !ResolvedPath.IsEmpty() ? ResolvedPath : MaterialInstance->GetPathName());
		SnapshotObject->SetStringField(TEXT("material_instance_name"), MaterialInstance->GetName());
		SnapshotObject->SetStringField(TEXT("parent_material"), MaterialInstance->Parent != nullptr ? MaterialInstance->Parent->GetPathName() : FString());
		SnapshotObject->SetNumberField(TEXT("selected_asset_count"), SelectedAssetCount);
		SnapshotObject->SetNumberField(TEXT("parameter_count"), AllParameterValues.Num());
		SnapshotObject->SetArrayField(TEXT("parameters"), AllParameterValues);
		SnapshotObject->SetArrayField(TEXT("scalar_parameters"), ScalarValues);
		SnapshotObject->SetArrayField(TEXT("vector_parameters"), VectorValues);
		SnapshotObject->SetArrayField(TEXT("texture_parameters"), TextureValues);
		SnapshotObject->SetArrayField(TEXT("static_switch_parameters"), StaticSwitchValues);
		return SnapshotObject;
	}
}

FUEAgentEditorToolServer::FUEAgentEditorToolServer() = default;

FUEAgentEditorToolServer::~FUEAgentEditorToolServer()
{
	Stop();
}

void FUEAgentEditorToolServer::StartFromConfig()
{
	bool bEnabled = false;
	FString ConfigHost = TEXT("127.0.0.1");
	int32 ConfigPort = 8765;

	if (GConfig)
	{
		GConfig->GetBool(TEXT("UEAgentTool.EditorToolServer"), TEXT("bEnabled"), bEnabled, GEngineIni);
		GConfig->GetString(TEXT("UEAgentTool.EditorToolServer"), TEXT("Host"), ConfigHost, GEngineIni);
		GConfig->GetInt(TEXT("UEAgentTool.EditorToolServer"), TEXT("Port"), ConfigPort, GEngineIni);
	}

	const FString EnvEnabled = UEAgentEditorToolServerPrivate::GetEnvVar(TEXT("UEAGENT_EDITOR_TOOL_SERVER_ENABLED"));
	if (!EnvEnabled.IsEmpty())
	{
		bEnabled = UEAgentEditorToolServerPrivate::IsTruthy(EnvEnabled);
	}

	const FString EnvHost = UEAgentEditorToolServerPrivate::GetEnvVar(TEXT("UEAGENT_EDITOR_TOOL_SERVER_HOST"));
	if (!EnvHost.IsEmpty())
	{
		ConfigHost = EnvHost;
	}

	const FString EnvPort = UEAgentEditorToolServerPrivate::GetEnvVar(TEXT("UEAGENT_EDITOR_TOOL_SERVER_PORT"));
	if (!EnvPort.IsEmpty())
	{
		ConfigPort = FCString::Atoi(*EnvPort);
	}

	if (!bEnabled)
	{
		UE_LOG(LogUEAgentEditorToolServer, Display, TEXT("UEAgent editor tool TCP server is disabled."));
		return;
	}

	Start(ConfigHost, ConfigPort);
}

bool FUEAgentEditorToolServer::Start(const FString& InHost, const int32 InPort)
{
	FScopeLock Lock(&StateLock);
	if (bRunning)
	{
		return true;
	}
	if (InPort <= 0)
	{
		UE_LOG(LogUEAgentEditorToolServer, Warning, TEXT("Invalid UEAgent editor tool TCP port: %d"), InPort);
		return false;
	}

	FIPv4Address Address;
	if (!FIPv4Address::Parse(InHost, Address))
	{
		UE_LOG(LogUEAgentEditorToolServer, Warning, TEXT("Invalid UEAgent editor tool TCP host: %s"), *InHost);
		return false;
	}

	Host = InHost;
	Port = InPort;
	const FIPv4Endpoint Endpoint(Address, Port);
	Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(100));
	Listener->OnConnectionAccepted().BindRaw(this, &FUEAgentEditorToolServer::HandleConnectionAccepted);
	if (!Listener->Init())
	{
		Listener.Reset();
		UE_LOG(LogUEAgentEditorToolServer, Warning, TEXT("Failed to start UEAgent editor tool TCP server on %s:%d."), *Host, Port);
		return false;
	}

	bRunning = true;
	UE_LOG(LogUEAgentEditorToolServer, Display, TEXT("UEAgent editor tool TCP server listening on %s:%d."), *Host, Port);
	return true;
}

void FUEAgentEditorToolServer::Stop()
{
	FScopeLock Lock(&StateLock);
	if (Listener.IsValid())
	{
		Listener->Stop();
		Listener.Reset();
	}
	bRunning = false;
}

bool FUEAgentEditorToolServer::IsRunning() const
{
	FScopeLock Lock(&StateLock);
	return bRunning;
}

FString FUEAgentEditorToolServer::GetStatusText() const
{
	FScopeLock Lock(&StateLock);
	return bRunning ? FString::Printf(TEXT("listening:%s:%d"), *Host, Port) : TEXT("disabled");
}

bool FUEAgentEditorToolServer::HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint)
{
	if (ClientSocket == nullptr)
	{
		return false;
	}
	HandleClient(ClientSocket, ClientEndpoint.ToString());
	return true;
}

void FUEAgentEditorToolServer::HandleClient(FSocket* ClientSocket, const FString& ClientLabel) const
{
	FString PendingText;
	TArray<uint8> Buffer;
	bool bKeepReading = true;

	while (bKeepReading && ClientSocket != nullptr && ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(UEAgentEditorToolServerPrivate::SocketWaitSeconds)))
	{
		uint32 PendingDataSize = 0;
		while (ClientSocket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
		{
			const int32 BytesToRead = static_cast<int32>(FMath::Min<uint32>(PendingDataSize, UEAgentEditorToolServerPrivate::MaxReadChunkBytes));
			Buffer.SetNumUninitialized(BytesToRead);
			int32 BytesRead = 0;
			if (!ClientSocket->Recv(Buffer.GetData(), Buffer.Num(), BytesRead, ESocketReceiveFlags::None) || BytesRead <= 0)
			{
				bKeepReading = false;
				break;
			}

			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), BytesRead);
			PendingText.AppendChars(Converted.Get(), Converted.Length());

			FString Line;
			while (PendingText.Split(TEXT("\n"), &Line, &PendingText, ESearchCase::CaseSensitive, ESearchDir::FromStart))
			{
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty())
				{
					continue;
				}
				const FString ResponseLine = HandleJsonRpcLine(Line);
				if (!ResponseLine.IsEmpty() && !SendLine(ClientSocket, ResponseLine))
				{
					bKeepReading = false;
					break;
				}
			}
		}
	}

	PendingText.TrimStartAndEndInline();
	if (!PendingText.IsEmpty())
	{
		const FString ResponseLine = HandleJsonRpcLine(PendingText);
		if (!ResponseLine.IsEmpty())
		{
			SendLine(ClientSocket, ResponseLine);
		}
	}

	UE_LOG(LogUEAgentEditorToolServer, Verbose, TEXT("UEAgent editor tool TCP client disconnected: %s"), *ClientLabel);
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
}

FString FUEAgentEditorToolServer::HandleJsonRpcLine(const FString& RequestLine) const
{
	const TSharedPtr<FJsonObject> RequestObject = ParseJsonObject(RequestLine);
	if (!RequestObject.IsValid())
	{
		return MakeJsonRpcError(nullptr, -32700, TEXT("Parse error"), TEXT("invalid_json"));
	}

	FString Method;
	RequestObject->TryGetStringField(TEXT("method"), Method);
	const bool bHasRequestId = RequestObject->HasField(TEXT("id"));
	if (Method.Equals(TEXT("notifications/initialized"), ESearchCase::IgnoreCase) && !bHasRequestId)
	{
		return FString();
	}
	if (Method.Equals(TEXT("initialize"), ESearchCase::IgnoreCase))
	{
		return MakeJsonRpcResponse(RequestObject, BuildInitializeResult());
	}
	if (Method.Equals(TEXT("tools/list"), ESearchCase::IgnoreCase))
	{
		return MakeJsonRpcResponse(RequestObject, BuildToolsListResult());
	}
	if (Method.Equals(TEXT("tools/call"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ParamsField = nullptr;
		const TSharedPtr<FJsonObject> ParamsObject = RequestObject->TryGetObjectField(TEXT("params"), ParamsField) && ParamsField != nullptr ? *ParamsField : nullptr;
		return MakeJsonRpcResponse(RequestObject, BuildToolCallResult(ParamsObject));
	}
	if (!bHasRequestId)
	{
		return FString();
	}
	return MakeJsonRpcError(RequestObject, -32601, TEXT("Method not found"), TEXT("unsupported_method"));
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildInitializeResult() const
{
	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetStringField(TEXT("protocolVersion"), TEXT("2024-11-05"));

	TSharedPtr<FJsonObject> CapabilitiesObject = MakeShared<FJsonObject>();
	CapabilitiesObject->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
	ResultObject->SetObjectField(TEXT("capabilities"), CapabilitiesObject);

	TSharedPtr<FJsonObject> ServerInfoObject = MakeShared<FJsonObject>();
	ServerInfoObject->SetStringField(TEXT("name"), TEXT("UEAgentTool.EditorToolServer"));
	ServerInfoObject->SetStringField(TEXT("version"), TEXT("0.1.0"));
	ResultObject->SetObjectField(TEXT("serverInfo"), ServerInfoObject);

	TSharedPtr<FJsonObject> UEAgentObject = MakeShared<FJsonObject>();
	UEAgentObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
	UEAgentObject->SetStringField(TEXT("status"), GetStatusText());
	UEAgentObject->SetBoolField(TEXT("http_proposal_flow_required_for_writes"), true);
	ResultObject->SetObjectField(TEXT("ue_agent"), UEAgentObject);
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildToolsListResult() const
{
	TArray<TSharedPtr<FJsonValue>> ToolValues;
	ToolValues.Add(MakeShared<FJsonValueObject>(UEAgentEditorToolServerPrivate::MakeReadOnlyCatalogToolObject()));
	for (const FUEAgentEditorToolDefinition& Definition : FUEAgentEditorToolCatalog::BuildCoreEditorOperationDefinitions())
	{
		ToolValues.Add(MakeShared<FJsonValueObject>(UEAgentEditorToolServerPrivate::MakeMcpToolObject(Definition)));
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetArrayField(TEXT("tools"), ToolValues);
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildToolCallResult(const TSharedPtr<FJsonObject>& ParamsObject) const
{
	FString ToolName;
	if (ParamsObject.IsValid())
	{
		ParamsObject->TryGetStringField(TEXT("name"), ToolName);
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));

	if (ToolName.Equals(TEXT("ue_agent_tools_list"), ESearchCase::IgnoreCase))
	{
		TextContent->SetStringField(TEXT("text"), SerializeJsonObject(FUEAgentEditorToolCatalog::BuildToolsListJson()));
		ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
		ResultObject->SetArrayField(TEXT("content"), ContentValues);
		ResultObject->SetObjectField(TEXT("structuredContent"), FUEAgentEditorToolCatalog::BuildToolsListJson());
		return ResultObject;
	}
	if (ToolName.Equals(TEXT("get_editor_context"), ESearchCase::IgnoreCase))
	{
		return BuildEditorContextResult();
	}
	if (ToolName.Equals(TEXT("get_selected_assets"), ESearchCase::IgnoreCase))
	{
		return BuildSelectedAssetsResult();
	}
	if (ToolName.Equals(TEXT("get_static_mesh_details"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ArgumentsField = nullptr;
		const TSharedPtr<FJsonObject> ArgumentsObject = ParamsObject.IsValid() && ParamsObject->TryGetObjectField(TEXT("arguments"), ArgumentsField) && ArgumentsField != nullptr ? *ArgumentsField : nullptr;
		FString StaticMeshPathOrQuery;
		if (ArgumentsObject.IsValid())
		{
			ArgumentsObject->TryGetStringField(TEXT("static_mesh_path"), StaticMeshPathOrQuery);
			if (StaticMeshPathOrQuery.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("asset_path"), StaticMeshPathOrQuery);
			}
			if (StaticMeshPathOrQuery.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("query"), StaticMeshPathOrQuery);
			}
		}
		return BuildStaticMeshDetailsResult(StaticMeshPathOrQuery);
	}
	if (ToolName.Equals(TEXT("get_selected_actors"), ESearchCase::IgnoreCase))
	{
		return BuildSelectedActorsResult();
	}
	if (ToolName.Equals(TEXT("get_level_actors"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ArgumentsField = nullptr;
		const TSharedPtr<FJsonObject> ArgumentsObject = ParamsObject.IsValid() && ParamsObject->TryGetObjectField(TEXT("arguments"), ArgumentsField) && ArgumentsField != nullptr ? *ArgumentsField : nullptr;
		return BuildLevelActorsResult(ArgumentsObject);
	}
	if (ToolName.Equals(TEXT("get_blueprint_graph"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ArgumentsField = nullptr;
		const TSharedPtr<FJsonObject> ArgumentsObject = ParamsObject.IsValid() && ParamsObject->TryGetObjectField(TEXT("arguments"), ArgumentsField) && ArgumentsField != nullptr ? *ArgumentsField : nullptr;
		FString BlueprintPath;
		if (ArgumentsObject.IsValid())
		{
			ArgumentsObject->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
		}
		return BuildBlueprintGraphResult(BlueprintPath);
	}
	if (ToolName.Equals(TEXT("get_widget_tree"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ArgumentsField = nullptr;
		const TSharedPtr<FJsonObject> ArgumentsObject = ParamsObject.IsValid() && ParamsObject->TryGetObjectField(TEXT("arguments"), ArgumentsField) && ArgumentsField != nullptr ? *ArgumentsField : nullptr;
		FString WidgetBlueprintPath;
		if (ArgumentsObject.IsValid())
		{
			ArgumentsObject->TryGetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
			if (WidgetBlueprintPath.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("blueprint_path"), WidgetBlueprintPath);
			}
		}
		return BuildWidgetTreeResult(WidgetBlueprintPath);
	}
	if (ToolName.Equals(TEXT("get_widget_details"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ArgumentsField = nullptr;
		const TSharedPtr<FJsonObject> ArgumentsObject = ParamsObject.IsValid() && ParamsObject->TryGetObjectField(TEXT("arguments"), ArgumentsField) && ArgumentsField != nullptr ? *ArgumentsField : nullptr;
		FString WidgetBlueprintPath;
		FString WidgetName;
		if (ArgumentsObject.IsValid())
		{
			ArgumentsObject->TryGetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
			if (WidgetBlueprintPath.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("blueprint_path"), WidgetBlueprintPath);
			}
			ArgumentsObject->TryGetStringField(TEXT("widget_name"), WidgetName);
			if (WidgetName.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("target_widget"), WidgetName);
			}
			if (WidgetName.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("query"), WidgetName);
			}
		}
		return BuildWidgetDetailsResult(WidgetBlueprintPath, WidgetName);
	}
	if (ToolName.Equals(TEXT("get_material_instance_parameters"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ArgumentsField = nullptr;
		const TSharedPtr<FJsonObject> ArgumentsObject = ParamsObject.IsValid() && ParamsObject->TryGetObjectField(TEXT("arguments"), ArgumentsField) && ArgumentsField != nullptr ? *ArgumentsField : nullptr;
		FString MaterialInstancePath;
		if (ArgumentsObject.IsValid())
		{
			ArgumentsObject->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath);
			if (MaterialInstancePath.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("asset_path"), MaterialInstancePath);
			}
			if (MaterialInstancePath.IsEmpty())
			{
				ArgumentsObject->TryGetStringField(TEXT("query"), MaterialInstancePath);
			}
		}
		return BuildMaterialInstanceParametersResult(MaterialInstancePath);
	}

	ResultObject->SetBoolField(TEXT("isError"), true);
	TextContent->SetStringField(TEXT("text"), FString::Printf(TEXT("Tool '%s' requires the existing HTTP Proposal confirmation flow and cannot be executed through raw MCP/TCP."), *ToolName));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildEditorContextResult() const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	const FString ServerStatus = GetStatusText();
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildEditorContextSnapshot(ServerStatus);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [ServerStatus, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildEditorContextSnapshot(ServerStatus);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading live editor context on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}


TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildSelectedAssetsResult() const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	const FString ServerStatus = GetStatusText();
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildSelectedAssetsSnapshot(ServerStatus);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [ServerStatus, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildSelectedAssetsSnapshot(ServerStatus);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading selected assets on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildStaticMeshDetailsResult(const FString& StaticMeshPathOrQuery) const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	const FString ServerStatus = GetStatusText();
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildStaticMeshDetailsSnapshot(StaticMeshPathOrQuery, ServerStatus);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [StaticMeshPathOrQuery, ServerStatus, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildStaticMeshDetailsSnapshot(StaticMeshPathOrQuery, ServerStatus);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading Static Mesh details on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildSelectedActorsResult() const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	const FString ServerStatus = GetStatusText();
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildSelectedActorsSnapshot(ServerStatus);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [ServerStatus, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildSelectedActorsSnapshot(ServerStatus);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading selected actors on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildLevelActorsResult(const TSharedPtr<FJsonObject>& ArgumentsObject) const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	const FString ServerStatus = GetStatusText();
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildLevelActorsSnapshot(ServerStatus, ArgumentsObject);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [ServerStatus, ArgumentsObject, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildLevelActorsSnapshot(ServerStatus, ArgumentsObject);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading level actors on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildBlueprintGraphResult(const FString& BlueprintPath) const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildBlueprintGraphSnapshot(BlueprintPath);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [BlueprintPath, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildBlueprintGraphSnapshot(BlueprintPath);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading Blueprint graph metadata on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildWidgetTreeResult(const FString& WidgetBlueprintPath) const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildWidgetTreeSnapshot(WidgetBlueprintPath);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [WidgetBlueprintPath, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildWidgetTreeSnapshot(WidgetBlueprintPath);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading Widget Tree metadata on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildWidgetDetailsResult(const FString& WidgetBlueprintPath, const FString& WidgetName) const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildWidgetDetailsSnapshot(WidgetBlueprintPath, WidgetName);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [WidgetBlueprintPath, WidgetName, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildWidgetDetailsSnapshot(WidgetBlueprintPath, WidgetName);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading Widget details on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildMaterialInstanceParametersResult(const FString& MaterialInstancePath) const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	const FString ServerStatus = GetStatusText();
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildMaterialInstanceParametersSnapshot(MaterialInstancePath, ServerStatus);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [MaterialInstancePath, ServerStatus, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildMaterialInstanceParametersSnapshot(MaterialInstancePath, ServerStatus);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading Material Instance parameters on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}
TSharedPtr<FJsonObject> FUEAgentEditorToolServer::ParseJsonObject(const FString& Text)
{
	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, JsonObject) ? JsonObject : nullptr;
}

FString FUEAgentEditorToolServer::SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
{
	if (!JsonObject.IsValid())
	{
		return TEXT("{}");
	}
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return Output;
}

FString FUEAgentEditorToolServer::MakeJsonRpcResponse(const TSharedPtr<FJsonObject>& RequestObject, const TSharedPtr<FJsonObject>& ResultObject)
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	TSharedPtr<FJsonValue> IdValue = MakeShared<FJsonValueNull>();
	if (RequestObject.IsValid() && RequestObject->HasField(TEXT("id")))
	{
		IdValue = RequestObject->TryGetField(TEXT("id"));
	}
	if (!IdValue.IsValid())
	{
		IdValue = MakeShared<FJsonValueNull>();
	}
	RootObject->SetField(TEXT("id"), IdValue);
	TSharedPtr<FJsonObject> SafeResultObject = ResultObject;
	if (!SafeResultObject.IsValid())
	{
		SafeResultObject = MakeShared<FJsonObject>();
	}
	RootObject->SetObjectField(TEXT("result"), SafeResultObject);
	return SerializeJsonObject(RootObject);
}

FString FUEAgentEditorToolServer::MakeJsonRpcError(const TSharedPtr<FJsonObject>& RequestObject, const int32 Code, const FString& Message, const FString& Reason)
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	TSharedPtr<FJsonValue> IdValue = MakeShared<FJsonValueNull>();
	if (RequestObject.IsValid() && RequestObject->HasField(TEXT("id")))
	{
		IdValue = RequestObject->TryGetField(TEXT("id"));
	}
	if (!IdValue.IsValid())
	{
		IdValue = MakeShared<FJsonValueNull>();
	}
	RootObject->SetField(TEXT("id"), IdValue);

	TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
	ErrorObject->SetNumberField(TEXT("code"), Code);
	ErrorObject->SetStringField(TEXT("message"), Message);
	TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
	DataObject->SetStringField(TEXT("reason"), Reason);
	ErrorObject->SetObjectField(TEXT("data"), DataObject);
	RootObject->SetObjectField(TEXT("error"), ErrorObject);
	return SerializeJsonObject(RootObject);
}

bool FUEAgentEditorToolServer::SendLine(FSocket* ClientSocket, const FString& Line)
{
	if (ClientSocket == nullptr)
	{
		return false;
	}
	const FString Payload = Line + TEXT("\n");
	const FTCHARToUTF8 Converted(*Payload);
	int32 BytesSent = 0;
	return ClientSocket->Send(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length(), BytesSent) && BytesSent == Converted.Length();
}
