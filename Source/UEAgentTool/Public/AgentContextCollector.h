// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AgentTypes.h"
#include "CoreMinimal.h"

class FJsonObject;

class FUEAgentContextCollector : public TSharedFromThis<FUEAgentContextCollector>
{
public:
	FUEAgentContextSummary CaptureContext() const;
	TSharedPtr<FJsonObject> BuildProjectInventorySnapshot(int32 MaxAssets = 1000, int32 MaxCodeFiles = 1000) const;

private:
	FString DeriveActiveModule(const FString& ProjectName) const;
	FString DeriveModuleFromRelativePath(const FString& RelativePath, const FString& ProjectName) const;
	TArray<FString> ExtractCodeSymbols(const FString& AbsoluteFilePath) const;
};
