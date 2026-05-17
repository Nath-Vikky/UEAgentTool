// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AgentTypes.h"
#include "CoreMinimal.h"

class FJsonObject;

class FUEAgentStateStore : public TSharedFromThis<FUEAgentStateStore>
{
public:
	DECLARE_MULTICAST_DELEGATE(FOnStateChanged);

	FUEAgentStateStore();

	FOnStateChanged& OnStateChanged();

	void SetActiveViewMode(EUEAgentViewMode InViewMode);
	EUEAgentViewMode GetActiveViewMode() const;

	void SetActiveFunction(EUEAgentFunctionType InFunction);
	EUEAgentFunctionType GetActiveFunction() const;

	void SetActiveDebugSection(EUEAgentDebugSection InSection);
	EUEAgentDebugSection GetActiveDebugSection() const;

	void SetSettingsExpanded(bool bExpanded);
	bool IsSettingsExpanded() const;

	void SetBackendBaseUrl(const FString& InBaseUrl);
	const FString& GetBackendBaseUrl() const;
	void SetPreferredOutputLanguage(const FString& InLanguageCode, bool bBroadcast = true);
	const FString& GetPreferredOutputLanguage() const;
	FString GetEffectiveOutputLanguage() const;
	FString GetLastLanguageSource() const;

	void SetEditorContext(const FUEAgentContextSummary& InContext);
	const FUEAgentContextSummary& GetEditorContext() const;

	FUEAgentFunctionParameters& EditFunctionParameters(EUEAgentFunctionType FunctionType);
	const FUEAgentFunctionParameters& GetFunctionParameters(EUEAgentFunctionType FunctionType) const;

	const FString& GetSessionId() const;
	FString GetShortSessionId() const;
	void SetSessionId(const FString& InSessionId, bool bBroadcast = true);
	bool IsSessionSynchronized() const;
	const FString& GetSessionStatusText() const;

	void ResetSession();

	void SetBusy(bool bInBusy, const FString& InStatusMessage = FString());
	bool IsBusy() const;

	void SetStatusMessage(const FString& InStatusMessage);
	const FString& GetStatusMessage() const;
	bool IsBackendOnline() const;
	const FString& GetBackendServiceStatus() const;

	void AppendUserMessage(const FString& InText, EUEAgentFunctionType FunctionType);
	void AppendAssistantMessage(const FString& InTitle, const FString& InText, const FString& InStatusHint, const FString& InTaskId = FString(), const FString& InFunctionId = FString());
	void AppendSystemMessage(const FString& InText, const FString& InTitle = TEXT("System"));
	const TArray<TSharedPtr<FUEAgentChatMessage>>& GetChatMessages() const;

	void SetLastRequestJson(const FString& InJson);
	const FUEAgentResultSnapshot& GetLastResult() const;
	FString GetDebugSectionText(EUEAgentDebugSection Section) const;
	const FString& GetSettingsSnapshotJson() const;
	const FString& GetMetricsSnapshotText() const;
	const FString& GetSessionSummaryJson() const;
	const FString& GetSessionHistoryJson() const;

	void ApplyHealthResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyBootstrapResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyCapabilitiesResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyEditorOperationCapabilitiesResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplySettingsResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyKnowledgeBaseStatus(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyRuntimeProfilesResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyRecentTasksResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyProposalListResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplySessionSummaryResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplySessionHistoryResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplySessionTasksResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyUnifiedResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyUserViewProjectionResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyDebugViewProjectionResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyTaskTraceResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyTaskArtifactsResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyRunEventsResponse(const FString& ResponseText);
	void ApplyAlertsResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyMetricsResponse(const FString& ResponseText);
	void ApplyCodeReviewFilesResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyProjectInventorySnapshotResponse(const TSharedPtr<FJsonObject>& ResponseObject);
	void ApplyFailure(const FString& FailureMessage, const FString& RawResponse = FString());

	const TArray<TSharedPtr<FUEAgentTaskSummary>>& GetRecentTasks() const;
	const TArray<TSharedPtr<FUEAgentProposalSummary>>& GetPendingProposals() const;
	const TArray<TSharedPtr<FUEAgentCodeFileItem>>& GetCodeReviewFiles() const;
	TArray<TSharedPtr<FUEAgentRuntimeProfile>>& GetRuntimeProfiles();
	const FString& GetActiveProfileId() const;
	const FString& GetDefaultProfileId() const;

private:
	void BroadcastStateChanged();
	void AddOrUpdateRecentTask(const FUEAgentResultSnapshot& Snapshot);
	void InitializeParameterDefaults();
	void LoadPersistedState();
	void PersistState() const;
	void RebuildTraceJson();
	void RebuildMonitorJson();
	void RebuildArtifactsJson();

private:
	FOnStateChanged StateChangedDelegate;

	EUEAgentViewMode ActiveViewMode = EUEAgentViewMode::User;
	EUEAgentFunctionType ActiveFunction = EUEAgentFunctionType::AgentChat;
	EUEAgentDebugSection ActiveDebugSection = EUEAgentDebugSection::Overview;
	bool bSettingsExpanded = false;
	bool bBusy = false;
	bool bBackendOnline = false;

	FString BackendBaseUrl = TEXT("http://127.0.0.1:8000");
	FString PreferredOutputLanguage = TEXT("zh-CN");
	FString SessionId;
	bool bSessionSynchronized = false;
	FString StatusMessage;
	FString SessionStatusText = TEXT("Local session");
	FString BackendServiceStatus = TEXT("Unknown");
	FString ActiveProfileId = TEXT("default");
	FString DefaultProfileId = TEXT("default");
	FString LastHealthJson;
	FString LastBootstrapJson;
	FString LastCapabilitiesJson;
	FString LastEditorOperationCapabilitiesJson = TEXT("{}");
	FString LastKnowledgeBaseStatusJson = TEXT("{}");
	FString LastSettingsJson = TEXT("{}");
	FString LastMetricsText;
	FString LastAlertsJson = TEXT("{}");
	FString LastSessionSummaryJson = TEXT("{}");
	FString LastSessionHistoryJson = TEXT("{}");

	FUEAgentContextSummary EditorContext;
	FUEAgentResultSnapshot LastResult;
	TMap<EUEAgentFunctionType, FUEAgentFunctionParameters> ParameterDrafts;
	TArray<TSharedPtr<FUEAgentChatMessage>> ChatMessages;
	TArray<TSharedPtr<FUEAgentTaskSummary>> RecentTasks;
	TArray<TSharedPtr<FUEAgentProposalSummary>> PendingProposals;
	TArray<TSharedPtr<FUEAgentRuntimeProfile>> RuntimeProfiles;
	TArray<TSharedPtr<FUEAgentCodeFileItem>> CodeReviewFiles;
};
