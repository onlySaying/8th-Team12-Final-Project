// [skydodo] 2026-10-08 작성
// 요약: 메인 NPC 스포너 + 스케줄 행(FMainNpcSpawnRow) - 그 시간·레벨·자리에 한 명만 세운다
// Fill out your copyright notice in the Description page of Project Settings.
//
// 메인 NPC를 "그 시간에, 그 레벨의, 그 자리에" 한 명만 세워 두는 스포너.
//
// 레벨마다 하나씩 두고, 모든 레벨의 스포너가 같은 스케줄 테이블(FMainNpcSpawnRow)을 본다.
// 자리는 레벨에 배치한 AMainNpcSpawnPoint의 ID로 가리킨다.
//
// [한 NPC는 한 곳에만]
//   NPC마다 "지금 조건에 맞는 행" 중 하나만 고른다 (조건 있는 행 > 우선순위 > 시간대 지정 행 > 테이블 순서).
//   어느 레벨의 스포너든 같은 테이블로 같은 계산을 하므로 같은 행이 뽑히고,
//   그 행의 레벨에 있는 스포너만 실제로 스폰한다. 그래서 두 레벨에 동시에 나올 수 없다.
//
// [호감도/진행에 따라 자리가 바뀐다]
//   행마다 조건(FGameCondition)을 붙일 수 있다. 같은 날 같은 시간에
//     - 조건 없음              -> 교실 (기본 자리)
//     - 조건: 관계 신뢰 이상   -> 옥상
//   처럼 적어 두면 호감도가 오르면 옥상에, 아니면 교실에 서 있는다.
//   둘 다 맞으면 항상 조건 있는 쪽이 이긴다. 조건 행끼리 동시에 맞을 때만 Priority를 본다.
//
// [언제 다시 고르는가]
//   레벨 진입 시, 그리고 일차/시간대가 바뀔 때. 대화/동행/안내/추적 중인 NPC는 끝날 때까지 기다렸다가 옮긴다.
//
// [자리별 행동]
//   행마다 시작 애니메이션(StartAnimId)과 Blackboard AIState(StartAIState, Patrol/Guide는 RouteId)를 준다.
//   같은 NPC라도 자리/상황에 따라 다른 모습으로 서 있게 하려는 것이다.

#pragma once

#include "CoreMinimal.h"
#include "Dialogue/DialogueDataTypes.h"
#include "Engine/DataTable.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "SpawnPointType.h"
#include "NPC/NpcTypes.h"
#include "MainNpcSpawner.generated.h"

class AMainNpcCharacter;
class AMainNpcSpawnPoint;
class UGameProgressSubsystem;

/** 메인 NPC 스케줄 한 줄: 누가(NpcId) 언제(Day + 시간대) 어떤 조건이면 어느 레벨 어느 자리에 서 있는가 */
USTRUCT(BlueprintType)
struct FMainNpcSpawnRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Behavior")
	bool bSeated = false;
	
	/** 메인 NPC 고유 ID (예: NPC_HanSeoYun). 스폰한 NPC에도 이 ID를 넣는다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc")
	FName NpcId;

	/** 스폰할 메인 NPC 블루프린트 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc")
	TSoftClassPtr<AMainNpcCharacter> NpcClass;

	/** 누적 일차 (UTimeSubsystem::GetAbsoluteDay 기준, 1일차부터) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Time", meta = (ClampMin = "0"))
	int32 Day = 0;

	/** 시간대 태그 (예: Schedule.MorningClass). 비우면 그 날 하루 종일 */
		
	/** 이지민 수정(2026. 10. 10)
	 * FGameplayTagContainer를 이용해 여러 개 스케쥴을 한꺼번에 필터링
	*/
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Time")
	FGameplayTagContainer ScheduleTag;



	/** 스폰할 레벨 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Location")
	TSoftObjectPtr<UWorld> Level;

		
	/** 이지민 수정(2026. 10. 10)
	 * 여러 후보 Point 중 아무데나 한 곳 랜덤으로 골라서 스폰하게 만들기 위해 추가
	 */
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Location")
	ESpawnPointType	SpawnPointType = ESpawnPointType::OneByOne;
	
	/** 그 레벨에 배치한 AMainNpcSpawnPoint의 SpawnPointId */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Location", meta = (EditCondition = "SpawnPointType != ESpawnPointType::MultiByRandom"))
	FName SpawnPointId;
	
	/** 이 행이 쓰이기 위한 조건 (전부 AND). 호감도는 "관계 O 이상"(Key=NpcId)으로 건다. 비우면 항상 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Condition")
	TArray<FGameCondition> Conditions;

	/**
	 * 조건 있는 행끼리 동시에 맞을 때 큰 값이 이긴다 (예: 신뢰 이상 0, 친밀 이상 10).
	 * 조건 없는 기본 행은 조건 행에 항상 지므로 이 값을 줄 필요가 없다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Condition")
	int32 Priority = 0;

	/**
	 * 이 자리에서 반복 재생할 애니메이션 (NPC AnimLibrary의 키, 예: Sit_Read).
	 * 비우면 블루프린트의 DefaultAnimation을 그대로 쓴다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Behavior")
	FName StartAnimId;

	/** 스폰 직후 Blackboard AIState. Idle이면 제자리에 서 있는다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Behavior")
	ENpcAIState StartAIState = ENpcAIState::Idle;

	/**
	 * Patrol/Guide가 따라갈 경로. 레벨의 ANpcRoute에 같은 이름의 Actor Tag를 달아 두면 된다
	 * (AMainNpcCharacter::GetRoute). Chase는 경로 없이 플레이어를 쫓는다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Behavior",
		meta = (EditCondition = "StartAIState == ENpcAIState::Patrol || StartAIState == ENpcAIState::Guide"))
	FName RouteId;
	
	//[이지민] 2026-10-10 추가: 확률적 스폰
	/**
	 * 해당 변수는 캐릭터가 스폰될 확률을 의미한다.
	 */
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Spawn Rate", meta = (ClampMin = "0", ClampMax = "100") )
	float SpawnRate = 100.f;

	// [skydodo] 2026-10-08 추가: 자리(상황)별 대화
	/**
	 * 이 자리에 서 있을 때 말을 걸면 고를 이벤트 테이블 (FNpcEventRow).
	 * 이벤트의 전제조건(일차/호감도/시간대 등)으로 대사가 갈린다. 비우면 블루프린트에 설정된 테이블을 쓴다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Dialogue",
		meta = (RequiredAssetDataTags = "RowStructure=/Script/Febricula.NpcEventRow"))
	TSoftObjectPtr<UDataTable> EventTable;

	/** 기본 대화 테이블 (FDialogueNodeRow). 이벤트 행이 대화 테이블을 따로 지정하지 않을 때 쓴다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Dialogue",
		meta = (RequiredAssetDataTags = "RowStructure=/Script/Febricula.DialogueNodeRow"))
	TSoftObjectPtr<UDataTable> DialogueTable;
};

UCLASS()
class FEBRICULA_API AMainNpcSpawner : public AActor
{
	GENERATED_BODY()

public:
	AMainNpcSpawner();

	/** 이 스포너가 스폰한 NPC 중 NpcId가 같은 것. 없으면 nullptr */
	UFUNCTION(BlueprintPure, Category = "MainNpc|Spawner")
	AMainNpcCharacter* FindSpawnedNpc(FName NpcId) const;

	/**
	 * 지금 상태로 NPC 배치를 다시 계산한다 (다음 틱에 한 번).
	 * 일차/시간대 변경 때는 자동으로 불린다. 호감도나 이벤트 완료 직후 바로 반영하고 싶으면 직접 부른다.
	 */
	UFUNCTION(BlueprintCallable, Category = "MainNpc|Spawner")
	void RequestRefresh();

	/**
	 * 스케줄 테이블 검사. 같은 NPC가 같은 날 같은 시간대, 같은 우선순위, 조건 없이 두 행 이상이면
	 * 뒤쪽 행이 절대 쓰이지 않으므로 경고를 남기고 false.
	 */
	UFUNCTION(BlueprintCallable, Category = "MainNpc|Spawner")
	bool ValidateSchedule() const;
	
	
	UDataTable* GetScheduleTable() const { return ScheduleTable; }

	/** 디버그 출력용: 지금 이 레벨에 세운 NPC와 자리/애니메이션/AIState를 한 줄씩 */
	TArray<FString> DescribeSpawnedNpcs() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// [skydodo] 2026-10-13 리팩토링: 쓰이지 않던 RandomStream 멤버 제거 (확률 판정은 UGameProgressSubsystem::MakeRandomStream이 매번 만든다)

	/** NPC마다 지금 쓸 행을 고르고, 이 레벨 몫을 스폰/이동/제거한다 */
	void RefreshNpcs();

	/** NPC별로 지금 조건에 맞는 행 하나씩 (레벨과 무관하게 전체에서 고른다) */
	TMap<FName, const FMainNpcSpawnRow*> ResolveCurrentRows() const;

	/** A가 B보다 우선하는가 */
	static bool IsRowPreferred(const FMainNpcSpawnRow& A, const FMainNpcSpawnRow& B);

	AMainNpcCharacter* SpawnFromRow(const FMainNpcSpawnRow& Row);

	/** 행의 StartAIState를 Blackboard에 넣는다 (Patrol/Guide는 경로까지) */
	void ApplyStartAIState(AMainNpcCharacter* Npc, const FMainNpcSpawnRow& Row) const;
	void DespawnNpc(FName NpcId);

	/** 대화/동행/상호작용 중이라 지금 옮기면 안 되는가 */
	static bool IsNpcBusy(const AMainNpcCharacter* Npc);

	/** 레벨에 이미 같은 NpcId의 메인 NPC가 있는지 (직접 배치한 것 포함) */
	AMainNpcCharacter* FindNpcInWorld(FName NpcId) const;

	TArray<AMainNpcSpawnPoint*> FindSpawnPoint(FName SpawnPointId, ESpawnPointType PointType) const;

	/** 이 행이 지금 레벨에 해당하는지 */
	bool IsRowForThisLevel(const FMainNpcSpawnRow& Row) const;

	UFUNCTION()
	void HandleDayChanged(int32 ChangedDay);

	UFUNCTION()
	void HandleScheduleChanged(FGameplayTag ChangedScheduleName);

	/** 바쁜 NPC 때문에 미뤄둔 갱신이 있으면, 그 NPC가 한가해질 때 다시 돈다 */
	UFUNCTION()
	void HandleNpcMovementStateChanged(ENpcMovementState OldState, ENpcMovementState NewState);

	/**
	 * 메인 NPC 스케줄 테이블 (FMainNpcSpawnRow).
	 * 모든 레벨의 스포너가 같은 테이블을 가리켜야 "한 NPC는 한 곳에만" 이 보장된다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Spawner",
		meta = (RequiredAssetDataTags = "RowStructure=/Script/Febricula.MainNpcSpawnRow"))
	TObjectPtr<UDataTable> ScheduleTable;

	/** 레벨 진입 시 스케줄 테이블 검사를 돌릴지 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainNpc|Spawner")
	bool bValidateOnBeginPlay = true;

	/** 이 스포너가 스폰한 NPC들 (NpcId -> NPC) */
	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<AMainNpcCharacter>> SpawnedNpcs;

	/** 스폰한 NPC가 따르고 있는 행 (NpcId -> 행). 테이블 행은 플레이 중 바뀌지 않으므로 포인터로 들고 있는다 */
	TMap<FName, const FMainNpcSpawnRow*> SpawnedRows;
	
	/** 이지민 수정(2026. 10. 10)
	 * 레벨에 같은 Type을 가진 여러 개의 Point 중 랜덤하게 하나 골라 스폰하기 위해 추가
	 */
	// [skydodo] 2026-10-13 리팩토링: 런타임 기록이라 Transient (SpawnedNpcs와 같게). 채우기만 하고 안 읽던 AllSpawnPoints는 제거
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "MainNpc|Spawner|Debug")
	TMap<FName, TObjectPtr<AMainNpcSpawnPoint>> SpawnedPoints;

	FTimerHandle RefreshTimerHandle;

	/** 바쁜 NPC 때문에 미룬 갱신을 다시 시도하는 타이머 */
	FTimerHandle DeferredRetryHandle;

	/** 바쁜 NPC 때문에 이번 갱신을 다 못 했다 */
	bool bRefreshDeferred = false;
};
