// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Febricula : ModuleRules
{
	public Febricula(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			// [skydodo] 2026-10-10 추가: 미니게임 위젯이 NativePaint에서 직접 그린다 (FSlateDrawElement, 브러시, 글꼴)
			"SlateCore",
			"DeveloperSettings",
			"GameplayTags", "CinematicCamera",
			// 메인 NPC 순찰·안내 이동
			"NavigationSystem",
			// [skydodo] 2026-10-07 추가: 행인 Mass / AnimToTexture 모듈
			// 행인 이동 계산을 Mass 프로세서에서 병렬로 돌린다 (SubNpcMassProcessors).
			"MassEntity",
			"MassCore",
			// 멀리 있는 행인을 인스턴스드 메시 + 텍스처로 구운 애니메이션으로 그린다 (SubNpcMassProcessors).
			"AnimToTexture",
			// 행인 상태 결정(스팟에 들를지 등)을 Mass StateTree로 (SubNpcMassStateTree).
			"MassAIBehavior",
			"MassSignals"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		// [skydodo] 2026-10-07 추가
		// 원경 행인 애니메이션 굽기 명령 crowd.BakeFarCrowd (SubNpcFarCrowdBake.cpp, 에디터 전용)
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("AnimToTextureEditor");
		}

		PublicIncludePaths.AddRange(new string[] {
			"Febricula"
		});

	}
}
