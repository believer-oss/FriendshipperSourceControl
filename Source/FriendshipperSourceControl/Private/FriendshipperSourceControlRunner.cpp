// Copyright Project Borealis

#include "FriendshipperSourceControlRunner.h"

#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlProvider.h"
#include "FriendshipperSourceControlOperations.h"

#include "Async/Async.h"

FFriendshipperSourceControlRunner::FFriendshipperSourceControlRunner()
{
	bRunThread = true;
	bRefreshSpawned = false;
	StopEvent = FPlatformProcess::GetSynchEventFromPool(true);
	Thread = FRunnableThread::Create(this, TEXT("GitSourceControlRunner"));
}

FFriendshipperSourceControlRunner::~FFriendshipperSourceControlRunner()
{
	if (Thread)
	{
		Thread->Kill();
		delete StopEvent;
		delete Thread;
	}
}

bool FFriendshipperSourceControlRunner::Init()
{
	return true;
}

uint32 FFriendshipperSourceControlRunner::Run()
{
	while (bRunThread)
	{
		StopEvent->Wait(30000);
		if (!bRunThread)
		{
			break;
		}
		// If we're not running the task already
		if (!bRefreshSpawned)
		{
			// Flag that we're running the task already
			bRefreshSpawned = true;
			const auto ExecuteResult = Async(EAsyncExecution::TaskGraphMainThread, [=, this] {
				FFriendshipperSourceControlModule* GitSourceControl = FFriendshipperSourceControlModule::GetThreadSafe();
				// Module not loaded, bail. Usually happens when editor is shutting down, and this prevents a crash from bad timing.
				if (!GitSourceControl)
				{
					return ECommandResult::Failed;
				}
				FFriendshipperSourceControlProvider& Provider = GitSourceControl->GetProvider();
				const TSharedRef<FFriendshipperFetch, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FFriendshipperFetch>();
				RefreshOperation->bUpdateStatus = true;
				const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FSourceControlChangelistPtr(), FFriendshipperSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous,
					FSourceControlOperationComplete::CreateRaw(this, &FFriendshipperSourceControlRunner::OnSourceControlOperationComplete));
				return Result;
				});
			// Wait for result if not already completed
			if (bRefreshSpawned && bRunThread)
			{
				// Get the result
				ECommandResult::Type Result = ExecuteResult.Get();
				// If still not completed,
				if (bRefreshSpawned)
				{
					// mark failures as done, successes have to complete
					bRefreshSpawned = Result == ECommandResult::Succeeded;
				}
			}
		}
	}

	return 0;
}

void FFriendshipperSourceControlRunner::Stop()
{
	bRunThread = false;
	StopEvent->Trigger();
}

void FFriendshipperSourceControlRunner::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	// Mark task as done
	bRefreshSpawned = false;
}
