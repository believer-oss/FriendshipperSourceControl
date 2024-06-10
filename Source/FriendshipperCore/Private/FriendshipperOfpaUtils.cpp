// Copyright The Believer Company. All Rights Reserved.

#include "FriendshipperOfpaUtils.h"

#include "ActorFolder.h"

TArray<FAssetFriendlyName> OfpaUtils::TranslatePackagePaths(TArrayView<const FString> FilePaths)
{
	TArray<FString> PackagePaths;
	PackagePaths.Reserve(FilePaths.Num());

	for (const FString& Path : FilePaths)
	{
		FString PackagePath;
		if (FPaths::IsRelative(Path))
		{
			PackagePath = FPaths::ProjectDir() / Path;
		}
		else
		{
			PackagePath = Path;
		}

		PackagePaths.Emplace(MoveTemp(PackagePath));
	}

	TArray<FAssetFriendlyName> FriendlyNames;
	FriendlyNames.Reserve(PackagePaths.Num());

	for (int i = 0; i < PackagePaths.Num(); ++i)
	{
		const FString& Path = PackagePaths[i];
		FString AssetName;
		FString Error;

		UPackage* TempPackage = LoadPackage(nullptr, *Path, LOAD_DisableCompileOnLoad | LOAD_SkipLoadImportedPackages | LOAD_DisableDependencyPreloading);
		if (TempPackage != nullptr)
		{
			if (UObject* Obj = TempPackage->FindAssetInPackage())
			{
				if (AActor* Actor = Cast<AActor>(Obj))
				{
					AssetName = Actor->GetActorLabel();
				}
				else if (UActorFolder* Folder = Cast<UActorFolder>(Obj))
				{
					const TCHAR* DeletedStr = Folder->IsMarkedAsDeleted() ? TEXT(" <Deleted>") : TEXT("");
					AssetName = FString::Printf(TEXT("%s%s (Folder)"), *Folder->GetLabel(), DeletedStr);
				}

				if (AssetName.IsEmpty())
				{
					AssetName = Obj->GetName();
				}
			}
			else
			{
				Error = FString::Printf(TEXT("Failed to find UObject inside package %s"), *Path);
			}
		}
		else
		{
			Error = FString::Printf(TEXT("Failed to find package for path %s"), *Path);
		}

		FriendlyNames.Emplace(FAssetFriendlyName{ FilePaths[i], AssetName, Error });
	}

	return FriendlyNames;
}
