// Copyright The Believer Company. All Rights Reserved.

#include "FriendshipperHttpRouter.h"
#include "FriendshipperOfpaUtils.h"
#include "FriendshipperClient.h"

#include "JsonObjectConverter.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "ISourceControlModule.h"

constexpr uint32 RouterPort = 8091;
constexpr bool bFailOnBindFailure = true;

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
// FFriendshipperHttpRouter

void FFriendshipperHttpRouter::OnModuleStartup()
{
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
}

void FFriendshipperHttpRouter::OnModuleShutdown()
{
	TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(RouterPort, bFailOnBindFailure);
	if (Router)
	{
		for (FHttpRouteHandle Handle : Routes)
		{
			Router->UnbindRoute(Handle);
		}
	}
}
