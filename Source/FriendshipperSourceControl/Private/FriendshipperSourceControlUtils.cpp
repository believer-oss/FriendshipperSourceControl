// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "FriendshipperSourceControlUtils.h"

#include "FriendshipperMessageLog.h"
#include "FriendshipperSourceControlCommand.h"
#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlProvider.h"
#include "HAL/PlatformProcess.h"

#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "ISourceControlModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ISourceControlModule.h"
#include "FriendshipperSourceControlModule.h"
#include "Logging/MessageLog.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"

#include "PackageTools.h"
#include "FileHelpers.h"
#include "Misc/MessageDialog.h"

#include "Async/Async.h"
#include "UObject/Linker.h"

// Friendshipper
#include "FriendshipperClient.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"

#ifndef GIT_DEBUG_STATUS
#define GIT_DEBUG_STATUS 0
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl"

static constexpr double DEFAULT_TIMEOUT = 3.0;

namespace GitSourceControlConstants
{
/** The maximum number of files we submit in a single Git command */
const int32 MaxFilesPerBatch = 50;
} // namespace GitSourceControlConstants

FFriendshipperScopedTempFile::FFriendshipperScopedTempFile(const FText& InText)
{
	Filename = FPaths::CreateTempFilename(*FPaths::ProjectLogDir(), TEXT("Git-Temp"), TEXT(".txt"));
	if (!FFileHelper::SaveStringToFile(InText.ToString(), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to write to temp file: %s"), *Filename);
	}
}

FFriendshipperScopedTempFile::~FFriendshipperScopedTempFile()
{
	if (FPaths::FileExists(Filename))
	{
		if (!FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Filename))
		{
			UE_LOG(LogSourceControl, Error, TEXT("Failed to delete temp file: %s"), *Filename);
		}
	}
}

const FString& FFriendshipperScopedTempFile::GetFilename() const
{
	return Filename;
}

namespace FriendshipperSourceControlUtils
{
	FString ChangeRepositoryRootIfSubmodule(const TArray<FString>& AbsoluteFilePaths, const FString& PathToRepositoryRoot)
	{
		FString Ret = PathToRepositoryRoot;
		// note this is not going to support operations where selected files are in different repositories

		for (auto& FilePath : AbsoluteFilePaths)
		{
			FString TestPath = FilePath;
			while (!FPaths::IsSamePath(TestPath, PathToRepositoryRoot))
			{
				// Iterating over path directories, looking for .git
				TestPath = FPaths::GetPath(TestPath);

				if (TestPath.IsEmpty())
				{
					// early out if empty directory string to prevent infinite loop
					UE_LOG(LogSourceControl, Error, TEXT("Can't find directory path for file :%s"), *FilePath);
					break;
				}
				
				FString GitTestPath = TestPath + "/.git";
				if (FPaths::FileExists(GitTestPath) || FPaths::DirectoryExists(GitTestPath))
				{
					FString RetNormalized = Ret;
					FPaths::NormalizeDirectoryName(RetNormalized);
					FString PathToRepositoryRootNormalized = PathToRepositoryRoot;
					FPaths::NormalizeDirectoryName(PathToRepositoryRootNormalized);
					if (!FPaths::IsSamePath(RetNormalized, PathToRepositoryRootNormalized) && Ret != GitTestPath)
					{
						UE_LOG(LogSourceControl, Error, TEXT("Selected files belong to different submodules"));
						return PathToRepositoryRoot;
					}
					Ret = TestPath;
					break;
				}
			}
		}
		return Ret;
	}

	FString ChangeRepositoryRootIfSubmodule(const FString& AbsoluteFilePath, const FString& PathToRepositoryRoot)
	{
		TArray<FString> AbsoluteFilePaths = { AbsoluteFilePath };
		return ChangeRepositoryRootIfSubmodule(AbsoluteFilePaths, PathToRepositoryRoot);
	}

// Launch the Git command line process and extract its results & errors
bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors, const int32 ExpectedReturnCode /* = 0 */)
{
	int32 ReturnCode = 0;
	FString FullCommand;
	FString LogableCommand; // short version of the command for logging purpose

	if (!InRepositoryRoot.IsEmpty())
	{
		FString RepositoryRoot = InRepositoryRoot;

		// Detect a "migrate asset" scenario (a "git add" command is applied to files outside the current project)
		if ((InFiles.Num() > 0) && !FPaths::IsRelative(InFiles[0]) && !InFiles[0].StartsWith(InRepositoryRoot))
		{
			// in this case, find the git repository (if any) of the destination Project
			FString DestinationRepositoryRoot;
			if (FindRootDirectory(FPaths::GetPath(InFiles[0]), DestinationRepositoryRoot))
			{
				RepositoryRoot = DestinationRepositoryRoot; // if found use it for the "add" command (else not, to avoid producing one more error in logs)
			}
		}

		// Specify the working copy (the root) of the git repository (before the command itself)
		FullCommand = TEXT("-C \"");
		FullCommand += RepositoryRoot;
		FullCommand += TEXT("\" ");
	}

	// Needed to avoid some cases where git logs on individual files can hang for a long time
	LogableCommand += TEXT("--no-pager ");

	// then the git command itself ("status", "log", "commit"...)
	LogableCommand += InCommand;

	// Append to the command all parameters, and then finally the files
	for (const auto& Parameter : InParameters)
	{
		LogableCommand += TEXT(" ");
		LogableCommand += Parameter;
	}
	for (const auto& File : InFiles)
	{
		LogableCommand += TEXT(" \"");
		LogableCommand += File;
		LogableCommand += TEXT("\"");
	}
	// Also, Git does not have a "--non-interactive" option, as it auto-detects when there are no connected standard input/output streams

	FullCommand += LogableCommand;

#if UE_BUILD_DEBUG
	UE_LOG(LogSourceControl, Log, TEXT("RunCommand: 'git %s'"), *LogableCommand);
#endif
	FString PathToGitOrEnvBinary = InPathToGitBinary;
#if PLATFORM_MAC
	// The Cocoa application does not inherit shell environment variables, so add the path expected to have git-lfs to PATH
	FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	FString GitInstallPath = FPaths::GetPath(InPathToGitBinary);

	TArray<FString> PathArray;
	PathEnv.ParseIntoArray(PathArray, FPlatformMisc::GetPathVarDelimiter());
	bool bHasGitInstallPath = false;
	for (auto Path : PathArray)
	{
		if (GitInstallPath.Equals(Path, ESearchCase::CaseSensitive))
		{
			bHasGitInstallPath = true;
			break;
		}
	}

	if (!bHasGitInstallPath)
	{
		PathToGitOrEnvBinary = FString("/usr/bin/env");
		FullCommand = FString::Printf(TEXT("PATH=\"%s%s%s\" \"%s\" %s"), *GitInstallPath, FPlatformMisc::GetPathVarDelimiter(), *PathEnv, *InPathToGitBinary, *FullCommand);
	}
#endif

	FPlatformProcess::ExecProcess(*PathToGitOrEnvBinary, *FullCommand, &ReturnCode, &OutResults, &OutErrors);

#if UE_BUILD_DEBUG
	// TODO: add a setting to easily enable Verbose logging
	UE_LOG(LogSourceControl, Verbose, TEXT("RunCommand(%s):\n%s"), *InCommand, *OutResults);
	if (ReturnCode != ExpectedReturnCode)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("RunCommand(%s) ReturnCode=%d:\n%s"), *InCommand, ReturnCode, *OutErrors);
	}
#endif

	// Move push/pull progress information from the error stream to the info stream
	if(ReturnCode == ExpectedReturnCode && OutErrors.Len() > 0)
	{
		OutResults.Append(OutErrors);
		OutErrors.Empty();
	}

	return ReturnCode == ExpectedReturnCode;
}

// Basic parsing or results & errors from the Git command line process
static bool RunCommandInternal(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
							   const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult;
	FString Results;
	FString Errors;

	bResult = RunCommandInternalRaw(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, Results, Errors);
	Results.ParseIntoArray(OutResults, TEXT("\n"), true);
	Errors.ParseIntoArray(OutErrorMessages, TEXT("\n"), true);

	return bResult;
}

FString FindGitBinaryPath()
{
#if PLATFORM_WINDOWS
	// 1) First of all, look into standard install directories
	// NOTE using only "git" (or "git.exe") relying on the "PATH" envvar does not always work as expected, depending on the installation:
	// If the PATH is set with "git/cmd" instead of "git/bin",
	// "git.exe" launch "git/cmd/git.exe" that redirect to "git/bin/git.exe" and ExecProcess() is unable to catch its outputs streams.
	// First check the 64-bit program files directory:
	FString GitBinaryPath(TEXT("C:/Program Files/Git/bin/git.exe"));
	bool bFound = CheckGitAvailability(GitBinaryPath);
	if (!bFound)
	{
		// otherwise check the 32-bit program files directory.
		GitBinaryPath = TEXT("C:/Program Files (x86)/Git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
	}
	if (!bFound)
	{
		// else the install dir for the current user: C:\Users\UserName\AppData\Local\Programs\Git\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Programs/Git/cmd/git.exe"), *AppDataLocalPath);
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 2) Else, look for the version of Git bundled with SmartGit "Installer with JRE"
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
		if (!bFound)
		{
			// If git is not found in "git/bin/" subdirectory, try the "bin/" path that was in use before
			GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/bin/git.exe");
			bFound = CheckGitAvailability(GitBinaryPath);
		}
	}

	// 3) Else, look for the local_git provided by SourceTree
	if (!bFound)
	{
		// C:\Users\UserName\AppData\Local\Atlassian\SourceTree\git_local\bin
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Atlassian/SourceTree/git_local/bin/git.exe"), *AppDataLocalPath);
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the PortableGit provided by GitHub Desktop
	if (!bFound)
	{
		// The latest GitHub Desktop adds its binaries into the local appdata directory:
		// C:\Users\UserName\AppData\Local\GitHub\PortableGit_c2ba306e536fdf878271f7fe636a147ff37326ad\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString SearchPath = FString::Printf(TEXT("%s/GitHub/PortableGit_*"), *AppDataLocalPath);
		TArray<FString> PortableGitFolders;
		IFileManager::Get().FindFiles(PortableGitFolders, *SearchPath, false, true);
		if (PortableGitFolders.Num() > 0)
		{
			// FindFiles just returns directory names, so we need to prepend the root path to get the full path.
			GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/cmd/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last PortableGit found
			bFound = CheckGitAvailability(GitBinaryPath);
			if (!bFound)
			{
				// If Portable git is not found in "cmd/" subdirectory, try the "bin/" path that was in use before
				GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/bin/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last
																																	// PortableGit found
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

	// 5) Else, look for the version of Git bundled with Tower
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/fournova/Tower/vendor/Git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

#elif PLATFORM_MAC
	// 1) First of all, look for the version of git provided by official git
	FString GitBinaryPath = TEXT("/usr/local/git/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);

	// 2) Else, look for the version of git provided by Homebrew
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 3) Else, look for the version of git provided by MacPorts
	if (!bFound)
	{
		GitBinaryPath = TEXT("/opt/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the version of git provided by Command Line Tools
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	{
		SCOPED_AUTORELEASE_POOL;
		NSWorkspace* SharedWorkspace = [NSWorkspace sharedWorkspace];

		// 5) Else, look for the version of local_git provided by SmartGit
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.syntevo.smartgit"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 6) Else, look for the version of local_git provided by SourceTree
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.torusknot.SourceTreeNotMAS"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git_local/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 7) Else, look for the version of local_git provided by GitHub Desktop
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.github.GitHubClient"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/app/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 8) Else, look for the version of local_git provided by Tower2
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.fournova.Tower2"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

#else
	FString GitBinaryPath = TEXT("/usr/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);
#endif

	if (bFound)
	{
		FPaths::MakePlatformFilename(GitBinaryPath);
	}
	else
	{
		// If we did not find a path to Git, set it empty
		GitBinaryPath.Empty();
	}

	return GitBinaryPath;
}

bool CheckGitAvailability(const FString& InPathToGitBinary, FFriendshipperVersion* OutVersion)
{
	FString InfoMessages;
	FString ErrorMessages;
	bool bGitAvailable = RunCommandInternalRaw(TEXT("version"), InPathToGitBinary, FString(), FFriendshipperSourceControlModule::GetEmptyStringArray(), FFriendshipperSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bGitAvailable)
	{
		if (!InfoMessages.StartsWith("git version"))
		{
			bGitAvailable = false;
		}
		else if (OutVersion)
		{
			ParseGitVersion(InfoMessages, OutVersion);
		}
	}

	return bGitAvailable;
}

void ParseGitVersion(const FString& InVersionString, FFriendshipperVersion* OutVersion)
{
#if UE_BUILD_DEBUG
	// Parse "git version 2.31.1.vfs.0.3" into the string "2.31.1.vfs.0.3"
	const FString& TokenVersionStringPtr = InVersionString.RightChop(12);
	if (!TokenVersionStringPtr.IsEmpty())
	{
		// Parse the version into its numerical components
		TArray<FString> ParsedVersionString;
		TokenVersionStringPtr.ParseIntoArray(ParsedVersionString, TEXT("."));
		const int Num = ParsedVersionString.Num();
		if (Num >= 3)
		{
			if (ParsedVersionString[0].IsNumeric() && ParsedVersionString[1].IsNumeric() && ParsedVersionString[2].IsNumeric())
			{
				OutVersion->Major = FCString::Atoi(*ParsedVersionString[0]);
				OutVersion->Minor = FCString::Atoi(*ParsedVersionString[1]);
				OutVersion->Patch = FCString::Atoi(*ParsedVersionString[2]);
				if (Num >= 5)
				{
					// If labeled with fork
					if (!ParsedVersionString[3].IsNumeric())
					{
						OutVersion->Fork = ParsedVersionString[3];
						OutVersion->bIsFork = true;
						OutVersion->ForkMajor = FCString::Atoi(*ParsedVersionString[4]);
						if (Num >= 6)
						{
							OutVersion->ForkMinor = FCString::Atoi(*ParsedVersionString[5]);
							if (Num >= 7)
							{
								OutVersion->ForkPatch = FCString::Atoi(*ParsedVersionString[6]);
							}
						}
					}
				}
				if (OutVersion->bIsFork)
				{
					UE_LOG(LogSourceControl, Log, TEXT("Git version %d.%d.%d.%s.%d.%d.%d"), OutVersion->Major, OutVersion->Minor, OutVersion->Patch, *OutVersion->Fork, OutVersion->ForkMajor, OutVersion->ForkMinor, OutVersion->ForkPatch);
				}
				else
				{
					UE_LOG(LogSourceControl, Log, TEXT("Git version %d.%d.%d"), OutVersion->Major, OutVersion->Minor, OutVersion->Patch);
				}
			}
		}
	}
#endif
}

// Find the root of the Git repository, looking from the provided path and upward in its parent directories.
bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot)
{
	OutRepositoryRoot = InPath;

	auto TrimTrailing = [](FString& Str, const TCHAR Char) {
		int32 Len = Str.Len();
		while (Len && Str[Len - 1] == Char)
		{
			Str = Str.LeftChop(1);
			Len = Str.Len();
		}
	};

	TrimTrailing(OutRepositoryRoot, '\\');
	TrimTrailing(OutRepositoryRoot, '/');

	bool bFound = false;
	FString PathToGitSubdirectory;
	while (!bFound && !OutRepositoryRoot.IsEmpty())
	{
		// Look for the ".git" subdirectory (or file) present at the root of every Git repository
		PathToGitSubdirectory = OutRepositoryRoot / TEXT(".git");
		bFound = IFileManager::Get().DirectoryExists(*PathToGitSubdirectory) || IFileManager::Get().FileExists(*PathToGitSubdirectory);
		if (!bFound)
		{
			int32 LastSlashIndex;
			if (OutRepositoryRoot.FindLastChar('/', LastSlashIndex))
			{
				OutRepositoryRoot = OutRepositoryRoot.Left(LastSlashIndex);
			}
			else
			{
				OutRepositoryRoot.Empty();
			}
		}
	}
	if (!bFound)
	{
		OutRepositoryRoot = InPath; // If not found, return the provided dir as best possible root.
	}
	return bFound;
}

void GetUserConfig(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutUserName, FString& OutUserEmail)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("user.name"));
	bResults = RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, FFriendshipperSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserName = InfoMessages[0];
	}
	else
	{
		OutUserName = TEXT("");
	}

	Parameters.Reset(1);
	Parameters.Add(TEXT("user.email"));
	InfoMessages.Reset();
	bResults &= RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, FFriendshipperSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserEmail = InfoMessages[0];
	}
	else
	{
		OutUserEmail = TEXT("");
	}
}

bool GetBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
	const FFriendshipperSourceControlModule* GitSourceControl = FFriendshipperSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return false;
	}
	const FFriendshipperSourceControlProvider& Provider = GitSourceControl->GetProvider();
	if (!Provider.GetBranchName().IsEmpty())
	{
		OutBranchName = Provider.GetBranchName();
		return true;
	}
	
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--short"));
	Parameters.Add(TEXT("--quiet")); // no error message while in detached HEAD
	Parameters.Add(TEXT("HEAD"));
	bResults = RunCommand(TEXT("symbolic-ref"), InPathToGitBinary, InRepositoryRoot, Parameters, FFriendshipperSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutBranchName = InfoMessages[0];
	}
	else
	{
		Parameters.Reset(2);
		Parameters.Add(TEXT("-1"));
		Parameters.Add(TEXT("--format=\"%h\"")); // no error message while in detached HEAD
		bResults = RunCommand(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, FFriendshipperSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
		if (bResults && InfoMessages.Num() > 0)
		{
			OutBranchName = "HEAD detached at ";
			OutBranchName += InfoMessages[0];
		}
		else
		{
			bResults = false;
		}
	}

	return bResults;
}

bool GetRemoteBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
	const FFriendshipperSourceControlModule* GitSourceControl = FFriendshipperSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return false;
	}
	const FFriendshipperSourceControlProvider& Provider = GitSourceControl->GetProvider();
	if (!Provider.GetRemoteBranchName().IsEmpty())
	{
		OutBranchName = Provider.GetRemoteBranchName();
		return true;
	}

	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--abbrev-ref"));
	Parameters.Add(TEXT("--symbolic-full-name"));
	Parameters.Add(TEXT("@{u}"));
	bool bResults = RunCommand(TEXT("rev-parse"), InPathToGitBinary, InRepositoryRoot, Parameters, FFriendshipperSourceControlModule::GetEmptyStringArray(),
								InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutBranchName = InfoMessages[0];
	}
	if (!bResults)
	{
		static bool bRunOnce = true;
		if (bRunOnce)
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Upstream branch not found for the current branch, skipping current branch for remote check. Please push a remote branch."));
			bRunOnce = false;
		}
	}
	return bResults;
}

bool GetRemoteBranchesWildcard(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& PatternMatch, TArray<FString>& OutBranchNames)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--remotes"));
	Parameters.Add(TEXT("--list"));
	bool bResults = RunCommand(TEXT("branch"), InPathToGitBinary, InRepositoryRoot, Parameters, { PatternMatch },
								InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutBranchNames = InfoMessages;
	}
	if (!bResults)
	{
		static bool bRunOnce = true;
		if (bRunOnce)
		{
			UE_LOG(LogSourceControl, Warning, TEXT("No remote branches matching pattern \"%s\" were found."), *PatternMatch);
			bRunOnce = false;
		}
	}
	return bResults;	
}
	
bool GetCommitInfo(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutCommitId, FString& OutCommitSummary)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("-1"));
	Parameters.Add(TEXT("--format=\"%H %s\""));
	bResults = RunCommandInternal(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, FFriendshipperSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutCommitId = InfoMessages[0].Left(40);
		OutCommitSummary = InfoMessages[0].RightChop(41);
	}

	return bResults;
}

bool GetRemoteUrl(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutRemoteUrl)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("get-url"));
	Parameters.Add(TEXT("origin"));
	const bool bResults = RunCommandInternal(TEXT("remote"), InPathToGitBinary, InRepositoryRoot, Parameters, FFriendshipperSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutRemoteUrl = InfoMessages[0];
	}

	return bResults;
}

bool RunCommand(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
				const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult = true;

	if (InFiles.Num() > GitSourceControlConstants::MaxFilesPerBatch)
	{
		// Batch files up so we dont exceed command-line limits
		int32 FileCount = 0;
		while (FileCount < InFiles.Num())
		{
			TArray<FString> FilesInBatch;
			for (int32 FileIndex = 0; FileCount < InFiles.Num() && FileIndex < GitSourceControlConstants::MaxFilesPerBatch; FileIndex++, FileCount++)
			{
				FilesInBatch.Add(InFiles[FileCount]);
			}

			TArray<FString> BatchResults;
			TArray<FString> BatchErrors;
			bResult &= RunCommandInternal(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, FilesInBatch, BatchResults, BatchErrors);
			OutResults += BatchResults;
			OutErrorMessages += BatchErrors;
		}
	}
	else
	{
		bResult = RunCommandInternal(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
	}

	return bResult;
}

#ifndef GIT_USE_CUSTOM_LFS
#define GIT_USE_CUSTOM_LFS 1
#endif

bool RunLFSCommand(const FString& InCommand, const FString& InRepositoryRoot, const FString& GitBinaryFallback, const TArray<FString>& InParameters, const TArray<FString>& InFiles,
				   TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	FString Command = InCommand;
#if GIT_USE_CUSTOM_LFS
	FString BaseDir = IPluginManager::Get().FindPlugin("FriendshipperSourceControl")->GetBaseDir();
#if PLATFORM_WINDOWS
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs.exe"), *BaseDir);
#elif PLATFORM_MAC
#if ENGINE_MAJOR_VERSION >= 5
#if PLATFORM_MAC_ARM64
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs-mac-arm64"), *BaseDir);
#else
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs-mac-amd64"), *BaseDir);
#endif
#else
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs-mac-amd64"), *BaseDir);
#endif
#elif PLATFORM_LINUX
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs"), *BaseDir);
#else
	ensureMsgf(false, TEXT("Unhandled platform for LFS binary!"));
	const FString& LFSLockBinary = GitBinaryFallback;
	Command = TEXT("lfs ") + Command;
#endif
#else
	const FString& LFSLockBinary = GitBinaryFallback;
	Command = TEXT("lfs ") + Command;
#endif

	return FriendshipperSourceControlUtils::RunCommand(Command, LFSLockBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
}

/**
 * Parse informations on a file locked with Git LFS
 *
 * Examples output of "git lfs locks":
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset    SRombauts       ID:891
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset                    ID:891
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset    ID:891
 */
class FFriendshipperLfsLocksParser
{
public:
	FFriendshipperLfsLocksParser(const FString& InRepositoryRoot, const FString& InStatus, const bool bAbsolutePaths = true)
	{
		TArray<FString> Informations;
		InStatus.ParseIntoArray(Informations, TEXT("\t"), true);
		
		if (Informations.Num() >= 2)
		{
			Informations[0].TrimEndInline(); // Trim whitespace from the end of the filename
			Informations[1].TrimEndInline(); // Trim whitespace from the end of the username
			if (bAbsolutePaths)
				LocalFilename = FPaths::ConvertRelativePathToFull(InRepositoryRoot, Informations[0]);
			else
				LocalFilename = Informations[0];
			// Filename ID (or we expect it to be the username, but it's empty, or is the ID, we have to assume it's the current user)
			if (Informations.Num() == 2 || Informations[1].IsEmpty() || Informations[1].StartsWith(TEXT("ID:")))
			{
				// TODO: thread safety
				LockUser = FFriendshipperSourceControlModule::Get().GetProvider().GetLockUser();
			}
			// Filename Username ID
			else
			{
				LockUser = MoveTemp(Informations[1]);
			}
		}
	}

	// Filename on disk
	FString LocalFilename;
	// Name of user who has file locked
	FString LockUser;
};

/**
 * @brief Extract the relative filename from a Git status result.
 *
 * Examples of status results:
M  Content/Textures/T_Perlin_Noise_M.uasset
R  Content/Textures/T_Perlin_Noise_M.uasset -> Content/Textures/T_Perlin_Noise_M2.uasset
?? Content/Materials/M_Basic_Wall.uasset
!! BasicCode.sln
 *
 * @param[in] InResult One line of status
 * @return Relative filename extracted from the line of status
 *
 * @see FFriendshipperStatusFileMatcher and StateFromGitStatus()
 */
static FString FilenameFromGitStatus(const FString& InResult)
{
	int32 RenameIndex;
	if (InResult.FindLastChar('>', RenameIndex))
	{
		// Extract only the second part of a rename "from -> to"
		return InResult.RightChop(RenameIndex + 2);
	}
	else
	{
		// Extract the relative filename from the Git status result (after the 2 letters status and 1 space)
		return InResult.RightChop(3);
	}
}

/** Match the relative filename of a Git status result with a provided absolute filename */
class FFriendshipperStatusFileMatcher
{
public:
	FFriendshipperStatusFileMatcher(const FString& InAbsoluteFilename) : AbsoluteFilename(InAbsoluteFilename)
	{}

	bool operator()(const FString& InResult) const
	{
		return AbsoluteFilename.Contains(FilenameFromGitStatus(InResult));
	}

private:
	const FString& AbsoluteFilename;
};

void ReloadPackages(TArray<UPackage*>& InPackagesToReload)
{
	// UE-COPY: ContentBrowserUtils::SyncPathsFromSourceControl()
	// Syncing may have deleted some packages, so we need to unload those rather than re-load them...
	TArray<UPackage*> PackagesToUnload;
	InPackagesToReload.RemoveAll([&](UPackage* InPackage) -> bool {
		const FString PackageExtension = InPackage->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(InPackage->GetName(), PackageExtension);
		if (!FPaths::FileExists(PackageFilename))
		{
			PackagesToUnload.Emplace(InPackage);
			return true; // remove package
		}
		return false; // keep package
	});

	// Hot-reload the new packages...
	UPackageTools::ReloadPackages(InPackagesToReload);

	// Unload any deleted packages...
	UPackageTools::UnloadPackages(PackagesToUnload);
}

/// Convert filename relative to the repository root to absolute path (inplace)
void AbsoluteFilenames(const FString& InRepositoryRoot, TArray<FString>& InFileNames)
{
	for (auto& FileName : InFileNames)
	{
		FileName = FPaths::ConvertRelativePathToFull(InRepositoryRoot, FileName);
	}
}

/** Run a 'git ls-files' command to get all files tracked by Git recursively in a directory.
 *
 * Called in case of a "directory status" (no file listed in the command) when using the "Submit to Revision Control" menu.
 */
bool ListFilesInDirectoryRecurse(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InDirectory, TArray<FString>& OutFiles)
{
	TArray<FString> ErrorMessages;
	TArray<FString> Directory;
	Directory.Add(InDirectory);
	const bool bResult = RunCommandInternal(TEXT("ls-files --cached --others --exclude-standard"), InPathToGitBinary, InRepositoryRoot, TArray<FString>(), Directory, OutFiles, ErrorMessages);
	AbsoluteFilenames(InRepositoryRoot, OutFiles);
	return bResult;
}

// Called in case of a refresh of status on a list of assets in the Content Browser, periodic update, or user manually refreshed.
static void ParseFileStatusResult(const TSet<FString>& InFiles, const FRepoStatus& InRepoStatus, TMap<FString, FFriendshipperSourceControlState>& OutStates)
{
	FFriendshipperSourceControlModule* GitSourceControl = FFriendshipperSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return;
	}
	FFriendshipperSourceControlProvider& Provider = GitSourceControl->GetProvider();
	const FString& LfsUserName = Provider.GetLockUser();

	TMap<FString, FString> LockedFiles;

	FString Result;

	// Iterate on all files explicitly listed in the command
	for (const auto& File : InFiles)
	{
		FFriendshipperSourceControlState FileState(File);
		FileState.State.FileState = EFileState::Unset;
		FileState.State.TreeState = ETreeState::Unset;
		FileState.State.LockState = ELockState::Unset;

		bool bFound = false;
		for (const FStatusFileState& StatusState : InRepoStatus.ModifiedFiles)
		{
			if (File.EndsWith(StatusState.Path))
			{
				FileState.State.TreeState = ETreeState::Working;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			for (const FStatusFileState& StatusState : InRepoStatus.UntrackedFiles)
			{
				if (File.EndsWith(StatusState.Path))
				{
					FileState.State.TreeState = ETreeState::Untracked;
					bFound = true;
					break;
				}
			}
		}

		const bool bFileExists = FPaths::FileExists(File);
		if (bFound)
		{
			if (!bFileExists)
			{
				FileState.State.FileState = EFileState::Deleted;
			}
		}
		else
		{
			FileState.State.FileState = EFileState::Unknown;
			// File not found in status
			if (bFileExists)
			{
				// usually means the file is unchanged,
				FileState.State.TreeState = ETreeState::Unmodified;
			}
			else
			{
				// but also the case for newly created content: there is no file on disk until the content is saved for the first time
				FileState.State.TreeState = ETreeState::NotInRepo;
			}
		}

		if (IsFileLFSLockable(File))
		{
			if (LockedFiles.IsEmpty())
			{
				const FString ProjectDir = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*FPaths::ProjectDir());

				LockedFiles.Reserve(InRepoStatus.LocksOurs.Num() + InRepoStatus.LocksTheirs.Num());
				for (const FLfsLock& Lock : InRepoStatus.LocksOurs)
				{
					FString AbsolutePath = FPaths::ConvertRelativePathToFull(ProjectDir, Lock.Path);
					LockedFiles.Add(AbsolutePath, Lock.Owner.Name);
				}
				for (const FLfsLock& Lock : InRepoStatus.LocksTheirs)
				{
					FString AbsolutePath = FPaths::ConvertRelativePathToFull(ProjectDir, Lock.Path);
					LockedFiles.Add(AbsolutePath, Lock.Owner.Name);
				}
			}

			if (const FString* LockUser = LockedFiles.Find(File))
			{
				FileState.State.LockUser = *LockUser;
				if (LfsUserName == FileState.State.LockUser)
				{
					FileState.State.LockState = ELockState::Locked;
				}
				else
				{
					FileState.State.LockState = ELockState::LockedOther;
				}
			}
			else
			{
				FileState.State.LockState = ELockState::NotLocked;
			}
		}
		else
		{
			FileState.State.LockState = ELockState::Unlockable;
		}

		OutStates.Add(File, MoveTemp(FileState));
	}
}

void CheckRemote(const FString& InRepositoryRoot, const FRepoStatus& InStatus, TMap<FString, FFriendshipperSourceControlState>& OutStates)
{
	// We can obtain a list of files that were modified between our remote branches and HEAD. Assumes that fetch has been run to get accurate info.
	for (const FString& Modified : InStatus.ModifiedUpstream)
	{
		const FString& AbsolutePath = FPaths::Combine(InRepositoryRoot, Modified);
		if (FFriendshipperSourceControlState* FileState = OutStates.Find(AbsolutePath))
		{
			FileState->State.RemoteState = ERemoteState::NotAtHead;
			FileState->State.HeadBranch = InStatus.RemoteBranch;
		}
	}
}

void GetLockedFiles(const TArray<FString>& InFiles, TArray<FString>& OutFiles)
{
	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	FFriendshipperSourceControlProvider& Provider = GitSourceControl.GetProvider();

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(InFiles, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		const auto& GitState = StaticCastSharedRef<FFriendshipperSourceControlState>(State);
		if (GitState->State.LockState == ELockState::Locked)
		{
			OutFiles.Add(GitState->GetFilename());
		}
	}
}

// Run a batch of Git "status" command to update status of given files and/or directories.
bool RunUpdateStatus(const FString& InRepositoryRoot, const TArray<FString>& InFiles, EForceStatusRefresh FetchRemote, TMap<FString, FFriendshipperSourceControlState>& OutStates)
{
	// Remove files that aren't in the repository
	const TArray<FString>& RepoFiles = InFiles.FilterByPredicate([InRepositoryRoot](const FString& File) { return File.StartsWith(InRepositoryRoot); });

	if (!RepoFiles.Num())
	{
		return false;
	}

	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	FFriendshipperSourceControlProvider& Provider = GitSourceControl.GetProvider();
	FFriendshipperClient& Client = Provider.GetFriendshipperClient();

	const FString ProjectDir = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*FPaths::ProjectDir());
	const TSet<FString> AllAbsolutePaths = Provider.GetAllPathsAbsolute();

	TSet<FString> AbsolutePaths;
	for (const FString& Filename : InFiles)
	{
		FString AbsolutePath = FPaths::ConvertRelativePathToFull(ProjectDir, Filename);
		AbsolutePaths.Add(MoveTemp(AbsolutePath));
	}

	FRepoStatus RepoStatus;
	const bool bIsStatusValid = Client.GetStatus(FetchRemote, RepoStatus);
	if (bIsStatusValid)
	{
		ParseFileStatusResult(AbsolutePaths, RepoStatus, OutStates);
		CheckRemote(InRepositoryRoot, RepoStatus, OutStates);
	}

	return bIsStatusValid;
}

TMap<const FString, FFriendshipperState> FriendshipperStatesFromRepoStatus(const FString& InRepositoryRoot, const TSet<FString>& AllTrackedFilesAbsolutePaths, const FRepoStatus& RepoStatus)
{
	TMap<FString, FFriendshipperSourceControlState> SCCStates;

	ParseFileStatusResult(AllTrackedFilesAbsolutePaths, RepoStatus, SCCStates);
	CheckRemote(InRepositoryRoot, RepoStatus, SCCStates);

	TMap<const FString, FFriendshipperState> States;
	FriendshipperSourceControlUtils::CollectNewStates(SCCStates, States);

	return States;
}

// Run a Git `cat-file --filters` command to dump the binary content of a revision into a file.
bool RunDumpToFile(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InParameter, const FString& InDumpFileName)
{
	int32 ReturnCode = -1;
	FString FullCommand;

	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();

	if (!InRepositoryRoot.IsEmpty())
	{
		// Specify the working copy (the root) of the git repository (before the command itself)
		FullCommand = TEXT("-C \"");
		FullCommand += InRepositoryRoot;
		FullCommand += TEXT("\" ");
	}

	// then the git command itself
	// Newer versions (2.9.3.windows.2) support smudge/clean filters used by Git LFS, git-fat, git-annex, etc
	FullCommand += TEXT("cat-file --filters ");

	// Append to the command the parameter
	FullCommand += InParameter;

	const bool bLaunchDetached = false;
	const bool bLaunchHidden = true;
	const bool bLaunchReallyHidden = bLaunchHidden;

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;

	verify(FPlatformProcess::CreatePipe(PipeRead, PipeWrite));

	UE_LOG(LogSourceControl, Log, TEXT("RunDumpToFile: 'git %s'"), *FullCommand);

    FString PathToGitOrEnvBinary = InPathToGitBinary;
    #if PLATFORM_MAC
        // The Cocoa application does not inherit shell environment variables, so add the path expected to have git-lfs to PATH
        FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
        FString GitInstallPath = FPaths::GetPath(InPathToGitBinary);

        TArray<FString> PathArray;
        PathEnv.ParseIntoArray(PathArray, FPlatformMisc::GetPathVarDelimiter());
        bool bHasGitInstallPath = false;
        for (auto Path : PathArray)
        {
            if (GitInstallPath.Equals(Path, ESearchCase::CaseSensitive))
            {
                bHasGitInstallPath = true;
                break;
            }
        }

        if (!bHasGitInstallPath)
        {
            PathToGitOrEnvBinary = FString("/usr/bin/env");
            FullCommand = FString::Printf(TEXT("PATH=\"%s%s%s\" \"%s\" %s"), *GitInstallPath, FPlatformMisc::GetPathVarDelimiter(), *PathEnv, *InPathToGitBinary, *FullCommand);
        }
    #endif

	FProcHandle ProcessHandle = FPlatformProcess::CreateProc(*PathToGitOrEnvBinary, *FullCommand, bLaunchDetached, bLaunchHidden, bLaunchReallyHidden, nullptr, 0, *InRepositoryRoot, PipeWrite);
	if(ProcessHandle.IsValid())
	{
		FPlatformProcess::Sleep(0.01f);

		TArray<uint8> BinaryFileContent;
		bool bRemovedLFSMessage = false;
		while (FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			TArray<uint8> BinaryData;
			FPlatformProcess::ReadPipeToArray(PipeRead, BinaryData);
			if (BinaryData.Num() > 0)
			{
				// @todo: this is hacky!
				if (BinaryData[0] == 68) // Check for D in "Downloading"
				{
					if (BinaryData[BinaryData.Num() - 1] == 10) // Check for newline
					{
						BinaryData.Reset();
						bRemovedLFSMessage = true;
					}
				}
				else
				{
					BinaryFileContent.Append(MoveTemp(BinaryData));
				}
			}
		}
		TArray<uint8> BinaryData;
		FPlatformProcess::ReadPipeToArray(PipeRead, BinaryData);
		if (BinaryData.Num() > 0)
		{
			// @todo: this is hacky!
			if (!bRemovedLFSMessage && BinaryData[0] == 68) // Check for D in "Downloading"
			{
				int32 NewLineIndex = 0;
				for (int32 Index = 0; Index < BinaryData.Num(); Index++)
				{
					if (BinaryData[Index] == 10) // Check for newline
					{
						NewLineIndex = Index;
						break;
					}
				}
				if (NewLineIndex > 0)
				{
					BinaryData.RemoveAt(0, NewLineIndex + 1);
				}
			}
			else
			{
				BinaryFileContent.Append(MoveTemp(BinaryData));
			}
		}

		FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
		if (ReturnCode == 0)
		{
			// Save buffer into temp file
			if (FFileHelper::SaveArrayToFile(BinaryFileContent, *InDumpFileName))
			{
				UE_LOG(LogSourceControl, Log, TEXT("Wrote '%s' (%do)"), *InDumpFileName, BinaryFileContent.Num());
			}
			else
			{
				UE_LOG(LogSourceControl, Error, TEXT("Could not write %s"), *InDumpFileName);
				ReturnCode = -1;
			}
		}
		else
		{
			UE_LOG(LogSourceControl, Error, TEXT("DumpToFile: ReturnCode=%d"), ReturnCode);
		}

		FPlatformProcess::CloseProc(ProcessHandle);
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to launch 'git cat-file'"));
	}

	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	return (ReturnCode == 0);
}

/**
 * Translate file actions from the given Git log --name-status command to keywords used by the Editor UI.
 *
 * @see https://www.kernel.org/pub/software/scm/git/docs/git-log.html
 * ' ' = unmodified
 * 'M' = modified
 * 'A' = added
 * 'D' = deleted
 * 'R' = renamed
 * 'C' = copied
 * 'T' = type changed
 * 'U' = updated but unmerged
 * 'X' = unknown
 * 'B' = broken pairing
 *
 * @see SHistoryRevisionListRowContent::GenerateWidgetForColumn(): "add", "edit", "delete", "branch" and "integrate" (everything else is taken like "edit")
 */
static FString LogStatusToString(TCHAR InStatus)
{
	switch (InStatus)
	{
		case TEXT(' '):
			return FString("unmodified");
		case TEXT('M'):
			return FString("modified");
		case TEXT('A'): // added: keyword "add" to display a specific icon instead of the default "edit" action one
			return FString("add");
		case TEXT('D'): // deleted: keyword "delete" to display a specific icon instead of the default "edit" action one
			return FString("delete");
		case TEXT('R'): // renamed keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('C'): // copied keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('T'):
			return FString("type changed");
		case TEXT('U'):
			return FString("unmerged");
		case TEXT('X'):
			return FString("unknown");
		case TEXT('B'):
			return FString("broked pairing");
	}

	return FString();
}

/**
 * Parse the array of strings results of a 'git log' command
 *
 * Example git log results:
commit 97a4e7626681895e073aaefd68b8ac087db81b0b
Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
Date:   2014-2015-05-15 21:32:27 +0200

	Another commit used to test History

	 - with many lines
	 - some <xml>
	 - and strange characteres $*+

M	Content/Blueprints/Blueprint_CeilingLight.uasset
R100	Content/Textures/T_Concrete_Poured_D.uasset Content/Textures/T_Concrete_Poured_D2.uasset

commit 355f0df26ebd3888adbb558fd42bb8bd3e565000
Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
Date:   2014-2015-05-12 11:28:14 +0200

	Testing git status, edit, and revert

A	Content/Blueprints/Blueprint_CeilingLight.uasset
C099	Content/Textures/T_Concrete_Poured_N.uasset Content/Textures/T_Concrete_Poured_N2.uasset
*/
static void ParseLogResults(const TArray<FString>& InResults, TGitSourceControlHistory& OutHistory)
{
	TSharedRef<FFriendshipperSourceControlRevision, ESPMode::ThreadSafe> SourceControlRevision = MakeShareable(new FFriendshipperSourceControlRevision);
	for (const auto& Result : InResults)
	{
		if (Result.StartsWith(TEXT("commit "))) // Start of a new commit
		{
			// End of the previous commit
			if (SourceControlRevision->RevisionNumber != 0)
			{
				OutHistory.Add(MoveTemp(SourceControlRevision));

				SourceControlRevision = MakeShareable(new FFriendshipperSourceControlRevision);
			}
			SourceControlRevision->CommitId = Result.RightChop(7); // Full commit SHA1 hexadecimal string
			SourceControlRevision->ShortCommitId = SourceControlRevision->CommitId.Left(8); // Short revision ; first 8 hex characters (max that can hold a 32
																							// bit integer)
			SourceControlRevision->CommitIdNumber = FParse::HexNumber(*SourceControlRevision->ShortCommitId);
			SourceControlRevision->RevisionNumber = -1; // RevisionNumber will be set at the end, based off the index in the History
		}
		else if (Result.StartsWith(TEXT("Author: "))) // Author name & email
		{
			// Remove the 'email' part of the UserName
			FString UserNameEmail = Result.RightChop(8);
			int32 EmailIndex = 0;
			if (UserNameEmail.FindLastChar('<', EmailIndex))
			{
				SourceControlRevision->UserName = UserNameEmail.Left(EmailIndex - 1);
			}
		}
		else if (Result.StartsWith(TEXT("Date:   "))) // Commit date
		{
			FString Date = Result.RightChop(8);
			SourceControlRevision->Date = FDateTime::FromUnixTimestamp(FCString::Atoi(*Date));
		}
		//	else if(Result.IsEmpty()) // empty line before/after commit message has already been taken care by FString::ParseIntoArray()
		else if (Result.StartsWith(TEXT("    "))) // Multi-lines commit message
		{
			SourceControlRevision->Description += Result.RightChop(4);
			SourceControlRevision->Description += TEXT("\n");
		}
		else // Name of the file, starting with an uppercase status letter ("A"/"M"...)
		{
			const TCHAR Status = Result[0];
			SourceControlRevision->Action = LogStatusToString(Status); // Readable action string ("Added", Modified"...) instead of "A"/"M"...
			// Take care of special case for Renamed/Copied file: extract the second filename after second tabulation
			int32 IdxTab;
			if (Result.FindLastChar('\t', IdxTab))
			{
				SourceControlRevision->Filename = Result.RightChop(IdxTab + 1); // relative filename
			}
		}
	}
	// End of the last commit
	if (SourceControlRevision->RevisionNumber != 0)
	{
		OutHistory.Add(MoveTemp(SourceControlRevision));
	}

	// Then set the revision number of each Revision based on its index (reverse order since the log starts with the most recent change)
	for (int32 RevisionIndex = 0; RevisionIndex < OutHistory.Num(); RevisionIndex++)
	{
		const auto& SourceControlRevisionItem = OutHistory[RevisionIndex];
		SourceControlRevisionItem->RevisionNumber = OutHistory.Num() - RevisionIndex;

		// Special case of a move ("branch" in Perforce term): point to the previous change (so the next one in the order of the log)
		if ((SourceControlRevisionItem->Action == "branch") && (RevisionIndex < OutHistory.Num() - 1))
		{
			SourceControlRevisionItem->BranchSource = OutHistory[RevisionIndex + 1];
		}
	}
}

// Run a Git "log" command and parse it.
bool RunGetHistory(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, bool bMergeConflict,
				   TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory)
{
	bool bResults;

	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	FFriendshipperSourceControlProvider& Provider = GitSourceControl.GetProvider();
	FFriendshipperClient& Client = Provider.GetFriendshipperClient();

	FFileHistoryResponse FileHistory;
	bResults = Client.GetFileHistory(InFile, FileHistory);

	// convert to the format expected by the Editor UI
	for (const auto& Revision : FileHistory.Revisions)
	{
		TSharedRef<FFriendshipperSourceControlRevision, ESPMode::ThreadSafe> SourceControlRevision = MakeShareable(new FFriendshipperSourceControlRevision);
		SourceControlRevision->CommitId = Revision.CommitId;
		SourceControlRevision->ShortCommitId = Revision.ShortCommitId;
		SourceControlRevision->CommitIdNumber = Revision.CommitIdNumber;
		SourceControlRevision->RevisionNumber = Revision.RevisionNumber;
		SourceControlRevision->UserName = Revision.UserName;
		SourceControlRevision->Date = Revision.Date;
		SourceControlRevision->Description = Revision.Description;
		SourceControlRevision->Action = Revision.Action;
		SourceControlRevision->Filename = Revision.Filename;
		SourceControlRevision->FileSize = Revision.FileSize;
		SourceControlRevision->PathToRepoRoot = InRepositoryRoot;

		OutHistory.Add(MoveTemp(SourceControlRevision));
	}

	for (int32 RevisionIndex = 0; RevisionIndex < OutHistory.Num(); RevisionIndex++)
	{
		const auto& SourceControlRevisionItem = OutHistory[RevisionIndex];
		// Special case of a move ("branch" in Perforce term): point to the previous change (so the next one in the order of the log)
		if ((SourceControlRevisionItem->Action == "branch") && (RevisionIndex + 1) < OutHistory.Num())
		{
			SourceControlRevisionItem->BranchSource = OutHistory[RevisionIndex + 1];
		}
	}

	return bResults;
}

TArray<FString> RelativeFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> RelativeFiles;
	FString RelativeTo = InRelativeTo;

	// Ensure that the path ends w/ '/'
	if ((RelativeTo.Len() > 0) && (RelativeTo.EndsWith(TEXT("/"), ESearchCase::CaseSensitive) == false) &&
		(RelativeTo.EndsWith(TEXT("\\"), ESearchCase::CaseSensitive) == false))
	{
		RelativeTo += TEXT("/");
	}
	for (FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		if (FPaths::MakePathRelativeTo(FileName, *RelativeTo))
		{
			RelativeFiles.Add(FileName);
		}
	}

	return RelativeFiles;
}

TArray<FString> AbsoluteFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> AbsFiles;

	for(FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		AbsFiles.Add(FPaths::Combine(InRelativeTo, FileName));
	}

	return AbsFiles;
}

bool UpdateCachedStates(const TMap<const FString, FFriendshipperState>& InResults)
{
	FFriendshipperSourceControlModule* GitSourceControl = FFriendshipperSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return false;
	}
	FFriendshipperSourceControlProvider& Provider = GitSourceControl->GetProvider();
	return Provider.UpdateCachedStates(InResults);
}

void CollectNewStates(const TMap<FString, FFriendshipperSourceControlState>& InStates, TMap<const FString, FFriendshipperState>& OutResults)
{
	for (const auto& InState : InStates)
	{
		OutResults.Add(InState.Key, InState.Value.State);
	}
}

void CollectNewStates(const TArray<FString>& InFiles, TMap<const FString, FFriendshipperState>& OutResults, EFileState::Type FileState, ETreeState::Type TreeState, ELockState::Type LockState, ERemoteState::Type RemoteState)
{
	FFriendshipperState NewState;
	NewState.FileState = FileState;
	NewState.TreeState = TreeState;
	NewState.LockState = LockState;
	NewState.RemoteState = RemoteState;

	for (const auto& File : InFiles)
	{
		FFriendshipperState& State = OutResults.FindOrAdd(File, NewState);
		if (NewState.FileState != EFileState::Unset)
		{
			State.FileState = NewState.FileState;
		}
		if (NewState.TreeState != ETreeState::Unset)
		{
			State.TreeState = NewState.TreeState;
		}
		if (NewState.LockState != ELockState::Unset)
		{
			State.LockState = NewState.LockState;
		}
		if (NewState.RemoteState != ERemoteState::Unset)
		{
			State.RemoteState = NewState.RemoteState;
		}
	}
}

/**
 * Helper struct for RemoveRedundantErrors()
 */
struct FRemoveRedundantErrors
{
	FRemoveRedundantErrors(const FString& InFilter) : Filter(InFilter)
	{}

	bool operator()(const FString& String) const
	{
		if (String.Contains(Filter))
		{
			return true;
		}

		return false;
	}

	/** The filter string we try to identify in the reported error */
	FString Filter;
};

void RemoveRedundantErrors(FFriendshipperSourceControlCommand& InCommand, const FString& InFilter)
{
	bool bFoundRedundantError = false;
	for (auto Iter(InCommand.ResultInfo.ErrorMessages.CreateConstIterator()); Iter; Iter++)
	{
		if (Iter->Contains(InFilter))
		{
			InCommand.ResultInfo.InfoMessages.Add(*Iter);
			bFoundRedundantError = true;
		}
	}

	InCommand.ResultInfo.ErrorMessages.RemoveAll(FRemoveRedundantErrors(InFilter));

	// if we have no error messages now, assume success!
	if (bFoundRedundantError && InCommand.ResultInfo.ErrorMessages.Num() == 0 && !InCommand.bCommandSuccessful)
	{
		InCommand.bCommandSuccessful = true;
	}
}

static TArray<FString> LockableTypes;

bool IsFileLFSLockable(const FString& InFile)
{
	for (const auto& Type : LockableTypes)
	{
		if (InFile.EndsWith(Type))
		{
			return true;
		}
	}
	return false;
}

bool CheckLFSLockable(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages)
{
	TArray<FString> Results;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("lockable")); // follow file renames

	const bool bResults = RunCommand(TEXT("check-attr"), InPathToGitBinary, InRepositoryRoot, Parameters, InFiles, Results, OutErrorMessages);
	if (!bResults)
	{
		return false;
	}

	for (int i = 0; i < InFiles.Num(); i++)
	{
		const FString& Result = Results[i];
		if (Result.EndsWith("set"))
		{
			const FString FileExt = InFiles[i].RightChop(1); // Remove wildcard (*)
			LockableTypes.Add(FileExt);
		}
	}

	return true;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetOriginRevisionOnBranch( const FString & InPathToGitBinary, const FString & InRepositoryRoot, const FString & InRelativeFileName, TArray<FString> & OutErrorMessages, const FString & BranchName )
{
	TGitSourceControlHistory OutHistory;

	TArray< FString > Results;
	TArray< FString > Parameters;
	Parameters.Add( BranchName );
	Parameters.Add( TEXT( "--date=raw" ) );
	Parameters.Add( TEXT( "--pretty=medium" ) ); // make sure format matches expected in ParseLogResults

	TArray< FString > Files;
	const auto bResults = RunCommand( TEXT( "show" ), InPathToGitBinary, InRepositoryRoot, Parameters, Files, Results, OutErrorMessages );

	if ( bResults )
	{
		ParseLogResults( Results, OutHistory );
	}

	if ( OutHistory.Num() > 0 )
	{
		auto AbsoluteFileName = FPaths::ConvertRelativePathToFull( InRelativeFileName );

		AbsoluteFileName.RemoveFromStart( InRepositoryRoot );

		if ( AbsoluteFileName[ 0 ] == '/' )
		{
			AbsoluteFileName.RemoveAt( 0 );
		}

		OutHistory[ 0 ]->Filename = AbsoluteFileName;

		return OutHistory[ 0 ];
	}

	return nullptr;
}

} // namespace FriendshipperSourceControlUtils

#undef LOCTEXT_NAMESPACE
