// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "SourceControlHelpers.h"

namespace FriendshipperSettingsConstants
{
	/** The section of the ini file we load our settings from */
	static const FString SettingsSection = TEXT("GitSourceControl.GitSourceControlSettings");
}

const FString FFriendshipperSourceControlSettings::GetBinaryPath() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return BinaryPath; // Return a copy to be thread-safe
}

bool FFriendshipperSourceControlSettings::SetBinaryPath(const FString& InString)
{
	FScopeLock ScopeLock(&CriticalSection);
	const bool bChanged = (BinaryPath != InString);
	if(bChanged)
	{
		BinaryPath = InString;
	}
	return bChanged;
}

const FString FFriendshipperSourceControlSettings::GetLfsUserName() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return LfsUserName; // Return a copy to be thread-safe
}

bool FFriendshipperSourceControlSettings::SetLfsUserName(const FString& InString)
{
	FScopeLock ScopeLock(&CriticalSection);
	const bool bChanged = (LfsUserName != InString);
	if (bChanged)
	{
		LfsUserName = InString;
	}
	return bChanged;
}

// This is called at startup nearly before anything else in our module: BinaryPath will then be used by the provider
void FFriendshipperSourceControlSettings::LoadSettings()
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->GetString(*FriendshipperSettingsConstants::SettingsSection, TEXT("BinaryPath"), BinaryPath, IniFile);
}

void FFriendshipperSourceControlSettings::Save() const
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->SetString(*FriendshipperSettingsConstants::SettingsSection, TEXT("BinaryPath"), *BinaryPath, IniFile);
}
