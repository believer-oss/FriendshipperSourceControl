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
                "DesktopWidgets",
                "EditorStyle",
                "Engine",
                "HTTP",
                "HTTPServer",
                "InputCore",
                "Json",
                "JsonUtilities",
                "Projects",
                "Slate",
                "SlateCore",
                "SourceControl",
                "SourceControlWindows",
                "UnrealEd",
                "DirectoryWatcher",

                "OpenTelemetry",

                "FriendshipperCore",
			}
		);

		if (Target.Version.MajorVersion == 5)
		{
			PrivateDependencyModuleNames.Add("ToolMenus");
		}
	}
}
