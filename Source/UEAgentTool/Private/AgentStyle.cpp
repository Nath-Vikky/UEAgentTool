// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyleRegistry.h"

TSharedPtr<FSlateStyleSet> FUEAgentToolStyle::StyleSet = nullptr;

FName FUEAgentToolStyle::GetStyleSetName()
{
	static FName StyleName(TEXT("UEAgentToolStyle"));
	return StyleName;
}

void FUEAgentToolStyle::Initialize()
{
	if (!StyleSet.IsValid())
	{
		StyleSet = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
	}
}

void FUEAgentToolStyle::Shutdown()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		ensure(StyleSet.IsUnique());
		StyleSet.Reset();
	}
}

void FUEAgentToolStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FUEAgentToolStyle::Get()
{
	return *StyleSet;
}

TSharedRef<FSlateStyleSet> FUEAgentToolStyle::Create()
{
	const FVector2D Icon16x16(16.0f, 16.0f);
	const FVector2D Icon20x20(20.0f, 20.0f);
	const FVector2D Icon40x40(40.0f, 40.0f);

	TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAgentTool"));
	const FString ResourcePath = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Resources") : FPaths::ProjectPluginsDir() / TEXT("UEAgentTool/Resources");

	Style->SetContentRoot(ResourcePath);
	Style->Set(TEXT("UEAgentTool.OpenPluginWindow"), new FSlateImageBrush(Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")), Icon20x20));
	Style->Set(TEXT("UEAgentTool.TabIcon"), new FSlateImageBrush(Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")), Icon16x16));
	Style->Set(TEXT("UEAgentTool.LargeIcon"), new FSlateImageBrush(Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")), Icon40x40));

	return Style;
}
