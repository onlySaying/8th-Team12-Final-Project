// [skydodo] 2026-10-10 작성
// 요약: 미니게임 프로젝트 설정 - 어느 레벨에서 어떤 미니게임을 띄울지
// Fill out your copyright notice in the Description page of Project Settings.
//
// Project Settings > Game > MiniGame Setting 에서 편집한다.
// 레벨 이름(예: L_ConvenienceStore)을 키로, 들어오자마자 띄울 미니게임과 시작 위치·보상을 적는다.
// 본편 대화의 StartMiniGame 액션은 레벨 이동 없이 이 키(예: Burger)로 지금 레벨에서 바로 띄운다.
// 레벨에 액터를 놓지 않으므로 다른 사람 맵 파일을 저장할 필요가 없다.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MiniGame/MiniGameTypes.h"
#include "MiniGameSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "MiniGame Setting"))
class FEBRICULA_API UMiniGameSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMiniGameSettings();

	/** 키: 레벨 애셋 이름 (경로 없이, 예: L_ConvenienceStore) 또는 StartMiniGame 액션이 쓰는 이름 (예: Burger) */
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "MiniGame")
	TMap<FName, FMiniGameLevelEntry> Levels;

	/** 돈을 쌓을 GameProgressSubsystem 카운터 이름 (아직 돈 시스템이 없어 카운터로 둔다) */
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "MiniGame")
	FName MoneyCounterId = TEXT("Money");
};
