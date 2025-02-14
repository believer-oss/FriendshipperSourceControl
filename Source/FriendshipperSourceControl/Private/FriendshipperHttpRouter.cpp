// Copyright The Believer Company. All Rights Reserved.

#include "FriendshipperHttpRouter.h"
#include "FriendshipperClient.h"
#include "FriendshipperOfpaUtils.h"
#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlProvider.h"

#include "HttpManager.h"
#include "HttpModule.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "ISourceControlModule.h"
#include "JsonObjectConverter.h"

constexpr uint32 RouterPort = 8091;

static bool OFPAFriendlyNameRequestHandler(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, FFriendshipperHttpRouter* Router)
{
	FString RequestJson;
	{
		FUTF8ToTCHAR TCHARData(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		RequestJson = FString(TCHARData.Length(), TCHARData.Get());
	}

	FOfpaFriendlyNameRequest RequestBody;
	FJsonObjectConverter::JsonObjectStringToUStruct(RequestJson, &RequestBody);

	FOfpaFriendlyNameResponse ResponseBody;
	ResponseBody.Names = OfpaUtils::TranslatePackagePaths(RequestBody.FileNames);

	FString JsonResponse;
	bool bSuccess = FJsonObjectConverter::UStructToJsonObjectString(ResponseBody, JsonResponse);
	ensure(bSuccess);

	auto Response = FHttpServerResponse::Create(JsonResponse, TEXT("application/json"));
	OnComplete(MoveTemp(Response));

	return true;
}

static bool StatusUpdateRequestHandler(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, FFriendshipperHttpRouter* Router)
{
	check(Router);

	if (Router->OnStatusUpdateRecieved.IsBound())
	{
		FUTF8ToTCHAR TCHARData(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		FString BodyJson = FString(TCHARData.Length(), TCHARData.Get());

		FRepoStatus RepoStatus;
		if (FJsonObjectConverter::JsonObjectStringToUStruct(BodyJson, &RepoStatus))
		{
			Router->OnStatusUpdateRecieved.Execute(RepoStatus);
		}
		else
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Recieved status update from Friendshipper, but was unable to deserialize the body contents:\n%s"), *BodyJson)
		}
	}

	auto Response = FHttpServerResponse::Create(TEXT("{}"), TEXT("application/json"));
	OnComplete(MoveTemp(Response));

	return true;
}

typedef bool HTTPHandlerFunc(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, FFriendshipperHttpRouter* Router);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FFriendshipperHttpTickerHackRunnable
//
// Unreal's main thread can go into "slow tasks" for a variety of reasons (e.g. saving files, building HLODs, etc), with
// no broadcasting of this happening to other systems. This causes the main thread to be blocked, and the HTTP manager
// to not be ticked during the slow task. This is really bad as Friendshipper relies on the ability to issue HTTP requests
// to Unreal to translate OFPA names. If Unreal is running a slow task that relies on a source control operation, such as
// building HLODs and adding/deleting them to/from source control, it's possible for Friendshipper to hang while Unreal
// doesn't answer the request for multiple minutes.
//
// Enter our amazing hack - this thread checks to see if the engine is in a slow task, and, if so, tells Friendshipper
// about it so it can drop any requests that are waiting for Unreal. It ticks the HTTP router manually to process the
// request, which is fine since the HTTP manager tick is threadsafe.

class FFriendshipperHttpTickerHackRunnable : public FRunnable
{
	// Tells friendshipper the engine is/isn't available to handle requests right now depending on the value of GIsSlowTask
	static bool SendNotifyStateRequest()
	{
		if (FFriendshipperSourceControlModule* Module = FFriendshipperSourceControlModule::GetThreadSafe())
		{
			FFriendshipperSourceControlProvider& Provider = Module->GetProvider();
			FFriendshipperClient& Client = Provider.GetFriendshipperClient();
			return Client.NotifyEngineState(ERequestProcessMode::ForceTickHttp);
		}

		return false;
	}

	virtual uint32 Run() override
	{
		float LastSlowTaskTimestamp = 0.0f;
		bool bPrevIsSlowTask = false;
		while (bShouldRun)
		{
			FPlatformProcess::Sleep(0.1);

			if (GIsSlowTask != bPrevIsSlowTask)
			{
				if (FFriendshipperSourceControlModule* Module = FFriendshipperSourceControlModule::GetThreadSafe())
				{
					FFriendshipperSourceControlProvider& Provider = Module->GetProvider();
					FFriendshipperClient& Client = Provider.GetFriendshipperClient();
					if (Client.NotifyEngineState(ERequestProcessMode::ForceTickHttp))
					{
						bPrevIsSlowTask = GIsSlowTask;
					}
				}
				LastSlowTaskTimestamp = FPlatformTime::Seconds();
			}

			if (GIsSlowTask)
			{
				const float AppTime = FPlatformTime::Seconds();
				const float DeltaTime = AppTime - LastSlowTaskTimestamp;
				FHttpModule::Get().GetHttpManager().Tick(DeltaTime);
				LastSlowTaskTimestamp = AppTime;
			}
		}

		return 0;
	}

	virtual void Stop() override
	{
		bShouldRun = false;
	}

	bool bShouldRun = true;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FFriendshipperHttpRouter

void FFriendshipperHttpRouter::OnModuleStartup()
{
	// Only the main editor should be responding to these requests - short lived editor processes like commandlets
	// should defer to the main process.
	if (IsRunningCommandlet() || FApp::IsUnattended())
	{
		return;
	}

	// Only allow one instance of a given UE editor process to bind the router endpoint. Note that
	// FPlatformProcess::NewInterprocessSynchObject() is only implemented on Windows. :(
#if PLATFORM_WINDOWS
	{
		bool bCreate = true;
		InterprocessRouterGuard = FPlatformProcess::NewInterprocessSynchObject(TEXT("FriendshipperHttpRouter"), bCreate);
		if (InterprocessRouterGuard == nullptr)
		{
			return;
		}
	}
#endif

	FHttpServerModule& Module = FHttpServerModule::Get();
	Module.StartAllListeners();

	constexpr const TCHAR* HttpPaths[] = {
		TEXT("/friendshipper-ue/ofpa/friendlynames"),
		TEXT("/friendshipper-ue/status/update"),
	};
	constexpr HTTPHandlerFunc* HttpHandlers[] = {
		&OFPAFriendlyNameRequestHandler,
		&StatusUpdateRequestHandler,
	};
	static_assert(GetNum(HttpPaths) == GetNum(HttpHandlers));

	const bool bFailOnBindFailure = true;
	TSharedPtr<IHttpRouter> Router = Module.GetHttpRouter(RouterPort, bFailOnBindFailure);
	if (Router)
	{
		for (int i = 0; i < GetNum(HttpPaths); ++i)
		{
			const FHttpPath RoutePath = FHttpPath(HttpPaths[i]);
			auto Handler = FHttpRequestHandler::CreateStatic(HttpHandlers[i], this);
			FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_POST, Handler);
			if (Handle)
			{
				Routes.Emplace(Handle);
			}
		}
	}

	HttpTickerHackRunnable = MakeUnique<FFriendshipperHttpTickerHackRunnable>();
	HttpTickerHackThread = TUniquePtr<FRunnableThread>(FRunnableThread::Create(HttpTickerHackRunnable.Get(), TEXT("FriendshipperHttpTickerHack")));
}

void FFriendshipperHttpRouter::OnModuleShutdown()
{
	const bool bFailOnBindFailure = false;
	if (FHttpServerModule::IsAvailable())
	{
		TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(RouterPort, false);
		if (Router)
		{
			for (FHttpRouteHandle Handle : Routes)
			{
				Router->UnbindRoute(Handle);
			}
		}
	}

	if (InterprocessRouterGuard)
	{
		FPlatformProcess::DeleteInterprocessSynchObject(InterprocessRouterGuard);
	}

	if (HttpTickerHackThread)
	{
		bool bShouldWait = true;
		HttpTickerHackThread->Kill(bShouldWait);
	}
}
