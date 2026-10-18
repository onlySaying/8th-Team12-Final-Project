// [skydodo] 2026-10-10 작성
// 요약: 미니게임 공통 타입 - 레벨별 설정, 결과, 보상
// Fill out your copyright notice in the Description page of Project Settings.
//
// 미니게임 3개(햄버거/꽃/서바이버)가 같이 쓰는 타입.
// 게임마다 다른 건 위젯(화면 + 규칙)뿐이고, 시작·정산·복귀는 UMiniGameSubsystem이 한 곳에서 한다.

#pragma once

#include "CoreMinimal.h"
#include "MiniGameTypes.generated.h"

class UMiniGameWidget;
class USoundBase;

/** 한 레벨에 들어왔을 때 띄울 미니게임 설정 (MiniGame Setting의 Levels 한 줄) */
USTRUCT(BlueprintType)
struct FMiniGameLevelEntry
{
	GENERATED_BODY()

	/** 띄울 미니게임 위젯 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	TSoftClassPtr<UMiniGameWidget> WidgetClass;

	/**
	 * 시작할 때 플레이어를 이 위치로 옮긴다.
	 * 레벨 파일(다른 사람 맵)을 건드리지 않고 시작 위치를 정하려고 설정에 둔다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	bool bMovePlayer = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame", meta = (EditCondition = "bMovePlayer"))
	FVector PlayerLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame", meta = (EditCondition = "bMovePlayer"))
	float PlayerYaw = 0.f;

	/**
	 * 미니게임 동안 이 위치의 고정 카메라로 화면을 잡는다.
	 * 끄면 플레이어 3인칭 카메라를 그대로 쓴다 (플레이어 등 뒤에서 보게 된다).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Camera")
	bool bUseFixedCamera = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Camera", meta = (EditCondition = "bUseFixedCamera"))
	FVector CameraLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Camera", meta = (EditCondition = "bUseFixedCamera"))
	FRotator CameraRotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Camera", meta = (EditCondition = "bUseFixedCamera", ClampMin = "20", ClampMax = "150"))
	float CameraFOV = 90.f;

	/** 미니게임 동안 틀 배경음. 반복 재생은 사운드 애셋의 Looping 설정을 따른다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Sound")
	TSoftObjectPtr<USoundBase> BGM;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Sound", meta = (ClampMin = "0", ClampMax = "2"))
	float BGMVolume = 0.6f;

	/**
	 * 미니게임 위젯 뒤에 화면 전체로 깔 색. 알파가 0이면 깔지 않는다.
	 * 본편에서는 가게 맵 없이 지금 레벨에서 띄우므로, 반투명 패널 뒤가 검은 화면으로 비치지 않게 쓴다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	FLinearColor BackdropColor = FLinearColor::Transparent;

	/** 들어오기 전 레벨을 모를 때(이 레벨에서 바로 PIE를 켠 경우 등) 돌아갈 레벨 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	TSoftObjectPtr<UWorld> FallbackReturnLevel;

	/** 결과와 상관없이 주는 돈 (기획 확정값) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Reward")
	int32 MoneyReward = 40000;

	/** 결과에 따라 올려 줄 스탯 (EPlayerStat 이름) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Reward")
	FName RewardStat = TEXT("HandWork");

	/**
	 * 스탯 증가량 기준. 기본 1에서 점수가 기준을 하나 넘을 때마다 +1.
	 * 예: {3,5,7,9} → 0~2점 1, 3~4점 2, 5~6점 3, 7~8점 4, 9점 이상 5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame|Reward")
	TArray<int32> StatThresholds;
};

/** 미니게임이 끝났을 때 위젯이 넘기는 결과 */
USTRUCT(BlueprintType)
struct FMiniGameResult
{
	GENERATED_BODY()

	/** 보상 기준 점수. 햄버거는 완성한 주문 수 */
	UPROPERTY(BlueprintReadOnly, Category = "MiniGame")
	int32 Score = 0;

	/** 플레이하지 않고 건너뛰었다 (점수 0으로 정산, 플레이 횟수에 안 센다) */
	UPROPERTY(BlueprintReadOnly, Category = "MiniGame")
	bool bSkipped = false;
};

/** 실제로 지급한(또는 지급할) 보상. 결과 화면에 그대로 보여준다 */
USTRUCT(BlueprintType)
struct FMiniGameReward
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MiniGame")
	int32 Money = 0;

	UPROPERTY(BlueprintReadOnly, Category = "MiniGame")
	FName Stat;

	UPROPERTY(BlueprintReadOnly, Category = "MiniGame")
	int32 StatAmount = 0;
};
