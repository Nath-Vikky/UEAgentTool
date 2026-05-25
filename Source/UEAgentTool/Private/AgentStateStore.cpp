// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentStateStore.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEAgentStateStorePrivate
{
	static const TCHAR* ConfigSection = TEXT("UEAgentTool");

	static FString JsonToPrettyString(const TSharedPtr<FJsonObject>& JsonObject)
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
		if (Text.TrimStartAndEnd().IsEmpty() || Text == TEXT("{}"))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, JsonObject) ? JsonObject : nullptr;
	}

	static FString JsonFieldToPrettyString(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
	{
		if (!JsonObject.IsValid() || !JsonObject->HasField(FieldName))
		{
			return TEXT("{}");
		}

		const TSharedPtr<FJsonValue> JsonValue = JsonObject->TryGetField(FieldName);
		if (!JsonValue.IsValid())
		{
			return TEXT("{}");
		}

		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
		switch (JsonValue->Type)
		{
		case EJson::Object:
			FJsonSerializer::Serialize(JsonValue->AsObject().ToSharedRef(), Writer);
			break;
		case EJson::Array:
			FJsonSerializer::Serialize(JsonValue->AsArray(), Writer);
			break;
		case EJson::String:
			Writer->WriteValue(JsonValue->AsString());
			break;
		case EJson::Number:
			Writer->WriteValue(JsonValue->AsNumber());
			break;
		case EJson::Boolean:
			Writer->WriteValue(JsonValue->AsBool());
			break;
		case EJson::Null:
			Writer->WriteNull();
			break;
		default:
			Writer->WriteValue(JsonValue->AsString());
			break;
		}
		Writer->Close();
		return Output;
	}

	static FString GetStringOrDefault(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const FString& DefaultValue = FString())
	{
		if (!JsonObject.IsValid())
		{
			return DefaultValue;
		}

		FString Value;
		return JsonObject->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
	}

	static FString GetScalarFieldAsString(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
	{
		if (!JsonObject.IsValid())
		{
			return FString();
		}

		FString StringValue;
		if (JsonObject->TryGetStringField(FieldName, StringValue))
		{
			return StringValue;
		}

		double NumberValue = 0.0;
		if (JsonObject->TryGetNumberField(FieldName, NumberValue))
		{
			const double RoundedValue = FMath::RoundToDouble(NumberValue);
			if (FMath::IsNearlyEqual(NumberValue, RoundedValue))
			{
				return FString::FromInt(static_cast<int32>(RoundedValue));
			}
			return FString::SanitizeFloat(NumberValue);
		}

		bool BoolValue = false;
		if (JsonObject->TryGetBoolField(FieldName, BoolValue))
		{
			return BoolValue ? TEXT("true") : TEXT("false");
		}

		return FString();
	}

	static bool GetBoolOrDefault(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const bool bDefaultValue)
	{
		if (!JsonObject.IsValid())
		{
			return bDefaultValue;
		}

		bool bValue = bDefaultValue;
		JsonObject->TryGetBoolField(FieldName, bValue);
		return bValue;
	}

	static TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
	{
		if (!JsonObject.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Result = nullptr;
		return JsonObject->TryGetObjectField(FieldName, Result) && Result != nullptr ? *Result : nullptr;
	}

	static TArray<TSharedPtr<FJsonValue>> GetArrayField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
	{
		if (!JsonObject.IsValid())
		{
			return {};
		}

		const TArray<TSharedPtr<FJsonValue>>* Result = nullptr;
		return JsonObject->TryGetArrayField(FieldName, Result) && Result != nullptr ? *Result : TArray<TSharedPtr<FJsonValue>>();
	}

	static TSharedPtr<FJsonObject> ObjectOrEmpty(const TSharedPtr<FJsonObject>& JsonObject)
	{
		return JsonObject.IsValid() ? JsonObject : MakeShared<FJsonObject>();
	}

	static int32 GetIntOrDefault(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const int32 DefaultValue = INDEX_NONE)
	{
		if (!JsonObject.IsValid())
		{
			return DefaultValue;
		}

		int32 Value = DefaultValue;
		return JsonObject->TryGetNumberField(FieldName, Value) ? Value : DefaultValue;
	}

	static FString NormalizePreferredOutputLanguage(const FString& LanguageCode)
	{
		return UEAgent::IsEnglishOutputLanguage(LanguageCode) ? TEXT("en-US") : TEXT("zh-CN");
	}

	static FString ResolveEffectiveOutputLanguage(const TSharedPtr<FJsonObject>& LocaleObject, const FString& FallbackLanguage)
	{
		const FString PreferredFallback = NormalizePreferredOutputLanguage(FallbackLanguage);
		if (!LocaleObject.IsValid())
		{
			return PreferredFallback;
		}

		const FString FinalOutputLanguage = GetStringOrDefault(LocaleObject, TEXT("final_output_language"));
		if (!FinalOutputLanguage.IsEmpty())
		{
			return NormalizePreferredOutputLanguage(FinalOutputLanguage);
		}

		const FString PreferredOutputLanguage = GetStringOrDefault(LocaleObject, TEXT("preferred_output_language"));
		if (!PreferredOutputLanguage.IsEmpty())
		{
			return NormalizePreferredOutputLanguage(PreferredOutputLanguage);
		}

		return PreferredFallback;
	}

	static FString GetLocalizedUiText(const FString& LanguageCode, const TCHAR* ZhText, const TCHAR* EnText)
	{
		return UEAgent::LocalizeStableUiText(LanguageCode, ZhText, EnText);
	}

	static FString LocalizeSeverityLabel(const FString& Severity, const FString& LanguageCode = TEXT("zh-CN"))
	{
		const bool bEnglish = UEAgent::IsEnglishOutputLanguage(LanguageCode);
		if (Severity.Equals(TEXT("critical"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Critical") : TEXT("严重");
		}
		if (Severity.Equals(TEXT("high"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("High") : TEXT("高");
		}
		if (Severity.Equals(TEXT("medium"), ESearchCase::IgnoreCase) || Severity.Equals(TEXT("moderate"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Medium") : TEXT("中");
		}
		if (Severity.Equals(TEXT("low"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Low") : TEXT("低");
		}
		if (Severity.Equals(TEXT("info"), ESearchCase::IgnoreCase) || Severity.Equals(TEXT("informational"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Info") : TEXT("提示");
		}
		return Severity;
	}

	static FString LocalizeStatusLabel(const FString& Status, const FString& LanguageCode = TEXT("zh-CN"))
	{
		const bool bEnglish = UEAgent::IsEnglishOutputLanguage(LanguageCode);
		if (Status.Equals(TEXT("success"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("ok"), ESearchCase::IgnoreCase)
			|| Status.Equals(TEXT("completed"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("saved"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Success") : TEXT("成功");
		}
		if (Status.Equals(TEXT("error"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Failed") : TEXT("失败");
		}
		if (Status.Equals(TEXT("skipped"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Skipped") : TEXT("已跳过");
		}
		if (Status.Equals(TEXT("pending"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("queued"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Pending") : TEXT("等待中");
		}
		return Status;
	}

	static FString LocalizePriorityLabel(const FString& Priority, const FString& LanguageCode = TEXT("zh-CN"))
	{
		const bool bEnglish = UEAgent::IsEnglishOutputLanguage(LanguageCode);
		if (Priority.Equals(TEXT("critical"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Critical") : TEXT("严重");
		}
		if (Priority.Equals(TEXT("high"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("High") : TEXT("高");
		}
		if (Priority.Equals(TEXT("medium"), ESearchCase::IgnoreCase) || Priority.Equals(TEXT("normal"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Medium") : TEXT("中");
		}
		if (Priority.Equals(TEXT("low"), ESearchCase::IgnoreCase))
		{
			return bEnglish ? TEXT("Low") : TEXT("低");
		}
		return Priority;
	}

	static FString GetTraceLinkUrl(const TSharedPtr<FJsonObject>& JsonObject)
	{
		return GetStringOrDefault(JsonObject, TEXT("url"),
			GetStringOrDefault(JsonObject, TEXT("href"),
				GetStringOrDefault(JsonObject, TEXT("link"))));
	}

	static FString GetTraceLinkLabel(const TSharedPtr<FJsonObject>& JsonObject)
	{
		return GetStringOrDefault(JsonObject, TEXT("label"),
			GetStringOrDefault(JsonObject, TEXT("title"),
				GetStringOrDefault(JsonObject, TEXT("name"), TEXT("Trace Link"))));
	}

	static EUEAgentChatRole ParseChatRole(const FString& RoleText)
	{
		if (RoleText.Equals(TEXT("user"), ESearchCase::IgnoreCase))
		{
			return EUEAgentChatRole::User;
		}
		if (RoleText.Equals(TEXT("assistant"), ESearchCase::IgnoreCase) || RoleText.Equals(TEXT("agent"), ESearchCase::IgnoreCase))
		{
			return EUEAgentChatRole::Agent;
		}
		return EUEAgentChatRole::System;
	}

	static bool IsUnifiedChatFunctionId(const FString& FunctionId)
	{
		return FunctionId.Equals(UEAgent::ToFunctionId(EUEAgentFunctionType::AgentChat), ESearchCase::IgnoreCase)
			|| FunctionId.Equals(UEAgent::ToFunctionId(EUEAgentFunctionType::ProjectQA), ESearchCase::IgnoreCase);
	}

	static bool IsUnifiedChatMessage(const TSharedPtr<FUEAgentChatMessage>& Message)
	{
		return Message.IsValid() && IsUnifiedChatFunctionId(Message->FunctionId);
	}

	static FDateTime ParseTimestamp(const FString& TimestampText)
	{
		FDateTime Parsed = FDateTime::UtcNow();
		if (!TimestampText.IsEmpty())
		{
			FDateTime::ParseIso8601(*TimestampText, Parsed);
		}
		return Parsed;
	}

	static TSharedPtr<FJsonObject> GetPreferredObject(const TSharedPtr<FJsonObject>& JsonObject, const FString& PreferredField)
	{
		if (!JsonObject.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject> PreferredObject = GetObjectField(JsonObject, PreferredField);
		if (PreferredObject.IsValid())
		{
			return PreferredObject;
		}

		const TSharedPtr<FJsonObject> ItemObject = GetObjectField(JsonObject, TEXT("item"));
		if (ItemObject.IsValid())
		{
			return ItemObject;
		}

		const TSharedPtr<FJsonObject> SessionObject = GetObjectField(JsonObject, TEXT("session"));
		if (SessionObject.IsValid())
		{
			return SessionObject;
		}

		return JsonObject;
	}

	static TSharedPtr<FJsonObject> MergeObjects(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary)
	{
		if (!Primary.IsValid() && !Secondary.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (Secondary.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Secondary->Values)
			{
				Result->SetField(Pair.Key, Pair.Value);
			}
		}
		if (Primary.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Primary->Values)
			{
				Result->SetField(Pair.Key, Pair.Value);
			}
		}
		return Result;
	}

	static void CopyJsonFieldIfPresent(const TSharedPtr<FJsonObject>& Target, const TSharedPtr<FJsonObject>& Source, const FString& FieldName)
	{
		if (!Target.IsValid() || !Source.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonValue> FieldValue = Source->TryGetField(FieldName);
		if (FieldValue.IsValid())
		{
			Target->SetField(FieldName, FieldValue);
		}
	}

	static FString BuildSkillDebugJson(
		const TSharedPtr<FJsonObject>& ExistingObject,
		const TSharedPtr<FJsonObject>& DebugViewObject,
		const TSharedPtr<FJsonObject>& DataObject,
		const TSharedPtr<FJsonObject>& TraceSummaryObject,
		const TSharedPtr<FJsonObject>& CapabilitiesObject)
	{
		TSharedPtr<FJsonObject> SkillObject = ExistingObject.IsValid() ? ExistingObject : MakeShared<FJsonObject>();

		const TSharedPtr<FJsonObject> DebugSkillObject = GetObjectField(DebugViewObject, TEXT("skill"));
		if (DebugSkillObject.IsValid())
		{
			SkillObject->SetObjectField(TEXT("debug_view_skill"), DebugSkillObject);
		}

		const TSharedPtr<FJsonObject> DataSkillObject = GetObjectField(DataObject, TEXT("skill"));
		if (DataSkillObject.IsValid())
		{
			SkillObject->SetObjectField(TEXT("data_skill"), DataSkillObject);
		}

		const FString TraceSkillId = GetStringOrDefault(TraceSummaryObject, TEXT("skill_id"));
		if (!TraceSkillId.IsEmpty())
		{
			SkillObject->SetStringField(TEXT("trace_summary_skill_id"), TraceSkillId);
		}

		if (CapabilitiesObject.IsValid())
		{
			TSharedPtr<FJsonObject> SkillCapabilitiesObject = MakeShared<FJsonObject>();
			CopyJsonFieldIfPresent(SkillCapabilitiesObject, CapabilitiesObject, TEXT("skill_catalog"));
			CopyJsonFieldIfPresent(SkillCapabilitiesObject, CapabilitiesObject, TEXT("core_skill_ids"));
			CopyJsonFieldIfPresent(SkillCapabilitiesObject, CapabilitiesObject, TEXT("skill_architecture"));
			CopyJsonFieldIfPresent(SkillCapabilitiesObject, CapabilitiesObject, TEXT("supported_task_types"));
			CopyJsonFieldIfPresent(SkillCapabilitiesObject, CapabilitiesObject, TEXT("deferred_task_types"));
			if (SkillCapabilitiesObject->Values.Num() > 0)
			{
				SkillObject->SetObjectField(TEXT("capabilities"), SkillCapabilitiesObject);
			}
		}

		return JsonToPrettyString(SkillObject);
	}

	static void PopulateTaskSummary(const TSharedPtr<FJsonObject>& TaskResponseObject, FUEAgentTaskSummary& TaskSummary)
	{
		const TSharedPtr<FJsonObject> TaskObject = GetObjectField(TaskResponseObject, TEXT("task"));
		const TSharedPtr<FJsonObject> ResolvedTaskObject = TaskObject.IsValid() ? TaskObject : TaskResponseObject;
		const TSharedPtr<FJsonObject> UserView = GetObjectField(TaskResponseObject, TEXT("user_view"));

		TaskSummary.TaskId = GetStringOrDefault(ResolvedTaskObject, TEXT("task_id"));
		TaskSummary.RunId = GetStringOrDefault(ResolvedTaskObject, TEXT("run_id"));
		TaskSummary.TaskType = GetStringOrDefault(ResolvedTaskObject, TEXT("task_type"));
		TaskSummary.Status = GetStringOrDefault(ResolvedTaskObject, TEXT("status"));
		TaskSummary.FinishReason = GetStringOrDefault(ResolvedTaskObject, TEXT("finish_reason"));
		TaskSummary.Title = GetStringOrDefault(UserView, TEXT("title"),
			GetStringOrDefault(TaskResponseObject, TEXT("title"), TaskSummary.TaskType));
	}

	static void PopulateProposalSummary(const TSharedPtr<FJsonObject>& ProposalObject, FUEAgentProposalSummary& Proposal)
	{
		if (!ProposalObject.IsValid())
		{
			return;
		}

		Proposal.ProposalId = GetStringOrDefault(ProposalObject, TEXT("proposal_id"));
		Proposal.Title = GetStringOrDefault(ProposalObject, TEXT("title"), TEXT("Proposal"));
		Proposal.ProposalType = GetStringOrDefault(ProposalObject, TEXT("proposal_type"));
		Proposal.BeforeSummary = GetStringOrDefault(ProposalObject, TEXT("before_summary"));
		Proposal.AfterSummary = GetStringOrDefault(ProposalObject, TEXT("after_summary"));
		Proposal.Rationale = GetStringOrDefault(ProposalObject, TEXT("rationale"));
		Proposal.RiskFlags = GetStringOrDefault(ProposalObject, TEXT("risk_flags"), TEXT("LOW"));
		Proposal.RawJson = JsonToPrettyString(ProposalObject);

		const TSharedPtr<FJsonObject> Confirmation = GetObjectField(ProposalObject, TEXT("confirmation"));
		Proposal.ConfirmationState = GetStringOrDefault(Confirmation, TEXT("state"), TEXT("pending"));
		Proposal.DecisionEndpoint = GetStringOrDefault(Confirmation, TEXT("decision_endpoint"));

		const TSharedPtr<FJsonObject> DryRunPreview = GetObjectField(ProposalObject, TEXT("dry_run_preview"));
		const TSharedPtr<FJsonObject> WritePlan = GetObjectField(DryRunPreview, TEXT("write_plan"));
		const TSharedPtr<FJsonObject> OperationPayload = GetObjectField(DryRunPreview, TEXT("operation_payload"));
		Proposal.OperationType = GetStringOrDefault(DryRunPreview, TEXT("operation_type"),
			GetStringOrDefault(ProposalObject, TEXT("operation_type")));
		Proposal.OperationPayloadJson = OperationPayload.IsValid() ? JsonToPrettyString(OperationPayload) : FString();

		TArray<FString> WritePlanLines;
		for (const TSharedPtr<FJsonValue>& FileValue : GetArrayField(WritePlan, TEXT("files")))
		{
			const TSharedPtr<FJsonObject> FileObject = FileValue.IsValid() && FileValue->Type == EJson::Object ? FileValue->AsObject() : nullptr;
			if (!FileObject.IsValid())
			{
				continue;
			}

			const FString RelativePath = GetStringOrDefault(FileObject, TEXT("relative_path"));
			const FString TargetPath = GetStringOrDefault(FileObject, TEXT("target_path"));
			const FString Status = GetStringOrDefault(FileObject, TEXT("status"));
			const FString Reason = GetStringOrDefault(FileObject, TEXT("reason"));
			const FString Bytes = GetScalarFieldAsString(FileObject, TEXT("bytes"));
			const bool bExists = GetBoolOrDefault(FileObject, TEXT("exists"), false);

			TArray<FString> Parts;
			Parts.Add(RelativePath.IsEmpty() ? TargetPath : RelativePath);
			if (!Status.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("status=%s"), *Status));
			}
			if (!Bytes.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("bytes=%s"), *Bytes));
			}
			if (bExists)
			{
				Parts.Add(TEXT("exists=true"));
			}
			if (!Reason.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("reason=%s"), *Reason));
			}
			WritePlanLines.Add(FString::Printf(TEXT("- %s"), *FString::Join(Parts, TEXT(" | "))));
		}
		Proposal.WritePlanSummary = FString::Join(WritePlanLines, TEXT("\n"));
	}

	static FUEAgentArtifactItem ParseArtifactItem(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FUEAgentArtifactItem Artifact;
		if (!JsonObject.IsValid())
		{
			return Artifact;
		}

		Artifact.ArtifactId = GetStringOrDefault(JsonObject, TEXT("artifact_id"));
		Artifact.ArtifactType = GetStringOrDefault(JsonObject, TEXT("artifact_type"),
			GetStringOrDefault(JsonObject, TEXT("type")));
		Artifact.Label = GetStringOrDefault(JsonObject, TEXT("label"),
			GetStringOrDefault(JsonObject, TEXT("title"), TEXT("Artifact")));
		Artifact.Path = GetStringOrDefault(JsonObject, TEXT("path"),
			GetStringOrDefault(JsonObject, TEXT("uri"),
				GetStringOrDefault(JsonObject, TEXT("url"))));
		Artifact.MetadataJson = JsonFieldToPrettyString(JsonObject, TEXT("metadata"));
		return Artifact;
	}

	static TSharedPtr<FUEAgentCodeFileItem> ParseCodeFileItem(const TSharedPtr<FJsonValue>& FileValue)
	{
		TSharedPtr<FUEAgentCodeFileItem> FileItem = MakeShared<FUEAgentCodeFileItem>();
		if (!FileValue.IsValid())
		{
			return nullptr;
		}

		if (FileValue->Type == EJson::String)
		{
			FileItem->FilePath = FileValue->AsString();
		}
		else if (FileValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> FileObject = FileValue->AsObject();
			FileItem->FilePath = GetStringOrDefault(FileObject, TEXT("file_path"),
				GetStringOrDefault(FileObject, TEXT("path")));
			FileItem->RelativePath = GetStringOrDefault(FileObject, TEXT("relative_path"));
			FileItem->ModuleName = GetStringOrDefault(FileObject, TEXT("module_name"),
				GetStringOrDefault(FileObject, TEXT("module")));
			FileItem->FileType = GetStringOrDefault(FileObject, TEXT("file_type"),
				GetStringOrDefault(FileObject, TEXT("type"),
					GetStringOrDefault(FileObject, TEXT("extension"))));
			FileItem->Label = GetStringOrDefault(FileObject, TEXT("label"),
				GetStringOrDefault(FileObject, TEXT("title"),
					GetStringOrDefault(FileObject, TEXT("name"))));
		}

		FileItem->FilePath.TrimStartAndEndInline();
		FileItem->RelativePath.TrimStartAndEndInline();
		if (FileItem->FilePath.IsEmpty())
		{
			FileItem->FilePath = FileItem->RelativePath;
		}
		if (FileItem->RelativePath.IsEmpty())
		{
			FileItem->RelativePath = FileItem->FilePath;
		}
		if (FileItem->FilePath.IsEmpty())
		{
			return nullptr;
		}

		if (FileItem->Label.IsEmpty())
		{
			FileItem->Label = FPaths::GetCleanFilename(FileItem->FilePath);
		}
		if (FileItem->FileType.IsEmpty())
		{
			FileItem->FileType = FPaths::GetExtension(FileItem->FilePath);
		}
		return FileItem;
	}

	static bool IsSameArtifact(const FUEAgentArtifactItem& Left, const FUEAgentArtifactItem& Right)
	{
		if (!Left.ArtifactId.IsEmpty() && !Right.ArtifactId.IsEmpty())
		{
			return Left.ArtifactId == Right.ArtifactId;
		}

		if (!Left.Path.IsEmpty() && !Right.Path.IsEmpty())
		{
			return Left.Path == Right.Path;
		}

		return !Left.Label.IsEmpty() && Left.Label == Right.Label && Left.ArtifactType == Right.ArtifactType;
	}

	static void UpsertArtifact(TArray<FUEAgentArtifactItem>& Artifacts, const FUEAgentArtifactItem& Candidate)
	{
		if (Candidate.Label.IsEmpty() && Candidate.Path.IsEmpty() && Candidate.ArtifactType.IsEmpty())
		{
			return;
		}

		for (FUEAgentArtifactItem& Existing : Artifacts)
		{
			if (!IsSameArtifact(Existing, Candidate))
			{
				continue;
			}

			if (!Candidate.ArtifactId.IsEmpty())
			{
				Existing.ArtifactId = Candidate.ArtifactId;
			}
			if (!Candidate.ArtifactType.IsEmpty())
			{
				Existing.ArtifactType = Candidate.ArtifactType;
			}
			if (!Candidate.Label.IsEmpty())
			{
				Existing.Label = Candidate.Label;
			}
			if (!Candidate.Path.IsEmpty())
			{
				Existing.Path = Candidate.Path;
			}
			if (!Candidate.MetadataJson.IsEmpty() && Candidate.MetadataJson != TEXT("{}"))
			{
				Existing.MetadataJson = Candidate.MetadataJson;
			}
			return;
		}

		Artifacts.Add(Candidate);
	}

	static FString BuildBlockItemSummary(const TSharedPtr<FJsonValue>& ItemValue, const FString& LanguageCode = TEXT("zh-CN"))
	{
		if (!ItemValue.IsValid())
		{
			return FString();
		}

		if (ItemValue->Type == EJson::String)
		{
			return ItemValue->AsString();
		}

		if (ItemValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> ItemObject = ItemValue->AsObject();
			const FString Title = GetStringOrDefault(ItemObject, TEXT("title"),
				GetStringOrDefault(ItemObject, TEXT("summary"),
					GetStringOrDefault(ItemObject, TEXT("label"))));
			const FString Subject = GetStringOrDefault(ItemObject, TEXT("asset_path"),
				GetStringOrDefault(ItemObject, TEXT("file_path"),
					GetStringOrDefault(ItemObject, TEXT("path"),
						GetStringOrDefault(ItemObject, TEXT("name"),
							GetStringOrDefault(ItemObject, TEXT("symbol"))))));
			const FString Severity = LocalizeSeverityLabel(GetStringOrDefault(ItemObject, TEXT("severity")), LanguageCode);
			const FString Location = GetStringOrDefault(ItemObject, TEXT("location"),
				GetStringOrDefault(ItemObject, TEXT("range")));
			const FString Line = GetScalarFieldAsString(ItemObject, TEXT("line")).IsEmpty()
				? GetScalarFieldAsString(ItemObject, TEXT("start_line"))
				: GetScalarFieldAsString(ItemObject, TEXT("line"));
			const FString Rule = GetStringOrDefault(ItemObject, TEXT("rule"),
				GetStringOrDefault(ItemObject, TEXT("rule_id"),
					GetStringOrDefault(ItemObject, TEXT("category"))));
			const FString Reason = GetStringOrDefault(ItemObject, TEXT("reason"));
			const FString Impact = GetStringOrDefault(ItemObject, TEXT("impact"));
			const FString Evidence = GetStringOrDefault(ItemObject, TEXT("evidence"),
				GetStringOrDefault(ItemObject, TEXT("snippet")));
			const FString Suggestion = GetStringOrDefault(ItemObject, TEXT("suggestion"));
			if (!Reason.IsEmpty() || !Suggestion.IsEmpty() || !Severity.IsEmpty() || !Location.IsEmpty() || !Line.IsEmpty() || !Impact.IsEmpty() || !Evidence.IsEmpty())
			{
				TArray<FString> Parts;
				if (!Title.IsEmpty())
				{
					Parts.Add(Title);
				}
				if (!Subject.IsEmpty())
				{
					Parts.Add(Subject);
				}
				if (!Severity.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("[%s]"), *Severity));
				}
				if (!Location.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("位置："), TEXT("Location: ")), *Location));
				}
				else if (!Line.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("行："), TEXT("Line: ")), *Line));
				}
				if (!Rule.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("规则："), TEXT("Rule: ")), *Rule));
				}
				if (!Reason.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("原因："), TEXT("Reason: ")), *Reason));
				}
				if (!Impact.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("影响："), TEXT("Impact: ")), *Impact));
				}
				if (!Evidence.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("证据："), TEXT("Evidence: ")), *Evidence));
				}
				if (!Suggestion.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("建议："), TEXT("Suggestion: ")), *Suggestion));
				}
				return FString::Join(Parts, TEXT("  "));
			}

			const FString Text = GetStringOrDefault(ItemObject, TEXT("text"),
				GetStringOrDefault(ItemObject, TEXT("summary"),
					GetStringOrDefault(ItemObject, TEXT("label"),
						GetStringOrDefault(ItemObject, TEXT("file_path"),
							GetStringOrDefault(ItemObject, TEXT("asset_path"),
								GetStringOrDefault(ItemObject, TEXT("source"),
									GetStringOrDefault(ItemObject, TEXT("name"))))))));
			if (!Title.IsEmpty() && !Text.IsEmpty())
			{
				return FString::Printf(TEXT("%s: %s"), *Title, *Text);
			}
			return !Title.IsEmpty() ? Title : Text;
		}

		if (ItemValue->Type == EJson::Number)
		{
			return FString::SanitizeFloat(ItemValue->AsNumber());
		}

		if (ItemValue->Type == EJson::Boolean)
		{
			return ItemValue->AsBool() ? TEXT("true") : TEXT("false");
		}

		return FString();
	}

	static TArray<FString> ExtractBlockItems(const TSharedPtr<FJsonObject>& DataObject, const FString& LanguageCode = TEXT("zh-CN"))
	{
		if (!DataObject.IsValid())
		{
			return {};
		}

		static const TCHAR* CandidateArrays[] = { TEXT("items"), TEXT("key_points"), TEXT("points"), TEXT("rows"), TEXT("entries"), TEXT("list") };
		for (const TCHAR* FieldName : CandidateArrays)
		{
			const TArray<TSharedPtr<FJsonValue>>* ItemArray = nullptr;
			if (!DataObject->TryGetArrayField(FieldName, ItemArray) || ItemArray == nullptr)
			{
				continue;
			}

			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& ItemValue : *ItemArray)
			{
				const FString Summary = BuildBlockItemSummary(ItemValue, LanguageCode);
				if (!Summary.IsEmpty())
				{
					Items.Add(Summary);
				}
			}

			if (Items.Num() > 0)
			{
				return Items;
			}
		}

		return {};
	}

	static void AddLocalizedReviewBlockFromField(
		TArray<FUEAgentUserViewBlock>& Blocks,
		const TSharedPtr<FJsonObject>& ReviewObject,
		const FString& FieldName,
		const FString& BlockType,
		const FString& Title,
		const FString& LanguageCode = TEXT("zh-CN"))
	{
		if (!ReviewObject.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonValue> FieldValue = ReviewObject->TryGetField(FieldName);
		if (!FieldValue.IsValid())
		{
			return;
		}

		FUEAgentUserViewBlock Block;
		Block.BlockType = BlockType;
		Block.Title = Title;

		if (FieldValue->Type == EJson::String)
		{
			Block.Text = FieldValue->AsString();
		}
		else if (FieldValue->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& ItemValue : FieldValue->AsArray())
			{
				const FString Summary = BuildBlockItemSummary(ItemValue, LanguageCode);
				if (!Summary.IsEmpty())
				{
					Block.Items.Add(Summary);
				}
			}
		}
		else if (FieldValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> FieldObject = FieldValue->AsObject();
			Block.Text = GetStringOrDefault(FieldObject, TEXT("text"),
				GetStringOrDefault(FieldObject, TEXT("summary")));
			Block.Items = ExtractBlockItems(FieldObject, LanguageCode);
			if (Block.Text.IsEmpty() && Block.Items.Num() == 0)
			{
				Block.Text = BuildBlockItemSummary(MakeShared<FJsonValueObject>(FieldObject), LanguageCode);
			}
			Block.JsonPreview = JsonToPrettyString(FieldObject);
		}

		if (!Block.Text.IsEmpty() || Block.Items.Num() > 0 || (!Block.JsonPreview.IsEmpty() && Block.JsonPreview != TEXT("{}")))
		{
			Blocks.Add(Block);
		}
	}

	static bool IsBlockType(const FUEAgentUserViewBlock& Block, const FString& BlockType)
	{
		FString CurrentType = Block.BlockType;
		CurrentType.ReplaceInline(TEXT("-"), TEXT("_"));
		FString ExpectedType = BlockType;
		ExpectedType.ReplaceInline(TEXT("-"), TEXT("_"));
		return CurrentType.Equals(ExpectedType, ESearchCase::IgnoreCase);
	}

	static int32 FindBlockIndex(const TArray<FUEAgentUserViewBlock>& Blocks, const FString& BlockType)
	{
		for (int32 Index = 0; Index < Blocks.Num(); ++Index)
		{
			if (IsBlockType(Blocks[Index], BlockType))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static void AddUniqueBlockItem(TArray<FString>& Items, const FString& Item)
	{
		if (!Item.IsEmpty())
		{
			Items.AddUnique(Item);
		}
	}

	static FString JoinJsonArrayAsText(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const int32 MaxItems = 6)
	{
		const TArray<TSharedPtr<FJsonValue>> Values = GetArrayField(JsonObject, FieldName);
		if (Values.Num() == 0)
		{
			return FString();
		}

		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			FString Text;
			if (Value->Type == EJson::String)
			{
				Text = Value->AsString();
			}
			else if (Value->Type == EJson::Number)
			{
				Text = FString::SanitizeFloat(Value->AsNumber());
			}
			else if (Value->Type == EJson::Boolean)
			{
				Text = Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else if (Value->Type == EJson::Object)
			{
				Text = BuildBlockItemSummary(Value);
			}

			Text.TrimStartAndEndInline();
			if (!Text.IsEmpty())
			{
				Parts.Add(Text);
			}
			if (Parts.Num() >= MaxItems)
			{
				break;
			}
		}

		if (Values.Num() > MaxItems)
		{
			Parts.Add(FString::Printf(TEXT("+%d more"), Values.Num() - MaxItems));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	static void EnrichEditorOperationResultBlock(FUEAgentUserViewBlock& Block, const TSharedPtr<FJsonObject>& SummaryObject, const FString& LanguageCode = TEXT("zh-CN"))
	{
		if (!IsBlockType(Block, TEXT("editor_operation_result_summary")))
		{
			return;
		}
		if (Block.Title.IsEmpty())
		{
			Block.Title = GetLocalizedUiText(LanguageCode, TEXT("Editor Operation Result"), TEXT("Editor Operation Result"));
		}
		if (!SummaryObject.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject> DiagnosticsObject = GetObjectField(SummaryObject, TEXT("operation_diagnostics"));
		const TSharedPtr<FJsonObject> RepairAdviceObject = GetObjectField(SummaryObject, TEXT("repair_advice")).IsValid()
			? GetObjectField(SummaryObject, TEXT("repair_advice"))
			: GetObjectField(DiagnosticsObject, TEXT("repair_advice"));

		AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Execution: %s | Success: %s"),
			*GetScalarFieldAsString(SummaryObject, TEXT("execution_state")),
			*GetScalarFieldAsString(SummaryObject, TEXT("success"))));

		const FString OperationType = GetStringOrDefault(DiagnosticsObject, TEXT("operation_type"));
		const FString BlueprintPath = GetStringOrDefault(DiagnosticsObject, TEXT("blueprint_path"));
		const FString GraphName = GetStringOrDefault(DiagnosticsObject, TEXT("graph_name"));
		if (!OperationType.IsEmpty())
		{
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Operation: %s"), *OperationType));
		}
		if (!BlueprintPath.IsEmpty())
		{
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Blueprint: %s"), *BlueprintPath));
		}
		if (!GraphName.IsEmpty())
		{
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Graph: %s"), *GraphName));
		}
		if (DiagnosticsObject.IsValid())
		{
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Created nodes: %s | Linked pins: %s | Compile: %s"),
				*GetScalarFieldAsString(DiagnosticsObject, TEXT("created_node_count")),
				*GetScalarFieldAsString(DiagnosticsObject, TEXT("linked_pin_count")),
				*GetStringOrDefault(DiagnosticsObject, TEXT("compile_status"), TEXT("n/a"))));

			const FString DiagnosticFlags = JoinJsonArrayAsText(DiagnosticsObject, TEXT("diagnostic_flags"));
			if (!DiagnosticFlags.IsEmpty())
			{
				AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Diagnostic flags: %s"), *DiagnosticFlags));
			}
		}

		const FString DirtyPackages = JoinJsonArrayAsText(SummaryObject, TEXT("dirty_packages"));
		if (!DirtyPackages.IsEmpty())
		{
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Dirty packages: %s"), *DirtyPackages));
		}

		if (RepairAdviceObject.IsValid())
		{
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Repair advice: %s | Severity: %s"),
				*GetStringOrDefault(RepairAdviceObject, TEXT("status"), TEXT("n/a")),
				*GetStringOrDefault(RepairAdviceObject, TEXT("severity"), TEXT("n/a"))));

			const TArray<TSharedPtr<FJsonValue>> Actions = GetArrayField(RepairAdviceObject, TEXT("actions"));
			for (int32 Index = 0; Index < Actions.Num() && Index < 3; ++Index)
			{
				const TSharedPtr<FJsonObject> ActionObject = Actions[Index].IsValid() ? Actions[Index]->AsObject() : nullptr;
				if (!ActionObject.IsValid())
				{
					continue;
				}
				const FString Title = GetStringOrDefault(ActionObject, TEXT("title"));
				const FString NextStep = GetStringOrDefault(ActionObject, TEXT("next_step"));
				if (!Title.IsEmpty() || !NextStep.IsEmpty())
				{
					AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Repair %d: %s%s%s"),
						Index + 1,
						*Title,
						!Title.IsEmpty() && !NextStep.IsEmpty() ? TEXT(" -> ") : TEXT(""),
						*NextStep));
				}
			}
		}
	}

	static void EnrichEditorOperationFollowUpsBlock(FUEAgentUserViewBlock& Block, const TSharedPtr<FJsonObject>& FollowUpObject, const FString& LanguageCode = TEXT("zh-CN"))
	{
		if (!IsBlockType(Block, TEXT("editor_operation_follow_ups")))
		{
			return;
		}
		if (Block.Title.IsEmpty())
		{
			Block.Title = GetLocalizedUiText(LanguageCode, TEXT("Follow-up Candidates"), TEXT("Follow-up Candidates"));
		}
		if (!FollowUpObject.IsValid())
		{
			return;
		}

		AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Status: %s | Ready: %s/%s"),
			*GetStringOrDefault(FollowUpObject, TEXT("status"), TEXT("n/a")),
			*GetScalarFieldAsString(FollowUpObject, TEXT("ready_candidate_count")),
			*GetScalarFieldAsString(FollowUpObject, TEXT("candidate_count"))));

		const FString DiagnosticFlags = JoinJsonArrayAsText(FollowUpObject, TEXT("diagnostic_flags"));
		if (!DiagnosticFlags.IsEmpty())
		{
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Diagnostic flags: %s"), *DiagnosticFlags));
		}

		const TArray<TSharedPtr<FJsonValue>> Candidates = GetArrayField(FollowUpObject, TEXT("candidates"));
		for (int32 Index = 0; Index < Candidates.Num() && Index < 4; ++Index)
		{
			const TSharedPtr<FJsonObject> CandidateObject = Candidates[Index].IsValid() ? Candidates[Index]->AsObject() : nullptr;
			if (!CandidateObject.IsValid())
			{
				continue;
			}

			const FString CandidateId = GetStringOrDefault(CandidateObject, TEXT("candidate_id"), FString::Printf(TEXT("candidate_%d"), Index + 1));
			const FString OperationType = GetStringOrDefault(CandidateObject, TEXT("operation_type"));
			const FString Ready = GetScalarFieldAsString(CandidateObject, TEXT("proposal_ready"));
			const FString Reason = GetStringOrDefault(CandidateObject, TEXT("reason"));
			const FString MissingInputs = JoinJsonArrayAsText(CandidateObject, TEXT("missing_inputs"));
			AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Candidate: %s | %s | ready=%s%s%s"),
				*CandidateId,
				*OperationType,
				*Ready,
				MissingInputs.IsEmpty() ? TEXT("") : TEXT(" | missing="),
				*MissingInputs));
			if (!Reason.IsEmpty())
			{
				AddUniqueBlockItem(Block.Items, FString::Printf(TEXT("Reason: %s"), *Reason));
			}
		}
	}

	static FString GetLlmAnalysisField(const TSharedPtr<FJsonObject>& AnalysisObject, const FString& FieldName)
	{
		const FString DirectValue = GetStringOrDefault(AnalysisObject, FieldName);
		if (!DirectValue.IsEmpty())
		{
			return DirectValue;
		}
		return GetStringOrDefault(GetObjectField(AnalysisObject, TEXT("data")), FieldName);
	}

	static TArray<FString> ExtractLlmAnalysisItems(const TSharedPtr<FJsonObject>& AnalysisObject, const FString& LanguageCode = TEXT("zh-CN"))
	{
		TArray<FString> Items = ExtractBlockItems(AnalysisObject, LanguageCode);
		if (Items.Num() == 0)
		{
			Items = ExtractBlockItems(GetObjectField(AnalysisObject, TEXT("data")), LanguageCode);
		}
		return Items;
	}

	static void EnrichLlmAnalysisBlock(FUEAgentUserViewBlock& Block, const TSharedPtr<FJsonObject>& AnalysisObject, const FString& LanguageCode = TEXT("zh-CN"))
	{
		if (!IsBlockType(Block, TEXT("llm_analysis")))
		{
			return;
		}

		if (Block.Title.IsEmpty())
		{
			Block.Title = GetLocalizedUiText(LanguageCode, TEXT("LLM 分析结果"), TEXT("LLM Analysis"));
		}

		if (AnalysisObject.IsValid())
		{
			if (Block.Text.IsEmpty())
			{
				Block.Text = GetStringOrDefault(AnalysisObject, TEXT("text"),
					GetStringOrDefault(AnalysisObject, TEXT("summary"),
						GetStringOrDefault(AnalysisObject, TEXT("analysis"),
							GetStringOrDefault(GetObjectField(AnalysisObject, TEXT("data")), TEXT("text")))));
			}

			if (Block.Items.Num() == 0)
			{
				Block.Items = ExtractLlmAnalysisItems(AnalysisObject, LanguageCode);
			}

			const FString Status = GetLlmAnalysisField(AnalysisObject, TEXT("status"));
			const FString Priority = GetLlmAnalysisField(AnalysisObject, TEXT("priority"));
			const FString Reason = GetLlmAnalysisField(AnalysisObject, TEXT("reason"));
			const FString ReasonCode = GetLlmAnalysisField(AnalysisObject, TEXT("reason_code"));
			if (!Status.IsEmpty())
			{
				const FString StatusItem = FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("状态："), TEXT("Status: ")), *LocalizeStatusLabel(Status, LanguageCode));
				if (!Block.Items.Contains(StatusItem))
				{
					Block.Items.Insert(StatusItem, 0);
				}
			}
			AddUniqueBlockItem(Block.Items, Priority.IsEmpty() ? FString() : FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("优先级："), TEXT("Priority: ")), *LocalizePriorityLabel(Priority, LanguageCode)));
			AddUniqueBlockItem(Block.Items, Reason.IsEmpty() ? FString() : FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("说明："), TEXT("Reason: ")), *Reason));
			AddUniqueBlockItem(Block.Items, ReasonCode.IsEmpty() ? FString() : FString::Printf(TEXT("%s%s"), *GetLocalizedUiText(LanguageCode, TEXT("原因代码："), TEXT("Reason Code: ")), *ReasonCode));
		}
	}

	static void MoveBlockAfterSummary(TArray<FUEAgentUserViewBlock>& Blocks, const FString& BlockType)
	{
		const int32 BlockIndex = FindBlockIndex(Blocks, BlockType);
		if (BlockIndex == INDEX_NONE)
		{
			return;
		}

		FUEAgentUserViewBlock MovingBlock = Blocks[BlockIndex];
		Blocks.RemoveAt(BlockIndex);

		int32 InsertIndex = 0;
		const int32 SummaryIndex = FindBlockIndex(Blocks, TEXT("summary"));
		if (SummaryIndex != INDEX_NONE)
		{
			InsertIndex = SummaryIndex + 1;
		}
		else
		{
			static const TCHAR* SummaryAliases[] = { TEXT("log_summary"), TEXT("logs_summary"), TEXT("analysis_summary"), TEXT("result_summary") };
			for (const TCHAR* SummaryAlias : SummaryAliases)
			{
				const int32 AliasIndex = FindBlockIndex(Blocks, SummaryAlias);
				if (AliasIndex != INDEX_NONE)
				{
					InsertIndex = AliasIndex + 1;
					break;
				}
			}
		}
		Blocks.Insert(MovingBlock, InsertIndex);
	}

	static void AddLlmAnalysisBlockIfMissing(TArray<FUEAgentUserViewBlock>& Blocks, const TSharedPtr<FJsonObject>& ResponseDataObject, const FString& LanguageCode = TEXT("zh-CN"))
	{
		const TSharedPtr<FJsonObject> LlmAnalysisObject = GetObjectField(ResponseDataObject, TEXT("llm_analysis"));
		if (!LlmAnalysisObject.IsValid())
		{
			MoveBlockAfterSummary(Blocks, TEXT("llm_analysis"));
			return;
		}

		if (FindBlockIndex(Blocks, TEXT("llm_analysis")) == INDEX_NONE)
		{
			FUEAgentUserViewBlock LlmAnalysisBlock;
			LlmAnalysisBlock.BlockType = TEXT("llm_analysis");
			LlmAnalysisBlock.Title = GetStringOrDefault(LlmAnalysisObject, TEXT("title"), GetLocalizedUiText(LanguageCode, TEXT("LLM 分析结果"), TEXT("LLM Analysis")));
			LlmAnalysisBlock.Text = GetStringOrDefault(LlmAnalysisObject, TEXT("text"),
				GetStringOrDefault(LlmAnalysisObject, TEXT("summary"),
					GetStringOrDefault(LlmAnalysisObject, TEXT("analysis"))));
			LlmAnalysisBlock.Items = ExtractLlmAnalysisItems(LlmAnalysisObject, LanguageCode);
			LlmAnalysisBlock.JsonPreview = JsonToPrettyString(LlmAnalysisObject);
			EnrichLlmAnalysisBlock(LlmAnalysisBlock, LlmAnalysisObject, LanguageCode);
			Blocks.Add(LlmAnalysisBlock);
		}
		else
		{
			const int32 ExistingIndex = FindBlockIndex(Blocks, TEXT("llm_analysis"));
			if (Blocks.IsValidIndex(ExistingIndex))
			{
				EnrichLlmAnalysisBlock(Blocks[ExistingIndex], LlmAnalysisObject, LanguageCode);
			}
		}

		MoveBlockAfterSummary(Blocks, TEXT("llm_analysis"));
	}

	static bool HasUserBlockByTypeOrTitle(const TArray<FUEAgentUserViewBlock>& Blocks, const FString& BlockType, const FString& Title)
	{
		for (const FUEAgentUserViewBlock& Block : Blocks)
		{
			if (IsBlockType(Block, BlockType))
			{
				return true;
			}

			if (!Title.IsEmpty() && Block.Title.Equals(Title, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	static void AddInventoryMatchBlockIfMissing(
		TArray<FUEAgentUserViewBlock>& Blocks,
		const TSharedPtr<FJsonObject>& ResponseDataObject,
		const FString& FieldName,
		const FString& Title,
		const FString& LanguageCode = TEXT("zh-CN"))
	{
		if (HasUserBlockByTypeOrTitle(Blocks, TEXT("project_inventory"), Title)
			|| HasUserBlockByTypeOrTitle(Blocks, TEXT("project_inventory_matches"), TEXT("Project Inventory Matches"))
			|| HasUserBlockByTypeOrTitle(Blocks, TEXT("inventory"), TEXT("Project Inventory Matches")))
		{
			return;
		}

		const TSharedPtr<FJsonObject> InventoryObject = GetObjectField(ResponseDataObject, FieldName);
		if (!InventoryObject.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>> Items = GetArrayField(InventoryObject, TEXT("items"));
		if (Items.Num() == 0 && GetStringOrDefault(InventoryObject, TEXT("summary")).IsEmpty()
			&& GetStringOrDefault(InventoryObject, TEXT("text")).IsEmpty())
		{
			return;
		}

		FUEAgentUserViewBlock InventoryBlock;
		InventoryBlock.BlockType = TEXT("project_inventory");
		InventoryBlock.Title = GetStringOrDefault(InventoryObject, TEXT("title"), Title);
		InventoryBlock.Text = GetStringOrDefault(InventoryObject, TEXT("text"),
			GetStringOrDefault(InventoryObject, TEXT("summary")));
		InventoryBlock.Items = ExtractBlockItems(InventoryObject, LanguageCode);
		InventoryBlock.JsonPreview = JsonToPrettyString(InventoryObject);
		Blocks.Add(InventoryBlock);
	}

	static FString BuildEventSummary(const TSharedPtr<FJsonObject>& EventObject)
	{
		if (!EventObject.IsValid())
		{
			return FString();
		}

		const TSharedPtr<FJsonObject> PayloadObject = GetObjectField(EventObject, TEXT("payload"));
		return GetStringOrDefault(EventObject, TEXT("summary"),
			GetStringOrDefault(PayloadObject, TEXT("summary"),
				GetStringOrDefault(PayloadObject, TEXT("message"),
					GetStringOrDefault(PayloadObject, TEXT("text"),
						GetStringOrDefault(PayloadObject, TEXT("title"))))));
	}

	static FUEAgentRunEvent ParseRunEventObject(const TSharedPtr<FJsonObject>& EventObject)
	{
		FUEAgentRunEvent Event;
		if (!EventObject.IsValid())
		{
			return Event;
		}

		Event.Seq = GetIntOrDefault(EventObject, TEXT("seq"));
		Event.EventType = GetStringOrDefault(EventObject, TEXT("event_type"),
			GetStringOrDefault(EventObject, TEXT("type"),
				GetStringOrDefault(EventObject, TEXT("event"))));
		Event.Timestamp = GetStringOrDefault(EventObject, TEXT("timestamp"));
		Event.Summary = BuildEventSummary(EventObject);
		Event.RawJson = JsonToPrettyString(EventObject);
		return Event;
	}

	static TArray<FUEAgentRunEvent> ParseSseEvents(const FString& ResponseText)
	{
		TArray<FUEAgentRunEvent> Events;
		TArray<FString> Lines;
		ResponseText.ParseIntoArrayLines(Lines, false);

		FString CurrentEventType;
		FString CurrentData;

		auto FlushCurrentEvent = [&Events, &CurrentEventType, &CurrentData]()
		{
			if (CurrentEventType.IsEmpty() && CurrentData.TrimStartAndEnd().IsEmpty())
			{
				return;
			}

			FUEAgentRunEvent Event;
			Event.EventType = CurrentEventType;

			TSharedPtr<FJsonObject> EventObject;
			if (!CurrentData.TrimStartAndEnd().IsEmpty())
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CurrentData);
				FJsonSerializer::Deserialize(Reader, EventObject);
			}

			if (EventObject.IsValid())
			{
				Event = ParseRunEventObject(EventObject);
				if (Event.EventType.IsEmpty())
				{
					Event.EventType = CurrentEventType;
				}
			}
			else
			{
				Event.Summary = CurrentData.TrimStartAndEnd();
				Event.RawJson = CurrentData.TrimStartAndEnd();
			}

			Events.Add(Event);
			CurrentEventType.Reset();
			CurrentData.Reset();
		};

		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.IsEmpty())
			{
				FlushCurrentEvent();
				continue;
			}

			if (Line.StartsWith(TEXT("event:")))
			{
				CurrentEventType = Line.RightChop(6).TrimStartAndEnd();
				continue;
			}

			if (Line.StartsWith(TEXT("data:")))
			{
				if (!CurrentData.IsEmpty())
				{
					CurrentData.Append(TEXT("\n"));
				}
				CurrentData.Append(Line.RightChop(5).TrimStartAndEnd());
			}
		}

		FlushCurrentEvent();
		return Events;
	}

	static FString JoinErrors(const TArray<TSharedPtr<FJsonValue>>& Errors)
	{
		TArray<FString> Messages;
		for (const TSharedPtr<FJsonValue>& ErrorValue : Errors)
		{
			const TSharedPtr<FJsonObject> ErrorObject = ErrorValue.IsValid() ? ErrorValue->AsObject() : nullptr;
			if (!ErrorObject.IsValid())
			{
				continue;
			}

			const FString Code = GetStringOrDefault(ErrorObject, TEXT("code"), TEXT("error"));
			const FString Message = GetStringOrDefault(ErrorObject, TEXT("message"), TEXT("Unknown error"));
			Messages.Add(FString::Printf(TEXT("[%s] %s"), *Code, *Message));
		}

		return Messages.Num() > 0 ? FString::Join(Messages, TEXT("\n")) : TEXT("Request failed.");
	}
}

FUEAgentStateStore::FUEAgentStateStore()
{
	InitializeParameterDefaults();
	LoadPersistedState();
}

FUEAgentStateStore::FOnStateChanged& FUEAgentStateStore::OnStateChanged()
{
	return StateChangedDelegate;
}

void FUEAgentStateStore::SetActiveViewMode(const EUEAgentViewMode InViewMode)
{
	ActiveViewMode = InViewMode;
	BroadcastStateChanged();
}

EUEAgentViewMode FUEAgentStateStore::GetActiveViewMode() const
{
	return ActiveViewMode;
}

void FUEAgentStateStore::SetActiveFunction(const EUEAgentFunctionType InFunction)
{
	ActiveFunction = InFunction;
	BroadcastStateChanged();
}

EUEAgentFunctionType FUEAgentStateStore::GetActiveFunction() const
{
	return ActiveFunction;
}

void FUEAgentStateStore::SetActiveDebugSection(const EUEAgentDebugSection InSection)
{
	ActiveDebugSection = InSection;
	BroadcastStateChanged();
}

EUEAgentDebugSection FUEAgentStateStore::GetActiveDebugSection() const
{
	return ActiveDebugSection;
}

void FUEAgentStateStore::SetSettingsExpanded(const bool bExpanded)
{
	bSettingsExpanded = bExpanded;
	BroadcastStateChanged();
}

bool FUEAgentStateStore::IsSettingsExpanded() const
{
	return bSettingsExpanded;
}

void FUEAgentStateStore::SetBackendBaseUrl(const FString& InBaseUrl)
{
	BackendBaseUrl = InBaseUrl;
	PersistState();
	BroadcastStateChanged();
}

const FString& FUEAgentStateStore::GetBackendBaseUrl() const
{
	return BackendBaseUrl;
}

void FUEAgentStateStore::SetPreferredOutputLanguage(const FString& InLanguageCode, const bool bBroadcast)
{
	const FString NormalizedLanguage = UEAgentStateStorePrivate::NormalizePreferredOutputLanguage(InLanguageCode);
	if (PreferredOutputLanguage == NormalizedLanguage)
	{
		return;
	}

	PreferredOutputLanguage = NormalizedLanguage;
	PersistState();
	if (bBroadcast)
	{
		BroadcastStateChanged();
	}
}

const FString& FUEAgentStateStore::GetPreferredOutputLanguage() const
{
	return PreferredOutputLanguage;
}

FString FUEAgentStateStore::GetEffectiveOutputLanguage() const
{
	return UEAgentStateStorePrivate::ResolveEffectiveOutputLanguage(
		UEAgentStateStorePrivate::ParseJsonObject(LastResult.LocaleJson),
		PreferredOutputLanguage);
}

FString FUEAgentStateStore::GetLastLanguageSource() const
{
	return UEAgentStateStorePrivate::GetStringOrDefault(
		UEAgentStateStorePrivate::ParseJsonObject(LastResult.LocaleJson),
		TEXT("language_source"));
}

void FUEAgentStateStore::SetEditorContext(const FUEAgentContextSummary& InContext)
{
	EditorContext = InContext;
	EditorContext.SessionId = SessionId;
	BroadcastStateChanged();
}

const FUEAgentContextSummary& FUEAgentStateStore::GetEditorContext() const
{
	return EditorContext;
}

FUEAgentFunctionParameters& FUEAgentStateStore::EditFunctionParameters(const EUEAgentFunctionType FunctionType)
{
	return ParameterDrafts.FindOrAdd(FunctionType);
}

const FUEAgentFunctionParameters& FUEAgentStateStore::GetFunctionParameters(const EUEAgentFunctionType FunctionType) const
{
	if (const FUEAgentFunctionParameters* Parameters = ParameterDrafts.Find(FunctionType))
	{
		return *Parameters;
	}

	static const FUEAgentFunctionParameters EmptyParameters;
	return EmptyParameters;
}

const FString& FUEAgentStateStore::GetSessionId() const
{
	return SessionId;
}

FString FUEAgentStateStore::GetShortSessionId() const
{
	return SessionId.Left(8);
}

void FUEAgentStateStore::SetSessionId(const FString& InSessionId, const bool bBroadcast)
{
	const FString SanitizedSessionId = InSessionId.TrimStartAndEnd();
	if (SanitizedSessionId.IsEmpty() || SanitizedSessionId == SessionId)
	{
		return;
	}

	SessionId = SanitizedSessionId;
	EditorContext.SessionId = SessionId;
	PersistState();
	if (bBroadcast)
	{
		BroadcastStateChanged();
	}
}

bool FUEAgentStateStore::IsSessionSynchronized() const
{
	return bSessionSynchronized;
}

const FString& FUEAgentStateStore::GetSessionStatusText() const
{
	return SessionStatusText;
}

void FUEAgentStateStore::ResetSession()
{
	SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	EditorContext.SessionId = SessionId;
	ChatMessages.Reset();
	RecentTasks.Reset();
	LastResult = FUEAgentResultSnapshot();
	LastSessionSummaryJson = TEXT("{}");
	LastSessionHistoryJson = TEXT("{}");
	bSessionSynchronized = false;
	SessionStatusText = TEXT("Local session");
	StatusMessage = TEXT("Ready.");
	PersistState();
	BroadcastStateChanged();
}

void FUEAgentStateStore::SetBusy(const bool bInBusy, const FString& InStatusMessage)
{
	bBusy = bInBusy;
	if (!InStatusMessage.IsEmpty())
	{
		StatusMessage = InStatusMessage;
	}
	BroadcastStateChanged();
}

bool FUEAgentStateStore::IsBusy() const
{
	return bBusy;
}

void FUEAgentStateStore::SetStatusMessage(const FString& InStatusMessage)
{
	StatusMessage = InStatusMessage;
	BroadcastStateChanged();
}

const FString& FUEAgentStateStore::GetStatusMessage() const
{
	return StatusMessage;
}

bool FUEAgentStateStore::IsBackendOnline() const
{
	return bBackendOnline;
}

const FString& FUEAgentStateStore::GetBackendServiceStatus() const
{
	return BackendServiceStatus;
}

void FUEAgentStateStore::AppendUserMessage(const FString& InText, const EUEAgentFunctionType FunctionType)
{
	TSharedPtr<FUEAgentChatMessage> Message = MakeShared<FUEAgentChatMessage>();
	Message->MessageId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Message->SessionId = SessionId;
	Message->FunctionId = UEAgent::ToFunctionId(FunctionType);
	Message->Role = EUEAgentChatRole::User;
	Message->Title = TEXT("You");
	Message->Text = InText;
	ChatMessages.Add(Message);
	BroadcastStateChanged();
}

void FUEAgentStateStore::AppendAssistantMessage(const FString& InTitle, const FString& InText, const FString& InStatusHint, const FString& InTaskId, const FString& InFunctionId)
{
	TSharedPtr<FUEAgentChatMessage> Message = MakeShared<FUEAgentChatMessage>();
	Message->MessageId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Message->SessionId = SessionId;
	Message->TaskId = InTaskId;
	Message->FunctionId = InFunctionId.IsEmpty() ? UEAgent::ToFunctionId(ActiveFunction) : InFunctionId;
	Message->Role = EUEAgentChatRole::Agent;
	Message->Title = InTitle.IsEmpty() ? TEXT("UE Agent") : InTitle;
	Message->Text = InText;
	Message->StatusHint = InStatusHint;
	Message->bIncomplete = !LastResult.bOutputComplete;
	ChatMessages.Add(Message);
	BroadcastStateChanged();
}

void FUEAgentStateStore::AppendSystemMessage(const FString& InText, const FString& InTitle)
{
	TSharedPtr<FUEAgentChatMessage> Message = MakeShared<FUEAgentChatMessage>();
	Message->MessageId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Message->SessionId = SessionId;
	Message->FunctionId = UEAgent::ToFunctionId(ActiveFunction);
	Message->Role = EUEAgentChatRole::System;
	Message->Title = InTitle;
	Message->Text = InText;
	ChatMessages.Add(Message);
	BroadcastStateChanged();
}

const TArray<TSharedPtr<FUEAgentChatMessage>>& FUEAgentStateStore::GetChatMessages() const
{
	return ChatMessages;
}

void FUEAgentStateStore::SetLastRequestJson(const FString& InJson)
{
	LastResult.RawRequestJson = InJson;
	BroadcastStateChanged();
}

const FUEAgentResultSnapshot& FUEAgentStateStore::GetLastResult() const
{
	return LastResult;
}

FString FUEAgentStateStore::GetDebugSectionText(const EUEAgentDebugSection Section) const
{
	switch (Section)
	{
	case EUEAgentDebugSection::Overview:
		return LastResult.OverviewText;
	case EUEAgentDebugSection::RawRequest:
		return LastResult.RawRequestJson;
	case EUEAgentDebugSection::RawResponse:
		return LastResult.RawResponseJson;
	case EUEAgentDebugSection::UserProjection:
		return LastResult.UserProjectionJson;
	case EUEAgentDebugSection::DebugProjection:
		return LastResult.DebugProjectionJson;
	case EUEAgentDebugSection::IntentRoute:
		return LastResult.IntentRouteJson;
	case EUEAgentDebugSection::Skill:
		return LastResult.SkillJson;
	case EUEAgentDebugSection::Retrieval:
		return LastResult.RetrievalJson;
	case EUEAgentDebugSection::Tools:
		return LastResult.ToolsJson;
	case EUEAgentDebugSection::StepResults:
		return LastResult.StepResultsJson;
	case EUEAgentDebugSection::Proposal:
		return LastResult.ProposalJson;
	case EUEAgentDebugSection::Trace:
		return LastResult.TraceJson;
	case EUEAgentDebugSection::Monitor:
		return LastResult.MonitorJson;
	case EUEAgentDebugSection::Artifacts:
		return LastResult.ArtifactsJson;
	default:
		return TEXT("{}");
	}
}

const FString& FUEAgentStateStore::GetSettingsSnapshotJson() const
{
	return LastSettingsJson;
}

const FString& FUEAgentStateStore::GetMetricsSnapshotText() const
{
	return LastMetricsText;
}

const FString& FUEAgentStateStore::GetSessionSummaryJson() const
{
	return LastSessionSummaryJson;
}

const FString& FUEAgentStateStore::GetSessionHistoryJson() const
{
	return LastSessionHistoryJson;
}

void FUEAgentStateStore::ApplyHealthResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	LastHealthJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	EditorContext.BackendVersion = UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("version"), TEXT("unknown"));
	BackendServiceStatus = UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("service_status"), TEXT("offline"));
	bBackendOnline = BackendServiceStatus.Equals(TEXT("online"), ESearchCase::IgnoreCase)
		|| BackendServiceStatus.Equals(TEXT("ok"), ESearchCase::IgnoreCase)
		|| BackendServiceStatus.Equals(TEXT("healthy"), ESearchCase::IgnoreCase);
	StatusMessage = FString::Printf(TEXT("Backend %s"), *BackendServiceStatus);
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyBootstrapResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	LastBootstrapJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	EditorContext.BackendVersion = UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("version"), EditorContext.BackendVersion);

	const TSharedPtr<FJsonObject> DefaultProfile = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("default_profile"));
	ActiveProfileId = UEAgentStateStorePrivate::GetStringOrDefault(DefaultProfile, TEXT("profile_id"), ActiveProfileId);
	DefaultProfileId = ActiveProfileId;

	const TSharedPtr<FJsonObject> KnowledgeBaseSummary = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("knowledge_base_summary"));
	if (KnowledgeBaseSummary.IsValid())
	{
		const FString Status = UEAgentStateStorePrivate::GetStringOrDefault(KnowledgeBaseSummary, TEXT("status"), TEXT("ready"));
		const FString Documents = UEAgentStateStorePrivate::GetStringOrDefault(KnowledgeBaseSummary, TEXT("document_count"), TEXT(""));
		EditorContext.KnowledgeBaseStatus = Documents.IsEmpty() ? Status : FString::Printf(TEXT("%s (%s docs)"), *Status, *Documents);
	}

	StatusMessage = TEXT("Bootstrap refreshed.");
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyCapabilitiesResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	LastCapabilitiesJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	LastResult.SkillJson = UEAgentStateStorePrivate::BuildSkillDebugJson(
		UEAgentStateStorePrivate::ParseJsonObject(LastResult.SkillJson),
		nullptr,
		nullptr,
		nullptr,
		ResponseObject);
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyEditorOperationCapabilitiesResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	LastEditorOperationCapabilitiesJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplySettingsResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	const TSharedPtr<FJsonObject> SettingsObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("settings"));
	LastSettingsJson = UEAgentStateStorePrivate::JsonToPrettyString(SettingsObject.IsValid() ? SettingsObject : ResponseObject);
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyKnowledgeBaseStatus(const TSharedPtr<FJsonObject>& ResponseObject)
{
	LastKnowledgeBaseStatusJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	const TSharedPtr<FJsonObject> SummaryObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("summary"));
	if (SummaryObject.IsValid())
	{
		const FString Status = UEAgentStateStorePrivate::GetStringOrDefault(SummaryObject, TEXT("status"), TEXT("ready"));
		const FString Documents = UEAgentStateStorePrivate::GetStringOrDefault(SummaryObject, TEXT("document_count"), TEXT(""));
		EditorContext.KnowledgeBaseStatus = Documents.IsEmpty() ? Status : FString::Printf(TEXT("%s (%s docs)"), *Status, *Documents);
	}
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyRuntimeProfilesResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	RuntimeProfiles.Reset();
	ActiveProfileId = UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("active_profile_id"), ActiveProfileId);
	DefaultProfileId = UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("default_profile_id"), DefaultProfileId);

	for (const TSharedPtr<FJsonValue>& ProfileValue : UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("profiles")))
	{
		const TSharedPtr<FJsonObject> ProfileObject = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
		if (!ProfileObject.IsValid())
		{
			continue;
		}

		TSharedPtr<FUEAgentRuntimeProfile> Profile = MakeShared<FUEAgentRuntimeProfile>();
		Profile->ProfileId = UEAgentStateStorePrivate::GetStringOrDefault(ProfileObject, TEXT("profile_id"), TEXT("default"));
		Profile->Label = UEAgentStateStorePrivate::GetStringOrDefault(ProfileObject, TEXT("label"), Profile->ProfileId);
		Profile->Description = UEAgentStateStorePrivate::GetStringOrDefault(ProfileObject, TEXT("description"));
		Profile->bIsActive = Profile->ProfileId == ActiveProfileId;
		Profile->bIsDefault = Profile->ProfileId == DefaultProfileId;
		RuntimeProfiles.Add(Profile);
	}

	if (RuntimeProfiles.Num() == 0)
	{
		TSharedPtr<FUEAgentRuntimeProfile> FallbackProfile = MakeShared<FUEAgentRuntimeProfile>();
		FallbackProfile->ProfileId = ActiveProfileId;
		FallbackProfile->Label = ActiveProfileId;
		FallbackProfile->Description = TEXT("Default runtime profile.");
		FallbackProfile->bIsActive = true;
		FallbackProfile->bIsDefault = true;
		RuntimeProfiles.Add(FallbackProfile);
	}

	PersistState();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyRecentTasksResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	RecentTasks.Reset();
	for (const TSharedPtr<FJsonValue>& TaskValue : UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("items")))
	{
		const TSharedPtr<FJsonObject> TaskResponseObject = TaskValue.IsValid() ? TaskValue->AsObject() : nullptr;
		if (!TaskResponseObject.IsValid())
		{
			continue;
		}

		TSharedPtr<FUEAgentTaskSummary> TaskSummary = MakeShared<FUEAgentTaskSummary>();
		UEAgentStateStorePrivate::PopulateTaskSummary(TaskResponseObject, *TaskSummary);
		if (TaskSummary->TaskId.IsEmpty())
		{
			continue;
		}
		RecentTasks.Add(TaskSummary);
	}
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyProposalListResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	PendingProposals.Reset();
	for (const TSharedPtr<FJsonValue>& ProposalValue : UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("items")))
	{
		const TSharedPtr<FJsonObject> ProposalObject = ProposalValue.IsValid() ? ProposalValue->AsObject() : nullptr;
		if (!ProposalObject.IsValid())
		{
			continue;
		}

		TSharedPtr<FUEAgentProposalSummary> Proposal = MakeShared<FUEAgentProposalSummary>();
		UEAgentStateStorePrivate::PopulateProposalSummary(ProposalObject, *Proposal);
		PendingProposals.Add(Proposal);
	}
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplySessionSummaryResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	const TSharedPtr<FJsonObject> SessionObject = UEAgentStateStorePrivate::GetPreferredObject(ResponseObject, TEXT("session"));
	if (!SessionObject.IsValid())
	{
		return;
	}

	const FString BackendSessionId = UEAgentStateStorePrivate::GetStringOrDefault(SessionObject, TEXT("session_id"), SessionId);
	SetSessionId(BackendSessionId, false);
	LastSessionSummaryJson = UEAgentStateStorePrivate::JsonToPrettyString(SessionObject);
	bSessionSynchronized = true;
	SessionStatusText = TEXT("Synced with backend");
	PersistState();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplySessionHistoryResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	LastSessionHistoryJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);

	TArray<TSharedPtr<FJsonValue>> MessageValues = UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("items"));
	if (MessageValues.Num() == 0)
	{
		MessageValues = UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("messages"));
	}

	TArray<TSharedPtr<FUEAgentChatMessage>> RestoredMessages;
	RestoredMessages.Reserve(MessageValues.Num());
	for (const TSharedPtr<FJsonValue>& MessageValue : MessageValues)
	{
		const TSharedPtr<FJsonObject> MessageObject = MessageValue.IsValid() ? MessageValue->AsObject() : nullptr;
		if (!MessageObject.IsValid())
		{
			continue;
		}

		TSharedPtr<FUEAgentChatMessage> Message = MakeShared<FUEAgentChatMessage>();
		Message->MessageId = UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("message_id"),
			UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("id"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Message->SessionId = UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("session_id"), SessionId);
		Message->TaskId = UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("task_id"));
		Message->FunctionId = UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("function_id"),
			UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("task_type"),
				UEAgent::ToFunctionId(EUEAgentFunctionType::AgentChat)));
		Message->Role = UEAgentStateStorePrivate::ParseChatRole(UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("role"), TEXT("system")));
		Message->Title = UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("title"),
			Message->Role == EUEAgentChatRole::User ? TEXT("You") : (Message->Role == EUEAgentChatRole::Agent ? TEXT("UE Agent") : TEXT("System")));
		Message->Text = UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("text"),
			UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("content"),
				UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("message"))));
		Message->StatusHint = UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("status_hint"));
		Message->Timestamp = UEAgentStateStorePrivate::ParseTimestamp(UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("timestamp"),
			UEAgentStateStorePrivate::GetStringOrDefault(MessageObject, TEXT("created_at"))));

		if (!Message->Text.IsEmpty())
		{
			RestoredMessages.Add(Message);
		}
	}

	TArray<TSharedPtr<FUEAgentChatMessage>> PreservedMessages;
	PreservedMessages.Reserve(ChatMessages.Num());
	for (const TSharedPtr<FUEAgentChatMessage>& ExistingMessage : ChatMessages)
	{
		if (ExistingMessage.IsValid() && !UEAgentStateStorePrivate::IsUnifiedChatMessage(ExistingMessage))
		{
			PreservedMessages.Add(ExistingMessage);
		}
	}

	ChatMessages = MoveTemp(PreservedMessages);
	ChatMessages.Append(RestoredMessages);

	bSessionSynchronized = true;
	SessionStatusText = FString::Printf(TEXT("History restored (%d messages)"), RestoredMessages.Num());
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplySessionTasksResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	ApplyRecentTasksResponse(ResponseObject);
}

void FUEAgentStateStore::ApplyUserViewProjectionResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	const TSharedPtr<FJsonObject> UserViewObject = UEAgentStateStorePrivate::GetPreferredObject(ResponseObject, TEXT("user_view"));
	if (!UserViewObject.IsValid())
	{
		return;
	}

	const FString UiLanguage = GetEffectiveOutputLanguage();
	LastResult.UserProjectionJson = UEAgentStateStorePrivate::JsonToPrettyString(UserViewObject);
	LastResult.UserTitle = UEAgentStateStorePrivate::GetStringOrDefault(UserViewObject, TEXT("title"), LastResult.UserTitle);
	LastResult.UserText = UEAgentStateStorePrivate::GetStringOrDefault(UserViewObject, TEXT("text"), LastResult.UserText);
	LastResult.StatusHint = UEAgentStateStorePrivate::GetStringOrDefault(UserViewObject, TEXT("status_hint"), LastResult.StatusHint);

	LastResult.Blocks.Reset();
	for (const TSharedPtr<FJsonValue>& BlockValue : UEAgentStateStorePrivate::GetArrayField(UserViewObject, TEXT("blocks")))
	{
		const TSharedPtr<FJsonObject> BlockObject = BlockValue.IsValid() ? BlockValue->AsObject() : nullptr;
		if (!BlockObject.IsValid())
		{
			continue;
		}

		FUEAgentUserViewBlock Block;
		Block.BlockType = UEAgentStateStorePrivate::GetStringOrDefault(BlockObject, TEXT("block_type"), TEXT("summary"));
		Block.Title = UEAgentStateStorePrivate::GetStringOrDefault(BlockObject, TEXT("title"));
		Block.Text = UEAgentStateStorePrivate::GetStringOrDefault(BlockObject, TEXT("text"));
		const TSharedPtr<FJsonObject> DataObject = UEAgentStateStorePrivate::GetObjectField(BlockObject, TEXT("data"));
		Block.Items = UEAgentStateStorePrivate::ExtractBlockItems(DataObject, UiLanguage);
		Block.JsonPreview = DataObject.IsValid() ? UEAgentStateStorePrivate::JsonToPrettyString(DataObject) : FString();
		UEAgentStateStorePrivate::EnrichLlmAnalysisBlock(Block, DataObject, UiLanguage);
		UEAgentStateStorePrivate::EnrichEditorOperationResultBlock(Block, DataObject, UiLanguage);
		UEAgentStateStorePrivate::EnrichEditorOperationFollowUpsBlock(Block, DataObject, UiLanguage);
		LastResult.Blocks.Add(Block);
	}
	UEAgentStateStorePrivate::MoveBlockAfterSummary(LastResult.Blocks, TEXT("llm_analysis"));

	LastResult.Citations.Reset();
	for (const TSharedPtr<FJsonValue>& CitationValue : UEAgentStateStorePrivate::GetArrayField(UserViewObject, TEXT("citations_preview")))
	{
		const TSharedPtr<FJsonObject> CitationObject = CitationValue.IsValid() ? CitationValue->AsObject() : nullptr;
		if (!CitationObject.IsValid())
		{
			continue;
		}

		FUEAgentCitation Citation;
		Citation.Title = UEAgentStateStorePrivate::GetStringOrDefault(CitationObject, TEXT("title"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("来源"), TEXT("Source")));
		Citation.Source = UEAgentStateStorePrivate::GetStringOrDefault(CitationObject, TEXT("source"));
		Citation.Snippet = UEAgentStateStorePrivate::GetStringOrDefault(CitationObject, TEXT("snippet"));
		LastResult.Citations.Add(Citation);
	}

	LastResult.QuickActions.Reset();
	for (const TSharedPtr<FJsonValue>& QuickActionValue : UEAgentStateStorePrivate::GetArrayField(UserViewObject, TEXT("quick_actions")))
	{
		const TSharedPtr<FJsonObject> QuickActionObject = QuickActionValue.IsValid() ? QuickActionValue->AsObject() : nullptr;
		if (!QuickActionObject.IsValid())
		{
			continue;
		}

		FUEAgentQuickAction QuickAction;
		QuickAction.ActionId = UEAgentStateStorePrivate::GetStringOrDefault(QuickActionObject, TEXT("action_id"));
		QuickAction.Label = UEAgentStateStorePrivate::GetStringOrDefault(QuickActionObject, TEXT("label"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("下一步"), TEXT("Next Step")));
		const TSharedPtr<FJsonObject> PayloadObject = UEAgentStateStorePrivate::GetObjectField(QuickActionObject, TEXT("payload"));
		QuickAction.SuggestedInput = UEAgentStateStorePrivate::GetStringOrDefault(PayloadObject, TEXT("user_query"), QuickAction.Label);
		QuickAction.PayloadJson = PayloadObject.IsValid() ? UEAgentStateStorePrivate::JsonToPrettyString(PayloadObject) : FString();
		LastResult.QuickActions.Add(QuickAction);
	}

	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyDebugViewProjectionResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	const TSharedPtr<FJsonObject> DebugViewObject = UEAgentStateStorePrivate::GetPreferredObject(ResponseObject, TEXT("debug_view"));
	if (!DebugViewObject.IsValid())
	{
		return;
	}

	LastResult.DebugProjectionJson = UEAgentStateStorePrivate::JsonToPrettyString(DebugViewObject);
	{
		const TSharedPtr<FJsonObject> ExistingRequestObject = UEAgentStateStorePrivate::ParseJsonObject(LastResult.RawRequestJson);
		const TSharedPtr<FJsonObject> ExistingFrontendRequest = UEAgentStateStorePrivate::GetObjectField(ExistingRequestObject, TEXT("frontend_request"));

		TSharedPtr<FJsonObject> RawRequestCombined = MakeShared<FJsonObject>();
		RawRequestCombined->SetObjectField(TEXT("frontend_request"), UEAgentStateStorePrivate::ObjectOrEmpty(ExistingFrontendRequest.IsValid() ? ExistingFrontendRequest : ExistingRequestObject));
		RawRequestCombined->SetObjectField(TEXT("backend_raw_request"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugViewObject, TEXT("raw_request"))));
		RawRequestCombined->SetObjectField(TEXT("normalized_request"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugViewObject, TEXT("normalized_request"))));
		LastResult.RawRequestJson = UEAgentStateStorePrivate::JsonToPrettyString(RawRequestCombined);
	}
	{
		TSharedPtr<FJsonObject> RetrievalCombined = MakeShared<FJsonObject>();
		RetrievalCombined->SetObjectField(TEXT("retrieval"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugViewObject, TEXT("retrieval"))));
		RetrievalCombined->SetObjectField(TEXT("retrieval_summary"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugViewObject, TEXT("retrieval_summary"))));
		RetrievalCombined->SetObjectField(TEXT("retrieval_quality_gate"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugViewObject, TEXT("retrieval_quality_gate"))));
		LastResult.RetrievalJson = UEAgentStateStorePrivate::JsonToPrettyString(UEAgentStateStorePrivate::MergeObjects(
			RetrievalCombined,
			UEAgentStateStorePrivate::ParseJsonObject(LastResult.RetrievalJson)));
	}

	{
		TSharedPtr<FJsonObject> ToolsCombined = MakeShared<FJsonObject>();
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("tools"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("react_loop"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("project_file"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("tool_contracts"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("self_reflection"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("active_context"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("tool_registry_protocol"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("tool_execution_policy"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugViewObject, TEXT("side_effects"));
		LastResult.ToolsJson = UEAgentStateStorePrivate::JsonToPrettyString(UEAgentStateStorePrivate::MergeObjects(
			ToolsCombined,
			UEAgentStateStorePrivate::ParseJsonObject(LastResult.ToolsJson)));
	}
	LastResult.StepResultsJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugViewObject, TEXT("step_results"));
	LastResult.MetricsJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugViewObject, TEXT("metrics"));
	LastResult.SessionSummaryJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugViewObject, TEXT("session_summary"));
	LastResult.MemorySummaryJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugViewObject, TEXT("memory_summary"));
	LastResult.SkillJson = UEAgentStateStorePrivate::BuildSkillDebugJson(
		UEAgentStateStorePrivate::ParseJsonObject(LastResult.SkillJson),
		DebugViewObject,
		nullptr,
		nullptr,
		UEAgentStateStorePrivate::ParseJsonObject(LastCapabilitiesJson));
	LastResult.bOutputComplete = UEAgentStateStorePrivate::GetBoolOrDefault(DebugViewObject, TEXT("output_complete"), LastResult.bOutputComplete);
	LastResult.FinishReason = UEAgentStateStorePrivate::GetStringOrDefault(DebugViewObject, TEXT("finish_reason"), LastResult.FinishReason);

	TSharedPtr<FJsonObject> IntentRouteCombined = UEAgentStateStorePrivate::ParseJsonObject(LastResult.IntentRouteJson);
	if (!IntentRouteCombined.IsValid())
	{
		IntentRouteCombined = MakeShared<FJsonObject>();
	}
	IntentRouteCombined->SetObjectField(TEXT("route"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugViewObject, TEXT("route"))));
	UEAgentStateStorePrivate::CopyJsonFieldIfPresent(IntentRouteCombined, DebugViewObject, TEXT("react_loop"));
	UEAgentStateStorePrivate::CopyJsonFieldIfPresent(IntentRouteCombined, DebugViewObject, TEXT("active_context"));
	LastResult.IntentRouteJson = UEAgentStateStorePrivate::JsonToPrettyString(IntentRouteCombined);

	LastResult.TraceLinks.Reset();
	for (const TSharedPtr<FJsonValue>& TraceLinkValue : UEAgentStateStorePrivate::GetArrayField(DebugViewObject, TEXT("trace_links")))
	{
		const TSharedPtr<FJsonObject> TraceLinkObject = TraceLinkValue.IsValid() ? TraceLinkValue->AsObject() : nullptr;
		if (!TraceLinkObject.IsValid())
		{
			continue;
		}

		FUEAgentTraceLink TraceLink;
		TraceLink.Label = UEAgentStateStorePrivate::GetTraceLinkLabel(TraceLinkObject);
		TraceLink.Url = UEAgentStateStorePrivate::GetTraceLinkUrl(TraceLinkObject);
		TraceLink.RawJson = UEAgentStateStorePrivate::JsonToPrettyString(TraceLinkObject);
		LastResult.TraceLinks.Add(TraceLink);
	}

	LastResult.Artifacts.Reset();
	for (const TSharedPtr<FJsonValue>& ArtifactValue : UEAgentStateStorePrivate::GetArrayField(DebugViewObject, TEXT("artifacts")))
	{
		const TSharedPtr<FJsonObject> ArtifactObject = ArtifactValue.IsValid() ? ArtifactValue->AsObject() : nullptr;
		if (!ArtifactObject.IsValid())
		{
			continue;
		}

		UEAgentStateStorePrivate::UpsertArtifact(LastResult.Artifacts, UEAgentStateStorePrivate::ParseArtifactItem(ArtifactObject));
	}

	LastResult.Warnings.Reset();
	for (const TSharedPtr<FJsonValue>& WarningValue : UEAgentStateStorePrivate::GetArrayField(DebugViewObject, TEXT("warnings")))
	{
		if (WarningValue.IsValid())
		{
			LastResult.Warnings.Add(WarningValue->AsString());
		}
	}

	RebuildMonitorJson();
	RebuildArtifactsJson();
	RebuildTraceJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyUnifiedResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	const FString PreviousRequestJson = LastResult.RawRequestJson;
	const TSharedPtr<FJsonObject> FrontendRequestObject = UEAgentStateStorePrivate::ParseJsonObject(PreviousRequestJson);
	LastResult = FUEAgentResultSnapshot();
	LastResult.RawRequestJson = PreviousRequestJson;
	LastResult.RawResponseJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	LastResult.UserProjectionJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("user_view"));
	LastResult.DebugProjectionJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("debug_view"));
	LastResult.ProposalJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("action_proposals"));
	LastResult.TraceJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("trace_summary"));
	LastResult.EventsJson = TEXT("[]");

	const TSharedPtr<FJsonObject> TaskObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("task"));
	LastResult.TaskId = UEAgentStateStorePrivate::GetStringOrDefault(TaskObject, TEXT("task_id"));
	LastResult.RunId = UEAgentStateStorePrivate::GetStringOrDefault(TaskObject, TEXT("run_id"));
	LastResult.TaskStatus = UEAgentStateStorePrivate::GetStringOrDefault(TaskObject, TEXT("status"));
	LastResult.FinishReason = UEAgentStateStorePrivate::GetStringOrDefault(TaskObject, TEXT("finish_reason"));
	LastResult.bOutputComplete = UEAgentStateStorePrivate::GetBoolOrDefault(TaskObject, TEXT("output_complete"), true);
	const FString ResponseTaskType = UEAgentStateStorePrivate::GetStringOrDefault(TaskObject, TEXT("task_type"),
		UEAgentStateStorePrivate::GetStringOrDefault(TaskObject, TEXT("type"), UEAgent::ToFunctionId(ActiveFunction)));

	const TSharedPtr<FJsonObject> IntentObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("intent"));
	LastResult.IntentType = UEAgentStateStorePrivate::GetStringOrDefault(IntentObject, TEXT("intent_type"));
	LastResult.RouteType = UEAgentStateStorePrivate::GetStringOrDefault(IntentObject, TEXT("route_type"));

	const TSharedPtr<FJsonObject> UserView = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("user_view"));
	const TSharedPtr<FJsonObject> Presentation = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("presentation"));
	const TSharedPtr<FJsonObject> LocaleObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("locale"));
	const FString UiLanguage = UEAgentStateStorePrivate::ResolveEffectiveOutputLanguage(LocaleObject, PreferredOutputLanguage);
	const FString DefaultUserText = UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("请求已完成，但后端没有返回 user_view 展示内容。"), TEXT("Request completed, but the backend did not return any user_view content."));
	LastResult.UserTitle = UEAgentStateStorePrivate::GetStringOrDefault(UserView, TEXT("title"),
		UEAgentStateStorePrivate::GetStringOrDefault(Presentation, TEXT("user_title"), UEAgent::ToFunctionLabel(ActiveFunction).ToString()));
	LastResult.UserText = UEAgentStateStorePrivate::GetStringOrDefault(UserView, TEXT("text"),
		UEAgentStateStorePrivate::GetStringOrDefault(Presentation, TEXT("user_text"),
			UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("assistant_message"), DefaultUserText)));
	LastResult.AssistantMessage = UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("assistant_message"), LastResult.UserText);
	LastResult.StatusHint = UEAgentStateStorePrivate::GetStringOrDefault(UserView, TEXT("status_hint"));
	const bool bReviewReadErrorHint = LastResult.StatusHint.Equals(TEXT("read_error"), ESearchCase::IgnoreCase);
	const TSharedPtr<FJsonObject> ResponseDataObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("data"));
	FString ReviewScopeError;
	LastResult.Blocks.Reset();
	for (const TSharedPtr<FJsonValue>& BlockValue : UEAgentStateStorePrivate::GetArrayField(UserView, TEXT("blocks")))
	{
		const TSharedPtr<FJsonObject> BlockObject = BlockValue.IsValid() ? BlockValue->AsObject() : nullptr;
		if (!BlockObject.IsValid())
		{
			continue;
		}

		FUEAgentUserViewBlock Block;
		Block.BlockType = UEAgentStateStorePrivate::GetStringOrDefault(BlockObject, TEXT("block_type"), TEXT("summary"));
		Block.Title = UEAgentStateStorePrivate::GetStringOrDefault(BlockObject, TEXT("title"));
		Block.Text = UEAgentStateStorePrivate::GetStringOrDefault(BlockObject, TEXT("text"));
		const TSharedPtr<FJsonObject> BlockDataObject = UEAgentStateStorePrivate::GetObjectField(BlockObject, TEXT("data"));
		Block.Items = UEAgentStateStorePrivate::ExtractBlockItems(BlockDataObject, UiLanguage);
		Block.JsonPreview = BlockDataObject.IsValid() ? UEAgentStateStorePrivate::JsonToPrettyString(BlockDataObject) : FString();
		UEAgentStateStorePrivate::EnrichLlmAnalysisBlock(Block, BlockDataObject, UiLanguage);
		UEAgentStateStorePrivate::EnrichEditorOperationResultBlock(Block, BlockDataObject, UiLanguage);
		UEAgentStateStorePrivate::EnrichEditorOperationFollowUpsBlock(Block, BlockDataObject, UiLanguage);
		LastResult.Blocks.Add(Block);
	}

	const TSharedPtr<FJsonObject> LocalizedReviewObject = UEAgentStateStorePrivate::GetObjectField(ResponseDataObject, TEXT("localized_review"));
	if (LastResult.Blocks.Num() == 0 && LocalizedReviewObject.IsValid())
	{
		UEAgentStateStorePrivate::AddLocalizedReviewBlockFromField(LastResult.Blocks, LocalizedReviewObject, TEXT("summary"), TEXT("summary"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("摘要"), TEXT("Summary")), UiLanguage);
		UEAgentStateStorePrivate::AddLocalizedReviewBlockFromField(LastResult.Blocks, LocalizedReviewObject, TEXT("issues"), TEXT("issues"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("问题"), TEXT("Issues")), UiLanguage);
		UEAgentStateStorePrivate::AddLocalizedReviewBlockFromField(LastResult.Blocks, LocalizedReviewObject, TEXT("recommendations"), TEXT("recommendations"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("建议"), TEXT("Recommendations")), UiLanguage);
		UEAgentStateStorePrivate::AddLocalizedReviewBlockFromField(LastResult.Blocks, LocalizedReviewObject, TEXT("references"), TEXT("references"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("参考"), TEXT("References")), UiLanguage);
		UEAgentStateStorePrivate::AddLocalizedReviewBlockFromField(LastResult.Blocks, LocalizedReviewObject, TEXT("next_steps"), TEXT("next_steps"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("下一步"), TEXT("Next Steps")), UiLanguage);
	}
	UEAgentStateStorePrivate::AddLlmAnalysisBlockIfMissing(LastResult.Blocks, ResponseDataObject, UiLanguage);
	UEAgentStateStorePrivate::AddInventoryMatchBlockIfMissing(LastResult.Blocks, ResponseDataObject, TEXT("project_inventory"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("项目资产匹配"), TEXT("Project Inventory Matches")), UiLanguage);
	UEAgentStateStorePrivate::AddInventoryMatchBlockIfMissing(LastResult.Blocks, ResponseDataObject, TEXT("inventory"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("项目资产匹配"), TEXT("Project Inventory Matches")), UiLanguage);

	const TArray<TSharedPtr<FJsonValue>> GeneratedItemValues = UEAgentStateStorePrivate::GetArrayField(ResponseDataObject, TEXT("generated_items"));
	if (GeneratedItemValues.Num() > 0 && UEAgentStateStorePrivate::FindBlockIndex(LastResult.Blocks, TEXT("generated_items")) == INDEX_NONE)
	{
		FUEAgentUserViewBlock GeneratedItemsBlock;
		GeneratedItemsBlock.BlockType = TEXT("generated_items");
		GeneratedItemsBlock.Title = UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("代码草稿"), TEXT("Code Drafts"));
		GeneratedItemsBlock.Text = FString::Printf(
			TEXT("%s%d%s"),
			*UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("生成了 "), TEXT("Generated ")),
			GeneratedItemValues.Num(),
			*UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT(" 个虚拟草稿，尚未写入工程。"), TEXT(" virtual draft(s), not written to the project.")));
		for (const TSharedPtr<FJsonValue>& ItemValue : GeneratedItemValues)
		{
			const FString ItemSummary = UEAgentStateStorePrivate::BuildBlockItemSummary(ItemValue, UiLanguage);
			if (!ItemSummary.IsEmpty())
			{
				GeneratedItemsBlock.Items.Add(ItemSummary);
			}
		}
		GeneratedItemsBlock.JsonPreview = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseDataObject, TEXT("generated_items"));
		LastResult.Blocks.Add(GeneratedItemsBlock);
	}

	if (ResponseDataObject.IsValid()
		&& (ResponseDataObject->HasField(TEXT("reference_lookup")) || ResponseDataObject->HasField(TEXT("generation_mode")) || ResponseDataObject->HasField(TEXT("retrieved_references"))))
	{
		TSharedPtr<FJsonObject> GenerationContextObject = MakeShared<FJsonObject>();
		for (const FString& FieldName : { FString(TEXT("generation_mode")), FString(TEXT("reference_lookup")), FString(TEXT("retrieved_references")) })
		{
			const TSharedPtr<FJsonValue> FieldValue = ResponseDataObject->TryGetField(FieldName);
			if (FieldValue.IsValid())
			{
				GenerationContextObject->SetField(FieldName, FieldValue);
			}
		}

		FUEAgentUserViewBlock GenerationContextBlock;
		GenerationContextBlock.BlockType = TEXT("generation_context");
		GenerationContextBlock.Title = UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("生成上下文"), TEXT("Generation Context"));
		GenerationContextBlock.JsonPreview = UEAgentStateStorePrivate::JsonToPrettyString(GenerationContextObject);
		LastResult.Blocks.Add(GenerationContextBlock);
	}

	LastResult.Citations.Reset();
	for (const TSharedPtr<FJsonValue>& CitationValue : UEAgentStateStorePrivate::GetArrayField(UserView, TEXT("citations_preview")))
	{
		const TSharedPtr<FJsonObject> CitationObject = CitationValue.IsValid() ? CitationValue->AsObject() : nullptr;
		if (!CitationObject.IsValid())
		{
			continue;
		}

		FUEAgentCitation Citation;
		Citation.Title = UEAgentStateStorePrivate::GetStringOrDefault(CitationObject, TEXT("title"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("来源"), TEXT("Source")));
		Citation.Source = UEAgentStateStorePrivate::GetStringOrDefault(CitationObject, TEXT("source"));
		Citation.Snippet = UEAgentStateStorePrivate::GetStringOrDefault(CitationObject, TEXT("snippet"));
		LastResult.Citations.Add(Citation);
	}

	LastResult.QuickActions.Reset();
	for (const TSharedPtr<FJsonValue>& QuickActionValue : UEAgentStateStorePrivate::GetArrayField(UserView, TEXT("quick_actions")))
	{
		const TSharedPtr<FJsonObject> QuickActionObject = QuickActionValue.IsValid() ? QuickActionValue->AsObject() : nullptr;
		if (!QuickActionObject.IsValid())
		{
			continue;
		}

		FUEAgentQuickAction QuickAction;
		QuickAction.ActionId = UEAgentStateStorePrivate::GetStringOrDefault(QuickActionObject, TEXT("action_id"));
		QuickAction.Label = UEAgentStateStorePrivate::GetStringOrDefault(QuickActionObject, TEXT("label"), UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("下一步"), TEXT("Next Step")));
		const TSharedPtr<FJsonObject> PayloadObject = UEAgentStateStorePrivate::GetObjectField(QuickActionObject, TEXT("payload"));
		QuickAction.SuggestedInput = UEAgentStateStorePrivate::GetStringOrDefault(PayloadObject, TEXT("user_query"), QuickAction.Label);
		QuickAction.PayloadJson = PayloadObject.IsValid() ? UEAgentStateStorePrivate::JsonToPrettyString(PayloadObject) : FString();
		LastResult.QuickActions.Add(QuickAction);
	}

	const TSharedPtr<FJsonObject> DebugView = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("debug_view"));
	{
		TSharedPtr<FJsonObject> ReviewScopeObject = UEAgentStateStorePrivate::GetObjectField(ResponseDataObject, TEXT("review_scope"));
		if (!ReviewScopeObject.IsValid())
		{
			const TSharedPtr<FJsonObject> RawResultObject = UEAgentStateStorePrivate::GetObjectField(DebugView, TEXT("raw_result"));
			ReviewScopeObject = UEAgentStateStorePrivate::GetObjectField(RawResultObject, TEXT("review_scope"));
		}

		if (ReviewScopeObject.IsValid())
		{
			const FString ReadStatus = UEAgentStateStorePrivate::GetStringOrDefault(ReviewScopeObject, TEXT("read_status"));
			const FString LoadError = UEAgentStateStorePrivate::GetStringOrDefault(ReviewScopeObject, TEXT("load_error"),
				UEAgentStateStorePrivate::GetStringOrDefault(ReviewScopeObject, TEXT("error")));

			FUEAgentUserViewBlock ReviewScopeBlock;
			ReviewScopeBlock.BlockType = TEXT("review_scope");
			ReviewScopeBlock.Title = UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("文件读取范围"), TEXT("File Read Scope"));
			TArray<FString> ReviewScopeItems;
			const FString ResolvedPath = UEAgentStateStorePrivate::GetStringOrDefault(ReviewScopeObject, TEXT("resolved_absolute_path"));
			const int32 ContentLength = UEAgentStateStorePrivate::GetIntOrDefault(ReviewScopeObject, TEXT("content_length"));
			const FString AppliedFocus = UEAgentStateStorePrivate::GetStringOrDefault(ReviewScopeObject, TEXT("applied_focus"));
			if (!ResolvedPath.IsEmpty())
			{
				ReviewScopeItems.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("读取路径："), TEXT("Resolved Path: ")), *ResolvedPath));
			}
			if (!ReadStatus.IsEmpty())
			{
				ReviewScopeItems.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("读取状态："), TEXT("Read Status: ")), *UEAgentStateStorePrivate::LocalizeStatusLabel(ReadStatus, UiLanguage)));
			}
			if (ContentLength != INDEX_NONE)
			{
				ReviewScopeItems.Add(FString::Printf(TEXT("%s%d"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("内容长度："), TEXT("Content Length: ")), ContentLength));
			}
			if (!AppliedFocus.IsEmpty())
			{
				ReviewScopeItems.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("审查重点："), TEXT("Applied Focus: ")), *AppliedFocus));
			}
			if (!LoadError.IsEmpty())
			{
				ReviewScopeItems.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("读取错误："), TEXT("Read Error: ")), *LoadError));
			}
			ReviewScopeBlock.Items = ReviewScopeItems;
			ReviewScopeBlock.JsonPreview = UEAgentStateStorePrivate::JsonToPrettyString(ReviewScopeObject);
			LastResult.Blocks.Add(ReviewScopeBlock);

			if (ReadStatus.Equals(TEXT("error"), ESearchCase::IgnoreCase) || !LoadError.IsEmpty() || bReviewReadErrorHint)
			{
				ReviewScopeError = LoadError.IsEmpty()
					? UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("代码审查读取文件失败。"), TEXT("Code review could not read the selected file."))
					: FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("代码审查读取文件失败："), TEXT("Code review could not read the selected file: ")), *LoadError);
			}
		}
	}
	if (ReviewScopeError.IsEmpty() && bReviewReadErrorHint)
	{
		ReviewScopeError = UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("代码审查读取文件失败。"), TEXT("Code review could not read the selected file."));
	}
	{
		TSharedPtr<FJsonObject> RawRequestCombined = MakeShared<FJsonObject>();
		RawRequestCombined->SetObjectField(TEXT("frontend_request"), UEAgentStateStorePrivate::ObjectOrEmpty(FrontendRequestObject));
		RawRequestCombined->SetObjectField(TEXT("backend_raw_request"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugView, TEXT("raw_request"))));
		RawRequestCombined->SetObjectField(TEXT("normalized_request"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugView, TEXT("normalized_request"))));
		LastResult.RawRequestJson = UEAgentStateStorePrivate::JsonToPrettyString(RawRequestCombined);
	}
	{
		TSharedPtr<FJsonObject> RetrievalCombined = MakeShared<FJsonObject>();
		RetrievalCombined->SetObjectField(TEXT("retrieval_trace"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("retrieval_trace"))));
		RetrievalCombined->SetObjectField(TEXT("retrieval"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugView, TEXT("retrieval"))));
		RetrievalCombined->SetObjectField(TEXT("retrieval_summary"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugView, TEXT("retrieval_summary"))));
		RetrievalCombined->SetObjectField(TEXT("retrieval_quality_gate"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(ResponseDataObject, TEXT("retrieval_quality_gate"))));
		LastResult.RetrievalJson = UEAgentStateStorePrivate::JsonToPrettyString(RetrievalCombined);
	}
	{
		TSharedPtr<FJsonObject> ToolsCombined = MakeShared<FJsonObject>();
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("tools"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("react_loop"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("project_file"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("tool_contracts"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("self_reflection"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("active_context"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("tool_registry_protocol"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("tool_execution_policy"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, DebugView, TEXT("side_effects"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("react_loop"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("project_file"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("tool_plan"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("tool_contracts"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("self_reflection"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("tool_registry_protocol"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("tool_execution_policy"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("side_effects"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(ToolsCombined, ResponseDataObject, TEXT("code_write_result"));
		LastResult.ToolsJson = UEAgentStateStorePrivate::JsonToPrettyString(ToolsCombined);
	}
	LastResult.StepResultsJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugView, TEXT("step_results"));
	LastResult.UsageJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("usage"));
	LastResult.LocaleJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("locale"));
	LastResult.MetricsJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugView, TEXT("metrics"));
	LastResult.SessionSummaryJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugView, TEXT("session_summary"));
	LastResult.MemorySummaryJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(DebugView, TEXT("memory_summary"));
	{
		TSharedPtr<FJsonObject> MemorySummaryObject = UEAgentStateStorePrivate::ParseJsonObject(LastResult.MemorySummaryJson);
		if (!MemorySummaryObject.IsValid())
		{
			MemorySummaryObject = MakeShared<FJsonObject>();
		}
		const TSharedPtr<FJsonObject> ContextBundleObject = UEAgentStateStorePrivate::GetObjectField(ResponseDataObject, TEXT("context_bundle"));
		UEAgentStateStorePrivate::CopyJsonFieldIfPresent(MemorySummaryObject, ContextBundleObject, TEXT("long_term_memory"));
		LastResult.MemorySummaryJson = UEAgentStateStorePrivate::JsonToPrettyString(MemorySummaryObject);
	}
	LastResult.ApprovalResultJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseDataObject, TEXT("approval_result"));
	LastResult.SkillJson = UEAgentStateStorePrivate::BuildSkillDebugJson(
		nullptr,
		DebugView,
		ResponseDataObject,
		UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("trace_summary")),
		UEAgentStateStorePrivate::ParseJsonObject(LastCapabilitiesJson));
	LastResult.TraceLinks.Reset();
	for (const TSharedPtr<FJsonValue>& TraceLinkValue : UEAgentStateStorePrivate::GetArrayField(DebugView, TEXT("trace_links")))
	{
		const TSharedPtr<FJsonObject> TraceLinkObject = TraceLinkValue.IsValid() ? TraceLinkValue->AsObject() : nullptr;
		if (!TraceLinkObject.IsValid())
		{
			continue;
		}

		FUEAgentTraceLink TraceLink;
		TraceLink.Label = UEAgentStateStorePrivate::GetTraceLinkLabel(TraceLinkObject);
		TraceLink.Url = UEAgentStateStorePrivate::GetTraceLinkUrl(TraceLinkObject);
		TraceLink.RawJson = UEAgentStateStorePrivate::JsonToPrettyString(TraceLinkObject);
		LastResult.TraceLinks.Add(TraceLink);
	}

	TSharedPtr<FJsonObject> IntentRouteCombined = MakeShared<FJsonObject>();
	IntentRouteCombined->SetObjectField(TEXT("intent"), UEAgentStateStorePrivate::ObjectOrEmpty(IntentObject));
	IntentRouteCombined->SetObjectField(TEXT("route"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::GetObjectField(DebugView, TEXT("route"))));
	UEAgentStateStorePrivate::CopyJsonFieldIfPresent(IntentRouteCombined, DebugView, TEXT("react_loop"));
	UEAgentStateStorePrivate::CopyJsonFieldIfPresent(IntentRouteCombined, DebugView, TEXT("active_context"));
	UEAgentStateStorePrivate::CopyJsonFieldIfPresent(IntentRouteCombined, ResponseDataObject, TEXT("tool_plan"));
	LastResult.IntentRouteJson = UEAgentStateStorePrivate::JsonToPrettyString(IntentRouteCombined);
	LastResult.Artifacts.Reset();
	for (const TSharedPtr<FJsonValue>& ArtifactValue : UEAgentStateStorePrivate::GetArrayField(DebugView, TEXT("artifacts")))
	{
		const TSharedPtr<FJsonObject> ArtifactObject = ArtifactValue.IsValid() ? ArtifactValue->AsObject() : nullptr;
		if (!ArtifactObject.IsValid())
		{
			continue;
		}

		UEAgentStateStorePrivate::UpsertArtifact(LastResult.Artifacts, UEAgentStateStorePrivate::ParseArtifactItem(ArtifactObject));
	}
	RebuildMonitorJson();
	RebuildArtifactsJson();

	TArray<FString> OverviewLines;
	OverviewLines.Add(FString::Printf(TEXT("Task: %s"), *LastResult.TaskId));
	OverviewLines.Add(FString::Printf(TEXT("Run: %s"), *LastResult.RunId));
	OverviewLines.Add(FString::Printf(TEXT("Status: %s"), *LastResult.TaskStatus));
	OverviewLines.Add(FString::Printf(TEXT("Finish Reason: %s"), *LastResult.FinishReason));
	OverviewLines.Add(FString::Printf(TEXT("Intent: %s"), *LastResult.IntentType));
	OverviewLines.Add(FString::Printf(TEXT("Route: %s"), *LastResult.RouteType));
	if (!LastResult.StatusHint.IsEmpty())
	{
		OverviewLines.Add(FString::Printf(TEXT("Hint: %s"), *LastResult.StatusHint));
	}
	const TSharedPtr<FJsonObject> LlmReviewObject = UEAgentStateStorePrivate::GetObjectField(ResponseDataObject, TEXT("llm_review"));
	if (LlmReviewObject.IsValid() && LlmReviewObject->HasField(TEXT("ok")))
	{
		const bool bLlmReviewOk = UEAgentStateStorePrivate::GetBoolOrDefault(LlmReviewObject, TEXT("ok"), false);
		const FString SkipReason = UEAgentStateStorePrivate::GetStringOrDefault(LlmReviewObject, TEXT("skip_reason"),
			UEAgentStateStorePrivate::GetStringOrDefault(LlmReviewObject, TEXT("reason")));
		OverviewLines.Add(SkipReason.IsEmpty()
			? FString::Printf(TEXT("LLM 审查：%s"), bLlmReviewOk ? TEXT("已完成") : TEXT("未执行"))
			: FString::Printf(TEXT("LLM 审查：%s（%s）"), bLlmReviewOk ? TEXT("已完成") : TEXT("未执行"), *SkipReason));
	}
	LastResult.OverviewText = FString::Join(OverviewLines, TEXT("\n"));

	LastResult.Proposals.Reset();
	for (const TSharedPtr<FJsonValue>& ProposalValue : UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("action_proposals")))
	{
		const TSharedPtr<FJsonObject> ProposalObject = ProposalValue.IsValid() ? ProposalValue->AsObject() : nullptr;
		if (!ProposalObject.IsValid())
		{
			continue;
		}

		TSharedPtr<FUEAgentProposalSummary> Proposal = MakeShared<FUEAgentProposalSummary>();
		UEAgentStateStorePrivate::PopulateProposalSummary(ProposalObject, *Proposal);
		LastResult.Proposals.Add(Proposal);
	}

	LastResult.Warnings.Reset();
	for (const TSharedPtr<FJsonValue>& WarningValue : UEAgentStateStorePrivate::GetArrayField(DebugView, TEXT("warnings")))
	{
		if (WarningValue.IsValid())
		{
			LastResult.Warnings.Add(WarningValue->AsString());
		}
	}

	LastResult.Errors.Reset();
	if (!ReviewScopeError.IsEmpty())
	{
		LastResult.Errors.Add(ReviewScopeError);
	}
	for (const TSharedPtr<FJsonValue>& ErrorValue : UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("errors")))
	{
		const TSharedPtr<FJsonObject> ErrorObject = ErrorValue.IsValid() ? ErrorValue->AsObject() : nullptr;
		if (!ErrorObject.IsValid())
		{
			continue;
		}
		LastResult.Errors.Add(FString::Printf(TEXT("[%s] %s"),
			*UEAgentStateStorePrivate::GetStringOrDefault(ErrorObject, TEXT("code"), TEXT("error")),
			*UEAgentStateStorePrivate::GetStringOrDefault(ErrorObject, TEXT("message"), TEXT("Unknown error"))));
	}

	const bool bResponseIsChat = ResponseTaskType == UEAgent::ToFunctionId(EUEAgentFunctionType::AgentChat)
		|| ResponseTaskType == UEAgent::ToFunctionId(EUEAgentFunctionType::ProjectQA);
	if (bResponseIsChat)
	{
		const FString ChatMessageText = LastResult.AssistantMessage.IsEmpty() ? LastResult.UserText : LastResult.AssistantMessage;
		AppendAssistantMessage(LastResult.UserTitle, ChatMessageText, LastResult.StatusHint, LastResult.TaskId, ResponseTaskType);
	}
	AddOrUpdateRecentTask(LastResult);
	StatusMessage = !ReviewScopeError.IsEmpty()
		? ReviewScopeError
		: (LastResult.TaskStatus.IsEmpty() ? UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("请求已完成。"), TEXT("Request completed.")) : FString::Printf(TEXT("Task %s"), *LastResult.TaskStatus));
	RebuildTraceJson();
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyTaskTraceResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	if (!ResponseObject.IsValid())
	{
		return;
	}

	TArray<FUEAgentRunEvent> ParsedEvents;
	for (const TSharedPtr<FJsonValue>& EventValue : UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("events")))
	{
		const TSharedPtr<FJsonObject> EventObject = EventValue.IsValid() ? EventValue->AsObject() : nullptr;
		if (!EventObject.IsValid())
		{
			continue;
		}

		ParsedEvents.Add(UEAgentStateStorePrivate::ParseRunEventObject(EventObject));
	}

	if (ParsedEvents.Num() > 0)
	{
		LastResult.Events = ParsedEvents;
		LastResult.EventsJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("events"));
	}

	const FString TraceSummaryJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("trace_summary"));
	if (!TraceSummaryJson.IsEmpty() && TraceSummaryJson != TEXT("{}"))
	{
		LastResult.TraceJson = TraceSummaryJson;
		LastResult.SkillJson = UEAgentStateStorePrivate::BuildSkillDebugJson(
			UEAgentStateStorePrivate::ParseJsonObject(LastResult.SkillJson),
			nullptr,
			nullptr,
			UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("trace_summary")),
			UEAgentStateStorePrivate::ParseJsonObject(LastCapabilitiesJson));
	}

	const FString StepResultsJson = UEAgentStateStorePrivate::JsonFieldToPrettyString(ResponseObject, TEXT("step_results"));
	if (!StepResultsJson.IsEmpty() && StepResultsJson != TEXT("{}"))
	{
		LastResult.StepResultsJson = StepResultsJson;
	}

	RebuildTraceJson();
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyTaskArtifactsResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	if (!ResponseObject.IsValid())
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& ArtifactValue : UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("items")))
	{
		const TSharedPtr<FJsonObject> ArtifactObject = ArtifactValue.IsValid() ? ArtifactValue->AsObject() : nullptr;
		if (!ArtifactObject.IsValid())
		{
			continue;
		}

		UEAgentStateStorePrivate::UpsertArtifact(LastResult.Artifacts, UEAgentStateStorePrivate::ParseArtifactItem(ArtifactObject));
	}

	RebuildArtifactsJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyAlertsResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	LastAlertsJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyMetricsResponse(const FString& ResponseText)
{
	LastMetricsText = ResponseText;
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyCodeReviewFilesResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	TArray<TSharedPtr<FJsonValue>> FileValues = UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("items"));
	if (FileValues.Num() == 0)
	{
		FileValues = UEAgentStateStorePrivate::GetArrayField(ResponseObject, TEXT("files"));
	}

	const TSharedPtr<FJsonObject> DataObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("data"));
	if (FileValues.Num() == 0 && DataObject.IsValid())
	{
		FileValues = UEAgentStateStorePrivate::GetArrayField(DataObject, TEXT("items"));
		if (FileValues.Num() == 0)
		{
			FileValues = UEAgentStateStorePrivate::GetArrayField(DataObject, TEXT("files"));
		}
	}

	TSharedPtr<FJsonObject> ScanDiagnostics = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("scan_diagnostics"));
	if (!ScanDiagnostics.IsValid() && DataObject.IsValid())
	{
		ScanDiagnostics = UEAgentStateStorePrivate::GetObjectField(DataObject, TEXT("scan_diagnostics"));
	}
	const FString EmptyReason = UEAgentStateStorePrivate::GetStringOrDefault(ScanDiagnostics, TEXT("empty_reason"));

	CodeReviewFiles.Reset();
	for (const TSharedPtr<FJsonValue>& FileValue : FileValues)
	{
		TSharedPtr<FUEAgentCodeFileItem> FileItem = UEAgentStateStorePrivate::ParseCodeFileItem(FileValue);
		if (FileItem.IsValid())
		{
			CodeReviewFiles.Add(FileItem);
		}
	}

	if (CodeReviewFiles.Num() == 0 && !EmptyReason.IsEmpty())
	{
		StatusMessage = FString::Printf(TEXT("未返回代码文件。原因：%s"), *EmptyReason);
	}
	else
	{
		StatusMessage = FString::Printf(TEXT("已加载 %d 个代码文件。"), CodeReviewFiles.Num());
	}
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyProjectInventorySnapshotResponse(const TSharedPtr<FJsonObject>& ResponseObject)
{
	const FString PreviousRequestJson = LastResult.RawRequestJson;
	LastResult = FUEAgentResultSnapshot();
	LastResult.RawRequestJson = PreviousRequestJson;
	LastResult.RawResponseJson = UEAgentStateStorePrivate::JsonToPrettyString(ResponseObject);
	const FString UiLanguage = PreferredOutputLanguage;
	LastResult.UserTitle = UEAgent::IsEnglishOutputLanguage(UiLanguage) ? TEXT("Project Inventory Snapshot") : TEXT("Project Inventory 快照");

	const TSharedPtr<FJsonObject> DataObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("data"));
	TSharedPtr<FJsonObject> SnapshotObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("snapshot"));
	if (!SnapshotObject.IsValid())
	{
		SnapshotObject = UEAgentStateStorePrivate::GetObjectField(DataObject, TEXT("snapshot"));
	}
	LastResult.TaskStatus = UEAgentStateStorePrivate::GetStringOrDefault(SnapshotObject, TEXT("status"),
		UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("status"), TEXT("saved")));

	TSharedPtr<FJsonObject> SummaryObject = UEAgentStateStorePrivate::GetObjectField(SnapshotObject, TEXT("summary"));
	if (!SummaryObject.IsValid())
	{
		SummaryObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("summary"));
	}
	if (!SummaryObject.IsValid())
	{
		SummaryObject = UEAgentStateStorePrivate::GetObjectField(DataObject, TEXT("summary"));
	}
	TSharedPtr<FJsonObject> ScanDiagnosticsObject = UEAgentStateStorePrivate::GetObjectField(SnapshotObject, TEXT("scan_diagnostics"));
	if (!ScanDiagnosticsObject.IsValid())
	{
		ScanDiagnosticsObject = UEAgentStateStorePrivate::GetObjectField(ResponseObject, TEXT("scan_diagnostics"));
	}
	if (!ScanDiagnosticsObject.IsValid())
	{
		ScanDiagnosticsObject = UEAgentStateStorePrivate::GetObjectField(DataObject, TEXT("scan_diagnostics"));
	}

	auto PickCount = [&ResponseObject, &DataObject, &SnapshotObject, &SummaryObject](const FString& FieldName, const FString& AlternateFieldName)
	{
		int32 Count = UEAgentStateStorePrivate::GetIntOrDefault(SummaryObject, FieldName);
		if (Count == INDEX_NONE)
		{
			Count = UEAgentStateStorePrivate::GetIntOrDefault(SummaryObject, AlternateFieldName);
		}
		if (Count == INDEX_NONE)
		{
			Count = UEAgentStateStorePrivate::GetIntOrDefault(SnapshotObject, FieldName);
		}
		if (Count == INDEX_NONE)
		{
			Count = UEAgentStateStorePrivate::GetIntOrDefault(SnapshotObject, AlternateFieldName);
		}
		if (Count == INDEX_NONE)
		{
			Count = UEAgentStateStorePrivate::GetIntOrDefault(DataObject, FieldName);
		}
		if (Count == INDEX_NONE)
		{
			Count = UEAgentStateStorePrivate::GetIntOrDefault(ResponseObject, FieldName);
		}
		return Count;
	};

	const int32 AssetCount = PickCount(TEXT("asset_count"), TEXT("assets_count"));
	const int32 CodeFileCount = PickCount(TEXT("code_file_count"), TEXT("code_files_count"));
	const int32 LevelActorCount = PickCount(TEXT("level_actor_count"), TEXT("level_actors_count"));
	const int32 MaterialInstanceCount = PickCount(TEXT("material_instance_count"), TEXT("material_instances_count"));
	const FString SnapshotId = UEAgentStateStorePrivate::GetStringOrDefault(SnapshotObject, TEXT("snapshot_id"),
		UEAgentStateStorePrivate::GetStringOrDefault(ResponseObject, TEXT("snapshot_id"),
			UEAgentStateStorePrivate::GetStringOrDefault(DataObject, TEXT("snapshot_id"))));

	auto SummarizeNumberObject = [](const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return FString();
		}

		TArray<FString> Parts;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonObject->Values)
		{
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
			{
				Parts.Add(FString::Printf(TEXT("%s=%d"), *Pair.Key, FMath::RoundToInt(Pair.Value->AsNumber())));
			}
		}
		Parts.Sort();
		return FString::Join(Parts, TEXT(", "));
	};

	auto AddScalarDiagnostics = [](const TSharedPtr<FJsonObject>& JsonObject, TArray<FString>& Items)
	{
		if (!JsonObject.IsValid())
		{
			return;
		}

		TArray<FString> Lines;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonObject->Values)
		{
			if (!Pair.Value.IsValid())
			{
				continue;
			}

			if (Pair.Value->Type == EJson::String)
			{
				Lines.Add(FString::Printf(TEXT("%s：%s"), *Pair.Key, *Pair.Value->AsString()));
			}
			else if (Pair.Value->Type == EJson::Number)
			{
				Lines.Add(FString::Printf(TEXT("%s：%s"), *Pair.Key, *FString::SanitizeFloat(Pair.Value->AsNumber())));
			}
			else if (Pair.Value->Type == EJson::Boolean)
			{
				Lines.Add(FString::Printf(TEXT("%s：%s"), *Pair.Key, Pair.Value->AsBool() ? TEXT("true") : TEXT("false")));
			}
		}
		Lines.Sort();
		for (const FString& Line : Lines)
		{
			Items.Add(Line);
		}
	};

	FUEAgentUserViewBlock SummaryBlock;
	SummaryBlock.BlockType = TEXT("summary");
	SummaryBlock.Title = UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("快照提交结果"), TEXT("Snapshot Result"));
	if (!SnapshotId.IsEmpty())
	{
		SummaryBlock.Items.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("快照 ID："), TEXT("Snapshot ID: ")), *SnapshotId));
	}
	if (AssetCount != INDEX_NONE)
	{
		SummaryBlock.Items.Add(FString::Printf(TEXT("%s%d"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("资产数量："), TEXT("Asset Count: ")), AssetCount));
	}
	if (CodeFileCount != INDEX_NONE)
	{
		SummaryBlock.Items.Add(FString::Printf(TEXT("%s%d"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("代码文件数量："), TEXT("Code File Count: ")), CodeFileCount));
	}
	if (LevelActorCount != INDEX_NONE)
	{
		SummaryBlock.Items.Add(FString::Printf(TEXT("%s%d"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("关卡 Actor 数量："), TEXT("Level Actor Count: ")), LevelActorCount));
	}
	if (MaterialInstanceCount != INDEX_NONE)
	{
		SummaryBlock.Items.Add(FString::Printf(TEXT("%s%d"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("材质实例数量："), TEXT("Material Instance Count: ")), MaterialInstanceCount));
	}
	const FString AssetTypeCounts = SummarizeNumberObject(UEAgentStateStorePrivate::GetObjectField(SummaryObject, TEXT("asset_type_counts")));
	if (!AssetTypeCounts.IsEmpty())
	{
		SummaryBlock.Items.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("资产类型分布："), TEXT("Asset Type Counts: ")), *AssetTypeCounts));
	}
	const FString CodeFileTypeCounts = SummarizeNumberObject(UEAgentStateStorePrivate::GetObjectField(SummaryObject, TEXT("code_file_type_counts")));
	if (!CodeFileTypeCounts.IsEmpty())
	{
		SummaryBlock.Items.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("代码类型分布："), TEXT("Code File Type Counts: ")), *CodeFileTypeCounts));
	}
	SummaryBlock.Items.Add(FString::Printf(TEXT("%s%s"), *UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("状态："), TEXT("Status: ")), *UEAgentStateStorePrivate::LocalizeStatusLabel(LastResult.TaskStatus, UiLanguage)));
	SummaryBlock.JsonPreview = SummaryObject.IsValid()
		? UEAgentStateStorePrivate::JsonToPrettyString(SummaryObject)
		: (SnapshotObject.IsValid() ? UEAgentStateStorePrivate::JsonToPrettyString(SnapshotObject) : LastResult.RawResponseJson);
	LastResult.Blocks.Add(SummaryBlock);

	if (ScanDiagnosticsObject.IsValid())
	{
		FUEAgentUserViewBlock DiagnosticsBlock;
		DiagnosticsBlock.BlockType = TEXT("diagnostics");
		DiagnosticsBlock.Title = UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("UE 侧扫描诊断"), TEXT("UE Scan Diagnostics"));
		AddScalarDiagnostics(ScanDiagnosticsObject, DiagnosticsBlock.Items);
		DiagnosticsBlock.JsonPreview = UEAgentStateStorePrivate::JsonToPrettyString(ScanDiagnosticsObject);
		LastResult.Blocks.Add(DiagnosticsBlock);
	}

	const FString AssetCountText = AssetCount == INDEX_NONE ? UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("未知"), TEXT("Unknown")) : FString::FromInt(AssetCount);
	const FString CodeFileCountText = CodeFileCount == INDEX_NONE ? UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("未知"), TEXT("Unknown")) : FString::FromInt(CodeFileCount);
	const FString LevelActorCountText = LevelActorCount == INDEX_NONE ? UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("未知"), TEXT("Unknown")) : FString::FromInt(LevelActorCount);
	const FString MaterialInstanceCountText = MaterialInstanceCount == INDEX_NONE ? UEAgentStateStorePrivate::GetLocalizedUiText(UiLanguage, TEXT("未知"), TEXT("Unknown")) : FString::FromInt(MaterialInstanceCount);
	LastResult.UserText = UEAgent::IsEnglishOutputLanguage(UiLanguage)
		? FString::Printf(TEXT("Project Inventory snapshot submitted. Assets: %s, code files: %s, level actors: %s, material instances: %s."), *AssetCountText, *CodeFileCountText, *LevelActorCountText, *MaterialInstanceCountText)
		: FString::Printf(TEXT("已提交 Project Inventory 快照。资产：%s，代码文件：%s，关卡 Actor：%s，材质实例：%s。"), *AssetCountText, *CodeFileCountText, *LevelActorCountText, *MaterialInstanceCountText);
	LastResult.OverviewText = LastResult.UserText;
	StatusMessage = LastResult.UserText;
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyRunEventsResponse(const FString& ResponseText)
{
	LastResult.Events = UEAgentStateStorePrivate::ParseSseEvents(ResponseText);
	LastResult.EventsJson = ResponseText;
	RebuildTraceJson();
	RebuildMonitorJson();
	BroadcastStateChanged();
}

void FUEAgentStateStore::ApplyFailure(const FString& FailureMessage, const FString& RawResponse)
{
	const FString PreviousRequestJson = LastResult.RawRequestJson;
	LastResult = FUEAgentResultSnapshot();
	LastResult.RawResponseJson = RawResponse;
	LastResult.RawRequestJson = PreviousRequestJson;
	LastResult.OverviewText = FailureMessage;
	LastResult.Errors = { FailureMessage };
	BackendServiceStatus = TEXT("Request Failed");
	bBackendOnline = false;
	AppendSystemMessage(FailureMessage, TEXT("Request Failed"));
	StatusMessage = FailureMessage;
	BroadcastStateChanged();
}

const TArray<TSharedPtr<FUEAgentTaskSummary>>& FUEAgentStateStore::GetRecentTasks() const
{
	return RecentTasks;
}

const TArray<TSharedPtr<FUEAgentProposalSummary>>& FUEAgentStateStore::GetPendingProposals() const
{
	return PendingProposals;
}

const TArray<TSharedPtr<FUEAgentCodeFileItem>>& FUEAgentStateStore::GetCodeReviewFiles() const
{
	return CodeReviewFiles;
}

TArray<TSharedPtr<FUEAgentRuntimeProfile>>& FUEAgentStateStore::GetRuntimeProfiles()
{
	return RuntimeProfiles;
}

const FString& FUEAgentStateStore::GetActiveProfileId() const
{
	return ActiveProfileId;
}

const FString& FUEAgentStateStore::GetDefaultProfileId() const
{
	return DefaultProfileId;
}

void FUEAgentStateStore::BroadcastStateChanged()
{
	StateChangedDelegate.Broadcast();
}

void FUEAgentStateStore::AddOrUpdateRecentTask(const FUEAgentResultSnapshot& Snapshot)
{
	if (Snapshot.TaskId.IsEmpty())
	{
		return;
	}

	for (const TSharedPtr<FUEAgentTaskSummary>& ExistingTask : RecentTasks)
	{
		if (ExistingTask.IsValid() && ExistingTask->TaskId == Snapshot.TaskId)
		{
			ExistingTask->Status = Snapshot.TaskStatus;
			ExistingTask->FinishReason = Snapshot.FinishReason;
			ExistingTask->RunId = Snapshot.RunId;
			ExistingTask->Title = Snapshot.UserTitle;
			return;
		}
	}

	TSharedPtr<FUEAgentTaskSummary> NewTask = MakeShared<FUEAgentTaskSummary>();
	NewTask->TaskId = Snapshot.TaskId;
	NewTask->RunId = Snapshot.RunId;
	NewTask->TaskType = UEAgent::ToFunctionId(ActiveFunction);
	NewTask->Status = Snapshot.TaskStatus;
	NewTask->FinishReason = Snapshot.FinishReason;
	NewTask->Title = Snapshot.UserTitle;
	RecentTasks.Insert(NewTask, 0);
}

void FUEAgentStateStore::InitializeParameterDefaults()
{
	for (const EUEAgentFunctionType FunctionType : UEAgent::GetOrderedFunctions())
	{
		ParameterDrafts.FindOrAdd(FunctionType);
	}
}

void FUEAgentStateStore::LoadPersistedState()
{
	if (GConfig != nullptr)
	{
		GConfig->GetString(UEAgentStateStorePrivate::ConfigSection, TEXT("BackendBaseUrl"), BackendBaseUrl, GEditorPerProjectIni);
		GConfig->GetString(UEAgentStateStorePrivate::ConfigSection, TEXT("SessionId"), SessionId, GEditorPerProjectIni);
		GConfig->GetString(UEAgentStateStorePrivate::ConfigSection, TEXT("ActiveProfileId"), ActiveProfileId, GEditorPerProjectIni);
		GConfig->GetString(UEAgentStateStorePrivate::ConfigSection, TEXT("PreferredOutputLanguage"), PreferredOutputLanguage, GEditorPerProjectIni);
	}

	PreferredOutputLanguage = UEAgentStateStorePrivate::NormalizePreferredOutputLanguage(PreferredOutputLanguage);

	if (SessionId.IsEmpty())
	{
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	EditorContext.SessionId = SessionId;
	StatusMessage = TEXT("Ready.");
}

void FUEAgentStateStore::PersistState() const
{
	if (GConfig == nullptr)
	{
		return;
	}

	GConfig->SetString(UEAgentStateStorePrivate::ConfigSection, TEXT("BackendBaseUrl"), *BackendBaseUrl, GEditorPerProjectIni);
	GConfig->SetString(UEAgentStateStorePrivate::ConfigSection, TEXT("SessionId"), *SessionId, GEditorPerProjectIni);
	GConfig->SetString(UEAgentStateStorePrivate::ConfigSection, TEXT("ActiveProfileId"), *ActiveProfileId, GEditorPerProjectIni);
	GConfig->SetString(UEAgentStateStorePrivate::ConfigSection, TEXT("PreferredOutputLanguage"), *PreferredOutputLanguage, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void FUEAgentStateStore::RebuildTraceJson()
{
	TSharedPtr<FJsonObject> TraceObject = MakeShared<FJsonObject>();
	TraceObject->SetStringField(TEXT("task_id"), LastResult.TaskId);
	TraceObject->SetStringField(TEXT("run_id"), LastResult.RunId);

	TSharedPtr<FJsonObject> TraceSummaryParsed;
	if (!LastResult.TraceJson.IsEmpty() && LastResult.TraceJson != TEXT("{}"))
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LastResult.TraceJson);
		FJsonSerializer::Deserialize(Reader, TraceSummaryParsed);
	}
	if (TraceSummaryParsed.IsValid())
	{
		const TSharedPtr<FJsonObject> NestedTraceSummary = UEAgentStateStorePrivate::GetObjectField(TraceSummaryParsed, TEXT("trace_summary"));
		if (NestedTraceSummary.IsValid())
		{
			TraceSummaryParsed = NestedTraceSummary;
		}
	}
	TraceObject->SetObjectField(TEXT("trace_summary"), UEAgentStateStorePrivate::ObjectOrEmpty(TraceSummaryParsed));

	TArray<TSharedPtr<FJsonValue>> TraceLinkValues;
	for (const FUEAgentTraceLink& TraceLink : LastResult.TraceLinks)
	{
		TSharedPtr<FJsonObject> LinkObject = MakeShared<FJsonObject>();
		LinkObject->SetStringField(TEXT("label"), TraceLink.Label);
		LinkObject->SetStringField(TEXT("url"), TraceLink.Url);
		TraceLinkValues.Add(MakeShared<FJsonValueObject>(LinkObject));
	}
	TraceObject->SetArrayField(TEXT("trace_links"), TraceLinkValues);

	TArray<TSharedPtr<FJsonValue>> EventValues;
	for (const FUEAgentRunEvent& Event : LastResult.Events)
	{
		TSharedPtr<FJsonObject> EventObject = MakeShared<FJsonObject>();
		if (Event.Seq != INDEX_NONE)
		{
			EventObject->SetNumberField(TEXT("seq"), Event.Seq);
		}
		EventObject->SetStringField(TEXT("event_type"), Event.EventType);
		EventObject->SetStringField(TEXT("timestamp"), Event.Timestamp);
		EventObject->SetStringField(TEXT("summary"), Event.Summary);
		EventValues.Add(MakeShared<FJsonValueObject>(EventObject));
	}
	TraceObject->SetArrayField(TEXT("events"), EventValues);
	TraceObject->SetStringField(TEXT("events_stream_raw"), LastResult.EventsJson);

	LastResult.TraceJson = UEAgentStateStorePrivate::JsonToPrettyString(TraceObject);
}

void FUEAgentStateStore::RebuildMonitorJson()
{
	TSharedPtr<FJsonObject> MonitorObject = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> TaskObject = MakeShared<FJsonObject>();
	TaskObject->SetStringField(TEXT("task_id"), LastResult.TaskId);
	TaskObject->SetStringField(TEXT("run_id"), LastResult.RunId);
	TaskObject->SetStringField(TEXT("status"), LastResult.TaskStatus);
	TaskObject->SetStringField(TEXT("finish_reason"), LastResult.FinishReason);
	TaskObject->SetBoolField(TEXT("output_complete"), LastResult.bOutputComplete);
	MonitorObject->SetObjectField(TEXT("task"), TaskObject);

	TSharedPtr<FJsonObject> TraceSummaryObject = UEAgentStateStorePrivate::ParseJsonObject(LastResult.TraceJson);
	if (TraceSummaryObject.IsValid())
	{
		const TSharedPtr<FJsonObject> NestedTraceSummary = UEAgentStateStorePrivate::GetObjectField(TraceSummaryObject, TEXT("trace_summary"));
		if (NestedTraceSummary.IsValid())
		{
			TraceSummaryObject = NestedTraceSummary;
		}
	}

	MonitorObject->SetObjectField(TEXT("usage"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastResult.UsageJson)));
	MonitorObject->SetObjectField(TEXT("metrics"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastResult.MetricsJson)));
	MonitorObject->SetObjectField(TEXT("trace_summary"), UEAgentStateStorePrivate::ObjectOrEmpty(TraceSummaryObject));
	MonitorObject->SetObjectField(TEXT("locale"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastResult.LocaleJson)));
	MonitorObject->SetObjectField(TEXT("skill"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastResult.SkillJson)));
	MonitorObject->SetObjectField(TEXT("session_summary"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastResult.SessionSummaryJson)));
	MonitorObject->SetObjectField(TEXT("memory_summary"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastResult.MemorySummaryJson)));
	MonitorObject->SetObjectField(TEXT("health_snapshot"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastHealthJson)));
	MonitorObject->SetObjectField(TEXT("capabilities_snapshot"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastCapabilitiesJson)));
	MonitorObject->SetObjectField(TEXT("editor_operation_capabilities"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastEditorOperationCapabilitiesJson)));
	MonitorObject->SetObjectField(TEXT("knowledge_base_status"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastKnowledgeBaseStatusJson)));
	MonitorObject->SetObjectField(TEXT("bootstrap_snapshot"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastBootstrapJson)));
	MonitorObject->SetObjectField(TEXT("settings_snapshot"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastSettingsJson)));
	MonitorObject->SetObjectField(TEXT("alerts"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastAlertsJson)));
	MonitorObject->SetStringField(TEXT("prometheus_metrics_text"), LastMetricsText);
	MonitorObject->SetStringField(TEXT("session_status"), SessionStatusText);

	LastResult.MonitorJson = UEAgentStateStorePrivate::JsonToPrettyString(MonitorObject);
}

void FUEAgentStateStore::RebuildArtifactsJson()
{
	TSharedPtr<FJsonObject> ArtifactsObject = MakeShared<FJsonObject>();
	ArtifactsObject->SetStringField(TEXT("task_id"), LastResult.TaskId);

	TArray<TSharedPtr<FJsonValue>> ArtifactValues;
	for (const FUEAgentArtifactItem& Artifact : LastResult.Artifacts)
	{
		TSharedPtr<FJsonObject> ArtifactObject = MakeShared<FJsonObject>();
		ArtifactObject->SetStringField(TEXT("artifact_id"), Artifact.ArtifactId);
		ArtifactObject->SetStringField(TEXT("artifact_type"), Artifact.ArtifactType);
		ArtifactObject->SetStringField(TEXT("label"), Artifact.Label);
		ArtifactObject->SetStringField(TEXT("path"), Artifact.Path);
		ArtifactObject->SetObjectField(TEXT("metadata"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(Artifact.MetadataJson)));
		ArtifactValues.Add(MakeShared<FJsonValueObject>(ArtifactObject));
	}
	ArtifactsObject->SetArrayField(TEXT("items"), ArtifactValues);
	ArtifactsObject->SetObjectField(TEXT("approval_result"), UEAgentStateStorePrivate::ObjectOrEmpty(UEAgentStateStorePrivate::ParseJsonObject(LastResult.ApprovalResultJson)));

	LastResult.ArtifactsJson = UEAgentStateStorePrivate::JsonToPrettyString(ArtifactsObject);
}
