// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlModule.h"

#include "AssetToolsModule.h"
#include "Styling/AppStyle.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"

#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "FriendshipperHttpRouter.h"
#include "FriendshipperSourceControlOperations.h"
#include "FriendshipperSourceControlUtils.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"

// For the revert panel
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "SFriendshipperSourceControlRevert.h"
#include "Tasks/Task.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

TArray<FString> FFriendshipperSourceControlModule::EmptyStringArray;

template <typename Type>
static TSharedRef<IFriendshipperSourceControlWorker, ESPMode::ThreadSafe> CreateWorker()
{
	return MakeShareable(new Type());
}

void FFriendshipperSourceControlModule::StartupModule()
{
	// Register our operations (implemented in GitSourceControlOperations.cpp by subclassing from Engine\Source\Developer\SourceControl\Public\SourceControlOperations.h)
	FriendshipperSourceControlProvider.RegisterWorker("Connect", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperConnectWorker>));
	// Note: this provider uses the "CheckOut" command only with Git LFS 2 "lock" command, since Git itself has no lock command (all tracked files in the working copy are always already checked-out).
	FriendshipperSourceControlProvider.RegisterWorker("CheckOut", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperCheckOutWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("UpdateStatus", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperUpdateStatusWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("MarkForAdd", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperMarkForAddWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("Delete", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperDeleteWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("Revert", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperRevertWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("Fetch", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperFetchWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("CheckIn", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperCheckInWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("Copy", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperCopyWorker>));
	FriendshipperSourceControlProvider.RegisterWorker("Resolve", FGetFriendshipperSourceControlWorker::CreateStatic(&CreateWorker<FFriendshipperResolveWorker>));

	// load our settings
	FriendshipperSettings.LoadSettings();

	// Make sure we've initialized the provider
	FriendshipperSourceControlProvider.Init();

	UE::Tasks::FTask Task = UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]
		{
			FUserInfo UserInfo;
			if (FriendshipperSourceControlProvider.GetFriendshipperClient().GetUserInfo(UserInfo))
			{
				AsyncTask(ENamedThreads::GameThread, [this, UserInfo]
					{
						FriendshipperSettings.SetLfsUserName(UserInfo.Username);
						FriendshipperSourceControlProvider.UpdateSettings();
					});
			}
		});

	// Bind our revision control provider to the editor
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &FriendshipperSourceControlProvider);

	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	// Register ContentBrowserDelegate Handles for UE5 EA
	// At the time of writing this UE5 is in Early Access and has no support for revision control yet. So instead we hook into the content browser..
	// .. and force a state update on the next tick for revision control. Usually the contentbrowser assets will request this themselves, but that's not working
	// Values here are 1 or 2 based on whether the change can be done immediately or needs to be delayed as unreal needs to work through its internal delegates first
	// >> Technically you wouldn't need to use `GetOnAssetSelectionChanged` -- but it's there as a safety mechanism. States aren't forceupdated for the first path that loads
	// >> Making sure we force an update on selection change that acts like a just in case other measures fail
	CbdHandle_OnFilterChanged = ContentBrowserModule.GetOnFilterChanged().AddLambda([this](const FARFilter&, bool)
		{
			FriendshipperSourceControlProvider.TicksUntilNextForcedUpdate = 2;
		});
	CbdHandle_OnSearchBoxChanged = ContentBrowserModule.GetOnSearchBoxChanged().AddLambda([this](const FText&, bool)
		{
			FriendshipperSourceControlProvider.TicksUntilNextForcedUpdate = 1;
		});
	CbdHandle_OnAssetSelectionChanged = ContentBrowserModule.GetOnAssetSelectionChanged().AddLambda([this](const TArray<FAssetData>&, bool)
		{
			FriendshipperSourceControlProvider.TicksUntilNextForcedUpdate = 1;
		});
	CbdHandle_OnAssetPathChanged = ContentBrowserModule.GetOnAssetPathChanged().AddLambda([this](const FString&)
		{
			FriendshipperSourceControlProvider.TicksUntilNextForcedUpdate = 2;
		});

	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBAssetMenuExtenderDelegates = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	CBAssetMenuExtenderDelegates.Add(FContentBrowserMenuExtender_SelectedAssets::CreateStatic(&FFriendshipperSourceControlModule::OnExtendContentBrowserAssetSelectionMenu));
	CbdHandle_OnExtendAssetSelectionMenu = CBAssetMenuExtenderDelegates.Last().GetHandle();

	HttpRouter.OnModuleStartup();
}

void FFriendshipperSourceControlModule::ShutdownModule()
{
	// shut down the provider, as this module is going away
	FriendshipperSourceControlProvider.Close();

	// unbind provider from editor
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &FriendshipperSourceControlProvider);

	// Unregister ContentBrowserDelegate Handles
	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowserModule.GetOnFilterChanged().Remove(CbdHandle_OnFilterChanged);
	ContentBrowserModule.GetOnSearchBoxChanged().Remove(CbdHandle_OnSearchBoxChanged);
	ContentBrowserModule.GetOnAssetSelectionChanged().Remove(CbdHandle_OnAssetSelectionChanged);
	ContentBrowserModule.GetOnAssetPathChanged().Remove(CbdHandle_OnAssetPathChanged);

	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBAssetMenuExtenderDelegates = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	CBAssetMenuExtenderDelegates.RemoveAll([&ExtenderDelegateHandle = CbdHandle_OnExtendAssetSelectionMenu](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
		{
			return Delegate.GetHandle() == ExtenderDelegateHandle;
		});

	HttpRouter.OnModuleShutdown();
}

void FFriendshipperSourceControlModule::SaveSettings()
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	FriendshipperSettings.Save();
}

void FFriendshipperSourceControlModule::SetLastErrors(const TArray<FText>& InErrors)
{
	FFriendshipperSourceControlModule* Module = FModuleManager::GetModulePtr<FFriendshipperSourceControlModule>("FriendshipperSourceControl");
	if (Module)
	{
		Module->GetProvider().SetLastErrors(InErrors);
	}
}

TSharedRef<FExtender> FFriendshipperSourceControlModule::OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender(new FExtender());

	Extender->AddMenuExtension(
		"AssetSourceControlActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateStatic(&FFriendshipperSourceControlModule::CreateGitContentBrowserAssetMenu, SelectedAssets));

	return Extender;
}

void FFriendshipperSourceControlModule::CreateGitContentBrowserAssetMenu(FMenuBuilder& MenuBuilder, const TArray<FAssetData> SelectedAssets)
{
	if (!Get().GetProvider().GetStatusBranchNames().Num())
	{
		return;
	}

	MenuBuilder.BeginSection("BelieverMenu", LOCTEXT("BelieverGitMenuHeader", "Believer"));

	const TArray<FString>& StatusBranchNames = Get().GetProvider().GetStatusBranchNames();
	const FString& BranchName = StatusBranchNames[0];
	MenuBuilder.AddMenuEntry(
		FText::Format(LOCTEXT("StatusBranchDiff", "Diff against status branch"), FText::FromString(BranchName)),
		FText::Format(LOCTEXT("StatusBranchDiffDesc", "Compare this asset to the latest status branch version"), FText::FromString(BranchName)),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Diff"),
		FUIAction(FExecuteAction::CreateStatic(&FFriendshipperSourceControlModule::DiffAssetAgainstGitOriginBranch, SelectedAssets, BranchName)));

	bool bCanExecuteRevert = false;
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	if (ISourceControlModule::Get().IsEnabled())
	{
		for (const FAssetData& Asset : SelectedAssets)
		{
			// Check the SCC state for each package in the selected paths
			FSourceControlStatePtr SourceControlState = SourceControlProvider.GetState(SourceControlHelpers::PackageFilename(Asset.PackageName.ToString()), EStateCacheUsage::Use);
			if (SourceControlState.IsValid())
			{
				if (SourceControlState->CanRevert())
				{
					bCanExecuteRevert = true;
					break;
				}
			}
		}
	}

	if (bCanExecuteRevert)
	{
		MenuBuilder.AddMenuEntry(
			FText::Format(LOCTEXT("RevertReal", "Revert"), FText::FromString(BranchName)),
			FText::Format(LOCTEXT("RevertRealDesc", "Revert file correctly because Unreal is silly."), FText::FromString(BranchName)),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Revert"),
			FUIAction(FExecuteAction::CreateStatic(&FFriendshipperSourceControlModule::RevertIndividualFiles, SelectedAssets)));
	}

	MenuBuilder.EndSection();
}

void FFriendshipperSourceControlModule::DiffAssetAgainstGitOriginBranch(const TArray<FAssetData> SelectedAssets, FString BranchName)
{
	for (int32 AssetIdx = 0; AssetIdx < SelectedAssets.Num(); AssetIdx++)
	{
		// Get the actual asset (will load it)
		const FAssetData& AssetData = SelectedAssets[AssetIdx];

		if (UObject* CurrentObject = AssetData.GetAsset())
		{
			const FString PackagePath = AssetData.PackageName.ToString();
			const FString PackageName = AssetData.AssetName.ToString();
			DiffAgainstOriginBranch(CurrentObject, PackagePath, PackageName, BranchName);
		}
	}
}

void FFriendshipperSourceControlModule::DiffAgainstOriginBranch(UObject* InObject, const FString& InPackagePath, const FString& InPackageName, const FString& BranchName)
{
	check(InObject);

	const FFriendshipperSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FFriendshipperSourceControlModule>("FriendshipperSourceControl");
	const FString& PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
	const FString& PathToRepositoryRoot = GitSourceControl.GetProvider().GetPathToRepositoryRoot();

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();

	const FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

	// Get the SCC state
	const FSourceControlStatePtr SourceControlState = SourceControlProvider.GetState(SourceControlHelpers::PackageFilename(InPackagePath), EStateCacheUsage::Use);

	// If we have an asset and its in SCC..
	if (SourceControlState.IsValid() && InObject != nullptr && SourceControlState->IsSourceControlled())
	{
		// Get the file name of package
		FString RelativeFileName;
		if (FPackageName::DoesPackageExist(InPackagePath, &RelativeFileName))
		{
			// if(SourceControlState->GetHistorySize() > 0)
			{
				TArray<FString> Errors;
				const auto& Revision = FriendshipperSourceControlUtils::GetOriginRevisionOnBranch(PathToGitBinary, PathToRepositoryRoot, RelativeFileName, Errors, BranchName);

				check(Revision.IsValid());

				FString TempFileName;
				if (Revision->Get(TempFileName))
				{
					// Try and load that package
					UPackage* TempPackage = LoadPackage(nullptr, *TempFileName, LOAD_ForDiff | LOAD_DisableCompileOnLoad);
					if (TempPackage != nullptr)
					{
						// Grab the old asset from that old package
						UObject* OldObject = FindObject<UObject>(TempPackage, *InPackageName);
						if (OldObject != nullptr)
						{
							/* Set the revision information*/
							FRevisionInfo OldRevision;
							OldRevision.Changelist = Revision->GetCheckInIdentifier();
							OldRevision.Date = Revision->GetDate();
							OldRevision.Revision = Revision->GetRevision();

							FRevisionInfo NewRevision;
							NewRevision.Revision = TEXT("");

							AssetToolsModule.Get().DiffAssets(OldObject, InObject, OldRevision, NewRevision);
						}
					}
				}
			}
		}
	}
}

// Function body copied from SSourceControlRevert.cpp
void FFriendshipperSourceControlModule::RevertIndividualFiles(const TArray<FAssetData> SelectedAssets)
{
	// [BELIEVER-MOD]: Original function takes list of asset names, we use a loop here to convert from assets
	// to names.
	TArray<FString> PackageNames;
	for (TArray<FAssetData>::TConstIterator AssetIterator(SelectedAssets); AssetIterator; ++AssetIterator)
	{
		PackageNames.Push(*AssetIterator->PackageName.ToString());
	}

	RevertIndividualFiles(PackageNames);
}

void FFriendshipperSourceControlModule::RevertIndividualFiles(const TArray<FString>& PackageNames)
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();

	// Only add packages that can actually be reverted
	TArray<FString> InitialPackagesToRevert;
	for (TArray<FString>::TConstIterator PackageIter(PackageNames); PackageIter; ++PackageIter)
	{
		FSourceControlStatePtr SourceControlState = SourceControlProvider.GetState(SourceControlHelpers::PackageFilename(*PackageIter), EStateCacheUsage::Use);
		if (SourceControlState.IsValid() && SourceControlState->CanRevert())
		{
			InitialPackagesToRevert.Add(*PackageIter);
		}
	}

	// If any of the packages can be reverted, provide the revert prompt
	if (InitialPackagesToRevert.Num() > 0)
	{
		TSharedRef<SWindow> NewWindow = SNew(SWindow)
											.Title(NSLOCTEXT("SourceControl.RevertWindow", "Title", "Revert Files"))
											.SizingRule(ESizingRule::Autosized)
											.SupportsMinimize(false)
											.SupportsMaximize(false);

		TSharedRef<SFriendshipperSourceControlRevertWidget> SourceControlWidget =
			SNew(SFriendshipperSourceControlRevertWidget)
				.ParentWindow(NewWindow)
				.PackagesToRevert(InitialPackagesToRevert);

		NewWindow->SetContent(SourceControlWidget);

		FSlateApplication::Get().AddModalWindow(NewWindow, nullptr);

		// If the user decided to revert some packages, go ahead and do revert the ones they selected
		if (SourceControlWidget->GetResult() == ERevertResults::REVERT_ACCEPTED)
		{
			TArray<FString> FinalPackagesToRevert;
			SourceControlWidget->GetPackagesToRevert(FinalPackagesToRevert);

			if (FinalPackagesToRevert.Num() > 0)
			{
				RevertAndReloadPackages(FinalPackagesToRevert);
			}
		}
	}
}

// Taken from SourceControlHelpers.cpp
bool FFriendshipperSourceControlModule::ApplyOperationAndReloadPackages(const TArray<FString>& InFilenames, const TFunctionRef<bool(const TArray<FString>&)>& InOperation)
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	TArray<UPackage*> LoadedPackages;
	TArray<FString> PackageNames;
	TArray<FString> PackageFilenames;
	bool bSuccess = false;

	// Normalize packagenames and filenames
	for (const FString& Filename : InFilenames)
	{
		FString Result;

		if (FPackageName::TryConvertFilenameToLongPackageName(Filename, Result))
		{
			PackageNames.Add(MoveTemp(Result));
		}
		else
		{
			PackageNames.Add(Filename);
		}
	}

	// [BELIEVER-MOD] This loop used to exclude any world or external actor files
	for (const FString& PackageName : PackageNames)
	{
		UPackage* Package = FindPackage(nullptr, *PackageName);

		if (Package != nullptr)
		{
			LoadedPackages.Add(Package);
		}
	}

	// Prepare the packages to be reverted...
	for (UPackage* Package : LoadedPackages)
	{
		// Detach the linkers of any loaded packages so that SCC can overwrite the files...
		if (!Package->IsFullyLoaded())
		{
			FlushAsyncLoading();
			Package->FullyLoad();
		}
		ResetLoaders(Package);
	}

	PackageFilenames = SourceControlHelpers::PackageFilenames(PackageNames);

	// Apply Operation
	bSuccess = InOperation(PackageFilenames);

	// Reverting may have deleted some packages, so we need to delete those and unload them rather than re-load them...
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<UObject*> ObjectsToDelete;
	LoadedPackages.RemoveAll([&](UPackage* InPackage) -> bool
		{
			const FString PackageExtension = InPackage->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(InPackage->GetName(), PackageExtension);
			if (!FPaths::FileExists(PackageFilename))
			{
				TArray<FAssetData> Assets;
				AssetRegistryModule.Get().GetAssetsByPackageName(*InPackage->GetName(), Assets);

				for (const FAssetData& Asset : Assets)
				{
					if (UObject* ObjectToDelete = Asset.FastGetAsset())
					{
						ObjectsToDelete.Add(ObjectToDelete);
					}
				}
				return true; // remove package
			}
			return false; // keep package
		});

	// Hot-reload the new packages...
	FText OutReloadErrorMsg;
	constexpr bool bInteractive = true;
	UPackageTools::ReloadPackages(LoadedPackages, OutReloadErrorMsg, EReloadPackagesInteractionMode::Interactive);
	if (!OutReloadErrorMsg.IsEmpty())
	{
		UE_LOG(LogSourceControl, Warning, TEXT("%s"), *OutReloadErrorMsg.ToString());
	}

	// Delete and Unload assets...
	if (ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete) != ObjectsToDelete.Num())
	{
		UE_LOG(LogSourceControl, Warning, TEXT("Failed to unload some assets."));
	}

	// Re-cache the SCC state...
	SourceControlProvider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), PackageFilenames, EConcurrency::Asynchronous);

	return bSuccess;
}

bool FFriendshipperSourceControlModule::RevertAndReloadPackages(const TArray<FString>& InFilenames)
{
	auto RevertOperation = [](const TArray<FString>& InFilenames) -> bool
	{
		ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();

		auto OperationCompleteCallback = FSourceControlOperationComplete::CreateLambda([](const FSourceControlOperationRef& Operation, ECommandResult::Type InResult)
			{
				if (Operation->GetName() == TEXT("Revert"))
				{
					TSharedRef<FRevert> RevertOperation = StaticCastSharedRef<FRevert>(Operation);
					ISourceControlModule::Get().GetOnFilesDeleted().Broadcast(RevertOperation->GetDeletedFiles());
				}
			});

		return SourceControlProvider.Execute(ISourceControlOperation::Create<FRevert>(), InFilenames, EConcurrency::Synchronous, OperationCompleteCallback) == ECommandResult::Succeeded;
	};

	return ApplyOperationAndReloadPackages(InFilenames, RevertOperation);
}
IMPLEMENT_MODULE(FFriendshipperSourceControlModule, FriendshipperSourceControl);

#undef LOCTEXT_NAMESPACE
