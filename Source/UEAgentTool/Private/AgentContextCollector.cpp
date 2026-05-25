// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentContextCollector.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/MaterialInstance.h"
#include "MaterialShared.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"

namespace UEAgentContextCollectorPrivate
{
	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values, const int32 MaxItems = INDEX_NONE)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		const int32 Limit = MaxItems == INDEX_NONE ? Values.Num() : FMath::Min(Values.Num(), MaxItems);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			if (!Values[Index].IsEmpty())
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Values[Index]));
			}
		}
		return JsonValues;
	}

	static FString ToRelativeProjectPath(const FString& AbsolutePath, const FString& ProjectRoot)
	{
		FString RelativePath = FPaths::ConvertRelativePathToFull(AbsolutePath);
		FPaths::NormalizeFilename(RelativePath);
		FString NormalizedProjectRoot = FPaths::ConvertRelativePathToFull(ProjectRoot);
		FPaths::NormalizeFilename(NormalizedProjectRoot);
		FPaths::MakePathRelativeTo(RelativePath, *NormalizedProjectRoot);
		FPaths::NormalizeFilename(RelativePath);
		return RelativePath;
	}

	static void AddRegistryTags(const FAssetData& AssetData, const TSharedPtr<FJsonObject>& PropertiesObject)
	{
		TSharedPtr<FJsonObject> RegistryTagsObject = MakeShared<FJsonObject>();
		int32 TagCount = 0;
		AssetData.EnumerateTags([&RegistryTagsObject, &TagCount](const TPair<FName, FAssetTagValueRef>& Pair)
		{
			if (TagCount >= 48)
			{
				return;
			}

			RegistryTagsObject->SetStringField(Pair.Key.ToString(), Pair.Value.AsString());
			++TagCount;
		});

		if (TagCount > 0)
		{
			PropertiesObject->SetObjectField(TEXT("registry_tags"), RegistryTagsObject);
		}
	}

	static FString GetTagValue(const FAssetData& AssetData, const TCHAR* TagName)
	{
		return AssetData.GetTagValueRef<FString>(FName(TagName));
	}

	static void AddTagIfPresent(const FAssetData& AssetData, const TSharedPtr<FJsonObject>& TargetObject, const TCHAR* FieldName, const TCHAR* TagName)
	{
		const FString Value = GetTagValue(AssetData, TagName);
		if (!Value.IsEmpty())
		{
			TargetObject->SetStringField(FieldName, Value);
		}
	}

	static void AddCommonAssetSummaries(const FAssetData& AssetData, const TSharedPtr<FJsonObject>& SettingsObject, const TSharedPtr<FJsonObject>& PropertiesObject)
	{
		AddTagIfPresent(AssetData, SettingsObject, TEXT("nanite_enabled"), TEXT("NaniteEnabled"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("lod_group"), TEXT("LODGroup"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("compression_settings"), TEXT("CompressionSettings"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("srgb"), TEXT("SRGB"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("mip_gen_settings"), TEXT("MipGenSettings"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("texture_group"), TEXT("TextureGroup"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("blend_mode"), TEXT("BlendMode"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("shading_model"), TEXT("ShadingModel"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("parent_class"), TEXT("ParentClass"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("generated_class"), TEXT("GeneratedClass"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("skeleton"), TEXT("Skeleton"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("physics_asset"), TEXT("PhysicsAsset"));

		AddTagIfPresent(AssetData, PropertiesObject, TEXT("lod_count"), TEXT("LODCount"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("num_lods"), TEXT("NumLODs"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("triangle_count"), TEXT("NumTriangles"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("vertex_count"), TEXT("VertexCount"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("material_slots"), TEXT("MaterialSlotNames"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("row_struct"), TEXT("RowStruct"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("row_count"), TEXT("RowCount"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("duration"), TEXT("Duration"));
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

	static FString PinTypeToInventoryString(const FEdGraphPinType& PinType)
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
			TypeText = FString::Printf(TEXT("array<%s>"), *TypeText);
		}
		else if (PinType.IsSet())
		{
			TypeText = FString::Printf(TEXT("set<%s>"), *TypeText);
		}
		else if (PinType.IsMap())
		{
			TypeText = FString::Printf(TEXT("map<%s>"), *TypeText);
		}
		return TypeText;
	}

	static void AddGraphNames(const TArray<TObjectPtr<UEdGraph>>& Graphs, TArray<FString>& OutNames)
	{
		for (const TObjectPtr<UEdGraph>& Graph : Graphs)
		{
			if (Graph != nullptr)
			{
				OutNames.AddUnique(Graph->GetName());
			}
		}
	}

	static FString GraphPinDirectionToString(const EEdGraphPinDirection Direction)
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

	static TSharedPtr<FJsonObject> MakeGraphNodeInventoryObject(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		if (Node == nullptr)
		{
			return NodeObject;
		}

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
		int32 LinkCount = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr)
			{
				continue;
			}
			if (Pin->Direction == EGPD_Input)
			{
				++InputPinCount;
			}
			else if (Pin->Direction == EGPD_Output)
			{
				++OutputPinCount;
			}
			LinkCount += Pin->LinkedTo.Num();

			TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
			PinObject->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
			PinObject->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
			PinObject->SetStringField(TEXT("direction"), GraphPinDirectionToString(Pin->Direction));
			PinObject->SetStringField(TEXT("pin_type"), PinTypeToInventoryString(Pin->PinType));
			PinObject->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
			PinValues.Add(MakeShared<FJsonValueObject>(PinObject));
			if (PinValues.Num() >= 16)
			{
				break;
			}
		}

		NodeObject->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
		NodeObject->SetNumberField(TEXT("input_pin_count"), InputPinCount);
		NodeObject->SetNumberField(TEXT("output_pin_count"), OutputPinCount);
		NodeObject->SetNumberField(TEXT("link_count"), LinkCount);
		NodeObject->SetArrayField(TEXT("pins"), PinValues);
		return NodeObject;
	}

	static void AddGraphInventorySummaries(
		const TArray<TObjectPtr<UEdGraph>>& Graphs,
		const FString& GraphType,
		TArray<TSharedPtr<FJsonValue>>& OutGraphValues)
	{
		for (const TObjectPtr<UEdGraph>& Graph : Graphs)
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
			int32 PinCount = 0;
			int32 LinkCount = 0;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node == nullptr)
				{
					continue;
				}
				PinCount += Node->Pins.Num();
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin != nullptr)
					{
						LinkCount += Pin->LinkedTo.Num();
					}
				}
				NodeValues.Add(MakeShared<FJsonValueObject>(MakeGraphNodeInventoryObject(Node)));
				if (NodeValues.Num() >= 32)
				{
					break;
				}
			}

			GraphObject->SetNumberField(TEXT("pin_count"), PinCount);
			GraphObject->SetNumberField(TEXT("link_count"), LinkCount);
			GraphObject->SetNumberField(TEXT("max_nodes_returned"), 32);
			GraphObject->SetNumberField(TEXT("max_pins_per_node"), 16);
			GraphObject->SetArrayField(TEXT("nodes"), NodeValues);
			OutGraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
			if (OutGraphValues.Num() >= 32)
			{
				break;
			}
		}
	}

	static void AddStaticMeshInventoryDetails(const UStaticMesh* StaticMesh, const TSharedPtr<FJsonObject>& SettingsObject)
	{
		if (StaticMesh == nullptr)
		{
			return;
		}

#if WITH_EDITORONLY_DATA
		SettingsObject->SetBoolField(TEXT("nanite_enabled"), StaticMesh->NaniteSettings.bEnabled);
#endif
		SettingsObject->SetNumberField(TEXT("lod_count"), StaticMesh->GetNumLODs());
		SettingsObject->SetNumberField(TEXT("lightmap_resolution"), StaticMesh->GetLightMapResolution());

		if (const UBodySetup* BodySetup = StaticMesh->GetBodySetup())
		{
			SettingsObject->SetStringField(TEXT("collision_complexity"), CollisionTraceFlagToString(BodySetup->CollisionTraceFlag));
		}
	}

	static void AddBlueprintInventoryDetails(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& AssetObject)
	{
		if (Blueprint == nullptr)
		{
			return;
		}

		TSharedPtr<FJsonObject> BlueprintObject = MakeShared<FJsonObject>();
		if (Blueprint->ParentClass != nullptr)
		{
			BlueprintObject->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> ComponentValues;
		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			const TArray<USCS_Node*>& Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
			for (const USCS_Node* Node : Nodes)
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

				if (const USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(const_cast<USCS_Node*>(Node)))
				{
					ComponentObject->SetStringField(TEXT("attach_to"), ParentNode->GetVariableName().ToString());
				}

				ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
				if (ComponentValues.Num() >= 64)
				{
					break;
				}
			}
		}
		BlueprintObject->SetArrayField(TEXT("components"), ComponentValues);

		TArray<TSharedPtr<FJsonValue>> VariableValues;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
			VariableObject->SetStringField(TEXT("variable_name"), Variable.VarName.ToString());
			VariableObject->SetStringField(TEXT("variable_type"), PinTypeToInventoryString(Variable.VarType));
			VariableObject->SetStringField(TEXT("category"), Variable.Category.ToString());
			if (!Variable.DefaultValue.IsEmpty())
			{
				VariableObject->SetStringField(TEXT("default_value"), Variable.DefaultValue);
			}
			VariableValues.Add(MakeShared<FJsonValueObject>(VariableObject));
			if (VariableValues.Num() >= 64)
			{
				break;
			}
		}
		BlueprintObject->SetArrayField(TEXT("variables"), VariableValues);

#if WITH_EDITORONLY_DATA
		TArray<FString> FunctionNames;
		TArray<FString> GraphNames;
		TArray<TSharedPtr<FJsonValue>> GraphSummaryValues;
		AddGraphNames(Blueprint->FunctionGraphs, FunctionNames);
		AddGraphNames(Blueprint->UbergraphPages, GraphNames);
		AddGraphNames(Blueprint->FunctionGraphs, GraphNames);
		AddGraphNames(Blueprint->MacroGraphs, GraphNames);
		AddGraphInventorySummaries(Blueprint->UbergraphPages, TEXT("event"), GraphSummaryValues);
		AddGraphInventorySummaries(Blueprint->FunctionGraphs, TEXT("function"), GraphSummaryValues);
		AddGraphInventorySummaries(Blueprint->MacroGraphs, TEXT("macro"), GraphSummaryValues);
		BlueprintObject->SetArrayField(TEXT("functions"), MakeStringArray(FunctionNames, 64));
		BlueprintObject->SetArrayField(TEXT("graphs"), MakeStringArray(GraphNames, 96));
		BlueprintObject->SetArrayField(TEXT("graph_summaries"), GraphSummaryValues);

		TArray<FString> InterfaceNames;
		for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDescription.Interface != nullptr)
			{
				InterfaceNames.Add(InterfaceDescription.Interface->GetPathName());
			}
		}
		BlueprintObject->SetArrayField(TEXT("interfaces"), MakeStringArray(InterfaceNames, 32));
#endif

		TSharedPtr<FJsonObject> EditorFlagsObject = MakeShared<FJsonObject>();
		EditorFlagsObject->SetStringField(TEXT("status"), BlueprintStatusToString(Blueprint->Status));
		EditorFlagsObject->SetBoolField(TEXT("is_dirty"), Blueprint->GetPackage() != nullptr && Blueprint->GetPackage()->IsDirty());
		EditorFlagsObject->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint));
		EditorFlagsObject->SetBoolField(TEXT("force_full_editor"), Blueprint->bForceFullEditor);
		EditorFlagsObject->SetBoolField(TEXT("is_newly_created"), Blueprint->bIsNewlyCreated);
		BlueprintObject->SetObjectField(TEXT("editor_flags"), EditorFlagsObject);

		AssetObject->SetObjectField(TEXT("blueprint"), BlueprintObject);
	}

	static FString MobilityToString(const EComponentMobility::Type Mobility)
	{
		switch (Mobility)
		{
		case EComponentMobility::Static:
			return TEXT("static");
		case EComponentMobility::Stationary:
			return TEXT("stationary");
		case EComponentMobility::Movable:
			return TEXT("movable");
		default:
			return TEXT("unknown");
		}
	}

	static TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Vector)
	{
		TSharedPtr<FJsonObject> VectorObject = MakeShared<FJsonObject>();
		VectorObject->SetNumberField(TEXT("x"), Vector.X);
		VectorObject->SetNumberField(TEXT("y"), Vector.Y);
		VectorObject->SetNumberField(TEXT("z"), Vector.Z);
		return VectorObject;
	}

	static TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Rotator)
	{
		TSharedPtr<FJsonObject> RotatorObject = MakeShared<FJsonObject>();
		RotatorObject->SetNumberField(TEXT("pitch"), Rotator.Pitch);
		RotatorObject->SetNumberField(TEXT("yaw"), Rotator.Yaw);
		RotatorObject->SetNumberField(TEXT("roll"), Rotator.Roll);
		return RotatorObject;
	}

	static TSharedPtr<FJsonObject> MakeTransformObject(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> TransformObject = MakeShared<FJsonObject>();
		TransformObject->SetObjectField(TEXT("location"), MakeVectorObject(Transform.GetLocation()));
		TransformObject->SetObjectField(TEXT("rotation"), MakeRotatorObject(Transform.Rotator()));
		TransformObject->SetObjectField(TEXT("scale"), MakeVectorObject(Transform.GetScale3D()));
		return TransformObject;
	}

	static TSharedPtr<FJsonObject> MakeActorInventoryObject(const AActor* Actor)
	{
		if (Actor == nullptr)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
		ActorObject->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		ActorObject->SetStringField(TEXT("actor_name"), Actor->GetName());
		ActorObject->SetStringField(TEXT("actor_class"), Actor->GetClass() != nullptr ? Actor->GetClass()->GetPathName() : FString());
		ActorObject->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		ActorObject->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		ActorObject->SetBoolField(TEXT("hidden_in_game"), Actor->IsHidden());
		ActorObject->SetObjectField(TEXT("transform"), MakeTransformObject(Actor->GetActorTransform()));

		if (const ULevel* Level = Actor->GetLevel())
		{
			const FString LevelName = Level->GetOutermost() != nullptr ? Level->GetOutermost()->GetName() : Level->GetName();
			ActorObject->SetStringField(TEXT("level_name"), LevelName);
		}

		if (const UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(Actor->GetClass()))
		{
			if (BlueprintClass->ClassGeneratedBy != nullptr)
			{
				ActorObject->SetStringField(TEXT("blueprint_path"), BlueprintClass->ClassGeneratedBy->GetPathName());
			}
		}

		if (const USceneComponent* RootComponent = Actor->GetRootComponent())
		{
			ActorObject->SetStringField(TEXT("mobility"), MobilityToString(RootComponent->Mobility));
		}

		TArray<FString> TagStrings;
		for (const FName& Tag : Actor->Tags)
		{
			TagStrings.Add(Tag.ToString());
			if (TagStrings.Num() >= 32)
			{
				break;
			}
		}
		ActorObject->SetArrayField(TEXT("tags"), MakeStringArray(TagStrings, 32));

		TArray<TSharedPtr<FJsonValue>> ComponentValues;
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (const UActorComponent* Component : Components)
		{
			if (Component == nullptr)
			{
				continue;
			}

			TSharedPtr<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
			ComponentObject->SetStringField(TEXT("component_name"), Component->GetName());
			ComponentObject->SetStringField(TEXT("component_class"), Component->GetClass() != nullptr ? Component->GetClass()->GetPathName() : FString());
			if (const USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				ComponentObject->SetStringField(TEXT("mobility"), MobilityToString(SceneComponent->Mobility));
			}
			ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
			if (ComponentValues.Num() >= 64)
			{
				break;
			}
		}
		ActorObject->SetArrayField(TEXT("components"), ComponentValues);
		return ActorObject;
	}

	static TSharedPtr<FJsonObject> MakeColorObject(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> ColorObject = MakeShared<FJsonObject>();
		ColorObject->SetNumberField(TEXT("r"), Color.R);
		ColorObject->SetNumberField(TEXT("g"), Color.G);
		ColorObject->SetNumberField(TEXT("b"), Color.B);
		ColorObject->SetNumberField(TEXT("a"), Color.A);
		return ColorObject;
	}

	static TSharedPtr<FJsonObject> MakeParameterObject(const FMaterialParameterInfo& ParameterInfo, const FString& ParameterType)
	{
		TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
		ParameterObject->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
		ParameterObject->SetStringField(TEXT("parameter_name"), ParameterInfo.Name.ToString());
		ParameterObject->SetStringField(TEXT("parameter_type"), ParameterType);
		return ParameterObject;
	}

	static void AddScalarMaterialParameters(const UMaterialInstance* MaterialInstance, TArray<TSharedPtr<FJsonValue>>& ParameterValues, TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllScalarParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			float Value = 0.0f;
			TSharedPtr<FJsonObject> ParameterObject = MakeParameterObject(ParameterInfo, TEXT("scalar"));
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

	static void AddVectorMaterialParameters(const UMaterialInstance* MaterialInstance, TArray<TSharedPtr<FJsonValue>>& ParameterValues, TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllVectorParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			FLinearColor Value = FLinearColor::White;
			TSharedPtr<FJsonObject> ParameterObject = MakeParameterObject(ParameterInfo, TEXT("vector"));
			if (MaterialInstance->GetVectorParameterValue(ParameterInfo, Value))
			{
				ParameterObject->SetObjectField(TEXT("value"), MakeColorObject(Value));
			}
			ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			AllParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			if (ParameterValues.Num() >= 128)
			{
				break;
			}
		}
	}

	static void AddTextureMaterialParameters(const UMaterialInstance* MaterialInstance, TArray<TSharedPtr<FJsonValue>>& ParameterValues, TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllTextureParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			UTexture* Value = nullptr;
			TSharedPtr<FJsonObject> ParameterObject = MakeParameterObject(ParameterInfo, TEXT("texture"));
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

	static void AddStaticSwitchMaterialParameters(const UMaterialInstance* MaterialInstance, TArray<TSharedPtr<FJsonValue>>& ParameterValues, TArray<TSharedPtr<FJsonValue>>& AllParameterValues)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;
		MaterialInstance->GetAllStaticSwitchParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
		{
			bool bValue = false;
			FGuid ExpressionGuid;
			TSharedPtr<FJsonObject> ParameterObject = MakeParameterObject(ParameterInfo, TEXT("static_switch"));
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

	static TSharedPtr<FJsonObject> MakeMaterialInstanceInventoryObject(const FAssetData& AssetData, const UMaterialInstance* MaterialInstance)
	{
		if (MaterialInstance == nullptr)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> MaterialObject = MakeShared<FJsonObject>();
		MaterialObject->SetStringField(TEXT("material_instance_path"), AssetData.GetSoftObjectPath().ToString());
		MaterialObject->SetStringField(TEXT("material_instance_name"), AssetData.AssetName.ToString());
		MaterialObject->SetStringField(TEXT("parent_material"), MaterialInstance->Parent != nullptr ? MaterialInstance->Parent->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> AllParameterValues;
		TArray<TSharedPtr<FJsonValue>> ScalarValues;
		TArray<TSharedPtr<FJsonValue>> VectorValues;
		TArray<TSharedPtr<FJsonValue>> TextureValues;
		TArray<TSharedPtr<FJsonValue>> StaticSwitchValues;
		AddScalarMaterialParameters(MaterialInstance, ScalarValues, AllParameterValues);
		AddVectorMaterialParameters(MaterialInstance, VectorValues, AllParameterValues);
		AddTextureMaterialParameters(MaterialInstance, TextureValues, AllParameterValues);
		AddStaticSwitchMaterialParameters(MaterialInstance, StaticSwitchValues, AllParameterValues);

		MaterialObject->SetArrayField(TEXT("parameters"), AllParameterValues);
		MaterialObject->SetArrayField(TEXT("scalar_parameters"), ScalarValues);
		MaterialObject->SetArrayField(TEXT("vector_parameters"), VectorValues);
		MaterialObject->SetArrayField(TEXT("texture_parameters"), TextureValues);
		MaterialObject->SetArrayField(TEXT("static_switch_parameters"), StaticSwitchValues);
		return MaterialObject;
	}

	static bool IsValidSymbolToken(const FString& Token)
	{
		if (Token.IsEmpty() || Token.Contains(TEXT("(")) || Token.Contains(TEXT("=")))
		{
			return false;
		}

		const FString LowerToken = Token.ToLower();
		return LowerToken != TEXT("final")
			&& LowerToken != TEXT("public")
			&& LowerToken != TEXT("private")
			&& LowerToken != TEXT("protected")
			&& LowerToken != TEXT("virtual");
	}

	static void AddSymbolFromKeyword(const FString& Line, const FString& Keyword, TArray<FString>& Symbols)
	{
		int32 KeywordIndex = INDEX_NONE;
		if (!Line.FindChar(Keyword[0], KeywordIndex))
		{
			return;
		}

		KeywordIndex = Line.Find(Keyword, ESearchCase::CaseSensitive);
		if (KeywordIndex == INDEX_NONE)
		{
			return;
		}

		FString Tail = Line.Mid(KeywordIndex + Keyword.Len()).TrimStartAndEnd();
		Tail.ReplaceInline(TEXT("{"), TEXT(" "));
		Tail.ReplaceInline(TEXT(":"), TEXT(" "));
		Tail.ReplaceInline(TEXT(";"), TEXT(" "));
		Tail.ReplaceInline(TEXT(","), TEXT(" "));

		TArray<FString> Tokens;
		Tail.ParseIntoArrayWS(Tokens);
		for (FString Token : Tokens)
		{
			Token.TrimStartAndEndInline();
			if (Token.EndsWith(TEXT("_API")) || !IsValidSymbolToken(Token))
			{
				continue;
			}
			Symbols.AddUnique(Token);
			return;
		}
	}
}

FUEAgentContextSummary FUEAgentContextCollector::CaptureContext() const
{
	FUEAgentContextSummary Context;
	Context.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Context.ProjectName = FApp::GetProjectName();
	Context.ActiveModule = DeriveActiveModule(Context.ProjectName);
	Context.BackendVersion = TEXT("unknown");
	Context.KnowledgeBaseStatus = TEXT("Unknown");

	const FString SourceRoot = FPaths::ProjectDir() / TEXT("Source");
	if (IFileManager::Get().DirectoryExists(*SourceRoot))
	{
		TArray<FString> BuildFiles;
		IFileManager::Get().FindFilesRecursive(BuildFiles, *SourceRoot, TEXT("*.Build.cs"), true, false);
		if (BuildFiles.Num() > 0)
		{
			Context.CurrentFile = FPaths::ConvertRelativePathToFull(BuildFiles[0]);
		}
	}

	if (!Context.CurrentFile.IsEmpty())
	{
		Context.RecentOpenFiles.Add(Context.CurrentFile);
	}

	TArray<FAssetData> SelectedAssets;
	if (FModuleManager::Get().ModuleExists(TEXT("ContentBrowser")))
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
	}

	IAssetRegistry* AssetRegistry = nullptr;
	if (FModuleManager::Get().ModuleExists(TEXT("AssetRegistry")))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistry = &AssetRegistryModule.Get();
	}

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (AssetData.IsValid())
		{
			FUEAgentAssetContextItem AssetItem;
			AssetItem.AssetName = AssetData.AssetName.ToString();
			AssetItem.AssetPath = AssetData.GetSoftObjectPath().ToString();
			AssetItem.AssetType = AssetData.AssetClassPath.GetAssetName().ToString();
			AssetItem.PackagePath = AssetData.PackageName.ToString();
			Context.SelectedAssets.Add(AssetItem.AssetPath);

			if (AssetRegistry != nullptr)
			{
				TArray<FName> DependencyNames;
				AssetRegistry->GetDependencies(AssetData.PackageName, DependencyNames);
				for (int32 Index = 0; Index < DependencyNames.Num() && Index < 32; ++Index)
				{
					AssetItem.Dependencies.Add(DependencyNames[Index].ToString());
				}

				TArray<FName> ReferencerNames;
				AssetRegistry->GetReferencers(AssetData.PackageName, ReferencerNames);
				for (int32 Index = 0; Index < ReferencerNames.Num() && Index < 32; ++Index)
				{
					AssetItem.Referencers.Add(ReferencerNames[Index].ToString());
				}
			}

			Context.SelectedAssetItems.Add(AssetItem);
		}
	}

	return Context;
}

FString FUEAgentContextCollector::DeriveActiveModule(const FString& ProjectName) const
{
	const FString SourceRoot = FPaths::ProjectDir() / TEXT("Source");
	if (!IFileManager::Get().DirectoryExists(*SourceRoot))
	{
		return ProjectName;
	}

	TArray<FString> ModuleBuildFiles;
	IFileManager::Get().FindFilesRecursive(ModuleBuildFiles, *SourceRoot, TEXT("*.Build.cs"), true, false);
	if (ModuleBuildFiles.Num() == 0)
	{
		return ProjectName;
	}

	return FPaths::GetBaseFilename(ModuleBuildFiles[0]).Replace(TEXT(".Build"), TEXT(""));
}

FString FUEAgentContextCollector::DeriveModuleFromRelativePath(const FString& RelativePath, const FString& ProjectName) const
{
	TArray<FString> PathParts;
	RelativePath.ParseIntoArray(PathParts, TEXT("/"), true);

	if (PathParts.Num() > 1 && PathParts[0].Equals(TEXT("Source"), ESearchCase::IgnoreCase))
	{
		return PathParts[1];
	}

	if (PathParts.Num() > 1 && PathParts[0].Equals(TEXT("Plugins"), ESearchCase::IgnoreCase))
	{
		const int32 SourceIndex = PathParts.Find(FString(TEXT("Source")));
		if (SourceIndex != INDEX_NONE && PathParts.IsValidIndex(SourceIndex + 1))
		{
			return PathParts[SourceIndex + 1];
		}
		return PathParts[1];
	}

	return ProjectName;
}

TArray<FString> FUEAgentContextCollector::ExtractCodeSymbols(const FString& AbsoluteFilePath) const
{
	FString SourceText;
	const FFileStatData StatData = IFileManager::Get().GetStatData(*AbsoluteFilePath);
	if (StatData.FileSize > 1024 * 1024 || !FFileHelper::LoadFileToString(SourceText, *AbsoluteFilePath))
	{
		return {};
	}

	TArray<FString> Symbols;
	TArray<FString> Lines;
	SourceText.ParseIntoArrayLines(Lines, false);
	for (FString Line : Lines)
	{
		int32 CommentIndex = INDEX_NONE;
		if (Line.FindChar(TEXT('/'), CommentIndex))
		{
			const int32 DoubleSlashIndex = Line.Find(TEXT("//"), ESearchCase::CaseSensitive);
			if (DoubleSlashIndex != INDEX_NONE)
			{
				Line = Line.Left(DoubleSlashIndex);
			}
		}

		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("//")))
		{
			continue;
		}

		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("enum class "), Symbols);
		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("class "), Symbols);
		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("struct "), Symbols);
		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("interface "), Symbols);

		if (Symbols.Num() >= 32)
		{
			break;
		}
	}

	if (Symbols.Num() > 32)
	{
		Symbols.SetNum(32);
	}
	return Symbols;
}

TSharedPtr<FJsonObject> FUEAgentContextCollector::BuildProjectInventorySnapshot(const int32 MaxAssets, const int32 MaxCodeFiles) const
{
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString ProjectName = FApp::GetProjectName();

	TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
	SnapshotObject->SetStringField(TEXT("project_id"), ProjectName);
	SnapshotObject->SetStringField(TEXT("project_name"), ProjectName);
	SnapshotObject->SetStringField(TEXT("source"), TEXT("ue_plugin"));
	SnapshotObject->SetStringField(TEXT("snapshot_time"), FDateTime::UtcNow().ToIso8601());

	TSharedPtr<FJsonObject> ScanDiagnosticsObject = MakeShared<FJsonObject>();
	ScanDiagnosticsObject->SetStringField(TEXT("project_root"), ProjectRoot);
	ScanDiagnosticsObject->SetNumberField(TEXT("max_assets"), MaxAssets);
	ScanDiagnosticsObject->SetNumberField(TEXT("max_code_files"), MaxCodeFiles);

	TArray<TSharedPtr<FJsonValue>> AssetValues;
	TArray<TSharedPtr<FJsonValue>> MaterialInstanceValues;
	if (FModuleManager::Get().ModuleExists(TEXT("AssetRegistry")))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> ProjectAssets;
		AssetRegistry.GetAssetsByPath(FName(TEXT("/Game")), ProjectAssets, true);
		ProjectAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.PackageName.ToString() < Right.PackageName.ToString();
		});

		for (const FAssetData& AssetData : ProjectAssets)
		{
			if (!AssetData.IsValid() || (MaxAssets > 0 && AssetValues.Num() >= MaxAssets))
			{
				continue;
			}

			TArray<FName> DependencyNames;
			AssetRegistry.GetDependencies(AssetData.PackageName, DependencyNames);
			TArray<FString> Dependencies;
			for (const FName& DependencyName : DependencyNames)
			{
				Dependencies.Add(DependencyName.ToString());
			}

			TArray<FName> ReferencerNames;
			AssetRegistry.GetReferencers(AssetData.PackageName, ReferencerNames);
			TArray<FString> Referencers;
			for (const FName& ReferencerName : ReferencerNames)
			{
				Referencers.Add(ReferencerName.ToString());
			}

			TSharedPtr<FJsonObject> SettingsObject = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UEAgentContextCollectorPrivate::AddCommonAssetSummaries(AssetData, SettingsObject, PropertiesObject);
			UEAgentContextCollectorPrivate::AddRegistryTags(AssetData, PropertiesObject);

			TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
			AssetObject->SetStringField(TEXT("asset_path"), AssetData.GetSoftObjectPath().ToString());
			AssetObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
			const FString AssetType = AssetData.AssetClassPath.GetAssetName().ToString();
			AssetObject->SetStringField(TEXT("asset_type"), AssetType);
			AssetObject->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
			AssetObject->SetArrayField(TEXT("dependencies"), UEAgentContextCollectorPrivate::MakeStringArray(Dependencies, 64));
			AssetObject->SetArrayField(TEXT("referencers"), UEAgentContextCollectorPrivate::MakeStringArray(Referencers, 64));

			const bool bIsStaticMesh = AssetType.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase);
			const bool bIsBlueprint = AssetType.Equals(TEXT("Blueprint"), ESearchCase::IgnoreCase) || AssetType.Contains(TEXT("Blueprint"));
			const bool bIsMaterialInstance = AssetType.Contains(TEXT("MaterialInstance"), ESearchCase::IgnoreCase);
			if (bIsStaticMesh || bIsBlueprint || bIsMaterialInstance)
			{
				UObject* LoadedAsset = AssetData.GetAsset();
				UEAgentContextCollectorPrivate::AddStaticMeshInventoryDetails(Cast<UStaticMesh>(LoadedAsset), SettingsObject);
				UEAgentContextCollectorPrivate::AddBlueprintInventoryDetails(Cast<UBlueprint>(LoadedAsset), AssetObject);
				if (bIsMaterialInstance)
				{
					if (const UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(LoadedAsset))
					{
						TSharedPtr<FJsonObject> MaterialObject = UEAgentContextCollectorPrivate::MakeMaterialInstanceInventoryObject(AssetData, MaterialInstance);
						if (MaterialObject.IsValid())
						{
							MaterialInstanceValues.Add(MakeShared<FJsonValueObject>(MaterialObject));
						}
					}
				}
			}

			AssetObject->SetObjectField(TEXT("settings"), SettingsObject);
			AssetObject->SetObjectField(TEXT("properties"), PropertiesObject);
			AssetValues.Add(MakeShared<FJsonValueObject>(AssetObject));
		}

		ScanDiagnosticsObject->SetNumberField(TEXT("asset_registry_count"), ProjectAssets.Num());
		ScanDiagnosticsObject->SetBoolField(TEXT("asset_limit_reached"), MaxAssets > 0 && ProjectAssets.Num() > MaxAssets);
		ScanDiagnosticsObject->SetNumberField(TEXT("material_instance_scan_count"), MaterialInstanceValues.Num());
	}
	else
	{
		ScanDiagnosticsObject->SetStringField(TEXT("asset_scan_status"), TEXT("asset_registry_unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> CodeFileValues;
	TArray<FString> CandidateFiles;
	const TArray<FString> SourceRoots = { ProjectRoot / TEXT("Source"), ProjectRoot / TEXT("Plugins") };
	const TArray<FString> Patterns = { TEXT("*.h"), TEXT("*.hpp"), TEXT("*.hh"), TEXT("*.inl"), TEXT("*.cpp"), TEXT("*.cxx"), TEXT("*.cs") };
	for (const FString& SourceRoot : SourceRoots)
	{
		if (!IFileManager::Get().DirectoryExists(*SourceRoot))
		{
			continue;
		}

		for (const FString& Pattern : Patterns)
		{
			IFileManager::Get().FindFilesRecursive(CandidateFiles, *SourceRoot, *Pattern, true, false);
		}
	}

	CandidateFiles.Sort();
	for (const FString& CandidateFile : CandidateFiles)
	{
		if (MaxCodeFiles > 0 && CodeFileValues.Num() >= MaxCodeFiles)
		{
			break;
		}

		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(CandidateFile);
		const FString RelativePath = UEAgentContextCollectorPrivate::ToRelativeProjectPath(AbsolutePath, ProjectRoot);
		const FFileStatData StatData = IFileManager::Get().GetStatData(*AbsolutePath);

		TSharedPtr<FJsonObject> CodeFileObject = MakeShared<FJsonObject>();
		CodeFileObject->SetStringField(TEXT("file_path"), RelativePath);
		CodeFileObject->SetStringField(TEXT("module_name"), DeriveModuleFromRelativePath(RelativePath, ProjectName));
		CodeFileObject->SetStringField(TEXT("file_type"), FPaths::GetExtension(RelativePath).ToLower());
		CodeFileObject->SetNumberField(TEXT("size_bytes"), StatData.FileSize);
		CodeFileObject->SetStringField(TEXT("last_modified"), StatData.ModificationTime.ToIso8601());
		CodeFileObject->SetStringField(TEXT("modified_at"), StatData.ModificationTime.ToIso8601());
		const TArray<FString> Symbols = ExtractCodeSymbols(AbsolutePath);
		CodeFileObject->SetArrayField(TEXT("classes"), UEAgentContextCollectorPrivate::MakeStringArray(Symbols, 32));
		CodeFileObject->SetArrayField(TEXT("symbols"), UEAgentContextCollectorPrivate::MakeStringArray(Symbols, 32));
		CodeFileValues.Add(MakeShared<FJsonValueObject>(CodeFileObject));
	}

	ScanDiagnosticsObject->SetNumberField(TEXT("code_file_scan_count"), CandidateFiles.Num());
	ScanDiagnosticsObject->SetBoolField(TEXT("code_file_limit_reached"), MaxCodeFiles > 0 && CandidateFiles.Num() > MaxCodeFiles);

	TArray<TSharedPtr<FJsonValue>> LevelActorValues;
	if (GEditor != nullptr)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			for (ULevel* Level : EditorWorld->GetLevels())
			{
				if (Level == nullptr)
				{
					continue;
				}

				for (const TObjectPtr<AActor>& ActorPtr : Level->Actors)
				{
					if (LevelActorValues.Num() >= 1000)
					{
						break;
					}
					AActor* Actor = ActorPtr.Get();
					if (!IsValid(Actor))
					{
						continue;
					}

					TSharedPtr<FJsonObject> ActorObject = UEAgentContextCollectorPrivate::MakeActorInventoryObject(Actor);
					if (ActorObject.IsValid())
					{
						LevelActorValues.Add(MakeShared<FJsonValueObject>(ActorObject));
					}
				}

				if (LevelActorValues.Num() >= 1000)
				{
					break;
				}
			}
			ScanDiagnosticsObject->SetNumberField(TEXT("level_actor_scan_count"), LevelActorValues.Num());
			ScanDiagnosticsObject->SetBoolField(TEXT("level_actor_limit_reached"), LevelActorValues.Num() >= 1000);
		}
		else
		{
			ScanDiagnosticsObject->SetStringField(TEXT("level_actor_scan_status"), TEXT("editor_world_unavailable"));
		}
	}
	else
	{
		ScanDiagnosticsObject->SetStringField(TEXT("level_actor_scan_status"), TEXT("editor_unavailable"));
	}

	SnapshotObject->SetArrayField(TEXT("assets"), AssetValues);
	SnapshotObject->SetArrayField(TEXT("code_files"), CodeFileValues);
	SnapshotObject->SetArrayField(TEXT("level_actors"), LevelActorValues);
	SnapshotObject->SetArrayField(TEXT("material_instances"), MaterialInstanceValues);
	SnapshotObject->SetObjectField(TEXT("scan_diagnostics"), ScanDiagnosticsObject);
	return SnapshotObject;
}
