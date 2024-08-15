# Friendshipper Source Control Plugin

This plugin is originally based on the Project Borealis' [UEGitPlugin Fork](https://github.com/ProjectBorealis/UEGitPlugin). It has been heavily modified to use the [Friendshipper](https://github.com/believer-oss/ethos/tree/main/friendshipper) HTTP API instead of shelling out to git directly, though some operations still remain.

This means that Friendshipper _must_ be running for source control operations to work - this is a strict requirement of the plugin. See this documentation (TODO link) for more information on how Friendshipper source control works.

## Installation

Copy the entire repo (without the .git directory) into your project's `Plugins/` folder, and enable the plugin in your `.uproject` file like so:

```
{
	"Plugins":
	[
		{
			"Name": "FriendshipperSourceControl",
			"Enabled": true
		}
	]
}
```

### Note about .gitattributes and .gitignore

This plugin requires explicit file attributes for `*.umap` and `*.uasset`, rather than other approaches of using wildcards for the content folder (`Content/**`).

For an example, see the [Project Borealis `.gitattributes`](https://github.com/ProjectBorealis/PBCore/blob/main/.gitattributes).

You may also want to see [their `.gitignore`](https://github.com/ProjectBorealis/PBCore/blob/main/.gitignore).

### Note about Unreal configuration

#### Required

- The plugin makes the assumption that files are always explicitly added. We made this decision because it is beneficial for performance and our workflows. In `Config/DefaultEditorPerProjectUserSettings.ini`

```ini
[/Script/UnrealEd.EditorLoadingSavingSettings]
bSCCAutoAddNewFiles=False
```

## Status Branches - Required Code Changes

Epic Games added Status Branches in 4.20, and this plugin has implemented support for them. See [Workflow on Fortnite](https://youtu.be/p4RcDpGQ_tI?t=1443) for more information. Here is an example of how you may apply it to your own game.

1. Make an `UUnrealEdEngine` subclass, preferrably in an editor only module, or guarded by `WITH_EDITOR`.
2. Add the following:

```cpp
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"

void UMyEdEngine::Init(IEngineLoop* InEngineLoop)
{
	Super::Init(InEngineLoop);

	// Register state branches
	const ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
	{
		ISourceControlProvider& SourceControlProvider = SourceControlModule.GetProvider();
		// Order matters. Lower values are lower in the hierarchy, i.e., changes from higher branches are assumed
		// to be automatically merged down via the CI pipeline.
		// With this paradigm, the higher the index of the branch, the stabler it is.
		const TArray<FString> Branches {"origin/main", "origin/release"};
		SourceControlProvider.RegisterStateBranches(Branches, TEXT("Content"));
	}
}
```

3. Set to use the editor engine in `Config/DefaultEngine.ini` (make sure the class name is `MyUnrealEdEngine` for a class called `UMyUnrealEdEngine`!):

```ini
[/Script/Engine.Engine]
UnrealEdEngine=/Script/MyModule.MyEdEngine
```

See the [Project Borealis documentation](https://github.com/ProjectBorealis/UEGitPlugin?tab=readme-ov-file#status-branches---conceptual-overview) for more information about Status Branches.

## General In-Editor Usage

### Connecting to revision control:

To connect Unreal to Friendshipper, select the `Connect to Revision Control` option in the lower right hand corner `Revision Control` menu. From the `Provider` dropdown in the dialog, select `Friendshipper`. You should see the settings get automatically filled out with your user info if Friendshipper is running and has been setup correctly.

### Submitting changes:

`Submit Content` leverages the Friendshipper Quick Submit flow, which allows users to submit changes without having latest changes. This unlocks critical workflow needs for users in larger studios, where lots of changes are being merged 

