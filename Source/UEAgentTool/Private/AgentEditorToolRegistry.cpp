// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentEditorToolRegistry.h"

namespace UEAgentEditorToolRegistryPrivate
{
	static void AddError(TArray<TSharedPtr<FJsonValue>>& ErrorValues, const FString& Reason, const FString& Message)
	{
		TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
		ErrorObject->SetStringField(TEXT("reason"), Reason);
		ErrorObject->SetStringField(TEXT("message"), Message);
		ErrorValues.Add(MakeShared<FJsonValueObject>(ErrorObject));
	}

	static TArray<TSharedPtr<FJsonValue>> StringsToJsonArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	static bool HasArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* ExistingValues = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(FieldName, ExistingValues) && ExistingValues != nullptr;
	}

	static bool HasObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		const TSharedPtr<FJsonObject>* ExistingObject = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(FieldName, ExistingObject) && ExistingObject != nullptr;
	}

	static void EnsureArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		if (Object.IsValid() && !HasArrayField(Object, FieldName))
		{
			Object->SetArrayField(FieldName, TArray<TSharedPtr<FJsonValue>>());
		}
	}

	static void EnsureObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		if (Object.IsValid() && !HasObjectField(Object, FieldName))
		{
			Object->SetObjectField(FieldName, MakeShared<FJsonObject>());
		}
	}

	static FString FirstNonEmptyStringField(const TSharedPtr<FJsonObject>& Object, const TArray<const TCHAR*>& FieldNames)
	{
		if (!Object.IsValid())
		{
			return FString();
		}
		for (const TCHAR* FieldName : FieldNames)
		{
			FString Value;
			if (Object->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty())
			{
				return Value.TrimStartAndEnd();
			}
		}
		return FString();
	}

	static void EnsureDirtyPackagesField(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid() || HasArrayField(Object, TEXT("dirty_packages")))
		{
			return;
		}

		const FString DirtyPackage = FirstNonEmptyStringField(Object, {
			TEXT("package_name"),
			TEXT("level_name"),
			TEXT("final_asset_path"),
			TEXT("asset_path"),
			TEXT("blueprint_path"),
			TEXT("widget_blueprint_path"),
			TEXT("material_instance_path")
		});

		TArray<TSharedPtr<FJsonValue>> DirtyPackageValues;
		if (!DirtyPackage.IsEmpty())
		{
			DirtyPackageValues.Add(MakeShared<FJsonValueString>(DirtyPackage));
		}
		Object->SetArrayField(TEXT("dirty_packages"), DirtyPackageValues);
	}

	static void EnsureEditorOperationResultContract(FUEAgentEditorToolExecutionResult& Result, const FUEAgentEditorToolDefinition* Definition)
	{
		if (!Result.ResultObject.IsValid())
		{
			Result.ResultObject = MakeShared<FJsonObject>();
		}

		Result.ResultObject->SetBoolField(TEXT("success"), Result.bSuccess);
		Result.ResultObject->SetStringField(TEXT("execution_state"), Result.ExecutionState);
		if (Definition != nullptr)
		{
			Result.ResultObject->SetStringField(TEXT("operation_type"), Definition->OperationType);
			Result.ResultObject->SetStringField(TEXT("tool_id"), Definition->ToolName.ToString());
		}
		if (!Result.UndoHint.IsEmpty())
		{
			Result.ResultObject->SetStringField(TEXT("undo_hint"), Result.UndoHint);
		}
		if (!Result.ResultObject->TryGetField(TEXT("save_policy")).IsValid())
		{
			Result.ResultObject->SetStringField(TEXT("save_policy"), Result.bSuccess ? TEXT("mark_dirty_only") : TEXT("not_applied"));
		}

		EnsureObjectField(Result.ResultObject, TEXT("applied_fields"));
		EnsureArrayField(Result.ResultObject, TEXT("failed_fields"));
		EnsureDirtyPackagesField(Result.ResultObject);
	}
}

void FUEAgentEditorToolRegistry::RegisterTool(const FUEAgentEditorToolDefinition& Definition)
{
	const FString OperationKey = NormalizeOperationType(Definition.OperationType);
	if (OperationKey.IsEmpty())
	{
		return;
	}
	ToolsByOperationType.Add(OperationKey, Definition);
}

bool FUEAgentEditorToolRegistry::ContainsOperation(const FString& OperationType) const
{
	return ToolsByOperationType.Contains(NormalizeOperationType(OperationType));
}

bool FUEAgentEditorToolRegistry::FindByOperation(const FString& OperationType, FUEAgentEditorToolDefinition& OutDefinition) const
{
	const FUEAgentEditorToolDefinition* Found = ToolsByOperationType.Find(NormalizeOperationType(OperationType));
	if (Found == nullptr)
	{
		return false;
	}
	OutDefinition = *Found;
	return true;
}

TArray<FUEAgentEditorToolDefinition> FUEAgentEditorToolRegistry::ListTools() const
{
	TArray<FUEAgentEditorToolDefinition> Tools;
	ToolsByOperationType.GenerateValueArray(Tools);
	Tools.Sort([](const FUEAgentEditorToolDefinition& Left, const FUEAgentEditorToolDefinition& Right)
	{
		return Left.OperationType < Right.OperationType;
	});
	return Tools;
}

FUEAgentEditorToolExecutionResult FUEAgentEditorToolRegistry::ExecuteTool(
	const FString& OperationType,
	const TSharedPtr<FJsonObject>& Arguments,
	const FString& ProposalId) const
{
	const FUEAgentEditorToolDefinition* Definition = ToolsByOperationType.Find(NormalizeOperationType(OperationType));
	if (Definition == nullptr)
	{
		return BuildBlockedResult(TEXT("unsupported_editor_operation"), OperationType);
	}
	if (!Definition->bEnabled)
	{
		return BuildBlockedResult(TEXT("editor_tool_disabled"), OperationType);
	}
	if (!Arguments.IsValid())
	{
		return BuildBlockedResult(TEXT("invalid_operation_payload"), TEXT("Operation payload is empty or invalid JSON."));
	}
	if (!Definition->Executor.IsBound())
	{
		return BuildBlockedResult(TEXT("editor_tool_executor_missing"), OperationType);
	}

	FUEAgentEditorToolExecutionResult Result = Definition->Executor.Execute(Arguments, ProposalId);
	if (Result.MetadataObject.IsValid())
	{
		Result.MetadataObject->SetStringField(TEXT("tool_name"), Definition->ToolName.ToString());
		Result.MetadataObject->SetStringField(TEXT("operation_type"), Definition->OperationType);
		Result.MetadataObject->SetStringField(TEXT("side_effect_level"), Definition->SideEffectLevel);
		Result.MetadataObject->SetStringField(TEXT("tool_registry"), TEXT("UEAgentEditorToolRegistry"));
	}
	UEAgentEditorToolRegistryPrivate::EnsureEditorOperationResultContract(Result, Definition);
	return Result;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolRegistry::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ToolValues;
	for (const FUEAgentEditorToolDefinition& Definition : ListTools())
	{
		TSharedPtr<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), Definition.ToolName.ToString());
		ToolObject->SetStringField(TEXT("operation_type"), Definition.OperationType);
		ToolObject->SetStringField(TEXT("description"), Definition.Description);
		ToolObject->SetStringField(TEXT("category"), Definition.Category);
		ToolObject->SetStringField(TEXT("side_effect_level"), Definition.SideEffectLevel);
		ToolObject->SetBoolField(TEXT("enabled"), Definition.bEnabled);
		ToolObject->SetBoolField(TEXT("has_executor"), Definition.Executor.IsBound());
		ToolObject->SetArrayField(TEXT("required_fields"), UEAgentEditorToolRegistryPrivate::StringsToJsonArray(Definition.RequiredFields));
		ToolObject->SetArrayField(TEXT("optional_fields"), UEAgentEditorToolRegistryPrivate::StringsToJsonArray(Definition.OptionalFields));
		ToolValues.Add(MakeShared<FJsonValueObject>(ToolObject));
	}
	Root->SetStringField(TEXT("protocol_version"), TEXT("ue_agent_editor_tool_registry_v1"));
	Root->SetNumberField(TEXT("tool_count"), ToolValues.Num());
	Root->SetArrayField(TEXT("tools"), ToolValues);
	Root->SetBoolField(TEXT("executes_without_confirmation"), false);
	return Root;
}

FString FUEAgentEditorToolRegistry::NormalizeOperationType(const FString& OperationType)
{
	return OperationType.TrimStartAndEnd().ToLower();
}

FUEAgentEditorToolExecutionResult FUEAgentEditorToolRegistry::BuildBlockedResult(const FString& Reason, const FString& Message)
{
	FUEAgentEditorToolExecutionResult Result;
	Result.ExecutionState = TEXT("blocked");
	UEAgentEditorToolRegistryPrivate::AddError(Result.ErrorValues, Reason, Message);
	if (Result.MetadataObject.IsValid())
	{
		Result.MetadataObject->SetStringField(TEXT("tool_registry"), TEXT("UEAgentEditorToolRegistry"));
	}
	UEAgentEditorToolRegistryPrivate::EnsureEditorOperationResultContract(Result, nullptr);
	return Result;
}
