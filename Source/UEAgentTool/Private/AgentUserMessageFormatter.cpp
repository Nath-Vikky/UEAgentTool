// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentUserMessageFormatter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{

	static FString GetScalarFieldAsString(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
	{
		if (!JsonObject.IsValid())
		{
			return FString();
		}

		const TSharedPtr<FJsonValue> FieldValue = JsonObject->TryGetField(FieldName);
		if (!FieldValue.IsValid())
		{
			return FString();
		}

		switch (FieldValue->Type)
		{
		case EJson::String:
			return FieldValue->AsString();
		case EJson::Number:
			return FString::SanitizeFloat(FieldValue->AsNumber());
		case EJson::Boolean:
			return FieldValue->AsBool() ? TEXT("true") : TEXT("false");
		default:
			return FString();
		}
	}



	static bool GetBoolFieldOrDefault(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, const bool bDefaultValue)
	{
		if (!JsonObject.IsValid())
		{
			return bDefaultValue;
		}

		bool bValue = bDefaultValue;
		return JsonObject->TryGetBoolField(FieldName, bValue) ? bValue : bDefaultValue;
	}



	static TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
	{
		if (!JsonObject.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* ObjectField = nullptr;
		return JsonObject->TryGetObjectField(FieldName, ObjectField) && ObjectField != nullptr ? *ObjectField : nullptr;
	}



	static bool TryGetIntegerField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, int32& OutValue)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		double NumberValue = 0.0;
		if (!JsonObject->TryGetNumberField(FieldName, NumberValue))
		{
			return false;
		}

		OutValue = static_cast<int32>(NumberValue);
		return true;
	}



	static int32 GetIntegerFieldOrZero(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
	{
		int32 Value = 0;
		TryGetIntegerField(JsonObject, FieldName, Value);
		return Value;
	}



	static FString FormatScalarObjectPreview(const TSharedPtr<FJsonObject>& JsonObject, const FString& EmptyText, const int32 MaxItems = 6)
	{
		if (!JsonObject.IsValid() || JsonObject->Values.Num() == 0)
		{
			return EmptyText;
		}

		TArray<FString> Parts;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonObject->Values)
		{
			if (!Pair.Value.IsValid())
			{
				continue;
			}

			const FString Value = GetScalarFieldAsString(JsonObject, *Pair.Key);
			if (!Value.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *Value));
			}

			if (Parts.Num() >= MaxItems)
			{
				break;
			}
		}

		return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : EmptyText;
	}


	static FString FirstNonEmptyString(const TSharedPtr<FJsonObject>& JsonObject, const TArray<const TCHAR*>& FieldNames)
	{
		for (const TCHAR* FieldName : FieldNames)
		{
			const FString Value = GetScalarFieldAsString(JsonObject, FieldName).TrimStartAndEnd();
			if (!Value.IsEmpty())
			{
				return Value;
			}
		}

		return FString();
	}

	static TArray<FString> GetStringArrayField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
	{
		TArray<FString> Result;
		if (!JsonObject.IsValid())
		{
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayField = nullptr;
		if (!JsonObject->TryGetArrayField(FieldName, ArrayField) || ArrayField == nullptr)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Value : *ArrayField)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString StringValue;
			if (Value->Type == EJson::String)
			{
				StringValue = Value->AsString();
			}
			else if (Value->Type == EJson::Number)
			{
				StringValue = FString::SanitizeFloat(Value->AsNumber());
			}
			else if (Value->Type == EJson::Boolean)
			{
				StringValue = Value->AsBool() ? TEXT("true") : TEXT("false");
			}

			StringValue.TrimStartAndEndInline();
			if (!StringValue.IsEmpty())
			{
				Result.AddUnique(StringValue);
			}
		}
		return Result;
	}

	static FString JoinPreviewItems(const TArray<FString>& Values, const int32 MaxItems)
	{
		if (Values.Num() == 0)
		{
			return TEXT("none");
		}

		TArray<FString> PreviewValues;
		for (int32 Index = 0; Index < Values.Num() && Index < MaxItems; ++Index)
		{
			PreviewValues.Add(Values[Index]);
		}
		FString Preview = FString::Join(PreviewValues, TEXT(", "));
		if (Values.Num() > MaxItems)
		{
			Preview += FString::Printf(TEXT(" ... +%d more"), Values.Num() - MaxItems);
		}
		return Preview;
	}



	static FString GetObjectArrayNamePreview(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, const TArray<const TCHAR*>& NameFields, const int32 MaxItems)
	{
		if (!JsonObject.IsValid())
		{
			return TEXT("none");
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if (!JsonObject->TryGetArrayField(FieldName, ArrayValues) || ArrayValues == nullptr)
		{
			return TEXT("none");
		}

		TArray<FString> Names;
		for (const TSharedPtr<FJsonValue>& Value : *ArrayValues)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			FString Name;
			if (Value->Type == EJson::String)
			{
				Name = Value->AsString();
			}
			else if (Value->Type == EJson::Object)
			{
				Name = FirstNonEmptyString(Value->AsObject(), NameFields);
			}
			Name.TrimStartAndEndInline();
			if (!Name.IsEmpty())
			{
				Names.Add(Name);
			}
		}
		return JoinPreviewItems(Names, MaxItems);
	}



	static FString BuildMaterialParameterPreview(const TSharedPtr<FJsonObject>& ItemObject, const int32 MaxItems)
	{
		if (!ItemObject.IsValid())
		{
			return TEXT("none");
		}

		const TArray<TSharedPtr<FJsonValue>>* ParameterValues = nullptr;
		if (!ItemObject->TryGetArrayField(TEXT("parameters"), ParameterValues) || ParameterValues == nullptr || ParameterValues->Num() == 0)
		{
			return TEXT("none");
		}

		TArray<FString> Previews;
		for (const TSharedPtr<FJsonValue>& ParameterValue : *ParameterValues)
		{
			const TSharedPtr<FJsonObject> ParameterObject = ParameterValue.IsValid() ? ParameterValue->AsObject() : nullptr;
			if (!ParameterObject.IsValid())
			{
				continue;
			}

			const FString ParameterName = FirstNonEmptyString(ParameterObject, { TEXT("parameter_name"), TEXT("name"), TEXT("ParameterName") });
			const FString ParameterType = FirstNonEmptyString(ParameterObject, { TEXT("parameter_type"), TEXT("type"), TEXT("ParameterType") });
			FString ParameterValueText = FirstNonEmptyString(ParameterObject, {
				TEXT("value"), TEXT("scalar_value"), TEXT("texture_path"), TEXT("texture_name"), TEXT("default_value"), TEXT("bool_value")
			});
			if (ParameterValueText.IsEmpty())
			{
				ParameterValueText = FormatScalarObjectPreview(GetObjectField(ParameterObject, TEXT("value")), TEXT(""), 4);
			}
			FString Preview = ParameterName.IsEmpty() ? FString(TEXT("parameter")) : ParameterName;
			if (!ParameterType.IsEmpty())
			{
				Preview += FString::Printf(TEXT("<%s>"), *ParameterType);
			}
			if (!ParameterValueText.IsEmpty())
			{
				Preview += FString::Printf(TEXT("=%s"), *ParameterValueText);
			}
			Previews.Add(Preview);
		}
		return JoinPreviewItems(Previews, MaxItems);
	}

}

bool FUEAgentUserMessageFormatter::BuildProjectInventorySummaryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus)
	{
		const TSharedPtr<FJsonObject> SummaryObject = GetObjectField(ResponseObject, TEXT("summary"));
		if (!SummaryObject.IsValid())
		{
			OutMessage = TEXT("Project Inventory summary response did not include a summary object.");
			OutStatus = TEXT("Project Inventory summary is unavailable.");
			return false;
		}

		const bool bHasSnapshot = GetBoolFieldOrDefault(SummaryObject, TEXT("has_snapshot"), false);
		if (!bHasSnapshot)
		{
			OutMessage = TEXT("No Project Inventory snapshot is available yet. Click Sync Inventory Now, then ask current-project questions again.");
			OutStatus = TEXT("Project Inventory summary is empty.");
			return false;
		}

		const FString ProjectName = GetScalarFieldAsString(SummaryObject, TEXT("project_name"));
		const FString SnapshotId = GetScalarFieldAsString(SummaryObject, TEXT("snapshot_id"));
		const FString CreatedAt = GetScalarFieldAsString(SummaryObject, TEXT("created_at"));

		TArray<FString> Lines;
		Lines.Add(TEXT("Project Inventory Summary"));
		Lines.Add(FString::Printf(TEXT("- project: %s"), *(ProjectName.IsEmpty() ? FString(TEXT("Unknown")) : ProjectName)));
		Lines.Add(FString::Printf(TEXT("- snapshot: %s"), *(SnapshotId.IsEmpty() ? FString(TEXT("latest")) : SnapshotId)));
		if (!CreatedAt.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("- created_at: %s"), *CreatedAt));
		}
		Lines.Add(FString::Printf(TEXT("- assets: %d  |  blueprints: %d  |  static_meshes: %d  |  maps: %d"),
			GetIntegerFieldOrZero(SummaryObject, TEXT("asset_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("blueprint_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("static_mesh_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("map_count"))));
		Lines.Add(FString::Printf(TEXT("- code_files: %d  |  level_actors: %d  |  material_instances: %d  |  material_parameters: %d"),
			GetIntegerFieldOrZero(SummaryObject, TEXT("code_file_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("level_actor_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("material_instance_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("material_parameter_count"))));

		Lines.Add(FString::Printf(TEXT("- asset_types: %s"), *FormatScalarObjectPreview(GetObjectField(SummaryObject, TEXT("asset_type_counts")), TEXT("none"))));
		Lines.Add(FString::Printf(TEXT("- blueprint_parents: %s"), *FormatScalarObjectPreview(GetObjectField(SummaryObject, TEXT("blueprint_parent_class_counts")), TEXT("none"))));
		Lines.Add(FString::Printf(TEXT("- levels: %s"), *FormatScalarObjectPreview(GetObjectField(SummaryObject, TEXT("level_actor_level_counts")), TEXT("none"))));

		const TSharedPtr<FJsonObject> DiagnosticsObject = GetObjectField(SummaryObject, TEXT("scan_diagnostics"));
		if (DiagnosticsObject.IsValid() && DiagnosticsObject->Values.Num() > 0)
		{
			Lines.Add(FString::Printf(TEXT("- scan_diagnostics: %s"), *FormatScalarObjectPreview(DiagnosticsObject, TEXT("available"))));
		}

		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = TEXT("Loaded Project Inventory summary.");
		return true;
	}


bool FUEAgentUserMessageFormatter::BuildAssetInventoryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus)
	{
		const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
		if (!ResponseObject.IsValid() || !ResponseObject->TryGetArrayField(TEXT("items"), ItemValues) || ItemValues == nullptr)
		{
			OutMessage = TEXT("Asset inventory response did not include an items array.");
			OutStatus = TEXT("Asset inventory is unavailable.");
			return false;
		}

		const TSharedPtr<FJsonObject> InspectionObject = GetObjectField(ResponseObject, TEXT("inspection"));
		const TSharedPtr<FJsonObject> SummaryObject = GetObjectField(ResponseObject, TEXT("summary"));
		const FString EmptyReason = GetScalarFieldAsString(InspectionObject, TEXT("empty_reason"));
		const int32 AssetCount = GetIntegerFieldOrZero(SummaryObject, TEXT("asset_count"));

		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("Project Asset Inventory (%d shown, %d total)"), ItemValues->Num(), AssetCount));
		int32 DisplayedCount = 0;
		for (const TSharedPtr<FJsonValue>& ItemValue : *ItemValues)
		{
			const TSharedPtr<FJsonObject> ItemObject = ItemValue.IsValid() ? ItemValue->AsObject() : nullptr;
			if (!ItemObject.IsValid())
			{
				continue;
			}

			const FString AssetName = FirstNonEmptyString(ItemObject, { TEXT("asset_name"), TEXT("asset_id"), TEXT("asset_path") });
			const FString AssetType = GetScalarFieldAsString(ItemObject, TEXT("asset_type"));
			const FString PackagePath = GetScalarFieldAsString(ItemObject, TEXT("package_path"));
			const FString AssetPath = GetScalarFieldAsString(ItemObject, TEXT("asset_path"));
			Lines.Add(FString::Printf(TEXT("- %s [%s] %s"),
				*(AssetName.IsEmpty() ? FString(TEXT("UnnamedAsset")) : AssetName),
				*(AssetType.IsEmpty() ? FString(TEXT("Unknown")) : AssetType),
				*(PackagePath.IsEmpty() ? AssetPath : PackagePath)));

			++DisplayedCount;
			if (DisplayedCount >= 15)
			{
				break;
			}
		}

		if (DisplayedCount == 0)
		{
			Lines.Add(EmptyReason.IsEmpty()
				? TEXT("No assets were returned. Sync Project Inventory first, then try again.")
				: FString::Printf(TEXT("No assets were returned: %s"), *EmptyReason));
		}
		else if (ItemValues->Num() > DisplayedCount)
		{
			Lines.Add(FString::Printf(TEXT("...and %d more assets. Ask Agent Chat for a filtered list if needed."), ItemValues->Num() - DisplayedCount));
		}

		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = FString::Printf(TEXT("Loaded %d Project Inventory assets."), ItemValues->Num());
		return DisplayedCount > 0;
	}

bool FUEAgentUserMessageFormatter::BuildAssetDetailMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus)
	{
		const TSharedPtr<FJsonObject> ItemObject = GetObjectField(ResponseObject, TEXT("item"));
		const TSharedPtr<FJsonObject> InspectionObject = GetObjectField(ResponseObject, TEXT("inspection"));
		if (!ItemObject.IsValid() || ItemObject->Values.Num() == 0)
		{
			const FString EmptyReason = GetScalarFieldAsString(InspectionObject, TEXT("empty_reason"));
			OutMessage = EmptyReason.IsEmpty()
				? TEXT("Selected asset detail was not found in Project Inventory. Sync Inventory Now, then try again.")
				: FString::Printf(TEXT("Selected asset detail was not found: %s"), *EmptyReason);
			OutStatus = TEXT("Selected asset detail unavailable.");
			return false;
		}

		const FString AssetName = FirstNonEmptyString(ItemObject, { TEXT("asset_name"), TEXT("asset_id"), TEXT("asset_path") });
		const FString AssetType = GetScalarFieldAsString(ItemObject, TEXT("asset_type"));
		const FString AssetPath = GetScalarFieldAsString(ItemObject, TEXT("asset_path"));
		const FString PackagePath = GetScalarFieldAsString(ItemObject, TEXT("package_path"));
		const TArray<FString> Dependencies = GetStringArrayField(ItemObject, TEXT("dependencies"));
		const TArray<FString> Referencers = GetStringArrayField(ItemObject, TEXT("referencers"));
		const TSharedPtr<FJsonObject> SettingsObject = GetObjectField(ItemObject, TEXT("settings"));
		const TSharedPtr<FJsonObject> PropertiesObject = GetObjectField(ItemObject, TEXT("properties"));

		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("Selected Asset Detail: %s"), *(AssetName.IsEmpty() ? FString(TEXT("UnnamedAsset")) : AssetName)));
		Lines.Add(FString::Printf(TEXT("- type: %s"), *(AssetType.IsEmpty() ? FString(TEXT("Unknown")) : AssetType)));
		if (!AssetPath.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("- asset_path: %s"), *AssetPath));
		}
		if (!PackagePath.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("- package_path: %s"), *PackagePath));
		}
		Lines.Add(FString::Printf(TEXT("- dependencies: %s"), *JoinPreviewItems(Dependencies, 6)));
		Lines.Add(FString::Printf(TEXT("- referencers: %s"), *JoinPreviewItems(Referencers, 6)));
		Lines.Add(FString::Printf(TEXT("- settings: %s"), *FormatScalarObjectPreview(SettingsObject, TEXT("none"), 8)));
		Lines.Add(FString::Printf(TEXT("- properties: %s"), *FormatScalarObjectPreview(PropertiesObject, TEXT("none"), 8)));

		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = FString::Printf(TEXT("Loaded selected asset detail: %s."), *(AssetName.IsEmpty() ? FString(TEXT("asset")) : AssetName));
		return true;
	}


bool FUEAgentUserMessageFormatter::BuildLevelActorsInventoryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus)
	{
		const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
		if (!ResponseObject.IsValid() || !ResponseObject->TryGetArrayField(TEXT("items"), ItemValues) || ItemValues == nullptr)
		{
			OutMessage = TEXT("Level actor inventory response did not include an items array.");
			OutStatus = TEXT("Level actor inventory is unavailable.");
			return false;
		}

		const TSharedPtr<FJsonObject> InspectionObject = GetObjectField(ResponseObject, TEXT("inspection"));
		const TSharedPtr<FJsonObject> SummaryObject = GetObjectField(ResponseObject, TEXT("summary"));
		const FString EmptyReason = GetScalarFieldAsString(InspectionObject, TEXT("empty_reason"));
		const int32 TotalActorCount = GetIntegerFieldOrZero(SummaryObject, TEXT("level_actor_count"));

		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("Level Actor Inventory (%d shown, %d total)"), ItemValues->Num(), TotalActorCount));
		int32 DisplayedCount = 0;
		for (const TSharedPtr<FJsonValue>& ItemValue : *ItemValues)
		{
			const TSharedPtr<FJsonObject> ItemObject = ItemValue.IsValid() ? ItemValue->AsObject() : nullptr;
			if (!ItemObject.IsValid())
			{
				continue;
			}

			const FString ActorLabel = FirstNonEmptyString(ItemObject, { TEXT("actor_label"), TEXT("actor_name"), TEXT("actor_path") });
			const FString ActorClass = GetScalarFieldAsString(ItemObject, TEXT("actor_class"));
			const FString LevelName = GetScalarFieldAsString(ItemObject, TEXT("level_name"));
			const FString FolderPath = GetScalarFieldAsString(ItemObject, TEXT("folder_path"));
			const FString Mobility = GetScalarFieldAsString(ItemObject, TEXT("mobility"));
			const TSharedPtr<FJsonObject> TransformObject = GetObjectField(ItemObject, TEXT("transform"));
			const FString LocationPreview = FormatScalarObjectPreview(GetObjectField(TransformObject, TEXT("location")), TEXT("unknown"), 3);
			const TArray<FString> Tags = GetStringArrayField(ItemObject, TEXT("tags"));
			const FString ComponentsPreview = GetObjectArrayNamePreview(ItemObject, TEXT("components"), { TEXT("component_name"), TEXT("name"), TEXT("component_class") }, 4);

			Lines.Add(FString::Printf(TEXT("- %s [%s] level=%s loc=%s"),
				*(ActorLabel.IsEmpty() ? FString(TEXT("UnnamedActor")) : ActorLabel),
				*(ActorClass.IsEmpty() ? FString(TEXT("UnknownClass")) : ActorClass),
				*(LevelName.IsEmpty() ? FString(TEXT("UnknownLevel")) : LevelName),
				*LocationPreview));
			if (!FolderPath.IsEmpty() || !Mobility.IsEmpty() || Tags.Num() > 0 || ComponentsPreview != TEXT("none"))
			{
				Lines.Add(FString::Printf(TEXT("  folder=%s mobility=%s tags=%s components=%s"),
					*(FolderPath.IsEmpty() ? FString(TEXT("none")) : FolderPath),
					*(Mobility.IsEmpty() ? FString(TEXT("unknown")) : Mobility),
					*JoinPreviewItems(Tags, 4),
					*ComponentsPreview));
			}

			++DisplayedCount;
			if (DisplayedCount >= 15)
			{
				break;
			}
		}

		if (DisplayedCount == 0)
		{
			Lines.Add(EmptyReason.IsEmpty()
				? TEXT("No level actors were returned. Sync Project Inventory while a level is open, then try again.")
				: FString::Printf(TEXT("No level actors were returned: %s"), *EmptyReason));
		}
		else if (ItemValues->Num() > DisplayedCount)
		{
			Lines.Add(FString::Printf(TEXT("...and %d more actors. Ask Agent Chat for a filtered list if needed."), ItemValues->Num() - DisplayedCount));
		}

		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = FString::Printf(TEXT("Loaded %d Level Actor inventory items."), ItemValues->Num());
		return DisplayedCount > 0;
	}


bool FUEAgentUserMessageFormatter::BuildMaterialInstancesInventoryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus)
	{
		const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
		if (!ResponseObject.IsValid() || !ResponseObject->TryGetArrayField(TEXT("items"), ItemValues) || ItemValues == nullptr)
		{
			OutMessage = TEXT("Material Instance inventory response did not include an items array.");
			OutStatus = TEXT("Material Instance inventory is unavailable.");
			return false;
		}

		const TSharedPtr<FJsonObject> InspectionObject = GetObjectField(ResponseObject, TEXT("inspection"));
		const TSharedPtr<FJsonObject> SummaryObject = GetObjectField(ResponseObject, TEXT("summary"));
		const FString EmptyReason = GetScalarFieldAsString(InspectionObject, TEXT("empty_reason"));
		const int32 TotalMaterialCount = GetIntegerFieldOrZero(SummaryObject, TEXT("material_instance_count"));

		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("Material Instance Inventory (%d shown, %d total)"), ItemValues->Num(), TotalMaterialCount));
		int32 DisplayedCount = 0;
		for (const TSharedPtr<FJsonValue>& ItemValue : *ItemValues)
		{
			const TSharedPtr<FJsonObject> ItemObject = ItemValue.IsValid() ? ItemValue->AsObject() : nullptr;
			if (!ItemObject.IsValid())
			{
				continue;
			}

			const FString MaterialName = FirstNonEmptyString(ItemObject, { TEXT("material_instance_name"), TEXT("asset_name"), TEXT("material_instance_path") });
			const FString MaterialPath = GetScalarFieldAsString(ItemObject, TEXT("material_instance_path"));
			const FString ParentMaterial = GetScalarFieldAsString(ItemObject, TEXT("parent_material"));
			const FString ParameterCount = GetScalarFieldAsString(ItemObject, TEXT("parameter_count"));
			const FString ParameterPreview = BuildMaterialParameterPreview(ItemObject, 6);

			Lines.Add(FString::Printf(TEXT("- %s parent=%s params=%s"),
				*(MaterialName.IsEmpty() ? FString(TEXT("UnnamedMaterialInstance")) : MaterialName),
				*(ParentMaterial.IsEmpty() ? FString(TEXT("Unknown")) : ParentMaterial),
				*(ParameterCount.IsEmpty() ? FString(TEXT("0")) : ParameterCount)));
			if (!MaterialPath.IsEmpty() || ParameterPreview != TEXT("none"))
			{
				Lines.Add(FString::Printf(TEXT("  path=%s parameters=%s"),
					*(MaterialPath.IsEmpty() ? FString(TEXT("unknown")) : MaterialPath),
					*ParameterPreview));
			}

			++DisplayedCount;
			if (DisplayedCount >= 15)
			{
				break;
			}
		}

		if (DisplayedCount == 0)
		{
			Lines.Add(EmptyReason.IsEmpty()
				? TEXT("No Material Instances were returned. Sync Project Inventory, then try again.")
				: FString::Printf(TEXT("No Material Instances were returned: %s"), *EmptyReason));
		}
		else if (ItemValues->Num() > DisplayedCount)
		{
			Lines.Add(FString::Printf(TEXT("...and %d more Material Instances. Ask Agent Chat for a filtered list if needed."), ItemValues->Num() - DisplayedCount));
		}

		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = FString::Printf(TEXT("Loaded %d Material Instance inventory items."), ItemValues->Num());
		return DisplayedCount > 0;
	}


bool FUEAgentUserMessageFormatter::BuildEditorOperationCapabilitiesMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus)
	{
		const TSharedPtr<FJsonObject> CapabilitiesObject = GetObjectField(ResponseObject, TEXT("capabilities"));
		if (!CapabilitiesObject.IsValid())
		{
			OutMessage = TEXT("Editor operation capabilities response did not include a capabilities object.");
			OutStatus = TEXT("Editor operation tools are unavailable.");
			return false;
		}

		const TSharedPtr<FJsonObject> SummaryObject = GetObjectField(CapabilitiesObject, TEXT("summary"));
		const TSharedPtr<FJsonObject> SafetyObject = GetObjectField(CapabilitiesObject, TEXT("safety_policy"));
		const FString ProtocolVersion = GetScalarFieldAsString(CapabilitiesObject, TEXT("protocol_version"));
		const FString Transport = GetScalarFieldAsString(CapabilitiesObject, TEXT("transport"));
		const FString ProposalType = GetScalarFieldAsString(CapabilitiesObject, TEXT("proposal_type"));

		auto CountArrayField = [](const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName) -> int32
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			return JsonObject.IsValid() && JsonObject->TryGetArrayField(FieldName, Values) && Values != nullptr
				? Values->Num()
				: 0;
		};

		auto PreviewOperationItems = [](const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, const int32 MaxItems) -> FString
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!JsonObject.IsValid() || !JsonObject->TryGetArrayField(FieldName, Values) || Values == nullptr || Values->Num() == 0)
			{
				return TEXT("none");
			}

			TArray<FString> Parts;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> ItemObject = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!ItemObject.IsValid())
				{
					continue;
				}

				FString Title = FirstNonEmptyString(ItemObject, { TEXT("title"), TEXT("operation_type"), TEXT("tool_id") });
				const FString OperationType = GetScalarFieldAsString(ItemObject, TEXT("operation_type"));
				const FString SideEffect = GetScalarFieldAsString(ItemObject, TEXT("side_effect_level"));
				if (Title.IsEmpty())
				{
					Title = TEXT("operation");
				}
				if (!OperationType.IsEmpty() && !Title.Contains(*OperationType))
				{
					Title += FString::Printf(TEXT(" (%s)"), *OperationType);
				}
				if (!SideEffect.IsEmpty())
				{
					Title += FString::Printf(TEXT("<%s>"), *SideEffect);
				}
				Parts.Add(Title);
				if (Parts.Num() >= MaxItems)
				{
					break;
				}
			}
			if (Values->Num() > Parts.Num())
			{
				Parts.Add(FString::Printf(TEXT("+%d more"), Values->Num() - Parts.Num()));
			}
			return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("none"));
		};

		TArray<FString> Lines;
		Lines.Add(TEXT("Editor Operation Tool Catalog"));
		Lines.Add(FString::Printf(TEXT("- protocol=%s transport=%s proposal_type=%s"),
			*(ProtocolVersion.IsEmpty() ? FString(TEXT("unknown")) : ProtocolVersion),
			*(Transport.IsEmpty() ? FString(TEXT("unknown")) : Transport),
			*(ProposalType.IsEmpty() ? FString(TEXT("unknown")) : ProposalType)));
		Lines.Add(FString::Printf(TEXT("- confirmed_write=%d implemented_frontend=%d read_only=%d roadmap=%d groups=%d"),
			GetIntegerFieldOrZero(SummaryObject, TEXT("operation_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("implemented_frontend_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("read_only_operation_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("roadmap_operation_count")),
			GetIntegerFieldOrZero(SummaryObject, TEXT("group_count"))));
		Lines.Add(FString::Printf(TEXT("- safety: llm_direct_execution=%s confirmation=%s frontend_executes=%s auto_save=%s"),
			*GetScalarFieldAsString(SafetyObject, TEXT("llm_direct_execution")),
			*GetScalarFieldAsString(SafetyObject, TEXT("requires_frontend_confirmation")),
			*GetScalarFieldAsString(SafetyObject, TEXT("ue_plugin_executes_editor_api")),
			*GetScalarFieldAsString(SafetyObject, TEXT("auto_save"))));
		Lines.Add(FString::Printf(TEXT("- status_counts: %s"), *FormatScalarObjectPreview(GetObjectField(SummaryObject, TEXT("frontend_status_counts")), TEXT("none"), 8)));
		Lines.Add(FString::Printf(TEXT("- risk_counts: %s"), *FormatScalarObjectPreview(GetObjectField(SummaryObject, TEXT("risk_flag_counts")), TEXT("none"), 8)));

		const TArray<TSharedPtr<FJsonValue>>* GroupValues = nullptr;
		if (CapabilitiesObject->TryGetArrayField(TEXT("groups"), GroupValues) && GroupValues != nullptr && GroupValues->Num() > 0)
		{
			Lines.Add(TEXT("Groups:"));
			for (const TSharedPtr<FJsonValue>& GroupValue : *GroupValues)
			{
				const TSharedPtr<FJsonObject> GroupObject = GroupValue.IsValid() ? GroupValue->AsObject() : nullptr;
				if (!GroupObject.IsValid())
				{
					continue;
				}
				const FString Title = FirstNonEmptyString(GroupObject, { TEXT("title"), TEXT("group_id") });
				Lines.Add(FString::Printf(TEXT("- %s | write=%d read_only=%d roadmap=%d"),
					*(Title.IsEmpty() ? FString(TEXT("Group")) : Title),
					GetIntegerFieldOrZero(GroupObject, TEXT("operation_count")),
					GetIntegerFieldOrZero(GroupObject, TEXT("read_only_count")),
					GetIntegerFieldOrZero(GroupObject, TEXT("roadmap_count"))));
			}
		}

		Lines.Add(FString::Printf(TEXT("Confirmed-write examples: %s"), *PreviewOperationItems(CapabilitiesObject, TEXT("items"), 8)));
		Lines.Add(FString::Printf(TEXT("Read-only examples: %s"), *PreviewOperationItems(CapabilitiesObject, TEXT("read_only_items"), 6)));
		Lines.Add(FString::Printf(TEXT("Total catalog entries: %d"), CountArrayField(CapabilitiesObject, TEXT("items")) + CountArrayField(CapabilitiesObject, TEXT("read_only_items")) + CountArrayField(CapabilitiesObject, TEXT("roadmap_items"))));

		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = TEXT("Loaded Editor Operation tool catalog.");
		return true;
	}


bool FUEAgentUserMessageFormatter::BuildToolManifestWorkflowPreviewMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus)
	{
		const TSharedPtr<FJsonObject> ManifestObject = GetObjectField(ResponseObject, TEXT("manifest"));
		const TSharedPtr<FJsonObject> ProfilesObject = GetObjectField(ManifestObject, TEXT("profiles"));
		const TArray<TSharedPtr<FJsonValue>>* PreviewValues = nullptr;
		if (!ProfilesObject.IsValid() || !ProfilesObject->TryGetArrayField(TEXT("workflow_previews"), PreviewValues) || PreviewValues == nullptr || PreviewValues->Num() == 0)
		{
			OutMessage = TEXT("Tool workflow preview is unavailable.");
			OutStatus = TEXT("Tool workflow preview unavailable.");
			return false;
		}

		auto PreviewStringArrayField = [](const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, const int32 MaxItems) -> FString
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!JsonObject.IsValid() || !JsonObject->TryGetArrayField(FieldName, Values) || Values == nullptr || Values->Num() == 0)
			{
				return TEXT("none");
			}

			TArray<FString> Parts;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const FString Text = Value.IsValid() ? Value->AsString() : FString();
				if (!Text.IsEmpty())
				{
					Parts.Add(Text);
				}
				if (Parts.Num() >= MaxItems)
				{
					break;
				}
			}
			if (Values->Num() > Parts.Num())
			{
				Parts.Add(FString::Printf(TEXT("+%d more"), Values->Num() - Parts.Num()));
			}
			return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("none"));
		};

		TArray<FString> Lines;
		Lines.Add(TEXT("Suggested Tool Workflows"));
		Lines.Add(TEXT("- Recommended path: observe editor facts -> set optional context -> create Proposal -> confirm in UE."));
		int32 DisplayedCount = 0;
		for (const TSharedPtr<FJsonValue>& PreviewValue : *PreviewValues)
		{
			const TSharedPtr<FJsonObject> PreviewObject = PreviewValue.IsValid() ? PreviewValue->AsObject() : nullptr;
			if (!PreviewObject.IsValid())
			{
				continue;
			}

			const FString ProfileId = FirstNonEmptyString(PreviewObject, { TEXT("profile_id"), TEXT("workflow_id") });
			const FString Title = FirstNonEmptyString(PreviewObject, { TEXT("title"), TEXT("workflow_id") });
			const FString Summary = GetScalarFieldAsString(PreviewObject, TEXT("summary"));
			Lines.Add(FString::Printf(TEXT("- %s: %s"),
				*(ProfileId.IsEmpty() ? FString(TEXT("profile")) : ProfileId),
				*(Title.IsEmpty() ? FString(TEXT("workflow")) : Title)));
			if (!Summary.IsEmpty())
			{
				Lines.Add(FString::Printf(TEXT("  %s"), *Summary));
			}
			Lines.Add(FString::Printf(TEXT("  observe=%s | context=%s | proposal=%s"),
				*PreviewStringArrayField(PreviewObject, TEXT("observe_tools"), 3),
				*PreviewStringArrayField(PreviewObject, TEXT("context_tools"), 3),
				*PreviewStringArrayField(PreviewObject, TEXT("proposal_tools"), 3)));
			const FString HappyPath = PreviewStringArrayField(PreviewObject, TEXT("happy_path"), 3);
			if (HappyPath != TEXT("none"))
			{
				Lines.Add(FString::Printf(TEXT("  path=%s"), *HappyPath));
			}
			++DisplayedCount;
			if (DisplayedCount >= 6)
			{
				break;
			}
		}

		if (DisplayedCount == 0)
		{
			OutMessage = TEXT("Tool workflow preview did not include any displayable workflows.");
			OutStatus = TEXT("Tool workflow preview unavailable.");
			return false;
		}

		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = FString::Printf(TEXT("Loaded %d suggested tool workflows."), DisplayedCount);
		return true;
	}


bool FUEAgentUserMessageFormatter::BuildEditorOperationActivityMessage(const TSharedPtr<FJsonObject>& DiagnosticsObject, const TSharedPtr<FJsonObject>& HistoryObject, FString& OutMessage, FString& OutStatus)
	{
		const TSharedPtr<FJsonObject> DiagnosticsSummary = GetObjectField(DiagnosticsObject, TEXT("summary"));
		const TSharedPtr<FJsonObject> HistorySummary = GetObjectField(HistoryObject, TEXT("summary"));
		const int32 InspectedCount = GetIntegerFieldOrZero(DiagnosticsSummary, TEXT("inspected_count"));
		const int32 ExecutedCount = GetIntegerFieldOrZero(DiagnosticsSummary, TEXT("executed_count"));
		const int32 PendingCount = GetIntegerFieldOrZero(DiagnosticsSummary, TEXT("pending_count"));
		const int32 SuccessCount = GetIntegerFieldOrZero(DiagnosticsSummary, TEXT("success_count"));
		const int32 FailedCount = GetIntegerFieldOrZero(DiagnosticsSummary, TEXT("failed_count"));
		const int32 AttentionCount = GetIntegerFieldOrZero(DiagnosticsSummary, TEXT("needs_user_attention_count"));
		const FString AttentionRate = GetScalarFieldAsString(DiagnosticsSummary, TEXT("attention_rate"));

		TArray<FString> Lines;
		Lines.Add(TEXT("Editor Operation Activity"));
		Lines.Add(FString::Printf(TEXT("- inspected=%d executed=%d pending=%d success=%d failed=%d attention=%d rate=%s"),
			InspectedCount,
			ExecutedCount,
			PendingCount,
			SuccessCount,
			FailedCount,
			AttentionCount,
			*(AttentionRate.IsEmpty() ? FString(TEXT("0")) : AttentionRate)));
		Lines.Add(FString::Printf(TEXT("- operation_types: %s"), *FormatScalarObjectPreview(GetObjectField(DiagnosticsSummary, TEXT("operation_type_counts")), TEXT("none"), 8)));
		Lines.Add(FString::Printf(TEXT("- execution_states: %s"), *FormatScalarObjectPreview(GetObjectField(DiagnosticsSummary, TEXT("execution_state_counts")), TEXT("none"), 6)));
		Lines.Add(FString::Printf(TEXT("- diagnostic_flags: %s"), *FormatScalarObjectPreview(GetObjectField(DiagnosticsSummary, TEXT("diagnostic_flag_counts")), TEXT("none"), 6)));

		const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
		if (HistoryObject.IsValid() && HistoryObject->TryGetArrayField(TEXT("items"), ItemValues) && ItemValues != nullptr && ItemValues->Num() > 0)
		{
			Lines.Add(TEXT("Recent operations:"));
			int32 DisplayedCount = 0;
			for (const TSharedPtr<FJsonValue>& ItemValue : *ItemValues)
			{
				const TSharedPtr<FJsonObject> ItemObject = ItemValue.IsValid() ? ItemValue->AsObject() : nullptr;
				if (!ItemObject.IsValid())
				{
					continue;
				}

				const FString Title = FirstNonEmptyString(ItemObject, { TEXT("title"), TEXT("proposal_id"), TEXT("operation_type") });
				const FString OperationType = GetScalarFieldAsString(ItemObject, TEXT("operation_type"));
				const FString ConfirmationState = GetScalarFieldAsString(ItemObject, TEXT("confirmation_state"));
				const FString ExecutionState = GetScalarFieldAsString(ItemObject, TEXT("execution_state"));
				const FString Success = GetScalarFieldAsString(ItemObject, TEXT("success"));
				const FString UpdatedAt = GetScalarFieldAsString(ItemObject, TEXT("updated_at"));
				Lines.Add(FString::Printf(TEXT("- %s | op=%s confirm=%s exec=%s success=%s updated=%s"),
					*(Title.IsEmpty() ? FString(TEXT("Untitled Proposal")) : Title),
					*(OperationType.IsEmpty() ? FString(TEXT("unknown")) : OperationType),
					*(ConfirmationState.IsEmpty() ? FString(TEXT("unknown")) : ConfirmationState),
					*(ExecutionState.IsEmpty() ? FString(TEXT("pending_result")) : ExecutionState),
					*(Success.IsEmpty() ? FString(TEXT("n/a")) : Success),
					*(UpdatedAt.IsEmpty() ? FString(TEXT("unknown")) : UpdatedAt)));

				++DisplayedCount;
				if (DisplayedCount >= 8)
				{
					break;
				}
			}
		}
		else
		{
			Lines.Add(TEXT("Recent operations: none"));
		}

		const int32 HistoryCount = GetIntegerFieldOrZero(HistorySummary, TEXT("item_count"));
		OutMessage = FString::Join(Lines, TEXT("\n"));
		OutStatus = FString::Printf(TEXT("Loaded %d recent editor operations."), HistoryCount);
		return InspectedCount > 0 || HistoryCount > 0;
	}

