// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FUEAgentEditorToolExecutionResult
{
	bool bSuccess = false;
	FString ExecutionState = TEXT("failed");
	FString TransactionId;
	FString UndoHint;
	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	TSharedPtr<FJsonObject> MetadataObject = MakeShared<FJsonObject>();
};

DECLARE_DELEGATE_RetVal_TwoParams(
	FUEAgentEditorToolExecutionResult,
	FUEAgentEditorToolExecutor,
	const TSharedPtr<FJsonObject>&,
	const FString&);

struct FUEAgentEditorToolDefinition
{
	FName ToolName;
	FString OperationType;
	FString Description;
	FString Category = TEXT("editor_operation");
	FString SideEffectLevel = TEXT("confirmed_write");
	TArray<FString> RequiredFields;
	TArray<FString> OptionalFields;
	FUEAgentEditorToolExecutor Executor;
	bool bEnabled = true;
};

class FUEAgentEditorToolRegistry
{
public:
	void RegisterTool(const FUEAgentEditorToolDefinition& Definition);
	bool ContainsOperation(const FString& OperationType) const;
	bool FindByOperation(const FString& OperationType, FUEAgentEditorToolDefinition& OutDefinition) const;
	TArray<FUEAgentEditorToolDefinition> ListTools() const;

	FUEAgentEditorToolExecutionResult ExecuteTool(
		const FString& OperationType,
		const TSharedPtr<FJsonObject>& Arguments,
		const FString& ProposalId) const;

	TSharedPtr<FJsonObject> ToJsonObject() const;

private:
	static FString NormalizeOperationType(const FString& OperationType);
	static FUEAgentEditorToolExecutionResult BuildBlockedResult(const FString& Reason, const FString& Message);

	TMap<FString, FUEAgentEditorToolDefinition> ToolsByOperationType;
};
