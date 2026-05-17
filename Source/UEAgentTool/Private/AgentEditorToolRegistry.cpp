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
}

void FUEAgentEditorToolRegistry::RegisterTool(const FUEAgentEditorToolDefinition& Definition)
{
	const FString OperationKey = NormalizeOperationType(Definition.OperationType);
	if (OperationKey.IsEmpty() || !Definition.Executor.IsBound())
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
	return Result;
}
