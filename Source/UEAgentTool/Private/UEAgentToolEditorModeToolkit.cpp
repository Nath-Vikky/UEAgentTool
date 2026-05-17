// Copyright Epic Games, Inc. All Rights Reserved.

#include "UEAgentToolEditorModeToolkit.h"
#include "UEAgentToolEditorMode.h"
#include "Engine/Selection.h"

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "EditorModeManager.h"

#define LOCTEXT_NAMESPACE "UEAgentToolEditorModeToolkit"

FUEAgentToolEditorModeToolkit::FUEAgentToolEditorModeToolkit()
{
}

void FUEAgentToolEditorModeToolkit::Init(const TSharedPtr<IToolkitHost>& InitToolkitHost, TWeakObjectPtr<UEdMode> InOwningMode)
{
	FModeToolkit::Init(InitToolkitHost, InOwningMode);
}

void FUEAgentToolEditorModeToolkit::GetToolPaletteNames(TArray<FName>& PaletteNames) const
{
	PaletteNames.Add(NAME_Default);
}


FName FUEAgentToolEditorModeToolkit::GetToolkitFName() const
{
	return FName("UEAgentToolEditorMode");
}

FText FUEAgentToolEditorModeToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("DisplayName", "UEAgentToolEditorMode Toolkit");
}

#undef LOCTEXT_NAMESPACE
