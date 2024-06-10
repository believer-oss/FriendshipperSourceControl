// Copyright The Believer Company. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "FriendshipperTranslateOFPAFilenamesCommandlet.generated.h"

// This commandlet translates a given set of paths to files to their in-engine asset names.
//
// Usage:
//
//   UnrealEditor-Cmd.exe <PathToUProject> -run=TranslateOFPAFilenames <Space separated filenames.>
//
// Arguments:
//		Paths to assets to translate. Specify a space-separated list of paths. For example:
//       -run=TranslateOFPAFilenames D:/repos/fellowship/Plugins/GameFeatures/ShooterMaps/Content/__ExternalActors__/Maps/L_Convolution_Blockout/0/CR/ZZ6IFPOFLPOW1WBSFYOPW6.uasset
//       -run=TranslateOFPAFilenames D:/repos/fellowship/Content/__ExternalObjects__/Levels/DevTest/Traversal/L_TraversalGym/0/Z0/ZZGHJ5DWZEAJAM6BNQYL8O.uasset
//       -run=TranslateOFPAFilenames Content/__ExternalObjects__/Levels/DevTest/Traversal/L_TraversalGym/0/Z0/ZZGHJ5DWZEAJAM6BNQYL8O.uasset Content/__ExternalActors__/Levels/DevTest/Combat/L_CombatGym/C/BO/ZZGR76KX8SMOWJNZAZ63KX.uasset
//
UCLASS()
class UTranslateOFPAFilenamesCommandlet : public UCommandlet
{
	GENERATED_BODY()

	virtual int32 Main(const FString& Params) override;
};
