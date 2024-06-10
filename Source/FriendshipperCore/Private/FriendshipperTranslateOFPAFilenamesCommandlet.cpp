// Copyright The Believer Company. All Rights Reserved.

#include "FriendshipperTranslateOFPAFilenamesCommandlet.h"
#include "FriendshipperOfpaUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogFriendshipperTranslateOFPAFilenamesCommandlet, Display, All);

int32 UTranslateOFPAFilenamesCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> SwitchParams;

	UCommandlet::ParseCommandLine(*Params, Tokens, Switches, SwitchParams);

	TArray<FAssetFriendlyName> FriendlyNames = OfpaUtils::TranslatePackagePaths(Tokens); // We expect Tokens to be FilePaths

	for (const FAssetFriendlyName& FriendlyName : FriendlyNames)
	{
		if (FriendlyName.Error.IsEmpty())
		{
			UE_LOG(LogFriendshipperTranslateOFPAFilenamesCommandlet, Display, TEXT("%s has friendly name %s"), *FriendlyName.FilePath, *FriendlyName.AssetName);
		}
		else
		{
			UE_LOG(LogFriendshipperTranslateOFPAFilenamesCommandlet, Warning, TEXT("%s"), *FriendlyName.Error);
		}
	}

	return 0;
}
