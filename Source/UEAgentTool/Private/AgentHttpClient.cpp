// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentHttpClient.h"

#include "AgentStateStore.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEAgentHttpClientPrivate
{
	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return TEXT("{}");
		}

		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return Output;
	}

	static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, JsonObject) ? JsonObject : nullptr;
	}

	static FString GetErrorFromResponse(const TSharedPtr<FJsonObject>& JsonObject, const FString& DefaultMessage)
	{
		if (!JsonObject.IsValid())
		{
			return DefaultMessage;
		}

		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (!JsonObject->TryGetArrayField(TEXT("errors"), Errors) || Errors == nullptr || Errors->Num() == 0)
		{
			return DefaultMessage;
		}

		TArray<FString> Messages;
		for (const TSharedPtr<FJsonValue>& ErrorValue : *Errors)
		{
			const TSharedPtr<FJsonObject> ErrorObject = ErrorValue.IsValid() ? ErrorValue->AsObject() : nullptr;
			if (!ErrorObject.IsValid())
			{
				continue;
			}

			FString Message;
			if (!ErrorObject->TryGetStringField(TEXT("message"), Message))
			{
				continue;
			}
			Messages.Add(Message);
		}

		return Messages.Num() > 0 ? FString::Join(Messages, TEXT("\n")) : DefaultMessage;
	}

	static FString GetPrimaryUserQuery(EUEAgentFunctionType FunctionType, const FUEAgentFunctionParameters& Parameters, const FString& InputText)
	{
		if (!InputText.TrimStartAndEnd().IsEmpty())
		{
			return InputText.TrimStartAndEnd();
		}

		if (!Parameters.PrimaryText.TrimStartAndEnd().IsEmpty())
		{
			return Parameters.PrimaryText.TrimStartAndEnd();
		}

		switch (FunctionType)
		{
		case EUEAgentFunctionType::CodeReview:
			return TEXT("Review the supplied Unreal code change.");
		case EUEAgentFunctionType::CodeGenerate:
			return TEXT("Generate a draft implementation plan.");
		case EUEAgentFunctionType::LogsAnalyze:
			return TEXT("Analyze the supplied logs.");
		case EUEAgentFunctionType::ConfigGenerate:
			return TEXT("Generate the requested configuration draft.");
		case EUEAgentFunctionType::ConfigValidate:
			return TEXT("Validate the supplied configuration.");
		case EUEAgentFunctionType::AssetsInspect:
			return TEXT("Inspect the selected assets.");
		case EUEAgentFunctionType::AssetsPlan:
			return TEXT("Plan safe asset changes.");
		case EUEAgentFunctionType::PerformanceAnalyze:
			return TEXT("Analyze the supplied performance report.");
		default:
			return TEXT("Help with the current Unreal project.");
		}
	}

	static FString ToChatRole(const EUEAgentChatRole Role)
	{
		switch (Role)
		{
		case EUEAgentChatRole::User:
			return TEXT("user");
		case EUEAgentChatRole::Agent:
			return TEXT("assistant");
		case EUEAgentChatRole::System:
		default:
			return TEXT("system");
		}
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	static TArray<TSharedPtr<FJsonValue>> MakeDefaultSourceRoots()
	{
		TArray<FString> SourceRoots;
		SourceRoots.Add(TEXT("Source"));
		SourceRoots.Add(TEXT("Plugins"));
		return MakeStringArray(SourceRoots);
	}

	static bool IsLikelyLogFilePath(const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}

		const FString Extension = FPaths::GetExtension(Trimmed).ToLower();
		if (Extension == TEXT("log") || Extension == TEXT("txt") || Extension == TEXT("crashcontext")
			|| Extension == TEXT("xml") || Extension == TEXT("json") || Extension == TEXT("ini"))
		{
			return true;
		}

		return Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT("\\"));
	}

	static TArray<FString> ExtractAttachmentPaths(const FString& Text)
	{
		TArray<FString> Candidates;
		Text.ParseIntoArrayLines(Candidates, true);

		TArray<FString> SemicolonCandidates;
		for (const FString& Candidate : Candidates)
		{
			TArray<FString> Parts;
			Candidate.ParseIntoArray(Parts, TEXT(";"), true);
			SemicolonCandidates.Append(Parts);
		}

		TArray<FString> AttachmentPaths;
		for (const FString& Candidate : SemicolonCandidates)
		{
			const FString Trimmed = Candidate.TrimStartAndEnd();
			if (IsLikelyLogFilePath(Trimmed))
			{
				AttachmentPaths.Add(Trimmed);
			}
		}

		return AttachmentPaths;
	}
}

FUEAgentHttpClient::FUEAgentHttpClient(TSharedRef<FUEAgentStateStore> InStateStore)
	: StateStore(InStateStore)
{
}

void FUEAgentHttpClient::RequestHealth(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/system/health"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestBootstrap(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/system/bootstrap"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestCapabilities(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/system/capabilities"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestSystemSettings(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/system/settings"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestKnowledgeBaseStatus(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/knowledge-base/status"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestRuntimeProfiles(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/system/runtime-profiles"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestSystemAlerts(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/system/alerts"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestMetrics(const FTextResponseCallback& Callback) const
{
	SendTextRequest(TEXT("GET"), TEXT("/metrics"), Callback);
}

void FUEAgentHttpClient::CreateSession(const FString& PreferredSessionId, const FJsonResponseCallback& Callback) const
{
	TSharedPtr<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	if (!PreferredSessionId.IsEmpty())
	{
		RequestBody->SetStringField(TEXT("session_id"), PreferredSessionId);
	}
	RequestBody->SetStringField(TEXT("client"), TEXT("ue_editor"));
	RequestBody->SetObjectField(TEXT("metadata"), MakeShared<FJsonObject>());

	if (const TSharedPtr<FUEAgentStateStore> State = StateStore.Pin())
	{
		const FUEAgentContextSummary& Context = State->GetEditorContext();
		if (!Context.ProjectName.IsEmpty())
		{
			RequestBody->SetStringField(TEXT("project_name"), Context.ProjectName);
		}
		RequestBody->SetStringField(TEXT("preferred_output_language"), State->GetPreferredOutputLanguage());
		if (!State->GetActiveProfileId().IsEmpty())
		{
			RequestBody->SetStringField(TEXT("profile_id"), State->GetActiveProfileId());
		}
	}
	SendRequest(TEXT("POST"), TEXT("/api/v1/sessions"), RequestBody, Callback);
}

void FUEAgentHttpClient::RequestSessionSummary(const FString& SessionId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/sessions/%s"), *SessionId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestSessionHistory(const FString& SessionId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/sessions/%s/history"), *SessionId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestSessionTasks(const FString& SessionId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/sessions/%s/tasks"), *SessionId), nullptr, Callback);
}

void FUEAgentHttpClient::ClearSession(const FString& SessionId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("POST"), FString::Printf(TEXT("/api/v1/sessions/%s/clear"), *SessionId), MakeShared<FJsonObject>(), Callback);
}

void FUEAgentHttpClient::ActivateProfile(const FString& ProfileId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("POST"), FString::Printf(TEXT("/api/v1/system/runtime-profiles/%s/activate"), *ProfileId), MakeShared<FJsonObject>(), Callback);
}

void FUEAgentHttpClient::RequestRecentTasks(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/tasks/recent"), nullptr, Callback);
}

void FUEAgentHttpClient::RequestCodeReviewFiles(const FString& ProjectRoot, const FString& Query, const int32 Limit, const FJsonResponseCallback& Callback) const
{
	TSharedPtr<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	RequestBody->SetStringField(TEXT("project_root"), ProjectRoot);
	RequestBody->SetArrayField(TEXT("source_roots"), UEAgentHttpClientPrivate::MakeDefaultSourceRoots());
	RequestBody->SetStringField(TEXT("query"), Query);
	RequestBody->SetNumberField(TEXT("limit"), Limit);
	SendRequest(TEXT("POST"), TEXT("/api/v1/tasks/code-review/files"), RequestBody, Callback);
}

void FUEAgentHttpClient::SubmitProjectInventorySnapshot(const TSharedPtr<FJsonObject>& SnapshotObject, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("POST"), TEXT("/api/v1/project-inventory/snapshot"), SnapshotObject, Callback);
}

void FUEAgentHttpClient::RequestTaskDetail(const FString& TaskId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/tasks/%s"), *TaskId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestTaskUserView(const FString& TaskId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/tasks/%s/user-view"), *TaskId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestTaskDebugView(const FString& TaskId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/tasks/%s/debug-view"), *TaskId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestTaskTrace(const FString& TaskId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/tasks/%s/trace"), *TaskId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestTaskArtifacts(const FString& TaskId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/tasks/%s/artifacts"), *TaskId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestRunDetail(const FString& RunId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/chat/runs/%s"), *RunId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestRunUserView(const FString& RunId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/chat/runs/%s/user-view"), *RunId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestRunDebugView(const FString& RunId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/chat/runs/%s/debug-view"), *RunId), nullptr, Callback);
}

void FUEAgentHttpClient::RequestRunEvents(const FString& RunId, const FTextResponseCallback& Callback) const
{
	SendTextRequest(TEXT("GET"), FString::Printf(TEXT("/api/v1/chat/runs/%s/events/stream"), *RunId), Callback);
}

void FUEAgentHttpClient::RequestPendingProposals(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/proposals/pending"), nullptr, Callback);
}

void FUEAgentHttpClient::SubmitProposalDecision(const FString& ProposalId, const FString& Decision, const FString& Comment, const FJsonResponseCallback& Callback) const
{
	TSharedPtr<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	RequestBody->SetStringField(TEXT("decision"), Decision);
	RequestBody->SetStringField(TEXT("actor"), TEXT("ue_plugin"));
	RequestBody->SetStringField(TEXT("comment"), Comment);
	RequestBody->SetObjectField(TEXT("metadata"), MakeShared<FJsonObject>());

	SendRequest(TEXT("POST"), FString::Printf(TEXT("/api/v1/proposals/%s/decision"), *ProposalId), RequestBody, Callback);
}

void FUEAgentHttpClient::RequestEditorOperationCapabilities(const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("GET"), TEXT("/api/v1/editor-operations/capabilities"), nullptr, Callback);
}

void FUEAgentHttpClient::ConfirmEditorOperationProposal(const FString& ProposalId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("POST"), FString::Printf(TEXT("/api/v1/editor-operations/proposals/%s/confirm"), *ProposalId), MakeShared<FJsonObject>(), Callback);
}

void FUEAgentHttpClient::RejectEditorOperationProposal(const FString& ProposalId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("POST"), FString::Printf(TEXT("/api/v1/editor-operations/proposals/%s/reject"), *ProposalId), MakeShared<FJsonObject>(), Callback);
}

void FUEAgentHttpClient::SubmitEditorOperationResult(
	const FString& ProposalId,
	const FString& OperationType,
	const FString& ExecutionState,
	const bool bSuccess,
	const FString& TransactionId,
	const FString& UndoHint,
	const TSharedPtr<FJsonObject>& ResultObject,
	const TArray<TSharedPtr<FJsonValue>>& ErrorValues,
	const TSharedPtr<FJsonObject>& MetadataObject,
	const FJsonResponseCallback& Callback) const
{
	TSharedPtr<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	RequestBody->SetStringField(TEXT("proposal_id"), ProposalId);
	RequestBody->SetStringField(TEXT("operation_type"), OperationType);
	RequestBody->SetStringField(TEXT("execution_state"), ExecutionState);
	RequestBody->SetBoolField(TEXT("success"), bSuccess);
	RequestBody->SetStringField(TEXT("executed_by"), TEXT("ue_plugin"));
	RequestBody->SetStringField(TEXT("transaction_id"), TransactionId);
	RequestBody->SetStringField(TEXT("undo_hint"), UndoHint);
	RequestBody->SetObjectField(TEXT("result"), ResultObject.IsValid() ? ResultObject : MakeShared<FJsonObject>());
	RequestBody->SetArrayField(TEXT("errors"), ErrorValues);
	RequestBody->SetObjectField(TEXT("metadata"), MetadataObject.IsValid() ? MetadataObject : MakeShared<FJsonObject>());

	SendRequest(TEXT("POST"), TEXT("/api/v1/editor-operations/results"), RequestBody, Callback);
}

void FUEAgentHttpClient::CancelRun(const FString& RunId, const FJsonResponseCallback& Callback) const
{
	SendRequest(TEXT("POST"), FString::Printf(TEXT("/api/v1/chat/runs/%s/cancel"), *RunId), MakeShared<FJsonObject>(), Callback);
}

void FUEAgentHttpClient::SubmitFunction(
	EUEAgentFunctionType FunctionType,
	const FUEAgentContextSummary& Context,
	const FUEAgentFunctionParameters& Parameters,
	const FString& InputText,
	EUEAgentViewMode ActiveView,
	const FJsonResponseCallback& Callback) const
{
	TSharedPtr<FJsonObject> BodyObject = BuildRequestPayload(FunctionType, Context, Parameters, InputText, ActiveView);
	const FString Endpoint = UEAgent::UsesUnifiedChat(FunctionType) ? TEXT("/api/v1/chat/runs") : UEAgent::ToTaskEndpoint(FunctionType);
	SendRequest(TEXT("POST"), Endpoint, BodyObject, Callback);
}

TSharedPtr<FJsonObject> FUEAgentHttpClient::BuildRequestPayload(
	const EUEAgentFunctionType FunctionType,
	const FUEAgentContextSummary& Context,
	const FUEAgentFunctionParameters& Parameters,
	const FString& InputText,
	const EUEAgentViewMode ActiveView) const
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("task_type"), UEAgent::ToFunctionId(FunctionType));

	TSharedPtr<FJsonObject> SessionObject = MakeShared<FJsonObject>();
	FString PreferredOutputLanguage = TEXT("zh-CN");
	if (const TSharedPtr<FUEAgentStateStore> State = StateStore.Pin())
	{
		SessionObject->SetStringField(TEXT("session_id"), State->GetSessionId());
		PreferredOutputLanguage = State->GetPreferredOutputLanguage();
		TArray<TSharedPtr<FJsonValue>> MessageValues;
		if (UEAgent::UsesUnifiedChat(FunctionType))
		{
			for (const TSharedPtr<FUEAgentChatMessage>& Message : State->GetChatMessages())
			{
				if (!Message.IsValid() || Message->Text.IsEmpty())
				{
					continue;
				}

				if (Message->FunctionId != UEAgent::ToFunctionId(EUEAgentFunctionType::AgentChat)
					&& Message->FunctionId != UEAgent::ToFunctionId(EUEAgentFunctionType::ProjectQA))
				{
					continue;
				}

				TSharedPtr<FJsonObject> MessageObject = MakeShared<FJsonObject>();
				MessageObject->SetStringField(TEXT("role"), UEAgentHttpClientPrivate::ToChatRole(Message->Role));
				MessageObject->SetStringField(TEXT("content"), Message->Text);
				MessageObject->SetStringField(TEXT("language"), TEXT("auto"));
				MessageValues.Add(MakeShared<FJsonValueObject>(MessageObject));
			}
		}
		SessionObject->SetArrayField(TEXT("messages"), MessageValues);
	}
	RootObject->SetObjectField(TEXT("session"), SessionObject);

	TSharedPtr<FJsonObject> ContextObject = MakeShared<FJsonObject>();
	ContextObject->SetStringField(TEXT("project_root"), Context.ProjectRoot);
	ContextObject->SetStringField(TEXT("project_name"), Context.ProjectName);
	ContextObject->SetStringField(TEXT("active_panel"), UEAgent::ToPanelId(FunctionType));
	ContextObject->SetStringField(TEXT("current_file"), Context.CurrentFile);
	ContextObject->SetStringField(TEXT("current_module"), Context.ActiveModule);
	ContextObject->SetStringField(TEXT("session_id"), Context.SessionId);
	ContextObject->SetStringField(TEXT("selected_panel"), UEAgent::ToPanelId(FunctionType));
	ContextObject->SetStringField(TEXT("user_timezone"), TEXT("Asia/Shanghai"));

	TArray<TSharedPtr<FJsonValue>> SelectedAssetValues;
	for (const FString& SelectedAsset : Context.SelectedAssets)
	{
		SelectedAssetValues.Add(MakeShared<FJsonValueString>(SelectedAsset));
	}
	ContextObject->SetArrayField(TEXT("selected_assets"), SelectedAssetValues);

	auto BuildAssetItemValues = [&Context]()
	{
		TArray<TSharedPtr<FJsonValue>> AssetItemValues;
		for (const FUEAgentAssetContextItem& AssetItem : Context.SelectedAssetItems)
		{
			TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
			AssetObject->SetStringField(TEXT("asset_name"), AssetItem.AssetName);
			AssetObject->SetStringField(TEXT("asset_path"), AssetItem.AssetPath);
			AssetObject->SetStringField(TEXT("asset_type"), AssetItem.AssetType);
			AssetObject->SetStringField(TEXT("package_path"), AssetItem.PackagePath);
			AssetObject->SetArrayField(TEXT("dependencies"), UEAgentHttpClientPrivate::MakeStringArray(AssetItem.Dependencies));
			AssetObject->SetArrayField(TEXT("referencers"), UEAgentHttpClientPrivate::MakeStringArray(AssetItem.Referencers));
			AssetItemValues.Add(MakeShared<FJsonValueObject>(AssetObject));
		}
		return AssetItemValues;
	};

	ContextObject->SetArrayField(TEXT("selected_asset_items"), BuildAssetItemValues());

	TArray<TSharedPtr<FJsonValue>> RecentOpenFileValues;
	for (const FString& OpenFile : Context.RecentOpenFiles)
	{
		RecentOpenFileValues.Add(MakeShared<FJsonValueString>(OpenFile));
	}
	ContextObject->SetArrayField(TEXT("recent_open_files"), RecentOpenFileValues);
	TSharedPtr<FJsonObject> EditorStateObject = MakeShared<FJsonObject>();
	const FString EditorCultureName = FInternationalization::Get().GetCurrentCulture()->GetName();
	const FString EditorLanguageName = FInternationalization::Get().GetCurrentLanguage()->GetName();
	if (!EditorCultureName.IsEmpty())
	{
		EditorStateObject->SetStringField(TEXT("locale"), EditorCultureName);
		EditorStateObject->SetStringField(TEXT("culture"), EditorCultureName);
		EditorStateObject->SetStringField(TEXT("editor_locale"), EditorCultureName);
	}
	if (!EditorLanguageName.IsEmpty())
	{
		EditorStateObject->SetStringField(TEXT("language"), EditorLanguageName);
	}
	ContextObject->SetObjectField(TEXT("editor_state"), EditorStateObject);
	ContextObject->SetArrayField(TEXT("kb_domains_hint"), TArray<TSharedPtr<FJsonValue>>());
	RootObject->SetObjectField(TEXT("context"), ContextObject);

	TSharedPtr<FJsonObject> PayloadObject = MakeShared<FJsonObject>();
	const FString UserQuery = UEAgentHttpClientPrivate::GetPrimaryUserQuery(FunctionType, Parameters, InputText);
	PayloadObject->SetStringField(TEXT("user_query"), UserQuery);

	switch (FunctionType)
	{
	case EUEAgentFunctionType::ProjectQA:
		PayloadObject->SetArrayField(TEXT("domain_filters"), TArray<TSharedPtr<FJsonValue>>());
		break;
	case EUEAgentFunctionType::CodeReview:
	{
		FString FilePath = Parameters.FilePath.TrimStartAndEnd();
		if (FilePath.IsEmpty())
		{
			FilePath = Context.CurrentFile;
		}
		if (FilePath.IsEmpty())
		{
			FilePath = Parameters.PrimaryText.TrimStartAndEnd();
		}
		PayloadObject->SetStringField(TEXT("project_root"), Context.ProjectRoot);
		PayloadObject->SetArrayField(TEXT("source_roots"), UEAgentHttpClientPrivate::MakeDefaultSourceRoots());
		PayloadObject->SetStringField(TEXT("file_path"), FilePath);
		PayloadObject->SetStringField(TEXT("review_mode"), Parameters.ReviewMode);
		PayloadObject->SetStringField(TEXT("focus"), Parameters.FocusArea);
		break;
	}
	case EUEAgentFunctionType::CodeGenerate:
		PayloadObject->SetStringField(TEXT("requirement_description"), Parameters.PrimaryText);
		PayloadObject->SetStringField(TEXT("target_type"), Parameters.TargetType);
		if (Parameters.bCreateWriteProposal)
		{
			PayloadObject->SetBoolField(TEXT("create_write_proposal"), true);
			PayloadObject->SetStringField(TEXT("write_mode"), TEXT("proposal"));
		}
		break;
	case EUEAgentFunctionType::LogsAnalyze:
	{
		const FString LogText = Parameters.PrimaryText.TrimStartAndEnd();
		const FString LogSource = Parameters.LogSource.TrimStartAndEnd();
		const FString Notes = Parameters.SecondaryText.TrimStartAndEnd();
		const bool bHasLogFilePath = UEAgentHttpClientPrivate::IsLikelyLogFilePath(LogSource);
		const TArray<FString> AttachmentPaths = UEAgentHttpClientPrivate::ExtractAttachmentPaths(Notes);

		PayloadObject->SetStringField(TEXT("log_text"), LogText);
		PayloadObject->SetStringField(TEXT("selected_log_text"), LogText);
		PayloadObject->SetStringField(TEXT("log_excerpt"), LogText);
		PayloadObject->SetStringField(TEXT("log_source"), LogSource);
		if (bHasLogFilePath)
		{
			PayloadObject->SetStringField(TEXT("log_file_path"), LogSource);
			PayloadObject->SetStringField(TEXT("log_path"), LogSource);
		}
		PayloadObject->SetStringField(TEXT("notes"), Notes);
		PayloadObject->SetStringField(TEXT("user_notes"), Notes);
		if (AttachmentPaths.Num() > 0)
		{
			PayloadObject->SetArrayField(TEXT("attachment_paths"), UEAgentHttpClientPrivate::MakeStringArray(AttachmentPaths));
		}
		PayloadObject->SetBoolField(TEXT("include_file_context"), !LogText.IsEmpty() && bHasLogFilePath);
		break;
	}
	case EUEAgentFunctionType::ConfigGenerate:
		PayloadObject->SetStringField(TEXT("requirement_description"), Parameters.PrimaryText);
		PayloadObject->SetStringField(TEXT("object_type"), Parameters.ObjectType);
		PayloadObject->SetStringField(TEXT("schema"), Parameters.SchemaText);
		PayloadObject->SetStringField(TEXT("export_format"), Parameters.ExportFormat);
		break;
	case EUEAgentFunctionType::ConfigValidate:
		PayloadObject->SetStringField(TEXT("schema"), Parameters.SchemaText);
		PayloadObject->SetStringField(TEXT("config_json"), Parameters.JsonText);
		break;
	case EUEAgentFunctionType::AssetsInspect:
		PayloadObject->SetStringField(TEXT("inspection_note"), Parameters.PrimaryText);
		PayloadObject->SetArrayField(TEXT("asset_items"), BuildAssetItemValues());
		break;
	case EUEAgentFunctionType::AssetsPlan:
		PayloadObject->SetStringField(TEXT("planning_note"), Parameters.PrimaryText);
		break;
	case EUEAgentFunctionType::PerformanceAnalyze:
		PayloadObject->SetStringField(TEXT("report_text"), Parameters.PrimaryText);
		PayloadObject->SetStringField(TEXT("insights_summary"), Parameters.SecondaryText);
		break;
	default:
		break;
	}
	RootObject->SetObjectField(TEXT("payload"), PayloadObject);

	TSharedPtr<FJsonObject> UiStateObject = MakeShared<FJsonObject>();
	UiStateObject->SetStringField(TEXT("active_view"), ActiveView == EUEAgentViewMode::User ? TEXT("user") : TEXT("debug"));
	UiStateObject->SetStringField(TEXT("selected_panel"), UEAgent::ToPanelId(FunctionType));
	RootObject->SetObjectField(TEXT("ui_state"), UiStateObject);

	TSharedPtr<FJsonObject> RuntimeOptionsObject = MakeShared<FJsonObject>();
	if (const TSharedPtr<FUEAgentStateStore> State = StateStore.Pin())
	{
		RuntimeOptionsObject->SetStringField(TEXT("profile_id"), State->GetActiveProfileId());
	}
	RuntimeOptionsObject->SetBoolField(TEXT("stream"), false);
	RuntimeOptionsObject->SetBoolField(TEXT("debug"), true);
	RuntimeOptionsObject->SetStringField(TEXT("preferred_output_language"), PreferredOutputLanguage);
	RuntimeOptionsObject->SetBoolField(TEXT("return_debug_projection"), true);
	RootObject->SetObjectField(TEXT("runtime_options"), RuntimeOptionsObject);

	return RootObject;
}

void FUEAgentHttpClient::SendRequest(const FString& Verb, const FString& RelativePath, const TSharedPtr<FJsonObject>& BodyObject, const FJsonResponseCallback& Callback) const
{
	const TSharedPtr<FUEAgentStateStore> State = StateStore.Pin();
	if (!State.IsValid())
	{
		Callback(false, TEXT("State store is unavailable."), TEXT(""), nullptr);
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BuildUrl(RelativePath));
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	FString RequestBody;
	if (BodyObject.IsValid())
	{
		RequestBody = UEAgentHttpClientPrivate::SerializeJsonObject(BodyObject);
		Request->SetContentAsString(RequestBody);
		State->SetLastRequestJson(RequestBody);
	}

	Request->OnProcessRequestComplete().BindLambda(
		[Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, const bool bSucceeded)
		{
			const bool bHasResponse = HttpResponse.IsValid();
			const FString ResponseText = bHasResponse ? HttpResponse->GetContentAsString() : TEXT("");
			const TSharedPtr<FJsonObject> JsonObject = ResponseText.IsEmpty() ? nullptr : UEAgentHttpClientPrivate::ParseJsonObject(ResponseText);
			const bool bHttpSuccess = bSucceeded && bHasResponse && EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode());

			if (!bHttpSuccess)
			{
				const FString FailureMessage = bHasResponse
					? FString::Printf(TEXT("HTTP %d\n%s"), HttpResponse->GetResponseCode(), *UEAgentHttpClientPrivate::GetErrorFromResponse(JsonObject, ResponseText))
					: TEXT("The backend request failed. Check the base URL and backend status.");
				Callback(false, FailureMessage, ResponseText, JsonObject);
				return;
			}

			const bool bSuccessFlag = !JsonObject.IsValid() || !JsonObject->HasField(TEXT("success")) || JsonObject->GetBoolField(TEXT("success"));
			if (!bSuccessFlag)
			{
				Callback(false, UEAgentHttpClientPrivate::GetErrorFromResponse(JsonObject, TEXT("The backend returned a failure state.")), ResponseText, JsonObject);
				return;
			}

			Callback(true, TEXT("OK"), ResponseText, JsonObject);
		});

	Request->ProcessRequest();
}

void FUEAgentHttpClient::SendTextRequest(const FString& Verb, const FString& RelativePath, const FTextResponseCallback& Callback) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BuildUrl(RelativePath));
	Request->SetVerb(Verb);

	Request->OnProcessRequestComplete().BindLambda(
		[Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, const bool bSucceeded)
		{
			const bool bHasResponse = HttpResponse.IsValid();
			const FString ResponseText = bHasResponse ? HttpResponse->GetContentAsString() : TEXT("");
			const bool bHttpSuccess = bSucceeded && bHasResponse && EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode());
			if (!bHttpSuccess)
			{
				const FString FailureMessage = bHasResponse
					? FString::Printf(TEXT("HTTP %d\n%s"), HttpResponse->GetResponseCode(), *ResponseText)
					: TEXT("The backend text request failed. Check the base URL and run id.");
				Callback(false, FailureMessage, ResponseText);
				return;
			}

			Callback(true, TEXT("OK"), ResponseText);
		});

	Request->ProcessRequest();
}

FString FUEAgentHttpClient::BuildUrl(const FString& RelativePath) const
{
	const TSharedPtr<FUEAgentStateStore> State = StateStore.Pin();
	FString BaseUrl = State.IsValid() ? State->GetBackendBaseUrl() : TEXT("http://127.0.0.1:8000");
	BaseUrl.TrimStartAndEndInline();
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1, false);
	}
	return FString::Printf(TEXT("%s%s"), *BaseUrl, *RelativePath);
}
