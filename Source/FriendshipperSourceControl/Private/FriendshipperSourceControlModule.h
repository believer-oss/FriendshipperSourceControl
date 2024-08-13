// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

#include "FriendshipperSourceControlSettings.h"
#include "FriendshipperSourceControlProvider.h"
#include "FriendshipperHttpRouter.h"

struct FAssetData;
class FExtender;

class FFriendshipperSourceControlModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Access the Git revision control settings */
	FFriendshipperSourceControlSettings& AccessSettings()
	{
		return FriendshipperSettings;
	}

	const FFriendshipperSourceControlSettings& AccessSettings() const
	{
		return FriendshipperSettings;
	}

	/** Save the Git revision control settings */
	void SaveSettings();

	/** Access the Git revision control provider */
	FFriendshipperSourceControlProvider& GetProvider()
	{
		return FriendshipperSourceControlProvider;
	}

	const FFriendshipperSourceControlProvider& GetProvider() const
	{
		return FriendshipperSourceControlProvider;
	}

	static const TArray<FString>& GetEmptyStringArray()
	{
		return EmptyStringArray;
	}

	/**
	 * Singleton-like access to this module's interface.  This is just for convenience!
	 * Beware of calling this during the shutdown phase, though.  Your module might have been unloaded already.
	 *
	 * @return Returns singleton instance, loading the module on demand if needed
	 */
	static FFriendshipperSourceControlModule& Get()
	{
		return FModuleManager::Get().LoadModuleChecked<FFriendshipperSourceControlModule>("FriendshipperSourceControl");
	}

	static FFriendshipperSourceControlModule* GetThreadSafe()
	{
		IModuleInterface* ModulePtr = FModuleManager::Get().GetModule("FriendshipperSourceControl");
		if (!ModulePtr && !IsEngineExitRequested())
		{
			// Main thread should never have this unloaded.
			check(!IsInGameThread());
			return nullptr;
		}
		return static_cast<FFriendshipperSourceControlModule*>(ModulePtr);
	}

	/** Set list of error messages that occurred after last git command */
	static void SetLastErrors(const TArray<FText>& InErrors);

	static void RevertIndividualFiles(const TArray<FString>& PackageNames);

private:
	static TSharedRef<FExtender> OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets);
	static void CreateGitContentBrowserAssetMenu(FMenuBuilder& MenuBuilder, const TArray<FAssetData> SelectedAssets);
	static void DiffAssetAgainstGitOriginBranch(const TArray<FAssetData> SelectedAssets, FString BranchName);
	static void DiffAgainstOriginBranch(UObject* InObject, const FString& InPackagePath, const FString& InPackageName, const FString& BranchName);
	static void RevertIndividualFiles(const TArray<FAssetData> SelectedAssets);
	static bool RevertAndReloadPackages(const TArray<FString>& InFilenames);
	static bool ApplyOperationAndReloadPackages(const TArray<FString>& InFilenames, const TFunctionRef<bool(const TArray<FString>&)>& InOperation);

	/** The one and only Git revision control provider */
	FFriendshipperSourceControlProvider FriendshipperSourceControlProvider;

	/** The settings for Git revision control */
	FFriendshipperSourceControlSettings FriendshipperSettings;

	static TArray<FString> EmptyStringArray;

	// ContentBrowserDelegate Handles
	FDelegateHandle CbdHandle_OnFilterChanged;
	FDelegateHandle CbdHandle_OnSearchBoxChanged;
	FDelegateHandle CbdHandle_OnAssetSelectionChanged;
	FDelegateHandle CbdHandle_OnSourcesViewChanged;
	FDelegateHandle CbdHandle_OnAssetPathChanged;
	FDelegateHandle CbdHandle_OnExtendAssetSelectionMenu;

	FFriendshipperHttpRouter HttpRouter;
};
