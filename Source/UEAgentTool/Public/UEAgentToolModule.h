// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AgentContextCollector.h"
#include "AgentHttpClient.h"
#include "AgentStateStore.h"
#include "CoreMinimal.h"
#include "Framework/Commands/UICommandList.h"
#include "Modules/ModuleManager.h"

/**
 * UE Agent editor module with a dockable Slate workbench.
 */
class FUEAgentToolModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static const FName MainTabId;

private:
	void RegisterMenus();
	void UnregisterMenus();
	void PluginButtonClicked();
	TSharedRef<class SDockTab> SpawnMainTab(const class FSpawnTabArgs& SpawnTabArgs);

private:
	TSharedPtr<FUICommandList> PluginCommands;
	TSharedPtr<FUEAgentStateStore> StateStore;
	TSharedPtr<FUEAgentHttpClient> HttpClient;
	TSharedPtr<FUEAgentContextCollector> ContextCollector;
};
