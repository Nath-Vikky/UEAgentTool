// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "AgentEditorToolRegistry.h"

class FUEAgentEditorToolCatalog
{
public:
	static TArray<FUEAgentEditorToolDefinition> BuildCoreEditorOperationDefinitions();
	static FUEAgentEditorToolRegistry BuildMetadataRegistry();
	static TSharedPtr<FJsonObject> BuildToolsListJson();
};
