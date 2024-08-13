#include "FriendshipperClient.h"

#include "HttpManager.h"
#include "HttpModule.h"
#include "ISourceControlModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsonObjectConverter.h"
#include "Chaos/ImplicitQRSVD.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"

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

enum class ELockOperation
{
	Lock,
	Unlock,
};

static bool RequestLockOperation(TSharedRef<IHttpRequest> Request, FFriendshipperClient& Client, ELockOperation LockOperation, const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages)
{
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
	if (ProcessRequestAndWait(Request, Client) == false)
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to process %s operation request."), OperationName);
		return false;
	}

	if (const TSharedPtr<IHttpResponse> Response = Request->GetResponse())
	{
		const FString& ResponseBody = Response->GetContentAsString();
		if (Response->GetResponseCode() == 200)
		{
			FLockResponse LockResponse;
			if (FJsonObjectConverter::JsonObjectStringToUStruct(ResponseBody, &LockResponse, 0, 0))
			{
				for (FLockFailure& Failure : LockResponse.Batch.Failures)
				{
					if (OutFailedFiles)
					{
						OutFailedFiles->Emplace(MoveTemp(Failure.Path));
					}

					if (OutFailureMessages)
					{
						OutFailureMessages->Emplace(FString::Printf(TEXT("Failed to %s asset %s: %s"), OperationName, *Failure.Path, *Failure.Reason));
					}
				}

				// In the future we can return true if there was a partial success
				if (LockResponse.Batch.Failures.Num() > 0)
				{
					return false;
				}
			}
			else
			{
				UE_LOG(LogSourceControl, Log, TEXT("Error decoding status for %s operation - body: %s"), OperationName, *ResponseBody);
				// note we don't return false here because the response code is good
			}
		}
		else
		{
			UE_LOG(LogSourceControl, Error, TEXT("Failed to perform %s operation. EHttpErrorCode: %d. Error: %s"), OperationName, Response->GetResponseCode(), *ResponseBody);
			return false;
		}
	}

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// FFriendshipperClient

FFriendshipperClient::FFriendshipperClient() = default;

TSharedRef<IHttpRequest> FFriendshipperClient::CreateRequest(const FString& Path, const FString& Method) const
{
	const FString uri = FString::Printf(TEXT("%s/%s"), *ServiceUrl, *Path);

	FHttpModule& HttpModule = FHttpModule::Get();

	const TSharedRef<IHttpRequest> Request = HttpModule.CreateRequest();
	Request->SetHeader("Content-Type", TEXT("application/json"));
	AddNonceHeader(Request);
	Request->SetVerb(Method);

	// Submits are potentially long operations, so we set a long timeout
	Request->SetTimeout(300);
	Request->SetActivityTimeout(300);
	
	Request->SetURL(uri);

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
	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/diff"), TEXT("GET"));

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
	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/gh/user"), TEXT("GET"));

	ProcessRequestAndWait(pRequest, *this);

	if (const TSharedPtr<IHttpResponse> Response = pRequest->GetResponse())
	{
		if (Response->GetResponseCode() != 0)
		{
			const FString& ResponseBody = Response->GetContentAsString();

			if (!FJsonObjectConverter::JsonObjectStringToUStruct(Response->GetContentAsString(), &OutUserInfo, 0, 0))
			{
				UE_LOG(LogSourceControl, Log, TEXT("Error decoding Status - body: %s"), *ResponseBody)
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

bool FFriendshipperClient::GetStatus(EForceStatusRefresh ForceRefresh, FRepoStatus& OutStatus)
{
	bool bIsSet = false;
	{
		FReadScopeLock Lock = FReadScopeLock(LastRepoStatusLock);
		bIsSet = LastRepoStatus.IsSet();
	}

	if ((bIsSet == false) || (ForceRefresh == EForceStatusRefresh::True))
	{
		const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/status?skipDllCheck=true&skipEngineUpdate=true"), TEXT("GET"));

		ProcessRequestAndWait(pRequest, *this);

		if (const TSharedPtr<IHttpResponse> Response = pRequest->GetResponse())
		{
			if (Response->GetResponseCode() != 0)
			{
				const FString& ResponseBody = Response->GetContentAsString();

				FRepoStatus TempStatus;
				if (FJsonObjectConverter::JsonObjectStringToUStruct(Response->GetContentAsString(), &TempStatus, 0, 0))
				{
					FWriteScopeLock Lock = FWriteScopeLock(LastRepoStatusLock);
					LastRepoStatus = TempStatus;
				}
				else
				{
					UE_LOG(LogSourceControl, Log, TEXT("GetStatus: Error decoding json: %s"), *ResponseBody);
					return false;
				}
			}
			else
			{
				UE_LOG(LogSourceControl, Log, TEXT("GetStatus returned response error code: %d"), Response->GetResponseCode());
				return false;
			}
		}
		else
		{
			UE_LOG(LogSourceControl, Error, TEXT("HTTP request failed."))
			FWriteScopeLock Lock = FWriteScopeLock(LastRepoStatusLock);
			LastRepoStatus.Reset();
			return false;
		}
	}

	FWriteScopeLock Lock = FWriteScopeLock(LastRepoStatusLock);
	OutStatus = *LastRepoStatus;
	return true;
}

bool FFriendshipperClient::Submit(const FString& InCommitMsg, const TArray<FString>& InFiles)
{
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

	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("repo/gh/submit"), TEXT("POST"));
	pRequest->SetContentAsString(Body);

	ProcessRequestAndWait(pRequest, *this);

	if (const TSharedPtr<IHttpResponse> Response = pRequest->GetResponse())
	{
		if (Response->GetResponseCode() == 200)
		{
			UE_LOG(LogSourceControl, Log, TEXT("Successfully submitted content."));
		}
		else
		{
			const FString& ResponseBody = Response->GetContentAsString();
			UE_LOG(LogSourceControl, Error, TEXT("Failed to submit content. EHttpErrorCode: %d. Error: %s"), Response->GetResponseCode(), *ResponseBody);
			return false;
		}
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("Submit: HTTP request failed."));

		return false;
	}

	return true;
}

bool FFriendshipperClient::Revert(const TArray<FString>& InFiles)
{
	FString Body;
	{
		FRevertRequest Request;
		Request.Files = InFiles;
		Request.SkipEngineCheck = true;

		bool bSuccess = FJsonObjectConverter::UStructToJsonObjectString(Request, Body);
		ensure(bSuccess);
	}

	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("repo/revert"), TEXT("POST"));
	Request->SetContentAsString(Body);

	ProcessRequestAndWait(Request, *this);

	if (const TSharedPtr<IHttpResponse> Response = Request->GetResponse())
	{
		if (Response->GetResponseCode() == 200)
		{
			UE_LOG(LogSourceControl, Log, TEXT("Successfully reverted files."));
		}
		else
		{
			const FString& ResponseBody = Response->GetContentAsString();
			UE_LOG(LogSourceControl, Error, TEXT("Failed to revert files. EHttpErrorCode: %d. Error: %s"), Response->GetResponseCode(), *ResponseBody);
			return false;
		}
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("Revert: HTTP request failed."));
		return false;
	}

	return true;
}

bool FFriendshipperClient::LockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages)
{
	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("repo/locks/lock"), TEXT("POST"));
	return RequestLockOperation(Request, *this, ELockOperation::Lock, InFiles, OutFailedFiles, OutFailureMessages);
}

bool FFriendshipperClient::UnlockFiles(const TArray<FString>& InFiles, TArray<FString>* OutFailedFiles, TArray<FString>* OutFailureMessages)
{
	const TSharedRef<IHttpRequest> Request = CreateRequest(TEXT("repo/locks/unlock"), TEXT("POST"));
	return RequestLockOperation(Request, *this, ELockOperation::Unlock, InFiles, OutFailedFiles, OutFailureMessages);
}

bool FFriendshipperClient::CheckSystemStatus()
{
	const TSharedRef<IHttpRequest> pRequest = CreateRequest(TEXT("system/status"), TEXT("GET"));

	ProcessRequestAndWait(pRequest, *this);

	if (const TSharedPtr<IHttpResponse> Response = pRequest->GetResponse())
	{
		return Response->GetResponseCode() == 200;
	}

	return false;
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
		if (FFileHelper::LoadFileToString(NonceKey, LegacyPath.ToString())) {
			UE_LOG(LogSourceControl, Warning, TEXT("Failed to read Friendshipper nonce key from path '%s'. Source control operations will fail."), Path.ToString());
		}
	}
}

void FFriendshipperClient::PromptConflicts(TArray<FString>& Files)
{
	FText Message(LOCTEXT("Friendshipper_IsSvcRunning_Msg", "Source control process timed out. Is Friendshipper running?\n\n"
																"If so, and the problem persists contact #ask-git."));

	for (const FString& File : Files)
	{
		Message = FText::Format(LOCTEXT("Friendshipper_Conflict_Format", "{0}\n{1}"), Message, FText::FromString(File));
	}
	
	FMessageDialog::Open(EAppMsgType::Ok, Message);
}

#undef LOCTEXT_NAMESPACE
