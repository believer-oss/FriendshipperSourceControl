#pragma once

namespace FriendshipperSourceControlHelpers
{
	void FRIENDSHIPPERSOURCECONTROL_API UploadToStorage(const FString& Path, const FString& Prefix, const FSimpleDelegate& OnComplete);
	void FRIENDSHIPPERSOURCECONTROL_API DownloadFromStorage(const FString& Path, const FString& Key, const FSimpleDelegate& OnComplete);
	void FRIENDSHIPPERSOURCECONTROL_API ListModelNames(const FString& Prefix, const TDelegate<void(TArray<FString>)>& OnComplete);
} // namespace FriendshipperSourceControlHelpers