#pragma once

#include "Interfaces/IHttpRequest.h"
#include "Misc/ScopeRWLock.h"
#include "FriendshipperClient.generated.h"

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
	FString IndexState;
	UPROPERTY()
	FString WorkingState;
};

enum class EFetchRemote : uint8
{
	False,
	True,
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
};

USTRUCT()
struct FPushResponse
{
	GENERATED_BODY()

	UPROPERTY()
	bool PushAttempted = false;
	UPROPERTY()
	TArray<FString> Conflicts;
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

class FFriendshipperClient
{
public:
	FFriendshipperClient();

	void Init(const FString& Url);
	void RefreshNonce();
	bool Diff(TArray<FString>& OutResults);
	bool GetStatus(EFetchRemote FetchRemote, FRepoStatus& OutStatus);
	bool GetUserInfo(FUserInfo& OutUserInfo);
	bool GetOperationStatus(FRepoStatus& OutStatus);
	bool CheckSystemStatus();
	bool Submit(const FString& InCommitMsg, const TArray<FString>& InFiles);
	bool LockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages);
	bool UnlockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages);

	void AddNonceHeader(const TSharedRef<IHttpRequest>& Request) const;
	
private:
	static void PromptConflicts(TArray<FString>& Files);

	TSharedRef<IHttpRequest> CreateRequest(const FString& Path, const FString& Method) const;

	// Friendshipper service URL - probably http://localhost:8484
	FString ServiceUrl;

	// Nonce auth token - read from %APPDATA%/Friendshipper/data/.nonce
	mutable FRWLock NonceKeyLock;
	FString NonceKey;
};
