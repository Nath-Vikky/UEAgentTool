// Copyright Epic Games, Inc. All Rights Reserved.

#include "UEAgentToolEditorMode.h"
#include "UEAgentToolEditorModeToolkit.h"
#include "EdModeInteractiveToolsContext.h"
#include "InteractiveToolManager.h"
#include "UEAgentToolEditorModeCommands.h"


//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////// 
// AddYourTool Step 1 - include the header file for your Tools here
//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////// 
#include "Tools/UEAgentToolSimpleTool.h"
#include "Tools/UEAgentToolInteractiveTool.h"

// step 2: register a ToolBuilder in FUEAgentToolEditorMode::Enter() below


#define LOCTEXT_NAMESPACE "UEAgentToolEditorMode"

const FEditorModeID UUEAgentToolEditorMode::EM_UEAgentToolEditorModeId = TEXT("EM_UEAgentToolEditorMode");

FString UUEAgentToolEditorMode::SimpleToolName = TEXT("UEAgentTool_ActorInfoTool");
FString UUEAgentToolEditorMode::InteractiveToolName = TEXT("UEAgentTool_MeasureDistanceTool");


UUEAgentToolEditorMode::UUEAgentToolEditorMode()
{
	FModuleManager::Get().LoadModule("EditorStyle");

	// appearance and icon in the editing mode ribbon can be customized here
	Info = FEditorModeInfo(UUEAgentToolEditorMode::EM_UEAgentToolEditorModeId,
		LOCTEXT("ModeName", "UEAgentTool"),
		FSlateIcon(),
		true);
}


UUEAgentToolEditorMode::~UUEAgentToolEditorMode()
{
}


void UUEAgentToolEditorMode::ActorSelectionChangeNotify()
{
}

void UUEAgentToolEditorMode::Enter()
{
	UEdMode::Enter();

	//////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////
	// AddYourTool Step 2 - register the ToolBuilders for your Tools here.
	// The string name you pass to the ToolManager is used to select/activate your ToolBuilder later.
	//////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////// 
	const FUEAgentToolEditorModeCommands& SampleToolCommands = FUEAgentToolEditorModeCommands::Get();

	RegisterTool(SampleToolCommands.SimpleTool, SimpleToolName, NewObject<UUEAgentToolSimpleToolBuilder>(this));
	RegisterTool(SampleToolCommands.InteractiveTool, InteractiveToolName, NewObject<UUEAgentToolInteractiveToolBuilder>(this));

	// active tool type is not relevant here, we just set to default
	GetToolManager()->SelectActiveToolType(EToolSide::Left, SimpleToolName);
}

void UUEAgentToolEditorMode::CreateToolkit()
{
	Toolkit = MakeShareable(new FUEAgentToolEditorModeToolkit);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> UUEAgentToolEditorMode::GetModeCommands() const
{
	return FUEAgentToolEditorModeCommands::Get().GetCommands();
}

#undef LOCTEXT_NAMESPACE
