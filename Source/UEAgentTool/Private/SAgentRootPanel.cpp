// Copyright Epic Games, Inc. All Rights Reserved.

#include "SAgentRootPanel.h"

#include "AgentEditorToolRegistry.h"
#include "AgentEditorToolCatalog.h"

#include "AgentStyle.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IAssetTools.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UObject/Class.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SWindow.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

namespace UEAgentRootPanelPrivate
{
	struct FGeneratedDraftItem
	{
		FString Label;
		FString FilePath;
		FString Language;
		FString Code;
		FString WriteStatus;
		bool bIsVirtual = false;
		bool bWrittenToDisk = false;
	};

	using FEditorOperationExecutionResult = FUEAgentEditorToolExecutionResult;

	static FString GetLocalizedUiText(const FString& LanguageCode, const TCHAR* ZhText, const TCHAR* EnText)
	{
		return UEAgent::LocalizeStableUiText(LanguageCode, ZhText, EnText);
	}

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

	static TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonText)
	{
		if (JsonText.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, JsonObject) ? JsonObject : nullptr;
	}

	static void AddEditorOperationError(TArray<TSharedPtr<FJsonValue>>& ErrorValues, const FString& Reason, const FString& Message)
	{
		TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
		ErrorObject->SetStringField(TEXT("reason"), Reason);
		ErrorObject->SetStringField(TEXT("message"), Message);
		ErrorValues.Add(MakeShared<FJsonValueObject>(ErrorObject));
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

	static bool TryGetBoolField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, bool& bOutValue)
	{
		return JsonObject.IsValid() && JsonObject->TryGetBoolField(FieldName, bOutValue);
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

	static UObject* LoadEditorAsset(const FString& AssetPath)
	{
		const FString PackagePath = NormalizeAssetPackagePath(AssetPath);
		return StaticLoadObject(UObject::StaticClass(), nullptr, *ToObjectPath(PackagePath));
	}

	static void SetAppliedField(TSharedPtr<FJsonObject>& ResultObject, const FString& FieldName, const FString& Value)
	{
		TSharedPtr<FJsonObject> AppliedFields = GetObjectField(ResultObject, TEXT("applied_fields"));
		if (!AppliedFields.IsValid())
		{
			AppliedFields = MakeShared<FJsonObject>();
			ResultObject->SetObjectField(TEXT("applied_fields"), AppliedFields);
		}
		AppliedFields->SetStringField(FieldName, Value);
	}

	static void AddResultStringArrayItem(TSharedPtr<FJsonObject>& ResultObject, const FString& FieldName, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> ArrayValues;
		const TArray<TSharedPtr<FJsonValue>>* ExistingValues = nullptr;
		if (ResultObject->TryGetArrayField(FieldName, ExistingValues) && ExistingValues != nullptr)
		{
			ArrayValues = *ExistingValues;
		}

		ArrayValues.Add(MakeShared<FJsonValueString>(Value));
		ResultObject->SetArrayField(FieldName, ArrayValues);
	}

	static void AddFailedField(TSharedPtr<FJsonObject>& ResultObject, const FString& FieldName, const FString& Reason)
	{
		TSharedPtr<FJsonObject> FailedFieldObject = MakeShared<FJsonObject>();
		FailedFieldObject->SetStringField(TEXT("field"), FieldName);
		FailedFieldObject->SetStringField(TEXT("reason"), Reason);

		TArray<TSharedPtr<FJsonValue>> FailedFields;
		const TArray<TSharedPtr<FJsonValue>>* ExistingValues = nullptr;
		if (ResultObject->TryGetArrayField(TEXT("failed_fields"), ExistingValues) && ExistingValues != nullptr)
		{
			FailedFields = *ExistingValues;
		}
		FailedFields.Add(MakeShared<FJsonValueObject>(FailedFieldObject));
		ResultObject->SetArrayField(TEXT("failed_fields"), FailedFields);
	}

	static bool TryGetNumberComponent(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, double& OutValue)
	{
		return JsonObject.IsValid() && JsonObject->TryGetNumberField(FieldName, OutValue);
	}

	static bool TryReadVectorObject(const TSharedPtr<FJsonObject>& JsonObject, FVector& OutVector)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!TryGetNumberComponent(JsonObject, TEXT("x"), X) && !TryGetNumberComponent(JsonObject, TEXT("X"), X))
		{
			return false;
		}
		if (!TryGetNumberComponent(JsonObject, TEXT("y"), Y) && !TryGetNumberComponent(JsonObject, TEXT("Y"), Y))
		{
			return false;
		}
		if (!TryGetNumberComponent(JsonObject, TEXT("z"), Z) && !TryGetNumberComponent(JsonObject, TEXT("Z"), Z))
		{
			return false;
		}

		OutVector = FVector(X, Y, Z);
		return true;
	}

	static bool TryReadVectorField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, FVector& OutVector)
	{
		const TSharedPtr<FJsonObject> VectorObject = GetObjectField(JsonObject, FieldName);
		if (TryReadVectorObject(VectorObject, OutVector))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if (JsonObject.IsValid() && JsonObject->TryGetArrayField(FieldName, ArrayValues) && ArrayValues != nullptr && ArrayValues->Num() >= 3)
		{
			OutVector = FVector((*ArrayValues)[0]->AsNumber(), (*ArrayValues)[1]->AsNumber(), (*ArrayValues)[2]->AsNumber());
			return true;
		}

		return false;
	}

	static bool TryReadRotatorField(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, FRotator& OutRotator)
	{
		const TSharedPtr<FJsonObject> RotatorObject = GetObjectField(JsonObject, FieldName);
		if (RotatorObject.IsValid())
		{
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			if ((TryGetNumberComponent(RotatorObject, TEXT("pitch"), Pitch) || TryGetNumberComponent(RotatorObject, TEXT("Pitch"), Pitch))
				&& (TryGetNumberComponent(RotatorObject, TEXT("yaw"), Yaw) || TryGetNumberComponent(RotatorObject, TEXT("Yaw"), Yaw))
				&& (TryGetNumberComponent(RotatorObject, TEXT("roll"), Roll) || TryGetNumberComponent(RotatorObject, TEXT("Roll"), Roll)))
			{
				OutRotator = FRotator(Pitch, Yaw, Roll);
				return true;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if (JsonObject.IsValid() && JsonObject->TryGetArrayField(FieldName, ArrayValues) && ArrayValues != nullptr && ArrayValues->Num() >= 3)
		{
			OutRotator = FRotator((*ArrayValues)[0]->AsNumber(), (*ArrayValues)[1]->AsNumber(), (*ArrayValues)[2]->AsNumber());
			return true;
		}

		return false;
	}

	static FString PinTypeToOperationString(const FEdGraphPinType& PinType)
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
		return TypeText;
	}

	static UBlueprint* LoadBlueprintAsset(const FString& BlueprintPath)
	{
		return Cast<UBlueprint>(LoadEditorAsset(BlueprintPath));
	}

	static FString BlueprintStatusToOperationString(const EBlueprintStatus Status)
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

	static ECollisionTraceFlag CollisionTraceFlagFromString(const FString& Value, bool& bOutValid)
	{
		const FString NormalizedValue = Value.TrimStartAndEnd().ToLower();
		bOutValid = true;
		if (NormalizedValue == TEXT("project_default"))
		{
			return CTF_UseDefault;
		}
		if (NormalizedValue == TEXT("simple_and_complex"))
		{
			return CTF_UseSimpleAndComplex;
		}
		if (NormalizedValue == TEXT("use_simple_as_complex"))
		{
			return CTF_UseSimpleAsComplex;
		}
		if (NormalizedValue == TEXT("use_complex_as_simple"))
		{
			return CTF_UseComplexAsSimple;
		}
		bOutValid = false;
		return CTF_UseDefault;
	}

	static UClass* ResolveBlueprintParentClass(const FString& ParentClassPath)
	{
		const FString ClassPath = ParentClassPath.TrimStartAndEnd();
		if (ClassPath.Equals(TEXT("/Script/Engine.Actor"), ESearchCase::IgnoreCase) || ClassPath.Equals(TEXT("Actor"), ESearchCase::IgnoreCase))
		{
			return AActor::StaticClass();
		}
		if (ClassPath.Equals(TEXT("/Script/Engine.Character"), ESearchCase::IgnoreCase) || ClassPath.Equals(TEXT("Character"), ESearchCase::IgnoreCase))
		{
			return ACharacter::StaticClass();
		}
		if (ClassPath.Equals(TEXT("/Script/Engine.Pawn"), ESearchCase::IgnoreCase) || ClassPath.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))
		{
			return APawn::StaticClass();
		}
		return LoadObject<UClass>(nullptr, *ClassPath);
	}

	static UObject* LoadTypeObject(const FString& TypePath)
	{
		const FString TrimmedTypePath = TypePath.TrimStartAndEnd();
		if (TrimmedTypePath.IsEmpty())
		{
			return nullptr;
		}

		UObject* TypeObject = StaticLoadObject(UObject::StaticClass(), nullptr, *TrimmedTypePath);
		if (TypeObject != nullptr)
		{
			return TypeObject;
		}

		if (TrimmedTypePath.StartsWith(TEXT("/Game/")))
		{
			const FString ObjectPath = ToObjectPath(NormalizeAssetPackagePath(TrimmedTypePath));
			TypeObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
			if (TypeObject != nullptr)
			{
				return TypeObject;
			}

			return LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")));
		}

		return nullptr;
	}

	static bool TryBuildBlueprintVariablePinType(const FString& VariableType, FEdGraphPinType& OutPinType, FString& OutError)
	{
		const FString NormalizedType = VariableType.TrimStartAndEnd().ToLower();
		OutPinType = FEdGraphPinType();

		if (NormalizedType == TEXT("bool") || NormalizedType == TEXT("boolean"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		}
		if (NormalizedType == TEXT("byte"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			return true;
		}
		if (NormalizedType == TEXT("int") || NormalizedType == TEXT("integer") || NormalizedType == TEXT("int32"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		}
		if (NormalizedType == TEXT("int64"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			return true;
		}
		if (NormalizedType == TEXT("float"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Float;
			return true;
		}
		if (NormalizedType == TEXT("double"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Double;
			return true;
		}
		if (NormalizedType == TEXT("name"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			return true;
		}
		if (NormalizedType == TEXT("string"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
			return true;
		}
		if (NormalizedType == TEXT("text"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			return true;
		}
		if (NormalizedType == TEXT("vector"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			return true;
		}
		if (NormalizedType == TEXT("rotator"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			return true;
		}
		if (NormalizedType == TEXT("transform"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			return true;
		}
		if (NormalizedType == TEXT("object"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = UObject::StaticClass();
			return true;
		}
		if (NormalizedType == TEXT("actor"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = AActor::StaticClass();
			return true;
		}

		if (VariableType.StartsWith(TEXT("/Script/")) || VariableType.StartsWith(TEXT("/Game/")))
		{
			UObject* TypeObject = LoadTypeObject(VariableType);
			if (UBlueprint* TypeBlueprint = Cast<UBlueprint>(TypeObject))
			{
				TypeObject = TypeBlueprint->GeneratedClass;
			}

			if (UClass* TypeClass = Cast<UClass>(TypeObject))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = TypeClass;
				return true;
			}
			if (UScriptStruct* TypeStruct = Cast<UScriptStruct>(TypeObject))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TypeStruct;
				return true;
			}
			if (UEnum* TypeEnum = Cast<UEnum>(TypeObject))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
				OutPinType.PinSubCategoryObject = TypeEnum;
				return true;
			}

			OutError = FString::Printf(TEXT("Unsupported or unloaded variable type path: %s"), *VariableType);
			return false;
		}

		OutError = FString::Printf(TEXT("Unsupported Blueprint variable type: %s"), *VariableType);
		return false;
	}

	static UClass* ResolveBlueprintComponentClass(const FString& ComponentClassPath)
	{
		const FString ClassPath = ComponentClassPath.TrimStartAndEnd();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (ClassPath.Equals(TEXT("ActorComponent"), ESearchCase::IgnoreCase))
		{
			return UActorComponent::StaticClass();
		}
		if (ClassPath.Equals(TEXT("SceneComponent"), ESearchCase::IgnoreCase))
		{
			return USceneComponent::StaticClass();
		}

		return LoadObject<UClass>(nullptr, *ClassPath);
	}

	static bool ResolveBlueprintEventFunction(const FString& EventName, FName& OutFunctionName, UClass*& OutOwnerClass)
	{
		const FString NormalizedName = EventName.TrimStartAndEnd().ToLower();
		OutOwnerClass = AActor::StaticClass();
		if (NormalizedName == TEXT("beginplay"))
		{
			OutFunctionName = FName(TEXT("ReceiveBeginPlay"));
			return true;
		}
		if (NormalizedName == TEXT("tick"))
		{
			OutFunctionName = FName(TEXT("ReceiveTick"));
			return true;
		}
		if (NormalizedName == TEXT("actorbeginoverlap"))
		{
			OutFunctionName = FName(TEXT("ReceiveActorBeginOverlap"));
			return true;
		}
		if (NormalizedName == TEXT("actorendoverlap"))
		{
			OutFunctionName = FName(TEXT("ReceiveActorEndOverlap"));
			return true;
		}
		return false;
	}

	static FEditorOperationExecutionResult ExecuteRenameSelectedAsset(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("AssetTools.RenameAssets"));

		const FString AssetPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("asset_path")));
		const FString NewName = GetScalarFieldAsString(PayloadObject, TEXT("new_name")).TrimStartAndEnd();
		if (AssetPath.IsEmpty() || NewName.IsEmpty())
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("asset_path and new_name are required."));
			return ExecutionResult;
		}

		UObject* Asset = LoadEditorAsset(AssetPath);
		if (Asset == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("asset_not_found"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
			return ExecutionResult;
		}

		const FString FolderPath = FPaths::GetPath(AssetPath);
		const FString TargetPath = FString::Printf(TEXT("%s/%s"), *FolderPath, *NewName);
		if (LoadEditorAsset(TargetPath) != nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("target_asset_exists"), FString::Printf(TEXT("Target asset already exists: %s"), *TargetPath));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Rename Selected Asset")));
		Asset->Modify();
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		TArray<FAssetRenameData> RenameData;
		RenameData.Add(FAssetRenameData(Asset, FolderPath, NewName));
		const bool bRenamed = AssetToolsModule.Get().RenameAssets(RenameData);
		if (!bRenamed)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("rename_failed"), TEXT("AssetTools RenameAssets returned false."));
			return ExecutionResult;
		}

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo or rename the asset back. Redirectors are not fixed automatically.");
		ExecutionResult.ResultObject->SetStringField(TEXT("final_asset_path"), TargetPath);
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		return ExecutionResult;
	}

	static FEditorOperationExecutionResult ExecuteApplyStaticMeshBasicSettings(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UStaticMesh editor properties"));

		const FString AssetPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("asset_path")));
		const TSharedPtr<FJsonObject> SettingsObject = GetObjectField(PayloadObject, TEXT("settings"));
		UObject* Asset = LoadEditorAsset(AssetPath);
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (StaticMesh == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("static_mesh_not_found"), FString::Printf(TEXT("Static Mesh not found: %s"), *AssetPath));
			return ExecutionResult;
		}
		if (!SettingsObject.IsValid() || SettingsObject->Values.Num() == 0)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("empty_settings"), TEXT("settings must be a non-empty object."));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Apply Static Mesh Settings")));
		StaticMesh->Modify();

		bool bNaniteEnabled = false;
		if (TryGetBoolField(SettingsObject, TEXT("nanite_enabled"), bNaniteEnabled))
		{
#if WITH_EDITORONLY_DATA
			StaticMesh->NaniteSettings.bEnabled = bNaniteEnabled;
			SetAppliedField(ExecutionResult.ResultObject, TEXT("nanite_enabled"), bNaniteEnabled ? TEXT("true") : TEXT("false"));
#else
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("nanite_editor_only_unavailable"), TEXT("Nanite settings are editor-only in this build."));
#endif
		}

		const FString CollisionComplexity = GetScalarFieldAsString(SettingsObject, TEXT("collision_complexity"));
		if (!CollisionComplexity.IsEmpty())
		{
			bool bValidCollision = false;
			const ECollisionTraceFlag CollisionFlag = CollisionTraceFlagFromString(CollisionComplexity, bValidCollision);
			if (bValidCollision && StaticMesh->GetBodySetup() != nullptr)
			{
				StaticMesh->GetBodySetup()->Modify();
				StaticMesh->GetBodySetup()->CollisionTraceFlag = CollisionFlag;
				SetAppliedField(ExecutionResult.ResultObject, TEXT("collision_complexity"), CollisionComplexity);
			}
			else
			{
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("collision_complexity_not_applied"), CollisionComplexity);
			}
		}

		const FString LodGroup = GetScalarFieldAsString(SettingsObject, TEXT("lod_group"));
		if (!LodGroup.IsEmpty())
		{
			StaticMesh->SetLODGroup(FName(*LodGroup), false, true);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("lod_group"), LodGroup);
		}

		bool bGenerateLightmapUv = false;
		if (TryGetBoolField(SettingsObject, TEXT("generate_lightmap_uv"), bGenerateLightmapUv))
		{
			if (StaticMesh->GetNumSourceModels() > 0)
			{
				StaticMesh->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs = bGenerateLightmapUv;
				SetAppliedField(ExecutionResult.ResultObject, TEXT("generate_lightmap_uv"), bGenerateLightmapUv ? TEXT("true") : TEXT("false"));
			}
			else
			{
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("source_model_missing"), TEXT("Static Mesh has no source model for lightmap UV settings."));
			}
		}

		int32 LightmapResolution = 0;
		if (TryGetIntegerField(SettingsObject, TEXT("lightmap_resolution"), LightmapResolution))
		{
			StaticMesh->SetLightMapResolution(LightmapResolution);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("lightmap_resolution"), FString::FromInt(LightmapResolution));
		}

		StaticMesh->PostEditChange();
		StaticMesh->MarkPackageDirty();

		ExecutionResult.bSuccess = ExecutionResult.ErrorValues.Num() == 0;
		ExecutionResult.ExecutionState = ExecutionResult.bSuccess ? TEXT("completed") : TEXT("failed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to revert Static Mesh property changes. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("asset_path"), AssetPath);
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		return ExecutionResult;
	}

	static FEditorOperationExecutionResult ExecuteCreateBlueprintAsset(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("FKismetEditorUtilities.CreateBlueprint"));

		const FString ParentClassPath = GetScalarFieldAsString(PayloadObject, TEXT("parent_class"));
		FString TargetFolderPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("target_folder")));
		TargetFolderPath.RemoveFromEnd(TEXT("/"));
		const FString AssetName = GetScalarFieldAsString(PayloadObject, TEXT("asset_name")).TrimStartAndEnd();
		if (!(TargetFolderPath == TEXT("/Game") || TargetFolderPath.StartsWith(TEXT("/Game/"))))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("target_folder_must_be_under_game"), TargetFolderPath);
			return ExecutionResult;
		}

		const FString PackageName = FString::Printf(TEXT("%s/%s"), *TargetFolderPath, *AssetName);
		if (AssetName.IsEmpty() || ParentClassPath.IsEmpty())
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("parent_class, target_folder and asset_name are required."));
			return ExecutionResult;
		}
		if (LoadEditorAsset(PackageName) != nullptr || FPackageName::DoesPackageExist(PackageName))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("target_blueprint_exists"), PackageName);
			return ExecutionResult;
		}

		UClass* ParentClass = ResolveBlueprintParentClass(ParentClassPath);
		if (ParentClass == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("parent_class_not_found"), ParentClassPath);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Create Blueprint Asset")));
		UPackage* Package = CreatePackage(*PackageName);
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UEAgentTool")));
		if (Blueprint == nullptr)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_creation_failed"), PackageName);
			return ExecutionResult;
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo or delete the created Blueprint asset. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("asset_path"), PackageName);
		ExecutionResult.ResultObject->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		ExecutionResult.ResultObject->SetBoolField(TEXT("opened_editor"), false);
		return ExecutionResult;
	}

	static FEditorOperationExecutionResult ExecuteAddBlueprintVariable(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("FBlueprintEditorUtils.AddMemberVariable"));

		const FString BlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("blueprint_path")));
		const FString VariableName = GetScalarFieldAsString(PayloadObject, TEXT("variable_name")).TrimStartAndEnd();
		const FString VariableType = GetScalarFieldAsString(PayloadObject, TEXT("variable_type")).TrimStartAndEnd();
		const FString Category = GetScalarFieldAsString(PayloadObject, TEXT("category")).TrimStartAndEnd();
		const FString DefaultValue = GetScalarFieldAsString(PayloadObject, TEXT("default_value"));
		if (BlueprintPath.IsEmpty() || VariableName.IsEmpty() || VariableType.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("blueprint_path, variable_name and variable_type are required."));
			return ExecutionResult;
		}

		FText InvalidNameReason;
		const FName VariableFName(*VariableName);
		if (!VariableFName.IsValidXName(INVALID_NAME_CHARACTERS, &InvalidNameReason))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("invalid_variable_name"), InvalidNameReason.ToString());
			return ExecutionResult;
		}

		UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath);
		if (Blueprint == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return ExecutionResult;
		}
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			if (Variable.VarName == VariableFName)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("variable_already_exists"), VariableName);
				return ExecutionResult;
			}
		}

		FEdGraphPinType PinType;
		FString TypeError;
		if (!TryBuildBlueprintVariablePinType(VariableType, PinType, TypeError))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("variable_type_unsupported"), TypeError);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Add Blueprint Variable")));
		Blueprint->Modify();
		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableFName, PinType, DefaultValue);
		if (!bAdded)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("add_variable_failed"), TEXT("FBlueprintEditorUtils::AddMemberVariable returned false."));
			return ExecutionResult;
		}

		SetAppliedField(ExecutionResult.ResultObject, TEXT("variable_name"), VariableName);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("variable_type"), PinTypeToOperationString(PinType));
		if (!Category.IsEmpty())
		{
			FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VariableFName, nullptr, FText::FromString(Category), true);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("category"), Category);
		}
		if (!DefaultValue.IsEmpty())
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("default_value"), DefaultValue);
		}

		bool bExposeOnSpawn = false;
		if (TryGetBoolField(PayloadObject, TEXT("expose_on_spawn"), bExposeOnSpawn))
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VariableFName, nullptr, FName(TEXT("ExposeOnSpawn")), bExposeOnSpawn ? TEXT("true") : TEXT("false"));
			SetAppliedField(ExecutionResult.ResultObject, TEXT("expose_on_spawn"), bExposeOnSpawn ? TEXT("true") : TEXT("false"));
		}

		bool bEditable = true;
		if (TryGetBoolField(PayloadObject, TEXT("editable"), bEditable))
		{
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VariableFName, !bEditable);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("editable"), bEditable ? TEXT("true") : TEXT("false"));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to remove the added Blueprint variable. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), Blueprint->GetOutermost()->GetName());
		return ExecutionResult;
	}

	static FEditorOperationExecutionResult ExecuteAddBlueprintComponent(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("USimpleConstructionScript.CreateNode"));

		const FString BlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("blueprint_path")));
		const FString ComponentName = GetScalarFieldAsString(PayloadObject, TEXT("component_name")).TrimStartAndEnd();
		const FString ComponentClassPath = GetScalarFieldAsString(PayloadObject, TEXT("component_class")).TrimStartAndEnd();
		const FString AttachTo = GetScalarFieldAsString(PayloadObject, TEXT("attach_to")).TrimStartAndEnd();
		if (BlueprintPath.IsEmpty() || ComponentName.IsEmpty() || ComponentClassPath.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("blueprint_path, component_name and component_class are required."));
			return ExecutionResult;
		}

		FText InvalidNameReason;
		const FName ComponentFName(*ComponentName);
		if (!ComponentFName.IsValidXName(INVALID_NAME_CHARACTERS, &InvalidNameReason))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("invalid_component_name"), InvalidNameReason.ToString());
			return ExecutionResult;
		}

		UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath);
		if (Blueprint == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return ExecutionResult;
		}
		if (Blueprint->SimpleConstructionScript == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("simple_construction_script_missing"), BlueprintPath);
			return ExecutionResult;
		}
		if (Blueprint->SimpleConstructionScript->FindSCSNode(ComponentFName) != nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("component_already_exists"), ComponentName);
			return ExecutionResult;
		}

		UClass* ComponentClass = ResolveBlueprintComponentClass(ComponentClassPath);
		if (ComponentClass == nullptr || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("component_class_invalid"), ComponentClassPath);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Add Blueprint Component")));
		Blueprint->Modify();
		Blueprint->SimpleConstructionScript->Modify();

		USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, ComponentFName);
		if (NewNode == nullptr)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("component_node_creation_failed"), ComponentName);
			return ExecutionResult;
		}
		NewNode->SetFlags(RF_Transactional);
		NewNode->Modify();

		bool bAttachedToRequestedParent = false;
		if (!AttachTo.IsEmpty() && !AttachTo.Equals(TEXT("RootComponent"), ESearchCase::IgnoreCase))
		{
			USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*AttachTo));
			const bool bCanAttachAsSceneComponent = ComponentClass->IsChildOf(USceneComponent::StaticClass())
				&& ParentNode != nullptr
				&& ParentNode->ComponentTemplate != nullptr
				&& ParentNode->ComponentTemplate->IsA<USceneComponent>();
			if (bCanAttachAsSceneComponent)
			{
				ParentNode->Modify();
				ParentNode->AddChildNode(NewNode);
				bAttachedToRequestedParent = true;
				SetAppliedField(ExecutionResult.ResultObject, TEXT("attach_to"), AttachTo);
			}
			else
			{
				AddFailedField(ExecutionResult.ResultObject, TEXT("attach_to"), ParentNode == nullptr ? TEXT("attach target not found; component added as root node") : TEXT("attach target or component is not a scene component; component added as root node"));
				Blueprint->SimpleConstructionScript->AddNode(NewNode);
			}
		}
		else
		{
			Blueprint->SimpleConstructionScript->AddNode(NewNode);
		}

		const TSharedPtr<FJsonObject> TransformObject = GetObjectField(PayloadObject, TEXT("transform"));
		if (TransformObject.IsValid() && TransformObject->Values.Num() > 0)
		{
			USceneComponent* SceneTemplate = Cast<USceneComponent>(NewNode->ComponentTemplate);
			if (SceneTemplate != nullptr)
			{
				SceneTemplate->Modify();
				FVector Location;
				FRotator Rotation;
				FVector Scale;
				bool bAppliedAnyTransform = false;
				if (TryReadVectorField(TransformObject, TEXT("location"), Location))
				{
					SceneTemplate->SetRelativeLocation(Location);
					SetAppliedField(ExecutionResult.ResultObject, TEXT("transform.location"), Location.ToString());
					bAppliedAnyTransform = true;
				}
				if (TryReadRotatorField(TransformObject, TEXT("rotation"), Rotation))
				{
					SceneTemplate->SetRelativeRotation(Rotation);
					SetAppliedField(ExecutionResult.ResultObject, TEXT("transform.rotation"), Rotation.ToString());
					bAppliedAnyTransform = true;
				}
				if (TryReadVectorField(TransformObject, TEXT("scale"), Scale))
				{
					SceneTemplate->SetRelativeScale3D(Scale);
					SetAppliedField(ExecutionResult.ResultObject, TEXT("transform.scale"), Scale.ToString());
					bAppliedAnyTransform = true;
				}
				if (!bAppliedAnyTransform)
				{
					AddFailedField(ExecutionResult.ResultObject, TEXT("transform"), TEXT("transform object did not contain supported location/rotation/scale values"));
				}
			}
			else
			{
				AddFailedField(ExecutionResult.ResultObject, TEXT("transform"), TEXT("component is not a scene component"));
			}
		}

		Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to remove the added Blueprint component. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("component_name"), ComponentName);
		ExecutionResult.ResultObject->SetStringField(TEXT("component_class"), ComponentClass->GetPathName());
		ExecutionResult.ResultObject->SetBoolField(TEXT("attached_to_requested_parent"), bAttachedToRequestedParent);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), ComponentName);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), Blueprint->GetOutermost()->GetName());
		SetAppliedField(ExecutionResult.ResultObject, TEXT("component_name"), ComponentName);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("component_class"), ComponentClass->GetPathName());
		return ExecutionResult;
	}

	static FEditorOperationExecutionResult ExecuteCreateBlueprintEventStub(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UK2Node_Event"));

		const FString BlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("blueprint_path")));
		const FString EventName = GetScalarFieldAsString(PayloadObject, TEXT("event_name")).TrimStartAndEnd();
		FString GraphName = GetScalarFieldAsString(PayloadObject, TEXT("graph_name")).TrimStartAndEnd();
		const FString NodeComment = GetScalarFieldAsString(PayloadObject, TEXT("node_comment"));
		if (GraphName.IsEmpty())
		{
			GraphName = TEXT("EventGraph");
		}
		if (BlueprintPath.IsEmpty() || EventName.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("blueprint_path and event_name are required."));
			return ExecutionResult;
		}

		UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath);
		if (Blueprint == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return ExecutionResult;
		}

		FName FunctionName;
		UClass* EventOwnerClass = nullptr;
		if (!ResolveBlueprintEventFunction(EventName, FunctionName, EventOwnerClass))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("event_name_unsupported"), TEXT("Allowed events: BeginPlay, Tick, ActorBeginOverlap, ActorEndOverlap."));
			return ExecutionResult;
		}
		if (EventOwnerClass == nullptr || EventOwnerClass->FindFunctionByName(FunctionName) == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("event_function_not_found"), FunctionName.ToString());
			return ExecutionResult;
		}
		if (FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, EventOwnerClass, FunctionName) != nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("event_already_exists"), EventName);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Create Blueprint Event Stub")));
		Blueprint->Modify();

		UEdGraph* TargetGraph = nullptr;
#if WITH_EDITORONLY_DATA
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph != nullptr && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				TargetGraph = Graph;
				break;
			}
		}
#endif
		if (TargetGraph == nullptr)
		{
			TargetGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		}
		if (TargetGraph == nullptr)
		{
			TargetGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (TargetGraph != nullptr)
			{
				FBlueprintEditorUtils::AddUbergraphPage(Blueprint, TargetGraph);
			}
		}
		if (TargetGraph == nullptr)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("event_graph_unavailable"), GraphName);
			return ExecutionResult;
		}

		TargetGraph->Modify();
		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(TargetGraph);
		EventNode->SetFlags(RF_Transactional);
		EventNode->EventReference.SetExternalMember(FunctionName, EventOwnerClass);
		EventNode->bOverrideFunction = true;
		EventNode->CreateNewGuid();
		EventNode->PostPlacedNewNode();
		EventNode->AllocateDefaultPins();
		const FVector2D NodePosition = TargetGraph->GetGoodPlaceForNewNode();
		EventNode->NodePosX = static_cast<int32>(NodePosition.X);
		EventNode->NodePosY = static_cast<int32>(NodePosition.Y);
		if (!NodeComment.IsEmpty())
		{
			EventNode->NodeComment = NodeComment;
			EventNode->bCommentBubblePinned = true;
			EventNode->bCommentBubbleVisible = true;
		}
		TargetGraph->AddNode(EventNode, true, true);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to remove the created Blueprint event node. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("event_name"), EventName);
		ExecutionResult.ResultObject->SetStringField(TEXT("event_function"), FunctionName.ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), EventNode->GetName());
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), Blueprint->GetOutermost()->GetName());
		SetAppliedField(ExecutionResult.ResultObject, TEXT("event_name"), EventName);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("graph_name"), TargetGraph->GetName());
		return ExecutionResult;
	}

	static FEditorOperationExecutionResult ExecuteCompileBlueprint(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("FKismetEditorUtilities.CompileBlueprint"));

		const FString BlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("blueprint_path")));
		if (BlueprintPath.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("blueprint_path is required."));
			return ExecutionResult;
		}

		UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath);
		if (Blueprint == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return ExecutionResult;
		}

		const FString PreviousStatus = BlueprintStatusToOperationString(Blueprint->Status);
		Blueprint->Modify();
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		const FString CompileStatus = BlueprintStatusToOperationString(Blueprint->Status);
		const bool bCompileSuccess = Blueprint->Status != BS_Error;

		ExecutionResult.bSuccess = bCompileSuccess;
		ExecutionResult.ExecutionState = bCompileSuccess ? TEXT("completed") : TEXT("failed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Blueprint compile is an editor action; inspect compiler output and use source control or editor undo for related graph edits if needed.");
		ExecutionResult.ResultObject->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("previous_status"), PreviousStatus);
		ExecutionResult.ResultObject->SetStringField(TEXT("compile_status"), CompileStatus);
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), Blueprint->GetOutermost() != nullptr && Blueprint->GetOutermost()->IsDirty());
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), Blueprint->GetOutermost() != nullptr ? Blueprint->GetOutermost()->GetName() : FString());
		if (!bCompileSuccess)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_compile_failed"), FString::Printf(TEXT("Blueprint compile status: %s"), *CompileStatus));
		}
		return ExecutionResult;
	}

	static bool BindEditorOperationExecutor(FUEAgentEditorToolDefinition& Definition)
	{
		if (Definition.OperationType.Equals(TEXT("rename_selected_asset"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteRenameSelectedAsset);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("apply_static_mesh_basic_settings"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteApplyStaticMeshBasicSettings);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("create_blueprint_asset"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteCreateBlueprintAsset);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("add_blueprint_variable"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteAddBlueprintVariable);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("add_blueprint_component"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteAddBlueprintComponent);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("create_blueprint_event_stub"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteCreateBlueprintEventStub);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("compile_blueprint"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteCompileBlueprint);
			return true;
		}
		return false;
	}

	static FUEAgentEditorToolRegistry BuildEditorOperationToolRegistry()
	{
		FUEAgentEditorToolRegistry Registry;
		for (FUEAgentEditorToolDefinition Definition : FUEAgentEditorToolCatalog::BuildCoreEditorOperationDefinitions())
		{
			if (BindEditorOperationExecutor(Definition))
			{
				Registry.RegisterTool(Definition);
			}
		}
		return Registry;
	}

	static FEditorOperationExecutionResult ExecuteEditorOperationProposal(const FUEAgentProposalSummary& Proposal)
	{
		TSharedPtr<FJsonObject> PayloadObject = ParseJsonObject(Proposal.OperationPayloadJson);
		const FUEAgentEditorToolRegistry Registry = BuildEditorOperationToolRegistry();
		return Registry.ExecuteTool(Proposal.OperationType, PayloadObject, Proposal.ProposalId);
	}

	static void AppendGeneratedDraftItem(const TSharedPtr<FJsonObject>& ItemObject, TArray<FGeneratedDraftItem>& OutDraftItems)
	{
		if (!ItemObject.IsValid())
		{
			return;
		}

		FGeneratedDraftItem DraftItem;
		DraftItem.FilePath = FirstNonEmptyString(ItemObject, { TEXT("file_path"), TEXT("path"), TEXT("filename"), TEXT("virtual_path"), TEXT("name") });
		DraftItem.Label = FirstNonEmptyString(ItemObject, { TEXT("title"), TEXT("label"), TEXT("name"), TEXT("file_path"), TEXT("path") });
		DraftItem.Language = FirstNonEmptyString(ItemObject, { TEXT("language"), TEXT("language_id"), TEXT("syntax"), TEXT("target_type") });
		DraftItem.Code = FirstNonEmptyString(ItemObject, { TEXT("code"), TEXT("content"), TEXT("source"), TEXT("body"), TEXT("text") });
		DraftItem.WriteStatus = FirstNonEmptyString(ItemObject, { TEXT("write_status"), TEXT("status") });
		DraftItem.bIsVirtual = GetBoolFieldOrDefault(ItemObject, TEXT("is_virtual"), true);
		DraftItem.bWrittenToDisk = GetBoolFieldOrDefault(ItemObject, TEXT("written_to_disk"), false);

		if (DraftItem.Label.IsEmpty() && !DraftItem.FilePath.IsEmpty())
		{
			DraftItem.Label = FPaths::GetCleanFilename(DraftItem.FilePath);
		}

		if (!DraftItem.Code.IsEmpty() || !DraftItem.FilePath.IsEmpty() || !DraftItem.Label.IsEmpty())
		{
			OutDraftItems.Add(DraftItem);
		}
	}

	static void AppendGeneratedDraftItemsFromArray(const TArray<TSharedPtr<FJsonValue>>& ItemValues, TArray<FGeneratedDraftItem>& OutDraftItems)
	{
		for (const TSharedPtr<FJsonValue>& ItemValue : ItemValues)
		{
			if (ItemValue.IsValid() && ItemValue->Type == EJson::Object)
			{
				AppendGeneratedDraftItem(ItemValue->AsObject(), OutDraftItems);
			}
		}
	}

	static TArray<FGeneratedDraftItem> ParseGeneratedDraftItems(const FString& JsonText)
	{
		TArray<FGeneratedDraftItem> DraftItems;
		const FString TrimmedJson = JsonText.TrimStartAndEnd();
		if (TrimmedJson.IsEmpty())
		{
			return DraftItems;
		}

		if (TrimmedJson.StartsWith(TEXT("[")))
		{
			TArray<TSharedPtr<FJsonValue>> ItemValues;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TrimmedJson);
			if (FJsonSerializer::Deserialize(Reader, ItemValues))
			{
				AppendGeneratedDraftItemsFromArray(ItemValues, DraftItems);
			}
			return DraftItems;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TrimmedJson);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			return DraftItems;
		}

		const TArray<TSharedPtr<FJsonValue>>* GeneratedItems = nullptr;
		if (RootObject->TryGetArrayField(TEXT("generated_items"), GeneratedItems) && GeneratedItems != nullptr)
		{
			AppendGeneratedDraftItemsFromArray(*GeneratedItems, DraftItems);
			return DraftItems;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (RootObject->TryGetArrayField(TEXT("items"), Items) && Items != nullptr)
		{
			AppendGeneratedDraftItemsFromArray(*Items, DraftItems);
			return DraftItems;
		}

		AppendGeneratedDraftItem(RootObject, DraftItems);
		return DraftItems;
	}

	static FString BuildGeneratedDraftStatusText(const FGeneratedDraftItem& DraftItem, const FString& LanguageCode)
	{
		TArray<FString> Parts;
		const bool bEnglish = UEAgent::IsEnglishOutputLanguage(LanguageCode);

		if (DraftItem.bWrittenToDisk)
		{
			Parts.Add(bEnglish ? TEXT("A write-state flag is present; verify in Debug View before using it.") : TEXT("检测到写入状态标记，请到 Debug View 联调确认后再使用。"));
		}
		else
		{
			Parts.Add(bEnglish ? TEXT("Virtual draft, not written to project files.") : TEXT("虚拟草稿，尚未写入工程文件。"));
		}

		if (!DraftItem.WriteStatus.IsEmpty())
		{
			Parts.Add(FString::Printf(TEXT("write_status=%s"), *DraftItem.WriteStatus));
		}
		if (DraftItem.bIsVirtual)
		{
			Parts.Add(TEXT("is_virtual=true"));
		}
		if (!DraftItem.Language.IsEmpty())
		{
			Parts.Add(FString::Printf(TEXT("language=%s"), *DraftItem.Language));
		}

		return FString::Join(Parts, TEXT("  |  "));
	}

	static FString BuildGeneratedDraftLabel(const FGeneratedDraftItem& DraftItem, const int32 Index, const FString& LanguageCode)
	{
		const FString FallbackLabel = UEAgent::IsEnglishOutputLanguage(LanguageCode)
			? FString::Printf(TEXT("Draft %d"), Index + 1)
			: FString::Printf(TEXT("草稿 %d"), Index + 1);
		const FString BaseLabel = DraftItem.Label.IsEmpty() ? FallbackLabel : DraftItem.Label;
		return FString::Printf(TEXT("%d. %s"), Index + 1, *BaseLabel);
	}

	static FString LocalizeBlockTypeLabel(const FString& BlockType, const FString& LanguageCode)
	{
		FString Normalized = BlockType.IsEmpty() ? TEXT("summary") : BlockType;
		Normalized.ReplaceInline(TEXT("-"), TEXT("_"));
		const FString Key = Normalized.ToLower();

		if (Key == TEXT("summary") || Key == TEXT("overview") || Key == TEXT("result") || Key == TEXT("final_answer"))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("摘要"), TEXT("Summary"));
		}
		if (Key == TEXT("log_summary") || Key == TEXT("logs_summary"))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("日志摘要"), TEXT("Log Summary"));
		}
		if (Key == TEXT("llm_analysis") || Key.Contains(TEXT("analysis")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("LLM 分析结果"), TEXT("LLM Analysis"));
		}
		if (Key.Contains(TEXT("issue")) || Key.Contains(TEXT("finding")) || Key.Contains(TEXT("violation"))
			|| Key.Contains(TEXT("warning")) || Key.Contains(TEXT("risk")) || Key.Contains(TEXT("error")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("问题"), TEXT("Issues"));
		}
		if (Key.Contains(TEXT("recommendation")) || Key.Contains(TEXT("suggestion")) || Key.Contains(TEXT("advice")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("建议"), TEXT("Recommendations"));
		}
		if (Key == TEXT("generated_items") || Key == TEXT("generated_item") || Key.Contains(TEXT("artifact")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("代码草稿"), TEXT("Code Drafts"));
		}
		if (Key.Contains(TEXT("reference")) || Key.Contains(TEXT("citation")) || Key.Contains(TEXT("source")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("参考"), TEXT("References"));
		}
		if (Key.Contains(TEXT("inventory")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("项目资产匹配"), TEXT("Project Inventory Matches"));
		}
		if (Key == TEXT("next_steps") || Key == TEXT("next_step") || Key.Contains(TEXT("step")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("下一步"), TEXT("Next Steps"));
		}
		if (Key.Contains(TEXT("action")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("操作"), TEXT("Actions"));
		}
		if (Key == TEXT("review_scope") || Key == TEXT("scope"))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("文件范围"), TEXT("File Scope"));
		}
		if (Key == TEXT("generation_context") || Key == TEXT("context"))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("生成上下文"), TEXT("Generation Context"));
		}
		if (Key.Contains(TEXT("json")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("JSON 预览"), TEXT("JSON Preview"));
		}
		if (Key.Contains(TEXT("diagnostic")))
		{
			return GetLocalizedUiText(LanguageCode, TEXT("诊断"), TEXT("Diagnostics"));
		}

		FString Fallback = BlockType.IsEmpty() ? TEXT("summary") : BlockType;
		Fallback.ReplaceInline(TEXT("_"), TEXT(" "));
		return Fallback;
	}

	static FString LocalizeStatusLabel(const FString& Status, const FString& LanguageCode)
	{
		const bool bEnglish = UEAgent::IsEnglishOutputLanguage(LanguageCode);
		if (Status.Equals(TEXT("completed"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("success"), ESearchCase::IgnoreCase)
			|| Status.Equals(TEXT("saved"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Completed") : TEXT("已完成");
		}
		if (Status.Equals(TEXT("running"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("in_progress"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Running") : TEXT("运行中");
		}
		if (Status.Equals(TEXT("pending"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("queued"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Pending") : TEXT("等待中");
		}
		if (Status.Equals(TEXT("waiting_confirmation"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Waiting Confirmation") : TEXT("等待确认");
		}
		if (Status.Equals(TEXT("skipped"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Skipped") : TEXT("已跳过");
		}
		if (Status.Equals(TEXT("failed"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Failed") : TEXT("失败");
		}
		if (Status.Equals(TEXT("cancelled"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("canceled"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Cancelled") : TEXT("已取消");
		}
		return Status;
	}
}

void SAgentRootPanel::Construct(const FArguments& InArgs)
{
	StateStore = InArgs._StateStore;
	HttpClient = InArgs._HttpClient;
	ContextCollector = InArgs._ContextCollector;

	check(StateStore.IsValid());
	check(HttpClient.IsValid());
	check(ContextCollector.IsValid());

	for (const EUEAgentFunctionType FunctionType : UEAgent::GetOrderedFunctions())
	{
		FunctionOptions.Add(MakeShared<EUEAgentFunctionType>(FunctionType));
	}

	for (const EUEAgentDebugSection Section : UEAgent::GetOrderedDebugSections())
	{
		DebugSectionOptions.Add(MakeShared<EUEAgentDebugSection>(Section));
	}

	StateStore->OnStateChanged().AddSP(this, &SAgentRootPanel::HandleStateChanged);
	SyncStateSnapshots();

	ChildSlot
	[
		SNew(SBorder)
		.Padding(8.0f)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				BuildTopShell()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(SettingsPanelBox, SBox)
				[
					StateStore->IsSettingsExpanded() ? BuildSettingsPanel() : SNullWidget::NullWidget
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SWidgetSwitcher)
				.WidgetIndex_Lambda([this]() -> int32
				{
					return StateStore->GetActiveViewMode() == EUEAgentViewMode::User ? 0 : 1;
				})
				+ SWidgetSwitcher::Slot()
				[
					BuildUserWorkspace()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildDebugWorkspace()
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
				.Padding(FMargin(8.0f, 6.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return FText::FromString(StateStore->GetStatusMessage()); })
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SProgressBar)
						.Visibility_Lambda([this]() -> EVisibility { return StateStore->IsBusy() ? EVisibility::Visible : EVisibility::Collapsed; })
						.Percent(TOptional<float>())
					]
				]
			]
		]
	];

	RefreshEditorContext();
	InitializeSession();
	PingBackend();
	RefreshBootstrapData();
	RefreshKnowledgeBaseStatus();
	RefreshTaskData();
}

void SAgentRootPanel::HandleStateChanged()
{
	SyncStateSnapshots();

	if (FunctionComboBox.IsValid())
	{
		FunctionComboBox->SetSelectedItem(FindFunctionOption(StateStore->GetActiveFunction()));
	}

	if (DebugSectionComboBox.IsValid())
	{
		DebugSectionComboBox->SetSelectedItem(FindDebugSectionOption(StateStore->GetActiveDebugSection()));
	}

	if (RuntimeProfileComboBox.IsValid())
	{
		RuntimeProfileComboBox->RefreshOptions();
		RuntimeProfileComboBox->SetSelectedItem(FindRuntimeProfileOption(StateStore->GetActiveProfileId()));
	}

	if (ChatListView.IsValid())
	{
		ChatListView->RequestListRefresh();
		if (ChatItems.Num() > 0)
		{
			ChatListView->RequestScrollIntoView(ChatItems.Last());
		}
	}

	if (TaskListView.IsValid())
	{
		TaskListView->RequestListRefresh();
	}

	if (ParameterPanelBox.IsValid())
	{
		ParameterPanelBox->SetContent(BuildFunctionParameterPanel());
	}

	if (ResultCardsBox.IsValid())
	{
		ResultCardsBox->SetContent(BuildResultHighlightsSummary());
	}

	if (SettingsPanelBox.IsValid())
	{
		SettingsPanelBox->SetContent(StateStore->IsSettingsExpanded() ? BuildSettingsPanel() : SNullWidget::NullWidget);
	}

	if (DebugSectionBodyBox.IsValid())
	{
		DebugSectionBodyBox->SetContent(BuildDebugSectionBody());
	}
}

void SAgentRootPanel::SyncStateSnapshots()
{
	ChatItems.Reset();
	for (const TSharedPtr<FUEAgentChatMessage>& Message : StateStore->GetChatMessages())
	{
		if (!Message.IsValid())
		{
			continue;
		}

		if (Message->FunctionId == UEAgent::ToFunctionId(EUEAgentFunctionType::AgentChat)
			|| Message->FunctionId == UEAgent::ToFunctionId(EUEAgentFunctionType::ProjectQA))
		{
			ChatItems.Add(Message);
		}
	}
	TaskItems = StateStore->GetRecentTasks();
	RuntimeProfileOptions = StateStore->GetRuntimeProfiles();
}

void SAgentRootPanel::RefreshEditorContext() const
{
	StateStore->SetEditorContext(ContextCollector->CaptureContext());
}

void SAgentRootPanel::InitializeSession()
{
	const FString CurrentSessionId = StateStore->GetSessionId();
	HttpClient->RequestSessionSummary(CurrentSessionId, [WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this)), StateStore = StateStore, CurrentSessionId](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplySessionSummaryResponse(JsonObject);
			if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
			{
				This->RestoreCurrentSession();
			}
			return;
		}

		if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
		{
			This->HttpClient->CreateSession(CurrentSessionId, [WeakThis, StateStore](bool bCreateSuccess, const FString& CreateMessage, const FString& CreateRawText, TSharedPtr<FJsonObject> CreateJsonObject)
			{
				if (bCreateSuccess)
				{
					StateStore->ApplySessionSummaryResponse(CreateJsonObject);
					if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
					{
						This->RestoreCurrentSession();
					}
				}
				else
				{
					StateStore->AppendSystemMessage(FString::Printf(TEXT("Backend session bootstrap unavailable, using local session. %s"), *CreateMessage), TEXT("Session"));
				}
			});
		}
	});
}

void SAgentRootPanel::RestoreCurrentSession()
{
	const FString SessionId = StateStore->GetSessionId();
	if (SessionId.IsEmpty())
	{
		return;
	}

	HttpClient->RequestSessionHistory(SessionId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplySessionHistoryResponse(JsonObject);
		}
		else
		{
			StateStore->AppendSystemMessage(FString::Printf(TEXT("Session history restore skipped: %s"), *Message), TEXT("Session"));
		}
	});

	HttpClient->RequestSessionTasks(SessionId, [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplySessionTasksResponse(JsonObject);
			if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
			{
				const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
				if (Result.TaskId.IsEmpty() && Result.RunId.IsEmpty() && StateStore->GetRecentTasks().Num() > 0)
				{
					This->LoadTaskDetail(StateStore->GetRecentTasks()[0]);
				}
			}
		}
		else
		{
			StateStore->AppendSystemMessage(FString::Printf(TEXT("Session task restore skipped: %s"), *Message), TEXT("Session"));
		}
	});
}

void SAgentRootPanel::ClearCurrentSession()
{
	const FString PreviousSessionId = StateStore->GetSessionId();
	if (!PreviousSessionId.IsEmpty())
	{
		HttpClient->ClearSession(PreviousSessionId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			if (!bSuccess)
			{
				StateStore->AppendSystemMessage(FString::Printf(TEXT("Backend session clear skipped: %s"), *Message), TEXT("Session"));
			}
		});
	}

	StateStore->ResetSession();
	InitializeSession();
}

void SAgentRootPanel::RefreshBootstrapData() const
{
	HttpClient->RequestBootstrap([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyBootstrapResponse(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});

	HttpClient->RequestCapabilities([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyCapabilitiesResponse(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});

	HttpClient->RequestEditorOperationCapabilities([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyEditorOperationCapabilitiesResponse(JsonObject);
		}
		else
		{
			StateStore->AppendSystemMessage(FString::Printf(TEXT("Editor operation capabilities unavailable: %s"), *Message), TEXT("Editor Operations"));
		}
	});

	HttpClient->RequestRuntimeProfiles([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyRuntimeProfilesResponse(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});

	RefreshSystemSettings();
	RefreshMetrics();

	RefreshSystemAlerts();
}

void SAgentRootPanel::RefreshTaskData() const
{
	HttpClient->RequestRecentTasks([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyRecentTasksResponse(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});

	HttpClient->RequestPendingProposals([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyProposalListResponse(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});
}

void SAgentRootPanel::RefreshSystemAlerts() const
{
	HttpClient->RequestSystemAlerts([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyAlertsResponse(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});
}

void SAgentRootPanel::RefreshSystemSettings() const
{
	HttpClient->RequestSystemSettings([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplySettingsResponse(JsonObject);
		}
		else
		{
			StateStore->AppendSystemMessage(FString::Printf(TEXT("System settings refresh skipped: %s"), *Message), TEXT("Settings"));
		}
	});
}

void SAgentRootPanel::RefreshMetrics() const
{
	HttpClient->RequestMetrics([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& ResponseText)
	{
		if (bSuccess)
		{
			StateStore->ApplyMetricsResponse(ResponseText);
		}
		else
		{
			StateStore->AppendSystemMessage(FString::Printf(TEXT("Metrics refresh skipped: %s"), *Message), TEXT("Metrics"));
		}
	});
}

void SAgentRootPanel::RefreshCodeReviewFiles()
{
	RefreshEditorContext();
	const FUEAgentContextSummary& Context = StateStore->GetEditorContext();
	const FString ProjectRoot = Context.ProjectRoot.IsEmpty()
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())
		: Context.ProjectRoot;
	const FString Query = StateStore->GetFunctionParameters(EUEAgentFunctionType::CodeReview).FileSearchQuery;

	StateStore->SetBusy(true, TEXT("Loading code files..."));
	HttpClient->RequestCodeReviewFiles(ProjectRoot, Query, 200, [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this)), Query](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		StateStore->SetBusy(false);
		if (bSuccess)
		{
			StateStore->ApplyCodeReviewFilesResponse(JsonObject);
			return;
		}

		if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
		{
			This->LoadLocalCodeReviewFilesFallback(Query, FString::Printf(TEXT("Backend code file list failed: %s"), *Message));
		}
	});
}

void SAgentRootPanel::LoadLocalCodeReviewFilesFallback(const FString& Query, const FString& Reason)
{
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	TArray<FString> CandidateFiles;
	const TArray<FString> SourceRoots = { ProjectRoot / TEXT("Source"), ProjectRoot / TEXT("Plugins") };
	const TArray<FString> Patterns = { TEXT("*.h"), TEXT("*.hpp"), TEXT("*.cpp"), TEXT("*.cxx"), TEXT("*.cs") };

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
	const FString QueryText = Query.TrimStartAndEnd();
	TArray<TSharedPtr<FJsonValue>> FileValues;
	for (const FString& CandidateFile : CandidateFiles)
	{
		FString RelativePath = FPaths::ConvertRelativePathToFull(CandidateFile);
		FPaths::NormalizeFilename(RelativePath);
		FPaths::MakePathRelativeTo(RelativePath, *ProjectRoot);
		FPaths::NormalizeFilename(RelativePath);

		if (!QueryText.IsEmpty() && !RelativePath.Contains(QueryText, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TArray<FString> PathParts;
		RelativePath.ParseIntoArray(PathParts, TEXT("/"), true);
		FString ModuleName = StateStore->GetEditorContext().ActiveModule;
		if (PathParts.Num() > 1 && PathParts[0].Equals(TEXT("Source"), ESearchCase::IgnoreCase))
		{
			ModuleName = PathParts[1];
		}
		else if (PathParts.Num() > 1 && PathParts[0].Equals(TEXT("Plugins"), ESearchCase::IgnoreCase))
		{
			const int32 SourceIndex = PathParts.Find(FString(TEXT("Source")));
			if (SourceIndex != INDEX_NONE && PathParts.IsValidIndex(SourceIndex + 1))
			{
				ModuleName = PathParts[SourceIndex + 1];
			}
			else
			{
				ModuleName = PathParts[1];
			}
		}

		TSharedPtr<FJsonObject> FileObject = MakeShared<FJsonObject>();
		FileObject->SetStringField(TEXT("file_path"), RelativePath);
		FileObject->SetStringField(TEXT("relative_path"), RelativePath);
		FileObject->SetStringField(TEXT("label"), FPaths::GetCleanFilename(RelativePath));
		FileObject->SetStringField(TEXT("module_name"), ModuleName);
		FileObject->SetStringField(TEXT("file_type"), FPaths::GetExtension(RelativePath));
		FileValues.Add(MakeShared<FJsonValueObject>(FileObject));

		if (FileValues.Num() >= 200)
		{
			break;
		}
	}

	TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
	ResponseObject->SetArrayField(TEXT("items"), FileValues);
	StateStore->ApplyCodeReviewFilesResponse(ResponseObject);
	const FString Status = FileValues.Num() > 0
		? FString::Printf(TEXT("Loaded %d code files from local project scan. %s"), FileValues.Num(), *Reason)
		: FString::Printf(TEXT("No local code files found in Source/Plugins. %s"), *Reason);
	StateStore->SetStatusMessage(Status);
}

void SAgentRootPanel::RefreshProjectionDataForCurrentResult() const
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	if (!Result.TaskId.IsEmpty())
	{
		HttpClient->RequestTaskUserView(Result.TaskId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			if (bSuccess)
			{
				StateStore->ApplyUserViewProjectionResponse(JsonObject);
			}
		});

		HttpClient->RequestTaskDebugView(Result.TaskId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			if (bSuccess)
			{
				StateStore->ApplyDebugViewProjectionResponse(JsonObject);
			}
		});
		return;
	}

	if (!Result.RunId.IsEmpty())
	{
		HttpClient->RequestRunUserView(Result.RunId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			if (bSuccess)
			{
				StateStore->ApplyUserViewProjectionResponse(JsonObject);
			}
		});

		HttpClient->RequestRunDebugView(Result.RunId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			if (bSuccess)
			{
				StateStore->ApplyDebugViewProjectionResponse(JsonObject);
			}
		});
	}
}

void SAgentRootPanel::ReloadCurrentResultDetail()
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	if (Result.TaskId.IsEmpty() && Result.RunId.IsEmpty())
	{
		StateStore->AppendSystemMessage(TEXT("There is no current task or run to reload."), TEXT("Reload"));
		return;
	}

	TSharedPtr<FUEAgentTaskSummary> CurrentTask = MakeShared<FUEAgentTaskSummary>();
	CurrentTask->TaskId = Result.TaskId;
	CurrentTask->RunId = Result.RunId;
	LoadTaskDetail(CurrentTask);
}

void SAgentRootPanel::RefreshKnowledgeBaseStatus() const
{
	HttpClient->RequestKnowledgeBaseStatus([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyKnowledgeBaseStatus(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});
}

void SAgentRootPanel::PingBackend() const
{
	HttpClient->RequestHealth([StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplyHealthResponse(JsonObject);
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	});
}

void SAgentRootPanel::RefreshTraceDataForCurrentResult() const
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	if (!Result.TaskId.IsEmpty())
	{
		HttpClient->RequestTaskTrace(Result.TaskId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			if (bSuccess)
			{
				StateStore->ApplyTaskTraceResponse(JsonObject);
			}
		});

		HttpClient->RequestTaskArtifacts(Result.TaskId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			if (bSuccess)
			{
				StateStore->ApplyTaskArtifactsResponse(JsonObject);
			}
		});
	}

	if (!Result.RunId.IsEmpty())
	{
		HttpClient->RequestRunEvents(Result.RunId, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& ResponseText)
		{
			if (bSuccess)
			{
				StateStore->ApplyRunEventsResponse(ResponseText);
			}
		});
	}
}

void SAgentRootPanel::SubmitCurrentRequest()
{
	RefreshEditorContext();

	const EUEAgentFunctionType FunctionType = StateStore->GetActiveFunction();
	const FUEAgentFunctionParameters Parameters = StateStore->GetFunctionParameters(FunctionType);
	const bool bUseChatInput = UEAgent::UsesUnifiedChat(FunctionType);
	const FString InputText = bUseChatInput && ChatInputBox.IsValid() ? ChatInputBox->GetText().ToString() : FString();
	const FString ValidationError = GetCurrentRequestValidationError(Parameters, InputText);
	if (!ValidationError.IsEmpty())
	{
		StateStore->AppendSystemMessage(ValidationError, TEXT("Validation"));
		StateStore->SetStatusMessage(ValidationError);
		return;
	}

	FString UserFacingText = !InputText.TrimStartAndEnd().IsEmpty() ? InputText.TrimStartAndEnd()
		: (!Parameters.PrimaryText.TrimStartAndEnd().IsEmpty() ? Parameters.PrimaryText.TrimStartAndEnd() : UEAgent::ToFunctionLabel(FunctionType).ToString());

	if (FunctionType == EUEAgentFunctionType::CodeReview && !Parameters.FilePath.IsEmpty())
	{
		UserFacingText = FString::Printf(TEXT("Review %s"), *Parameters.FilePath);
	}
	else if (FunctionType == EUEAgentFunctionType::AssetsInspect)
	{
		UserFacingText = FString::Printf(TEXT("Inspect %d selected assets"), StateStore->GetEditorContext().SelectedAssetItems.Num());
	}

	if (bUseChatInput)
	{
		StateStore->AppendUserMessage(UserFacingText, FunctionType);
	}
	StateStore->SetBusy(true, TEXT("Sending request..."));

	HttpClient->SubmitFunction(
		FunctionType,
		StateStore->GetEditorContext(),
		Parameters,
		InputText,
		StateStore->GetActiveViewMode(),
		[StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			StateStore->SetBusy(false);
			if (bSuccess)
			{
				StateStore->ApplyUnifiedResponse(JsonObject);
			}
			else
			{
				StateStore->ApplyFailure(Message, RawText);
			}

			if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
			{
				if (This->ChatInputBox.IsValid())
				{
					This->ChatInputBox->SetText(FText::GetEmpty());
				}
				if (bSuccess)
				{
					This->RefreshProjectionDataForCurrentResult();
					This->RefreshTraceDataForCurrentResult();
					This->RefreshTaskData();
				}
			}
		});
}

FString SAgentRootPanel::GetCurrentRequestValidationError(const FUEAgentFunctionParameters& Parameters, const FString& InputText) const
{
	switch (StateStore->GetActiveFunction())
	{
	case EUEAgentFunctionType::AgentChat:
	case EUEAgentFunctionType::ProjectQA:
		return InputText.TrimStartAndEnd().IsEmpty() ? TEXT("Enter a question or instruction before sending.") : FString();

	case EUEAgentFunctionType::CodeReview:
		return Parameters.FilePath.TrimStartAndEnd().IsEmpty() ? TEXT("Select a code file before running Code Review.") : FString();

	case EUEAgentFunctionType::CodeGenerate:
		return Parameters.PrimaryText.TrimStartAndEnd().IsEmpty() ? TEXT("Describe the code requirement before generating.") : FString();

	case EUEAgentFunctionType::LogsAnalyze:
		return Parameters.PrimaryText.TrimStartAndEnd().IsEmpty() && Parameters.LogSource.TrimStartAndEnd().IsEmpty()
			? TEXT("Provide a log file/source path or paste an Error/Fatal snippet before running Logs Analyze.")
			: FString();

	case EUEAgentFunctionType::AssetsInspect:
		return StateStore->GetEditorContext().SelectedAssetItems.Num() == 0 ? TEXT("Select one or more assets in Content Browser before running Assets Inspect.") : FString();

	default:
		return FString();
	}
}

void SAgentRootPanel::LoadTaskDetail(const TSharedPtr<FUEAgentTaskSummary>& TaskSummary)
{
	if (!TaskSummary.IsValid() || (TaskSummary->TaskId.IsEmpty() && TaskSummary->RunId.IsEmpty()))
	{
		return;
	}

	const FString DetailTarget = !TaskSummary->TaskId.IsEmpty() ? TaskSummary->TaskId : TaskSummary->RunId;
	StateStore->SetBusy(true, FString::Printf(TEXT("Loading %s..."), *DetailTarget));

	const FUEAgentHttpClient::FJsonResponseCallback HandleDetailResponse = [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		StateStore->SetBusy(false);
		if (bSuccess)
		{
			StateStore->ApplyUnifiedResponse(JsonObject);
			StateStore->SetActiveViewMode(EUEAgentViewMode::Debug);
			if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
			{
				This->RefreshProjectionDataForCurrentResult();
				This->RefreshTraceDataForCurrentResult();
			}
		}
		else
		{
			StateStore->ApplyFailure(Message, RawText);
		}
	};

	if (!TaskSummary->TaskId.IsEmpty())
	{
		HttpClient->RequestTaskDetail(TaskSummary->TaskId, HandleDetailResponse);
	}
	else
	{
		HttpClient->RequestRunDetail(TaskSummary->RunId, HandleDetailResponse);
	}
}

void SAgentRootPanel::SubmitProposalDecision(const TSharedPtr<FUEAgentProposalSummary>& Proposal, const FString& Decision)
{
	if (!Proposal.IsValid() || Proposal->ProposalId.IsEmpty())
	{
		return;
	}

	if (Proposal->ProposalType.Equals(TEXT("editor_operation"), ESearchCase::IgnoreCase) || !Proposal->OperationType.IsEmpty())
	{
		if (Decision.Equals(TEXT("confirmed"), ESearchCase::IgnoreCase))
		{
			ConfirmEditorOperationProposal(Proposal);
		}
		else
		{
			RejectEditorOperationProposal(Proposal);
		}
		return;
	}

	StateStore->SetBusy(true, FString::Printf(TEXT("Submitting proposal decision: %s"), *Decision));
	HttpClient->SubmitProposalDecision(Proposal->ProposalId, Decision, TEXT("Decision submitted from UE Agent panel."), [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		StateStore->SetBusy(false);
		if (!bSuccess)
		{
			StateStore->ApplyFailure(Message, RawText);
			return;
		}

		if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
		{
			This->RefreshTaskData();
			if (!StateStore->GetLastResult().TaskId.IsEmpty() || !StateStore->GetLastResult().RunId.IsEmpty())
			{
				TSharedPtr<FUEAgentTaskSummary> CurrentTask = MakeShared<FUEAgentTaskSummary>();
				CurrentTask->TaskId = StateStore->GetLastResult().TaskId;
				CurrentTask->RunId = StateStore->GetLastResult().RunId;
				This->LoadTaskDetail(CurrentTask);
			}
		}
	});
}

void SAgentRootPanel::ConfirmEditorOperationProposal(const TSharedPtr<FUEAgentProposalSummary>& Proposal)
{
	if (!Proposal.IsValid() || Proposal->ProposalId.IsEmpty())
	{
		return;
	}

	StateStore->SetBusy(true, FString::Printf(TEXT("Confirming editor operation: %s"), *Proposal->OperationType));
	HttpClient->ConfirmEditorOperationProposal(Proposal->ProposalId, [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this)), Proposal](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (!bSuccess)
		{
			StateStore->SetBusy(false);
			StateStore->ApplyFailure(Message, RawText);
			return;
		}

		if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
		{
			This->ExecuteAndReportEditorOperation(Proposal);
		}
	});
}

void SAgentRootPanel::RejectEditorOperationProposal(const TSharedPtr<FUEAgentProposalSummary>& Proposal)
{
	if (!Proposal.IsValid() || Proposal->ProposalId.IsEmpty())
	{
		return;
	}

	StateStore->SetBusy(true, FString::Printf(TEXT("Rejecting editor operation: %s"), *Proposal->OperationType));
	HttpClient->RejectEditorOperationProposal(Proposal->ProposalId, [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		StateStore->SetBusy(false);
		if (!bSuccess)
		{
			StateStore->ApplyFailure(Message, RawText);
			return;
		}

		if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
		{
			This->RefreshTaskData();
			This->ReloadCurrentResultDetail();
		}
	});
}

void SAgentRootPanel::ExecuteAndReportEditorOperation(const TSharedPtr<FUEAgentProposalSummary>& Proposal)
{
	if (!Proposal.IsValid() || Proposal->ProposalId.IsEmpty())
	{
		StateStore->SetBusy(false);
		return;
	}

	const UEAgentRootPanelPrivate::FEditorOperationExecutionResult ExecutionResult = UEAgentRootPanelPrivate::ExecuteEditorOperationProposal(*Proposal);
	HttpClient->SubmitEditorOperationResult(
		Proposal->ProposalId,
		Proposal->OperationType,
		ExecutionResult.ExecutionState,
		ExecutionResult.bSuccess,
		ExecutionResult.TransactionId,
		ExecutionResult.UndoHint,
		ExecutionResult.ResultObject,
		ExecutionResult.ErrorValues,
		ExecutionResult.MetadataObject,
		[StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
		{
			StateStore->SetBusy(false);
			if (!bSuccess)
			{
				StateStore->ApplyFailure(Message, RawText);
				return;
			}

			if (JsonObject.IsValid())
			{
				StateStore->SetStatusMessage(TEXT("Editor operation result reported to backend."));
			}

			if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
			{
				This->RefreshTaskData();
				This->ReloadCurrentResultDetail();
			}
		});
}

void SAgentRootPanel::SubmitProjectInventorySnapshot()
{
	RefreshEditorContext();
	StateStore->SetBusy(true, TEXT("Collecting Project Inventory snapshot..."));

	const TSharedPtr<FJsonObject> SnapshotObject = ContextCollector->BuildProjectInventorySnapshot();
	HttpClient->SubmitProjectInventorySnapshot(SnapshotObject, [StateStore = StateStore](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		StateStore->SetBusy(false);
		if (bSuccess)
		{
			StateStore->ApplyProjectInventorySnapshotResponse(JsonObject);
			return;
		}

		StateStore->ApplyFailure(Message, RawText);
	});
}

void SAgentRootPanel::ApplyQuickAction(const FUEAgentQuickAction& QuickAction)
{
	if (ChatInputBox.IsValid())
	{
		ChatInputBox->SetText(FText::FromString(QuickAction.SuggestedInput));
	}

	StateStore->AppendSystemMessage(FString::Printf(TEXT("Quick action ready: %s"), *QuickAction.Label), TEXT("Quick Action"));
}

void SAgentRootPanel::ApplyBaseUrl()
{
	if (!BaseUrlTextBox.IsValid())
	{
		return;
	}

	const FString BaseUrl = BaseUrlTextBox->GetText().ToString().TrimStartAndEnd();
	if (BaseUrl.IsEmpty())
	{
		StateStore->AppendSystemMessage(TEXT("Backend Base URL cannot be empty."), TEXT("Settings"));
		return;
	}

	StateStore->SetBackendBaseUrl(BaseUrl);
	RefreshEditorContext();
	PingBackend();
	InitializeSession();
	RefreshBootstrapData();
	RefreshKnowledgeBaseStatus();
	RefreshTaskData();
}

void SAgentRootPanel::SetPreferredOutputLanguage(const FString& InLanguageCode)
{
	const FString PreviousLanguage = StateStore->GetPreferredOutputLanguage();
	StateStore->SetPreferredOutputLanguage(InLanguageCode);
	const FString CurrentLanguage = StateStore->GetPreferredOutputLanguage();
	if (CurrentLanguage == PreviousLanguage)
	{
		return;
	}

	StateStore->SetStatusMessage(UEAgent::IsEnglishOutputLanguage(CurrentLanguage)
		? TEXT("Output language switched to English.")
		: TEXT("输出语言已切换为中文。"));

	HttpClient->CreateSession(StateStore->GetSessionId(), [StateStore = StateStore, CurrentLanguage](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (bSuccess)
		{
			StateStore->ApplySessionSummaryResponse(JsonObject);
			StateStore->SetStatusMessage(UEAgent::IsEnglishOutputLanguage(CurrentLanguage)
				? TEXT("Output language synced to backend session.")
				: TEXT("输出语言已同步到后端会话。"));
			return;
		}

		StateStore->AppendSystemMessage(FString::Printf(TEXT("Session language sync skipped: %s"), *Message), TEXT("Session"));
	});
}

void SAgentRootPanel::ActivateSelectedProfile(const TSharedPtr<FUEAgentRuntimeProfile>& Profile)
{
	if (!Profile.IsValid() || Profile->ProfileId.IsEmpty() || Profile->ProfileId == StateStore->GetActiveProfileId())
	{
		return;
	}

	StateStore->SetBusy(true, FString::Printf(TEXT("Activating profile %s..."), *Profile->ProfileId));
	HttpClient->ActivateProfile(Profile->ProfileId, [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		StateStore->SetBusy(false);
		if (!bSuccess)
		{
			StateStore->ApplyFailure(Message, RawText);
			return;
		}

		if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
		{
			This->RefreshBootstrapData();
		}
	});
}

void SAgentRootPanel::ExportCurrentResponse() const
{
	const FString ExportDir = FPaths::ProjectSavedDir() / TEXT("UEAgentTool");
	IFileManager::Get().MakeDirectory(*ExportDir, true);

	const FString FilePath = ExportDir / FString::Printf(TEXT("ue-agent-response-%s.json"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")));
	if (FFileHelper::SaveStringToFile(StateStore->GetLastResult().RawResponseJson, *FilePath))
	{
		StateStore->AppendSystemMessage(FString::Printf(TEXT("Exported response to %s"), *FilePath), TEXT("Export"));
	}
	else
	{
		StateStore->AppendSystemMessage(TEXT("Failed to export the response JSON."), TEXT("Export"));
	}
}

void SAgentRootPanel::CopyCurrentDebugSection() const
{
	FPlatformApplicationMisc::ClipboardCopy(*StateStore->GetDebugSectionText(StateStore->GetActiveDebugSection()));
	StateStore->AppendSystemMessage(TEXT("Copied the active debug section to the clipboard."), TEXT("Clipboard"));
}

void SAgentRootPanel::OpenTraceOrCopyFallback() const
{
	for (const FUEAgentTraceLink& TraceLink : StateStore->GetLastResult().TraceLinks)
	{
		if (!TraceLink.Url.IsEmpty())
		{
			FPlatformProcess::LaunchURL(*TraceLink.Url, nullptr, nullptr);
			StateStore->AppendSystemMessage(FString::Printf(TEXT("Opened trace link: %s"), *TraceLink.Label), TEXT("Trace"));
			return;
		}
	}

	FPlatformApplicationMisc::ClipboardCopy(*StateStore->GetDebugSectionText(EUEAgentDebugSection::Trace));
	StateStore->AppendSystemMessage(TEXT("Trace summary copied to the clipboard. No trace URL was returned by the backend."), TEXT("Trace"));
}

void SAgentRootPanel::OpenHighlightsWindow()
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	const FString UiLanguage = StateStore->GetEffectiveOutputLanguage();
	if (Result.Blocks.Num() == 0 && Result.Citations.Num() == 0 && Result.QuickActions.Num() == 0 && Result.Proposals.Num() == 0)
	{
		StateStore->SetStatusMessage(UEAgent::IsEnglishOutputLanguage(UiLanguage)
			? TEXT("There are no user_view highlights to display yet.")
			: TEXT("当前还没有可显示的 user_view 高亮结果。"));
		return;
	}

	const FString WindowTitle = Result.UserTitle.IsEmpty()
		? (UEAgent::IsEnglishOutputLanguage(UiLanguage) ? TEXT("UE Agent Highlights") : TEXT("UE Agent 高亮结果"))
		: Result.UserTitle;
	TSharedRef<SWindow> HighlightsWindow = SNew(SWindow)
		.Title(FText::FromString(WindowTitle))
		.ClientSize(FVector2D(760.0f, 640.0f))
		.SupportsMaximize(true)
		.SupportsMinimize(true)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(UEAgent::IsEnglishOutputLanguage(UiLanguage)
							? FString::Printf(TEXT("Task: %s  |  Status: %s"), *Result.TaskId, *UEAgentRootPanelPrivate::LocalizeStatusLabel(Result.TaskStatus, UiLanguage))
							: FString::Printf(TEXT("任务: %s  |  状态: %s"), *Result.TaskId, *UEAgentRootPanelPrivate::LocalizeStatusLabel(Result.TaskStatus, UiLanguage))))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(UEAgent::IsEnglishOutputLanguage(UiLanguage) ? TEXT("Copy Raw Response") : TEXT("复制原始响应")))
						.OnClicked_Lambda([this]()
						{
							FPlatformApplicationMisc::ClipboardCopy(*StateStore->GetLastResult().RawResponseJson);
							StateStore->SetStatusMessage(TEXT("原始响应已复制到剪贴板。"));
							return FReply::Handled();
						})
					]
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						BuildUserResultCards()
					]
				]
			]
		];

	FSlateApplication::Get().AddWindow(HighlightsWindow);
}

void SAgentRootPanel::CancelCurrentRun()
{
	const FString RunId = StateStore->GetLastResult().RunId;
	if (RunId.IsEmpty())
	{
		StateStore->AppendSystemMessage(TEXT("There is no active run to cancel."), TEXT("Cancel Run"));
		return;
	}

	StateStore->SetBusy(true, FString::Printf(TEXT("Cancelling run %s..."), *RunId));
	HttpClient->CancelRun(RunId, [StateStore = StateStore, WeakThis = TWeakPtr<SAgentRootPanel>(SharedThis(this))](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		StateStore->SetBusy(false);
		if (!bSuccess)
		{
			StateStore->ApplyFailure(Message, RawText);
			return;
		}

		StateStore->AppendSystemMessage(TEXT("Cancel request submitted."), TEXT("Cancel Run"));
		if (const TSharedPtr<SAgentRootPanel> This = WeakThis.Pin())
		{
			This->RefreshTaskData();
			This->ReloadCurrentResultDetail();
		}
	});
}

TSharedRef<SWidget> SAgentRootPanel::BuildTopShell()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(FUEAgentToolStyle::Get().GetBrush("UEAgentTool.LargeIcon"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(10.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("UE Agent")))
					.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(this, &SAgentRootPanel::GetBackendStatusColor)
					.Padding(FMargin(8.0f, 4.0f))
					[
						SNew(STextBlock)
						.Text(this, &SAgentRootPanel::GetBackendStatusLabel)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(RuntimeProfileComboBox, SComboBox<TSharedPtr<FUEAgentRuntimeProfile>>)
					.OptionsSource(&RuntimeProfileOptions)
					.OnGenerateWidget_Lambda([](const TSharedPtr<FUEAgentRuntimeProfile>& Profile)
					{
						return SNew(STextBlock).Text(Profile.IsValid() ? FText::FromString(Profile->Label) : FText::FromString(TEXT("Profile")));
					})
					.OnSelectionChanged(this, &SAgentRootPanel::OnProfileSelectionChanged)
					.InitiallySelectedItem(FindRuntimeProfileOption(StateStore->GetActiveProfileId()))
					[
						SNew(STextBlock)
						.Text(this, &SAgentRootPanel::GetProfileComboLabel)
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(12.0f, 0.0f)
				[
					SAssignNew(FunctionComboBox, SComboBox<TSharedPtr<EUEAgentFunctionType>>)
					.OptionsSource(&FunctionOptions)
					.OnGenerateWidget_Lambda([](const TSharedPtr<EUEAgentFunctionType>& Option)
					{
						return SNew(STextBlock).Text(Option.IsValid() ? UEAgent::ToFunctionLabel(*Option) : FText::GetEmpty());
					})
					.OnSelectionChanged(this, &SAgentRootPanel::OnFunctionSelectionChanged)
					.InitiallySelectedItem(FindFunctionOption(StateStore->GetActiveFunction()))
					[
						SNew(STextBlock)
						.Text(this, &SAgentRootPanel::GetFunctionComboLabel)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("中文")))
						.ButtonColorAndOpacity_Lambda([this]()
						{
							return !UEAgent::IsEnglishOutputLanguage(StateStore->GetPreferredOutputLanguage())
								? FLinearColor(0.16f, 0.40f, 0.20f)
								: FLinearColor(0.18f, 0.18f, 0.18f);
						})
						.OnClicked_Lambda([this]()
						{
							SetPreferredOutputLanguage(TEXT("zh-CN"));
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("English")))
						.ButtonColorAndOpacity_Lambda([this]()
						{
							return UEAgent::IsEnglishOutputLanguage(StateStore->GetPreferredOutputLanguage())
								? FLinearColor(0.16f, 0.40f, 0.20f)
								: FLinearColor(0.18f, 0.18f, 0.18f);
						})
						.OnClicked_Lambda([this]()
						{
							SetPreferredOutputLanguage(TEXT("en-US"));
							return FReply::Handled();
						})
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("User View")))
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return StateStore->GetActiveViewMode() == EUEAgentViewMode::User ? FLinearColor(0.10f, 0.32f, 0.56f) : FLinearColor(0.18f, 0.18f, 0.18f);
					})
					.OnClicked_Lambda([this]()
					{
						StateStore->SetActiveViewMode(EUEAgentViewMode::User);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Debug View")))
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return StateStore->GetActiveViewMode() == EUEAgentViewMode::Debug ? FLinearColor(0.52f, 0.29f, 0.11f) : FLinearColor(0.18f, 0.18f, 0.18f);
					})
					.OnClicked_Lambda([this]()
					{
						StateStore->SetActiveViewMode(EUEAgentViewMode::Debug);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Settings")))
					.OnClicked_Lambda([this]()
					{
						StateStore->SetSettingsExpanded(!StateStore->IsSettingsExpanded());
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				BuildContextChips()
			]
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildSettingsPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Backend & Runtime Settings")))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Output Language")))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("中文")))
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return !UEAgent::IsEnglishOutputLanguage(StateStore->GetPreferredOutputLanguage())
							? FLinearColor(0.16f, 0.40f, 0.20f)
							: FLinearColor(0.18f, 0.18f, 0.18f);
					})
					.OnClicked_Lambda([this]()
					{
						SetPreferredOutputLanguage(TEXT("zh-CN"));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("English")))
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return UEAgent::IsEnglishOutputLanguage(StateStore->GetPreferredOutputLanguage())
							? FLinearColor(0.16f, 0.40f, 0.20f)
							: FLinearColor(0.18f, 0.18f, 0.18f);
					})
					.OnClicked_Lambda([this]()
					{
						SetPreferredOutputLanguage(TEXT("en-US"));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						const FString PreferredLanguage = UEAgent::ToOutputLanguageLabel(StateStore->GetPreferredOutputLanguage());
						const FString EffectiveLanguage = UEAgent::ToOutputLanguageLabel(StateStore->GetEffectiveOutputLanguage());
						const FString LanguageSource = StateStore->GetLastLanguageSource();
						return FText::FromString(LanguageSource.IsEmpty()
							? FString::Printf(TEXT("Preferred: %s  |  Current result: %s"), *PreferredLanguage, *EffectiveLanguage)
							: FString::Printf(TEXT("Preferred: %s  |  Current result: %s  |  Source: %s"), *PreferredLanguage, *EffectiveLanguage, *LanguageSource));
					})
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SAssignNew(BaseUrlTextBox, SEditableTextBox)
					.Text(FText::FromString(StateStore->GetBackendBaseUrl()))
					.HintText(FText::FromString(TEXT("http://127.0.0.1:8000")))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Apply")))
					.OnClicked_Lambda([this]()
					{
						ApplyBaseUrl();
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("KB Status")))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(StateStore->GetEditorContext().KnowledgeBaseStatus); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Restore Session")))
					.OnClicked_Lambda([this]()
					{
						InitializeSession();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("New Session")))
					.OnClicked_Lambda([this]()
					{
						ClearCurrentSession();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Refresh All")))
					.OnClicked_Lambda([this]()
					{
						PingBackend();
						InitializeSession();
						RefreshBootstrapData();
						RefreshKnowledgeBaseStatus();
						RefreshTaskData();
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				.Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Session ID")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return FText::FromString(StateStore->GetSessionId()); })
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Session Status")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return FText::FromString(FString::Printf(TEXT("%s  |  %s"),
								StateStore->IsSessionSynchronized() ? TEXT("synced") : TEXT("local"),
								*StateStore->GetSessionStatusText()));
						})
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SWrapBox)
				.UseAllottedWidth(true)
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Settings"))).OnClicked_Lambda([this]() { RefreshSystemSettings(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Metrics"))).OnClicked_Lambda([this]() { RefreshMetrics(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Alerts"))).OnClicked_Lambda([this]() { RefreshSystemAlerts(); return FReply::Handled(); })]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("System Settings Snapshot")))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(SMultiLineEditableTextBox)
					.Text_Lambda([this]()
					{
						const FString Text = StateStore->GetSettingsSnapshotJson();
						return FText::FromString(Text.IsEmpty() ? TEXT("{}") : Text);
					})
					.IsReadOnly(true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("Session Summary")))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(SMultiLineEditableTextBox)
					.Text_Lambda([this]()
					{
						const FString Text = StateStore->GetSessionSummaryJson();
						return FText::FromString(Text.IsEmpty() ? TEXT("{}") : Text);
					})
					.IsReadOnly(true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("Session History")))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(SMultiLineEditableTextBox)
					.Text_Lambda([this]()
					{
						const FString Text = StateStore->GetSessionHistoryJson();
						return FText::FromString(Text.IsEmpty() ? TEXT("{}") : Text);
					})
					.IsReadOnly(true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("Prometheus Metrics")))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(SMultiLineEditableTextBox)
					.Text_Lambda([this]()
					{
						const FString Text = StateStore->GetMetricsSnapshotText();
						return FText::FromString(Text.IsEmpty() ? TEXT("# metrics not loaded") : Text);
					})
					.IsReadOnly(true)
				]
			]
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildUserWorkspace()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SAssignNew(ParameterPanelBox, SBox)
			[
				BuildFunctionParameterPanel()
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.Visibility_Lambda([this]()
			{
				return UEAgent::UsesUnifiedChat(StateStore->GetActiveFunction()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(6.0f)
			[
				SAssignNew(ChatListView, SListView<TSharedPtr<FUEAgentChatMessage>>)
				.ListItemsSource(&ChatItems)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SAgentRootPanel::OnGenerateChatRow)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SAssignNew(ResultCardsBox, SBox)
			[
				BuildResultHighlightsSummary()
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			BuildContextChips()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.Visibility_Lambda([this]()
			{
				return UEAgent::UsesUnifiedChat(StateStore->GetActiveFunction()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(8.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(ChatInputBox, SMultiLineEditableTextBox)
					.HintText(FText::FromString(TEXT("Type your message, question or task summary here.")))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("User View only renders user-friendly `user_view` output. Full diagnostics stay in Debug View.")))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text_Lambda([this]() { return UEAgent::ToSubmitLabel(StateStore->GetActiveFunction()); })
						.IsEnabled_Lambda([this]() { return !StateStore->IsBusy(); })
						.OnClicked_Lambda([this]()
						{
							SubmitCurrentRequest();
							return FReply::Handled();
						})
					]
				]
			]
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildDebugWorkspace()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(8.0f)
			[
				SNew(SWrapBox)
				.UseAllottedWidth(true)
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Ping Backend"))).OnClicked_Lambda([this]() { PingBackend(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Bootstrap"))).OnClicked_Lambda([this]() { RefreshBootstrapData(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Settings"))).OnClicked_Lambda([this]() { RefreshSystemSettings(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Metrics"))).OnClicked_Lambda([this]() { RefreshMetrics(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Alerts"))).OnClicked_Lambda([this]() { RefreshSystemAlerts(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Tasks"))).OnClicked_Lambda([this]() { RefreshTaskData(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh KB"))).OnClicked_Lambda([this]() { RefreshKnowledgeBaseStatus(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Submit Inventory"))).OnClicked_Lambda([this]() { SubmitProjectInventorySnapshot(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Restore Session"))).OnClicked_Lambda([this]() { InitializeSession(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Reload Detail"))).OnClicked_Lambda([this]() { ReloadCurrentResultDetail(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Refresh Projections"))).OnClicked_Lambda([this]() { RefreshProjectionDataForCurrentResult(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Open Trace"))).OnClicked_Lambda([this]() { OpenTraceOrCopyFallback(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Copy JSON"))).OnClicked_Lambda([this]() { CopyCurrentDebugSection(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Retry"))).OnClicked_Lambda([this]() { SubmitCurrentRequest(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Cancel Run"))).OnClicked_Lambda([this]() { CancelCurrentRun(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Clear Session"))).OnClicked_Lambda([this]() { ClearCurrentSession(); return FReply::Handled(); })]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[SNew(SButton).Text(FText::FromString(TEXT("Export Response"))).OnClicked_Lambda([this]() { ExportCurrentResponse(); return FReply::Handled(); })]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SSplitter)
			+ SSplitter::Slot()
			.Value(0.28f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(6.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Task / Run List")))
						.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SAssignNew(TaskListView, SListView<TSharedPtr<FUEAgentTaskSummary>>)
						.ListItemsSource(&TaskItems)
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SAgentRootPanel::OnGenerateTaskRow)
						.OnSelectionChanged(this, &SAgentRootPanel::OnTaskSelectionChanged)
					]
				]
			]
			+ SSplitter::Slot()
			.Value(0.72f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.45f)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SAssignNew(DebugSectionComboBox, SComboBox<TSharedPtr<EUEAgentDebugSection>>)
						.OptionsSource(&DebugSectionOptions)
						.OnGenerateWidget_Lambda([](const TSharedPtr<EUEAgentDebugSection>& Option)
						{
							return SNew(STextBlock).Text(Option.IsValid() ? UEAgent::ToDebugSectionLabel(*Option) : FText::GetEmpty());
						})
						.OnSelectionChanged(this, &SAgentRootPanel::OnDebugSectionSelectionChanged)
						.InitiallySelectedItem(FindDebugSectionOption(StateStore->GetActiveDebugSection()))
						[
							SNew(STextBlock)
							.Text(this, &SAgentRootPanel::GetDebugSectionComboLabel)
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.55f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return FText::FromString(FString::Printf(TEXT("Task: %s  |  Run: %s"), *StateStore->GetLastResult().TaskId, *StateStore->GetLastResult().RunId));
						})
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(DebugSectionBodyBox, SBox)
					[
						BuildDebugSectionBody()
					]
				]
			]
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildFunctionParameterPanel()
{
	return SNew(SExpandableArea)
		.AreaTitle(FText::FromString(TEXT("Function Workspace")))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				BuildFunctionSpecificForm(StateStore->GetActiveFunction())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]()
				{
					return UEAgent::UsesUnifiedChat(StateStore->GetActiveFunction()) ? EVisibility::Collapsed : EVisibility::Visible;
				})
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Tool panels submit structured payloads; full request and trace remain in Debug View.")))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text_Lambda([this]() { return UEAgent::ToSubmitLabel(StateStore->GetActiveFunction()); })
					.IsEnabled_Lambda([this]() { return !StateStore->IsBusy(); })
					.OnClicked_Lambda([this]()
					{
						SubmitCurrentRequest();
						return FReply::Handled();
					})
				]
			]
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildResultHighlightsSummary()
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	const bool bHasHighlights = Result.Blocks.Num() > 0 || Result.Citations.Num() > 0 || Result.QuickActions.Num() > 0 || Result.Proposals.Num() > 0;
	if (!bHasHighlights)
	{
		return SNullWidget::NullWidget;
	}

	const FString UiLanguage = StateStore->GetEffectiveOutputLanguage();
	const FString Title = Result.UserTitle.IsEmpty()
		? UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("最新结果"), TEXT("Latest Result"))
		: Result.UserTitle;
	const FString MetaText = UEAgent::IsEnglishOutputLanguage(UiLanguage)
		? FString::Printf(TEXT("Blocks: %d  |  Citations: %d  |  Actions: %d  |  Proposals: %d  |  Status: %s"),
			Result.Blocks.Num(),
			Result.Citations.Num(),
			Result.QuickActions.Num(),
			Result.Proposals.Num(),
			*UEAgentRootPanelPrivate::LocalizeStatusLabel(Result.TaskStatus, UiLanguage))
		: FString::Printf(TEXT("板块: %d  |  引用: %d  |  操作: %d  |  提案: %d  |  状态: %s"),
			Result.Blocks.Num(),
			Result.Citations.Num(),
			Result.QuickActions.Num(),
			Result.Proposals.Num(),
			*UEAgentRootPanelPrivate::LocalizeStatusLabel(Result.TaskStatus, UiLanguage));

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Title))
					.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(MetaText))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("打开高亮"), TEXT("Open Highlights"))))
				.OnClicked_Lambda([this]()
				{
					OpenHighlightsWindow();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("调试视图"), TEXT("Debug View"))))
				.OnClicked_Lambda([this]()
				{
					StateStore->SetActiveViewMode(EUEAgentViewMode::Debug);
					return FReply::Handled();
				})
			]
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildUserResultCards()
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	if (Result.Blocks.Num() == 0 && Result.Citations.Num() == 0 && Result.QuickActions.Num() == 0)
	{
		if (!UEAgent::UsesUnifiedChat(StateStore->GetActiveFunction()))
		{
			const FString UiLanguage = StateStore->GetEffectiveOutputLanguage();
			return BuildEmptyState(UEAgent::IsEnglishOutputLanguage(UiLanguage)
				? TEXT("Run the selected tool to show user_view results here.")
				: TEXT("运行当前工具后，会在这里显示 user_view 结果。"));
		}
		return SNullWidget::NullWidget;
	}

	TSharedRef<SVerticalBox> Container = SNew(SVerticalBox);
	for (const FUEAgentUserViewBlock& Block : Result.Blocks)
	{
		Container->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			BuildUserBlockCard(Block)
		];
	}

	if (Result.Proposals.Num() > 0)
	{
		Container->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			BuildProposalCards()
		];
	}

	if (Result.Citations.Num() > 0)
	{
		TSharedRef<SWrapBox> CitationWrap = SNew(SWrapBox).UseAllottedWidth(true);
		for (const FUEAgentCitation& Citation : Result.Citations)
		{
			const FString Label = Citation.Source.IsEmpty()
				? Citation.Title
				: FString::Printf(TEXT("%s | %s"), *Citation.Title, *Citation.Source);
			CitationWrap->AddSlot()
			.Padding(0.0f, 0.0f, 6.0f, 6.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FLinearColor(0.14f, 0.20f, 0.15f))
				.Padding(FMargin(8.0f, 4.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.ToolTipText(FText::FromString(Citation.Snippet))
				]
			];
		}

		Container->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				CitationWrap
			]
		];
	}

	if (Result.QuickActions.Num() > 0)
	{
		TSharedRef<SWrapBox> ActionWrap = SNew(SWrapBox).UseAllottedWidth(true);
		for (const FUEAgentQuickAction& QuickAction : Result.QuickActions)
		{
			ActionWrap->AddSlot()
			.Padding(0.0f, 0.0f, 6.0f, 6.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(QuickAction.Label))
				.ToolTipText(FText::FromString(QuickAction.PayloadJson))
				.OnClicked_Lambda([this, QuickAction]()
				{
					ApplyQuickAction(QuickAction);
					return FReply::Handled();
				})
			];
		}

		Container->AddSlot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				ActionWrap
			]
		];
	}

	return Container;
}

TSharedRef<SWidget> SAgentRootPanel::BuildGeneratedItemsDrafts(const FUEAgentUserViewBlock& Block, const FString& UiLanguage) const
{
	const TArray<UEAgentRootPanelPrivate::FGeneratedDraftItem> DraftItems = UEAgentRootPanelPrivate::ParseGeneratedDraftItems(Block.JsonPreview);
	if (DraftItems.Num() == 0)
	{
		TSharedRef<SVerticalBox> FallbackBox = SNew(SVerticalBox);
		for (const FString& Item : Block.Items)
		{
			FallbackBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("- %s"), *Item)))
				.AutoWrapText(true)
			];
		}

		FallbackBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("未找到可直接预览的 code 字段；请在 Debug View 查看原始响应。"), TEXT("No previewable code field was found; check the raw response in Debug View."))))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
		return FallbackBox;
	}

	TSharedRef<int32> SelectedDraftIndex = MakeShared<int32>(0);
	const int32 DraftCount = DraftItems.Num();
	const bool bEnglish = UEAgent::IsEnglishOutputLanguage(UiLanguage);

	TSharedRef<SVerticalBox> DraftsBox = SNew(SVerticalBox);
	DraftsBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		SNew(STextBlock)
		.Text(FText::FromString(bEnglish
			? TEXT("Backend returned virtual code drafts. They are not written to the project until you copy and apply them manually.")
			: TEXT("后端返回的是虚拟代码草稿；前端只预览，不会自动写入工程。")))
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.AutoWrapText(true)
	];

	TSharedRef<SWrapBox> DraftTabs = SNew(SWrapBox).UseAllottedWidth(true);
	for (int32 DraftIndex = 0; DraftIndex < DraftItems.Num(); ++DraftIndex)
	{
		const UEAgentRootPanelPrivate::FGeneratedDraftItem& DraftItem = DraftItems[DraftIndex];
		const FString DraftLabel = UEAgentRootPanelPrivate::BuildGeneratedDraftLabel(DraftItem, DraftIndex, UiLanguage);
		const FString TooltipText = DraftItem.FilePath.IsEmpty() ? DraftLabel : DraftItem.FilePath;
		DraftTabs->AddSlot()
		.Padding(0.0f, 0.0f, 6.0f, 6.0f)
		[
			SNew(SButton)
			.Text(FText::FromString(DraftLabel))
			.ToolTipText(FText::FromString(TooltipText))
			.ButtonColorAndOpacity_Lambda([SelectedDraftIndex, DraftIndex]()
			{
				return *SelectedDraftIndex == DraftIndex ? FLinearColor(0.14f, 0.38f, 0.62f) : FLinearColor(0.18f, 0.18f, 0.18f);
			})
			.OnClicked_Lambda([SelectedDraftIndex, DraftIndex]()
			{
				*SelectedDraftIndex = DraftIndex;
				return FReply::Handled();
			})
		];
	}

	DraftsBox->AddSlot()
	.AutoHeight()
	[
		DraftTabs
	];

	TSharedRef<SWidgetSwitcher> DraftSwitcher = SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([SelectedDraftIndex, DraftCount]() -> int32
		{
			return FMath::Clamp(*SelectedDraftIndex, 0, DraftCount - 1);
		});

	for (int32 DraftIndex = 0; DraftIndex < DraftItems.Num(); ++DraftIndex)
	{
		const UEAgentRootPanelPrivate::FGeneratedDraftItem& DraftItem = DraftItems[DraftIndex];
		const FString DraftLabel = UEAgentRootPanelPrivate::BuildGeneratedDraftLabel(DraftItem, DraftIndex, UiLanguage);
		const FString DraftStatus = UEAgentRootPanelPrivate::BuildGeneratedDraftStatusText(DraftItem, UiLanguage);
		const FString CodeText = DraftItem.Code;
		const FString PathText = DraftItem.FilePath;

		TSharedRef<SVerticalBox> DraftDetail = SNew(SVerticalBox);
		DraftDetail->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(DraftLabel))
			.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			.AutoWrapText(true)
		];

		if (!PathText.IsEmpty())
		{
			DraftDetail->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%s%s"),
					*UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("建议路径："), TEXT("Suggested path: ")),
					*PathText)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			];
		}

		DraftDetail->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(DraftStatus))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];

		DraftDetail->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 6.0f)
		[
			SNew(SWrapBox)
			.UseAllottedWidth(true)
			+ SWrapBox::Slot()
			.Padding(0.0f, 0.0f, 6.0f, 6.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("复制代码"), TEXT("Copy Code"))))
				.IsEnabled(!CodeText.IsEmpty())
				.OnClicked_Lambda([this, CodeText]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*CodeText);
					StateStore->SetStatusMessage(TEXT("Code draft copied to clipboard."));
					return FReply::Handled();
				})
			]
			+ SWrapBox::Slot()
			.Padding(0.0f, 0.0f, 6.0f, 6.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("复制建议路径"), TEXT("Copy Suggested Path"))))
				.IsEnabled(!PathText.IsEmpty())
				.OnClicked_Lambda([this, PathText]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*PathText);
					StateStore->SetStatusMessage(TEXT("Suggested draft path copied to clipboard."));
					return FReply::Handled();
				})
			]
		];

		DraftDetail->AddSlot()
		.AutoHeight()
		[
			SNew(SBox)
			.MaxDesiredHeight(420.0f)
			[
				CodeText.IsEmpty()
				? StaticCastSharedRef<SWidget>(
					SNew(STextBlock)
					.Text(FText::FromString(UEAgentRootPanelPrivate::GetLocalizedUiText(UiLanguage, TEXT("此草稿没有返回 code 字段。"), TEXT("This draft did not include a code field."))))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true))
				: StaticCastSharedRef<SWidget>(
					SNew(SMultiLineEditableTextBox)
					.Text(FText::FromString(CodeText))
					.IsReadOnly(true))
			]
		];

		DraftSwitcher->AddSlot()
		[
			DraftDetail
		];
	}

	DraftsBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 6.0f, 0.0f, 0.0f)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			DraftSwitcher
		]
	];

	return DraftsBox;
}

TSharedRef<SWidget> SAgentRootPanel::BuildUserBlockCard(const FUEAgentUserViewBlock& Block) const
{
	const FString UiLanguage = StateStore->GetEffectiveOutputLanguage();
	const FString BlockTypeLabel = UEAgentRootPanelPrivate::LocalizeBlockTypeLabel(Block.BlockType, UiLanguage);

	const bool bWarningBlock = Block.BlockType.Contains(TEXT("warning"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("risk"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("error"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("issue"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("finding"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("violation"), ESearchCase::IgnoreCase);
	const bool bActionBlock = Block.BlockType.Contains(TEXT("action"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("recommendation"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("suggestion"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("rename"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("check"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("step"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("list"), ESearchCase::IgnoreCase);
	const bool bAnalysisBlock = Block.BlockType.Contains(TEXT("analysis"), ESearchCase::IgnoreCase);
	const bool bJsonBlock = Block.BlockType.Contains(TEXT("json"), ESearchCase::IgnoreCase);
	const bool bEvidenceBlock = Block.BlockType.Contains(TEXT("citation"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("reference"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Contains(TEXT("source"), ESearchCase::IgnoreCase);
	const bool bGeneratedItemsBlock = Block.BlockType.Equals(TEXT("generated_items"), ESearchCase::IgnoreCase)
		|| Block.BlockType.Equals(TEXT("generated_item"), ESearchCase::IgnoreCase);

	const FLinearColor AccentColor = bWarningBlock ? FLinearColor(0.64f, 0.25f, 0.18f)
		: (bAnalysisBlock ? FLinearColor(0.55f, 0.39f, 0.12f)
			: (bActionBlock ? FLinearColor(0.21f, 0.42f, 0.20f)
				: (bJsonBlock ? FLinearColor(0.34f, 0.34f, 0.34f)
					: (bEvidenceBlock ? FLinearColor(0.11f, 0.42f, 0.44f)
						: (bGeneratedItemsBlock ? FLinearColor(0.24f, 0.30f, 0.62f) : FLinearColor(0.18f, 0.34f, 0.54f))))));
	const FLinearColor BackgroundColor = bWarningBlock ? FLinearColor(0.20f, 0.10f, 0.08f)
		: (bAnalysisBlock ? FLinearColor(0.17f, 0.13f, 0.06f)
			: (bActionBlock ? FLinearColor(0.10f, 0.16f, 0.10f)
				: (bJsonBlock ? FLinearColor(0.14f, 0.14f, 0.14f)
					: (bEvidenceBlock ? FLinearColor(0.08f, 0.15f, 0.16f)
						: (bGeneratedItemsBlock ? FLinearColor(0.10f, 0.10f, 0.18f) : FLinearColor(0.09f, 0.12f, 0.17f))))));
	const FString ItemPrefix = bWarningBlock ? TEXT("! ") : TEXT("- ");

	TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
	Content->AddSlot()
	.AutoHeight()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(AccentColor)
			.Padding(FMargin(8.0f, 3.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(BlockTypeLabel))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]
		]
	];

	if (!Block.Title.IsEmpty())
	{
		Content->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Block.Title))
			.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
		];
	}

	if (!Block.Text.IsEmpty())
	{
		Content->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Block.Text))
			.AutoWrapText(true)
		];
	}

	if (bGeneratedItemsBlock)
	{
		Content->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			BuildGeneratedItemsDrafts(Block, UiLanguage)
		];
	}
	else
	{
		for (const FString& Item : Block.Items)
		{
			Content->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%s%s"), *ItemPrefix, *Item)))
				.AutoWrapText(true)
			];
		}
	}

	if (Block.BlockType.Equals(TEXT("json_preview"), ESearchCase::IgnoreCase) && !Block.JsonPreview.IsEmpty())
	{
		Content->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(Block.JsonPreview))
			.IsReadOnly(true)
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(BackgroundColor)
		.Padding(10.0f)
		[
			Content
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildFunctionSpecificForm(const EUEAgentFunctionType FunctionType)
{
	FUEAgentFunctionParameters& Parameters = StateStore->EditFunctionParameters(FunctionType);

	auto MakeSingleLine = [](const FString& InitialText, const FText& HintText, TFunction<void(const FString&)> OnChanged)
	{
		return SNew(SEditableTextBox)
			.Text(FText::FromString(InitialText))
			.HintText(HintText)
			.OnTextChanged_Lambda([OnChanged](const FText& NewText)
			{
				OnChanged(NewText.ToString());
			});
	};

	auto MakeMultiLine = [](const FString& InitialText, const FText& HintText, TFunction<void(const FString&)> OnChanged)
	{
		return SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(InitialText))
			.HintText(HintText)
			.OnTextChanged_Lambda([OnChanged](const FText& NewText)
			{
				OnChanged(NewText.ToString());
			});
	};

	switch (FunctionType)
	{
	case EUEAgentFunctionType::AgentChat:
	case EUEAgentFunctionType::ProjectQA:
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SCheckBox)
				.IsChecked(Parameters.bIncludeContext ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged_Lambda([this, FunctionType](ECheckBoxState NewState)
				{
					StateStore->EditFunctionParameters(FunctionType).bIncludeContext = NewState == ECheckBoxState::Checked;
				})
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Include current editor context")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Use the bottom input for project questions or general agent chat. Routing is decided by the backend.")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			];

	case EUEAgentFunctionType::CodeReview:
	{
		TSharedRef<SVerticalBox> FileListBox = SNew(SVerticalBox);
		const TArray<TSharedPtr<FUEAgentCodeFileItem>>& CodeFiles = StateStore->GetCodeReviewFiles();
		if (CodeFiles.Num() == 0)
		{
			FileListBox->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("No files loaded. Search or refresh to load Source/Plugins code files.")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			];
		}
		for (const TSharedPtr<FUEAgentCodeFileItem>& CodeFile : CodeFiles)
		{
			if (!CodeFile.IsValid())
			{
				continue;
			}

			const FString FileTitle = CodeFile->Label.IsEmpty() ? FPaths::GetCleanFilename(CodeFile->FilePath) : CodeFile->Label;
			const FString FileMeta = FString::Printf(TEXT("%s  |  %s"), *CodeFile->FileType, *CodeFile->ModuleName);
			const FString DisplayPath = CodeFile->RelativePath.IsEmpty() ? CodeFile->FilePath : CodeFile->RelativePath;
			FileListBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SButton)
				.ButtonColorAndOpacity_Lambda([this, CodeFile]()
				{
					return StateStore->GetFunctionParameters(EUEAgentFunctionType::CodeReview).FilePath == CodeFile->FilePath
						? FLinearColor(0.10f, 0.32f, 0.56f)
						: FLinearColor(0.18f, 0.18f, 0.18f);
				})
				.OnClicked_Lambda([this, CodeFile]()
				{
					FUEAgentFunctionParameters& Draft = StateStore->EditFunctionParameters(EUEAgentFunctionType::CodeReview);
					Draft.FilePath = CodeFile->FilePath;
					Draft.PrimaryText = CodeFile->FilePath;
					StateStore->SetStatusMessage(FString::Printf(TEXT("Selected code file: %s"), *CodeFile->FilePath));
					return FReply::Handled();
				})
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(FileTitle))
						.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(DisplayPath))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FileMeta))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
			];
		}

		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SEditableTextBox)
					.Text(FText::FromString(Parameters.FileSearchQuery))
					.HintText(FText::FromString(TEXT("Search C++ file, class, or module")))
					.OnTextChanged_Lambda([this](const FText& NewText)
					{
						StateStore->EditFunctionParameters(EUEAgentFunctionType::CodeReview).FileSearchQuery = NewText.ToString();
					})
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
					{
						StateStore->EditFunctionParameters(EUEAgentFunctionType::CodeReview).FileSearchQuery = NewText.ToString();
						if (CommitType == ETextCommit::OnEnter)
						{
							RefreshCodeReviewFiles();
						}
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Refresh Files")))
					.IsEnabled_Lambda([this]() { return !StateStore->IsBusy(); })
					.OnClicked_Lambda([this]()
					{
						RefreshCodeReviewFiles();
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeSingleLine(Parameters.FocusArea, FText::FromString(TEXT("Optional focus: Lifecycle / Performance / API / General")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).FocusArea = Value;
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					const FString FilePath = StateStore->GetFunctionParameters(EUEAgentFunctionType::CodeReview).FilePath;
					return FText::FromString(FilePath.IsEmpty() ? TEXT("Selected file: none") : FString::Printf(TEXT("Selected file: %s"), *FilePath));
				})
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SBox)
				.MaxDesiredHeight(320.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(6.0f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							FileListBox
						]
					]
				]
			];
	}

	case EUEAgentFunctionType::CodeGenerate:
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeSingleLine(Parameters.TargetType, FText::FromString(TEXT("ue_cpp / cpp / blueprint / tooling")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).TargetType = Value;
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SCheckBox)
				.IsChecked(Parameters.bCreateWriteProposal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged_Lambda([this, FunctionType](ECheckBoxState NewState)
				{
					StateStore->EditFunctionParameters(FunctionType).bCreateWriteProposal = NewState == ECheckBoxState::Checked;
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Create write proposal after generation (requires explicit confirmation)")))
					.AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Default mode shows virtual drafts only. If write proposal is enabled, the backend still waits for Confirm before writing files.")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				MakeMultiLine(Parameters.PrimaryText, FText::FromString(TEXT("Describe the code to generate.")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).PrimaryText = Value;
				})
			];

	case EUEAgentFunctionType::LogsAnalyze:
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeSingleLine(Parameters.LogSource, FText::FromString(TEXT("Log source / file path: Output Log / Saved/Logs/RushBa.log / crashcontext")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).LogSource = Value;
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Provide either a log file/source path or a pasted Error/Fatal snippet. Full log text is no longer required.")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeMultiLine(Parameters.PrimaryText, FText::FromString(TEXT("Error snippet / pasted text: a few Error/Fatal lines or a longer log excerpt")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).PrimaryText = Value;
				})
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSingleLine(Parameters.SecondaryText, FText::FromString(TEXT("Optional notes / attachment paths separated by semicolon")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).SecondaryText = Value;
				})
			];

	case EUEAgentFunctionType::ConfigGenerate:
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeSingleLine(Parameters.ObjectType, FText::FromString(TEXT("Object type")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).ObjectType = Value;
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeSingleLine(Parameters.ExportFormat, FText::FromString(TEXT("json / ini / yaml")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).ExportFormat = Value;
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeMultiLine(Parameters.SchemaText, FText::FromString(TEXT("Paste schema here.")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).SchemaText = Value;
				})
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				MakeMultiLine(Parameters.PrimaryText, FText::FromString(TEXT("Describe the config requirement.")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).PrimaryText = Value;
				})
			];

	case EUEAgentFunctionType::ConfigValidate:
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeMultiLine(Parameters.SchemaText, FText::FromString(TEXT("Paste schema here.")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).SchemaText = Value;
				})
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				MakeMultiLine(Parameters.JsonText, FText::FromString(TEXT("Paste config JSON here.")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).JsonText = Value;
				})
			];

	case EUEAgentFunctionType::AssetsInspect:
	case EUEAgentFunctionType::AssetsPlan:
	{
		TSharedRef<SVerticalBox> SelectedAssetsBox = SNew(SVerticalBox);
		const TArray<FUEAgentAssetContextItem>& AssetItems = StateStore->GetEditorContext().SelectedAssetItems;
		if (AssetItems.Num() == 0)
		{
			SelectedAssetsBox->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("No assets selected in Content Browser.")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		}
		for (const FUEAgentAssetContextItem& AssetItem : AssetItems)
		{
			SelectedAssetsBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(6.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(AssetItem.AssetName.IsEmpty() ? AssetItem.AssetPath : AssetItem.AssetName))
						.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("Path: %s  |  Type: %s  |  Package: %s"), *AssetItem.AssetPath, *AssetItem.AssetType, *AssetItem.PackagePath)))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("Dependencies: %d  |  Referencers: %d"), AssetItem.Dependencies.Num(), AssetItem.Referencers.Num())))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
			];
		}

		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return FText::FromString(FString::Printf(TEXT("Selected assets: %d"), StateStore->GetEditorContext().SelectedAssetItems.Num()));
				})
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SBox)
				.MaxDesiredHeight(260.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SelectedAssetsBox
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeMultiLine(Parameters.PrimaryText, FText::FromString(TEXT("Optional note for asset inspection.")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).PrimaryText = Value;
				})
			];
	}

	case EUEAgentFunctionType::PerformanceAnalyze:
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeMultiLine(Parameters.PrimaryText, FText::FromString(TEXT("Paste report_text here.")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).PrimaryText = Value;
				})
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSingleLine(Parameters.SecondaryText, FText::FromString(TEXT("Optional insights summary")), [this, FunctionType](const FString& Value)
				{
					StateStore->EditFunctionParameters(FunctionType).SecondaryText = Value;
				})
			];

	default:
		return BuildEmptyState(TEXT("No parameter panel available."));
	}
}

TSharedRef<SWidget> SAgentRootPanel::BuildContextChips() const
{
	const FUEAgentContextSummary& Context = StateStore->GetEditorContext();

	auto MakeChip = [](const FString& Label)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.12f, 0.12f, 0.12f))
			.Padding(FMargin(8.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
			];
	};

	TSharedRef<SWrapBox> WrapBox = SNew(SWrapBox).UseAllottedWidth(true);
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("Project: %s"), *Context.ProjectName))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("Module: %s"), *Context.ActiveModule))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("File: %s"), *FPaths::GetCleanFilename(Context.CurrentFile)))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("Assets: %d"), Context.SelectedAssets.Num()))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("Session: %s"), *StateStore->GetShortSessionId()))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("Output: %s"), *UEAgent::ToOutputLanguageLabel(StateStore->GetPreferredOutputLanguage())))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("Session State: %s"), *StateStore->GetSessionStatusText()))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("KB: %s"), *Context.KnowledgeBaseStatus))];
	WrapBox->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)[MakeChip(FString::Printf(TEXT("Backend: %s"), *StateStore->GetBackendServiceStatus()))];

	return WrapBox;
}

TSharedRef<SWidget> SAgentRootPanel::BuildChatBubble(const TSharedPtr<FUEAgentChatMessage>& Message) const
{
	const bool bUser = Message->Role == EUEAgentChatRole::User;
	const FLinearColor BubbleColor = bUser ? FLinearColor(0.11f, 0.28f, 0.48f) : (Message->Role == EUEAgentChatRole::Agent ? FLinearColor(0.19f, 0.19f, 0.19f) : FLinearColor(0.24f, 0.17f, 0.08f));

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(BubbleColor)
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Message->Title))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Message->Text))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Message->StatusHint.IsEmpty()
					? Message->Timestamp.ToString(TEXT("%H:%M:%S"))
					: FString::Printf(TEXT("%s  |  %s"), *Message->Timestamp.ToString(TEXT("%H:%M:%S")), *Message->StatusHint)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildDebugSectionBody()
{
	if (StateStore->GetActiveDebugSection() == EUEAgentDebugSection::Proposal)
	{
		return BuildProposalCards();
	}

	if (StateStore->GetActiveDebugSection() == EUEAgentDebugSection::Trace)
	{
		return BuildTracePanel();
	}

	if (StateStore->GetActiveDebugSection() == EUEAgentDebugSection::Artifacts)
	{
		return BuildArtifactsPanel();
	}

	const FString SectionText = StateStore->GetDebugSectionText(StateStore->GetActiveDebugSection());
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(6.0f)
		[
			SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(SectionText.IsEmpty() ? TEXT("{}") : SectionText))
			.IsReadOnly(true)
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildTracePanel()
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	TSharedRef<SVerticalBox> Container = SNew(SVerticalBox);

	if (Result.TraceLinks.Num() > 0)
	{
		TSharedRef<SWrapBox> LinkWrap = SNew(SWrapBox).UseAllottedWidth(true);
		for (const FUEAgentTraceLink& TraceLink : Result.TraceLinks)
		{
			LinkWrap->AddSlot()
			.Padding(0.0f, 0.0f, 6.0f, 6.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(TraceLink.Label.IsEmpty() ? TEXT("Open Trace") : TraceLink.Label))
				.ToolTipText(FText::FromString(TraceLink.Url))
				.OnClicked_Lambda([TraceLink]()
				{
					if (!TraceLink.Url.IsEmpty())
					{
						FPlatformProcess::LaunchURL(*TraceLink.Url, nullptr, nullptr);
					}
					return FReply::Handled();
				})
			];
		}

		Container->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				LinkWrap
			]
		];
	}

	if (Result.Events.Num() > 0)
	{
		TSharedRef<SVerticalBox> EventsBox = SNew(SVerticalBox);
		for (const FUEAgentRunEvent& Event : Result.Events)
		{
			const FString EventTitle = Event.Seq != INDEX_NONE
				? FString::Printf(TEXT("#%d %s"), Event.Seq, *Event.EventType)
				: Event.EventType;
			const FString EventSummary = Event.Summary.IsEmpty() ? Event.RawJson : Event.Summary;

			EventsBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FLinearColor(0.16f, 0.16f, 0.16f))
				.Padding(8.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(EventTitle))
						.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Event.Timestamp))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(EventSummary))
						.AutoWrapText(true)
					]
				]
			];
		}

		Container->AddSlot()
		.FillHeight(1.0f)
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					EventsBox
				]
			]
		];
	}

	Container->AddSlot()
	.FillHeight(0.55f)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(6.0f)
		[
			SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(Result.TraceJson.IsEmpty() ? TEXT("{}") : Result.TraceJson))
			.IsReadOnly(true)
		]
	];

	return Container;
}

TSharedRef<SWidget> SAgentRootPanel::BuildArtifactsPanel()
{
	const FUEAgentResultSnapshot& Result = StateStore->GetLastResult();
	if (Result.Artifacts.Num() == 0 && (Result.ApprovalResultJson.IsEmpty() || Result.ApprovalResultJson == TEXT("{}")))
	{
		const FString FallbackJson = Result.ArtifactsJson.IsEmpty() ? TEXT("{}") : Result.ArtifactsJson;
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(6.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text(FText::FromString(FallbackJson))
				.IsReadOnly(true)
			];
	}

	TSharedRef<SVerticalBox> Container = SNew(SVerticalBox);

	if (!Result.ApprovalResultJson.IsEmpty() && Result.ApprovalResultJson != TEXT("{}"))
	{
		Container->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Approval Result")))
					.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.Text(FText::FromString(Result.ApprovalResultJson))
					.IsReadOnly(true)
				]
			]
		];
	}

	for (const FUEAgentArtifactItem& Artifact : Result.Artifacts)
	{
		const FString ArtifactTitle = Artifact.Label.IsEmpty() ? TEXT("Artifact") : Artifact.Label;
		const FString ArtifactType = Artifact.ArtifactType.IsEmpty() ? TEXT("unknown") : Artifact.ArtifactType;
		const FString ArtifactPath = Artifact.Path.IsEmpty() ? TEXT("(no path returned)") : Artifact.Path;

		Container->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(ArtifactTitle))
					.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Type: %s"), *ArtifactType)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(ArtifactPath))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.Text(FText::FromString(Artifact.MetadataJson.IsEmpty() ? TEXT("{}") : Artifact.MetadataJson))
					.IsReadOnly(true)
				]
			]
		];
	}

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			Container
		];
}

TSharedRef<SWidget> SAgentRootPanel::BuildProposalCards()
{
	const TArray<TSharedPtr<FUEAgentProposalSummary>>* ProposalSource = StateStore->GetLastResult().Proposals.Num() > 0
		? &StateStore->GetLastResult().Proposals
		: &StateStore->GetPendingProposals();

	if (ProposalSource == nullptr || ProposalSource->Num() == 0)
	{
		return BuildEmptyState(TEXT("No pending proposals."));
	}

	TSharedRef<SScrollBox> ScrollBox = SNew(SScrollBox);
	for (const TSharedPtr<FUEAgentProposalSummary>& Proposal : *ProposalSource)
	{
		if (!Proposal.IsValid())
		{
			continue;
		}

		const bool bEditorOperation = Proposal->ProposalType.Equals(TEXT("editor_operation"), ESearchCase::IgnoreCase) || !Proposal->OperationType.IsEmpty();
		const FString OperationPayloadText = Proposal->OperationPayloadJson.IsEmpty()
			? FString()
			: FString::Printf(TEXT("Operation: %s\nPayload:\n%s"), *Proposal->OperationType, *Proposal->OperationPayloadJson);
		const FString ConfirmLabel = bEditorOperation ? TEXT("Confirm & Execute in UE") : TEXT("Confirm");
		const FString RiskSuffix = bEditorOperation ? TEXT("  |  Executor: UE Plugin") : FString();

		ScrollBox->AddSlot()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Proposal->Title))
					.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("Type: %s  |  State: %s  |  Risk: %s%s"), *Proposal->ProposalType, *Proposal->ConfirmationState, *Proposal->RiskFlags, *RiskSuffix)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("Before: %s"), *Proposal->BeforeSummary))).AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("After: %s"), *Proposal->AfterSummary))).AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("Rationale: %s"), *Proposal->Rationale))).AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Proposal->WritePlanSummary.IsEmpty() ? TEXT("") : FString::Printf(TEXT("Write Plan:\n%s"), *Proposal->WritePlanSummary)))
					.AutoWrapText(true)
					.Visibility(Proposal->WritePlanSummary.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(OperationPayloadText))
					.AutoWrapText(true)
					.Visibility(OperationPayloadText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton)
						.Text(FText::FromString(ConfirmLabel))
						.IsEnabled(Proposal->ConfirmationState.Equals(TEXT("pending"), ESearchCase::IgnoreCase))
						.OnClicked_Lambda([this, Proposal]()
						{
							SubmitProposalDecision(Proposal, TEXT("confirmed"));
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Reject")))
						.IsEnabled(Proposal->ConfirmationState.Equals(TEXT("pending"), ESearchCase::IgnoreCase))
						.OnClicked_Lambda([this, Proposal]()
						{
							SubmitProposalDecision(Proposal, TEXT("rejected"));
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Copy Proposal JSON")))
						.IsEnabled(!Proposal->RawJson.IsEmpty())
						.OnClicked_Lambda([this, Proposal]()
						{
							FPlatformApplicationMisc::ClipboardCopy(*Proposal->RawJson);
							StateStore->SetStatusMessage(TEXT("Proposal JSON copied to clipboard."));
							return FReply::Handled();
						})
					]
				]
			]
		];
	}

	return ScrollBox;
}

TSharedRef<SWidget> SAgentRootPanel::BuildEmptyState(const FString& Message) const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Message))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
}

TSharedRef<ITableRow> SAgentRootPanel::OnGenerateChatRow(TSharedPtr<FUEAgentChatMessage> Message, const TSharedRef<STableViewBase>& OwnerTable) const
{
	const bool bUser = Message.IsValid() && Message->Role == EUEAgentChatRole::User;

	return SNew(STableRow<TSharedPtr<FUEAgentChatMessage>>, OwnerTable)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				bUser ? SNew(SSpacer) : BuildChatBubble(Message)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 6.0f)
			[
				bUser ? BuildChatBubble(Message) : SNew(SSpacer)
			]
		];
}

TSharedRef<ITableRow> SAgentRootPanel::OnGenerateTaskRow(TSharedPtr<FUEAgentTaskSummary> TaskSummary, const TSharedRef<STableViewBase>& OwnerTable) const
{
	return SNew(STableRow<TSharedPtr<FUEAgentTaskSummary>>, OwnerTable)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TaskSummary.IsValid() ? TaskSummary->Title : TEXT("Task")))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TaskSummary.IsValid() ? FString::Printf(TEXT("%s  |  %s"), *TaskSummary->Status, *TaskSummary->TaskId) : TEXT("")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

void SAgentRootPanel::OnTaskSelectionChanged(TSharedPtr<FUEAgentTaskSummary> TaskSummary, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo != ESelectInfo::Direct)
	{
		LoadTaskDetail(TaskSummary);
	}
}

void SAgentRootPanel::OnFunctionSelectionChanged(TSharedPtr<EUEAgentFunctionType> Selection, ESelectInfo::Type SelectInfo)
{
	if (Selection.IsValid())
	{
		StateStore->SetActiveFunction(*Selection);
		if (*Selection == EUEAgentFunctionType::CodeReview && StateStore->GetCodeReviewFiles().Num() == 0)
		{
			RefreshCodeReviewFiles();
		}
	}
}

void SAgentRootPanel::OnDebugSectionSelectionChanged(TSharedPtr<EUEAgentDebugSection> Selection, ESelectInfo::Type SelectInfo)
{
	if (Selection.IsValid())
	{
		StateStore->SetActiveDebugSection(*Selection);
		switch (*Selection)
		{
		case EUEAgentDebugSection::UserProjection:
		case EUEAgentDebugSection::DebugProjection:
			RefreshProjectionDataForCurrentResult();
			break;
		case EUEAgentDebugSection::Trace:
		case EUEAgentDebugSection::Artifacts:
			RefreshTraceDataForCurrentResult();
			break;
		case EUEAgentDebugSection::Monitor:
			RefreshSystemSettings();
			RefreshMetrics();
			RefreshSystemAlerts();
			break;
		default:
			break;
		}
	}
}

void SAgentRootPanel::OnProfileSelectionChanged(TSharedPtr<FUEAgentRuntimeProfile> Selection, ESelectInfo::Type SelectInfo)
{
	if (Selection.IsValid())
	{
		ActivateSelectedProfile(Selection);
	}
}

TSharedPtr<EUEAgentFunctionType> SAgentRootPanel::FindFunctionOption(const EUEAgentFunctionType FunctionType) const
{
	for (const TSharedPtr<EUEAgentFunctionType>& Option : FunctionOptions)
	{
		if (Option.IsValid() && *Option == FunctionType)
		{
			return Option;
		}
	}
	return nullptr;
}

TSharedPtr<EUEAgentDebugSection> SAgentRootPanel::FindDebugSectionOption(const EUEAgentDebugSection Section) const
{
	for (const TSharedPtr<EUEAgentDebugSection>& Option : DebugSectionOptions)
	{
		if (Option.IsValid() && *Option == Section)
		{
			return Option;
		}
	}
	return nullptr;
}

TSharedPtr<FUEAgentRuntimeProfile> SAgentRootPanel::FindRuntimeProfileOption(const FString& ProfileId) const
{
	for (const TSharedPtr<FUEAgentRuntimeProfile>& Profile : RuntimeProfileOptions)
	{
		if (Profile.IsValid() && Profile->ProfileId == ProfileId)
		{
			return Profile;
		}
	}
	return RuntimeProfileOptions.Num() > 0 ? RuntimeProfileOptions[0] : nullptr;
}

FText SAgentRootPanel::GetFunctionComboLabel() const
{
	return UEAgent::ToFunctionLabel(StateStore->GetActiveFunction());
}

FText SAgentRootPanel::GetDebugSectionComboLabel() const
{
	return UEAgent::ToDebugSectionLabel(StateStore->GetActiveDebugSection());
}

FText SAgentRootPanel::GetProfileComboLabel() const
{
	const TSharedPtr<FUEAgentRuntimeProfile> Profile = FindRuntimeProfileOption(StateStore->GetActiveProfileId());
	return Profile.IsValid()
		? FText::FromString(FString::Printf(TEXT("Profile: %s"), *Profile->Label))
		: FText::FromString(TEXT("Profile: default"));
}

FText SAgentRootPanel::GetBackendStatusLabel() const
{
	return FText::FromString(FString::Printf(TEXT("Backend: %s"), *StateStore->GetBackendServiceStatus()));
}

FSlateColor SAgentRootPanel::GetBackendStatusColor() const
{
	return StateStore->IsBackendOnline()
		? FSlateColor(FLinearColor(0.15f, 0.40f, 0.17f))
		: FSlateColor(FLinearColor(0.48f, 0.18f, 0.12f));
}
