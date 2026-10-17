// [skydodo] 2026-10-10 작성
// 요약: 미니게임 설정 기본값
// Fill out your copyright notice in the Description page of Project Settings.

#include "MiniGame/MiniGameSettings.h"

#include "MiniGame/MiniGameWidget.h"
#include "Sound/SoundBase.h"

UMiniGameSettings::UMiniGameSettings()
{
	// 햄버거집 알바. ini에 값이 있으면 그쪽이 이긴다.
	// [skydodo] 2026-10-18 변경: 햄버거집 맵을 지우고, 본편 대화의 StartMiniGame 액션이 지금 레벨에서 바로 띄운다.
	FMiniGameLevelEntry Burger;
	Burger.WidgetClass = TSoftClassPtr<UMiniGameWidget>(FSoftObjectPath(TEXT("/Script/Febricula.BurgerMiniGameWidget")));
	Burger.bMovePlayer = false;
	Burger.bUseFixedCamera = false;
	Burger.BGM = TSoftObjectPtr<USoundBase>(FSoftObjectPath(TEXT("/Game/MiniGame/Burger/Audio/S_Burger_BGM.S_Burger_BGM")));
	Burger.BGMVolume = 0.6f;
	Burger.MoneyReward = 40000;
	Burger.RewardStat = TEXT("HandWork");
	Burger.StatThresholds = { 3, 5, 7, 9 };
	Levels.Add(TEXT("Burger"), Burger);

	// 꽃집 알바. 햄버거집처럼 본편 대화에서 바로 띄운다.
	// 가로 화면 패널이 화면을 다 덮으므로 플레이어 이동 · 고정 카메라는 쓰지 않는다.
	FMiniGameLevelEntry Flower;
	Flower.WidgetClass = TSoftClassPtr<UMiniGameWidget>(FSoftObjectPath(TEXT("/Script/Febricula.FlowerMiniGameWidget")));
	Flower.bMovePlayer = false;
	Flower.bUseFixedCamera = false;
	Flower.MoneyReward = 30000;
	Flower.RewardStat = TEXT("Health");
	// 완료 주문 수 0~6건 +1, 7~9건 +2, 10~11건 +3, 12~13건 +4, 14건 이상 +5.
	// 주문은 묶음이 작업대에 동시에 다 있어야 완료된다. 난이도 시뮬레이션(실제 물리 코드 그대로, 전략별 30판) 기준
	// 천천히(0.6초 간격) 9건 +2, 쉬지 않고 놓으면 11~13건 +3~+4, 14건(+5)은 30판 중 손에 꼽게 나온다.
	Flower.StatThresholds = { 7, 10, 12, 14 };
	Levels.Add(TEXT("Flower"), Flower);

	// [skydodo] 2026-10-14 추가: 편의점 야간 알바 (빈 맵 L_ConvenienceStore - 팀원이 편의점 맵을 만들면 키만 바꾼다).
	// 가로 화면 전체를 위젯이 덮으므로 플레이어 이동 · 고정 카메라는 쓰지 않는다.
	FMiniGameLevelEntry Survivor;
	Survivor.WidgetClass = TSoftClassPtr<UMiniGameWidget>(FSoftObjectPath(TEXT("/Script/Febricula.SurvivorMiniGameWidget")));
	Survivor.bMovePlayer = false;
	Survivor.bUseFixedCamera = false;
	Survivor.BGM = TSoftObjectPtr<USoundBase>(FSoftObjectPath(TEXT("/Game/MiniGame/Survivor/Audio/S_Survivor_BGM.S_Survivor_BGM")));
	Survivor.BGMVolume = 0.45f;
	Survivor.FallbackReturnLevel = TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/MiniGame/Maps/L_MiniGameTest.L_MiniGameTest")));
	Survivor.MoneyReward = 30000;
	Survivor.RewardStat = TEXT("Speech");
	// 점수 = 퇴점시킨 손님 수. 0~54명 +1, 55~79명 +2, 80~99명 +3, 100~119명 +4, 120명 이상 +5.
	// 난이도 시뮬레이션(실제 규칙, 봇 40판씩) 기준 가만히 있으면 30명 안팎(+1), 피하며 주우면 중앙 95명(+3),
	// 상위 25%가 110명(+4), 최고 135명 안팎(+5).
	Survivor.StatThresholds = { 55, 80, 100, 120 };
	Levels.Add(TEXT("L_ConvenienceStore"), Survivor);
}
