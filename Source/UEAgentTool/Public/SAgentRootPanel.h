// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AgentContextCollector.h"
#include "AgentHttpClient.h"
#include "AgentStateStore.h"
#include "Widgets/SCompoundWidget.h"

class SAgentRootPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAgentRootPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FUEAgentStateStore>, StateStore)
		SLATE_ARGUMENT(TSharedPtr<FUEAgentHttpClient>, HttpClient)
		SLATE_ARGUMENT(TSharedPtr<FUEAgentContextCollector>, ContextCollector)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void HandleStateChanged();
	void SyncStateSnapshots();
	void RefreshEditorContext() const;
	void InitializeSession();
	void RestoreCurrentSession();
	void ClearCurrentSession();
	void RefreshBootstrapData() const;
	void RefreshTaskData() const;
	void RefreshKnowledgeBaseStatus() const;
	void RefreshSystemSettings() const;
	void RefreshSystemAlerts() const;
	void RefreshMetrics() const;
	void RefreshCodeReviewFiles();
	void LoadLocalCodeReviewFilesFallback(const FString& Query, const FString& Reason);
	void PingBackend() const;
	void RefreshProjectionDataForCurrentResult() const;
	void RefreshTraceDataForCurrentResult() const;
	void ReloadCurrentResultDetail();
	void SubmitCurrentRequest();
	FString GetCurrentRequestValidationError(const FUEAgentFunctionParameters& Parameters, const FString& InputText) const;
	void LoadTaskDetail(const TSharedPtr<FUEAgentTaskSummary>& TaskSummary);
	void SubmitProposalDecision(const TSharedPtr<FUEAgentProposalSummary>& Proposal, const FString& Decision);
	void ConfirmEditorOperationProposal(const TSharedPtr<FUEAgentProposalSummary>& Proposal);
	void RejectEditorOperationProposal(const TSharedPtr<FUEAgentProposalSummary>& Proposal);
	void ExecuteAndReportEditorOperation(const TSharedPtr<FUEAgentProposalSummary>& Proposal);
	void SubmitProjectInventorySnapshot(bool bSilent = false);
	void ApplyQuickAction(const FUEAgentQuickAction& QuickAction);
	void ApplyBaseUrl();
	void SetPreferredOutputLanguage(const FString& InLanguageCode);
	void ActivateSelectedProfile(const TSharedPtr<FUEAgentRuntimeProfile>& Profile);
	void ExportCurrentResponse() const;
	void CopyCurrentDebugSection() const;
	void OpenTraceOrCopyFallback() const;
	void OpenHighlightsWindow();
	void CancelCurrentRun();

	TSharedRef<SWidget> BuildTopShell();
	TSharedRef<SWidget> BuildSettingsPanel();
	TSharedRef<SWidget> BuildUserWorkspace();
	TSharedRef<SWidget> BuildDebugWorkspace();
	TSharedRef<SWidget> BuildFunctionParameterPanel();
	TSharedRef<SWidget> BuildFunctionSpecificForm(EUEAgentFunctionType FunctionType);
	TSharedRef<SWidget> BuildResultHighlightsSummary();
	TSharedRef<SWidget> BuildUserResultCards();
	TSharedRef<SWidget> BuildUserBlockCard(const FUEAgentUserViewBlock& Block) const;
	TSharedRef<SWidget> BuildGeneratedItemsDrafts(const FUEAgentUserViewBlock& Block, const FString& UiLanguage) const;
	TSharedRef<SWidget> BuildContextChips() const;
	TSharedRef<SWidget> BuildChatBubble(const TSharedPtr<FUEAgentChatMessage>& Message) const;
	TSharedRef<SWidget> BuildDebugSectionBody();
	TSharedRef<SWidget> BuildProposalCards();
	TSharedRef<SWidget> BuildTracePanel();
	TSharedRef<SWidget> BuildArtifactsPanel();
	TSharedRef<SWidget> BuildEmptyState(const FString& Message) const;

	TSharedRef<class ITableRow> OnGenerateChatRow(TSharedPtr<FUEAgentChatMessage> Message, const TSharedRef<class STableViewBase>& OwnerTable) const;
	TSharedRef<class ITableRow> OnGenerateTaskRow(TSharedPtr<FUEAgentTaskSummary> TaskSummary, const TSharedRef<class STableViewBase>& OwnerTable) const;
	void OnTaskSelectionChanged(TSharedPtr<FUEAgentTaskSummary> TaskSummary, ESelectInfo::Type SelectInfo);
	void OnFunctionSelectionChanged(TSharedPtr<EUEAgentFunctionType> Selection, ESelectInfo::Type SelectInfo);
	void OnDebugSectionSelectionChanged(TSharedPtr<EUEAgentDebugSection> Selection, ESelectInfo::Type SelectInfo);
	void OnProfileSelectionChanged(TSharedPtr<FUEAgentRuntimeProfile> Selection, ESelectInfo::Type SelectInfo);

	TSharedPtr<EUEAgentFunctionType> FindFunctionOption(EUEAgentFunctionType FunctionType) const;
	TSharedPtr<EUEAgentDebugSection> FindDebugSectionOption(EUEAgentDebugSection Section) const;
	TSharedPtr<FUEAgentRuntimeProfile> FindRuntimeProfileOption(const FString& ProfileId) const;

	FText GetFunctionComboLabel() const;
	FText GetDebugSectionComboLabel() const;
	FText GetProfileComboLabel() const;
	FText GetBackendStatusLabel() const;
	FSlateColor GetBackendStatusColor() const;

private:
	TSharedPtr<FUEAgentStateStore> StateStore;
	TSharedPtr<FUEAgentHttpClient> HttpClient;
	TSharedPtr<FUEAgentContextCollector> ContextCollector;

	TArray<TSharedPtr<EUEAgentFunctionType>> FunctionOptions;
	TArray<TSharedPtr<EUEAgentDebugSection>> DebugSectionOptions;
	TArray<TSharedPtr<FUEAgentRuntimeProfile>> RuntimeProfileOptions;
	TArray<TSharedPtr<FUEAgentChatMessage>> ChatItems;
	TArray<TSharedPtr<FUEAgentTaskSummary>> TaskItems;

	TSharedPtr<class SComboBox<TSharedPtr<EUEAgentFunctionType>>> FunctionComboBox;
	TSharedPtr<class SComboBox<TSharedPtr<EUEAgentDebugSection>>> DebugSectionComboBox;
	TSharedPtr<class SComboBox<TSharedPtr<FUEAgentRuntimeProfile>>> RuntimeProfileComboBox;
	TSharedPtr<class SListView<TSharedPtr<FUEAgentChatMessage>>> ChatListView;
	TSharedPtr<class SListView<TSharedPtr<FUEAgentTaskSummary>>> TaskListView;
	TSharedPtr<class SBox> ParameterPanelBox;
	TSharedPtr<class SBox> ResultCardsBox;
	TSharedPtr<class SBox> SettingsPanelBox;
	TSharedPtr<class SBox> DebugSectionBodyBox;
	TSharedPtr<class SMultiLineEditableTextBox> ChatInputBox;
	TSharedPtr<class SEditableTextBox> BaseUrlTextBox;
	bool bAutoInventorySubmitted = false;
};
