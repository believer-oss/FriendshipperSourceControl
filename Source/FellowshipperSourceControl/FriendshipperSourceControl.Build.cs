// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

using UnrealBuildTool;

public class FriendshipperSourceControl : ModuleRules
{
	public FriendshipperSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"HTTP",
				"Json",
				"JsonUtilities",
				"Slate",
				"SlateCore",
				"InputCore",
				"DesktopWidgets",
				"EditorStyle",
				"UnrealEd",
				"SourceControl",
				"SourceControlWindows",
				"Projects"
			}
		);

		if (Target.Version.MajorVersion == 5)
		{
			PrivateDependencyModuleNames.Add("ToolMenus");
		}
	}
}
