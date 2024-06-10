// Copyright The Believer Company. All Rights Reserved.

#pragma once

#include "FriendshipperOfpaUtils.generated.h"

USTRUCT()
struct FAssetFriendlyName
{
	GENERATED_BODY()

	UPROPERTY()
	FString FilePath;

	UPROPERTY()
	FString AssetName;

	UPROPERTY()
	FString Error;
};

namespace OfpaUtils
{
	// Translates the given asset file paths into friendly names as seen in-editor
	FRIENDSHIPPERCORE_API TArray<FAssetFriendlyName> TranslatePackagePaths(TArrayView<const FString> FilePaths);
} // namespace OfpaUtils
