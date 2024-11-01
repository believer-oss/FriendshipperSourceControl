// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlState.h"
#include "FriendshipperSourceControlUtils.h"

#include "Textures/SlateIcon.h"
#if ENGINE_MINOR_VERSION >= 2
#include "RevisionControlStyle/RevisionControlStyle.h"
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl.State"

int32 FFriendshipperSourceControlState::GetHistorySize() const
{
	return History.Num();
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFriendshipperSourceControlState::GetHistoryItem(int32 HistoryIndex) const
{
	check(History.IsValidIndex(HistoryIndex));
	return History[HistoryIndex];
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFriendshipperSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (auto Iter(History.CreateConstIterator()); Iter; Iter++)
	{
		if ((*Iter)->GetRevisionNumber() == RevisionNumber)
		{
			return *Iter;
		}
	}

	return nullptr;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFriendshipperSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevision() == InRevision)
		{
			return Revision;
		}
	}

	return nullptr;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFriendshipperSourceControlState::GetCurrentRevision() const
{
	return nullptr;
}
#endif

// @todo add Slate icons for git specific states (NotAtHead vs Conflicted...)
#if ENGINE_MINOR_VERSION >= 2
#define GET_ICON_RETURN(NAME) FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), NAME)
#else
#define GET_ICON_RETURN(NAME) FSlateIcon(FAppStyle::GetAppStyleSetName(), NAME)
#endif
FSlateIcon FFriendshipperSourceControlState::GetIcon() const
{
	switch (GetGitState())
	{
		case EGitState::NotAtHead:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.NotAtHeadRevision");
#else
			return GET_ICON_RETURN("Perforce.NotAtHeadRevision");
#endif
		case EGitState::LockedOther:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.CheckedOutByOtherUser");
#else
			return GET_ICON_RETURN("Perforce.CheckedOutByOtherUser");
#endif
		case EGitState::NotLatest:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.ModifiedOtherBranch");
#else
			return GET_ICON_RETURN("Perforce.ModifiedOtherBranch");
#endif
		case EGitState::Unmerged:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.Conflicted");
#else
			return GET_ICON_RETURN("Perforce.Branched");
#endif
		case EGitState::Added:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.OpenForAdd");
#else
			return GET_ICON_RETURN("Perforce.OpenForAdd");
#endif
		case EGitState::Untracked:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.NotInDepot");
#else
			return GET_ICON_RETURN("Perforce.NotInDepot");
#endif
		case EGitState::Deleted:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.MarkedForDelete");
#else
			return GET_ICON_RETURN("Perforce.MarkedForDelete");
#endif
		case EGitState::Modified:
		case EGitState::CheckedOut:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.CheckedOut");
#else
			return GET_ICON_RETURN("Perforce.CheckedOut");
#endif
		case EGitState::Ignored:
#if ENGINE_MINOR_VERSION >= 2
			return GET_ICON_RETURN("RevisionControl.NotInDepot");
#else
			return GET_ICON_RETURN("Perforce.NotInDepot");
#endif
		default:
			return FSlateIcon();
	}
}

FText FFriendshipperSourceControlState::GetDisplayName() const
{
	switch (GetGitState())
	{
		case EGitState::NotAtHead:
			return LOCTEXT("NotCurrent", "Not current");
		case EGitState::LockedOther:
			return FText::Format(LOCTEXT("CheckedOutOther", "Checked out by: {0}"), FText::FromString(State.LockUser));
		case EGitState::NotLatest:
			return FText::Format(LOCTEXT("ModifiedOtherBranch", "Modified in branch: {0}"), FText::FromString(State.HeadBranch));
		case EGitState::Unmerged:
			return LOCTEXT("Conflicted", "Conflicted");
		case EGitState::Added:
			return LOCTEXT("OpenedForAdd", "Opened for add");
		case EGitState::Untracked:
			return LOCTEXT("NotControlled", "Not Under Revision Control");
		case EGitState::Deleted:
			return LOCTEXT("MarkedForDelete", "Marked for delete");
		case EGitState::Modified:
		case EGitState::CheckedOut:
			return LOCTEXT("CheckedOut", "Checked out");
		case EGitState::Ignored:
			return LOCTEXT("Ignore", "Ignore");
		case EGitState::Lockable:
			return LOCTEXT("ReadOnly", "Read only");
		case EGitState::None:
			return LOCTEXT("Unknown", "Unknown");
		default:
			return FText();
	}
}

FText FFriendshipperSourceControlState::GetDisplayTooltip() const
{
	switch (GetGitState())
	{
		case EGitState::NotAtHead:
			return LOCTEXT("NotCurrent_Tooltip", "The file(s) are not at the head revision");
		case EGitState::LockedOther:
			return FText::Format(LOCTEXT("CheckedOutOther_Tooltip", "Checked out by: {0}"), FText::FromString(State.LockUser));
		case EGitState::NotLatest:
			return FText::Format(LOCTEXT("ModifiedOtherBranch_Tooltip", "Modified in branch: {0} CL:{1} ({2})"), FText::FromString(State.HeadBranch), FText::FromString(HeadCommit), FText::FromString(HeadAction));
		case EGitState::Unmerged:
			return LOCTEXT("ContentsConflict_Tooltip", "The contents of the item conflict with updates received from the repository.");
		case EGitState::Added:
			return LOCTEXT("OpenedForAdd_Tooltip", "The file(s) are opened for add");
		case EGitState::Untracked:
			return LOCTEXT("NotControlled_Tooltip", "Item is not under revision control.");
		case EGitState::Deleted:
			return LOCTEXT("MarkedForDelete_Tooltip", "The file(s) are marked for delete");
		case EGitState::Modified:
		case EGitState::CheckedOut:
			return LOCTEXT("CheckedOut_Tooltip", "The file(s) are checked out");
		case EGitState::Ignored:
			return LOCTEXT("Ignored_Tooltip", "Item is being ignored.");
		case EGitState::Lockable:
			return LOCTEXT("ReadOnly_Tooltip", "The file(s) are marked locally as read-only");
		case EGitState::None:
			return LOCTEXT("Unknown_Tooltip", "Unknown revision control state");
		default:
			return FText();
	}
}

const FString& FFriendshipperSourceControlState::GetFilename() const
{
	return LocalFilename;
}

const FDateTime& FFriendshipperSourceControlState::GetTimeStamp() const
{
	return TimeStamp;
}

// Deleted and Missing assets cannot appear in the Content Browser, but they do in the Submit files to Revision Control window!
bool FFriendshipperSourceControlState::CanCheckIn() const
{
	// We can check in if this is new content
	if (IsAdded())
	{
		return true;
	}

	// Cannot check back in if conflicted or not current
	if (!IsCurrent() || IsConflicted())
	{
		return false;
	}

	if (FriendshipperSourceControlUtils::IsFileLFSLockable(LocalFilename))
	{
		// We can check back in if we're locked.
		if (State.LockState == ELockState::Locked)
		{
			return true;
		}
	}
	else
	{
		if (IsModified())
		{
			return true;
		}
	}

	return false;
}

bool FFriendshipperSourceControlState::CanCheckout() const
{
	// Packages that don't exist on disk can't be checked out
	if (State.TreeState == ETreeState::NotInRepo)
	{
		return false;
	}

	// untracked files go through the "mark for add" workflow
	if (State.TreeState == ETreeState::Untracked)
	{
		return false;
	}

	if (State.LockState == ELockState::Unlockable)
	{
		// Everything is already available for check in (checked out).
		return false;
	}

	// We don't want to allow checkout if the file is out-of-date, as modifying an out-of-date binary file will most likely result in a merge conflict
	return State.LockState == ELockState::NotLocked && IsCurrent();
}

bool FFriendshipperSourceControlState::IsCheckedOut() const
{
	return State.TreeState != ETreeState::Untracked && State.LockState == ELockState::Locked;
}

bool FFriendshipperSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (Who != nullptr)
	{
		// The packages dialog uses our lock user regardless if it was locked by other or us.
		// But, if there is no lock user, it shows information about modification in other branches, which is important.
		// So, only show our own lock user if it hasn't been modified in another branch.
		// This is a very, very rare state (maybe impossible), but one that should be displayed properly.
		if (State.LockState == ELockState::LockedOther || (State.LockState == ELockState::Locked && !IsModifiedInOtherBranch()))
		{
			*Who = State.LockUser;
		}
	}
	return State.LockState == ELockState::LockedOther;
}

bool FFriendshipperSourceControlState::IsCheckedOutInOtherBranch(const FString& CurrentBranch) const
{
	// You can't check out separately per branch
	return false;
}

bool FFriendshipperSourceControlState::IsModifiedInOtherBranch(const FString& CurrentBranch) const
{
	return State.RemoteState == ERemoteState::NotLatest;
}

bool FFriendshipperSourceControlState::GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const
{
	if (!IsModifiedInOtherBranch())
	{
		return false;
	}

	HeadBranchOut = State.HeadBranch;
	ActionOut = HeadAction; // TODO: from ERemoteState
	HeadChangeListOut = 0;	// TODO: get head commit
	return true;
}

bool FFriendshipperSourceControlState::IsCurrent() const
{
	return State.RemoteState != ERemoteState::NotAtHead && State.RemoteState != ERemoteState::NotLatest;
}

bool FFriendshipperSourceControlState::IsSourceControlled() const
{
	return State.TreeState != ETreeState::Untracked && State.TreeState != ETreeState::Ignored && State.TreeState != ETreeState::NotInRepo;
}

bool FFriendshipperSourceControlState::IsAdded() const
{
	// We don't stage files in this plugin on purpose, but treat untracked + locked files as added
	return (State.TreeState == ETreeState::Staged) || (State.TreeState == ETreeState::Untracked && State.LockState == ELockState::Locked);
}

bool FFriendshipperSourceControlState::IsDeleted() const
{
	return State.FileState == EFileState::Deleted;
}

bool FFriendshipperSourceControlState::IsIgnored() const
{
	return State.TreeState == ETreeState::Ignored;
}

bool FFriendshipperSourceControlState::CanEdit() const
{
	// Perforce does not care about it being current
	return IsCheckedOut() || IsAdded();
}

bool FFriendshipperSourceControlState::CanDelete() const
{
	// Perforce enforces that a deleted file must be current.
	if (!IsCurrent())
	{
		return false;
	}
	// If someone else hasn't checked it out, we can delete revision controlled files.
	return !IsCheckedOutOther() && IsSourceControlled();
}

bool FFriendshipperSourceControlState::IsUnknown() const
{
	return State.FileState == EFileState::Unknown && State.TreeState == ETreeState::NotInRepo;
}

bool FFriendshipperSourceControlState::IsModified() const
{
	return State.TreeState == ETreeState::Working || State.TreeState == ETreeState::Staged;
}

bool FFriendshipperSourceControlState::CanAdd() const
{
	return State.TreeState == ETreeState::Untracked;
}

bool FFriendshipperSourceControlState::IsConflicted() const
{
	return State.FileState == EFileState::Unmerged;
}

bool FFriendshipperSourceControlState::CanRevert() const
{
	// Can revert the file state if we modified, even if it was locked by someone else.
	// Useful for when someone locked a file, and you just wanna play around with it locallly, and then revert it.
	return CanCheckIn() || IsModified();
}

EGitState::Type FFriendshipperSourceControlState::GetGitState() const
{
	// No matter what, we must pull from remote, even if we have locked or if we have modified.
	switch (State.RemoteState)
	{
		case ERemoteState::NotAtHead:
			return EGitState::NotAtHead;
		default:
			break;
	}

	/** Someone else locked this file across branches. */
	// We cannot push under any circumstance, if someone else has locked.
	if (State.LockState == ELockState::LockedOther)
	{
		return EGitState::LockedOther;
	}

	// We could theoretically push, but we shouldn't.
	if (State.RemoteState == ERemoteState::NotLatest)
	{
		return EGitState::NotLatest;
	}

	if (IsAdded())
	{
		return EGitState::Added;
	}

	switch (State.FileState)
	{
		case EFileState::Unmerged:
			return EGitState::Unmerged;
		case EFileState::Deleted:
			return EGitState::Deleted;
		case EFileState::Modified:
			return EGitState::Modified;
		default:
			break;
	}

	if (State.TreeState == ETreeState::Untracked)
	{
		return EGitState::Untracked;
	}

	if (State.LockState == ELockState::Locked)
	{
		return EGitState::CheckedOut;
	}

	if (IsSourceControlled())
	{
		if (CanCheckout())
		{
			return EGitState::Lockable;
		}
		return EGitState::Unmodified;
	}

	return EGitState::None;
}

#undef LOCTEXT_NAMESPACE
