#include "SFriendshipperSourceControlRevert.h"

#include "Styling/AppStyle.h"

#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "FriendshipperSourceControlOperations.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Layout/SUniformGridPanel.h"

#define LOCTEXT_NAMESPACE "SFriendshipperSourceControlRevert"

/**
 * Source control panel for reverting files. Allows the user to select which files should be reverted, as well as
 * provides the option to only allow unmodified files to be reverted.
 */
void SFriendshipperSourceControlRevertWidget::Construct( const FArguments& InArgs )
{
	ParentFrame = InArgs._ParentWindow.Get();

	for ( TArray<FString>::TConstIterator PackageIter( InArgs._PackagesToRevert.Get() ); PackageIter; ++PackageIter )
	{
		ListViewItemSource.Add( MakeShareable(new FRevertCheckBoxListViewItem(*PackageIter) ));
	}


	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		[

			SNew(SVerticalBox)
			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(10)
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("SourceControl.Revert", "SelectFiles", "Select the files that should be reverted below"))
			]
			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(10,0)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(5)
				[
					SNew(SCheckBox)
					.OnCheckStateChanged(this, &SFriendshipperSourceControlRevertWidget::ColumnHeaderClicked)
					.IsEnabled(this, &SFriendshipperSourceControlRevertWidget::OnGetItemsEnabled)
					[
						SNew(STextBlock)
						.Text(NSLOCTEXT("SourceControl.Revert", "ListHeader", "File Name"))
					]
				]
			]
			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(10,0)
			.MaxHeight(300)
			[
				SNew(SBorder)
				.Padding(5)
				[
					SAssignNew(RevertListView, SListViewType)
					.ItemHeight(24)
					.ListItemsSource(&ListViewItemSource)
					.OnGenerateRow(this, &SFriendshipperSourceControlRevertWidget::OnGenerateRowForList)
				]
			]
			+SVerticalBox::Slot()
			.Padding(0, 10, 0, 0)
			.FillHeight(1)
			.VAlign(VAlign_Bottom)
			.HAlign(HAlign_Fill)
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(15,5)
				.HAlign(HAlign_Left)
				[
					SNew(SCheckBox)
					.OnCheckStateChanged(this, &SFriendshipperSourceControlRevertWidget::RevertUnchangedToggled)
					[
						SNew(STextBlock)
						.Text(NSLOCTEXT("SourceControl.Revert", "RevertUnchanged", "Revert Unchanged Only"))
					]
				]
				+SHorizontalBox::Slot()
				.HAlign(HAlign_Right)
				.FillWidth(1)
				.Padding(5)
				[
					SNew(SUniformGridPanel)
					.SlotPadding(FAppStyle::GetMargin("StandardDialog.SlotPadding"))
					.MinDesiredSlotWidth(FAppStyle::GetFloat("StandardDialog.MinDesiredSlotWidth"))
					.MinDesiredSlotHeight(FAppStyle::GetFloat("StandardDialog.MinDesiredSlotHeight"))
					+SUniformGridPanel::Slot(0,0)
					[
						SNew(SButton) 
						.HAlign(HAlign_Center)
						.ContentPadding(FAppStyle::GetMargin("StandardDialog.ContentPadding"))
						.OnClicked(this, &SFriendshipperSourceControlRevertWidget::OKClicked)
						.IsEnabled(this, &SFriendshipperSourceControlRevertWidget::IsOKEnabled)
						.Text(LOCTEXT("RevertButton", "Revert"))
					]
					+SUniformGridPanel::Slot(1,0)
					[
						SNew(SButton) 
						.HAlign(HAlign_Center)
						.ContentPadding(FAppStyle::GetMargin("StandardDialog.ContentPadding"))
						.OnClicked(this, &SFriendshipperSourceControlRevertWidget::CancelClicked)
						.Text(LOCTEXT("CancelButton", "Cancel"))
					]
				]
			]
		]
	];

	// update the modified state of all the files. 
	UpdateSCCStatus();

	DialogResult = ERevertResults::REVERT_CANCELED;
	bRevertUnchangedFilesOnly = false;
}

/**
 * Populates the provided array with the names of the packages the user elected to revert, if any.
 *
 * @param	OutPackagesToRevert	Array of package names to revert, as specified by the user in the dialog
 */
void SFriendshipperSourceControlRevertWidget::GetPackagesToRevert( TArray<FString>& OutPackagesToRevert )
{
	for ( const auto& ListViewItem : ListViewItemSource )
	{
		if ((bRevertUnchangedFilesOnly && !ListViewItem->IsModified) || 
			(!bRevertUnchangedFilesOnly && ListViewItem->IsSelected))
		{
			OutPackagesToRevert.Add(ListViewItem->Text);
		}
	}
}

TSharedRef<ITableRow> SFriendshipperSourceControlRevertWidget::OnGenerateRowForList( TSharedPtr<FRevertCheckBoxListViewItem> ListItemPtr, const TSharedRef<STableViewBase>& OwnerTable )
{
	TSharedPtr<SCheckBox> CheckBox;
	TSharedRef<ITableRow> Row =
		SNew(STableRow< TSharedPtr<FString> >, OwnerTable)
		.IsEnabled(this, &SFriendshipperSourceControlRevertWidget::OnGetItemsEnabled)
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot()
			.HAlign(HAlign_Left)
			.AutoWidth()
			[
				SAssignNew(CheckBox, SCheckBox)
				.OnCheckStateChanged(ListItemPtr.ToSharedRef(), &FRevertCheckBoxListViewItem::OnCheckStateChanged)
				.IsChecked(ListItemPtr.ToSharedRef(), &FRevertCheckBoxListViewItem::OnIsChecked)
				[
					SNew(STextBlock)
					.Text(FText::FromString(ListItemPtr->Text))
				]
			]
			+SHorizontalBox::Slot()
			.HAlign(HAlign_Right)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("ContentBrowser.ContentDirty")))
				.Visibility(ListItemPtr.ToSharedRef(), &FRevertCheckBoxListViewItem::OnGetModifiedStateVisibility)
				.ToolTipText(LOCTEXT("ModifiedFileToolTip","This file has been modified from the source version"))
			]
		];

	return Row;
}

/** Called when the settings of the dialog are to be accepted*/
FReply SFriendshipperSourceControlRevertWidget::OKClicked()
{
	DialogResult = ERevertResults::REVERT_ACCEPTED;
	ParentFrame.Pin()->RequestDestroyWindow();

	return FReply::Handled();
}

bool SFriendshipperSourceControlRevertWidget::IsOKEnabled() const
{
	if (bRevertUnchangedFilesOnly)
	{
		return true;
	}

	for (int32 i=0; i<ListViewItemSource.Num(); i++)
	{
		if (ListViewItemSource[i]->IsSelected)
		{
			return true;
		}
	}
	return false;
}

/** Called when the settings of the dialog are to be ignored*/
FReply SFriendshipperSourceControlRevertWidget::CancelClicked()
{
	DialogResult = ERevertResults::REVERT_CANCELED;
	ParentFrame.Pin()->RequestDestroyWindow();

	return FReply::Handled();
}

/** Called when the user checks or unchecks the revert unchanged checkbox; updates the list view accordingly */
void SFriendshipperSourceControlRevertWidget::RevertUnchangedToggled( const ECheckBoxState NewCheckedState )
{
	bRevertUnchangedFilesOnly = (NewCheckedState == ECheckBoxState::Checked);
}

/**
 * Called whenever a column header is clicked, or in the case of the dialog, also when the "Check/Uncheck All" column header
 * checkbox is called, because its event bubbles to the column header. 
 */
void SFriendshipperSourceControlRevertWidget::ColumnHeaderClicked( const ECheckBoxState NewCheckedState )
{
	for (int32 i=0; i<ListViewItemSource.Num(); i++)
	{
		TSharedPtr<FRevertCheckBoxListViewItem> CurListViewItem = ListViewItemSource[i];

		if (OnGetItemsEnabled())
		{
			CurListViewItem->IsSelected = (NewCheckedState == ECheckBoxState::Checked);
		}
	}
}

/** Caches the current state of the files, */
void SFriendshipperSourceControlRevertWidget::UpdateSCCStatus()
{
	TArray<FString> PackagesToCheck;
	for ( const auto& CurItem : ListViewItemSource )
	{
		PackagesToCheck.Add(SourceControlHelpers::PackageFilename(CurItem->Text));
	}

	// Make sure we update the modified state of the files
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateStatusOperation = ISourceControlOperation::Create<FUpdateStatus>();
	UpdateStatusOperation->SetUpdateModifiedState(true);
	ISourceControlModule::Get().GetProvider().Execute(UpdateStatusOperation, PackagesToCheck);

	// Find the files modified from the server version
	TArray< FSourceControlStateRef > SourceControlStates;
	ISourceControlModule::Get().GetProvider().GetState( PackagesToCheck, SourceControlStates, EStateCacheUsage::Use );

	ModifiedPackages.Empty();

	for( const auto& ControlState : SourceControlStates )
	{
		FString PackageName;
		FPackageName::TryConvertFilenameToLongPackageName(ControlState->GetFilename(), PackageName);
		for ( const auto& CurItem : ListViewItemSource )
		{
			if (CurItem->Text == PackageName)
			{
				CurItem->IsModified = ControlState->IsModified();
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
