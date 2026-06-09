// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

class FUEAgentUserMessageFormatter
{
public:
	static bool BuildProjectInventorySummaryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus);
	static bool BuildAssetInventoryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus);
	static bool BuildAssetDetailMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus);
	static bool BuildLevelActorsInventoryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus);
	static bool BuildMaterialInstancesInventoryMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus);
	static bool BuildEditorOperationCapabilitiesMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus);
	static bool BuildToolManifestWorkflowPreviewMessage(const TSharedPtr<FJsonObject>& ResponseObject, FString& OutMessage, FString& OutStatus);
	static bool BuildEditorOperationActivityMessage(const TSharedPtr<FJsonObject>& DiagnosticsObject, const TSharedPtr<FJsonObject>& HistoryObject, FString& OutMessage, FString& OutStatus);
};
