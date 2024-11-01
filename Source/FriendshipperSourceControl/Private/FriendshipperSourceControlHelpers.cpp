#include "FriendshipperSourceControlHelpers.h"
#include "FriendshipperSourceControlModule.h"

void FriendshipperSourceControlHelpers::UploadToStorage(const FString& Path, const FString& Prefix, const FSimpleDelegate& OnComplete)
{
	// Upload the file to the storage
	FFriendshipperSourceControlModule::Get().UploadFile(Path, Prefix, OnComplete);
}

void FriendshipperSourceControlHelpers::DownloadFromStorage(const FString& Path, const FString& Key, const FSimpleDelegate& OnComplete)
{
	// Download the file from the storage
	FFriendshipperSourceControlModule::Get().DownloadFile(Path, Key, OnComplete);
}

void FriendshipperSourceControlHelpers::ListModelNames(const FString& Prefix, const TDelegate<void(TArray<FString>)>& OnComplete)
{
	// List all object keys under the prefix from the storage
	FFriendshipperSourceControlModule::Get().ListModelNames(Prefix, OnComplete);
}
