// Copyright Epic Games, Inc. All Rights Reserved.

#include "SAgentRootPanel.h"

#include "AgentEditorToolRegistry.h"
#include "AgentEditorToolCatalog.h"

#include "AgentStyle.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ActorComponent.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/SceneComponent.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Containers/Set.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IAssetTools.h"
#include "InputAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
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

	static bool TryReadLinearColorObject(const TSharedPtr<FJsonObject>& JsonObject, FLinearColor& OutColor)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		if (!TryGetNumberComponent(JsonObject, TEXT("r"), R) && !TryGetNumberComponent(JsonObject, TEXT("R"), R))
		{
			return false;
		}
		if (!TryGetNumberComponent(JsonObject, TEXT("g"), G) && !TryGetNumberComponent(JsonObject, TEXT("G"), G))
		{
			return false;
		}
		if (!TryGetNumberComponent(JsonObject, TEXT("b"), B) && !TryGetNumberComponent(JsonObject, TEXT("B"), B))
		{
			return false;
		}
		double OptionalA = 1.0;
		if (TryGetNumberComponent(JsonObject, TEXT("a"), OptionalA) || TryGetNumberComponent(JsonObject, TEXT("A"), OptionalA))
		{
			A = OptionalA;
		}

		OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
		return true;
	}

	static bool TryParseSlateVisibility(const FString& RawValue, ESlateVisibility& OutVisibility)
	{
		const FString Value = RawValue.TrimStartAndEnd().Replace(TEXT("-"), TEXT("_")).ToLower();
		if (Value == TEXT("visible") || Value == TEXT("show") || Value == TEXT("shown"))
		{
			OutVisibility = ESlateVisibility::Visible;
			return true;
		}
		if (Value == TEXT("collapsed") || Value == TEXT("collapse") || Value == TEXT("hide"))
		{
			OutVisibility = ESlateVisibility::Collapsed;
			return true;
		}
		if (Value == TEXT("hidden") || Value == TEXT("invisible"))
		{
			OutVisibility = ESlateVisibility::Hidden;
			return true;
		}
		if (Value == TEXT("hit_test_invisible") || Value == TEXT("hittestinvisible"))
		{
			OutVisibility = ESlateVisibility::HitTestInvisible;
			return true;
		}
		if (Value == TEXT("self_hit_test_invisible") || Value == TEXT("selfhittestinvisible"))
		{
			OutVisibility = ESlateVisibility::SelfHitTestInvisible;
			return true;
		}
		return false;
	}
	static bool TryReadVector2Object(const TSharedPtr<FJsonObject>& JsonObject, FVector2D& OutVector)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		if (!TryGetNumberComponent(JsonObject, TEXT("x"), X) && !TryGetNumberComponent(JsonObject, TEXT("X"), X))
		{
			return false;
		}
		if (!TryGetNumberComponent(JsonObject, TEXT("y"), Y) && !TryGetNumberComponent(JsonObject, TEXT("Y"), Y))
		{
			return false;
		}

		OutVector = FVector2D(X, Y);
		return true;
	}

	static bool TryReadVector2Field(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName, FVector2D& OutVector)
	{
		const TSharedPtr<FJsonObject> VectorObject = GetObjectField(JsonObject, FieldName);
		if (TryReadVector2Object(VectorObject, OutVector))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if (JsonObject.IsValid() && JsonObject->TryGetArrayField(FieldName, ArrayValues) && ArrayValues != nullptr && ArrayValues->Num() >= 2)
		{
			OutVector = FVector2D((*ArrayValues)[0]->AsNumber(), (*ArrayValues)[1]->AsNumber());
			return true;
		}

		return false;
	}

	static bool TryReadAnchorsObject(const TSharedPtr<FJsonObject>& JsonObject, FAnchors& OutAnchors)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		FVector2D Minimum;
		FVector2D Maximum;
		if (TryReadVector2Field(JsonObject, TEXT("minimum"), Minimum) && TryReadVector2Field(JsonObject, TEXT("maximum"), Maximum))
		{
			OutAnchors = FAnchors(Minimum.X, Minimum.Y, Maximum.X, Maximum.Y);
			return true;
		}

		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
		if ((TryGetNumberComponent(JsonObject, TEXT("min_x"), MinX) || TryGetNumberComponent(JsonObject, TEXT("MinX"), MinX))
			&& (TryGetNumberComponent(JsonObject, TEXT("min_y"), MinY) || TryGetNumberComponent(JsonObject, TEXT("MinY"), MinY))
			&& (TryGetNumberComponent(JsonObject, TEXT("max_x"), MaxX) || TryGetNumberComponent(JsonObject, TEXT("MaxX"), MaxX))
			&& (TryGetNumberComponent(JsonObject, TEXT("max_y"), MaxY) || TryGetNumberComponent(JsonObject, TEXT("MaxY"), MaxY)))
		{
			OutAnchors = FAnchors(MinX, MinY, MaxX, MaxY);
			return true;
		}

		return false;
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

	static UWidgetBlueprint* LoadWidgetBlueprintAsset(const FString& WidgetBlueprintPath)
	{
		return Cast<UWidgetBlueprint>(LoadEditorAsset(WidgetBlueprintPath));
	}

	static UClass* ResolveUmgWidgetClass(const FString& WidgetClassPath)
	{
		const FString Normalized = WidgetClassPath.TrimStartAndEnd().Replace(TEXT("_"), TEXT("")).ToLower();
		if (Normalized == TEXT("text") || Normalized == TEXT("textblock") || WidgetClassPath == TEXT("/Script/UMG.TextBlock"))
		{
			return UTextBlock::StaticClass();
		}
		if (Normalized == TEXT("button") || WidgetClassPath == TEXT("/Script/UMG.Button"))
		{
			return UButton::StaticClass();
		}
		if (Normalized == TEXT("image") || WidgetClassPath == TEXT("/Script/UMG.Image"))
		{
			return UImage::StaticClass();
		}
		if (Normalized == TEXT("border") || WidgetClassPath == TEXT("/Script/UMG.Border"))
		{
			return UBorder::StaticClass();
		}
		if (Normalized == TEXT("canvas") || Normalized == TEXT("canvaspanel") || WidgetClassPath == TEXT("/Script/UMG.CanvasPanel"))
		{
			return UCanvasPanel::StaticClass();
		}
		if (Normalized == TEXT("horizontalbox") || WidgetClassPath == TEXT("/Script/UMG.HorizontalBox"))
		{
			return UHorizontalBox::StaticClass();
		}
		if (Normalized == TEXT("verticalbox") || WidgetClassPath == TEXT("/Script/UMG.VerticalBox"))
		{
			return UVerticalBox::StaticClass();
		}
		return nullptr;
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

		UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (LoadedClass != nullptr)
		{
			return LoadedClass;
		}
		if (ClassPath.StartsWith(TEXT("/Game/")))
		{
			const FString ObjectPath = ToObjectPath(NormalizeAssetPackagePath(ClassPath));
			if (UBlueprint* BlueprintAsset = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ObjectPath)))
			{
				return BlueprintAsset->GeneratedClass;
			}
			return LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")));
		}
		return nullptr;
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

	static FEditorOperationExecutionResult ExecuteBatchRenameAssets(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("AssetTools.RenameAssets(batch)"));

		const TArray<TSharedPtr<FJsonValue>>* RenameValues = nullptr;
		if (!PayloadObject.IsValid() || !PayloadObject->TryGetArrayField(TEXT("renames"), RenameValues) || RenameValues == nullptr || RenameValues->Num() == 0)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("renames must be a non-empty array."));
			return ExecutionResult;
		}
		if (RenameValues->Num() > 20)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("too_many_renames"), TEXT("Batch rename is limited to 20 assets."));
			return ExecutionResult;
		}

		TArray<UObject*> Assets;
		TArray<FString> SourcePaths;
		TArray<FString> FolderPaths;
		TArray<FString> NewNames;
		TArray<FString> TargetPaths;
		TSet<FString> SeenTargets;

		for (int32 Index = 0; Index < RenameValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> RenameObject = (*RenameValues)[Index].IsValid() ? (*RenameValues)[Index]->AsObject() : nullptr;
			const FString AssetPath = NormalizeAssetPackagePath(GetScalarFieldAsString(RenameObject, TEXT("asset_path")));
			const FString NewName = GetScalarFieldAsString(RenameObject, TEXT("new_name")).TrimStartAndEnd();
			if (AssetPath.IsEmpty() || NewName.IsEmpty())
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_rename_item_payload"), FString::Printf(TEXT("renames[%d] requires asset_path and new_name."), Index));
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
			if (SeenTargets.Contains(TargetPath))
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("duplicate_target_path"), TargetPath);
				return ExecutionResult;
			}
			SeenTargets.Add(TargetPath);
			if (LoadEditorAsset(TargetPath) != nullptr)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("target_asset_exists"), FString::Printf(TEXT("Target asset already exists: %s"), *TargetPath));
				return ExecutionResult;
			}

			Assets.Add(Asset);
			SourcePaths.Add(AssetPath);
			FolderPaths.Add(FolderPath);
			NewNames.Add(NewName);
			TargetPaths.Add(TargetPath);
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Batch Rename Assets")));
		TArray<FAssetRenameData> RenameData;
		for (int32 Index = 0; Index < Assets.Num(); ++Index)
		{
			Assets[Index]->Modify();
			RenameData.Add(FAssetRenameData(Assets[Index], FolderPaths[Index], NewNames[Index]));
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		const bool bRenamed = AssetToolsModule.Get().RenameAssets(RenameData);
		if (!bRenamed)
		{
			ExecutionResult.ExecutionState = TEXT("failed");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("batch_rename_failed"), TEXT("AssetTools RenameAssets returned false."));
			return ExecutionResult;
		}

		TArray<TSharedPtr<FJsonValue>> RenamedValues;
		for (int32 Index = 0; Index < TargetPaths.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> RenameResult = MakeShared<FJsonObject>();
			RenameResult->SetStringField(TEXT("asset_path"), SourcePaths[Index]);
			RenameResult->SetStringField(TEXT("new_name"), NewNames[Index]);
			RenameResult->SetStringField(TEXT("target_path"), TargetPaths[Index]);
			RenamedValues.Add(MakeShared<FJsonValueObject>(RenameResult));
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), TargetPaths[Index]);
		}

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo or rename assets back. Redirectors are not fixed automatically.");
		ExecutionResult.ResultObject->SetNumberField(TEXT("item_count"), TargetPaths.Num());
		ExecutionResult.ResultObject->SetArrayField(TEXT("renamed_assets"), RenamedValues);
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		return ExecutionResult;
	}


	static FEditorOperationExecutionResult ExecuteMoveAssets(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("AssetTools.RenameAssets(move)"));

		FString TargetFolderPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("target_folder")));
		TargetFolderPath.RemoveFromEnd(TEXT("/"));
		if (!(TargetFolderPath == TEXT("/Game") || TargetFolderPath.StartsWith(TEXT("/Game/"))))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("target_folder_must_be_under_game"), TargetFolderPath);
			return ExecutionResult;
		}

		const TArray<TSharedPtr<FJsonValue>>* AssetPathValues = nullptr;
		if (!PayloadObject.IsValid() || !PayloadObject->TryGetArrayField(TEXT("asset_paths"), AssetPathValues) || AssetPathValues == nullptr || AssetPathValues->Num() == 0)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("asset_paths must be a non-empty array."));
			return ExecutionResult;
		}
		if (AssetPathValues->Num() > 20)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("too_many_assets"), TEXT("Move assets is limited to 20 assets."));
			return ExecutionResult;
		}

		TArray<UObject*> Assets;
		TArray<FString> SourcePaths;
		TArray<FString> AssetNames;
		TArray<FString> TargetPaths;
		TSet<FString> SeenTargets;

		for (int32 Index = 0; Index < AssetPathValues->Num(); ++Index)
		{
			const FString AssetPath = NormalizeAssetPackagePath((*AssetPathValues)[Index].IsValid() ? (*AssetPathValues)[Index]->AsString() : FString());
			if (AssetPath.IsEmpty())
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("empty_asset_path"), FString::Printf(TEXT("asset_paths[%d] is empty."), Index));
				return ExecutionResult;
			}
			const FString CurrentFolder = FPaths::GetPath(AssetPath);
			if (CurrentFolder == TargetFolderPath)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("target_folder_matches_current"), AssetPath);
				return ExecutionResult;
			}
			UObject* Asset = LoadEditorAsset(AssetPath);
			if (Asset == nullptr)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("asset_not_found"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
				return ExecutionResult;
			}

			const FString AssetName = FPaths::GetCleanFilename(AssetPath);
			const FString TargetPath = FString::Printf(TEXT("%s/%s"), *TargetFolderPath, *AssetName);
			if (SeenTargets.Contains(TargetPath))
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("duplicate_target_path"), TargetPath);
				return ExecutionResult;
			}
			SeenTargets.Add(TargetPath);
			if (LoadEditorAsset(TargetPath) != nullptr)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("target_asset_exists"), FString::Printf(TEXT("Target asset already exists: %s"), *TargetPath));
				return ExecutionResult;
			}

			Assets.Add(Asset);
			SourcePaths.Add(AssetPath);
			AssetNames.Add(AssetName);
			TargetPaths.Add(TargetPath);
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Move Assets")));
		TArray<FAssetRenameData> RenameData;
		for (int32 Index = 0; Index < Assets.Num(); ++Index)
		{
			Assets[Index]->Modify();
			RenameData.Add(FAssetRenameData(Assets[Index], TargetFolderPath, AssetNames[Index]));
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		const bool bMoved = AssetToolsModule.Get().RenameAssets(RenameData);
		if (!bMoved)
		{
			ExecutionResult.ExecutionState = TEXT("failed");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("move_assets_failed"), TEXT("AssetTools RenameAssets returned false."));
			return ExecutionResult;
		}

		TArray<TSharedPtr<FJsonValue>> MovedValues;
		for (int32 Index = 0; Index < TargetPaths.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> MoveResult = MakeShared<FJsonObject>();
			MoveResult->SetStringField(TEXT("asset_path"), SourcePaths[Index]);
			MoveResult->SetStringField(TEXT("target_path"), TargetPaths[Index]);
			MovedValues.Add(MakeShared<FJsonValueObject>(MoveResult));
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), TargetPaths[Index]);
		}

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo or move assets back. Redirectors are not fixed automatically.");
		ExecutionResult.ResultObject->SetStringField(TEXT("target_folder"), TargetFolderPath);
		ExecutionResult.ResultObject->SetNumberField(TEXT("item_count"), TargetPaths.Num());
		ExecutionResult.ResultObject->SetArrayField(TEXT("moved_assets"), MovedValues);
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

	static FEditorOperationExecutionResult ExecuteAddBlueprintNodeTemplate(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UK2Node_CallFunction"));

		const FString BlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("blueprint_path")));
		const FString TemplateId = GetScalarFieldAsString(PayloadObject, TEXT("template_id")).TrimStartAndEnd().ToLower();
		FString GraphName = GetScalarFieldAsString(PayloadObject, TEXT("graph_name")).TrimStartAndEnd();
		const FString Message = GetScalarFieldAsString(PayloadObject, TEXT("message"));
		const FString NodeComment = GetScalarFieldAsString(PayloadObject, TEXT("node_comment"));
		const FString EntryEventName = GetScalarFieldAsString(PayloadObject, TEXT("entry_event")).TrimStartAndEnd();
		const FString VariableName = GetScalarFieldAsString(PayloadObject, TEXT("variable_name")).TrimStartAndEnd();
		const FString VariableValue = GetScalarFieldAsString(PayloadObject, TEXT("variable_value"));
		const FString FunctionName = GetScalarFieldAsString(PayloadObject, TEXT("function_name")).TrimStartAndEnd();
		const FString InputActionPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("input_action_path")));
		FString BranchPath = GetScalarFieldAsString(PayloadObject, TEXT("branch_path")).TrimStartAndEnd().ToLower();
		const bool bConditionDefault = GetBoolFieldOrDefault(PayloadObject, TEXT("condition_default"), true);
		const bool bPrintToScreen = GetBoolFieldOrDefault(PayloadObject, TEXT("print_to_screen"), true);
		const bool bPrintToLog = GetBoolFieldOrDefault(PayloadObject, TEXT("print_to_log"), true);
		const bool bCompileAfterEdit = GetBoolFieldOrDefault(PayloadObject, TEXT("compile_after_edit"), true);
		const bool bIsPrintStringTemplate = TemplateId.Equals(TEXT("print_string"), ESearchCase::IgnoreCase);
		const bool bIsBranchPrintStringTemplate = TemplateId.Equals(TEXT("branch_print_string"), ESearchCase::IgnoreCase);
		const bool bIsSequencePrintStringsTemplate = TemplateId.Equals(TEXT("sequence_print_strings"), ESearchCase::IgnoreCase);
		const bool bIsGetVariableTemplate = TemplateId.Equals(TEXT("get_variable"), ESearchCase::IgnoreCase);
		const bool bIsSetVariableTemplate = TemplateId.Equals(TEXT("set_variable"), ESearchCase::IgnoreCase);
		const bool bIsCallFunctionTemplate = TemplateId.Equals(TEXT("call_function"), ESearchCase::IgnoreCase);
		const bool bIsEnhancedInputActionEventTemplate = TemplateId.Equals(TEXT("enhanced_input_action_event"), ESearchCase::IgnoreCase);
		const bool bUsesPrintString = bIsPrintStringTemplate || bIsBranchPrintStringTemplate || bIsSequencePrintStringsTemplate;
		TArray<FString> SequenceMessages;
		const TArray<TSharedPtr<FJsonValue>>* MessageValues = nullptr;
		if (PayloadObject.IsValid() && PayloadObject->TryGetArrayField(TEXT("messages"), MessageValues) && MessageValues != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& MessageValue : *MessageValues)
			{
				if (MessageValue.IsValid() && SequenceMessages.Num() < 2)
				{
					const FString SequenceMessage = MessageValue->AsString();
					if (!SequenceMessage.IsEmpty())
					{
						SequenceMessages.Add(SequenceMessage);
					}
				}
			}
		}
		while (SequenceMessages.Num() < 2)
		{
			SequenceMessages.Add(SequenceMessages.Num() == 0 && !Message.IsEmpty()
				? Message
				: FString::Printf(TEXT("Sequence step %d from UEAgent"), SequenceMessages.Num() + 1));
		}
		double DurationSeconds = 2.0;
		PayloadObject->TryGetNumberField(TEXT("duration"), DurationSeconds);
		if (GraphName.IsEmpty())
		{
			GraphName = TEXT("EventGraph");
		}
		if (BlueprintPath.IsEmpty() || TemplateId.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("blueprint_path and template_id are required."));
			return ExecutionResult;
		}
		if (!bIsPrintStringTemplate && !bIsBranchPrintStringTemplate && !bIsSequencePrintStringsTemplate && !bIsGetVariableTemplate && !bIsSetVariableTemplate && !bIsCallFunctionTemplate && !bIsEnhancedInputActionEventTemplate)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_node_template_unsupported"), TEXT("Only print_string, branch_print_string, sequence_print_strings, get_variable, set_variable, call_function, and enhanced_input_action_event are supported in v1."));
			return ExecutionResult;
		}
		if ((bIsGetVariableTemplate || bIsSetVariableTemplate) && VariableName.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("variable_name_required"), TEXT("variable_name is required for Blueprint variable node templates."));
			return ExecutionResult;
		}
		if (bIsCallFunctionTemplate && FunctionName.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("function_name_required"), TEXT("function_name is required for Blueprint function call templates."));
			return ExecutionResult;
		}
		if (bIsEnhancedInputActionEventTemplate && InputActionPath.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("input_action_path_required"), TEXT("input_action_path is required for Enhanced Input Action event templates."));
			return ExecutionResult;
		}
		if (BranchPath.IsEmpty())
		{
			BranchPath = TEXT("true");
		}
		if (bIsBranchPrintStringTemplate && !BranchPath.Equals(TEXT("true"), ESearchCase::IgnoreCase) && !BranchPath.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("branch_path_unsupported"), TEXT("branch_path must be true or false."));
			return ExecutionResult;
		}

		UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath);
		if (Blueprint == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return ExecutionResult;
		}
		UInputAction* InputAction = nullptr;
		if (bIsEnhancedInputActionEventTemplate)
		{
			InputAction = Cast<UInputAction>(LoadEditorAsset(InputActionPath));
			if (InputAction == nullptr)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("input_action_not_found"), FString::Printf(TEXT("Input Action not found: %s"), *InputActionPath));
				return ExecutionResult;
			}
		}
		UFunction* FunctionToCall = nullptr;
		if (bIsCallFunctionTemplate)
		{
			const FName FunctionFName(*FunctionName);
			UClass* SearchClass = Blueprint->SkeletonGeneratedClass != nullptr ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
			FunctionToCall = SearchClass != nullptr ? SearchClass->FindFunctionByName(FunctionFName) : nullptr;
			if (FunctionToCall == nullptr && Blueprint->GeneratedClass != nullptr)
			{
				FunctionToCall = Blueprint->GeneratedClass->FindFunctionByName(FunctionFName);
			}
			if (FunctionToCall == nullptr && Blueprint->ParentClass != nullptr)
			{
				FunctionToCall = Blueprint->ParentClass->FindFunctionByName(FunctionFName);
			}
			if (FunctionToCall == nullptr)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_function_not_found"), FString::Printf(TEXT("Existing self function not found: %s"), *FunctionName));
				return ExecutionResult;
			}
			if (FunctionToCall->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("pure_function_not_supported_in_v1"), TEXT("call_function currently supports exec-callable functions only; Blueprint Pure functions have no exec pins."));
				return ExecutionResult;
			}
			bool bHasInputParams = false;
			for (TFieldIterator<FProperty> PropertyIt(FunctionToCall); PropertyIt; ++PropertyIt)
			{
				const FProperty* Property = *PropertyIt;
				if (Property != nullptr
					&& Property->HasAnyPropertyFlags(CPF_Parm)
					&& !Property->HasAnyPropertyFlags(CPF_ReturnParm)
					&& !Property->HasAnyPropertyFlags(CPF_OutParm))
				{
					bHasInputParams = true;
					break;
				}
			}
			if (bHasInputParams)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("function_inputs_not_supported_in_v1"), TEXT("call_function currently supports existing self functions without input parameters only."));
				return ExecutionResult;
			}
		}
		if (bIsGetVariableTemplate || bIsSetVariableTemplate)
		{
			const FName VariableFName(*VariableName);
			bool bVariableExists = false;
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				if (Variable.VarName == VariableFName)
				{
					bVariableExists = true;
					break;
				}
			}
			UClass* SearchClass = Blueprint->SkeletonGeneratedClass != nullptr ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
			if (!bVariableExists && SearchClass != nullptr && FindFProperty<FProperty>(SearchClass, VariableFName) != nullptr)
			{
				bVariableExists = true;
			}
			if (!bVariableExists)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_variable_not_found"), FString::Printf(TEXT("Existing member variable not found: %s"), *VariableName));
				return ExecutionResult;
			}
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Add Blueprint Node Template")));
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
		if (TargetGraph == nullptr && GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
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
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_graph_unavailable"), GraphName);
			return ExecutionResult;
		}

		FVector2D NodePosition = TargetGraph->GetGoodPlaceForNewNode();
		TSharedPtr<FJsonObject> NodePositionObject = GetObjectField(PayloadObject, TEXT("node_position"));
		double PositionX = 0.0;
		double PositionY = 0.0;
		if (NodePositionObject.IsValid()
			&& (TryGetNumberComponent(NodePositionObject, TEXT("x"), PositionX) || TryGetNumberComponent(NodePositionObject, TEXT("X"), PositionX))
			&& (TryGetNumberComponent(NodePositionObject, TEXT("y"), PositionY) || TryGetNumberComponent(NodePositionObject, TEXT("Y"), PositionY)))
		{
			NodePosition = FVector2D(PositionX, PositionY);
		}

		UK2Node_Event* EntryEventNode = nullptr;
		bool bCreatedEntryEvent = false;
		if (!EntryEventName.IsEmpty())
		{
			FName EntryFunctionName;
			UClass* EntryOwnerClass = nullptr;
			if (!ResolveBlueprintEventFunction(EntryEventName, EntryFunctionName, EntryOwnerClass)
				|| EntryOwnerClass == nullptr
				|| EntryOwnerClass->FindFunctionByName(EntryFunctionName) == nullptr)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("entry_event_unsupported"), TEXT("Only BeginPlay is supported for Blueprint node template links in v1."));
				return ExecutionResult;
			}

			EntryEventNode = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, EntryOwnerClass, EntryFunctionName);
			if (EntryEventNode != nullptr && EntryEventNode->GetGraph() != nullptr)
			{
				TargetGraph = EntryEventNode->GetGraph();
			}
			if (EntryEventNode == nullptr)
			{
				TargetGraph->Modify();
				EntryEventNode = NewObject<UK2Node_Event>(TargetGraph);
				EntryEventNode->SetFlags(RF_Transactional);
				EntryEventNode->EventReference.SetExternalMember(EntryFunctionName, EntryOwnerClass);
				EntryEventNode->bOverrideFunction = true;
				EntryEventNode->CreateNewGuid();
				EntryEventNode->PostPlacedNewNode();
				EntryEventNode->AllocateDefaultPins();
				EntryEventNode->NodePosX = static_cast<int32>(NodePosition.X - 320.0);
				EntryEventNode->NodePosY = static_cast<int32>(NodePosition.Y);
				TargetGraph->AddNode(EntryEventNode, true, true);
				bCreatedEntryEvent = true;
			}
		}

		TargetGraph->Modify();
		UK2Node_IfThenElse* BranchNode = nullptr;
		if (bIsBranchPrintStringTemplate)
		{
			BranchNode = NewObject<UK2Node_IfThenElse>(TargetGraph);
			BranchNode->SetFlags(RF_Transactional);
			BranchNode->CreateNewGuid();
			BranchNode->PostPlacedNewNode();
			BranchNode->AllocateDefaultPins();
			BranchNode->NodePosX = static_cast<int32>(NodePosition.X);
			BranchNode->NodePosY = static_cast<int32>(NodePosition.Y);
			if (!NodeComment.IsEmpty())
			{
				BranchNode->NodeComment = NodeComment;
				BranchNode->bCommentBubblePinned = true;
				BranchNode->bCommentBubbleVisible = true;
			}
			if (UEdGraphPin* ConditionPin = BranchNode->GetConditionPin())
			{
				ConditionPin->DefaultValue = bConditionDefault ? TEXT("true") : TEXT("false");
			}
			TargetGraph->AddNode(BranchNode, true, true);
		}
		UK2Node_ExecutionSequence* SequenceNode = nullptr;
		if (bIsSequencePrintStringsTemplate)
		{
			SequenceNode = NewObject<UK2Node_ExecutionSequence>(TargetGraph);
			SequenceNode->SetFlags(RF_Transactional);
			SequenceNode->CreateNewGuid();
			SequenceNode->PostPlacedNewNode();
			SequenceNode->AllocateDefaultPins();
			SequenceNode->NodePosX = static_cast<int32>(NodePosition.X);
			SequenceNode->NodePosY = static_cast<int32>(NodePosition.Y);
			if (!NodeComment.IsEmpty())
			{
				SequenceNode->NodeComment = NodeComment;
				SequenceNode->bCommentBubblePinned = true;
				SequenceNode->bCommentBubbleVisible = true;
			}
			TargetGraph->AddNode(SequenceNode, true, true);
		}

		TArray<UK2Node_CallFunction*> PrintNodes;
		UK2Node_CallFunction* CallNode = nullptr;
		UK2Node_CallFunction* FunctionCallNode = nullptr;
		UK2Node_EnhancedInputAction* EnhancedInputActionNode = nullptr;
		UK2Node_VariableGet* VariableGetNode = nullptr;
		UK2Node_VariableSet* VariableSetNode = nullptr;
		if (bIsGetVariableTemplate)
		{
			VariableGetNode = NewObject<UK2Node_VariableGet>(TargetGraph);
			VariableGetNode->SetFlags(RF_Transactional);
			VariableGetNode->CreateNewGuid();
			VariableGetNode->VariableReference.SetSelfMember(FName(*VariableName));
			VariableGetNode->NodePosX = static_cast<int32>(NodePosition.X);
			VariableGetNode->NodePosY = static_cast<int32>(NodePosition.Y);
			if (!NodeComment.IsEmpty())
			{
				VariableGetNode->NodeComment = NodeComment;
				VariableGetNode->bCommentBubblePinned = true;
				VariableGetNode->bCommentBubbleVisible = true;
			}
			TargetGraph->AddNode(VariableGetNode, true, true);
			VariableGetNode->PostPlacedNewNode();
			VariableGetNode->AllocateDefaultPins();
		}
		if (bIsSetVariableTemplate)
		{
			VariableSetNode = NewObject<UK2Node_VariableSet>(TargetGraph);
			VariableSetNode->SetFlags(RF_Transactional);
			VariableSetNode->CreateNewGuid();
			VariableSetNode->VariableReference.SetSelfMember(FName(*VariableName));
			VariableSetNode->NodePosX = static_cast<int32>(NodePosition.X);
			VariableSetNode->NodePosY = static_cast<int32>(NodePosition.Y);
			if (!NodeComment.IsEmpty())
			{
				VariableSetNode->NodeComment = NodeComment;
				VariableSetNode->bCommentBubblePinned = true;
				VariableSetNode->bCommentBubbleVisible = true;
			}
			TargetGraph->AddNode(VariableSetNode, true, true);
			VariableSetNode->PostPlacedNewNode();
			VariableSetNode->AllocateDefaultPins();
			if (!VariableValue.IsEmpty())
			{
				UEdGraphPin* VariableValuePin = VariableSetNode->FindPin(FName(*VariableName));
				if (VariableValuePin == nullptr)
				{
					for (UEdGraphPin* Pin : VariableSetNode->Pins)
					{
						if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						{
							VariableValuePin = Pin;
							break;
						}
					}
				}
				if (VariableValuePin != nullptr)
				{
					VariableValuePin->DefaultValue = VariableValue;
				}
			}
		}
		if (bIsCallFunctionTemplate)
		{
			FunctionCallNode = NewObject<UK2Node_CallFunction>(TargetGraph);
			FunctionCallNode->SetFlags(RF_Transactional);
			FunctionCallNode->CreateNewGuid();
			FunctionCallNode->SetFromFunction(FunctionToCall);
			FunctionCallNode->NodePosX = static_cast<int32>(NodePosition.X);
			FunctionCallNode->NodePosY = static_cast<int32>(NodePosition.Y);
			if (!NodeComment.IsEmpty())
			{
				FunctionCallNode->NodeComment = NodeComment;
				FunctionCallNode->bCommentBubblePinned = true;
				FunctionCallNode->bCommentBubbleVisible = true;
			}
			TargetGraph->AddNode(FunctionCallNode, true, true);
			FunctionCallNode->PostPlacedNewNode();
			FunctionCallNode->AllocateDefaultPins();
		}
		if (bIsEnhancedInputActionEventTemplate)
		{
			EnhancedInputActionNode = NewObject<UK2Node_EnhancedInputAction>(TargetGraph);
			EnhancedInputActionNode->SetFlags(RF_Transactional);
			EnhancedInputActionNode->CreateNewGuid();
			EnhancedInputActionNode->InputAction = InputAction;
			EnhancedInputActionNode->NodePosX = static_cast<int32>(NodePosition.X);
			EnhancedInputActionNode->NodePosY = static_cast<int32>(NodePosition.Y);
			if (!NodeComment.IsEmpty())
			{
				EnhancedInputActionNode->NodeComment = NodeComment;
				EnhancedInputActionNode->bCommentBubblePinned = true;
				EnhancedInputActionNode->bCommentBubbleVisible = true;
			}
			if (!EnhancedInputActionNode->IsCompatibleWithGraph(TargetGraph))
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("enhanced_input_node_incompatible_graph"), TEXT("Enhanced Input Action nodes must be placed in an input-event-capable Ubergraph, not Construction Script or a function graph."));
				return ExecutionResult;
			}
			TargetGraph->AddNode(EnhancedInputActionNode, true, true);
			EnhancedInputActionNode->PostPlacedNewNode();
			EnhancedInputActionNode->AllocateDefaultPins();
		}
		if (bUsesPrintString)
		{
			CallNode = NewObject<UK2Node_CallFunction>(TargetGraph);
			CallNode->SetFlags(RF_Transactional);
			CallNode->FunctionReference.SetExternalMember(
				GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
				UKismetSystemLibrary::StaticClass());
			CallNode->CreateNewGuid();
			CallNode->PostPlacedNewNode();
			CallNode->AllocateDefaultPins();

			CallNode->NodePosX = static_cast<int32>((bIsBranchPrintStringTemplate || bIsSequencePrintStringsTemplate) ? NodePosition.X + 360.0 : NodePosition.X);
			CallNode->NodePosY = static_cast<int32>(NodePosition.Y);
			if (!NodeComment.IsEmpty())
			{
				CallNode->NodeComment = NodeComment;
				CallNode->bCommentBubblePinned = true;
				CallNode->bCommentBubbleVisible = true;
			}

			if (UEdGraphPin* InStringPin = CallNode->FindPin(TEXT("InString")))
			{
				InStringPin->DefaultValue = bIsSequencePrintStringsTemplate ? SequenceMessages[0] : (Message.IsEmpty() ? TEXT("Hello from UEAgent") : Message);
			}
			if (UEdGraphPin* PrintToScreenPin = CallNode->FindPin(TEXT("bPrintToScreen")))
			{
				PrintToScreenPin->DefaultValue = bPrintToScreen ? TEXT("true") : TEXT("false");
			}
			if (UEdGraphPin* PrintToLogPin = CallNode->FindPin(TEXT("bPrintToLog")))
			{
				PrintToLogPin->DefaultValue = bPrintToLog ? TEXT("true") : TEXT("false");
			}
			if (UEdGraphPin* DurationPin = CallNode->FindPin(TEXT("Duration")))
			{
				DurationPin->DefaultValue = FString::SanitizeFloat(DurationSeconds);
			}

			TargetGraph->AddNode(CallNode, true, true);
			PrintNodes.Add(CallNode);
		}
		if (bIsSequencePrintStringsTemplate)
		{
			for (int32 MessageIndex = 1; MessageIndex < SequenceMessages.Num(); ++MessageIndex)
			{
				UK2Node_CallFunction* ExtraCallNode = NewObject<UK2Node_CallFunction>(TargetGraph);
				ExtraCallNode->SetFlags(RF_Transactional);
				ExtraCallNode->FunctionReference.SetExternalMember(
					GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
					UKismetSystemLibrary::StaticClass());
				ExtraCallNode->CreateNewGuid();
				ExtraCallNode->PostPlacedNewNode();
				ExtraCallNode->AllocateDefaultPins();
				ExtraCallNode->NodePosX = static_cast<int32>(NodePosition.X + 360.0);
				ExtraCallNode->NodePosY = static_cast<int32>(NodePosition.Y + MessageIndex * 180.0);
				if (UEdGraphPin* InStringPin = ExtraCallNode->FindPin(TEXT("InString")))
				{
					InStringPin->DefaultValue = SequenceMessages[MessageIndex];
				}
				if (UEdGraphPin* PrintToScreenPin = ExtraCallNode->FindPin(TEXT("bPrintToScreen")))
				{
					PrintToScreenPin->DefaultValue = bPrintToScreen ? TEXT("true") : TEXT("false");
				}
				if (UEdGraphPin* PrintToLogPin = ExtraCallNode->FindPin(TEXT("bPrintToLog")))
				{
					PrintToLogPin->DefaultValue = bPrintToLog ? TEXT("true") : TEXT("false");
				}
				if (UEdGraphPin* DurationPin = ExtraCallNode->FindPin(TEXT("Duration")))
				{
					DurationPin->DefaultValue = FString::SanitizeFloat(DurationSeconds);
				}
				TargetGraph->AddNode(ExtraCallNode, true, true);
				PrintNodes.Add(ExtraCallNode);
			}
		}
		TArray<FString> LinkedPinsSummaries;
		auto FindExecPin = [](UEdGraphNode* Node, const EEdGraphPinDirection Direction, const FName PreferredName) -> UEdGraphPin*
		{
			if (Node == nullptr)
			{
				return nullptr;
			}
			if (!PreferredName.IsNone())
			{
				if (UEdGraphPin* PreferredPin = Node->FindPin(PreferredName))
				{
					return PreferredPin;
				}
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr && Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					return Pin;
				}
			}
			return nullptr;
		};
		auto ConnectExecPins = [&TargetGraph, &LinkedPinsSummaries](UEdGraphNode* SourceNode, UEdGraphPin* SourcePin, UEdGraphNode* TargetNode, UEdGraphPin* TargetPin) -> bool
		{
			if (SourceNode == nullptr || SourcePin == nullptr || TargetNode == nullptr || TargetPin == nullptr)
			{
				return false;
			}
			bool bLinked = SourcePin->LinkedTo.Contains(TargetPin);
			if (!bLinked && (SourcePin->LinkedTo.Num() > 0 || TargetPin->LinkedTo.Num() > 0))
			{
				return false;
			}
			if (!bLinked)
			{
				if (const UEdGraphSchema* Schema = TargetGraph != nullptr ? TargetGraph->GetSchema() : nullptr)
				{
					bLinked = Schema->TryCreateConnection(SourcePin, TargetPin);
				}
			}
			if (bLinked)
			{
				LinkedPinsSummaries.Add(FString::Printf(TEXT("%s.%s -> %s.%s"),
					*SourceNode->GetName(),
					*SourcePin->PinName.ToString(),
					*TargetNode->GetName(),
					*TargetPin->PinName.ToString()));
			}
			return bLinked;
		};

		bool bTemplateLinkSuccess = EntryEventName.IsEmpty();
		if (bIsBranchPrintStringTemplate)
		{
			UEdGraphPin* BranchInputPin = FindExecPin(BranchNode, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
			UEdGraphPin* BranchOutputPin = BranchPath.Equals(TEXT("false"), ESearchCase::IgnoreCase)
				? (BranchNode != nullptr ? BranchNode->GetElsePin() : nullptr)
				: (BranchNode != nullptr ? BranchNode->GetThenPin() : nullptr);
			UEdGraphPin* PrintInputPin = FindExecPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
			const bool bBranchToPrintLinked = ConnectExecPins(BranchNode, BranchOutputPin, CallNode, PrintInputPin);
			bool bEntryToBranchLinked = EntryEventName.IsEmpty();
			if (EntryEventNode != nullptr)
			{
				UEdGraphPin* EntryOutputPin = FindExecPin(EntryEventNode, EGPD_Output, UEdGraphSchema_K2::PN_Then);
				bEntryToBranchLinked = ConnectExecPins(EntryEventNode, EntryOutputPin, BranchNode, BranchInputPin);
			}
			bTemplateLinkSuccess = bEntryToBranchLinked && bBranchToPrintLinked;
			if (!bTemplateLinkSuccess)
			{
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_template_link_failed"), TEXT("Could not connect BeginPlay -> Branch -> PrintString exec chain."));
				AddFailedField(ExecutionResult.ResultObject, TEXT("linked_pins"), TEXT("BeginPlay -> Branch -> PrintString failed."));
			}
		}
		else if (bIsSequencePrintStringsTemplate)
		{
			UEdGraphPin* SequenceInputPin = FindExecPin(SequenceNode, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
			bool bEntryToSequenceLinked = EntryEventName.IsEmpty();
			if (EntryEventNode != nullptr)
			{
				UEdGraphPin* EntryOutputPin = FindExecPin(EntryEventNode, EGPD_Output, UEdGraphSchema_K2::PN_Then);
				bEntryToSequenceLinked = ConnectExecPins(EntryEventNode, EntryOutputPin, SequenceNode, SequenceInputPin);
			}
			bool bSequenceOutputsLinked = SequenceNode != nullptr;
			for (int32 NodeIndex = 0; NodeIndex < PrintNodes.Num(); ++NodeIndex)
			{
				UEdGraphPin* SequenceOutputPin = SequenceNode != nullptr ? SequenceNode->GetThenPinGivenIndex(NodeIndex) : nullptr;
				UEdGraphPin* PrintInputPin = FindExecPin(PrintNodes[NodeIndex], EGPD_Input, UEdGraphSchema_K2::PN_Execute);
				bSequenceOutputsLinked = ConnectExecPins(SequenceNode, SequenceOutputPin, PrintNodes[NodeIndex], PrintInputPin) && bSequenceOutputsLinked;
			}
			bTemplateLinkSuccess = bEntryToSequenceLinked && bSequenceOutputsLinked;
			if (!bTemplateLinkSuccess)
			{
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_template_link_failed"), TEXT("Could not connect BeginPlay -> Sequence -> PrintString exec chain."));
				AddFailedField(ExecutionResult.ResultObject, TEXT("linked_pins"), TEXT("BeginPlay -> Sequence -> PrintString failed."));
			}
		}
		else if (bIsPrintStringTemplate && EntryEventNode != nullptr)
		{
			UEdGraphPin* SourceExecPin = FindExecPin(EntryEventNode, EGPD_Output, UEdGraphSchema_K2::PN_Then);
			UEdGraphPin* TargetExecPin = FindExecPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
			bTemplateLinkSuccess = ConnectExecPins(EntryEventNode, SourceExecPin, CallNode, TargetExecPin);
			if (!bTemplateLinkSuccess)
			{
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_template_link_failed"), TEXT("Could not connect BeginPlay exec output to PrintString exec input."));
				AddFailedField(ExecutionResult.ResultObject, TEXT("linked_pins"), TEXT("BeginPlay.Then -> PrintString.Execute failed."));
			}
		}
		else if (bIsSetVariableTemplate && EntryEventNode != nullptr)
		{
			UEdGraphPin* SourceExecPin = FindExecPin(EntryEventNode, EGPD_Output, UEdGraphSchema_K2::PN_Then);
			UEdGraphPin* TargetExecPin = FindExecPin(VariableSetNode, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
			bTemplateLinkSuccess = ConnectExecPins(EntryEventNode, SourceExecPin, VariableSetNode, TargetExecPin);
			if (!bTemplateLinkSuccess)
			{
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_template_link_failed"), TEXT("Could not connect BeginPlay exec output to Set Variable exec input."));
				AddFailedField(ExecutionResult.ResultObject, TEXT("linked_pins"), TEXT("BeginPlay.Then -> SetVariable.Execute failed."));
			}
		}
		else if (bIsCallFunctionTemplate && EntryEventNode != nullptr)
		{
			UEdGraphPin* SourceExecPin = FindExecPin(EntryEventNode, EGPD_Output, UEdGraphSchema_K2::PN_Then);
			UEdGraphPin* TargetExecPin = FindExecPin(FunctionCallNode, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
			bTemplateLinkSuccess = ConnectExecPins(EntryEventNode, SourceExecPin, FunctionCallNode, TargetExecPin);
			if (!bTemplateLinkSuccess)
			{
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_template_link_failed"), TEXT("Could not connect BeginPlay exec output to CallFunction exec input."));
				AddFailedField(ExecutionResult.ResultObject, TEXT("linked_pins"), TEXT("BeginPlay.Then -> CallFunction.Execute failed."));
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		FString CompileStatus = BlueprintStatusToOperationString(Blueprint->Status);
		bool bCompileSuccess = true;
		if (bCompileAfterEdit)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			CompileStatus = BlueprintStatusToOperationString(Blueprint->Status);
			bCompileSuccess = Blueprint->Status != BS_Error;
		}

		ExecutionResult.bSuccess = bCompileSuccess && bTemplateLinkSuccess;
		ExecutionResult.ExecutionState = ExecutionResult.bSuccess ? TEXT("completed") : TEXT("failed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to remove the created Blueprint node. The package is marked dirty but not auto-saved.");
		FString PrimaryCreatedNodeName;
		if (CallNode != nullptr)
		{
			PrimaryCreatedNodeName = CallNode->GetName();
		}
		else if (VariableSetNode != nullptr)
		{
			PrimaryCreatedNodeName = VariableSetNode->GetName();
		}
		else if (VariableGetNode != nullptr)
		{
			PrimaryCreatedNodeName = VariableGetNode->GetName();
		}
		else if (FunctionCallNode != nullptr)
		{
			PrimaryCreatedNodeName = FunctionCallNode->GetName();
		}
		else if (EnhancedInputActionNode != nullptr)
		{
			PrimaryCreatedNodeName = EnhancedInputActionNode->GetName();
		}
		ExecutionResult.ResultObject->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("template_id"), TemplateId);
		ExecutionResult.ResultObject->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
		ExecutionResult.ResultObject->SetStringField(TEXT("created_node_name"), PrimaryCreatedNodeName);
		ExecutionResult.ResultObject->SetStringField(TEXT("compile_status"), CompileStatus);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("compiled_after_edit"), bCompileAfterEdit);
		ExecutionResult.ResultObject->SetBoolField(TEXT("linked_entry_event"), bTemplateLinkSuccess && !EntryEventName.IsEmpty());
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), Blueprint->GetOutermost() != nullptr && Blueprint->GetOutermost()->IsDirty());
		if (BranchNode != nullptr)
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("branch_path"), BranchPath);
			ExecutionResult.ResultObject->SetBoolField(TEXT("condition_default"), bConditionDefault);
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), BranchNode->GetName());
		}
		if (SequenceNode != nullptr)
		{
			ExecutionResult.ResultObject->SetNumberField(TEXT("sequence_output_count"), PrintNodes.Num());
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), SequenceNode->GetName());
			for (const FString& SequenceMessage : SequenceMessages)
			{
				AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("messages"), SequenceMessage);
			}
		}
		if (VariableGetNode != nullptr)
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("variable_name"), VariableName);
			ExecutionResult.ResultObject->SetStringField(TEXT("variable_scope"), TEXT("self"));
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), VariableGetNode->GetName());
		}
		if (VariableSetNode != nullptr)
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("variable_name"), VariableName);
			ExecutionResult.ResultObject->SetStringField(TEXT("variable_scope"), TEXT("self"));
			ExecutionResult.ResultObject->SetStringField(TEXT("variable_value"), VariableValue);
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), VariableSetNode->GetName());
		}
		if (FunctionCallNode != nullptr)
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("function_name"), FunctionName);
			ExecutionResult.ResultObject->SetStringField(TEXT("function_target"), TEXT("self"));
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), FunctionCallNode->GetName());
		}
		if (EnhancedInputActionNode != nullptr)
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("input_action_path"), InputActionPath);
			ExecutionResult.ResultObject->SetStringField(TEXT("input_action_name"), InputAction != nullptr ? InputAction->GetName() : FString());
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), EnhancedInputActionNode->GetName());
		}
		if (CallNode != nullptr)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), CallNode->GetName());
		}
		for (int32 NodeIndex = 1; NodeIndex < PrintNodes.Num(); ++NodeIndex)
		{
			if (PrintNodes[NodeIndex] != nullptr)
			{
				AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), PrintNodes[NodeIndex]->GetName());
			}
		}
		if (bCreatedEntryEvent && EntryEventNode != nullptr)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_nodes"), EntryEventNode->GetName());
		}
		if (EntryEventNode != nullptr)
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("entry_event"), EntryEventName);
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), EntryEventNode->GetName());
			SetAppliedField(ExecutionResult.ResultObject, TEXT("entry_event"), EntryEventName);
		}
		if (BranchNode != nullptr)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), BranchNode->GetName());
		}
		if (SequenceNode != nullptr)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), SequenceNode->GetName());
		}
		if (VariableSetNode != nullptr && EntryEventNode != nullptr)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), VariableSetNode->GetName());
		}
		if (FunctionCallNode != nullptr && EntryEventNode != nullptr)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), FunctionCallNode->GetName());
		}
		if (EntryEventNode != nullptr || BranchNode != nullptr || SequenceNode != nullptr)
		{
			for (UK2Node_CallFunction* PrintNode : PrintNodes)
			{
				if (PrintNode != nullptr)
				{
					AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), PrintNode->GetName());
				}
			}
		}
		for (const FString& LinkedPinsSummary : LinkedPinsSummaries)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_pins"), LinkedPinsSummary);
		}
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), Blueprint->GetOutermost() != nullptr ? Blueprint->GetOutermost()->GetName() : FString());
		SetAppliedField(ExecutionResult.ResultObject, TEXT("template_id"), TemplateId);
		if (bUsesPrintString)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("message"), Message.IsEmpty() ? TEXT("Hello from UEAgent") : Message);
		}
		SetAppliedField(ExecutionResult.ResultObject, TEXT("graph_name"), TargetGraph->GetName());
		if (VariableGetNode != nullptr || VariableSetNode != nullptr)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("variable_name"), VariableName);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("variable_scope"), TEXT("self"));
		}
		if (VariableSetNode != nullptr && !VariableValue.IsEmpty())
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("variable_value"), VariableValue);
		}
		if (FunctionCallNode != nullptr)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("function_name"), FunctionName);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("function_target"), TEXT("self"));
		}
		if (EnhancedInputActionNode != nullptr)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("input_action_path"), InputActionPath);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("input_action_name"), InputAction != nullptr ? InputAction->GetName() : FString());
		}
		if (BranchNode != nullptr)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("branch_path"), BranchPath);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("condition_default"), bConditionDefault ? TEXT("true") : TEXT("false"));
		}
		if (SequenceNode != nullptr)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("sequence_output_count"), FString::FromInt(PrintNodes.Num()));
		}
		if (!bCompileSuccess)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_compile_failed"), FString::Printf(TEXT("Blueprint compile status: %s"), *CompileStatus));
		}
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

	static FEditorOperationExecutionResult ExecuteConnectBlueprintNodes(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UEdGraphSchema.TryCreateConnection"));

		const FString BlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("blueprint_path")));
		FString GraphName = GetScalarFieldAsString(PayloadObject, TEXT("graph_name")).TrimStartAndEnd();
		const FString SourceNodeId = GetScalarFieldAsString(PayloadObject, TEXT("source_node_id")).TrimStartAndEnd();
		const FString SourcePinName = GetScalarFieldAsString(PayloadObject, TEXT("source_pin_name")).TrimStartAndEnd();
		const FString TargetNodeId = GetScalarFieldAsString(PayloadObject, TEXT("target_node_id")).TrimStartAndEnd();
		const FString TargetPinName = GetScalarFieldAsString(PayloadObject, TEXT("target_pin_name")).TrimStartAndEnd();
		const bool bCompileAfterEdit = GetBoolFieldOrDefault(PayloadObject, TEXT("compile_after_edit"), true);
		if (GraphName.IsEmpty())
		{
			GraphName = TEXT("EventGraph");
		}
		if (BlueprintPath.IsEmpty() || SourceNodeId.IsEmpty() || SourcePinName.IsEmpty() || TargetNodeId.IsEmpty() || TargetPinName.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("blueprint_path, graph_name, source_node_id, source_pin_name, target_node_id and target_pin_name are required."));
			return ExecutionResult;
		}

		UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath);
		if (Blueprint == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return ExecutionResult;
		}

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
		if (TargetGraph == nullptr && GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			TargetGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		}
		if (TargetGraph == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_graph_unavailable"), GraphName);
			return ExecutionResult;
		}

		auto MatchesNodeIdentifier = [](const UEdGraphNode* Node, const FString& NodeIdentifier) -> bool
		{
			if (Node == nullptr)
			{
				return false;
			}
			if (Node->GetName().Equals(NodeIdentifier, ESearchCase::IgnoreCase))
			{
				return true;
			}
			const FString GuidWithHyphens = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			const FString GuidDigits = Node->NodeGuid.ToString(EGuidFormats::Digits);
			return GuidWithHyphens.Equals(NodeIdentifier, ESearchCase::IgnoreCase)
				|| GuidDigits.Equals(NodeIdentifier, ESearchCase::IgnoreCase);
		};
		auto FindNodeByIdentifier = [&TargetGraph, &MatchesNodeIdentifier](const FString& NodeIdentifier) -> UEdGraphNode*
		{
			if (TargetGraph == nullptr)
			{
				return nullptr;
			}
			for (UEdGraphNode* Node : TargetGraph->Nodes)
			{
				if (MatchesNodeIdentifier(Node, NodeIdentifier))
				{
					return Node;
				}
			}
			return nullptr;
		};
		auto FindPinByName = [](UEdGraphNode* Node, const FString& PinName) -> UEdGraphPin*
		{
			if (Node == nullptr)
			{
				return nullptr;
			}
			if (UEdGraphPin* DirectPin = Node->FindPin(FName(*PinName)))
			{
				return DirectPin;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
				{
					return Pin;
				}
			}
			return nullptr;
		};

		UEdGraphNode* SourceNode = FindNodeByIdentifier(SourceNodeId);
		UEdGraphNode* TargetNode = FindNodeByIdentifier(TargetNodeId);
		if (SourceNode == nullptr || TargetNode == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_node_not_found"), TEXT("source_node_id or target_node_id was not found in the target graph."));
			return ExecutionResult;
		}
		UEdGraphPin* SourcePin = FindPinByName(SourceNode, SourcePinName);
		UEdGraphPin* TargetPin = FindPinByName(TargetNode, TargetPinName);
		if (SourcePin == nullptr || TargetPin == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_pin_not_found"), TEXT("source_pin_name or target_pin_name was not found on the selected nodes."));
			return ExecutionResult;
		}
		if (SourcePin->Direction != EGPD_Output || TargetPin->Direction != EGPD_Input)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_pin_direction_invalid"), TEXT("connect_blueprint_nodes requires Source Output -> Target Input."));
			return ExecutionResult;
		}

		const bool bAlreadyLinked = SourcePin->LinkedTo.Contains(TargetPin);
		if (!bAlreadyLinked && (SourcePin->LinkedTo.Num() > 0 || TargetPin->LinkedTo.Num() > 0))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_pin_already_linked"), TEXT("One of the selected pins is already linked; v1 will not break or rewrite existing links."));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Connect Blueprint Nodes")));
		Blueprint->Modify();
		TargetGraph->Modify();

		bool bLinked = bAlreadyLinked;
		if (!bLinked)
		{
			if (const UEdGraphSchema* Schema = TargetGraph->GetSchema())
			{
				bLinked = Schema->TryCreateConnection(SourcePin, TargetPin);
			}
		}
		if (!bLinked)
		{
			ExecutionResult.ExecutionState = TEXT("failed");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_pin_connection_failed"), TEXT("UE graph schema rejected the pin connection."));
			return ExecutionResult;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		FString CompileStatus = BlueprintStatusToOperationString(Blueprint->Status);
		bool bCompileSuccess = true;
		if (bCompileAfterEdit)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			CompileStatus = BlueprintStatusToOperationString(Blueprint->Status);
			bCompileSuccess = Blueprint->Status != BS_Error;
		}

		const FString LinkSummary = FString::Printf(TEXT("%s.%s -> %s.%s"),
			*SourceNode->GetName(),
			*SourcePin->PinName.ToString(),
			*TargetNode->GetName(),
			*TargetPin->PinName.ToString());
		ExecutionResult.bSuccess = bCompileSuccess;
		ExecutionResult.ExecutionState = bCompileSuccess ? TEXT("completed") : TEXT("failed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to remove the created Blueprint pin link. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
		ExecutionResult.ResultObject->SetStringField(TEXT("source_node_id"), SourceNodeId);
		ExecutionResult.ResultObject->SetStringField(TEXT("source_pin_name"), SourcePinName);
		ExecutionResult.ResultObject->SetStringField(TEXT("target_node_id"), TargetNodeId);
		ExecutionResult.ResultObject->SetStringField(TEXT("target_pin_name"), TargetPinName);
		ExecutionResult.ResultObject->SetStringField(TEXT("compile_status"), CompileStatus);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("compiled_after_edit"), bCompileAfterEdit);
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), Blueprint->GetOutermost() != nullptr && Blueprint->GetOutermost()->IsDirty());
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), SourceNode->GetName());
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_nodes"), TargetNode->GetName());
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("linked_pins"), LinkSummary);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), Blueprint->GetOutermost() != nullptr ? Blueprint->GetOutermost()->GetName() : FString());
		SetAppliedField(ExecutionResult.ResultObject, TEXT("source_pin_name"), SourcePinName);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("target_pin_name"), TargetPinName);
		if (!bCompileSuccess)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("blueprint_compile_failed"), FString::Printf(TEXT("Blueprint compile status: %s"), *CompileStatus));
		}
		return ExecutionResult;
	}

	static FEditorOperationExecutionResult ExecuteAddUmgWidget(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UWidgetTree.ConstructWidget"));

		const FString WidgetBlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("widget_blueprint_path")));
		const FString WidgetName = GetScalarFieldAsString(PayloadObject, TEXT("widget_name")).TrimStartAndEnd();
		const FString WidgetClassPath = GetScalarFieldAsString(PayloadObject, TEXT("widget_class")).TrimStartAndEnd();
		const FString ParentWidgetName = GetScalarFieldAsString(PayloadObject, TEXT("parent_widget_name")).TrimStartAndEnd();
		const FString TextValue = GetScalarFieldAsString(PayloadObject, TEXT("text"));
		if (WidgetBlueprintPath.IsEmpty() || WidgetName.IsEmpty() || WidgetClassPath.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("widget_blueprint_path, widget_name and widget_class are required."));
			return ExecutionResult;
		}

		FText InvalidNameReason;
		const FName WidgetFName(*WidgetName);
		if (!WidgetFName.IsValidXName(INVALID_NAME_CHARACTERS, &InvalidNameReason))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("invalid_widget_name"), InvalidNameReason.ToString());
			return ExecutionResult;
		}

		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(WidgetBlueprintPath);
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint not found: %s"), *WidgetBlueprintPath));
			return ExecutionResult;
		}

		bool bWidgetAlreadyExists = false;
		WidgetBlueprint->WidgetTree->ForEachWidget([&bWidgetAlreadyExists, WidgetFName](UWidget* ExistingWidget)
		{
			if (ExistingWidget != nullptr && ExistingWidget->GetFName() == WidgetFName)
			{
				bWidgetAlreadyExists = true;
			}
		});
		if (bWidgetAlreadyExists)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_already_exists"), WidgetName);
			return ExecutionResult;
		}

		UClass* WidgetClass = ResolveUmgWidgetClass(WidgetClassPath);
		if (WidgetClass == nullptr || !WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_class_unsupported"), WidgetClassPath);
			return ExecutionResult;
		}

		UWidget* ParentWidget = nullptr;
		if (!ParentWidgetName.IsEmpty())
		{
			WidgetBlueprint->WidgetTree->ForEachWidget([&ParentWidget, ParentWidgetName](UWidget* ExistingWidget)
			{
				if (ExistingWidget != nullptr && ExistingWidget->GetName().Equals(ParentWidgetName, ESearchCase::IgnoreCase))
				{
					ParentWidget = ExistingWidget;
				}
			});
			if (ParentWidget == nullptr)
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("parent_widget_not_found"), ParentWidgetName);
				return ExecutionResult;
			}
		}
		else
		{
			ParentWidget = WidgetBlueprint->WidgetTree->RootWidget;
		}

		UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (WidgetBlueprint->WidgetTree->RootWidget != nullptr && ParentPanel == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("parent_widget_is_not_panel"), ParentWidgetName.IsEmpty() ? TEXT("RootWidget") : ParentWidgetName);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Add UMG Widget")));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();

		TSubclassOf<UWidget> WidgetSubclass(WidgetClass);
		UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetSubclass, WidgetFName);
		if (NewWidget == nullptr)
		{
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_creation_failed"), WidgetName);
			return ExecutionResult;
		}
		NewWidget->SetFlags(RF_Transactional);
		NewWidget->Modify();

		bool bIsVariable = true;
		TryGetBoolField(PayloadObject, TEXT("is_variable"), bIsVariable);
		NewWidget->bIsVariable = bIsVariable;
		SetAppliedField(ExecutionResult.ResultObject, TEXT("is_variable"), bIsVariable ? TEXT("true") : TEXT("false"));

		if (!TextValue.IsEmpty())
		{
			if (UTextBlock* TextBlock = Cast<UTextBlock>(NewWidget))
			{
				TextBlock->SetText(FText::FromString(TextValue));
				SetAppliedField(ExecutionResult.ResultObject, TEXT("text"), TextValue);
			}
			else
			{
				AddFailedField(ExecutionResult.ResultObject, TEXT("text"), TEXT("text is only applied to TextBlock widgets in v1"));
			}
		}

		if (WidgetBlueprint->WidgetTree->RootWidget == nullptr)
		{
			WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
			SetAppliedField(ExecutionResult.ResultObject, TEXT("root_widget"), WidgetName);
		}
		else
		{
			ParentPanel->Modify();
			UPanelSlot* NewSlot = ParentPanel->AddChild(NewWidget);
			if (NewSlot != nullptr)
			{
				NewSlot->SetFlags(RF_Transactional);
				NewSlot->Modify();
			}
			SetAppliedField(ExecutionResult.ResultObject, TEXT("parent_widget_name"), ParentPanel->GetName());
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to remove the added widget. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_name"), WidgetName);
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_widgets"), WidgetName);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), WidgetBlueprint->GetOutermost() != nullptr ? WidgetBlueprint->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetUmgWidgetText(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UTextBlock.SetText"));

		const FString WidgetBlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("widget_blueprint_path")));
		const FString WidgetName = GetScalarFieldAsString(PayloadObject, TEXT("widget_name")).TrimStartAndEnd();
		const FString TextValue = GetScalarFieldAsString(PayloadObject, TEXT("text"));
		if (WidgetBlueprintPath.IsEmpty() || WidgetName.IsEmpty() || TextValue.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("widget_blueprint_path, widget_name and text are required."));
			return ExecutionResult;
		}

		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(WidgetBlueprintPath);
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint not found: %s"), *WidgetBlueprintPath));
			return ExecutionResult;
		}

		UWidget* TargetWidget = nullptr;
		WidgetBlueprint->WidgetTree->ForEachWidget([&TargetWidget, WidgetName](UWidget* ExistingWidget)
		{
			if (ExistingWidget != nullptr && ExistingWidget->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
			{
				TargetWidget = ExistingWidget;
			}
		});
		if (TargetWidget == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_not_found"), WidgetName);
			return ExecutionResult;
		}

		UTextBlock* TextBlock = Cast<UTextBlock>(TargetWidget);
		if (TextBlock == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_is_not_textblock"), WidgetName);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set UMG Widget Text")));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		TextBlock->Modify();
		TextBlock->SetText(FText::FromString(TextValue));
		TextBlock->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to revert the TextBlock text. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_name"), WidgetName);
		ExecutionResult.ResultObject->SetStringField(TEXT("text"), TextValue);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("text"), TextValue);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), WidgetBlueprint->GetOutermost() != nullptr ? WidgetBlueprint->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetUmgWidgetLayout(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UCanvasPanelSlot.SetLayoutFields"));

		const FString WidgetBlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("widget_blueprint_path")));
		const FString WidgetName = GetScalarFieldAsString(PayloadObject, TEXT("widget_name")).TrimStartAndEnd();
		const TSharedPtr<FJsonObject> LayoutObject = GetObjectField(PayloadObject, TEXT("layout"));
		if (WidgetBlueprintPath.IsEmpty() || WidgetName.IsEmpty() || !LayoutObject.IsValid())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("widget_blueprint_path, widget_name and layout are required."));
			return ExecutionResult;
		}

		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(WidgetBlueprintPath);
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint not found: %s"), *WidgetBlueprintPath));
			return ExecutionResult;
		}

		UWidget* TargetWidget = nullptr;
		WidgetBlueprint->WidgetTree->ForEachWidget([&TargetWidget, WidgetName](UWidget* ExistingWidget)
		{
			if (ExistingWidget != nullptr && ExistingWidget->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
			{
				TargetWidget = ExistingWidget;
			}
		});
		if (TargetWidget == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_not_found"), WidgetName);
			return ExecutionResult;
		}

		UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(TargetWidget->Slot);
		if (CanvasSlot == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_slot_is_not_canvas_panel_slot"), WidgetName);
			return ExecutionResult;
		}

		FVector2D Position;
		FVector2D Size;
		FVector2D Alignment;
		FAnchors Anchors;
		const bool bHasPosition = TryReadVector2Field(LayoutObject, TEXT("position"), Position);
		const bool bHasSize = TryReadVector2Field(LayoutObject, TEXT("size"), Size);
		const bool bHasAlignment = TryReadVector2Field(LayoutObject, TEXT("alignment"), Alignment);
		const bool bHasAnchors = TryReadAnchorsObject(GetObjectField(LayoutObject, TEXT("anchors")), Anchors);
		if (!bHasPosition && !bHasSize && !bHasAlignment && !bHasAnchors)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("layout_has_no_supported_fields"), TEXT("layout must contain position, size, alignment or anchors."));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set UMG Widget Layout")));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		TargetWidget->Modify();
		CanvasSlot->Modify();
		if (bHasPosition)
		{
			CanvasSlot->SetPosition(Position);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("layout.position"), Position.ToString());
		}
		if (bHasSize)
		{
			CanvasSlot->SetSize(Size);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("layout.size"), Size.ToString());
		}
		if (bHasAlignment)
		{
			CanvasSlot->SetAlignment(Alignment);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("layout.alignment"), Alignment.ToString());
		}
		if (bHasAnchors)
		{
			CanvasSlot->SetAnchors(Anchors);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("layout.anchors"), FString::Printf(TEXT("%s,%s"), *Anchors.Minimum.ToString(), *Anchors.Maximum.ToString()));
		}
		CanvasSlot->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to revert the CanvasPanelSlot layout. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_name"), WidgetName);
		ExecutionResult.ResultObject->SetStringField(TEXT("slot_type"), TEXT("CanvasPanelSlot"));
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), WidgetBlueprint->GetOutermost() != nullptr ? WidgetBlueprint->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetUmgWidgetVisibility(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UWidget.SetVisibility"));

		const FString WidgetBlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("widget_blueprint_path")));
		const FString WidgetName = GetScalarFieldAsString(PayloadObject, TEXT("widget_name")).TrimStartAndEnd();
		const FString VisibilityValue = GetScalarFieldAsString(PayloadObject, TEXT("visibility")).TrimStartAndEnd();
		if (WidgetBlueprintPath.IsEmpty() || WidgetName.IsEmpty() || VisibilityValue.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("widget_blueprint_path, widget_name and visibility are required."));
			return ExecutionResult;
		}

		ESlateVisibility NewVisibility = ESlateVisibility::Visible;
		if (!TryParseSlateVisibility(VisibilityValue, NewVisibility))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("visibility_unsupported"), VisibilityValue);
			return ExecutionResult;
		}

		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(WidgetBlueprintPath);
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint not found: %s"), *WidgetBlueprintPath));
			return ExecutionResult;
		}

		UWidget* TargetWidget = nullptr;
		WidgetBlueprint->WidgetTree->ForEachWidget([&TargetWidget, WidgetName](UWidget* ExistingWidget)
		{
			if (ExistingWidget != nullptr && ExistingWidget->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
			{
				TargetWidget = ExistingWidget;
			}
		});
		if (TargetWidget == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_not_found"), WidgetName);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set UMG Widget Visibility")));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		TargetWidget->Modify();
		TargetWidget->SetVisibility(NewVisibility);
		TargetWidget->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to revert the widget visibility. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_name"), WidgetName);
		ExecutionResult.ResultObject->SetStringField(TEXT("visibility"), VisibilityValue);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("visibility"), VisibilityValue);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), WidgetBlueprint->GetOutermost() != nullptr ? WidgetBlueprint->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetUmgWidgetAppearance(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UWidget appearance setters"));

		const FString WidgetBlueprintPath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("widget_blueprint_path")));
		const FString WidgetName = GetScalarFieldAsString(PayloadObject, TEXT("widget_name")).TrimStartAndEnd();
		const TSharedPtr<FJsonObject> AppearanceObject = GetObjectField(PayloadObject, TEXT("appearance"));
		if (WidgetBlueprintPath.IsEmpty() || WidgetName.IsEmpty() || !AppearanceObject.IsValid())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("widget_blueprint_path, widget_name and appearance are required."));
			return ExecutionResult;
		}

		double RenderOpacity = 0.0;
		const bool bHasRenderOpacity = AppearanceObject->TryGetNumberField(TEXT("render_opacity"), RenderOpacity);
		bool bIsEnabled = false;
		const bool bHasIsEnabled = AppearanceObject->TryGetBoolField(TEXT("is_enabled"), bIsEnabled);
		FLinearColor ColorAndOpacity;
		const bool bHasColorAndOpacity = TryReadLinearColorObject(GetObjectField(AppearanceObject, TEXT("color_and_opacity")), ColorAndOpacity);
		int32 FontSize = 0;
		const bool bHasFontSize = TryGetIntegerField(AppearanceObject, TEXT("font_size"), FontSize);
		if (!bHasRenderOpacity && !bHasIsEnabled && !bHasColorAndOpacity && !bHasFontSize)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("appearance_empty"), TEXT("appearance must contain render_opacity, is_enabled, color_and_opacity, or font_size."));
			return ExecutionResult;
		}

		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(WidgetBlueprintPath);
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint not found: %s"), *WidgetBlueprintPath));
			return ExecutionResult;
		}

		UWidget* TargetWidget = nullptr;
		WidgetBlueprint->WidgetTree->ForEachWidget([&TargetWidget, WidgetName](UWidget* ExistingWidget)
		{
			if (ExistingWidget != nullptr && ExistingWidget->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
			{
				TargetWidget = ExistingWidget;
			}
		});
		if (TargetWidget == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("widget_not_found"), WidgetName);
			return ExecutionResult;
		}

		UTextBlock* TextBlock = Cast<UTextBlock>(TargetWidget);
		if ((bHasColorAndOpacity || bHasFontSize) && TextBlock == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("textblock_required"), TEXT("color_and_opacity and font_size are currently supported only for UTextBlock widgets."));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set UMG Widget Appearance")));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		TargetWidget->Modify();

		if (bHasRenderOpacity)
		{
			const float ClampedOpacity = FMath::Clamp(static_cast<float>(RenderOpacity), 0.0f, 1.0f);
			TargetWidget->SetRenderOpacity(ClampedOpacity);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("render_opacity"), FString::SanitizeFloat(ClampedOpacity));
		}
		if (bHasIsEnabled)
		{
			TargetWidget->SetIsEnabled(bIsEnabled);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("is_enabled"), bIsEnabled ? TEXT("true") : TEXT("false"));
		}
		if (TextBlock != nullptr && bHasColorAndOpacity)
		{
			TextBlock->SetColorAndOpacity(FSlateColor(ColorAndOpacity));
			SetAppliedField(ExecutionResult.ResultObject, TEXT("color_and_opacity"), ColorAndOpacity.ToString());
		}
		if (TextBlock != nullptr && bHasFontSize)
		{
			FSlateFontInfo FontInfo = TextBlock->GetFont();
			FontInfo.Size = FontSize;
			TextBlock->SetFont(FontInfo);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("font_size"), FString::FromInt(FontSize));
		}
		TargetWidget->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to revert the widget appearance. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
		ExecutionResult.ResultObject->SetStringField(TEXT("widget_name"), WidgetName);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		if (bHasRenderOpacity)
		{
			ExecutionResult.ResultObject->SetNumberField(TEXT("render_opacity"), FMath::Clamp(RenderOpacity, 0.0, 1.0));
		}
		if (bHasIsEnabled)
		{
			ExecutionResult.ResultObject->SetBoolField(TEXT("is_enabled"), bIsEnabled);
		}
		if (bHasColorAndOpacity)
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("color_and_opacity"), ColorAndOpacity.ToString());
		}
		if (bHasFontSize)
		{
			ExecutionResult.ResultObject->SetNumberField(TEXT("font_size"), FontSize);
		}
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), WidgetBlueprint->GetOutermost() != nullptr ? WidgetBlueprint->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecutePlaceActorInLevel(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("GEditor.AddActor"));

		const FString ActorClassPath = GetScalarFieldAsString(PayloadObject, TEXT("actor_class")).TrimStartAndEnd();
		const FString ActorLabel = GetScalarFieldAsString(PayloadObject, TEXT("actor_label")).TrimStartAndEnd();
		if (ActorClassPath.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("actor_class is required."));
			return ExecutionResult;
		}

		UClass* ActorClass = ResolveBlueprintParentClass(ActorClassPath);
		if (ActorClass == nullptr || !ActorClass->IsChildOf(AActor::StaticClass()))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("actor_class_invalid"), ActorClassPath);
			return ExecutionResult;
		}
		if (GEditor == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("editor_unavailable"), TEXT("GEditor is unavailable."));
			return ExecutionResult;
		}

		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (EditorWorld == nullptr || EditorWorld->GetCurrentLevel() == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("editor_world_unavailable"), TEXT("No editable level is currently available."));
			return ExecutionResult;
		}

		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		const TSharedPtr<FJsonObject> TransformObject = GetObjectField(PayloadObject, TEXT("transform"));
		if (TransformObject.IsValid())
		{
			TryReadVectorField(TransformObject, TEXT("location"), Location);
			TryReadRotatorField(TransformObject, TEXT("rotation"), Rotation);
			TryReadVectorField(TransformObject, TEXT("scale"), Scale);
		}

		const FTransform ActorTransform(Rotation, Location, Scale);
		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Place Actor In Level")));
		EditorWorld->Modify();
		ULevel* CurrentLevel = EditorWorld->GetCurrentLevel();
		CurrentLevel->Modify();
		AActor* NewActor = GEditor->AddActor(CurrentLevel, ActorClass, ActorTransform, false, RF_Transactional);
		if (NewActor == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("failed");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("actor_spawn_failed"), ActorClassPath);
			return ExecutionResult;
		}

		NewActor->SetFlags(RF_Transactional);
		NewActor->Modify();
		if (!ActorLabel.IsEmpty())
		{
			NewActor->SetActorLabel(ActorLabel);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("actor_label"), ActorLabel);
		}
		CurrentLevel->MarkPackageDirty();

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to remove the placed Actor. The level is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_class"), ActorClass->GetPathName());
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_label"), NewActor->GetActorLabel());
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_name"), NewActor->GetName());
		ExecutionResult.ResultObject->SetStringField(TEXT("level_name"), CurrentLevel->GetOutermost() != nullptr ? CurrentLevel->GetOutermost()->GetName() : CurrentLevel->GetName());
		ExecutionResult.ResultObject->SetStringField(TEXT("location"), Location.ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("rotation"), Rotation.ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("scale"), Scale.ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("created_actors"), NewActor->GetActorLabel());
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), CurrentLevel->GetOutermost() != nullptr ? CurrentLevel->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}

	static AActor* FindActorByReference(UWorld* EditorWorld, const FString& ActorReference)
	{
		const FString Reference = ActorReference.TrimStartAndEnd();
		if (EditorWorld == nullptr || Reference.IsEmpty())
		{
			return nullptr;
		}

		for (ULevel* Level : EditorWorld->GetLevels())
		{
			if (Level == nullptr)
			{
				continue;
			}
			for (AActor* Actor : Level->Actors)
			{
				if (Actor == nullptr || Actor->IsPendingKillPending())
				{
					continue;
				}

				const FString ActorLabel = Actor->GetActorLabel();
				const FString ActorName = Actor->GetName();
				const FString ActorPath = Actor->GetPathName();
				if (Reference.Equals(ActorLabel, ESearchCase::IgnoreCase)
					|| Reference.Equals(ActorName, ESearchCase::IgnoreCase)
					|| Reference.Equals(ActorPath, ESearchCase::IgnoreCase))
				{
					return Actor;
				}
			}
		}
		return nullptr;
	}

	static FEditorOperationExecutionResult ExecuteSetActorTransform(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("AActor.SetActorTransform"));

		const FString ActorReference = GetScalarFieldAsString(PayloadObject, TEXT("actor_reference")).TrimStartAndEnd();
		const FString TransformMode = GetScalarFieldAsString(PayloadObject, TEXT("transform_mode")).TrimStartAndEnd().ToLower();
		if (ActorReference.IsEmpty() || TransformMode.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("actor_reference and transform_mode are required."));
			return ExecutionResult;
		}
		if (TransformMode != TEXT("absolute") && TransformMode != TEXT("delta"))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("transform_mode_unsupported"), TEXT("Only absolute and delta transform modes are supported."));
			return ExecutionResult;
		}
		if (GEditor == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("editor_unavailable"), TEXT("GEditor is unavailable."));
			return ExecutionResult;
		}

		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		AActor* TargetActor = FindActorByReference(EditorWorld, ActorReference);
		if (TargetActor == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("actor_not_found"), ActorReference);
			return ExecutionResult;
		}

		const TCHAR* TransformFieldName = TransformMode == TEXT("delta") ? TEXT("transform_delta") : TEXT("transform");
		const TSharedPtr<FJsonObject> TransformObject = GetObjectField(PayloadObject, TransformFieldName);
		if (!TransformObject.IsValid() || TransformObject->Values.Num() == 0)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("transform_payload_required"), TEXT("transform or transform_delta must contain location, rotation, or scale."));
			return ExecutionResult;
		}

		FVector LocationValue;
		FRotator RotationValue;
		FVector ScaleValue;
		const bool bHasLocation = TryReadVectorField(TransformObject, TEXT("location"), LocationValue);
		const bool bHasRotation = TryReadRotatorField(TransformObject, TEXT("rotation"), RotationValue);
		const bool bHasScale = TryReadVectorField(TransformObject, TEXT("scale"), ScaleValue);
		if (!bHasLocation && !bHasRotation && !bHasScale)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("transform_fields_missing"), TEXT("No supported transform fields were provided."));
			return ExecutionResult;
		}

		FVector NewLocation = TargetActor->GetActorLocation();
		FRotator NewRotation = TargetActor->GetActorRotation();
		FVector NewScale = TargetActor->GetActorScale3D();
		if (TransformMode == TEXT("delta"))
		{
			if (bHasLocation)
			{
				NewLocation += LocationValue;
			}
			if (bHasRotation)
			{
				NewRotation.Pitch += RotationValue.Pitch;
				NewRotation.Yaw += RotationValue.Yaw;
				NewRotation.Roll += RotationValue.Roll;
			}
			if (bHasScale)
			{
				NewScale = FVector(NewScale.X * ScaleValue.X, NewScale.Y * ScaleValue.Y, NewScale.Z * ScaleValue.Z);
			}
		}
		else
		{
			if (bHasLocation)
			{
				NewLocation = LocationValue;
			}
			if (bHasRotation)
			{
				NewRotation = RotationValue;
			}
			if (bHasScale)
			{
				NewScale = ScaleValue;
			}
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set Actor Transform")));
		TargetActor->SetFlags(RF_Transactional);
		TargetActor->Modify();
		if (ULevel* ActorLevel = TargetActor->GetLevel())
		{
			ActorLevel->Modify();
		}

		TargetActor->SetActorLocation(NewLocation);
		TargetActor->SetActorRotation(NewRotation);
		TargetActor->SetActorScale3D(NewScale);
		TargetActor->PostEditMove(true);
		if (ULevel* ActorLevel = TargetActor->GetLevel())
		{
			ActorLevel->MarkPackageDirty();
		}

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to revert the Actor transform. The level is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_reference"), ActorReference);
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_label"), TargetActor->GetActorLabel());
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_name"), TargetActor->GetName());
		ExecutionResult.ResultObject->SetStringField(TEXT("transform_mode"), TransformMode);
		ExecutionResult.ResultObject->SetStringField(TEXT("location"), NewLocation.ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("rotation"), NewRotation.ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("scale"), NewScale.ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		if (bHasLocation)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("transform.location"), NewLocation.ToString());
		}
		if (bHasRotation)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("transform.rotation"), NewRotation.ToString());
		}
		if (bHasScale)
		{
			SetAppliedField(ExecutionResult.ResultObject, TEXT("transform.scale"), NewScale.ToString());
		}
		if (ULevel* ActorLevel = TargetActor->GetLevel())
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("level_name"), ActorLevel->GetOutermost() != nullptr ? ActorLevel->GetOutermost()->GetName() : ActorLevel->GetName());
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), ActorLevel->GetOutermost() != nullptr ? ActorLevel->GetOutermost()->GetName() : FString());
		}
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetActorMetadata(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("AActor.SetActorLabel/SetFolderPath/Tags"));

		const FString ActorReference = GetScalarFieldAsString(PayloadObject, TEXT("actor_reference")).TrimStartAndEnd();
		const TSharedPtr<FJsonObject> MetadataObject = GetObjectField(PayloadObject, TEXT("metadata"));
		const FString NewActorLabel = GetScalarFieldAsString(MetadataObject, TEXT("actor_label")).TrimStartAndEnd();
		FString NewFolderPath = GetScalarFieldAsString(MetadataObject, TEXT("folder_path")).TrimStartAndEnd();
		NewFolderPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		NewFolderPath.RemoveFromStart(TEXT("/"));
		NewFolderPath.RemoveFromEnd(TEXT("/"));
		FString TagMode = GetScalarFieldAsString(MetadataObject, TEXT("tag_mode")).TrimStartAndEnd().ToLower();
		if (TagMode.IsEmpty())
		{
			TagMode = TEXT("replace");
		}
		const TArray<FString> Tags = GetStringArrayField(MetadataObject, TEXT("tags"));

		if (ActorReference.IsEmpty() || !MetadataObject.IsValid())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("actor_reference and metadata are required."));
			return ExecutionResult;
		}
		if (NewActorLabel.IsEmpty() && NewFolderPath.IsEmpty() && Tags.Num() == 0)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("metadata_empty"), TEXT("metadata must contain actor_label, folder_path, or tags."));
			return ExecutionResult;
		}
		if (TagMode != TEXT("replace") && TagMode != TEXT("append") && TagMode != TEXT("remove"))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("tag_mode_unsupported"), TEXT("Only replace, append and remove tag modes are supported."));
			return ExecutionResult;
		}
		if (GEditor == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("editor_unavailable"), TEXT("GEditor is unavailable."));
			return ExecutionResult;
		}

		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		AActor* TargetActor = FindActorByReference(EditorWorld, ActorReference);
		if (TargetActor == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("actor_not_found"), ActorReference);
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set Actor Metadata")));
		TargetActor->SetFlags(RF_Transactional);
		TargetActor->Modify();
		if (ULevel* ActorLevel = TargetActor->GetLevel())
		{
			ActorLevel->Modify();
		}

		if (!NewActorLabel.IsEmpty())
		{
			TargetActor->SetActorLabel(NewActorLabel);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("actor_label"), NewActorLabel);
		}
		if (!NewFolderPath.IsEmpty())
		{
			TargetActor->SetFolderPath(FName(*NewFolderPath));
			SetAppliedField(ExecutionResult.ResultObject, TEXT("folder_path"), NewFolderPath);
		}
		if (Tags.Num() > 0)
		{
			if (TagMode == TEXT("replace"))
			{
				TargetActor->Tags.Reset();
			}
			for (FString TagValue : Tags)
			{
				TagValue.TrimStartAndEndInline();
				if (TagValue.IsEmpty())
				{
					continue;
				}
				const FName TagName(*TagValue);
				if (TagMode == TEXT("remove"))
				{
					TargetActor->Tags.Remove(TagName);
				}
				else
				{
					TargetActor->Tags.AddUnique(TagName);
				}
			}
			SetAppliedField(ExecutionResult.ResultObject, TEXT("tag_mode"), TagMode);
		}

		TargetActor->PostEditChange();
		if (ULevel* ActorLevel = TargetActor->GetLevel())
		{
			ActorLevel->MarkPackageDirty();
		}

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo to revert the Actor metadata change. The level is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_reference"), ActorReference);
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_label"), TargetActor->GetActorLabel());
		ExecutionResult.ResultObject->SetStringField(TEXT("actor_name"), TargetActor->GetName());
		ExecutionResult.ResultObject->SetStringField(TEXT("folder_path"), TargetActor->GetFolderPath().ToString());
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		for (const FName& ActorTag : TargetActor->Tags)
		{
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("tags"), ActorTag.ToString());
		}
		if (ULevel* ActorLevel = TargetActor->GetLevel())
		{
			ExecutionResult.ResultObject->SetStringField(TEXT("level_name"), ActorLevel->GetOutermost() != nullptr ? ActorLevel->GetOutermost()->GetName() : ActorLevel->GetName());
			AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), ActorLevel->GetOutermost() != nullptr ? ActorLevel->GetOutermost()->GetName() : FString());
		}
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UMaterialInstanceConstant.SetParameterValueEditorOnly"));

		const FString MaterialInstancePath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("material_instance_path")));
		const FString ParameterName = GetScalarFieldAsString(PayloadObject, TEXT("parameter_name")).TrimStartAndEnd();
		const FString ParameterType = GetScalarFieldAsString(PayloadObject, TEXT("parameter_type")).TrimStartAndEnd().ToLower();
		if (MaterialInstancePath.IsEmpty() || ParameterName.IsEmpty() || ParameterType.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("material_instance_path, parameter_name and parameter_type are required."));
			return ExecutionResult;
		}

		UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(LoadEditorAsset(MaterialInstancePath));
		if (MaterialInstance == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("material_instance_not_found"), FString::Printf(TEXT("Material Instance Constant not found: %s"), *MaterialInstancePath));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set Material Instance Parameter")));
		MaterialInstance->Modify();
		const FMaterialParameterInfo ParameterInfo{FName(*ParameterName)};

		if (ParameterType == TEXT("scalar"))
		{
			double ScalarValue = 0.0;
			if (!PayloadObject->TryGetNumberField(TEXT("value"), ScalarValue))
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("scalar_value_required"), TEXT("value must be a number for scalar parameters."));
				return ExecutionResult;
			}
			MaterialInstance->SetScalarParameterValueEditorOnly(ParameterInfo, static_cast<float>(ScalarValue));
			SetAppliedField(ExecutionResult.ResultObject, TEXT("value"), FString::SanitizeFloat(ScalarValue));
		}
		else if (ParameterType == TEXT("vector"))
		{
			FLinearColor ColorValue;
			if (!TryReadLinearColorObject(GetObjectField(PayloadObject, TEXT("value")), ColorValue))
			{
				ExecutionResult.ExecutionState = TEXT("blocked");
				AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("vector_value_required"), TEXT("value must contain r, g, b and optional a for vector parameters."));
				return ExecutionResult;
			}
			MaterialInstance->SetVectorParameterValueEditorOnly(ParameterInfo, ColorValue);
			SetAppliedField(ExecutionResult.ResultObject, TEXT("value"), ColorValue.ToString());
		}
		else
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("parameter_type_unsupported"), TEXT("Only scalar and vector parameters are supported in v1."));
			return ExecutionResult;
		}

		MaterialInstance->PostEditChange();
		MaterialInstance->MarkPackageDirty();

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo or source control to revert the Material Instance parameter change. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("material_instance_path"), MaterialInstancePath);
		ExecutionResult.ResultObject->SetStringField(TEXT("parameter_name"), ParameterName);
		ExecutionResult.ResultObject->SetStringField(TEXT("parameter_type"), ParameterType);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), MaterialInstance->GetOutermost() != nullptr ? MaterialInstance->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetMaterialInstanceTextureParameter(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UMaterialInstanceConstant.SetTextureParameterValueEditorOnly"));

		const FString MaterialInstancePath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("material_instance_path")));
		const FString ParameterName = GetScalarFieldAsString(PayloadObject, TEXT("parameter_name")).TrimStartAndEnd();
		const FString TexturePath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("texture_path")));
		if (MaterialInstancePath.IsEmpty() || ParameterName.IsEmpty() || TexturePath.IsEmpty())
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("material_instance_path, parameter_name and texture_path are required."));
			return ExecutionResult;
		}

		UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(LoadEditorAsset(MaterialInstancePath));
		if (MaterialInstance == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("material_instance_not_found"), FString::Printf(TEXT("Material Instance Constant not found: %s"), *MaterialInstancePath));
			return ExecutionResult;
		}

		UTexture* Texture = Cast<UTexture>(LoadEditorAsset(TexturePath));
		if (Texture == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("texture_not_found"), FString::Printf(TEXT("Texture asset not found: %s"), *TexturePath));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set Material Instance Texture Parameter")));
		MaterialInstance->Modify();
		const FMaterialParameterInfo ParameterInfo{FName(*ParameterName)};
		MaterialInstance->SetTextureParameterValueEditorOnly(ParameterInfo, Texture);
		MaterialInstance->PostEditChange();
		MaterialInstance->MarkPackageDirty();

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo or source control to revert the Material Instance texture parameter change. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("material_instance_path"), MaterialInstancePath);
		ExecutionResult.ResultObject->SetStringField(TEXT("parameter_name"), ParameterName);
		ExecutionResult.ResultObject->SetStringField(TEXT("texture_path"), TexturePath);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("texture_path"), TexturePath);
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), MaterialInstance->GetOutermost() != nullptr ? MaterialInstance->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}
	static FEditorOperationExecutionResult ExecuteSetMaterialInstanceStaticSwitch(const TSharedPtr<FJsonObject>& PayloadObject, const FString& ProposalId)
	{
		FEditorOperationExecutionResult ExecutionResult;
		ExecutionResult.MetadataObject->SetStringField(TEXT("ue_api"), TEXT("UMaterialInstanceConstant.SetStaticSwitchParameterValueEditorOnly"));

		const FString MaterialInstancePath = NormalizeAssetPackagePath(GetScalarFieldAsString(PayloadObject, TEXT("material_instance_path")));
		const FString ParameterName = GetScalarFieldAsString(PayloadObject, TEXT("parameter_name")).TrimStartAndEnd();
		bool bSwitchValue = false;
		if (MaterialInstancePath.IsEmpty() || ParameterName.IsEmpty() || !TryGetBoolField(PayloadObject, TEXT("value"), bSwitchValue))
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("missing_payload"), TEXT("material_instance_path, parameter_name and boolean value are required."));
			return ExecutionResult;
		}

		UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(LoadEditorAsset(MaterialInstancePath));
		if (MaterialInstance == nullptr)
		{
			ExecutionResult.ExecutionState = TEXT("blocked");
			AddEditorOperationError(ExecutionResult.ErrorValues, TEXT("material_instance_not_found"), FString::Printf(TEXT("Material Instance Constant not found: %s"), *MaterialInstancePath));
			return ExecutionResult;
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("UE Agent Set Material Instance Static Switch")));
		MaterialInstance->Modify();
		const FMaterialParameterInfo ParameterInfo{FName(*ParameterName)};
		MaterialInstance->SetStaticSwitchParameterValueEditorOnly(ParameterInfo, bSwitchValue);
		MaterialInstance->PostEditChange();
		MaterialInstance->MarkPackageDirty();

		ExecutionResult.bSuccess = true;
		ExecutionResult.ExecutionState = TEXT("completed");
		ExecutionResult.TransactionId = FString::Printf(TEXT("ue_transaction_%s"), *ProposalId);
		ExecutionResult.UndoHint = TEXT("Use editor Undo or source control to revert the Material Instance static switch change. The package is marked dirty but not auto-saved.");
		ExecutionResult.ResultObject->SetStringField(TEXT("material_instance_path"), MaterialInstancePath);
		ExecutionResult.ResultObject->SetStringField(TEXT("parameter_name"), ParameterName);
		ExecutionResult.ResultObject->SetBoolField(TEXT("value"), bSwitchValue);
		ExecutionResult.ResultObject->SetStringField(TEXT("save_policy"), TEXT("mark_dirty_only"));
		ExecutionResult.ResultObject->SetBoolField(TEXT("dirty"), true);
		SetAppliedField(ExecutionResult.ResultObject, TEXT("value"), bSwitchValue ? TEXT("true") : TEXT("false"));
		AddResultStringArrayItem(ExecutionResult.ResultObject, TEXT("dirty_packages"), MaterialInstance->GetOutermost() != nullptr ? MaterialInstance->GetOutermost()->GetName() : FString());
		return ExecutionResult;
	}

	static bool BindEditorOperationExecutor(FUEAgentEditorToolDefinition& Definition)
	{
		if (Definition.OperationType.Equals(TEXT("rename_selected_asset"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteRenameSelectedAsset);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("batch_rename_assets"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteBatchRenameAssets);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("move_assets"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteMoveAssets);
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
		if (Definition.OperationType.Equals(TEXT("add_blueprint_node_template"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteAddBlueprintNodeTemplate);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("connect_blueprint_nodes"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteConnectBlueprintNodes);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("compile_blueprint"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteCompileBlueprint);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("add_umg_widget"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteAddUmgWidget);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_umg_widget_text"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetUmgWidgetText);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_umg_widget_layout"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetUmgWidgetLayout);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_umg_widget_visibility"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetUmgWidgetVisibility);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_umg_widget_appearance"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetUmgWidgetAppearance);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("place_actor_in_level"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecutePlaceActorInLevel);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_actor_transform"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetActorTransform);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_actor_metadata"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetActorMetadata);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_material_instance_parameter"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetMaterialInstanceParameter);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_material_instance_texture_parameter"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetMaterialInstanceTextureParameter);
			return true;
		}
		if (Definition.OperationType.Equals(TEXT("set_material_instance_static_switch"), ESearchCase::IgnoreCase))
		{
			Definition.Executor = FUEAgentEditorToolExecutor::CreateStatic(&ExecuteSetMaterialInstanceStaticSwitch);
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
	SubmitProjectInventorySnapshot(true);
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

void SAgentRootPanel::SubmitProjectInventorySnapshot(const bool bSilent)
{
	if (bSilent)
	{
		if (bAutoInventorySubmitted)
		{
			return;
		}
		bAutoInventorySubmitted = true;
	}

	RefreshEditorContext();
	if (!bSilent)
	{
		StateStore->SetBusy(true, TEXT("Collecting Project Inventory snapshot..."));
	}

	const TSharedPtr<FJsonObject> SnapshotObject = ContextCollector->BuildProjectInventorySnapshot();
	HttpClient->SubmitProjectInventorySnapshot(SnapshotObject, [StateStore = StateStore, bSilent](bool bSuccess, const FString& Message, const FString& RawText, TSharedPtr<FJsonObject> JsonObject)
	{
		if (!bSilent)
		{
			StateStore->SetBusy(false);
		}
		if (bSuccess)
		{
			if (bSilent)
			{
				StateStore->SetStatusMessage(TEXT("Project Inventory auto-synced."));
			}
			else
			{
				StateStore->ApplyProjectInventorySnapshotResponse(JsonObject);
			}
			return;
		}

		if (!bSilent)
		{
			StateStore->ApplyFailure(Message, RawText);
		}
		else
		{
			StateStore->SetStatusMessage(FString::Printf(TEXT("Project Inventory auto-sync skipped: %s"), *Message));
		}
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
