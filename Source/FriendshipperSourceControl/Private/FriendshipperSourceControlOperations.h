// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "IFriendshipperSourceControlWorker.h"
#include "FriendshipperSourceControlState.h"

#include "ISourceControlOperation.h"

/**
 * Internal operation used to fetch from remote
 */
class FFriendshipperFetch : public ISourceControlOperation
{
public:
	// ISourceControlOperation interface
	virtual FName GetName() const override;

	virtual FText GetInProgressString() const override;

	bool bUpdateStatus = false;
};

/** Called when first activated on a project, and then at project load time.
 *  Look for the root directory of the git repository (where the ".git/" subdirectory is located). */
class FFriendshipperConnectWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperConnectWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** Lock (check-out) a set of files using Git LFS 2. */
class FFriendshipperCheckOutWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperCheckOutWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** Commit (check-in) a set of files to the local depot. */
class FFriendshipperCheckInWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperCheckInWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** Add an untracked file to revision control (so only a subset of the git add command). */
class FFriendshipperMarkForAddWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperMarkForAddWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** Delete a file and remove it from revision control. */
class FFriendshipperDeleteWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperDeleteWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** Revert any change to a file to its state on the local depot. */
class FFriendshipperRevertWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperRevertWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** Get revision control status of files on local working copy. */
class FFriendshipperUpdateStatusWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperUpdateStatusWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;

	/** Map of filenames to history */
	TMap<FString, TGitSourceControlHistory> Histories;
};

/** Copy or Move operation on a single file */
class FFriendshipperCopyWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperCopyWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** git add to mark a conflict as resolved */
class FFriendshipperResolveWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperResolveWorker() {}
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};

/** Git push to publish branch for its configured remote */
class FFriendshipperFetchWorker : public IFriendshipperSourceControlWorker
{
public:
	virtual ~FFriendshipperFetchWorker() {}
	// IFriendshipperSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FFriendshipperSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FFriendshipperState> States;
};
