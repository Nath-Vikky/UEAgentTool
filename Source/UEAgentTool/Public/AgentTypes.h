// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class EUEAgentViewMode : uint8
{
	User,
	Debug
};

enum class EUEAgentFunctionType : uint8
{
	AgentChat,
	ProjectQA,
	CodeReview,
	CodeGenerate,
	LogsAnalyze,
	ConfigGenerate,
	ConfigValidate,
	AssetsInspect,
	AssetsPlan,
	PerformanceAnalyze
};

enum class EUEAgentDebugSection : uint8
{
	Overview,
	RawRequest,
	RawResponse,
	UserProjection,
	DebugProjection,
	IntentRoute,
	Skill,
	Retrieval,
	Tools,
	StepResults,
	Proposal,
	Trace,
	Monitor,
	Artifacts
};

enum class EUEAgentChatRole : uint8
{
	User,
	Agent,
	System
};

struct FUEAgentFunctionParameters
{
	FString PrimaryText;
	FString SecondaryText;
	FString SchemaText;
	FString JsonText;
	FString FileSearchQuery;
	FString FilePath;
	FString LogSource;
	FString ObjectType = TEXT("DataAsset");
	FString ExportFormat = TEXT("json");
	FString TargetType = TEXT("ue_cpp");
	FString ReviewMode = TEXT("Current File");
	FString FocusArea = TEXT("General");
	bool bIncludeContext = true;
	bool bProjectKnowledgeOnly = false;
	bool bCreateWriteProposal = false;
};

struct FUEAgentChatMessage
{
	FString MessageId;
	FString SessionId;
	FString TaskId;
	FString FunctionId;
	EUEAgentChatRole Role = EUEAgentChatRole::System;
	FString Title;
	FString Text;
	FString StatusHint;
	FDateTime Timestamp = FDateTime::UtcNow();
	bool bStreaming = false;
	bool bIncomplete = false;
};

struct FUEAgentCitation
{
	FString Title;
	FString Source;
	FString Snippet;
};

struct FUEAgentUserViewBlock
{
	FString BlockType;
	FString Title;
	FString Text;
	TArray<FString> Items;
	FString JsonPreview;
};

struct FUEAgentQuickAction
{
	FString ActionId;
	FString Label;
	FString SuggestedInput;
	FString PayloadJson;
};

struct FUEAgentTraceLink
{
	FString Label;
	FString Url;
	FString RawJson;
};

struct FUEAgentArtifactItem
{
	FString ArtifactId;
	FString ArtifactType;
	FString Label;
	FString Path;
	FString MetadataJson;
};

struct FUEAgentRunEvent
{
	int32 Seq = INDEX_NONE;
	FString EventType;
	FString Timestamp;
	FString Summary;
	FString RawJson;
};

struct FUEAgentTaskSummary
{
	FString TaskId;
	FString RunId;
	FString TaskType;
	FString Status;
	FString FinishReason;
	FString Title;
	FDateTime Timestamp = FDateTime::UtcNow();
};

struct FUEAgentProposalSummary
{
	FString ProposalId;
	FString Title;
	FString ProposalType;
	FString BeforeSummary;
	FString AfterSummary;
	FString Rationale;
	FString RiskFlags;
	FString ConfirmationState;
	FString DecisionEndpoint;
	FString WritePlanSummary;
	FString OperationType;
	FString OperationPayloadJson;
	FString RawJson;
};

struct FUEAgentRuntimeProfile
{
	FString ProfileId;
	FString Label;
	FString Description;
	bool bIsActive = false;
	bool bIsDefault = false;
};

struct FUEAgentCodeFileItem
{
	FString FilePath;
	FString RelativePath;
	FString ModuleName;
	FString FileType;
	FString Label;
};

struct FUEAgentAssetContextItem
{
	FString AssetName;
	FString AssetPath;
	FString AssetType;
	FString PackagePath;
	TArray<FString> Dependencies;
	TArray<FString> Referencers;
};

struct FUEAgentContextSummary
{
	FString ProjectRoot;
	FString ProjectName;
	FString ActiveModule;
	FString CurrentFile;
	TArray<FString> SelectedAssets;
	TArray<FUEAgentAssetContextItem> SelectedAssetItems;
	TArray<FString> RecentOpenFiles;
	FString KnowledgeBaseStatus;
	FString BackendVersion;
	FString SessionId;
};

struct FUEAgentResultSnapshot
{
	FString TaskId;
	FString RunId;
	FString TaskStatus;
	FString FinishReason;
	FString UserTitle;
	FString UserText;
	FString AssistantMessage;
	FString StatusHint;
	FString IntentType;
	FString RouteType;
	bool bOutputComplete = true;
	TArray<FUEAgentUserViewBlock> Blocks;
	TArray<FUEAgentCitation> Citations;
	TArray<FUEAgentQuickAction> QuickActions;
	TArray<TSharedPtr<FUEAgentProposalSummary>> Proposals;
	TArray<FUEAgentTraceLink> TraceLinks;
	TArray<FUEAgentArtifactItem> Artifacts;
	TArray<FUEAgentRunEvent> Events;
	FString OverviewText;
	FString RawRequestJson;
	FString RawResponseJson;
	FString UserProjectionJson;
	FString DebugProjectionJson;
	FString IntentRouteJson;
	FString SkillJson;
	FString RetrievalJson;
	FString ToolsJson;
	FString StepResultsJson;
	FString ProposalJson;
	FString TraceJson;
	FString MonitorJson;
	FString ArtifactsJson;
	FString EventsJson;
	FString ApprovalResultJson;
	FString UsageJson;
	FString LocaleJson;
	FString MetricsJson;
	FString SessionSummaryJson;
	FString MemorySummaryJson;
	TArray<FString> Warnings;
	TArray<FString> Errors;
};

namespace UEAgent
{
	inline FString NormalizeOutputLanguageCode(const FString& LanguageCode)
	{
		FString Normalized = LanguageCode;
		Normalized.TrimStartAndEndInline();

		if (Normalized.StartsWith(TEXT("en"), ESearchCase::IgnoreCase))
		{
			return TEXT("en-US");
		}
		if (Normalized.StartsWith(TEXT("zh"), ESearchCase::IgnoreCase))
		{
			return TEXT("zh-CN");
		}
		if (Normalized.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			return TEXT("auto");
		}
		return TEXT("zh-CN");
	}

	inline bool IsEnglishOutputLanguage(const FString& LanguageCode)
	{
		return NormalizeOutputLanguageCode(LanguageCode).StartsWith(TEXT("en"), ESearchCase::IgnoreCase);
	}

	inline FString ToOutputLanguageLabel(const FString& LanguageCode)
	{
		return IsEnglishOutputLanguage(LanguageCode) ? TEXT("English") : TEXT("中文");
	}

	inline FString LocalizeStableUiText(const FString& LanguageCode, const TCHAR* ZhText, const TCHAR* EnText)
	{
		return IsEnglishOutputLanguage(LanguageCode) ? FString(EnText) : FString(ZhText);
	}

	inline TArray<EUEAgentFunctionType> GetOrderedFunctions()
	{
		return {
			EUEAgentFunctionType::AgentChat,
			EUEAgentFunctionType::CodeReview,
			EUEAgentFunctionType::CodeGenerate,
			EUEAgentFunctionType::LogsAnalyze,
			EUEAgentFunctionType::AssetsInspect
		};
	}

	inline bool IsCoreFunction(const EUEAgentFunctionType FunctionType)
	{
		return FunctionType == EUEAgentFunctionType::AgentChat
			|| FunctionType == EUEAgentFunctionType::CodeReview
			|| FunctionType == EUEAgentFunctionType::CodeGenerate
			|| FunctionType == EUEAgentFunctionType::LogsAnalyze
			|| FunctionType == EUEAgentFunctionType::AssetsInspect;
	}

	inline TArray<EUEAgentDebugSection> GetOrderedDebugSections()
	{
		return {
			EUEAgentDebugSection::Overview,
			EUEAgentDebugSection::RawRequest,
			EUEAgentDebugSection::RawResponse,
			EUEAgentDebugSection::UserProjection,
			EUEAgentDebugSection::DebugProjection,
			EUEAgentDebugSection::IntentRoute,
			EUEAgentDebugSection::Skill,
			EUEAgentDebugSection::Retrieval,
			EUEAgentDebugSection::Tools,
			EUEAgentDebugSection::StepResults,
			EUEAgentDebugSection::Proposal,
			EUEAgentDebugSection::Trace,
			EUEAgentDebugSection::Monitor,
			EUEAgentDebugSection::Artifacts
		};
	}

	inline FString ToFunctionId(const EUEAgentFunctionType FunctionType)
	{
		switch (FunctionType)
		{
		case EUEAgentFunctionType::AgentChat:
			return TEXT("agent_chat");
		case EUEAgentFunctionType::ProjectQA:
			return TEXT("project_qa");
		case EUEAgentFunctionType::CodeReview:
			return TEXT("code_review");
		case EUEAgentFunctionType::CodeGenerate:
			return TEXT("code_generate");
		case EUEAgentFunctionType::LogsAnalyze:
			return TEXT("logs_analyze");
		case EUEAgentFunctionType::ConfigGenerate:
			return TEXT("config_generate");
		case EUEAgentFunctionType::ConfigValidate:
			return TEXT("config_validate");
		case EUEAgentFunctionType::AssetsInspect:
			return TEXT("assets_inspect");
		case EUEAgentFunctionType::AssetsPlan:
			return TEXT("assets_plan");
		case EUEAgentFunctionType::PerformanceAnalyze:
			return TEXT("perf_analyze");
		default:
			return TEXT("agent_chat");
		}
	}

	inline FString ToPanelId(const EUEAgentFunctionType FunctionType)
	{
		switch (FunctionType)
		{
		case EUEAgentFunctionType::AgentChat:
			return TEXT("AgentChat");
		case EUEAgentFunctionType::ProjectQA:
			return TEXT("ProjectQA");
		case EUEAgentFunctionType::CodeReview:
			return TEXT("CodeReview");
		case EUEAgentFunctionType::CodeGenerate:
			return TEXT("CodeGenerator");
		case EUEAgentFunctionType::LogsAnalyze:
			return TEXT("LogAnalyzer");
		case EUEAgentFunctionType::ConfigGenerate:
			return TEXT("ConfigGenerator");
		case EUEAgentFunctionType::ConfigValidate:
			return TEXT("ConfigValidator");
		case EUEAgentFunctionType::AssetsInspect:
			return TEXT("AssetInspector");
		case EUEAgentFunctionType::AssetsPlan:
			return TEXT("AssetPlanner");
		case EUEAgentFunctionType::PerformanceAnalyze:
			return TEXT("PerfAnalysis");
		default:
			return TEXT("AgentChat");
		}
	}

	inline bool UsesUnifiedChat(const EUEAgentFunctionType FunctionType)
	{
		return FunctionType == EUEAgentFunctionType::AgentChat || FunctionType == EUEAgentFunctionType::ProjectQA;
	}

	inline FString ToTaskEndpoint(const EUEAgentFunctionType FunctionType)
	{
		switch (FunctionType)
		{
		case EUEAgentFunctionType::ProjectQA:
			return TEXT("/api/v1/tasks/project-qa");
		case EUEAgentFunctionType::CodeReview:
			return TEXT("/api/v1/tasks/code-review");
		case EUEAgentFunctionType::CodeGenerate:
			return TEXT("/api/v1/tasks/code-generate");
		case EUEAgentFunctionType::LogsAnalyze:
			return TEXT("/api/v1/tasks/logs-analyze");
		case EUEAgentFunctionType::ConfigGenerate:
			return TEXT("/api/v1/tasks/config-generate");
		case EUEAgentFunctionType::ConfigValidate:
			return TEXT("/api/v1/tasks/config-validate");
		case EUEAgentFunctionType::AssetsInspect:
			return TEXT("/api/v1/tasks/assets-inspect");
		case EUEAgentFunctionType::PerformanceAnalyze:
			return TEXT("/api/v1/tasks/perf-analyze");
		default:
			return TEXT("/api/v1/chat/runs");
		}
	}

	inline FText ToFunctionLabel(const EUEAgentFunctionType FunctionType)
	{
		switch (FunctionType)
		{
		case EUEAgentFunctionType::AgentChat:
			return FText::FromString(TEXT("Agent Chat / Project QA"));
		case EUEAgentFunctionType::ProjectQA:
			return FText::FromString(TEXT("Project QA"));
		case EUEAgentFunctionType::CodeReview:
			return FText::FromString(TEXT("Code Review"));
		case EUEAgentFunctionType::CodeGenerate:
			return FText::FromString(TEXT("Code Generate"));
		case EUEAgentFunctionType::LogsAnalyze:
			return FText::FromString(TEXT("Logs Analyze"));
		case EUEAgentFunctionType::ConfigGenerate:
			return FText::FromString(TEXT("Config Generate"));
		case EUEAgentFunctionType::ConfigValidate:
			return FText::FromString(TEXT("Config Validate"));
		case EUEAgentFunctionType::AssetsInspect:
			return FText::FromString(TEXT("Assets Inspect"));
		case EUEAgentFunctionType::AssetsPlan:
			return FText::FromString(TEXT("Assets Plan"));
		case EUEAgentFunctionType::PerformanceAnalyze:
			return FText::FromString(TEXT("Performance Analyze"));
		default:
			return FText::FromString(TEXT("Agent Chat"));
		}
	}

	inline FText ToDebugSectionLabel(const EUEAgentDebugSection Section)
	{
		switch (Section)
		{
		case EUEAgentDebugSection::Overview:
			return FText::FromString(TEXT("Overview"));
		case EUEAgentDebugSection::RawRequest:
			return FText::FromString(TEXT("Raw Request"));
		case EUEAgentDebugSection::RawResponse:
			return FText::FromString(TEXT("Raw Response"));
		case EUEAgentDebugSection::UserProjection:
			return FText::FromString(TEXT("User Projection"));
		case EUEAgentDebugSection::DebugProjection:
			return FText::FromString(TEXT("Debug Projection"));
		case EUEAgentDebugSection::IntentRoute:
			return FText::FromString(TEXT("Intent / Route"));
		case EUEAgentDebugSection::Skill:
			return FText::FromString(TEXT("Skill"));
		case EUEAgentDebugSection::Retrieval:
			return FText::FromString(TEXT("Retrieval"));
		case EUEAgentDebugSection::Tools:
			return FText::FromString(TEXT("Tools"));
		case EUEAgentDebugSection::StepResults:
			return FText::FromString(TEXT("Step Results"));
		case EUEAgentDebugSection::Proposal:
			return FText::FromString(TEXT("Proposal"));
		case EUEAgentDebugSection::Trace:
			return FText::FromString(TEXT("Trace"));
		case EUEAgentDebugSection::Monitor:
			return FText::FromString(TEXT("Monitor"));
		case EUEAgentDebugSection::Artifacts:
			return FText::FromString(TEXT("Artifacts"));
		default:
			return FText::FromString(TEXT("Overview"));
		}
	}

	inline FText ToSubmitLabel(const EUEAgentFunctionType FunctionType)
	{
		switch (FunctionType)
		{
		case EUEAgentFunctionType::CodeReview:
			return FText::FromString(TEXT("Analyze Selected File"));
		case EUEAgentFunctionType::CodeGenerate:
			return FText::FromString(TEXT("Generate"));
		case EUEAgentFunctionType::LogsAnalyze:
			return FText::FromString(TEXT("Analyze Log"));
		case EUEAgentFunctionType::ConfigGenerate:
			return FText::FromString(TEXT("Generate Config"));
		case EUEAgentFunctionType::ConfigValidate:
			return FText::FromString(TEXT("Validate Config"));
		case EUEAgentFunctionType::AssetsInspect:
			return FText::FromString(TEXT("Inspect Selected Assets"));
		case EUEAgentFunctionType::AssetsPlan:
			return FText::FromString(TEXT("Plan Changes"));
		case EUEAgentFunctionType::PerformanceAnalyze:
			return FText::FromString(TEXT("Analyze Performance"));
		default:
			return FText::FromString(TEXT("Send"));
		}
	}
}
