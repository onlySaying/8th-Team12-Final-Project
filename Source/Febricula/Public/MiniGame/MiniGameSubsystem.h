// [skydodo] 2026-10-10 작성
// 요약: 미니게임 진행 - 레벨이 열리면 시작, 끝나면 보상 지급 후 원래 레벨로 복귀
// Fill out your copyright notice in the Description page of Project Settings.
//
// 흐름:
//   (테스트 맵) AMiniGameDoor 상호작용 → 미니게임 레벨 열기
//   → UMiniGameSubsystem::OnWorldBeginPlay 가 MiniGame Setting에서 이 레벨을 찾음
//   → 플레이어를 시작 위치로 옮기고 입력을 위젯으로 넘김, 위젯 표시
//   → 위젯이 FinishGame() → 보상 지급 (한 번만)
//   → 위젯이 ExitGame() → 들어오기 전 레벨로 OpenLevel
//
// "들어오기 전 레벨"은 레벨을 넘어가도 살아 있어야 하므로 GameInstance 쪽(UMiniGameTravelSubsystem)에 둔다.

#pragma once

#include "CoreMinimal.h"
#include "MiniGame/MiniGameTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "MiniGameSubsystem.generated.h"

class APlayerController;
class UAudioComponent;
class UMiniGameWidget;

/** 레벨 이동 기록. 미니게임이 끝나면 들어오기 전 레벨로 돌려보낸다 */
UCLASS()
class FEBRICULA_API UMiniGameTravelSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** 레벨이 시작될 때마다 부른다. 같은 레벨을 다시 열면(재시작) 이전 레벨을 덮어쓰지 않는다 */
	void NotifyLevelStarted(const UWorld& World);

	/** 들어오기 전 레벨로 이동. 모르면 Fallback으로. 둘 다 없으면 false */
	bool ReturnToPreviousLevel(const TSoftObjectPtr<UWorld>& Fallback);

	const FString& GetPreviousLevel() const { return PreviousLevel; }
	void SetPreviousLevel(const FString& NewPreviousLevel) { PreviousLevel = NewPreviousLevel; }

	/**
	 * 문으로 미니게임에 들어갈 때 플레이어가 서 있던 자리. 돌아오면 PlayerStart 대신 이 자리에 세운다.
	 * (PlayerStart가 행인이 다니는 길 위면, 돌아오자마자 행인 무리에 밀려 혼자 움직이는 것처럼 보였다)
	 */
	void SetReturnPoint(const UWorld& World, const FTransform& Transform);

	/** 이 레벨에 쓸 돌아올 자리가 있으면 꺼낸다 (한 번 쓰면 지운다) */
	bool ConsumeReturnPoint(const UWorld& World, FTransform& OutTransform);

	static FString GetLevelPath(const UWorld& World);
	
	bool GetMinigameBooking() const { return MinigameBooking; }
	void SetMinigameBooking(bool State) { MinigameBooking = State; }


private:
	/** 긴 패키지 경로 (PIE 접두사 제거). 예: /Game/MiniGame/Maps/L_MiniGameTest */
	FString CurrentLevel;
	FString PreviousLevel;

	FString ReturnPointLevel;
	FTransform ReturnPoint;
	bool bHasReturnPoint = false;
	bool MinigameBooking = false;

};

UCLASS()
class FEBRICULA_API UMiniGameSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UWorldSubsystem
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	//~ End UWorldSubsystem

	bool IsMiniGameActive() const { return ActiveWidget != nullptr; }

	/** MiniGame Setting의 EntryKey 항목을 레벨 이동 없이 지금 레벨에서 띄운다 (본편 대화의 StartMiniGame 액션) */
	bool StartHere(FName EntryKey);
	
	void TryStart();

	/**
	 * 이 미니게임을 끝까지 플레이한 횟수 (건너뛰기 제외). 세이브에 남는다.
	 * 0이면 처음 - 설명 카드를 보여주고, 1 이상이면 건너뛰기를 허용한다.
	 */
	int32 GetPlayCount() const;

	/** 점수로 받을 보상 계산만 한다 (지급 X). 플레이 중 "예상 보상" 표시용 */
	FMiniGameReward PreviewReward(int32 Score) const;

	/** 보상을 지급한다. 한 판에 한 번만 지급하고, 두 번째부터는 지급했던 값을 그대로 돌려준다 */
	FMiniGameReward GrantReward(const FMiniGameResult& Result);

	
	/** 위젯을 닫고 입력을 돌려준 뒤 원래 레벨로 이동 */
	void EndMiniGame();
	
	
private:
	/** 플레이어 폰이 생길 때까지 잠깐 기다렸다가 시작한다 */

	/** 미니게임에서 돌아온 레벨: 들어간 자리로 되돌리고 키보드 포커스를 게임 화면으로 돌려준다 */
	void TryRestoreReturnPoint();
	void StartMiniGame(APlayerController* PC);

	static FName GetLevelShortName(const UWorld& World);

	/** 플레이 횟수를 쌓는 GameProgressSubsystem 카운터 이름. 예: MiniGame.Burger.Played */
	FName GetPlayCounterId() const;

	FMiniGameLevelEntry ActiveEntry;
	FName ActiveLevelKey;
	bool bHasEntry = false;

	UPROPERTY(Transient)
	TObjectPtr<UMiniGameWidget> ActiveWidget;

	/** 미니게임 동안 쓰는 고정 카메라 (레벨에 놓지 않고 런타임에 만든다) */
	UPROPERTY(Transient)
	TObjectPtr<AActor> FixedCamera;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> BGMComponent;

	FTimerHandle StartTimer;
	int32 StartAttempts = 0;

	FTransform PendingReturnPoint;
	FTimerHandle ReturnTimer;
	int32 ReturnAttempts = 0;

	bool bRewardGranted = false;
	FMiniGameReward GrantedReward;
	

};
