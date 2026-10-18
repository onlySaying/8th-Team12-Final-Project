// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CheatManager.h"
#include "FebriculaCheatManager.generated.h"

/**
 * 
 */
UCLASS()
class FEBRICULA_API UFebriculaCheatManager : public UCheatManager
{
	GENERATED_BODY()
public:
	UFUNCTION(Exec)
	void SkipDay(int32 Day = 1);
	
	UFUNCTION(Exec)
	void SkipSchedule(int32 Count = 1);
	
	UFUNCTION(Exec)
	void GetCachedCharacter();
	
	UFUNCTION(Exec)
	void GetCurrentAP();
	
	UFUNCTION(Exec)
	void GetClearedEvents();
	
	UFUNCTION(Exec)
	void AddClearedEvent(FName EventId);
	
	UFUNCTION(Exec)
	void GetRunnableActivities();
	
	UFUNCTION(Exec)
	void GetAllActivities();
	
	UFUNCTION(Exec)
	void GetAllCachedTargetPoints();
	
	UFUNCTION(Exec)
	void GetNowStat(FName StatName);
	
	UFUNCTION(Exec)
	void GetMoveHistory();
	
	UFUNCTION(Exec)
	void GetNextScheduleName();
	
	UFUNCTION(Exec)
	void InScheduleBlock(FName BlockName);
	
	UFUNCTION(Exec)
	void SetSchedule(FName ScheduleName);
	
	UFUNCTION(Exec)
	void TeleportDay(int32 Day);

	/** [skydodo] 2026-10-18 추가: 지금 떠 있는 대화 선택지 중 Index(0부터)번째를 고른다 (본편 자동 플레이 테스트용) */
	UFUNCTION(Exec)
	void DialogueChoose(int32 Index);

	/** [skydodo] 2026-10-18 추가: 진행 중인 미니게임을 Score 점수로 끝낸다 (보상 지급 → 본편 복귀까지) */
	UFUNCTION(Exec)
	void MiniGameFinish(int32 Score = 5);
};
