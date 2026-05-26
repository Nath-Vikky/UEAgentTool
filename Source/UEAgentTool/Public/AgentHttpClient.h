// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AgentTypes.h"
#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class FUEAgentStateStore;

class FUEAgentHttpClient : public TSharedFromThis<FUEAgentHttpClient>
{
public:
	using FJsonResponseCallback = TFunction<void(bool, const FString&, const FString&, TSharedPtr<FJsonObject>)>;
	using FTextResponseCallback = TFunction<void(bool, const FString&, const FString&)>;

	explicit FUEAgentHttpClient(TSharedRef<FUEAgentStateStore> InStateStore);

	void RequestHealth(const FJsonResponseCallback& Callback) const;
	void RequestBootstrap(const FJsonResponseCallback& Callback) const;
	void RequestCapabilities(const FJsonResponseCallback& Callback) const;
	void RequestSystemSettings(const FJsonResponseCallback& Callback) const;
	void RequestKnowledgeBaseStatus(const FJsonResponseCallback& Callback) const;
	void RequestRuntimeProfiles(const FJsonResponseCallback& Callback) const;
	void RequestSystemAlerts(const FJsonResponseCallback& Callback) const;
	void RequestMetrics(const FTextResponseCallback& Callback) const;
	void CreateSession(const FString& PreferredSessionId, const FJsonResponseCallback& Callback) const;
	void RequestSessionSummary(const FString& SessionId, const FJsonResponseCallback& Callback) const;
	void RequestSessionHistory(const FString& SessionId, const FJsonResponseCallback& Callback) const;
	void RequestSessionTasks(const FString& SessionId, const FJsonResponseCallback& Callback) const;
	void ClearSession(const FString& SessionId, const FJsonResponseCallback& Callback) const;
	void ActivateProfile(const FString& ProfileId, const FJsonResponseCallback& Callback) const;
	void RequestRecentTasks(const FJsonResponseCallback& Callback) const;
	void RequestTaskDetail(const FString& TaskId, const FJsonResponseCallback& Callback) const;
	void RequestCodeReviewFiles(const FString& ProjectRoot, const FString& Query, int32 Limit, const FJsonResponseCallback& Callback) const;
	void SubmitProjectInventorySnapshot(const TSharedPtr<FJsonObject>& SnapshotObject, const FJsonResponseCallback& Callback) const;
	void RequestBlueprintGraphs(const FString& BlueprintQuery, int32 Limit, bool bIncludeNodes, const FJsonResponseCallback& Callback) const;
	void RequestTaskUserView(const FString& TaskId, const FJsonResponseCallback& Callback) const;
	void RequestTaskDebugView(const FString& TaskId, const FJsonResponseCallback& Callback) const;
	void RequestTaskTrace(const FString& TaskId, const FJsonResponseCallback& Callback) const;
	void RequestTaskArtifacts(const FString& TaskId, const FJsonResponseCallback& Callback) const;
	void RequestRunDetail(const FString& RunId, const FJsonResponseCallback& Callback) const;
	void RequestRunUserView(const FString& RunId, const FJsonResponseCallback& Callback) const;
	void RequestRunDebugView(const FString& RunId, const FJsonResponseCallback& Callback) const;
	void RequestRunEvents(const FString& RunId, const FTextResponseCallback& Callback) const;
	void RequestPendingProposals(const FJsonResponseCallback& Callback) const;
	void SubmitProposalDecision(const FString& ProposalId, const FString& Decision, const FString& Comment, const FJsonResponseCallback& Callback) const;
	void RequestEditorOperationCapabilities(const FJsonResponseCallback& Callback) const;
	void CreateWorkflowStepProposal(const TSharedPtr<FJsonObject>& RequestObject, const FJsonResponseCallback& Callback) const;
	void CreateEditorOperationFollowUpProposal(const FString& ProposalId, const TSharedPtr<FJsonObject>& RequestObject, const FJsonResponseCallback& Callback) const;
	void ConfirmEditorOperationProposal(const FString& ProposalId, const FJsonResponseCallback& Callback) const;
	void RejectEditorOperationProposal(const FString& ProposalId, const FJsonResponseCallback& Callback) const;
	void SubmitEditorOperationResult(
		const FString& ProposalId,
		const FString& OperationType,
		const FString& ExecutionState,
		bool bSuccess,
		const FString& TransactionId,
		const FString& UndoHint,
		const TSharedPtr<FJsonObject>& ResultObject,
		const TArray<TSharedPtr<FJsonValue>>& ErrorValues,
		const TSharedPtr<FJsonObject>& MetadataObject,
		const FJsonResponseCallback& Callback) const;
	void CancelRun(const FString& RunId, const FJsonResponseCallback& Callback) const;

	void SubmitFunction(
		EUEAgentFunctionType FunctionType,
		const FUEAgentContextSummary& Context,
		const FUEAgentFunctionParameters& Parameters,
		const FString& InputText,
		EUEAgentViewMode ActiveView,
		const FJsonResponseCallback& Callback) const;

private:
	TSharedPtr<FJsonObject> BuildRequestPayload(
		EUEAgentFunctionType FunctionType,
		const FUEAgentContextSummary& Context,
		const FUEAgentFunctionParameters& Parameters,
		const FString& InputText,
		EUEAgentViewMode ActiveView) const;

	void SendRequest(const FString& Verb, const FString& RelativePath, const TSharedPtr<FJsonObject>& BodyObject, const FJsonResponseCallback& Callback) const;
	void SendTextRequest(const FString& Verb, const FString& RelativePath, const FTextResponseCallback& Callback) const;
	FString BuildUrl(const FString& RelativePath) const;

private:
	TWeakPtr<FUEAgentStateStore> StateStore;
};
