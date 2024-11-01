#include "FriendshipperClient.h"
#include "FriendshipperSourceControlModule.h" // kOtelTracer

#include "HttpManager.h"
#include "HttpModule.h"
#include "ISourceControlModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsonObjectConverter.h"
#include "Chaos/ImplicitQRSVD.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Algo/RemoveIf.h"
#include "Otel.h"
#include "AnalyticsEventAttribute.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "FriendshipperClient"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Local helpers

static bool ProcessRequestAndWait(const TSharedRef<IHttpRequest>& Request, FFriendshipperClient& Client)
{
	if (!Request->ProcessRequest())
	{
		return false;
	}

	auto WaitForRequest = [&]()
	{
		double LastTime = FPlatformTime::Seconds();
		while (Request->GetStatus() == EHttpRequestStatus::Processing && !IsEngineExitRequested())
		{
			const double AppTime = FPlatformTime::Seconds();
			if (IsInGameThread())
			{
				FHttpModule::Get().GetHttpManager().Tick(AppTime - LastTime);
				LastTime = AppTime;
			}
			FPlatformProcess::Sleep(0.1);
		}
	};

	WaitForRequest();

	int32 NumNonceAuthRetries = 1;

	while (Request->GetResponse() && Request->GetResponse()->GetResponseCode() == EHttpResponseCodes::Denied && NumNonceAuthRetries > 0) // 401
	{
		--NumNonceAuthRetries;

		Client.RefreshNonce();
		Client.AddNonceHeader(Request);
		if (!Request->ProcessRequest())
		{
			return false;
		}
		WaitForRequest();
	}

	return true;
}

template <typename T>
static bool ParseResponse(const TSharedPtr<IHttpResponse>& Response, T* ResponseData, FOtelScopedSpan& ScopedSpan)
{
	OTEL_TRACER_SCOPED_LOG_HOOK(kOtelTracer, LogSourceControl, ELogVerbosity::Warning);

	bool bSuccess = false;

	if (Response.IsValid())
	{
		const int32 ResponseCode = Response->GetResponseCode();
		const FString ResponseBody = Response->GetContentAsString();
		if (ResponseCode == 200)
		{
			if (ResponseData)
			{
				if (FJsonObjectConverter::JsonObjectStringToUStruct<T>(ResponseBody, ResponseData, 0, 0))
				{
					bSuccess = true;
				}
				else
				{
					FString StructName = T::StaticStruct()->GetName();
					UE_LOG(LogSourceControl, Error, TEXT("Error decoding response json to type %s: %s"), *StructName, *ResponseBody);
				}
			}
			else
			{
				bSuccess = true;
			}
		}
		else
		{
			if (ResponseBody.IsEmpty())
			{
				UE_LOG(LogSourceControl, Error, TEXT("Response has error code: %d"), ResponseCode);
			}
			else
			{
				UE_LOG(LogSourceControl, Error, TEXT("Response error (%d): %s"), ResponseCode, *ResponseBody);
			}
		}
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("HTTP request failed: no response received. Is Friendshipper running?"));
	}

	if (bSuccess == false)
	{
		ScopedSpan.Inner().SetStatus(EOtelStatus::Error);
	}

	return bSuccess;
}

static bool ParseResponse(const TSharedPtr<IHttpResponse>& Response, FOtelScopedSpan& ScopedSpan)
{
	// use FUserInfo here as a dummy struct that is unused
	return ParseResponse<FUserInfo>(Response, nullptr, ScopedSpan);
}

enum class ELockOperation
{
	Lock,
	Unlock,
};

static bool RequestLockOperation(TSharedRef<IHttpRequest> Request, FFriendshipperClient& Client, ELockOperation LockOperation, const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	const TCHAR* OperationName = (LockOperation == ELockOperation::Lock) ? TEXT("lock") : TEXT("unlock");

	FString Body;
	{
		FLockRequest LockRequest;
		LockRequest.Paths = InFiles;
		LockRequest.Force = false;

		bool bSuccess = FJsonObjectConverter::UStructToJsonObjectString(LockRequest, Body);
		ensure(bSuccess);
	}

	Request->SetContentAsString(Body);

	ProcessRequestAndWait(Request, Client);

	FLockResponse LockResponse;
	if (ParseResponse(Request->GetResponse(), &LockResponse, OtelSpan))
	{
		for (FLockFailure& Failure : LockResponse.Batch.Failures)
		{
			if (OutFailedFiles)
			{
				OutFailedFiles->Emplace(MoveTemp(Failure.Path));
			}

			FString FailMsg = FString::Printf(TEXT("Failed to %s asset %s: %s"), OperationName, *Failure.Path, *Failure.Reason);
			OtelSpan.Inner().AddEvent(*FailMsg, {});
			if (OutFailureMessages)
			{
				OutFailureMessages->Emplace(FailMsg);
			}
		}

		// In the future we can return true if there was a partial success
		if (LockResponse.Batch.Failures.Num() > 0)
		{
			OtelSpan.Inner().SetStatus(EOtelStatus::Error);
			return false;
		}
	}

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// FFriendshipperClient

FFriendshipperClient::FFriendshipperClient() = default;

TSharedRef<IHttpRequest> FFriendshipperClient::CreateRequest(const FString& Path, const FString& Method, const FOtelScopedSpan& OtelScopedSpan) const
{
	const FString uri = FString::Printf(TEXT("%s/%s"), *ServiceUrl, *Path);

	FHttpModule& HttpModule = FHttpModule::Get();

	const TSharedRef<IHttpRequest> Request = HttpModule.CreateRequest();
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	AddNonceHeader(Request);
	Request->SetVerb(Method);

	Request->SetTimeout(300);
	Request->SetActivityTimeout(300);

	Request->SetURL(uri);

	// otel hooks
	FOtelSpan OtelSpan = OtelScopedSpan.Inner();
	const FString OtelTraceId = OtelSpan.TraceId();
	if (OtelTraceId.IsEmpty() == false)
	{
		Request->SetHeader(TEXT("x-trace-id"), OtelTraceId);
	}

	auto RouteAttrib = FAnalyticsEventAttribute(TEXT("route"), FString::Printf(TEXT("%s %s"), *Method, *Path));
	OtelSpan.AddAttribute(RouteAttrib);

	return Request;
}

void FFriendshipperClient::AddNonceHeader(const TSharedRef<IHttpRequest>& Request) const
{
	FReadScopeLock Lock = FReadScopeLock(NonceKeyLock);
	Request->SetHeader("X-Ethos-Nonce", NonceKey);
}

void FFriendshipperClient::OnRecievedHttpStatusUpdate(const FRepoStatus& RepoStatus)
{
	FWriteScopeLock Lock = FWriteScopeLock(LastRepoStatusLock);
	LastRepoStatus = RepoStatus;
}

bool FFriendshipperClient::Diff(TArray<FString>& OutResults)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/diff"), TEXT("GET"), OtelSpan);

	ProcessRequestAndWait(pRequest, *this);

	if (const TSharedPtr<IHttpResponse> Response = pRequest->GetResponse())
	{
		if (Response->GetResponseCode() != 0)
		{
			TArray<TSharedPtr<FJsonValue>> JsonArray;
			const FString& ResponseBody = Response->GetContentAsString();
			TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ResponseBody);
			UE_LOG(LogSourceControl, Log, TEXT("%s"), *ResponseBody)
			if (FJsonSerializer::Deserialize(JsonReader, JsonArray))
			{
				for (const TSharedPtr<FJsonValue>& JsonValue : JsonArray)
				{
					OutResults.Add(JsonValue->AsString());
				}
			}
		}
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("HTTP request failed."))

		return false;
	}

	return true;
}

bool FFriendshipperClient::GetUserInfo(FUserInfo& OutUserInfo)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/gh/user"), TEXT("GET"), OtelSpan);

	ProcessRequestAndWait(pRequest, *this);

	if (ParseResponse(pRequest->GetResponse(), &UserInfo, OtelSpan))
	{
		OutUserInfo = UserInfo;
		return true;
	}

	return false;
}

bool FFriendshipperClient::GetStatus(EForceStatusRefresh ForceRefresh, FRepoStatus& OutStatus)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	bool bIsSet = false;
	{
		FReadScopeLock Lock = FReadScopeLock(LastRepoStatusLock);
		bIsSet = LastRepoStatus.IsSet();
	}

	bool bSuccess = true;
	if ((bIsSet == false) || (ForceRefresh == EForceStatusRefresh::True))
	{
		const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/status?skipDllCheck=true&skipEngineUpdate=true"), TEXT("GET"), OtelSpan);

		ProcessRequestAndWait(pRequest, *this);

		FRepoStatus TempStatus;
		if (ParseResponse(pRequest->GetResponse(), &TempStatus, OtelSpan))
		{
			FWriteScopeLock Lock = FWriteScopeLock(LastRepoStatusLock);
			LastRepoStatus = TempStatus;
		}
		else
		{
			bSuccess = false;
		}
	}

	FReadScopeLock Lock = FReadScopeLock(LastRepoStatusLock);
	if (LastRepoStatus.IsSet())
	{
		OutStatus = *LastRepoStatus;
	}

	return bSuccess;
}

bool FFriendshipperClient::Submit(const FString& InCommitMsg, const TArray<FString>& InFiles)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	FString Body;
	{
		const TSharedPtr<FJsonObject> JsonObject = MakeShareable<>(new FJsonObject());
		TArray<TSharedPtr<FJsonValue>> Files;
		for (const FString& File : InFiles)
		{
			Files.Add(MakeShareable(new FJsonValueString(File)));
		}

		JsonObject->SetStringField("commitMessage", InCommitMsg);
		JsonObject->SetArrayField("files", Files);

		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	}

	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/gh/submit"), TEXT("POST"), OtelSpan);
	pRequest->SetContentAsString(Body);

	ProcessRequestAndWait(pRequest, *this);

	return ParseResponse(pRequest->GetResponse(), OtelSpan);
}

bool FFriendshipperClient::Revert(const TArray<FString>& InFiles)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	FString Body;
	{
		FRevertRequest Request;
		Request.Files = InFiles;
		Request.SkipEngineCheck = true;

		bool bSuccess = FJsonObjectConverter::UStructToJsonObjectString(Request, Body);
		ensure(bSuccess);
	}

	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("repo/revert"), TEXT("POST"), OtelSpan);
	Request->SetContentAsString(Body);

	ProcessRequestAndWait(Request, *this);

	return ParseResponse(Request->GetResponse(), OtelSpan);
}

bool FFriendshipperClient::LockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	check(OutFailedFiles);

	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("repo/locks/lock"), TEXT("POST"), OtelSpan);
	bool bSuccess = RequestLockOperation(Request, *this, ELockOperation::Lock, InFiles, OutFailedFiles, OutFailureMessages);

	FWriteScopeLock Lock = FWriteScopeLock(LastRepoStatusLock);

	if (LastRepoStatus.IsSet())
	{
		for (const FString& File : InFiles)
		{
			if (OutFailedFiles->Contains(File) == false)
			{
				FLfsLock LfsLock;
				LfsLock.Path = File;
				LfsLock.Owner.Name = UserInfo.Username;
				LastRepoStatus->LocksOurs.Add(LfsLock);
			}
		}
	}

	return bSuccess;
}

bool FFriendshipperClient::UnlockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	check(OutFailedFiles);

	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("repo/locks/unlock"), TEXT("POST"), OtelSpan);
	bool bSuccess = RequestLockOperation(Request, *this, ELockOperation::Unlock, InFiles, OutFailedFiles, OutFailureMessages);

	FWriteScopeLock Lock = FWriteScopeLock(LastRepoStatusLock);

	if (LastRepoStatus.IsSet())
	{
		int Num = Algo::StableRemoveIf(LastRepoStatus->LocksOurs, [&InFiles, OutFailedFiles](const FLfsLock& Lock)
			{
				return InFiles.Contains(Lock.Path) && (OutFailedFiles->Contains(Lock.Path) == false);
			});
		LastRepoStatus->LocksOurs.SetNum(Num);
	}

	return bSuccess;
}

bool FFriendshipperClient::GetFileHistory(const FString& Path, FFileHistoryResponse& OutResults)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	// format path as urlencoded query param
	FString EncodedPath = FGenericPlatformHttp::UrlEncode(Path);
	FString HistoryPath = FString::Printf(TEXT("repo/file-history?path=%s"), *EncodedPath);

	const TSharedRef<IHttpRequest> pRequest = CreateRequest(*HistoryPath, TEXT("GET"), OtelSpan);

	ProcessRequestAndWait(pRequest, *this);

	// parse response
	FFileHistoryResponse HistoryResponse;
	if (ParseResponse(pRequest->GetResponse(), &HistoryResponse, OtelSpan))
	{
		// convert to source control revisions
		OutResults = MoveTemp(HistoryResponse);
		return true;
	}

	return false;
}

bool FFriendshipperClient::CheckSystemStatus()
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("system/status"), TEXT("GET"), OtelSpan);

	ProcessRequestAndWait(pRequest, *this);

	if (const TSharedPtr<IHttpResponse> Response = pRequest->GetResponse())
	{
		return Response->GetResponseCode() == 200;
	}

	return false;
}

bool FFriendshipperClient::UploadFile(const FString& Path, const FString& Prefix, const FSimpleDelegate& OnComplete)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	FString Body;
	{
		FStorageUploadRequest Request;
		Request.Path = Path;
		Request.Prefix = Prefix;

		bool bSuccess = FJsonObjectConverter::UStructToJsonObjectString(Request, Body);
		ensure(bSuccess);
	}

	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("storage/upload"), TEXT("POST"), OtelSpan);
	Request->SetContentAsString(Body);

	bool bSuccess = ProcessRequestAndWait(Request, *this);

	OnComplete.ExecuteIfBound();

	return bSuccess;
}

bool FFriendshipperClient::DownloadFile(const FString& Path, const FString& Key, const FSimpleDelegate& OnComplete)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	FString Body;
	{
		FStorageDownloadRequest Request;
		Request.Path = Path;
		Request.Key = Key;

		bool bSuccess = FJsonObjectConverter::UStructToJsonObjectString(Request, Body);
		ensure(bSuccess);
	}

	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("storage/download"), TEXT("POST"), OtelSpan);
	Request->SetContentAsString(Body);

	bool bSuccess = ProcessRequestAndWait(Request, *this);

	OnComplete.ExecuteIfBound();

	return bSuccess;
}

bool FFriendshipperClient::ListModelNames(const FString& Prefix, const TDelegate<void(TArray<FString>)>& OnComplete)
{
	FOtelScopedSpan OtelSpan = OTEL_TRACER_SPAN_FUNC(kOtelTracer);

	FString Body;
	{
		FStorageListRequest Request;
		Request.Prefix = Prefix;

		bool bSuccess = FJsonObjectConverter::UStructToJsonObjectString(Request, Body);
		ensure(bSuccess);
	}

	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("storage/list"), TEXT("POST"), OtelSpan);
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete() = FHttpRequestCompleteDelegate::CreateLambda([OnComplete](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			TArray<FString> ModelNames;
			if (bConnectedSuccessfully)
			{
				if (Response)
				{
					const int ResponseCode = Response->GetResponseCode();
					if (ResponseCode == EHttpResponseCodes::Ok)
					{
						FString ResponseBody = Response->GetContentAsString();
						TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ResponseBody);
						TArray<TSharedPtr<FJsonValue>> JsonModelPaths;

						if (FJsonSerializer::Deserialize(JsonReader, JsonModelPaths))
						{
							for (TSharedPtr<FJsonValue> Value : JsonModelPaths)
							{
								if (Value->Type == EJson::String)
								{
									// if the component after "models" is a folder, then add the folder name to ModelNames
									FString Path = Value->AsString();
									TArray<FString> Components;
									Path.ParseIntoArray(Components, TEXT("/"), true);
									int32 ModelsIndex = Components.Find(TEXT("models"));

									if (ModelsIndex != INDEX_NONE && Components.IsValidIndex(ModelsIndex + 1))
									{
										FString ModelName = Components[ModelsIndex + 1];
										if (!ModelName.Contains(TEXT(".")) && !ModelNames.Contains(ModelName))
										{
											ModelNames.Add(ModelName);
										}
									}
								}
							}
						}
					}
					else
					{
						UE_LOG(LogSourceControl, Error, TEXT("Failed to fetch model objects"));
					}
				}
				else
				{
					UE_LOG(LogSourceControl, Error, TEXT("Unable to get response from S3. Was the request unable to be sent?"));
				}
			}
			else
			{
				UE_LOG(LogSourceControl, Error, TEXT("Failed to connect to remote endpoint"));
			}

			OnComplete.Execute(MoveTemp(ModelNames));
		});

	return Request->ProcessRequest();
}

void FFriendshipperClient::Init(const FString& Url)
{
	ServiceUrl = Url;
}

void FFriendshipperClient::RefreshNonce()
{
	FWriteScopeLock Lock = FWriteScopeLock(NonceKeyLock);

	TStringBuilder<256> Path;
	Path.Append(FPlatformProcess::UserSettingsDir());
#if PLATFORM_WINDOWS
	Path.Append("../Roaming/"); // UserSettingsDir() returns the Local appdata dir by default, but Friendshipper data is in Roaming
#endif
	Path.Append("Friendshipper/data/.nonce");
	if (FFileHelper::LoadFileToString(NonceKey, Path.ToString()) == false)
	{
		TStringBuilder<256> LegacyPath;
		LegacyPath.Append(FPlatformProcess::UserSettingsDir());
#if PLATFORM_WINDOWS
		LegacyPath.Append("../Roaming/"); // UserSettingsDir() returns the Local appdata dir by default, but Friendshipper data is in Roaming
#endif
		LegacyPath.Append("Fellowshipper/data/.nonce");

		// compatibility for older versions of Friendshipper
		if (FFileHelper::LoadFileToString(NonceKey, LegacyPath.ToString()))
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Failed to read Friendshipper nonce key from path '%s'. Source control operations will fail."), Path.ToString());
		}
	}
}

void FFriendshipperClient::PromptConflicts(TArray<FString>& Files)
{
	FText Message(LOCTEXT("Friendshipper_IsSvcRunning_Msg", "Source control process timed out. Is Friendshipper running?"));

	for (const FString& File : Files)
	{
		Message = FText::Format(LOCTEXT("Friendshipper_Conflict_Format", "{0}\n{1}"), Message, FText::FromString(File));
	}

	FMessageDialog::Open(EAppMsgType::Ok, Message);
}

#undef LOCTEXT_NAMESPACE
