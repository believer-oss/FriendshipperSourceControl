// Copyright The Believer Company. All Rights Reserved.

#include "FriendshipperHttpRouter.h"
#include "FriendshipperOfpaUtils.h"
#include "JsonObjectConverter.h"
#include "HttpServerModule.h"

#include "IHttpRouter.h"

constexpr uint32 RouterPort = 8091;
constexpr bool bFailOnBindFailure = true;

static bool OFPAFriendlyNameRequestHandler(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
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

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FFriendshipperHttpRouter

void FFriendshipperHttpRouter::OnModuleStartup()
{
	FHttpServerModule& Module = FHttpServerModule::Get();
	Module.StartAllListeners();

	TSharedPtr<IHttpRouter> Router = Module.GetHttpRouter(RouterPort, bFailOnBindFailure);
	if (Router)
	{
		const FHttpPath RoutePath = FHttpPath("/friendshipper-ue/ofpa/friendlynames");
		auto Handler = FHttpRequestHandler::CreateStatic(&OFPAFriendlyNameRequestHandler);
		FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_POST, Handler);
		if (Handle)
		{
			Routes.Emplace(Handle);
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
