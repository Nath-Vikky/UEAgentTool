// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentCommands.h"

#include "AgentStyle.h"

#define LOCTEXT_NAMESPACE "FUEAgentToolCommands"

FUEAgentToolCommands::FUEAgentToolCommands()
	: TCommands<FUEAgentToolCommands>(
		TEXT("UEAgentTool"),
		LOCTEXT("UEAgentToolContext", "UE Agent Tool"),
		NAME_None,
		FUEAgentToolStyle::GetStyleSetName())
{
}

void FUEAgentToolCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "UE Agent", "Open the UE Agent panel.", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
