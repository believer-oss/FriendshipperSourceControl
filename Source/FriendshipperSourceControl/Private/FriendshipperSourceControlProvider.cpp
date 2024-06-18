// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlProvider.h"

#include "FriendshipperMessageLog.h"
#include "FriendshipperSourceControlState.h"
#include "Misc/Paths.h"
#include "Misc/QueuedThreadPool.h"
#include "FriendshipperSourceControlCommand.h"
#include "ISourceControlModule.h"
#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlUtils.h"
#include "SFriendshipperSourceControlSettings.h"
#include "FriendshipperSourceControlRunner.h"
#include "Logging/MessageLog.h"
#include "ScopedSourceControlProgress.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "Async/Async.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/MessageDialog.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "HttpServerModule.h"
#include "FileHelpers.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

static FName ProviderName("Friendshipper");

void FFriendshipperSourceControlProvider::Init(bool bForceConnection)
{
	// Init() is called multiple times at startup: do not check git each time
	if (!bGitAvailable)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FriendshipperSourceControl"));
		if (Plugin.IsValid())
		{
			UE_LOG(LogSourceControl, Log, TEXT("Git plugin '%s'"), *(Plugin->GetDescriptor().VersionName));
		}

		CheckGitAvailability();
	}

	if (!bFriendshipperAvailable)
	{
		FriendshipperClient.Init("http://localhost:8484");
		bFriendshipperAvailable = true;
	}

	// bForceConnection: not used anymore
}

void FFriendshipperSourceControlProvider::CheckGitAvailability()
{
	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
	if (PathToGitBinary.IsEmpty())
	{
		// Try to find Git binary, and update settings accordingly
		PathToGitBinary = FriendshipperSourceControlUtils::FindGitBinaryPath();
		if (!PathToGitBinary.IsEmpty())
		{
			GitSourceControl.AccessSettings().SetBinaryPath(PathToGitBinary);
		}
	}

	if (!PathToGitBinary.IsEmpty())
	{
		UE_LOG(LogSourceControl, Log, TEXT("Using '%s'"), *PathToGitBinary);
		bGitAvailable = true;
		CheckRepositoryStatus();
	}
	else
	{
		bGitAvailable = false;
	}
}

void FFriendshipperSourceControlProvider::UpdateSettings()
{
	const FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	LockUser = GitSourceControl.AccessSettings().GetLfsUserName();
}

void FFriendshipperSourceControlProvider::CheckRepositoryStatus()
{
	GitSourceControlMenu.Register();

	// Make sure our settings our up to date
	UpdateSettings();

	// Find the path to the root Git directory (if any, else uses the ProjectDir)
	const FString PathToProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	PathToRepositoryRoot = PathToProjectDir;

	// [BELIEVER-MOD] This is due to the fact that our .uproject is down a level from .git. The plugin's default
	// behavior is to look for .git directories next to the .uproject and in subfolders, but it never checks anywhere
	// in the tree above the project root.
	PathToRepositoryRoot.RemoveFromEnd("ThirdPersonMP/");

	if (!FriendshipperSourceControlUtils::FindRootDirectory(PathToProjectDir, PathToGitRoot))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to find valid Git root directory."));
		bGitRepositoryFound = false;
		return;
	}
	if (!FriendshipperSourceControlUtils::CheckGitAvailability(PathToGitBinary, &GitVersion))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to find valid Git executable."));
		bGitRepositoryFound = false;
		return;
	}

	TUniqueFunction<void()> InitFunc = [this]()
	{
		if (!IsInGameThread())
		{
			// Wait until the module interface is valid
			IModuleInterface* GitModule;
			do
			{
				GitModule = FModuleManager::Get().GetModule("FriendshipperSourceControl");
				FPlatformProcess::Sleep(0.0f);
			} while (!GitModule);
		}

		// Get user name & email (of the repository, else from the global Git config)
		FriendshipperSourceControlUtils::GetUserConfig(PathToGitBinary, PathToRepositoryRoot, UserName, UserEmail);

		TMap<FString, FFriendshipperSourceControlState> States;
		auto ConditionalRepoInit = [this, &States]()
		{
			if (!FriendshipperSourceControlUtils::GetBranchName(PathToGitBinary, PathToRepositoryRoot, BranchName))
			{
				return false;
			}
			FriendshipperSourceControlUtils::GetRemoteBranchName(PathToGitBinary, PathToRepositoryRoot, RemoteBranchName);
			FriendshipperSourceControlUtils::GetRemoteUrl(PathToGitBinary, PathToRepositoryRoot, RemoteUrl);
			const TArray<FString> Files{ TEXT("*.uasset"), TEXT("*.umap") };
			TArray<FString> LockableErrorMessages;
			if (!FriendshipperSourceControlUtils::CheckLFSLockable(PathToGitBinary, PathToRepositoryRoot, Files, LockableErrorMessages))
			{
				for (const auto& ErrorMessage : LockableErrorMessages)
				{
					UE_LOG(LogSourceControl, Error, TEXT("%s"), *ErrorMessage);
				}
			}
			const TArray<FString> ProjectDirs{ FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()),
				FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
				FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()) };
			TArray<FString> StatusErrorMessages;
			if (!FriendshipperSourceControlUtils::RunUpdateStatus(PathToGitBinary, PathToRepositoryRoot, ProjectDirs, EFetchRemote::True, States))
			{
				return false;
			}
			return true;
		};
		if (ConditionalRepoInit())
		{
			TUniqueFunction<void()> SuccessFunc = [States, this]()
			{
				TMap<const FString, FFriendshipperState> Results;
				if (FriendshipperSourceControlUtils::CollectNewStates(States, Results))
				{
					FriendshipperSourceControlUtils::UpdateCachedStates(Results);
				}
				Runner = new FFriendshipperSourceControlRunner();
				bGitRepositoryFound = true;
			};
			AsyncTask(ENamedThreads::GameThread, MoveTemp(SuccessFunc));
		}
		else
		{
			TUniqueFunction<void()> ErrorFunc = [States, this]()
			{
				UE_LOG(LogSourceControl, Error, TEXT("Failed to update repo on initialization."));
				bGitRepositoryFound = false;
			};
			AsyncTask(ENamedThreads::GameThread, MoveTemp(ErrorFunc));
		}
	};

	if (!FApp::IsUnattended() && !IsRunningCommandlet())
	{
		AsyncTask(ENamedThreads::AnyHiPriThreadNormalTask, MoveTemp(InitFunc));
	}
}

void FFriendshipperSourceControlProvider::SetLastErrors(const TArray<FText>& InErrors)
{

	FScopeLock Lock(&LastErrorsCriticalSection);
	LastErrors = InErrors;
}

TArray<FText> FFriendshipperSourceControlProvider::GetLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	TArray<FText> Result = LastErrors;
	return Result;
}

int32 FFriendshipperSourceControlProvider::GetNumLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	return LastErrors.Num();
}

void FFriendshipperSourceControlProvider::Close()
{
	// clear the cache
	StateCache.Empty();
	// Remove all extensions to the "Revision Control" menu in the Editor Toolbar
	GitSourceControlMenu.Unregister();

	bGitAvailable = false;
	bGitRepositoryFound = false;
	UserName.Empty();
	UserEmail.Empty();
	if (Runner)
	{
		delete Runner;
		Runner = nullptr;
	}
}

TSharedRef<FFriendshipperSourceControlState, ESPMode::ThreadSafe> FFriendshipperSourceControlProvider::GetStateInternal(const FString& Filename)
{
	TSharedRef<FFriendshipperSourceControlState, ESPMode::ThreadSafe>* State = StateCache.Find(Filename);
	if (State != NULL)
	{
		// found cached item
		return (*State);
	}
	else
	{
		// cache an unknown state for this item
		TSharedRef<FFriendshipperSourceControlState, ESPMode::ThreadSafe> NewState = MakeShareable(new FFriendshipperSourceControlState(Filename));
		StateCache.Add(Filename, NewState);
		return NewState;
	}
}

FText FFriendshipperSourceControlProvider::GetStatusText() const
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("IsAvailable"), (IsEnabled() && IsAvailable()) ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"));
	Args.Add(TEXT("RepositoryName"), FText::FromString(PathToRepositoryRoot));
	Args.Add(TEXT("RemoteUrl"), FText::FromString(RemoteUrl));
	Args.Add(TEXT("UserName"), FText::FromString(UserName));
	Args.Add(TEXT("UserEmail"), FText::FromString(UserEmail));
	Args.Add(TEXT("BranchName"), FText::FromString(BranchName));
	Args.Add(TEXT("CommitId"), FText::FromString(CommitId.Left(8)));
	Args.Add(TEXT("CommitSummary"), FText::FromString(CommitSummary));

	FText FormattedError;
	const TArray<FText>& RecentErrors = GetLastErrors();
	if (RecentErrors.Num() > 0)
	{
		FFormatNamedArguments ErrorArgs;
		ErrorArgs.Add(TEXT("ErrorText"), RecentErrors[0]);

		FormattedError = FText::Format(LOCTEXT("GitErrorStatusText", "Error: {ErrorText}\n\n"), ErrorArgs);
	}

	Args.Add(TEXT("ErrorText"), FormattedError);

	return FText::Format(NSLOCTEXT("GitStatusText", "{ErrorText}Enabled: {IsAvailable}", "Local repository: {RepositoryName}\nRemote: {RemoteUrl}\nUser: {UserName}\nE-mail: {UserEmail}\n[{BranchName} {CommitId}] {CommitSummary}"), Args);
}

/** Quick check if revision control is enabled */
bool FFriendshipperSourceControlProvider::IsEnabled() const
{
	return bGitRepositoryFound;
}

/** Quick check if revision control is available for use (useful for server-based providers) */
bool FFriendshipperSourceControlProvider::IsAvailable() const
{
	return bGitRepositoryFound;
}

const FName& FFriendshipperSourceControlProvider::GetName(void) const
{
	return ProviderName;
}

ECommandResult::Type FFriendshipperSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		TArray<FString> ForceUpdate;
		for (FString Path : InFiles)
		{
			// Remove the path from the cache, so it's not ignored the next time we force check.
			// If the file isn't in the cache, force update it now.
			if (!RemoveFileFromIgnoreForceCache(Path))
			{
				ForceUpdate.Add(Path);
			}
		}
		if (ForceUpdate.Num() > 0)
		{
			Execute(ISourceControlOperation::Create<FUpdateStatus>(), ForceUpdate);
		}
	}

	const TArray<FString>& AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	for (TArray<FString>::TConstIterator It(AbsoluteFiles); It; It++)
	{
		OutState.Add(GetStateInternal(*It));
	}

	return ECommandResult::Succeeded;
}

ECommandResult::Type FFriendshipperSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	return ECommandResult::Failed;
}

TArray<FSourceControlStateRef> FFriendshipperSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	for (const auto& CacheItem : StateCache)
	{
		const FSourceControlStateRef& State = CacheItem.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

bool FFriendshipperSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	return StateCache.Remove(Filename) > 0;
}

bool FFriendshipperSourceControlProvider::AddFileToIgnoreForceCache(const FString& Filename)
{
	return IgnoreForceCache.Add(Filename) > 0;
}

bool FFriendshipperSourceControlProvider::RemoveFileFromIgnoreForceCache(const FString& Filename)
{
	return IgnoreForceCache.Remove(Filename) > 0;
}

/** Get files in cache */
TArray<FString> FFriendshipperSourceControlProvider::GetFilesInCache()
{
	TArray<FString> Files;
	for (const auto& State : StateCache)
	{
		Files.Add(State.Key);
	}
	return Files;
}

FDelegateHandle FFriendshipperSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FFriendshipperSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

ECommandResult::Type FFriendshipperSourceControlProvider::Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
	if (!IsEnabled() && !(InOperation->GetName() == "Connect")) // Only Connect operation allowed while not Enabled (Repository found)
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	const TArray<FString>& AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	// Query to see if we allow this operation
	TSharedPtr<IFriendshipperSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		// this operation is unsupported by this revision control provider
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("OperationName"), FText::FromName(InOperation->GetName()));
		Arguments.Add(TEXT("ProviderName"), FText::FromName(GetName()));
		FText Message(FText::Format(LOCTEXT("UnsupportedOperation", "Operation '{OperationName}' not supported by revision control provider '{ProviderName}'"), Arguments));

		FTSMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);

		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	FFriendshipperSourceControlCommand* Command = new FFriendshipperSourceControlCommand(InOperation, Worker.ToSharedRef());
	Command->Files = AbsoluteFiles;
	Command->UpdateRepositoryRootIfSubmodule(AbsoluteFiles);
	Command->OperationCompleteDelegate = InOperationCompleteDelegate;

	// fire off operation
	if (InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("ExecuteSynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString(), false);
	}
	else
	{
		Command->bAutoDelete = true;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("IssueAsynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return IssueCommand(*Command);
	}
}

bool FFriendshipperSourceControlProvider::CanCancelOperation(const FSourceControlOperationRef& InOperation) const
{
	// TODO: maybe support cancellation again?
#if 0
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		const FFriendshipperSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.Operation == InOperation)
		{
			check(Command.bAutoDelete);
			return true;
		}
	}
#endif

	// operation was not in progress!
	return false;
}

void FFriendshipperSourceControlProvider::CancelOperation(const FSourceControlOperationRef& InOperation)
{
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FFriendshipperSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.Operation == InOperation)
		{
			check(Command.bAutoDelete);
			Command.Cancel();
			return;
		}
	}
}

bool FFriendshipperSourceControlProvider::UsesLocalReadOnlyState() const
{
	return false;
}

bool FFriendshipperSourceControlProvider::UsesChangelists() const
{
	return false;
}

bool FFriendshipperSourceControlProvider::UsesCheckout() const
{
	return true;
}

bool FFriendshipperSourceControlProvider::UsesFileRevisions() const
{
	// While git technically doesn't actually support file revisions, the engine uses this option to determine if it can
	// individually check in files, and since we DO support that functionality, we leave this enabled.
	return true;
}

TOptional<bool> FFriendshipperSourceControlProvider::IsAtLatestRevision() const
{
	return TOptional<bool>();
}

TOptional<int> FFriendshipperSourceControlProvider::GetNumLocalChanges() const
{
	return TOptional<int>();
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
bool FFriendshipperSourceControlProvider::AllowsDiffAgainstDepot() const
{
	return true;
}

bool FFriendshipperSourceControlProvider::UsesUncontrolledChangelists() const
{
	return true;
}

bool FFriendshipperSourceControlProvider::UsesSnapshots() const
{
	return false;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
bool FFriendshipperSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	return WorkersMap.Find(InOperation->GetName()) != nullptr;
}

TMap<ISourceControlProvider::EStatus, FString> FFriendshipperSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, (IsEnabled() && IsAvailable()) ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::User, UserName);
	Result.Add(EStatus::Repository, PathToRepositoryRoot);
	Result.Add(EStatus::Remote, RemoteUrl);
	Result.Add(EStatus::Branch, BranchName);
	Result.Add(EStatus::Email, UserEmail);
	return Result;
}
#endif

TSharedPtr<IFriendshipperSourceControlWorker, ESPMode::ThreadSafe> FFriendshipperSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	const FGetFriendshipperSourceControlWorker* Operation = WorkersMap.Find(InOperationName);
	if (Operation != nullptr)
	{
		return Operation->Execute();
	}

	return nullptr;
}

void FFriendshipperSourceControlProvider::RegisterWorker(const FName& InName, const FGetFriendshipperSourceControlWorker& InDelegate)
{
	WorkersMap.Add(InName, InDelegate);
}

void FFriendshipperSourceControlProvider::OutputCommandMessages(const FFriendshipperSourceControlCommand& InCommand) const
{
	FTSMessageLog SourceControlLog("SourceControl");

	for (int32 ErrorIndex = 0; ErrorIndex < InCommand.ResultInfo.ErrorMessages.Num(); ++ErrorIndex)
	{
		SourceControlLog.Error(FText::FromString(InCommand.ResultInfo.ErrorMessages[ErrorIndex]));
	}

	for (int32 InfoIndex = 0; InfoIndex < InCommand.ResultInfo.InfoMessages.Num(); ++InfoIndex)
	{
		SourceControlLog.Info(FText::FromString(InCommand.ResultInfo.InfoMessages[InfoIndex]));
	}
}

void FFriendshipperSourceControlProvider::UpdateRepositoryStatus(const class FFriendshipperSourceControlCommand& InCommand)
{
	// For all operations running UpdateStatus, get Commit information:
	if (!InCommand.CommitId.IsEmpty())
	{
		CommitId = InCommand.CommitId;
		CommitSummary = InCommand.CommitSummary;
	}
}

void FFriendshipperSourceControlProvider::Tick()
{
	bool bStatesUpdated = TicksUntilNextForcedUpdate == 1;
	if (TicksUntilNextForcedUpdate > 0)
	{
		--TicksUntilNextForcedUpdate;
	}

	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FFriendshipperSourceControlCommand& Command = *CommandQueue[CommandIndex];

		if (Command.bExecuteProcessed)
		{
			// Remove command from the queue
			CommandQueue.RemoveAt(CommandIndex);

			if (!Command.IsCanceled())
			{
				// Update repository status on UpdateStatus operations
				UpdateRepositoryStatus(Command);
			}

			// let command update the states of any files
			bStatesUpdated |= Command.Worker->UpdateStates();

			// dump any messages to output log
			OutputCommandMessages(Command);

			// run the completion delegate callback if we have one bound
			if (!Command.IsCanceled())
			{
				Command.ReturnResults();
			}

			// commands that are left in the array during a tick need to be deleted
			if (Command.bAutoDelete)
			{
				// Only delete commands that are not running 'synchronously'
				delete &Command;
			}

			// only do one command per tick loop, as we dont want concurrent modification
			// of the command queue (which can happen in the completion delegate)
			break;
		}
		else if (Command.bCancelled)
		{
			// If this was a synchronous command, set it free so that it will be deleted automatically
			// when its (still running) thread finally finishes
			Command.bAutoDelete = true;

			Command.ReturnResults();
			break;
		}
	}

	if (bStatesUpdated)
	{
		OnSourceControlStateChanged.Broadcast();
	}
}

TArray<TSharedRef<ISourceControlLabel>> FFriendshipperSourceControlProvider::GetLabels(const FString& InMatchingSpec) const
{
	TArray<TSharedRef<ISourceControlLabel>> Tags;

	// NOTE list labels. Called by CrashDebugHelper() (to remote debug Engine crash)
	//					 and by SourceControlHelpers::AnnotateFile() (to add source file to report)
	// Reserved for internal use by Epic Games with Perforce only
	return Tags;
}

TArray<FSourceControlChangelistRef> FFriendshipperSourceControlProvider::GetChangelists(EStateCacheUsage::Type InStateCacheUsage)
{
	return TArray<FSourceControlChangelistRef>();
}

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<class SWidget> FFriendshipperSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SFriendshipperSourceControlSettings);
}
#endif

ECommandResult::Type FFriendshipperSourceControlProvider::ExecuteSynchronousCommand(FFriendshipperSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg)
{
	ECommandResult::Type Result = ECommandResult::Failed;

	struct Local
	{
		static void CancelCommand(FFriendshipperSourceControlCommand* InControlCommand)
		{
			InControlCommand->Cancel();
		}
	};

	FText TaskText = Task;
	// Display the progress dialog
	if (bSuppressResponseMsg)
	{
		TaskText = FText::GetEmpty();
	}

	int i = 0;

	// Display the progress dialog if a string was provided
	{
		// TODO: support cancellation?
		// FScopedSourceControlProgress Progress(TaskText, FSimpleDelegate::CreateStatic(&Local::CancelCommand, &InCommand));
		FScopedSourceControlProgress Progress(TaskText);

		// Issue the command asynchronously...
		IssueCommand(InCommand);

		// ... then wait for its completion (thus making it synchronous)
		double LastTime = FPlatformTime::Seconds();
		while (!InCommand.IsCanceled() && CommandQueue.Contains(&InCommand))
		{
			// Tick the command queue and update progress.
			Tick();

			const double AppTime = FPlatformTime::Seconds();
			const double DeltaTime = AppTime - LastTime;
			FHttpModule::Get().GetHttpManager().Tick(DeltaTime);
			FHttpServerModule::Get().Tick(DeltaTime);
			LastTime = AppTime;

			if (i >= 20)
			{
				Progress.Tick();
				i = 0;
			}
			i++;

			// Sleep for a bit so we don't busy-wait so much.
			FPlatformProcess::Sleep(0.01f);
		}

		if (InCommand.bCancelled)
		{
			Result = ECommandResult::Cancelled;
		}
		if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
		else if (InCommand.Conflicts.Num() > 0)
		{

			FText Message(LOCTEXT("Friendshipper_Conflict_Msg", "Operation was cancelled due to conflicts detected in the following files:\n\n"));

			for (const FString& File : InCommand.Conflicts)
			{
				Message = FText::Format(LOCTEXT("Friendshipper_Conflict_Format", "{0}\n- {1}"), Message, FText::FromString(File));
			}

			Message = FText::Format(LOCTEXT("Felowshipper_Conflict_Footer", "{0}\n\nConsider reverting the file(s) or discussing with your team on how best to proceed."), Message);

			FMessageDialog::Open(EAppMsgType::Ok, Message);
		}
		else if (!bSuppressResponseMsg)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("Git_ServerUnresponsive", "Git command failed. Please check your connection and try again, or check the output log for more information."));
			UE_LOG(LogSourceControl, Error, TEXT("Command '%s' Failed!"), *InCommand.Operation->GetName().ToString());
		}
	}

	// Delete the command now if not marked as auto-delete
	if (!InCommand.bAutoDelete)
	{
		delete &InCommand;
	}

	return Result;
}

ECommandResult::Type FFriendshipperSourceControlProvider::IssueCommand(FFriendshipperSourceControlCommand& InCommand)
{
	if (GBackgroundPriorityThreadPool != nullptr)
	{
		// Queue this to our worker thread(s) for resolving.
		// When asynchronous, any callback gets called from Tick().
		GBackgroundPriorityThreadPool->AddQueuedWork(&InCommand);
		CommandQueue.Add(&InCommand);
		return ECommandResult::Succeeded;
	}
	else
	{
		UE_LOG(LogSourceControl, Log, TEXT("There are no threads available to process the revision control command '%s'. Running synchronously."), *InCommand.Operation->GetName().ToString());

		InCommand.bCommandSuccessful = InCommand.DoWork();

		InCommand.Worker->UpdateStates();

		OutputCommandMessages(InCommand);

		// Callback now if present. When asynchronous, this callback gets called from Tick().
		return InCommand.ReturnResults();
	}
}

bool FFriendshipperSourceControlProvider::QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest)
{
	// Check similar preconditions to Perforce (valid src and dest),
	if (ConfigSrc.Len() == 0 || ConfigDest.Len() == 0)
	{
		return false;
	}

	if (!bGitAvailable || !bGitRepositoryFound)
	{
		FTSMessageLog("SourceControl").Error(LOCTEXT("StatusBranchConfigNoConnection", "Unable to retrieve status branch configuration from repo, no connection"));
		return false;
	}

	// Otherwise, we can assume that whatever our user is doing to config state branches is properly synced, so just copy.
	// TODO: maybe don't assume, and use git show instead?
	IFileManager::Get().Copy(*ConfigDest, *ConfigSrc);
	return true;
}

void FFriendshipperSourceControlProvider::RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn)
{
	StatusBranchNamePatternsInternal = BranchNames;
}

int32 FFriendshipperSourceControlProvider::GetStateBranchIndex(const FString& StateBranchName) const
{
	// How do state branches indices work?
	// Order matters. Lower values are lower in the hierarchy, i.e., changes from higher branches get automatically merged down.
	// The higher branch is, the stabler it is, and has changes manually promoted up.

	// Check if we are checking the index of the current branch
	// UE uses FEngineVersion for the current branch name because of UEGames setup, but we want to handle otherwise for Git repos.
	auto StatusBranchNames = GetStatusBranchNames();
	if (StateBranchName == FEngineVersion::Current().GetBranch())
	{
		const int32 CurrentBranchStatusIndex = StatusBranchNames.IndexOfByKey(BranchName);
		const bool bCurrentBranchInStatusBranches = CurrentBranchStatusIndex != INDEX_NONE;
		// If the user's current branch is tracked as a status branch, give the proper index
		if (bCurrentBranchInStatusBranches)
		{
			return CurrentBranchStatusIndex;
		}
		// If the current branch is not a status branch, make it the highest branch
		// This is semantically correct, since if a branch is not marked as a status branch
		// it merges changes in a similar fashion to the highest status branch, i.e. manually promotes them
		// based on the user merging those changes in. and these changes always get merged from even the highest point
		// of the stream. i.e, promoted/stable changes are always up for consumption by this branch.
		return INT32_MAX;
	}

	// If we're not checking the current branch, then we don't need to do special handling.
	// If it is not a status branch, there is no message
	return StatusBranchNames.IndexOfByKey(StateBranchName);
}

TArray<FString> FFriendshipperSourceControlProvider::GetStatusBranchNames() const
{
	TArray<FString> StatusBranches;
	if (PathToGitBinary.IsEmpty() || PathToRepositoryRoot.IsEmpty())
		return StatusBranches;

	for (int i = 0; i < StatusBranchNamePatternsInternal.Num(); i++)
	{
		TArray<FString> Matches;
		bool bResult = FriendshipperSourceControlUtils::GetRemoteBranchesWildcard(PathToGitBinary, PathToRepositoryRoot, StatusBranchNamePatternsInternal[i], Matches);
		if (bResult && Matches.Num() > 0)
		{
			for (int j = 0; j < Matches.Num(); j++)
			{
				StatusBranches.Add(Matches[j].TrimStartAndEnd());
			}
		}
	}

	return StatusBranches;
}

#undef LOCTEXT_NAMESPACE
