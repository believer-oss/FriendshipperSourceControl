#pragma once

#include "SFriendshipperSourceControlRevert.h"
#include "Styling/AppStyle.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

/*
 * We've taken some classes and widgets from Unreal's internal Source Control code because it was blocking
 * reverts for level files. The code in this file and the adjacent .cpp come from SSourceControlRevert.h/.cpp
 * as of Unreal 5.1.1.
 *
 * GitSourceControlModule consumes this widget as part of a new Asset context button that uses a revert flow
 * which allows for the reverting of level/world files. The default Unreal source control revert remains unmodified.
 */

namespace ERevertResults
{
	enum Type
	{
		REVERT_ACCEPTED,
		REVERT_CANCELED
	};
}


struct FRevertCheckBoxListViewItem
{
	/**
	 * Constructor
	 *
	 * @param	InText	String that should appear for the item in the list view
	 */
	FRevertCheckBoxListViewItem( FString InText )
	{
		Text = InText;
		IsSelected = false;
		IsModified = false;
	}

	void OnCheckStateChanged( const ECheckBoxState NewCheckedState )
	{
		IsSelected = (NewCheckedState == ECheckBoxState::Checked);
	}

	ECheckBoxState OnIsChecked() const
	{
		return ( IsSelected ) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	}

	EVisibility OnGetModifiedStateVisibility() const
	{
		return (IsModified) ? EVisibility::Visible : EVisibility::Hidden;
	}

	bool IsSelected;
	bool IsModified;
	FString Text;
};

/**
 * Source control panel for reverting files. Allows the user to select which files should be reverted, as well as
 * provides the option to only allow unmodified files to be reverted.
 *
 * This Widget was originally taken from Unreal's SSourceControlRevertWidget, which was not exported in a re-usable
 * fashion as far as we could tell. 
 */
class SFriendshipperSourceControlRevertWidget : public SCompoundWidget
{
public:

	//* @param	InXamlName		Name of the XAML file defining this panel
	//* @param	InPackageNames	Names of the packages to be potentially reverted
	SLATE_BEGIN_ARGS( SFriendshipperSourceControlRevertWidget )
		: _ParentWindow()
		, _PackagesToRevert()
	{}

		SLATE_ATTRIBUTE( TSharedPtr<SWindow>, ParentWindow )	
		SLATE_ATTRIBUTE( TArray<FString>, PackagesToRevert )

	SLATE_END_ARGS()

	/**
	 * Constructor.
	 */
	SFriendshipperSourceControlRevertWidget()
	{
	}

	void Construct( const FArguments& InArgs );

	/**
	 * Populates the provided array with the names of the packages the user elected to revert, if any.
	 *
	 * @param	OutPackagesToRevert	Array of package names to revert, as specified by the user in the dialog
	 */
	void GetPackagesToRevert( TArray<FString>& OutPackagesToRevert );

	ERevertResults::Type GetResult()
	{
		return DialogResult;
	}

private:

	TSharedRef<ITableRow> OnGenerateRowForList( TSharedPtr<FRevertCheckBoxListViewItem> ListItemPtr, const TSharedRef<STableViewBase>& OwnerTable );

	/** Called when the settings of the dialog are to be accepted*/
	FReply OKClicked();
	bool IsOKEnabled() const;

	/** Called when the settings of the dialog are to be ignored*/
	FReply CancelClicked();

	/** Called when the user checks or unchecks the revert unchanged checkbox; updates the list view accordingly */
	void RevertUnchangedToggled( const ECheckBoxState NewCheckedState );

	/**
	 * Called whenever a column header is clicked, or in the case of the dialog, also when the "Check/Uncheck All" column header
	 * checkbox is called, because its event bubbles to the column header. 
	 */
	void ColumnHeaderClicked( const ECheckBoxState NewCheckedState );

	/** Caches the current state of the files, */
	void UpdateSCCStatus();

	/** Check for whether the list items are enabled or not */
	bool OnGetItemsEnabled() const
	{
		return !bRevertUnchangedFilesOnly;
	}

	TWeakPtr<SWindow> ParentFrame;
	ERevertResults::Type DialogResult;

	/** ListView for the packages the user can revert */
	typedef SListView<TSharedPtr<FRevertCheckBoxListViewItem>> SListViewType;
	TSharedPtr<SListViewType> RevertListView;

	/** Collection of items serving as the data source for the list view */
	TArray<TSharedPtr<FRevertCheckBoxListViewItem>> ListViewItemSource;

	/** List of package names that are modified from the versions stored in source control; Used as an optimization */
	TArray<FString> ModifiedPackages;

	/** Flag set by the user to only revert non modified files */
	bool bRevertUnchangedFilesOnly;
};
