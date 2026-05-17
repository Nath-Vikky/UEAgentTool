// Copyright Epic Games, Inc. All Rights Reserved.

#include "UEAgentToolModule.h"

#include "AgentCommands.h"
#include "AgentStyle.h"
#include "Framework/Docking/TabManager.h"
#include "SAgentRootPanel.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FUEAgentToolModule"

const FName FUEAgentToolModule::MainTabId(TEXT("UEAgentTool.MainTab"));

void FUEAgentToolModule::StartupModule()
{
	FUEAgentToolStyle::Initialize();
	FUEAgentToolCommands::Register();

	PluginCommands = MakeShared<FUICommandList>();
	PluginCommands->MapAction(
		FUEAgentToolCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FUEAgentToolModule::PluginButtonClicked),
		FCanExecuteAction());

	StateStore = MakeShared<FUEAgentStateStore>();
	HttpClient = MakeShared<FUEAgentHttpClient>(StateStore.ToSharedRef());
	ContextCollector = MakeShared<FUEAgentContextCollector>();
	EditorToolServer = MakeShared<FUEAgentEditorToolServer>();
	EditorToolServer->StartFromConfig();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		MainTabId,
		FOnSpawnTab::CreateRaw(this, &FUEAgentToolModule::SpawnMainTab))
		.SetDisplayName(LOCTEXT("UEAgentToolTabTitle", "UE Agent"))
		.SetTooltipText(LOCTEXT("UEAgentToolTabTooltip", "Open the UE Agent workbench."))
		.SetIcon(FSlateIcon(FUEAgentToolStyle::GetStyleSetName(), "UEAgentTool.TabIcon"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUEAgentToolModule::RegisterMenus));
}

void FUEAgentToolModule::ShutdownModule()
{
	UnregisterMenus();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(MainTabId);
	FUEAgentToolCommands::Unregister();
	FUEAgentToolStyle::Shutdown();

	if (EditorToolServer.IsValid())
	{
		EditorToolServer->Stop();
	}
	EditorToolServer.Reset();
	ContextCollector.Reset();
	HttpClient.Reset();
	StateStore.Reset();
	PluginCommands.Reset();
}

void FUEAgentToolModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
	FToolMenuSection& WindowSection = WindowMenu->FindOrAddSection(TEXT("WindowLayout"));
	WindowSection.AddMenuEntryWithCommandList(FUEAgentToolCommands::Get().OpenPluginWindow, PluginCommands);

	UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
	FToolMenuSection& ToolbarSection = ToolbarMenu->FindOrAddSection(TEXT("PluginTools"));

	FToolMenuEntry ToolbarEntry = FToolMenuEntry::InitToolBarButton(
		FUEAgentToolCommands::Get().OpenPluginWindow,
		LOCTEXT("UEAgentToolbarLabel", "UE Agent"),
		LOCTEXT("UEAgentToolbarTooltip", "Open the UE Agent workbench."),
		FSlateIcon(FUEAgentToolStyle::GetStyleSetName(), "UEAgentTool.OpenPluginWindow"));
	ToolbarEntry.SetCommandList(PluginCommands);
	ToolbarSection.AddEntry(ToolbarEntry);
}

void FUEAgentToolModule::UnregisterMenus()
{
	UToolMenus::UnRegisterStartupCallback(this);
	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		ToolMenus->UnregisterOwner(this);
	}
}

void FUEAgentToolModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(MainTabId);
}

TSharedRef<SDockTab> FUEAgentToolModule::SpawnMainTab(const FSpawnTabArgs& SpawnTabArgs)
{
	check(StateStore.IsValid());
	check(HttpClient.IsValid());
	check(ContextCollector.IsValid());

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("UEAgentDockTabLabel", "UE Agent"))
		[
			SNew(SAgentRootPanel)
			.StateStore(StateStore)
			.HttpClient(HttpClient)
			.ContextCollector(ContextCollector)
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUEAgentToolModule, UEAgentTool)
