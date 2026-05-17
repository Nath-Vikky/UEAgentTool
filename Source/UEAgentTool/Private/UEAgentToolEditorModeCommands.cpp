// Copyright Epic Games, Inc. All Rights Reserved.

#include "UEAgentToolEditorModeCommands.h"
#include "UEAgentToolEditorMode.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UEAgentToolEditorModeCommands"

FUEAgentToolEditorModeCommands::FUEAgentToolEditorModeCommands()
	: TCommands<FUEAgentToolEditorModeCommands>("UEAgentToolEditorMode",
		NSLOCTEXT("UEAgentToolEditorMode", "UEAgentToolEditorModeCommands", "UEAgentTool Editor Mode"),
		NAME_None,
		FAppStyle::GetAppStyleSetName())
{
}

void FUEAgentToolEditorModeCommands::RegisterCommands()
{
	TArray <TSharedPtr<FUICommandInfo>>& ToolCommands = Commands.FindOrAdd(NAME_Default);

	UI_COMMAND(SimpleTool, "Show Actor Info", "Opens message box with info about a clicked actor", EUserInterfaceActionType::Button, FInputChord());
	ToolCommands.Add(SimpleTool);

	UI_COMMAND(InteractiveTool, "Measure Distance", "Measures distance between 2 points (click to set origin, shift-click to set end point)", EUserInterfaceActionType::ToggleButton, FInputChord());
	ToolCommands.Add(InteractiveTool);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FUEAgentToolEditorModeCommands::GetCommands()
{
	return FUEAgentToolEditorModeCommands::Get().Commands;
}

#undef LOCTEXT_NAMESPACE
