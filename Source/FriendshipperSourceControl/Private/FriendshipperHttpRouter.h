// Copyright The Believer Company. All Rights Reserved.

#pragma once

#include "HttpRouteHandle.h"

#include "FriendshipperHttpRouter.generated.h"

struct FAssetFriendlyName;
struct FRepoStatus;

USTRUCT()
struct FOfpaFriendlyNameRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> FileNames;
};

USTRUCT()
struct FOfpaFriendlyNameResponse
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FAssetFriendlyName> Names;
};

DECLARE_DELEGATE_OneParam(FOnStatusUpdate, const FRepoStatus& RepoStatus);

struct FFriendshipperHttpRouter
{
	void OnModuleStartup();
	void OnModuleShutdown();

	FOnStatusUpdate OnStatusUpdateRecieved;

	TArray<FHttpRouteHandle> Routes;

	FGenericPlatformProcess::FSemaphore* InterprocessRouterGuard = nullptr;
	TUniquePtr<FRunnable> HttpTickerHackRunnable;
	TUniquePtr<FRunnableThread> HttpTickerHackThread;
};
