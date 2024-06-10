// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlCommand.h"

#include "Modules/ModuleManager.h"
#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlUtils.h"

FFriendshipperSourceControlCommand::FFriendshipperSourceControlCommand(const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TSharedRef<class IFriendshipperSourceControlWorker, ESPMode::ThreadSafe>& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate)
	: Operation(InOperation)
	, Worker(InWorker)
	, OperationCompleteDelegate(InOperationCompleteDelegate)
	, bExecuteProcessed(0)
	, bCancelled(0)
	, bCommandSuccessful(false)
	, bAutoDelete(true)
	, Concurrency(EConcurrency::Synchronous)
{
	// cache the providers settings here
	const FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	const FFriendshipperSourceControlProvider& Provider = GitSourceControl.GetProvider();
	PathToGitBinary = Provider.GetGitBinaryPath();
	PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
	PathToGitRoot = Provider.GetPathToGitRoot();
}

void FFriendshipperSourceControlCommand::UpdateRepositoryRootIfSubmodule(const TArray<FString>& AbsoluteFilePaths)
{
	PathToRepositoryRoot = FriendshipperSourceControlUtils::ChangeRepositoryRootIfSubmodule(AbsoluteFilePaths, PathToRepositoryRoot);
}

bool FFriendshipperSourceControlCommand::DoWork()
{
	bCommandSuccessful = Worker->Execute(*this);
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);

	return bCommandSuccessful;
}

void FFriendshipperSourceControlCommand::Abandon()
{
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
}

void FFriendshipperSourceControlCommand::DoThreadedWork()
{
	Concurrency = EConcurrency::Asynchronous;
	DoWork();
}

void FFriendshipperSourceControlCommand::Cancel()
{
	FPlatformAtomics::InterlockedExchange(&bCancelled, 1);
}

bool FFriendshipperSourceControlCommand::IsCanceled() const
{
	return bCancelled != 0;
}

ECommandResult::Type FFriendshipperSourceControlCommand::ReturnResults()
{
	// Save any messages that have accumulated
	for (const auto& String : ResultInfo.InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(String));
	}
	for (const auto& String : ResultInfo.ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(String));
	}

	// run the completion delegate if we have one bound
	ECommandResult::Type Result = bCancelled ? ECommandResult::Cancelled : (bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed);
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);

	return Result;
}
