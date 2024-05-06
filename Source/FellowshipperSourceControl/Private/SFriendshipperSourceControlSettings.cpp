// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "SFriendshipperSourceControlSettings.h"

#include "Fonts/SlateFontInfo.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SFilePathPicker.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "EditorDirectories.h"
#include "SourceControlOperations.h"
#include "FriendshipperSourceControlModule.h"
#include "FriendshipperSourceControlUtils.h"
#include "Runtime/Launch/Resources/Version.h"


#define LOCTEXT_NAMESPACE "SFriendshipperSourceControlSettings"

void SFriendshipperSourceControlSettings::Construct(const FArguments& InArgs)
{
	bAutoCreateGitIgnore = true;
	bAutoCreateReadme = true;
	bAutoCreateGitAttributes = false;
	bAutoInitialCommit = true;

	InitialCommitMessage = LOCTEXT("InitialCommitMessage", "Initial commit");
	ReadmeContent = FText::FromString(FString(TEXT("# ")) + FApp::GetProjectName() + "\n\nDeveloped with Unreal Engine\n");

	ConstructBasedOnEngineVersion( );
}

void SFriendshipperSourceControlSettings::ConstructBasedOnEngineVersion( )
{
	const FText FileFilterType = NSLOCTEXT("GitSourceControl", "Executables", "Executables");
#if PLATFORM_WINDOWS
	const FString FileFilterText = FString::Printf(TEXT("%s (*.exe)|*.exe"), *FileFilterType.ToString());
#else
	const FString FileFilterText = FString::Printf(TEXT("%s"), *FileFilterType.ToString());
#endif

	using Self = std::remove_pointer_t<decltype(this)>;

	#define ROW_LEFT( PADDING_HEIGHT ) +SHorizontalBox::Slot() \
			.VAlign(VAlign_Center) \
			.HAlign(HAlign_Right) \
			.FillWidth(1.0f) \
			.Padding(FMargin(0.0f, 0.0f, 16.0f, PADDING_HEIGHT))

	#define ROW_RIGHT( PADDING_HEIGHT ) +SHorizontalBox::Slot() \
			.VAlign(VAlign_Center) \
			.FillWidth(2.0f) \
			.Padding(FMargin(0.0f, 0.0f, 0.0f, PADDING_HEIGHT))

	#define TT_GitPath LOCTEXT("BinaryPathLabel_Tooltip", "Path to Git binary")
	#define TT_RepoRoot LOCTEXT("RepositoryRootLabel_Tooltip", "Path to the root of the Git repository")
	#define TT_UserName LOCTEXT("UserNameLabel_Tooltip", "Git Username fetched from local config")
	#define TT_Email LOCTEXT("GitUserEmail_Tooltip", "Git E-mail fetched from local config")
	#define TT_LFS LOCTEXT("UseGitLfsLocking_Tooltip", "Uses Git LFS 2 File Locking workflow (CheckOut and Commit/Push).")

	ChildSlot
	[
		SNew(SVerticalBox)
		// Git Path
		+SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BinaryPathLabel", "Git Path"))
				.ToolTipText( TT_GitPath )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(SFilePathPicker)
				.BrowseButtonImage(FAppStyle::GetBrush("PropertyWindow.Button_Ellipsis"))
				.BrowseButtonStyle(FAppStyle::Get(), "HoverHintOnly")
				.BrowseDirectory(FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_OPEN))
				.BrowseTitle(LOCTEXT("BinaryPathBrowseTitle", "File picker..."))
				.FilePath(this, &Self::GetBinaryPathString)
				.FileTypeFilter(FileFilterText)
				.OnPathPicked(this, &Self::OnBinaryPathPicked)
			]
		]
		// Repository Root
		+SVerticalBox::Slot()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RepositoryRootLabel", "Root of the repository"))
				.ToolTipText( TT_RepoRoot )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(STextBlock)
				.Text(this, &Self::GetPathToRepositoryRoot)
				.ToolTipText( TT_RepoRoot )
			]
		]
		// User Name
		+SVerticalBox::Slot()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("UserNameLabel", "User Name"))
				.ToolTipText( TT_UserName )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(STextBlock)
				.Text(this, &Self::GetUserName)
				.ToolTipText( TT_UserName )
			]
		]
		// Email
		+SVerticalBox::Slot()
		[
			SNew(SHorizontalBox)
			ROW_LEFT( 10.0f )
			[
				SNew(STextBlock)
				.Text(LOCTEXT("EmailLabel", "E-mail"))
				.ToolTipText( TT_Email )
			]
			ROW_RIGHT( 10.0f )
			[
				SNew(STextBlock)
				.Text(this, &Self::GetUserEmail )
				.ToolTipText( TT_Email )
			]
		]
	];

	// TODO [RW] The UE5 GUI for the two optional initial git support functionalities has not been tested
}

SFriendshipperSourceControlSettings::~SFriendshipperSourceControlSettings()
{
	RemoveInProgressNotification();
}

FString SFriendshipperSourceControlSettings::GetBinaryPathString() const
{
	const FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	return GitSourceControl.AccessSettings().GetBinaryPath();
}

void SFriendshipperSourceControlSettings::OnBinaryPathPicked( const FString& PickedPath ) const
{
	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	FString PickedFullPath = FPaths::ConvertRelativePathToFull(PickedPath);
	const bool bChanged = GitSourceControl.AccessSettings().SetBinaryPath(PickedFullPath);
	if(bChanged)
	{
		// Re-Check provided git binary path for each change
		GitSourceControl.GetProvider().CheckGitAvailability();
		if(GitSourceControl.GetProvider().IsGitAvailable())
		{
			GitSourceControl.SaveSettings();
		}
	}
}

FText SFriendshipperSourceControlSettings::GetPathToRepositoryRoot() const
{
	const FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	const FString& PathToRepositoryRoot = GitSourceControl.GetProvider().GetPathToRepositoryRoot();
	return FText::FromString(PathToRepositoryRoot);
}

FText SFriendshipperSourceControlSettings::GetUserName() const
{
	const FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	const FString& UserName = GitSourceControl.GetProvider().GetUserName();
	return FText::FromString(UserName);
}

FText SFriendshipperSourceControlSettings::GetUserEmail() const
{
	const FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	const FString& UserEmail = GitSourceControl.GetProvider().GetUserEmail();
	return FText::FromString(UserEmail);
}

// Launch an asynchronous "CheckIn" operation and start another ongoing notification
void SFriendshipperSourceControlSettings::LaunchCheckInOperation()
{
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOperation = ISourceControlOperation::Create<FCheckIn>();
	CheckInOperation->SetDescription(InitialCommitMessage);
	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	ECommandResult::Type Result = GitSourceControl.GetProvider().Execute(CheckInOperation, FSourceControlChangelistPtr(), FFriendshipperSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateSP(this, &SFriendshipperSourceControlSettings::OnSourceControlOperationComplete));
	if (Result == ECommandResult::Succeeded)
	{
		DisplayInProgressNotification(CheckInOperation);
	}
	else
	{
		DisplayFailureNotification(CheckInOperation);
	}
}

/// Delegate called when a Revision control operation has completed: launch the next one and manage notifications
void SFriendshipperSourceControlSettings::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	RemoveInProgressNotification();

	// Report result with a notification
	if (InResult == ECommandResult::Succeeded)
	{
		DisplaySuccessNotification(InOperation);
	}
	else
	{
		DisplayFailureNotification(InOperation);
	}
}


// Display an ongoing notification during the whole operation
void SFriendshipperSourceControlSettings::DisplayInProgressNotification(const FSourceControlOperationRef& InOperation)
{
	FNotificationInfo Info(InOperation->GetInProgressString());
	Info.bFireAndForget = false;
	Info.ExpireDuration = 0.0f;
	Info.FadeOutDuration = 1.0f;
	OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
	}
}

// Remove the ongoing notification at the end of the operation
void SFriendshipperSourceControlSettings::RemoveInProgressNotification()
{
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
		OperationInProgressNotification.Reset();
	}
}

// Display a temporary success notification at the end of the operation
void SFriendshipperSourceControlSettings::DisplaySuccessNotification(const FSourceControlOperationRef& InOperation)
{
	const FText NotificationText = FText::Format(LOCTEXT("InitialCommit_Success", "{0} operation was successfull!"), FText::FromName(InOperation->GetName()));
	FNotificationInfo Info(NotificationText);
	Info.bUseSuccessFailIcons = true;
	Info.Image = FAppStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
	FSlateNotificationManager::Get().AddNotification(Info);
}

// Display a temporary failure notification at the end of the operation
void SFriendshipperSourceControlSettings::DisplayFailureNotification(const FSourceControlOperationRef& InOperation)
{
	const FText NotificationText = FText::Format(LOCTEXT("InitialCommit_Failure", "Error: {0} operation failed!"), FText::FromName(InOperation->GetName()));
	FNotificationInfo Info(NotificationText);
	Info.ExpireDuration = 8.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

void SFriendshipperSourceControlSettings::OnCheckedCreateGitIgnore(ECheckBoxState NewCheckedState)
{
	bAutoCreateGitIgnore = (NewCheckedState == ECheckBoxState::Checked);
}

void SFriendshipperSourceControlSettings::OnCheckedCreateReadme(ECheckBoxState NewCheckedState)
{
	bAutoCreateReadme = (NewCheckedState == ECheckBoxState::Checked);
}

bool SFriendshipperSourceControlSettings::GetAutoCreateReadme() const
{
	return bAutoCreateReadme;
}

void SFriendshipperSourceControlSettings::OnReadmeContentCommited(const FText& InText, ETextCommit::Type InCommitType)
{
	ReadmeContent = InText;
}

FText SFriendshipperSourceControlSettings::GetReadmeContent() const
{
	return ReadmeContent;
}

void SFriendshipperSourceControlSettings::OnCheckedCreateGitAttributes(ECheckBoxState NewCheckedState)
{
	bAutoCreateGitAttributes = (NewCheckedState == ECheckBoxState::Checked);
}

FText SFriendshipperSourceControlSettings::GetLfsUserName() const
{
	FFriendshipperSourceControlModule& GitSourceControl = FFriendshipperSourceControlModule::Get();
	const FString LFSUserName = GitSourceControl.AccessSettings().GetLfsUserName();
	if (LFSUserName.IsEmpty())
	{
		const FText& UserName = GetUserName();
		GitSourceControl.AccessSettings().SetLfsUserName(UserName.ToString());
		GitSourceControl.AccessSettings().Save();
		GitSourceControl.GetProvider().UpdateSettings();
		return UserName;
	}

        return FText::FromString(LFSUserName);
}

void SFriendshipperSourceControlSettings::OnCheckedInitialCommit(ECheckBoxState NewCheckedState)
{
	bAutoInitialCommit = (NewCheckedState == ECheckBoxState::Checked);
}

bool SFriendshipperSourceControlSettings::GetAutoInitialCommit() const
{
	return bAutoInitialCommit;
}

void SFriendshipperSourceControlSettings::OnInitialCommitMessageCommited(const FText& InText, ETextCommit::Type InCommitType)
{
	InitialCommitMessage = InText;
}

FText SFriendshipperSourceControlSettings::GetInitialCommitMessage() const
{
	return InitialCommitMessage;
}

void SFriendshipperSourceControlSettings::OnRemoteUrlCommited(const FText& InText, ETextCommit::Type InCommitType)
{
	RemoteUrl = InText;
}

FText SFriendshipperSourceControlSettings::GetRemoteUrl() const
{
	return RemoteUrl;
}

#undef LOCTEXT_NAMESPACE
