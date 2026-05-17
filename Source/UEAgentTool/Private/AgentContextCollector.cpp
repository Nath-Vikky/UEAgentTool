// Copyright Epic Games, Inc. All Rights Reserved.

#include "AgentContextCollector.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "IContentBrowserSingleton.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"

namespace UEAgentContextCollectorPrivate
{
	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values, const int32 MaxItems = INDEX_NONE)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		const int32 Limit = MaxItems == INDEX_NONE ? Values.Num() : FMath::Min(Values.Num(), MaxItems);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			if (!Values[Index].IsEmpty())
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Values[Index]));
			}
		}
		return JsonValues;
	}

	static FString ToRelativeProjectPath(const FString& AbsolutePath, const FString& ProjectRoot)
	{
		FString RelativePath = FPaths::ConvertRelativePathToFull(AbsolutePath);
		FPaths::NormalizeFilename(RelativePath);
		FString NormalizedProjectRoot = FPaths::ConvertRelativePathToFull(ProjectRoot);
		FPaths::NormalizeFilename(NormalizedProjectRoot);
		FPaths::MakePathRelativeTo(RelativePath, *NormalizedProjectRoot);
		FPaths::NormalizeFilename(RelativePath);
		return RelativePath;
	}

	static void AddRegistryTags(const FAssetData& AssetData, const TSharedPtr<FJsonObject>& PropertiesObject)
	{
		TSharedPtr<FJsonObject> RegistryTagsObject = MakeShared<FJsonObject>();
		int32 TagCount = 0;
		AssetData.EnumerateTags([&RegistryTagsObject, &TagCount](const TPair<FName, FAssetTagValueRef>& Pair)
		{
			if (TagCount >= 48)
			{
				return;
			}

			RegistryTagsObject->SetStringField(Pair.Key.ToString(), Pair.Value.AsString());
			++TagCount;
		});

		if (TagCount > 0)
		{
			PropertiesObject->SetObjectField(TEXT("registry_tags"), RegistryTagsObject);
		}
	}

	static FString GetTagValue(const FAssetData& AssetData, const TCHAR* TagName)
	{
		return AssetData.GetTagValueRef<FString>(FName(TagName));
	}

	static void AddTagIfPresent(const FAssetData& AssetData, const TSharedPtr<FJsonObject>& TargetObject, const TCHAR* FieldName, const TCHAR* TagName)
	{
		const FString Value = GetTagValue(AssetData, TagName);
		if (!Value.IsEmpty())
		{
			TargetObject->SetStringField(FieldName, Value);
		}
	}

	static void AddCommonAssetSummaries(const FAssetData& AssetData, const TSharedPtr<FJsonObject>& SettingsObject, const TSharedPtr<FJsonObject>& PropertiesObject)
	{
		AddTagIfPresent(AssetData, SettingsObject, TEXT("nanite_enabled"), TEXT("NaniteEnabled"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("lod_group"), TEXT("LODGroup"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("compression_settings"), TEXT("CompressionSettings"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("srgb"), TEXT("SRGB"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("mip_gen_settings"), TEXT("MipGenSettings"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("texture_group"), TEXT("TextureGroup"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("blend_mode"), TEXT("BlendMode"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("shading_model"), TEXT("ShadingModel"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("parent_class"), TEXT("ParentClass"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("generated_class"), TEXT("GeneratedClass"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("skeleton"), TEXT("Skeleton"));
		AddTagIfPresent(AssetData, SettingsObject, TEXT("physics_asset"), TEXT("PhysicsAsset"));

		AddTagIfPresent(AssetData, PropertiesObject, TEXT("lod_count"), TEXT("LODCount"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("num_lods"), TEXT("NumLODs"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("triangle_count"), TEXT("NumTriangles"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("vertex_count"), TEXT("VertexCount"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("material_slots"), TEXT("MaterialSlotNames"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("row_struct"), TEXT("RowStruct"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("row_count"), TEXT("RowCount"));
		AddTagIfPresent(AssetData, PropertiesObject, TEXT("duration"), TEXT("Duration"));
	}

	static FString BlueprintStatusToString(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown:
			return TEXT("unknown");
		case BS_Dirty:
			return TEXT("dirty");
		case BS_Error:
			return TEXT("error");
		case BS_UpToDate:
			return TEXT("up_to_date");
		case BS_BeingCreated:
			return TEXT("being_created");
		case BS_UpToDateWithWarnings:
			return TEXT("up_to_date_with_warnings");
		default:
			return TEXT("unknown");
		}
	}

	static FString CollisionTraceFlagToString(const ECollisionTraceFlag CollisionFlag)
	{
		switch (CollisionFlag)
		{
		case CTF_UseDefault:
			return TEXT("project_default");
		case CTF_UseSimpleAndComplex:
			return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex:
			return TEXT("use_simple_as_complex");
		case CTF_UseComplexAsSimple:
			return TEXT("use_complex_as_simple");
		default:
			return TEXT("unknown");
		}
	}

	static FString PinTypeToInventoryString(const FEdGraphPinType& PinType)
	{
		FString TypeText = PinType.PinCategory.ToString();
		if (!PinType.PinSubCategory.IsNone())
		{
			TypeText += FString::Printf(TEXT(":%s"), *PinType.PinSubCategory.ToString());
		}
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeText += FString::Printf(TEXT(":%s"), *PinType.PinSubCategoryObject->GetPathName());
		}
		if (PinType.IsArray())
		{
			TypeText = FString::Printf(TEXT("array<%s>"), *TypeText);
		}
		else if (PinType.IsSet())
		{
			TypeText = FString::Printf(TEXT("set<%s>"), *TypeText);
		}
		else if (PinType.IsMap())
		{
			TypeText = FString::Printf(TEXT("map<%s>"), *TypeText);
		}
		return TypeText;
	}

	static void AddGraphNames(const TArray<TObjectPtr<UEdGraph>>& Graphs, TArray<FString>& OutNames)
	{
		for (const TObjectPtr<UEdGraph>& Graph : Graphs)
		{
			if (Graph != nullptr)
			{
				OutNames.AddUnique(Graph->GetName());
			}
		}
	}

	static void AddStaticMeshInventoryDetails(const UStaticMesh* StaticMesh, const TSharedPtr<FJsonObject>& SettingsObject)
	{
		if (StaticMesh == nullptr)
		{
			return;
		}

#if WITH_EDITORONLY_DATA
		SettingsObject->SetBoolField(TEXT("nanite_enabled"), StaticMesh->NaniteSettings.bEnabled);
#endif
		SettingsObject->SetNumberField(TEXT("lod_count"), StaticMesh->GetNumLODs());
		SettingsObject->SetNumberField(TEXT("lightmap_resolution"), StaticMesh->GetLightMapResolution());

		if (const UBodySetup* BodySetup = StaticMesh->GetBodySetup())
		{
			SettingsObject->SetStringField(TEXT("collision_complexity"), CollisionTraceFlagToString(BodySetup->CollisionTraceFlag));
		}
	}

	static void AddBlueprintInventoryDetails(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& AssetObject)
	{
		if (Blueprint == nullptr)
		{
			return;
		}

		TSharedPtr<FJsonObject> BlueprintObject = MakeShared<FJsonObject>();
		if (Blueprint->ParentClass != nullptr)
		{
			BlueprintObject->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> ComponentValues;
		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			const TArray<USCS_Node*>& Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
			for (const USCS_Node* Node : Nodes)
			{
				if (Node == nullptr)
				{
					continue;
				}

				TSharedPtr<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
				ComponentObject->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
				if (Node->ComponentTemplate != nullptr)
				{
					ComponentObject->SetStringField(TEXT("component_class"), Node->ComponentTemplate->GetClass()->GetPathName());
				}

				if (const USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(const_cast<USCS_Node*>(Node)))
				{
					ComponentObject->SetStringField(TEXT("attach_to"), ParentNode->GetVariableName().ToString());
				}

				ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
				if (ComponentValues.Num() >= 64)
				{
					break;
				}
			}
		}
		BlueprintObject->SetArrayField(TEXT("components"), ComponentValues);

		TArray<TSharedPtr<FJsonValue>> VariableValues;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
			VariableObject->SetStringField(TEXT("variable_name"), Variable.VarName.ToString());
			VariableObject->SetStringField(TEXT("variable_type"), PinTypeToInventoryString(Variable.VarType));
			VariableObject->SetStringField(TEXT("category"), Variable.Category.ToString());
			if (!Variable.DefaultValue.IsEmpty())
			{
				VariableObject->SetStringField(TEXT("default_value"), Variable.DefaultValue);
			}
			VariableValues.Add(MakeShared<FJsonValueObject>(VariableObject));
			if (VariableValues.Num() >= 64)
			{
				break;
			}
		}
		BlueprintObject->SetArrayField(TEXT("variables"), VariableValues);

#if WITH_EDITORONLY_DATA
		TArray<FString> FunctionNames;
		TArray<FString> GraphNames;
		AddGraphNames(Blueprint->FunctionGraphs, FunctionNames);
		AddGraphNames(Blueprint->UbergraphPages, GraphNames);
		AddGraphNames(Blueprint->FunctionGraphs, GraphNames);
		AddGraphNames(Blueprint->MacroGraphs, GraphNames);
		BlueprintObject->SetArrayField(TEXT("functions"), MakeStringArray(FunctionNames, 64));
		BlueprintObject->SetArrayField(TEXT("graphs"), MakeStringArray(GraphNames, 96));

		TArray<FString> InterfaceNames;
		for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDescription.Interface != nullptr)
			{
				InterfaceNames.Add(InterfaceDescription.Interface->GetPathName());
			}
		}
		BlueprintObject->SetArrayField(TEXT("interfaces"), MakeStringArray(InterfaceNames, 32));
#endif

		TSharedPtr<FJsonObject> EditorFlagsObject = MakeShared<FJsonObject>();
		EditorFlagsObject->SetStringField(TEXT("status"), BlueprintStatusToString(Blueprint->Status));
		EditorFlagsObject->SetBoolField(TEXT("is_dirty"), Blueprint->GetPackage() != nullptr && Blueprint->GetPackage()->IsDirty());
		EditorFlagsObject->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint));
		EditorFlagsObject->SetBoolField(TEXT("force_full_editor"), Blueprint->bForceFullEditor);
		EditorFlagsObject->SetBoolField(TEXT("is_newly_created"), Blueprint->bIsNewlyCreated);
		BlueprintObject->SetObjectField(TEXT("editor_flags"), EditorFlagsObject);

		AssetObject->SetObjectField(TEXT("blueprint"), BlueprintObject);
	}

	static bool IsValidSymbolToken(const FString& Token)
	{
		if (Token.IsEmpty() || Token.Contains(TEXT("(")) || Token.Contains(TEXT("=")))
		{
			return false;
		}

		const FString LowerToken = Token.ToLower();
		return LowerToken != TEXT("final")
			&& LowerToken != TEXT("public")
			&& LowerToken != TEXT("private")
			&& LowerToken != TEXT("protected")
			&& LowerToken != TEXT("virtual");
	}

	static void AddSymbolFromKeyword(const FString& Line, const FString& Keyword, TArray<FString>& Symbols)
	{
		int32 KeywordIndex = INDEX_NONE;
		if (!Line.FindChar(Keyword[0], KeywordIndex))
		{
			return;
		}

		KeywordIndex = Line.Find(Keyword, ESearchCase::CaseSensitive);
		if (KeywordIndex == INDEX_NONE)
		{
			return;
		}

		FString Tail = Line.Mid(KeywordIndex + Keyword.Len()).TrimStartAndEnd();
		Tail.ReplaceInline(TEXT("{"), TEXT(" "));
		Tail.ReplaceInline(TEXT(":"), TEXT(" "));
		Tail.ReplaceInline(TEXT(";"), TEXT(" "));
		Tail.ReplaceInline(TEXT(","), TEXT(" "));

		TArray<FString> Tokens;
		Tail.ParseIntoArrayWS(Tokens);
		for (FString Token : Tokens)
		{
			Token.TrimStartAndEndInline();
			if (Token.EndsWith(TEXT("_API")) || !IsValidSymbolToken(Token))
			{
				continue;
			}
			Symbols.AddUnique(Token);
			return;
		}
	}
}

FUEAgentContextSummary FUEAgentContextCollector::CaptureContext() const
{
	FUEAgentContextSummary Context;
	Context.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Context.ProjectName = FApp::GetProjectName();
	Context.ActiveModule = DeriveActiveModule(Context.ProjectName);
	Context.BackendVersion = TEXT("unknown");
	Context.KnowledgeBaseStatus = TEXT("Unknown");

	const FString SourceRoot = FPaths::ProjectDir() / TEXT("Source");
	if (IFileManager::Get().DirectoryExists(*SourceRoot))
	{
		TArray<FString> BuildFiles;
		IFileManager::Get().FindFilesRecursive(BuildFiles, *SourceRoot, TEXT("*.Build.cs"), true, false);
		if (BuildFiles.Num() > 0)
		{
			Context.CurrentFile = FPaths::ConvertRelativePathToFull(BuildFiles[0]);
		}
	}

	if (!Context.CurrentFile.IsEmpty())
	{
		Context.RecentOpenFiles.Add(Context.CurrentFile);
	}

	TArray<FAssetData> SelectedAssets;
	if (FModuleManager::Get().ModuleExists(TEXT("ContentBrowser")))
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
	}

	IAssetRegistry* AssetRegistry = nullptr;
	if (FModuleManager::Get().ModuleExists(TEXT("AssetRegistry")))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistry = &AssetRegistryModule.Get();
	}

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (AssetData.IsValid())
		{
			FUEAgentAssetContextItem AssetItem;
			AssetItem.AssetName = AssetData.AssetName.ToString();
			AssetItem.AssetPath = AssetData.GetSoftObjectPath().ToString();
			AssetItem.AssetType = AssetData.AssetClassPath.GetAssetName().ToString();
			AssetItem.PackagePath = AssetData.PackageName.ToString();
			Context.SelectedAssets.Add(AssetItem.AssetPath);

			if (AssetRegistry != nullptr)
			{
				TArray<FName> DependencyNames;
				AssetRegistry->GetDependencies(AssetData.PackageName, DependencyNames);
				for (int32 Index = 0; Index < DependencyNames.Num() && Index < 32; ++Index)
				{
					AssetItem.Dependencies.Add(DependencyNames[Index].ToString());
				}

				TArray<FName> ReferencerNames;
				AssetRegistry->GetReferencers(AssetData.PackageName, ReferencerNames);
				for (int32 Index = 0; Index < ReferencerNames.Num() && Index < 32; ++Index)
				{
					AssetItem.Referencers.Add(ReferencerNames[Index].ToString());
				}
			}

			Context.SelectedAssetItems.Add(AssetItem);
		}
	}

	return Context;
}

FString FUEAgentContextCollector::DeriveActiveModule(const FString& ProjectName) const
{
	const FString SourceRoot = FPaths::ProjectDir() / TEXT("Source");
	if (!IFileManager::Get().DirectoryExists(*SourceRoot))
	{
		return ProjectName;
	}

	TArray<FString> ModuleBuildFiles;
	IFileManager::Get().FindFilesRecursive(ModuleBuildFiles, *SourceRoot, TEXT("*.Build.cs"), true, false);
	if (ModuleBuildFiles.Num() == 0)
	{
		return ProjectName;
	}

	return FPaths::GetBaseFilename(ModuleBuildFiles[0]).Replace(TEXT(".Build"), TEXT(""));
}

FString FUEAgentContextCollector::DeriveModuleFromRelativePath(const FString& RelativePath, const FString& ProjectName) const
{
	TArray<FString> PathParts;
	RelativePath.ParseIntoArray(PathParts, TEXT("/"), true);

	if (PathParts.Num() > 1 && PathParts[0].Equals(TEXT("Source"), ESearchCase::IgnoreCase))
	{
		return PathParts[1];
	}

	if (PathParts.Num() > 1 && PathParts[0].Equals(TEXT("Plugins"), ESearchCase::IgnoreCase))
	{
		const int32 SourceIndex = PathParts.Find(FString(TEXT("Source")));
		if (SourceIndex != INDEX_NONE && PathParts.IsValidIndex(SourceIndex + 1))
		{
			return PathParts[SourceIndex + 1];
		}
		return PathParts[1];
	}

	return ProjectName;
}

TArray<FString> FUEAgentContextCollector::ExtractCodeSymbols(const FString& AbsoluteFilePath) const
{
	FString SourceText;
	const FFileStatData StatData = IFileManager::Get().GetStatData(*AbsoluteFilePath);
	if (StatData.FileSize > 1024 * 1024 || !FFileHelper::LoadFileToString(SourceText, *AbsoluteFilePath))
	{
		return {};
	}

	TArray<FString> Symbols;
	TArray<FString> Lines;
	SourceText.ParseIntoArrayLines(Lines, false);
	for (FString Line : Lines)
	{
		int32 CommentIndex = INDEX_NONE;
		if (Line.FindChar(TEXT('/'), CommentIndex))
		{
			const int32 DoubleSlashIndex = Line.Find(TEXT("//"), ESearchCase::CaseSensitive);
			if (DoubleSlashIndex != INDEX_NONE)
			{
				Line = Line.Left(DoubleSlashIndex);
			}
		}

		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("//")))
		{
			continue;
		}

		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("enum class "), Symbols);
		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("class "), Symbols);
		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("struct "), Symbols);
		UEAgentContextCollectorPrivate::AddSymbolFromKeyword(Line, TEXT("interface "), Symbols);

		if (Symbols.Num() >= 32)
		{
			break;
		}
	}

	if (Symbols.Num() > 32)
	{
		Symbols.SetNum(32);
	}
	return Symbols;
}

TSharedPtr<FJsonObject> FUEAgentContextCollector::BuildProjectInventorySnapshot(const int32 MaxAssets, const int32 MaxCodeFiles) const
{
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString ProjectName = FApp::GetProjectName();

	TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
	SnapshotObject->SetStringField(TEXT("project_id"), ProjectName);
	SnapshotObject->SetStringField(TEXT("project_name"), ProjectName);
	SnapshotObject->SetStringField(TEXT("source"), TEXT("ue_plugin"));
	SnapshotObject->SetStringField(TEXT("snapshot_time"), FDateTime::UtcNow().ToIso8601());

	TSharedPtr<FJsonObject> ScanDiagnosticsObject = MakeShared<FJsonObject>();
	ScanDiagnosticsObject->SetStringField(TEXT("project_root"), ProjectRoot);
	ScanDiagnosticsObject->SetNumberField(TEXT("max_assets"), MaxAssets);
	ScanDiagnosticsObject->SetNumberField(TEXT("max_code_files"), MaxCodeFiles);

	TArray<TSharedPtr<FJsonValue>> AssetValues;
	if (FModuleManager::Get().ModuleExists(TEXT("AssetRegistry")))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> ProjectAssets;
		AssetRegistry.GetAssetsByPath(FName(TEXT("/Game")), ProjectAssets, true);
		ProjectAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.PackageName.ToString() < Right.PackageName.ToString();
		});

		for (const FAssetData& AssetData : ProjectAssets)
		{
			if (!AssetData.IsValid() || (MaxAssets > 0 && AssetValues.Num() >= MaxAssets))
			{
				continue;
			}

			TArray<FName> DependencyNames;
			AssetRegistry.GetDependencies(AssetData.PackageName, DependencyNames);
			TArray<FString> Dependencies;
			for (const FName& DependencyName : DependencyNames)
			{
				Dependencies.Add(DependencyName.ToString());
			}

			TArray<FName> ReferencerNames;
			AssetRegistry.GetReferencers(AssetData.PackageName, ReferencerNames);
			TArray<FString> Referencers;
			for (const FName& ReferencerName : ReferencerNames)
			{
				Referencers.Add(ReferencerName.ToString());
			}

			TSharedPtr<FJsonObject> SettingsObject = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UEAgentContextCollectorPrivate::AddCommonAssetSummaries(AssetData, SettingsObject, PropertiesObject);
			UEAgentContextCollectorPrivate::AddRegistryTags(AssetData, PropertiesObject);

			TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
			AssetObject->SetStringField(TEXT("asset_path"), AssetData.GetSoftObjectPath().ToString());
			AssetObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
			const FString AssetType = AssetData.AssetClassPath.GetAssetName().ToString();
			AssetObject->SetStringField(TEXT("asset_type"), AssetType);
			AssetObject->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
			AssetObject->SetArrayField(TEXT("dependencies"), UEAgentContextCollectorPrivate::MakeStringArray(Dependencies, 64));
			AssetObject->SetArrayField(TEXT("referencers"), UEAgentContextCollectorPrivate::MakeStringArray(Referencers, 64));

			if (AssetType.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase) || AssetType.Equals(TEXT("Blueprint"), ESearchCase::IgnoreCase) || AssetType.Contains(TEXT("Blueprint")))
			{
				UObject* LoadedAsset = AssetData.GetAsset();
				UEAgentContextCollectorPrivate::AddStaticMeshInventoryDetails(Cast<UStaticMesh>(LoadedAsset), SettingsObject);
				UEAgentContextCollectorPrivate::AddBlueprintInventoryDetails(Cast<UBlueprint>(LoadedAsset), AssetObject);
			}

			AssetObject->SetObjectField(TEXT("settings"), SettingsObject);
			AssetObject->SetObjectField(TEXT("properties"), PropertiesObject);
			AssetValues.Add(MakeShared<FJsonValueObject>(AssetObject));
		}

		ScanDiagnosticsObject->SetNumberField(TEXT("asset_registry_count"), ProjectAssets.Num());
		ScanDiagnosticsObject->SetBoolField(TEXT("asset_limit_reached"), MaxAssets > 0 && ProjectAssets.Num() > MaxAssets);
	}
	else
	{
		ScanDiagnosticsObject->SetStringField(TEXT("asset_scan_status"), TEXT("asset_registry_unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> CodeFileValues;
	TArray<FString> CandidateFiles;
	const TArray<FString> SourceRoots = { ProjectRoot / TEXT("Source"), ProjectRoot / TEXT("Plugins") };
	const TArray<FString> Patterns = { TEXT("*.h"), TEXT("*.hpp"), TEXT("*.hh"), TEXT("*.inl"), TEXT("*.cpp"), TEXT("*.cxx"), TEXT("*.cs") };
	for (const FString& SourceRoot : SourceRoots)
	{
		if (!IFileManager::Get().DirectoryExists(*SourceRoot))
		{
			continue;
		}

		for (const FString& Pattern : Patterns)
		{
			IFileManager::Get().FindFilesRecursive(CandidateFiles, *SourceRoot, *Pattern, true, false);
		}
	}

	CandidateFiles.Sort();
	for (const FString& CandidateFile : CandidateFiles)
	{
		if (MaxCodeFiles > 0 && CodeFileValues.Num() >= MaxCodeFiles)
		{
			break;
		}

		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(CandidateFile);
		const FString RelativePath = UEAgentContextCollectorPrivate::ToRelativeProjectPath(AbsolutePath, ProjectRoot);
		const FFileStatData StatData = IFileManager::Get().GetStatData(*AbsolutePath);

		TSharedPtr<FJsonObject> CodeFileObject = MakeShared<FJsonObject>();
		CodeFileObject->SetStringField(TEXT("file_path"), RelativePath);
		CodeFileObject->SetStringField(TEXT("module_name"), DeriveModuleFromRelativePath(RelativePath, ProjectName));
		CodeFileObject->SetStringField(TEXT("file_type"), FPaths::GetExtension(RelativePath).ToLower());
		CodeFileObject->SetNumberField(TEXT("size_bytes"), StatData.FileSize);
		CodeFileObject->SetStringField(TEXT("last_modified"), StatData.ModificationTime.ToIso8601());
		CodeFileObject->SetStringField(TEXT("modified_at"), StatData.ModificationTime.ToIso8601());
		const TArray<FString> Symbols = ExtractCodeSymbols(AbsolutePath);
		CodeFileObject->SetArrayField(TEXT("classes"), UEAgentContextCollectorPrivate::MakeStringArray(Symbols, 32));
		CodeFileObject->SetArrayField(TEXT("symbols"), UEAgentContextCollectorPrivate::MakeStringArray(Symbols, 32));
		CodeFileValues.Add(MakeShared<FJsonValueObject>(CodeFileObject));
	}

	ScanDiagnosticsObject->SetNumberField(TEXT("code_file_scan_count"), CandidateFiles.Num());
	ScanDiagnosticsObject->SetBoolField(TEXT("code_file_limit_reached"), MaxCodeFiles > 0 && CandidateFiles.Num() > MaxCodeFiles);

	SnapshotObject->SetArrayField(TEXT("assets"), AssetValues);
	SnapshotObject->SetArrayField(TEXT("code_files"), CodeFileValues);
	SnapshotObject->SetObjectField(TEXT("scan_diagnostics"), ScanDiagnosticsObject);
	return SnapshotObject;
}
