// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "FriendshipperClient.h"
#include "ISourceControlProvider.h"
#include "IFriendshipperSourceControlWorker.h"
#include "FriendshipperSourceControlMenu.h"
#include "Runtime/Launch/Resources/Version.h"

class FFriendshipperSourceControlState;
class FFriendshipperSourceControlCommand;
struct FFriendshipperState;

DECLARE_DELEGATE_RetVal(FFriendshipperSourceControlWorkerRef, FGetFriendshipperSourceControlWorker)

	/// Git version and capabilites extracted from the string "git version 2.11.0.windows.3"
	struct FFriendshipperVersion
{
	// Git version extracted from the string "git version 2.11.0.windows.3" (Windows), "git version 2.11.0" (Linux/Mac/Cygwin/WSL) or "git version 2.31.1.vfs.0.3" (Microsoft)
	int Major; // 2	Major version number
	int Minor; // 31	Minor version number
	int Patch; // 1	Patch/bugfix number
	bool bIsFork;
	FString Fork;  // "vfs"
	int ForkMajor; // 0	Fork specific revision number
	int ForkMinor; // 3
	int ForkPatch; // ?

	FFriendshipperVersion()
		: Major(0)
		, Minor(0)
		, Patch(0)
		, bIsFork(false)
		, ForkMajor(0)
		, ForkMinor(0)
		, ForkPatch(0)
	{
	}
};

struct FFriendshipperFileWatchHandle
{
	FString Directory;
	FDelegateHandle DelegateHandle;
};

class FFriendshipperSourceControlProvider : public ISourceControlProvider
{
public:
	/* ISourceControlProvider implementation */
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual const FName& GetName(void) const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override;
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn) override;
	virtual int32 GetStateBranchIndex(const FString& BranchName) const override;
	virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;

	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanCancelOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual void CancelOperation(const FSourceControlOperationRef& InOperation) override;

	virtual bool UsesLocalReadOnlyState() const override;
	virtual bool UsesChangelists() const override;
	virtual bool UsesCheckout() const override;
	virtual bool UsesFileRevisions() const override;
	virtual TOptional<bool> IsAtLatestRevision() const override;
	virtual TOptional<int> GetNumLocalChanges() const override;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	virtual bool AllowsDiffAgainstDepot() const override;
	virtual bool UsesUncontrolledChangelists() const override;
	virtual bool UsesSnapshots() const override;
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	virtual bool CanExecuteOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual TMap<EStatus, FString> GetStatus() const override;
#endif
	virtual void Tick() override;
	virtual TArray<TSharedRef<class ISourceControlLabel>> GetLabels(const FString& InMatchingSpec) const override;

	virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type InStateCacheUsage) override;

#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif

	using ISourceControlProvider::Execute;

	/**
	 * Check configuration, else standard paths, and run a Git "version" command to check the availability of the binary.
	 */
	void CheckGitAvailability();

	/** Refresh Git settings from revision control settings */
	void UpdateSettings();

	/**
	 * Find the .git/ repository and check its status.
	 */
	void CheckRepositoryStatus();

	/** Is git binary found and working. */
	bool IsGitAvailable() const
	{
		return bGitAvailable;
	}

	/** Git version for feature checking */
	const FFriendshipperVersion& GetGitVersion() const
	{
		return GitVersion;
	}

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	const FString& GetPathToRepositoryRoot() const
	{
		return PathToRepositoryRoot;
	}

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	const FString& GetPathToGitRoot() const
	{
		return PathToGitRoot;
	}

	/** Gets the path to the Git binary */
	const FString& GetGitBinaryPath() const
	{
		return PathToGitBinary;
	}

	/** Git config user.name */
	const FString& GetUserName() const
	{
		return UserName;
	}

	/** Git config user.email */
	const FString& GetUserEmail() const
	{
		return UserEmail;
	}

	/** Git remote origin url */
	const FString& GetRemoteUrl() const
	{
		return RemoteUrl;
	}

	const FString& GetLockUser() const
	{
		return LockUser;
	}

	FFriendshipperClient& GetFriendshipperClient()
	{
		return FriendshipperClient;
	}

	/** Helper function used to update state cache */
	TSharedRef<FFriendshipperSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& Filename);

	/**
	 * Register a worker with the provider.
	 * This is used internally so the provider can maintain a map of all available operations.
	 */
	void RegisterWorker(const FName& InName, const FGetFriendshipperSourceControlWorker& InDelegate);

	/** Set list of error messages that occurred after last perforce command */
	void SetLastErrors(const TArray<FText>& InErrors);

	/** Get list of error messages that occurred after last perforce command */
	TArray<FText> GetLastErrors() const;

	/** Get number of error messages seen after running last perforce command */
	int32 GetNumLastErrors() const;

	/** Remove a named file from the state cache */
	bool RemoveFileFromCache(const FString& Filename);

	/** Get files in cache */
	TArray<FString> GetFilesInCache();

	bool AddFileToIgnoreForceCache(const FString& Filename);

	bool RemoveFileFromIgnoreForceCache(const FString& Filename);

	const FString& GetBranchName() const
	{
		return BranchName;
	}

	const FString& GetRemoteBranchName() const { return RemoteBranchName; }

	TArray<FString> GetStatusBranchNames() const;

	// Source control state cache refresh
	TSet<FString> GetAllPathsAbsolute();
	bool UpdateCachedStates(const TMap<const FString, FFriendshipperState>& InResults);
	void RefreshCacheFromSavedState();
	void RunFileRescanTask();
	void OnFilesChanged(const TArray<struct FFileChangeData>& FileChanges);
	void OnRecievedHttpStatusUpdate(const FRepoStatus& RepoStatus);

	uint32 TicksUntilNextForcedUpdate = 0;

private:
	/** Is git binary found and working. */
	bool bGitAvailable = false;
	bool bFriendshipperAvailable = false;

	/** Is git repository found. */
	bool bGitRepositoryFound = false;

	FString PathToGitBinary;

	FString LockUser;

	/** Critical section for thread safety of error messages that occurred after last perforce command */
	mutable FCriticalSection LastErrorsCriticalSection;

	/** List of error messages that occurred after last perforce command */
	TArray<FText> LastErrors;

	/** Helper function for Execute() */
	TSharedPtr<class IFriendshipperSourceControlWorker, ESPMode::ThreadSafe> CreateWorker(const FName& InOperationName) const;

	/** Helper function for running command synchronously. */
	ECommandResult::Type ExecuteSynchronousCommand(class FFriendshipperSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg);
	/** Issue a command asynchronously if possible. */
	ECommandResult::Type IssueCommand(class FFriendshipperSourceControlCommand& InCommand);

	/** Output any messages this command holds */
	void OutputCommandMessages(const class FFriendshipperSourceControlCommand& InCommand) const;

	/** Update repository status on Connect and UpdateStatus operations */
	void UpdateRepositoryStatus(const class FFriendshipperSourceControlCommand& InCommand);

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	FString PathToRepositoryRoot;

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	FString PathToGitRoot;

	/** Git config user.name (from local repository, else globally) */
	FString UserName;

	/** Git config user.email (from local repository, else globally) */
	FString UserEmail;

	/** Name of the current branch */
	FString BranchName;

	/** Name of the current remote branch */
	FString RemoteBranchName;

	/** URL of the "origin" default remote server */
	FString RemoteUrl;

	/** Current Commit full SHA1 */
	FString CommitId;

	/** Current Commit description's Summary */
	FString CommitSummary;

	/** State cache */
	TMap<FString, TSharedRef<class FFriendshipperSourceControlState, ESPMode::ThreadSafe>> StateCache;

	/** All source controlled files in the repo under Content/ and Config/ */
	FRWLock AllPathsAbsoluteLock;
	TSet<FString> AllPathsAbsolute;

	/** Flag to skip triggering another scan if one is in progress */
	std::atomic<bool> bAllPathsScanInProgress;

	/** Delegates to unregister on shutdown */
	TArray<FFriendshipperFileWatchHandle> FileWatchHandles;

	/** The currently registered revision control operations */
	TMap<FName, FGetFriendshipperSourceControlWorker> WorkersMap;

	/** Queue for commands given by the main thread */
	TArray<FFriendshipperSourceControlCommand*> CommandQueue;

	/** For notifying when the revision control states in the cache have changed */
	FSourceControlStateChanged OnSourceControlStateChanged;

	/** Git version for feature checking */
	FFriendshipperVersion GitVersion;

	/** Revision Control Menu Extension */
	FFriendshipperSourceControlMenu GitSourceControlMenu;

	FFriendshipperClient FriendshipperClient;

	/**
		Ignore these files when forcing status updates. We add to this list when we've just updated the status already.
		UE's SourceControl has a habit of performing a double status update, immediately after an operation.
	*/
	TArray<FString> IgnoreForceCache;

	/** Array of branch name patterns for status queries */
	TArray<FString> StatusBranchNamePatternsInternal;
};
