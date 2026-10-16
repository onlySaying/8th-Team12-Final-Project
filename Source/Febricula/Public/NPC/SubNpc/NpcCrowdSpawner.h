// [skydodo] 2026-09-01 작성 (최종 수정 2026-10-13)
// 요약: 거리에 행인이 끊기지 않게 채우는 스포너
// Fill out your copyright notice in the Description page of Project Settings.
//
// 거리에 사람이 끊기지 않게 채워 넣는 스포너.
//
// 레벨에 하나만 두면 된다. 하는 일:
//   1) 게임 시작 시 풀을 미리 채운다 (메타휴먼을 플레이 중에 만들지 않기 위해)
//   2) 주기적으로 "지금 목표 인원보다 적으면" 입구 경로에서 새 행인을 내보낸다
//   3) 시간대에 따라 목표 인원을 바꾼다 (UTimeSubsystem 연동 - 밤에는 한산하게)
//
// 스폰 자체는 USubNpcDirectorSubsystem이 하고, 여기는 "언제 몇 명" 만 결정한다.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NpcCrowdSpawner.generated.h"

class ANpcCrowdPath;
class ASubNpcCharacter;
class UAnimToTextureDataAsset;
class UDataTable;
class UStateTree;

/** 시간대별 인구 밀도 배율 */
USTRUCT(BlueprintType)
struct FCrowdDensityByHour
{
	GENERATED_BODY()

	/** 시간대 GameplayTag 이름 (예: Schedule.MorningClass). 비우면 기본값 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowd")
	FName ScheduleTagName;

	/** 목표 인원에 곱할 배율. 0이면 그 시간대엔 아무도 안 다닌다 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowd", meta = (ClampMin = "0.0"))
	float DensityMultiplier = 1.f;
};

/** 이벤트(StartCrowdSpawn)로 한 번에 내보낼 행렬 */
USTRUCT(BlueprintType)
struct FCrowdSequence
{
	GENERATED_BODY()

	/** 줄지어 걸을 경로 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd")
	TObjectPtr<ANpcCrowdPath> CrowdPath;

	/** 내보낼 인원 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd", meta = (ClampMin = 1))
	int32 SpawnCount = 20;

	/** 줄 사이 간격 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd", meta = (ClampMin = 1.f))
	float NpcGap = 120.f;

	/** 경로 시작점에서 첫 줄까지의 거리 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd", meta = (ClampMin = 0.f))
	float StartOffset = 0.f;

	/** 한 줄에 세울 인원 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd", meta = (ClampMin = 1))
	int32 NpcPerRow = 1;
};

UCLASS()
class FEBRICULA_API ANpcCrowdSpawner : public AActor
{
	GENERATED_BODY()

public:
	ANpcCrowdSpawner();

	/** 지금 목표 인원 (시간대 배율까지 반영한 값) */
	UFUNCTION(BlueprintPure, Category = "Crowd|Spawner")
	int32 GetCurrentTargetCount() const;

	/** 즉시 목표 인원까지 채운다 (테스트용). 플레이어 시야를 신경쓰지 않는다 */
	UFUNCTION(BlueprintCallable, Category = "Crowd|Spawner")
	void FillImmediately();

	/** EventCrowds에서 Name으로 찾은 행렬을 한 번에 내보낸다 (대사 액션 StartCrowdSpawn) */
	UFUNCTION(BlueprintCallable, Category = "Crowd|Spawner")
	void PlayEventCrowd(FName Name);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 주기적으로 인원을 확인하고 부족하면 채운다 */
	void TickSpawn();

	/** 입구로 쓸 수 있는 경로 중 하나를 고른다 */
	ANpcCrowdPath* PickEntryPath() const;

	/**
	 * 게임 시작 시 한 번, 레벨의 모든 경로에 사람을 흩어 놓는다.
	 *
	 * 입구 한 곳에서 90명을 뽑아내면 아무리 시차를 줘도 덩어리로 보인다.
	 * 시작 시점에는 "이미 거리에 사람이 있는" 상태여야 자연스럽다.
	 * 경로 길이에 비례해 인원을 나누고, 그 안에서 그룹 단위로 균등 배치 + 흔들기.
	 */
	void SpreadInitialPopulation();

	/** 다음 보충까지의 시간을 랜덤으로 굴려 타이머를 다시 건다 */
	void RearmSpawnTimer();

	/** 플레이어 폰 위치 (없으면 false) */
	bool GetViewerLocation(FVector& OutLocation) const;

	/** 스폰할 인원(단독/그룹)을 굴린다 */
	int32 RollGroupSize() const;

	/** 스폰할 클래스 (블루프린트로 만든 서브 NPC) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner")
	TSubclassOf<ASubNpcCharacter> NpcClass;

	// [skydodo] 2026-10-17 추가
	/** 섞어서 내보낼 행인 종류 (예: 남학생/여학생). 지정하면 Npc Class 대신 이 목록을 같은 수로 섞어 쓴다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner")
	TArray<TSubclassOf<ASubNpcCharacter>> NpcClassVariants;

	/**
	 * 미리 만들어둘 풀 크기. 목표 인원보다 넉넉해야 회수/재등장이 끊기지 않는다.
	 * 이 값이 곧 메모리 상한이다 - 여기서 정한 만큼만 메타휴먼이 존재한다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner", meta = (ClampMin = "0"))
	int32 PoolSize = 60;

	/** 평상시 유지할 활성 인원 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner", meta = (ClampMin = "0"))
	int32 BaseTargetCount = 30;

	/** 인원 점검 주기 (초). 아래 Jitter만큼 매번 흔들린다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner", meta = (ClampMin = "0.1"))
	float SpawnInterval = 1.f;

	/**
	 * 보충 주기의 흔들림 비율 (0~1).
	 *
	 * 정확히 같은 간격으로 내보내면 사람들이 등간격으로 줄지어 걷는 게 눈에 보인다.
	 * 0.5면 실제 간격이 SpawnInterval의 50%~150% 사이에서 매번 달라진다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner",
		meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float SpawnIntervalJitter = 0.5f;

	/**
	 * 한 주기에 내보낼 최대 그룹 수.
	 *
	 * 1이면 한 번에 한 무리씩만 등장한다. 이게 가장 자연스럽다.
	 * 인원이 크게 모자랄 때만 이 값까지 올려서 따라잡는다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner", meta = (ClampMin = "1"))
	int32 MaxGroupsPerTick = 1;

	/**
	 * 시작 시 레벨 전체에 미리 흩어 놓을지.
	 *
	 * 끄면 전원이 입구에서 걸어 들어온다. 사람이 다 찰 때까지 한참 걸리고,
	 * 그동안 입구 쪽만 붐빈다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner")
	bool bSpreadInitialPopulation = true;

	/**
	 * 보충 한 주기에 최대 몇 명까지 등장시킬지. 크면 한꺼번에 우르르 나타나 티가 난다.
	 * 무리는 쪼개지 않는다 - 첫 무리는 이보다 커도 통째로 나오고, 그다음 무리부터 이 한도를 본다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner", meta = (ClampMin = "1"))
	int32 MaxSpawnPerTick = 2;

	/**
	 * 플레이어와 이 거리(cm) 안에서는 등장시키지 않는다.
	 *
	 * 입구 구간이 화면 안에 있으면 보충이 일어날 때마다 눈앞에서 사람이 튀어나온다.
	 * 후보 지점을 몇 번 다시 굴려보고 끝내 못 찾으면 그 주기는 건너뛴다.
	 * 0이면 이 검사를 끈다 (예전 동작).
	 *
	 * 입구가 벽 뒤나 골목처럼 원래 안 보이는 곳이면 낮춰도 된다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner", meta = (ClampMin = "0.0"))
	float MinSpawnDistanceFromViewer = 2500.f;

	/** 아키타입 테이블 (FSubNpcArchetypeRow) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Spawner")
	TObjectPtr<UDataTable> ArchetypeTable;

	/**
	 * 입구로 쓸 경로들. 비워두면 레벨 전체에서 bSpawnEntry가 켜진 경로를 자동으로 찾는다.
	 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Crowd|Spawner")
	TArray<TObjectPtr<ANpcCrowdPath>> EntryPaths;

	// ---- 그룹 ----

	/** 일행 없이 혼자 다닐 확률 (0~1). 나머지는 2·3·5명 무리 (MaxGroupSize 이하만) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Group", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SoloChance = 0.4f;

	/** 무리 최대 인원. 무리는 2·3·5명 중에서 이 값 이하만 만든다 (5명 무리를 빼려면 3) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Group", meta = (ClampMin = "2"))
	int32 MaxGroupSize = 5;

	// ---- 시간대 연동 ----

	/**
	 * 시간대별 인구 배율. 비워두면 항상 BaseTargetCount를 유지한다.
	 * UTimeSubsystem이 없으면 이 설정은 무시된다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Time")
	TArray<FCrowdDensityByHour> DensityBySchedule;

	// ---- 이벤트 행렬 ----

	/** 대사 액션 StartCrowdSpawn의 Key2로 고르는 행렬 목록. 상시 인원과 별개로 한 번에 내보낸다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Event")
	TMap<FName, FCrowdSequence> EventCrowds;

	FTimerHandle SpawnTimerHandle;

public:
	/** 원경 표현에 쓸 애니메이션 데이터를 읽어 온다. 설정이 없거나 못 읽으면 nullptr (원경 표현을 쓰지 않는다) */
	UAnimToTextureDataAsset* LoadFarCrowdAnimData() const;

	/** 행인 상태 결정에 쓸 Mass StateTree를 읽어 온다. 비어 있으면 nullptr (예전처럼 액터가 스스로 정한다) */
	UStateTree* LoadCrowdStateTree() const;

protected:
	/**
	 * 행인의 상태 결정(걷다가 스팟에 들를지, 반응·스팟이 끝나면 무엇을 할지)을 맡을 Mass StateTree.
	 * 스키마는 "Mass Behavior"(MassStateTreeSchema)여야 하고, "행인:" 태스크들로 만든다.
	 * 비워 두면 예전처럼 액터 코드가 스스로 정한다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Behavior")
	TSoftObjectPtr<UStateTree> CrowdStateTree;

	/**
	 * 멀리 있는 행인을 그릴 인스턴스드 메시 애니메이션 데이터 (AnimToTexture).
	 *
	 * 걷기·대기 애니메이션을 텍스처로 구워 둔 데이터다. 스태틱 메시와 머티리얼은 이 데이터에 들어 있는 것을 쓴다.
	 * 애니메이션 이름에 "Walk"/"Idle"이 들어간 것을 걷기/대기로 쓴다.
	 * 기본값은 행인 Manny를 LOD별로 구운 DA_PasserbyFar다. 다시 구우려면 crowd.BakeFarCrowd (Docs/CrowdFarLOD.md).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|FarCrowd")
	TSoftObjectPtr<UAnimToTextureDataAsset> FarCrowdAnimData;
};
