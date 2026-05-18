// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentEditorToolServer.h"

#include "AgentEditorToolCatalog.h"
#include "Async/Async.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Common/TcpListener.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAgentEditorToolServer, Log, All);

namespace UEAgentEditorToolServerPrivate
{
	static constexpr int32 MaxReadChunkBytes = 64 * 1024;
	static constexpr int32 SocketWaitSeconds = 2;

	static TArray<TSharedPtr<FJsonValue>> StringsToJsonArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	static TSharedPtr<FJsonObject> MakeInputSchema(const TArray<FString>& RequiredFields, const TArray<FString>& OptionalFields)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		for (const FString& FieldName : RequiredFields)
		{
			Properties->SetObjectField(FieldName, MakeShared<FJsonObject>());
		}
		for (const FString& FieldName : OptionalFields)
		{
			if (!Properties->HasField(FieldName))
			{
				Properties->SetObjectField(FieldName, MakeShared<FJsonObject>());
			}
		}
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		Schema->SetArrayField(TEXT("required"), StringsToJsonArray(RequiredFields));
		return Schema;
	}

	static TSharedPtr<FJsonObject> MakeMcpToolObject(const FUEAgentEditorToolDefinition& Definition)
	{
		TSharedPtr<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), Definition.OperationType);
		ToolObject->SetStringField(TEXT("description"), Definition.Description);
		ToolObject->SetObjectField(TEXT("inputSchema"), MakeInputSchema(Definition.RequiredFields, Definition.OptionalFields));

		TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetStringField(TEXT("tool_name"), Definition.ToolName.ToString());
		Annotations->SetStringField(TEXT("operation_type"), Definition.OperationType);
		Annotations->SetStringField(TEXT("category"), Definition.Category);
		Annotations->SetStringField(TEXT("side_effect_level"), Definition.SideEffectLevel);
		Annotations->SetBoolField(TEXT("requires_confirmation"), Definition.SideEffectLevel != TEXT("read_only"));
		Annotations->SetStringField(TEXT("execution_policy"), TEXT("confirmed_write_tools_must_use_http_proposal"));
		ToolObject->SetObjectField(TEXT("annotations"), Annotations);
		return ToolObject;
	}

	static TSharedPtr<FJsonObject> MakeReadOnlyCatalogToolObject()
	{
		TSharedPtr<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), TEXT("ue_agent_tools_list"));
		ToolObject->SetStringField(TEXT("description"), TEXT("Return UEAgentTool editor tool metadata without executing editor writes."));
		ToolObject->SetObjectField(TEXT("inputSchema"), MakeInputSchema(TArray<FString>(), TArray<FString>()));

		TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetStringField(TEXT("category"), TEXT("diagnostics"));
		Annotations->SetStringField(TEXT("side_effect_level"), TEXT("read_only"));
		Annotations->SetBoolField(TEXT("requires_confirmation"), false);
		ToolObject->SetObjectField(TEXT("annotations"), Annotations);
		return ToolObject;
	}

	static FString GetEnvVar(const TCHAR* Name)
	{
		return FPlatformMisc::GetEnvironmentVariable(Name).TrimStartAndEnd();
	}

	static bool IsTruthy(const FString& Value)
	{
		return Value.Equals(TEXT("1")) || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("on"), ESearchCase::IgnoreCase);
	}

	static FString BlueprintStatusToString(const EBlueprintStatus Status)
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

	static UBlueprint* LoadBlueprintAsset(const FString& BlueprintPath)
	{
		const FString PackagePath = NormalizeAssetPackagePath(BlueprintPath);
		return Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
	}

	static TSharedPtr<FJsonObject> MakeToolErrorObject(const FString& Reason, const FString& Message)
	{
		TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetBoolField(TEXT("isError"), true);
		TArray<TSharedPtr<FJsonValue>> ContentValues;
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), Message);
		ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
		ResultObject->SetArrayField(TEXT("content"), ContentValues);
		TSharedPtr<FJsonObject> StructuredObject = MakeShared<FJsonObject>();
		StructuredObject->SetStringField(TEXT("reason"), Reason);
		StructuredObject->SetStringField(TEXT("message"), Message);
		ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
		return ResultObject;
	}

	static void AddGraphSummaries(const TArray<UEdGraph*>& Graphs, const FString& GraphType, TArray<TSharedPtr<FJsonValue>>& OutGraphValues)
	{
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
			GraphObject->SetStringField(TEXT("graph_name"), Graph->GetName());
			GraphObject->SetStringField(TEXT("graph_type"), GraphType);
			GraphObject->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

			TArray<TSharedPtr<FJsonValue>> NodeValues;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node == nullptr)
				{
					continue;
				}
				TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
				NodeObject->SetStringField(TEXT("node_name"), Node->GetName());
				NodeObject->SetStringField(TEXT("node_class"), Node->GetClass() != nullptr ? Node->GetClass()->GetName() : TEXT("unknown"));
				NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
				NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);
				NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));
				if (NodeValues.Num() >= 64)
				{
					break;
				}
			}
			GraphObject->SetArrayField(TEXT("nodes"), NodeValues);
			OutGraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
			if (OutGraphValues.Num() >= 64)
			{
				break;
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildBlueprintGraphSnapshot(const FString& BlueprintPath)
	{
		const FString NormalizedPath = NormalizeAssetPackagePath(BlueprintPath);
		if (NormalizedPath.IsEmpty())
		{
			return MakeToolErrorObject(TEXT("missing_blueprint_path"), TEXT("blueprint_path is required."));
		}
		UBlueprint* Blueprint = LoadBlueprintAsset(NormalizedPath);
		if (Blueprint == nullptr)
		{
			return MakeToolErrorObject(TEXT("blueprint_not_found"), FString::Printf(TEXT("Blueprint not found: %s"), *NormalizedPath));
		}

		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("blueprint_path"), NormalizedPath);
		SnapshotObject->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
		SnapshotObject->SetStringField(TEXT("status"), BlueprintStatusToString(Blueprint->Status));
		SnapshotObject->SetBoolField(TEXT("is_dirty"), Blueprint->GetPackage() != nullptr && Blueprint->GetPackage()->IsDirty());
		SnapshotObject->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint));
		if (Blueprint->ParentClass != nullptr)
		{
			SnapshotObject->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> GraphValues;
#if WITH_EDITORONLY_DATA
		AddGraphSummaries(Blueprint->UbergraphPages, TEXT("event"), GraphValues);
		AddGraphSummaries(Blueprint->FunctionGraphs, TEXT("function"), GraphValues);
		AddGraphSummaries(Blueprint->MacroGraphs, TEXT("macro"), GraphValues);
#endif
		SnapshotObject->SetArrayField(TEXT("graphs"), GraphValues);

		TArray<TSharedPtr<FJsonValue>> VariableValues;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
			VariableObject->SetStringField(TEXT("variable_name"), Variable.VarName.ToString());
			VariableObject->SetStringField(TEXT("variable_type"), Variable.VarType.PinCategory.ToString());
			VariableObject->SetStringField(TEXT("category"), Variable.Category.ToString());
			VariableValues.Add(MakeShared<FJsonValueObject>(VariableObject));
			if (VariableValues.Num() >= 64)
			{
				break;
			}
		}
		SnapshotObject->SetArrayField(TEXT("variables"), VariableValues);

		TArray<TSharedPtr<FJsonValue>> ComponentValues;
		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node == nullptr)
				{
					continue;
				}
				TSharedPtr<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
				ComponentObject->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
				if (Node->ComponentTemplate != nullptr)
				{
					ComponentObject->SetStringField(TEXT("component_class"), Node->ComponentTemplate->GetClass()->GetPathName());
				}
				ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
				if (ComponentValues.Num() >= 64)
				{
					break;
				}
			}
		}
		SnapshotObject->SetArrayField(TEXT("components"), ComponentValues);
		return SnapshotObject;
	}
}

FUEAgentEditorToolServer::FUEAgentEditorToolServer() = default;

FUEAgentEditorToolServer::~FUEAgentEditorToolServer()
{
	Stop();
}

void FUEAgentEditorToolServer::StartFromConfig()
{
	bool bEnabled = false;
	FString ConfigHost = TEXT("127.0.0.1");
	int32 ConfigPort = 8765;

	if (GConfig)
	{
		GConfig->GetBool(TEXT("UEAgentTool.EditorToolServer"), TEXT("bEnabled"), bEnabled, GEngineIni);
		GConfig->GetString(TEXT("UEAgentTool.EditorToolServer"), TEXT("Host"), ConfigHost, GEngineIni);
		GConfig->GetInt(TEXT("UEAgentTool.EditorToolServer"), TEXT("Port"), ConfigPort, GEngineIni);
	}

	const FString EnvEnabled = UEAgentEditorToolServerPrivate::GetEnvVar(TEXT("UEAGENT_EDITOR_TOOL_SERVER_ENABLED"));
	if (!EnvEnabled.IsEmpty())
	{
		bEnabled = UEAgentEditorToolServerPrivate::IsTruthy(EnvEnabled);
	}

	const FString EnvHost = UEAgentEditorToolServerPrivate::GetEnvVar(TEXT("UEAGENT_EDITOR_TOOL_SERVER_HOST"));
	if (!EnvHost.IsEmpty())
	{
		ConfigHost = EnvHost;
	}

	const FString EnvPort = UEAgentEditorToolServerPrivate::GetEnvVar(TEXT("UEAGENT_EDITOR_TOOL_SERVER_PORT"));
	if (!EnvPort.IsEmpty())
	{
		ConfigPort = FCString::Atoi(*EnvPort);
	}

	if (!bEnabled)
	{
		UE_LOG(LogUEAgentEditorToolServer, Display, TEXT("UEAgent editor tool TCP server is disabled."));
		return;
	}

	Start(ConfigHost, ConfigPort);
}

bool FUEAgentEditorToolServer::Start(const FString& InHost, const int32 InPort)
{
	FScopeLock Lock(&StateLock);
	if (bRunning)
	{
		return true;
	}
	if (InPort <= 0)
	{
		UE_LOG(LogUEAgentEditorToolServer, Warning, TEXT("Invalid UEAgent editor tool TCP port: %d"), InPort);
		return false;
	}

	FIPv4Address Address;
	if (!FIPv4Address::Parse(InHost, Address))
	{
		UE_LOG(LogUEAgentEditorToolServer, Warning, TEXT("Invalid UEAgent editor tool TCP host: %s"), *InHost);
		return false;
	}

	Host = InHost;
	Port = InPort;
	const FIPv4Endpoint Endpoint(Address, Port);
	Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(100));
	Listener->OnConnectionAccepted().BindRaw(this, &FUEAgentEditorToolServer::HandleConnectionAccepted);
	if (!Listener->Init())
	{
		Listener.Reset();
		UE_LOG(LogUEAgentEditorToolServer, Warning, TEXT("Failed to start UEAgent editor tool TCP server on %s:%d."), *Host, Port);
		return false;
	}

	bRunning = true;
	UE_LOG(LogUEAgentEditorToolServer, Display, TEXT("UEAgent editor tool TCP server listening on %s:%d."), *Host, Port);
	return true;
}

void FUEAgentEditorToolServer::Stop()
{
	FScopeLock Lock(&StateLock);
	if (Listener.IsValid())
	{
		Listener->Stop();
		Listener.Reset();
	}
	bRunning = false;
}

bool FUEAgentEditorToolServer::IsRunning() const
{
	FScopeLock Lock(&StateLock);
	return bRunning;
}

FString FUEAgentEditorToolServer::GetStatusText() const
{
	FScopeLock Lock(&StateLock);
	return bRunning ? FString::Printf(TEXT("listening:%s:%d"), *Host, Port) : TEXT("disabled");
}

bool FUEAgentEditorToolServer::HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint)
{
	if (ClientSocket == nullptr)
	{
		return false;
	}
	HandleClient(ClientSocket, ClientEndpoint.ToString());
	return true;
}

void FUEAgentEditorToolServer::HandleClient(FSocket* ClientSocket, const FString& ClientLabel) const
{
	FString PendingText;
	TArray<uint8> Buffer;
	bool bKeepReading = true;

	while (bKeepReading && ClientSocket != nullptr && ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(UEAgentEditorToolServerPrivate::SocketWaitSeconds)))
	{
		uint32 PendingDataSize = 0;
		while (ClientSocket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
		{
			const int32 BytesToRead = static_cast<int32>(FMath::Min<uint32>(PendingDataSize, UEAgentEditorToolServerPrivate::MaxReadChunkBytes));
			Buffer.SetNumUninitialized(BytesToRead);
			int32 BytesRead = 0;
			if (!ClientSocket->Recv(Buffer.GetData(), Buffer.Num(), BytesRead, ESocketReceiveFlags::None) || BytesRead <= 0)
			{
				bKeepReading = false;
				break;
			}

			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), BytesRead);
			PendingText.AppendChars(Converted.Get(), Converted.Length());

			FString Line;
			while (PendingText.Split(TEXT("\n"), &Line, &PendingText, ESearchCase::CaseSensitive, ESearchDir::FromStart))
			{
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty())
				{
					continue;
				}
				const FString ResponseLine = HandleJsonRpcLine(Line);
				if (!ResponseLine.IsEmpty() && !SendLine(ClientSocket, ResponseLine))
				{
					bKeepReading = false;
					break;
				}
			}
		}
	}

	PendingText.TrimStartAndEndInline();
	if (!PendingText.IsEmpty())
	{
		const FString ResponseLine = HandleJsonRpcLine(PendingText);
		if (!ResponseLine.IsEmpty())
		{
			SendLine(ClientSocket, ResponseLine);
		}
	}

	UE_LOG(LogUEAgentEditorToolServer, Verbose, TEXT("UEAgent editor tool TCP client disconnected: %s"), *ClientLabel);
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
}

FString FUEAgentEditorToolServer::HandleJsonRpcLine(const FString& RequestLine) const
{
	const TSharedPtr<FJsonObject> RequestObject = ParseJsonObject(RequestLine);
	if (!RequestObject.IsValid())
	{
		return MakeJsonRpcError(nullptr, -32700, TEXT("Parse error"), TEXT("invalid_json"));
	}

	FString Method;
	RequestObject->TryGetStringField(TEXT("method"), Method);
	const bool bHasRequestId = RequestObject->HasField(TEXT("id"));
	if (Method.Equals(TEXT("notifications/initialized"), ESearchCase::IgnoreCase) && !bHasRequestId)
	{
		return FString();
	}
	if (Method.Equals(TEXT("initialize"), ESearchCase::IgnoreCase))
	{
		return MakeJsonRpcResponse(RequestObject, BuildInitializeResult());
	}
	if (Method.Equals(TEXT("tools/list"), ESearchCase::IgnoreCase))
	{
		return MakeJsonRpcResponse(RequestObject, BuildToolsListResult());
	}
	if (Method.Equals(TEXT("tools/call"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ParamsField = nullptr;
		const TSharedPtr<FJsonObject> ParamsObject = RequestObject->TryGetObjectField(TEXT("params"), ParamsField) && ParamsField != nullptr ? *ParamsField : nullptr;
		return MakeJsonRpcResponse(RequestObject, BuildToolCallResult(ParamsObject));
	}
	if (!bHasRequestId)
	{
		return FString();
	}
	return MakeJsonRpcError(RequestObject, -32601, TEXT("Method not found"), TEXT("unsupported_method"));
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildInitializeResult() const
{
	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetStringField(TEXT("protocolVersion"), TEXT("2024-11-05"));

	TSharedPtr<FJsonObject> CapabilitiesObject = MakeShared<FJsonObject>();
	CapabilitiesObject->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
	ResultObject->SetObjectField(TEXT("capabilities"), CapabilitiesObject);

	TSharedPtr<FJsonObject> ServerInfoObject = MakeShared<FJsonObject>();
	ServerInfoObject->SetStringField(TEXT("name"), TEXT("UEAgentTool.EditorToolServer"));
	ServerInfoObject->SetStringField(TEXT("version"), TEXT("0.1.0"));
	ResultObject->SetObjectField(TEXT("serverInfo"), ServerInfoObject);

	TSharedPtr<FJsonObject> UEAgentObject = MakeShared<FJsonObject>();
	UEAgentObject->SetStringField(TEXT("transport"), TEXT("tcp_jsonrpc_line"));
	UEAgentObject->SetStringField(TEXT("status"), GetStatusText());
	UEAgentObject->SetBoolField(TEXT("http_proposal_flow_required_for_writes"), true);
	ResultObject->SetObjectField(TEXT("ue_agent"), UEAgentObject);
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildToolsListResult() const
{
	TArray<TSharedPtr<FJsonValue>> ToolValues;
	ToolValues.Add(MakeShared<FJsonValueObject>(UEAgentEditorToolServerPrivate::MakeReadOnlyCatalogToolObject()));
	for (const FUEAgentEditorToolDefinition& Definition : FUEAgentEditorToolCatalog::BuildCoreEditorOperationDefinitions())
	{
		ToolValues.Add(MakeShared<FJsonValueObject>(UEAgentEditorToolServerPrivate::MakeMcpToolObject(Definition)));
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetArrayField(TEXT("tools"), ToolValues);
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildToolCallResult(const TSharedPtr<FJsonObject>& ParamsObject) const
{
	FString ToolName;
	if (ParamsObject.IsValid())
	{
		ParamsObject->TryGetStringField(TEXT("name"), ToolName);
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));

	if (ToolName.Equals(TEXT("ue_agent_tools_list"), ESearchCase::IgnoreCase))
	{
		TextContent->SetStringField(TEXT("text"), SerializeJsonObject(FUEAgentEditorToolCatalog::BuildToolsListJson()));
		ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
		ResultObject->SetArrayField(TEXT("content"), ContentValues);
		ResultObject->SetObjectField(TEXT("structuredContent"), FUEAgentEditorToolCatalog::BuildToolsListJson());
		return ResultObject;
	}
	if (ToolName.Equals(TEXT("get_blueprint_graph"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* ArgumentsField = nullptr;
		const TSharedPtr<FJsonObject> ArgumentsObject = ParamsObject.IsValid() && ParamsObject->TryGetObjectField(TEXT("arguments"), ArgumentsField) && ArgumentsField != nullptr ? *ArgumentsField : nullptr;
		FString BlueprintPath;
		if (ArgumentsObject.IsValid())
		{
			ArgumentsObject->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
		}
		return BuildBlueprintGraphResult(BlueprintPath);
	}

	ResultObject->SetBoolField(TEXT("isError"), true);
	TextContent->SetStringField(TEXT("text"), FString::Printf(TEXT("Tool '%s' requires the existing HTTP Proposal confirmation flow and cannot be executed through raw MCP/TCP."), *ToolName));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	return ResultObject;
}

TSharedPtr<FJsonObject> FUEAgentEditorToolServer::BuildBlueprintGraphResult(const FString& BlueprintPath) const
{
	TSharedPtr<FJsonObject> SnapshotObject;
	if (IsInGameThread())
	{
		SnapshotObject = UEAgentEditorToolServerPrivate::BuildBlueprintGraphSnapshot(BlueprintPath);
	}
	else
	{
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [BlueprintPath, &SnapshotObject, CompletionEvent]()
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::BuildBlueprintGraphSnapshot(BlueprintPath);
			CompletionEvent->Trigger();
		});
		const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(3));
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		if (!bCompleted)
		{
			SnapshotObject = UEAgentEditorToolServerPrivate::MakeToolErrorObject(TEXT("game_thread_timeout"), TEXT("Timed out while reading Blueprint graph metadata on the game thread."));
		}
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentValues;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), SerializeJsonObject(SnapshotObject));
	ContentValues.Add(MakeShared<FJsonValueObject>(TextContent));
	ResultObject->SetArrayField(TEXT("content"), ContentValues);
	const TSharedPtr<FJsonObject> StructuredObject = SnapshotObject.IsValid() ? SnapshotObject : MakeShared<FJsonObject>();
	ResultObject->SetObjectField(TEXT("structuredContent"), StructuredObject);
	if (SnapshotObject.IsValid() && SnapshotObject->HasField(TEXT("reason")))
	{
		ResultObject->SetBoolField(TEXT("isError"), true);
	}
	return ResultObject;
}
TSharedPtr<FJsonObject> FUEAgentEditorToolServer::ParseJsonObject(const FString& Text)
{
	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, JsonObject) ? JsonObject : nullptr;
}

FString FUEAgentEditorToolServer::SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
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

FString FUEAgentEditorToolServer::MakeJsonRpcResponse(const TSharedPtr<FJsonObject>& RequestObject, const TSharedPtr<FJsonObject>& ResultObject)
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	TSharedPtr<FJsonValue> IdValue = MakeShared<FJsonValueNull>();
	if (RequestObject.IsValid() && RequestObject->HasField(TEXT("id")))
	{
		IdValue = RequestObject->TryGetField(TEXT("id"));
	}
	if (!IdValue.IsValid())
	{
		IdValue = MakeShared<FJsonValueNull>();
	}
	RootObject->SetField(TEXT("id"), IdValue);
	TSharedPtr<FJsonObject> SafeResultObject = ResultObject;
	if (!SafeResultObject.IsValid())
	{
		SafeResultObject = MakeShared<FJsonObject>();
	}
	RootObject->SetObjectField(TEXT("result"), SafeResultObject);
	return SerializeJsonObject(RootObject);
}

FString FUEAgentEditorToolServer::MakeJsonRpcError(const TSharedPtr<FJsonObject>& RequestObject, const int32 Code, const FString& Message, const FString& Reason)
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	TSharedPtr<FJsonValue> IdValue = MakeShared<FJsonValueNull>();
	if (RequestObject.IsValid() && RequestObject->HasField(TEXT("id")))
	{
		IdValue = RequestObject->TryGetField(TEXT("id"));
	}
	if (!IdValue.IsValid())
	{
		IdValue = MakeShared<FJsonValueNull>();
	}
	RootObject->SetField(TEXT("id"), IdValue);

	TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
	ErrorObject->SetNumberField(TEXT("code"), Code);
	ErrorObject->SetStringField(TEXT("message"), Message);
	TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
	DataObject->SetStringField(TEXT("reason"), Reason);
	ErrorObject->SetObjectField(TEXT("data"), DataObject);
	RootObject->SetObjectField(TEXT("error"), ErrorObject);
	return SerializeJsonObject(RootObject);
}

bool FUEAgentEditorToolServer::SendLine(FSocket* ClientSocket, const FString& Line)
{
	if (ClientSocket == nullptr)
	{
		return false;
	}
	const FString Payload = Line + TEXT("\n");
	const FTCHARToUTF8 Converted(*Payload);
	int32 BytesSent = 0;
	return ClientSocket->Send(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length(), BytesSent) && BytesSent == Converted.Length();
}
