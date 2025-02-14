#pragma once

#include "Interfaces/IHttpRequest.h"
#include "Misc/ScopeRWLock.h"
#include "FriendshipperClient.generated.h"

struct FOtelScopedSpan;

USTRUCT()
struct FUserInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString Username;
};

USTRUCT()
struct FStatusFileState
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString LockedBy;
};

enum class EForceStatusRefresh : uint8
{
	False,
	True,
};

USTRUCT()
struct FLfsLockOwner
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;
};

USTRUCT()
struct FLfsLock
{

	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FLfsLockOwner Owner;
};

USTRUCT()
struct FRepoStatus
{
	GENERATED_BODY()

	UPROPERTY()
	bool DetachedHead = false;
	UPROPERTY()
	FString LastUpdated;

	// branch
	UPROPERTY()
	FString Branch;
	UPROPERTY()
	FString RemoteBranch;

	// commits
	UPROPERTY()
	uint32 CommitsAhead = 0;
	UPROPERTY()
	uint32 CommitsBehind = 0;
	UPROPERTY()
	FString CommitHeadOrigin;

	// dlls
	UPROPERTY()
	bool OriginHasNewDlls = false;
	UPROPERTY()
	bool PullDlls = false;
	UPROPERTY()
	FString DllCommitLocal;
	UPROPERTY()
	FString DllArchiveForLocal;
	UPROPERTY()
	FString DllCommitRemote;
	UPROPERTY()
	FString DllArchiveForRemote;

	// file paths
	UPROPERTY()
	TArray<FStatusFileState> UntrackedFiles;
	UPROPERTY()
	TArray<FStatusFileState> ModifiedFiles;

	// change detection
	UPROPERTY()
	bool HasStagedChanges = false;
	UPROPERTY()
	bool HasLocalChanges = false;

	// upstream files
	UPROPERTY()
	bool ConflictUpstream = false;
	UPROPERTY()
	TArray<FString> Conflicts;
	UPROPERTY()
	TArray<FString> ModifiedUpstream;

	// locks
	UPROPERTY()
	TArray<FLfsLock> LocksOurs;

	UPROPERTY()
	TArray<FLfsLock> LocksTheirs;
};

USTRUCT()
struct FRevertRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> Files;

	UPROPERTY()
	bool SkipEngineCheck = true;
};

USTRUCT()
struct FLockRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> Paths;
	UPROPERTY()
	bool Force = false;
};

USTRUCT()
struct FLockFailure
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;
	UPROPERTY()
	FString Reason;
};

USTRUCT()
struct FLockResponseInner
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> Paths;
	UPROPERTY()
	TArray<FLockFailure> Failures;
};

USTRUCT()
struct FLockResponse
{
	GENERATED_BODY()

	UPROPERTY()
	FLockResponseInner Batch;
};

USTRUCT()
struct FStorageUploadRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString Prefix;
};

USTRUCT()
struct FStorageDownloadRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString Key;
};

USTRUCT()
struct FStorageListRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString Prefix;
};

USTRUCT()
struct FFileHistoryRevision
{
	GENERATED_BODY()

	UPROPERTY()
	FString Filename;

	UPROPERTY()
	FString CommitId;

	UPROPERTY()
	FString ShortCommitId;

	UPROPERTY()
	int32 CommitIdNumber = 0;

	UPROPERTY()
	int32 RevisionNumber = 0;

	UPROPERTY()
	FString FileHash;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString UserName;

	UPROPERTY()
	FString Action;

	UPROPERTY()
	FDateTime Date;

	UPROPERTY()
	int32 FileSize = 0;
};

USTRUCT()
struct FFileHistoryResponse
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FFileHistoryRevision> Revisions;
};

USTRUCT()
struct FEngineNotifyStateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool InSlowTask = false;
};

enum class ERequestProcessMode
{
	Wait,
	ForceTickHttp,
};

class FFriendshipperClient
{
public:
	FFriendshipperClient();

	void Init(const FString& Url);
	void RefreshNonce();
	bool Diff(TArray<FString>& OutResults);
	bool GetStatus(EForceStatusRefresh ForceRefresh, FRepoStatus& OutStatus);
	bool GetUserInfo(FUserInfo& OutUserInfo);
	bool CheckSystemStatus();
	bool Submit(const FString& InCommitMsg, const TArray<FString>& InFiles);
	bool Revert(const TArray<FString>& InFiles);
	bool LockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages);
	bool UnlockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages);
	bool UploadFile(const FString& Path, const FString& Prefix, const FSimpleDelegate& OnComplete);
	bool DownloadFile(const FString& Path, const FString& Key, const FSimpleDelegate& OnComplete);
	bool ListModelNames(const FString& Prefix, const TDelegate<void(TArray<FString>)>& OnComplete);
	bool GetFileHistory(const FString& Path, FFileHistoryResponse& OutResults);
	bool NotifyEngineState(ERequestProcessMode ProcessMode);

	void AddNonceHeader(const TSharedRef<IHttpRequest>& Request) const;

	void OnRecievedHttpStatusUpdate(const FRepoStatus& RepoStatus);

private:
	static void PromptConflicts(TArray<FString>& Files);

	TSharedRef<IHttpRequest> CreateRequest(const FString& Path, const FString& Method, const FOtelScopedSpan& OtelScopedSpan) const;

	// Friendshipper service URL - probably http://localhost:8484
	FString ServiceUrl;

	FUserInfo UserInfo;

	FRWLock LastRepoStatusLock;
	TOptional<FRepoStatus> LastRepoStatus;

	// Nonce auth token - read from %APPDATA%/Friendshipper/data/.nonce
	mutable FRWLock NonceKeyLock;
	FString NonceKey;
};
