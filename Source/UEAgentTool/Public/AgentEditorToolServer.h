// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FJsonObject;
class FSocket;
class FTcpListener;
struct FIPv4Endpoint;

class FUEAgentEditorToolServer : public TSharedFromThis<FUEAgentEditorToolServer>
{
public:
	FUEAgentEditorToolServer();
	~FUEAgentEditorToolServer();

	void StartFromConfig();
	bool Start(const FString& InHost, int32 InPort);
	void Stop();
	bool IsRunning() const;
	FString GetStatusText() const;

private:
	bool HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint);
	void HandleClient(FSocket* ClientSocket, const FString& ClientLabel) const;
	FString HandleJsonRpcLine(const FString& RequestLine) const;
	TSharedPtr<FJsonObject> BuildInitializeResult() const;
	TSharedPtr<FJsonObject> BuildToolsListResult() const;
	TSharedPtr<FJsonObject> BuildToolCallResult(const TSharedPtr<FJsonObject>& ParamsObject) const;
	TSharedPtr<FJsonObject> BuildEditorContextResult() const;
	TSharedPtr<FJsonObject> BuildSelectedAssetsResult() const;
	TSharedPtr<FJsonObject> BuildAssetDetailsResult(const FString& AssetPathOrQuery) const;
	TSharedPtr<FJsonObject> BuildSelectedActorsResult() const;
	TSharedPtr<FJsonObject> BuildLevelActorsResult(const TSharedPtr<FJsonObject>& ArgumentsObject) const;
	TSharedPtr<FJsonObject> BuildLevelActorDetailsResult(const FString& ActorReference) const;
	TSharedPtr<FJsonObject> BuildStaticMeshDetailsResult(const FString& StaticMeshPathOrQuery) const;
	TSharedPtr<FJsonObject> BuildBlueprintGraphResult(const FString& BlueprintPath) const;
	TSharedPtr<FJsonObject> BuildBlueprintNodeDetailsResult(const FString& BlueprintPath, const FString& GraphName, const FString& NodeQuery) const;
	TSharedPtr<FJsonObject> BuildWidgetTreeResult(const FString& WidgetBlueprintPath) const;
	TSharedPtr<FJsonObject> BuildWidgetDetailsResult(const FString& WidgetBlueprintPath, const FString& WidgetName) const;
	TSharedPtr<FJsonObject> BuildMaterialInstanceParametersResult(const FString& MaterialInstancePath) const;

	static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Text);
	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject);
	static FString MakeJsonRpcResponse(const TSharedPtr<FJsonObject>& RequestObject, const TSharedPtr<FJsonObject>& ResultObject);
	static FString MakeJsonRpcError(const TSharedPtr<FJsonObject>& RequestObject, int32 Code, const FString& Message, const FString& Reason);
	static bool SendLine(FSocket* ClientSocket, const FString& Line);

private:
	mutable FCriticalSection StateLock;
	TUniquePtr<FTcpListener> Listener;
	FString Host = TEXT("127.0.0.1");
	int32 Port = 8765;
	bool bRunning = false;
};
