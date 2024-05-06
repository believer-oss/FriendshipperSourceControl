// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlOperations.h"

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"
#include "ISourceControlModule.h"
#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlCommand.h"
#include "FriendshipperSourceControlUtils.h"
#include "SourceControlHelpers.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

#include <thread>

#include "FriendshipperClient.h"
#include "UObject/PackageTrailer.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

static bool LockFiles(const FString& PathToGitRoot, const TArray<FString>& Files, TMap<const FString, FFriendshipperState>& States, TArray<FString>* ErrorMessages)
{
	if (Files.IsEmpty())
	{
		return true;
	}

	TArray<FString> LockableFiles = Files.FilterByPredicate(FriendshipperSourceControlUtils::IsFileLFSLockable);
	if (LockableFiles.IsEmpty())
	{
		return true;
	}

	// lock paths should be relative to the repo root to ensure people with repos in different locations can
	// create the same lock path
	const TArray<FString> LockableRelativeFiles = FriendshipperSourceControlUtils::RelativeFilenames(LockableFiles, PathToGitRoot);

	FFriendshipperSourceControlProvider& Provider = FFriendshipperSourceControlModule::Get().GetProvider();
	FFriendshipperClient& Client = Provider.GetFriendshipperClient();

	TArray<FString> FailedRelativeFiles;
	const bool bSuccess = Client.LockFiles(LockableRelativeFiles, &FailedRelativeFiles, ErrorMessages);

	TArray<FString> SucceededFiles;
	for (FString& LockableFile : LockableFiles)
	{
		bool bWasLocked = true;
		for (FString& FailedRelativeFile : FailedRelativeFiles)
		{
			if (LockableFile.EndsWith(FailedRelativeFile))
			{
				bWasLocked = false;
				break;
			}
		}

		if (bWasLocked)
		{
			SucceededFiles.Emplace(MoveTemp(LockableFile));
		}
	}

	if (bSuccess)
	{
		const FString& LockUser = FFriendshipperSourceControlModule::Get().GetProvider().GetLockUser();
		for (const FString& AbsoluteFile : SucceededFiles)
		{
			 FFriendshipperLockedFilesCache::AddLockedFile(AbsoluteFile, LockUser);
		}

		FriendshipperSourceControlUtils::CollectNewStates(SucceededFiles, States, EFileState::Unset, ETreeState::Unset, ELockState::Locked);
		for (auto&& State : States)
		{
			State.Value.LockUser = LockUser;
		}
	}

	return bSuccess;
}


FName FFriendshipperConnectWorker::GetName() const
{
	return "Connect";
}

bool FFriendshipperConnectWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	// The connect worker checks if we are connected to the remote server.
	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FConnect, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FConnect>(InCommand.Operation);

	FFriendshipperClient& Client = FFriendshipperSourceControlModule::Get().GetProvider().GetFriendshipperClient();
	if (!Client.CheckSystemStatus())
	{
		const FText& UnableToConnect = LOCTEXT("FriendshipperNotFound", "Unable to connect to Friendshipper. Please make sure it's running and try again.");
		InCommand.ResultInfo.ErrorMessages.Add(UnableToConnect.ToString());
		Operation->SetErrorText(UnableToConnect);
		return false;
	}

	// Skip login operations, since Git does not have to login.
	// It's not a big deal for async commands though, so let those go through.
	// More information: this is a heuristic for cases where UE is trying to create
	// a valid Perforce connection as a side effect for the connect worker. For Git,
	// the connect worker has no side effects. It is simply a query to retrieve information
	// to be displayed to the user, like in the revision control settings or on init.
	// Therefore, there is no need for synchronously establishing a connection if not there.
	if (InCommand.Concurrency == EConcurrency::Synchronous)
	{
		return true;
	}

	// Check Git availability
	// We already know that Git is available if PathToGitBinary is not empty, since it is validated then.
	if (InCommand.PathToGitBinary.IsEmpty())
	{
		const FText& NotFound = LOCTEXT("GitNotFound", "Failed to enable Git revision control. You need to install Git and ensure the plugin has a valid path to the git executable.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
		return false;
	}

	// TODO: always return true, and enter an offline mode if could not connect to remote
	return true;
}

bool FFriendshipperConnectWorker::UpdateStates() const
{
	return false;
}

FName FFriendshipperCheckOutWorker::GetName() const
{
	return "CheckOut";
}

bool FFriendshipperCheckOutWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	return LockFiles(InCommand.PathToGitRoot, InCommand.Files, States, &InCommand.ResultInfo.ErrorMessages);
}

bool FFriendshipperCheckOutWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}

static FText ParseCommitResults(const TArray<FString>& InResults)
{
	if (InResults.Num() >= 1)
	{
		const FString& FirstLine = InResults[0];
		return FText::Format(LOCTEXT("CommitMessage", "Commited {0}."), FText::FromString(FirstLine));
	}
	return LOCTEXT("CommitMessageUnknown", "Submitted revision.");
}

FName FFriendshipperCheckInWorker::GetName() const
{
	return "CheckIn";
}

const FText EmptyCommitMsg;

bool FFriendshipperCheckInWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);

	const TArray<FString> FilesToCommit = FriendshipperSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);

	bool bDoCommit = InCommand.Files.Num() > 0;
	FFriendshipperSourceControlProvider& Provider = FFriendshipperSourceControlModule::Get().GetProvider();

	FFriendshipperClient& Client = Provider.GetFriendshipperClient();

	UE_LOG(LogSourceControl, Log, TEXT("Running Friendshipper quick submit!"))

	if (Client.Submit(Operation->GetDescription().ToString(), FilesToCommit) == false)
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to run Friendshipper quick submit"))

		return false;
	}

	// Remove any deleted files from status cache
	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(InCommand.Files, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		if (State->IsDeleted())
		{
			Provider.RemoveFileFromCache(State->GetFilename());
		}
	}
	Operation->SetSuccessMessage(FText::FromString("Commit successful!"));
	FriendshipperSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	// now update the status of our files
	TMap<FString, FFriendshipperSourceControlState> UpdatedStates;
	bool bSuccess = FriendshipperSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.Files, EFetchRemote::False, UpdatedStates);
	if (bSuccess)
	{
		FriendshipperSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	FriendshipperSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	return true;
}

bool FFriendshipperCheckInWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}

FName FFriendshipperMarkForAddWorker::GetName() const
{
	return "MarkForAdd";
}

bool FFriendshipperMarkForAddWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	return LockFiles(InCommand.PathToGitRoot, InCommand.Files, States, &InCommand.ResultInfo.ErrorMessages);
}

bool FFriendshipperMarkForAddWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}

FName FFriendshipperDeleteWorker::GetName() const
{
	return "Delete";
}

bool FFriendshipperDeleteWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	if (LockFiles(InCommand.PathToGitRoot, InCommand.Files, States, &InCommand.ResultInfo.ErrorMessages) == false)
	{
		return false;
	}

	// We just delete the file directly here because if we try to use "git rm" it will stage the file, and we try to avoid staging files since
	// it just complicates dealing with the file's state. We're only deleting files we have successfully locked anyway.
	bool bSuccess = true;
	for (const FString& Filename : InCommand.Files)
	{
		if (IFileManager::Get().Delete(*Filename))
		{
			bSuccess &= FriendshipperSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Deleted, ETreeState::Unset);
		}
	}

	if (bSuccess)
	{
		TMap<FString, FFriendshipperSourceControlState> UpdatedStates;
		if (FriendshipperSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.Files, EFetchRemote::False, UpdatedStates))
		{
			FriendshipperSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		FriendshipperSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return bSuccess;
}

bool FFriendshipperDeleteWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}


// Get lists of Missing files (ie "deleted"), Modified files, and "other than Added" Existing files
void GetMissingVsExistingFiles(const TArray<FString>& InFiles, TArray<FString>& OutMissingFiles, TArray<FString>& OutAllExistingFiles, TArray<FString>& OutOtherThanAddedExistingFiles, TArray<FString>& OutUntrackedMissingLockedFiles)
{
	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	FFriendshipperSourceControlProvider& Provider = GitSourceControl.GetProvider();

	const TArray<FString> Files = (InFiles.Num() > 0) ? (InFiles) : (Provider.GetFilesInCache());

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(Files, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		if (FPaths::FileExists(State->GetFilename()))
		{
			if (State->IsAdded())
			{
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->IsModified())
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->CanRevert()) // for locked but unmodified files
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
			}
		}
		else
		{
			if (State->CanRevert() && !State->IsModified()) // for locked but unmodified files
			{
				OutUntrackedMissingLockedFiles.Add(State->GetFilename());
			}
			else if (State->IsSourceControlled() && !State->IsDeleted())
			{
				// If already queued for deletion, don't try to delete again
				OutMissingFiles.Add(State->GetFilename());
			}
			else if (State->IsDeleted())
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
			}
		}
	}
}

FName FFriendshipperRevertWorker::GetName() const
{
	return "Revert";
}

bool FFriendshipperRevertWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	bool bSuccess = true;

	// Filter files by status
	TArray<FString> MissingFiles;
	TArray<FString> AllExistingFiles;
	TArray<FString> OtherThanAddedExistingFiles;
	TArray<FString> UntrackedMissingLockedFiles;
	GetMissingVsExistingFiles(InCommand.Files, MissingFiles, AllExistingFiles, OtherThanAddedExistingFiles, UntrackedMissingLockedFiles);

	const bool bRevertAll = InCommand.Files.Num() < 1;
	if (bRevertAll)
	{
		TArray<FString> Parms;
		Parms.Add(TEXT("--hard"));
		bSuccess &= FriendshipperSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parms, FFriendshipperSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

		Parms.Reset(2);
		Parms.Add(TEXT("-f")); // force
		Parms.Add(TEXT("-d")); // remove directories
		bSuccess &= FriendshipperSourceControlUtils::RunCommand(TEXT("clean"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parms, FFriendshipperSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	else
	{
		if (MissingFiles.Num() > 0)
		{
			// "Added" files that have been deleted needs to be removed from revision control
			bSuccess &= FriendshipperSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FFriendshipperSourceControlModule::GetEmptyStringArray(), MissingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (AllExistingFiles.Num() > 0)
		{
			// reset any changes already added to the index
			bSuccess &= FriendshipperSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FFriendshipperSourceControlModule::GetEmptyStringArray(), AllExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (OtherThanAddedExistingFiles.Num() > 0)
		{
			// revert any changes in working copy (this would fails if the asset was in "Added" state, since after "reset" it is now "untracked")
			// may need to try a few times due to file locks from prior operations
			bool CheckoutSuccess = false;
			int32 Attempts = 10;
			while( Attempts-- > 0 )
			{
				CheckoutSuccess = FriendshipperSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FFriendshipperSourceControlModule::GetEmptyStringArray(), OtherThanAddedExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
				if (CheckoutSuccess)
				{
					break;
				}

				FPlatformProcess::Sleep(0.1f);
			}

			bSuccess &= CheckoutSuccess;
		}
	}

	// Unlock files
	{
		TArray<FString> LockedFiles;
		FriendshipperSourceControlUtils::GetLockedFiles(InCommand.Files, LockedFiles);
		if (LockedFiles.Num() > 0)
		{
			const TArray<FString> RelativeFiles = FriendshipperSourceControlUtils::RelativeFilenames(LockedFiles, InCommand.PathToGitRoot);

			// Throw away the result here, this is an optimistic unlock
			FFriendshipperSourceControlProvider& Provider = FFriendshipperSourceControlModule::Get().GetProvider();
			FFriendshipperClient& Client = Provider.GetFriendshipperClient();

			TArray<FString> FailedFiles;
			if (Client.UnlockFiles(RelativeFiles, &FailedFiles, &InCommand.ResultInfo.ErrorMessages) == false)
			{
				return false;
			}

			for (const auto& File : LockedFiles)
			{
				bool bWasLocked = true;
				for (FString& RelativeFile : FailedFiles)
				{
					if (File.EndsWith(RelativeFile))
					{
						bWasLocked = false;
						break;
					}
				}

				if (bWasLocked)
				{
					FFriendshipperLockedFilesCache::RemoveLockedFile(File);
				}
			}
		}
	}

	// If no files were specified (full revert), refresh all relevant files instead of the specified files (which is an empty list in full revert)
	// This is required so that files that were "Marked for add" have their status updated after a full revert.
	TArray<FString> FilesToUpdate = InCommand.Files;
	if (InCommand.Files.Num() <= 0)
	{
		for (const auto& File : MissingFiles) FilesToUpdate.Add(File);
		for (const auto& File : AllExistingFiles) FilesToUpdate.Add(File);
		for (const auto& File : OtherThanAddedExistingFiles) FilesToUpdate.Add(File);
	}

	// now update the status of our files
	TMap<FString, FFriendshipperSourceControlState> UpdatedStates;
	if (FriendshipperSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FilesToUpdate, EFetchRemote::False, UpdatedStates))
	{
		FriendshipperSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	FriendshipperSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.ScanModifiedAssetFiles(FilesToUpdate);

	return bSuccess;
}

bool FFriendshipperRevertWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}

FName FFriendshipperFetch::GetName() const
{
	return "Fetch";
}

FText FFriendshipperFetch::GetInProgressString() const
{
	// TODO Configure origin
	return LOCTEXT("SourceControl_Push", "Fetching from remote origin...");
}

FName FFriendshipperFetchWorker::GetName() const
{
	return "Fetch";
}

bool FFriendshipperFetchWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FFriendshipperFetch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FFriendshipperFetch>(InCommand.Operation);

	bool bSuccess = true;
	if (Operation->bUpdateStatus)
	{
		// Now update the status of all our files
		const TArray<FString> ProjectDirs {FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()),FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
										   FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())};
		TMap<FString, FFriendshipperSourceControlState> UpdatedStates;
		bSuccess = FriendshipperSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, ProjectDirs, EFetchRemote::True, UpdatedStates);
		if (bSuccess)
		{
			FriendshipperSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		FriendshipperSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return bSuccess;
}

bool FFriendshipperFetchWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}

FName FFriendshipperUpdateStatusWorker::GetName() const
{
	return "UpdateStatus";
}

bool FFriendshipperUpdateStatusWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);

	bool bSuccess = true;
	if(InCommand.Files.Num() > 0)
	{
		TMap<FString, FFriendshipperSourceControlState> UpdatedStates;
		bSuccess = FriendshipperSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.Files, EFetchRemote::False, UpdatedStates);
		FriendshipperSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (bSuccess)
		{
			FriendshipperSourceControlUtils::CollectNewStates(UpdatedStates, States);
			if (Operation->ShouldUpdateHistory())
			{
				for (const auto& State : UpdatedStates)
				{
					const FString& File = State.Key;
					TGitSourceControlHistory History;

					if (State.Value.IsConflicted())
					{
						// In case of a merge conflict, we first need to get the tip of the "remote branch" (MERGE_HEAD)
						FriendshipperSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, true,
															 InCommand.ResultInfo.ErrorMessages, History);
					}
					// Get the history of the file in the current branch
					bSuccess &= FriendshipperSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, false,
																						 InCommand.ResultInfo.ErrorMessages, History);
					Histories.Add(*File, History);
				}
			}
		}
	}
	else
	{
		// no path provided: only update the status of assets in Content/ directory and also Config files
		const TArray<FString> ProjectDirs {FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()), FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
										   FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())};
		TMap<FString, FFriendshipperSourceControlState> UpdatedStates;
		bSuccess = FriendshipperSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, ProjectDirs, EFetchRemote::False, UpdatedStates);
		FriendshipperSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (bSuccess)
		{
			FriendshipperSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	FriendshipperSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	// don't use the ShouldUpdateModifiedState() hint here as it is specific to Perforce: the above normal Git status has already told us this information (like Git and Mercurial)

	return bSuccess;
}

bool FFriendshipperUpdateStatusWorker::UpdateStates() const
{
	bool bUpdated = FriendshipperSourceControlUtils::UpdateCachedStates(States);

	FFriendshipperSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FFriendshipperSourceControlModule>( "FriendshipperSourceControl" );
	FFriendshipperSourceControlProvider& Provider = GitSourceControl.GetProvider();

	// add history, if any
	for(const auto& History : Histories)
	{
		TSharedRef<FFriendshipperSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(History.Key);
		State->History = History.Value;
		State->TimeStamp = FDateTime::Now();
		bUpdated = true;
	}

	return bUpdated;
}

FName FFriendshipperCopyWorker::GetName() const
{
	return "Copy";
}

bool FFriendshipperCopyWorker::Execute(FFriendshipperSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	bool bSuccess = true;

	const FCopy& Copy = static_cast<FCopy&>(InCommand.Operation.Get());

	// lock the new file - this effectively marks it for add
	TArray<FString> FilesToLock;
	FilesToLock.Emplace(Copy.GetDestination());

	LockFiles(InCommand.PathToGitRoot, FilesToLock, States, &InCommand.ResultInfo.ErrorMessages);

	return true;
}

bool FFriendshipperCopyWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}

FName FFriendshipperResolveWorker::GetName() const
{
	return "Resolve";
}

bool FFriendshipperResolveWorker::Execute( class FFriendshipperSourceControlCommand& InCommand )
{
	check(InCommand.Operation->GetName() == GetName());

	// mark the conflicting files as resolved:
	TArray<FString> Results;
	const bool bSuccess = FriendshipperSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FFriendshipperSourceControlModule::GetEmptyStringArray(), InCommand.Files, Results, InCommand.ResultInfo.ErrorMessages);

	// now update the status of our files
	TMap<FString, FFriendshipperSourceControlState> UpdatedStates;
	if (FriendshipperSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.Files, EFetchRemote::False, UpdatedStates))
	{
		FriendshipperSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}

	FriendshipperSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

	return bSuccess;
}

bool FFriendshipperResolveWorker::UpdateStates() const
{
	return FriendshipperSourceControlUtils::UpdateCachedStates(States);
}

#undef LOCTEXT_NAMESPACE
