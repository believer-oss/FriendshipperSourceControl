// Copyright The Believer Company. All Rights Reserved.

using UnrealBuildTool;

public class FriendshipperCore : ModuleRules
{
	public FriendshipperCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
                "Core",
                "CoreUObject",
                "Engine",
			}
		);
	}
}
