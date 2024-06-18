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

	if (FString* ListFilePath = SwitchParams.Find(TEXT("ListFile")))
	{
		Tokens.Reset();

		// Trim surrounding single quotes if present. Friendshipper passes this string in single quotes to safely
		// handle spaces in the path.
		if (ListFilePath->StartsWith("'"))
		{
			ListFilePath->RightChopInline(1, EAllowShrinking::No);
		}

		if (ListFilePath->EndsWith("'"))
		{
			ListFilePath->LeftChopInline(1, EAllowShrinking::No);
		}

		FString FileContents;
		if (FFileHelper::LoadFileToString(FileContents, **ListFilePath) == false)
		{
			UE_LOG(LogFriendshipperTranslateOFPAFilenamesCommandlet, Error,
				TEXT("Unable to find provided ListFile '%s'. Unable to translate filenames."),
				**ListFilePath);
			return 1;
		}

		bool bInCullEmpty = true;
		FileContents.ParseIntoArray(Tokens, TEXT("\n"), bInCullEmpty); // friendshipper always writes newlines only
	}

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
