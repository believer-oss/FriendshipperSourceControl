// Copyright The Believer Company. All Rights Reserved.

#pragma once

#include "HttpRouteHandle.h"

#include "FriendshipperHttpRouter.generated.h"

struct FAssetFriendlyName;

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

struct FFriendshipperHttpRouter
{
	void OnModuleStartup();
	void OnModuleShutdown();

	TArray<FHttpRouteHandle> Routes;
};
