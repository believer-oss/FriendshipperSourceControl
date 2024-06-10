// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlRevision.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlUtils.h"
#include "ISourceControlModule.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

bool FFriendshipperSourceControlRevision::Get( FString& InOutFilename, EConcurrency::Type InConcurrency ) const
{
	if (InConcurrency != EConcurrency::Synchronous)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("Only EConcurrency::Synchronous is tested/supported for this operation."));
	}
	
	const FFriendshipperSourceControlModule* GitSourceControl = FFriendshipperSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return false;
	}
	const FFriendshipperSourceControlProvider& Provider = GitSourceControl->GetProvider();
	const FString PathToGitBinary = Provider.GetGitBinaryPath();
	FString PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
	// the repo root can be customised if in a plugin that has it's own repo
	if (PathToRepoRoot.Len())
	{
		PathToRepositoryRoot = PathToRepoRoot;
	}

	// if a filename for the temp file wasn't supplied generate a unique-ish one
	if(InOutFilename.Len() == 0)
	{
		// create the diff dir if we don't already have it (Git wont)
		IFileManager::Get().MakeDirectory(*FPaths::DiffDir(), true);
		// create a unique temp file name based on the unique commit Id
		const FString TempFileName = FString::Printf(TEXT("%stemp-%s-%s"), *FPaths::DiffDir(), *CommitId, *FPaths::GetCleanFilename(Filename));
		InOutFilename = FPaths::ConvertRelativePathToFull(TempFileName);
	}

	// Diff against the revision
	const FString Parameter = FString::Printf(TEXT("%s:%s"), *CommitId, *Filename);

	bool bCommandSuccessful;
	if(FPaths::FileExists(InOutFilename))
	{
		bCommandSuccessful = true; // if the temp file already exists, reuse it directly
	}
	else
	{
		bCommandSuccessful = FriendshipperSourceControlUtils::RunDumpToFile(PathToGitBinary, PathToRepositoryRoot, Parameter, InOutFilename);
	}
	return bCommandSuccessful;
}

bool FFriendshipperSourceControlRevision::GetAnnotated( TArray<FAnnotationLine>& OutLines ) const
{
	return false;
}

bool FFriendshipperSourceControlRevision::GetAnnotated( FString& InOutFilename ) const
{
	return false;
}

const FString& FFriendshipperSourceControlRevision::GetFilename() const
{
	return Filename;
}

int32 FFriendshipperSourceControlRevision::GetRevisionNumber() const
{
	return RevisionNumber;
}

const FString& FFriendshipperSourceControlRevision::GetRevision() const
{
	return ShortCommitId;
}

const FString& FFriendshipperSourceControlRevision::GetDescription() const
{
	return Description;
}

const FString& FFriendshipperSourceControlRevision::GetUserName() const
{
	return UserName;
}

const FString& FFriendshipperSourceControlRevision::GetClientSpec() const
{
	static FString EmptyString(TEXT(""));
	return EmptyString;
}

const FString& FFriendshipperSourceControlRevision::GetAction() const
{
	return Action;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFriendshipperSourceControlRevision::GetBranchSource() const
{
	// if this revision was copied/moved from some other revision
	return BranchSource;
}

const FDateTime& FFriendshipperSourceControlRevision::GetDate() const
{
	return Date;
}

int32 FFriendshipperSourceControlRevision::GetCheckInIdentifier() const
{
	return CommitIdNumber;
}

int32 FFriendshipperSourceControlRevision::GetFileSize() const
{
	return FileSize;
}

#undef LOCTEXT_NAMESPACE
