// [skydodo] 2026-10-07 작성
// 요약: 행인 이동 Mass 1단계 - 경로 이동 병렬 계산 프로세서
// Fill out your copyright notice in the Description page of Project Settings.
//
// 행인 이동의 Mass 1단계.
//
// [하는 일]
//   걷는 행인의 경로 이동 계산(ASubNpcCharacter::ComputePathStep)을 워커 스레드에서 여러 명 동시에 돌리고,
//   결과 적용(ApplyPathStep - 위치 설정·경로 갈아타기·스팟 판정)만 게임 스레드에서 한다.
//   예전에는 행인마다 자기 액터 틱에서 계산부터 적용까지 게임 스레드로 했다.
//
// [아직 액터에 남아 있는 것]
//   이동 상태(진행도·차선·속도)는 그대로 액터 멤버에 있다. 엔티티는 액터를 가리키기만 한다.
//   스팟·반응·대화·일행·회피(Director)도 지금 코드 그대로다. 단계적으로 옮긴다:
//     1) 이동 계산 병렬화 (이 파일)   2) 회피·겹침 금지   3) StateTree   4) 표현 LOD(인스턴스드 메시)
//
// [켜고 끄기]
//   crowd.Mass 1(기본) / 0 - 0이면 예전처럼 액터 틱이 이동한다. crowd.Bench로 전후를 비교한다.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassEntityTypes.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"	// [skydodo] 2026-10-13 버그 수정: 에디터에선 다른 헤더가 대신 끌어와서 몰랐는데, Shipping에선 FMassEntityQuery가 정의되지 않아 컴파일 에러
#include "SubNpcMassProcessors.generated.h"

class ASubNpcCharacter;
class UAnimToTextureDataAsset;
class UInstancedStaticMeshComponent;

/** 이 엔티티가 움직이는 행인 액터 */
USTRUCT()
struct FSubNpcAgentFragment : public FMassFragment
{
	GENERATED_BODY()

	TWeakObjectPtr<ASubNpcCharacter> Actor;

	/**
	 * 마지막 걸음 이후 쌓인 시간 (초).
	 * 액터 틱 주기(LOD: 가까움 매 프레임, 중간 0.1초, 멀리 0.3초)를 그대로 지키려고 쓴다.
	 * 프로세서는 매 프레임 돌지만, 이 값이 액터 틱 주기를 넘을 때만 한 걸음 계산한다.
	 */
	float TimeSinceStep = 0.f;
};

/**
 * 걷는 행인의 경로 이동을 병렬로 계산하고 게임 스레드에서 적용한다.
 *
 * 이 프로세서는 게임 스레드에서 실행되고(bRequiresGameThreadExecution), 안에서 ParallelFor로
 * 워커 스레드에 나눠 준다. 그래서 계산하는 동안 게임 스레드의 다른 코드(액터 틱, Director)가
 * 같은 액터를 동시에 건드릴 일이 없다.
 *
 * ParallelForEachEntityChunk를 쓰지 않는 이유: 프래그먼트가 작아 행인 100명이 청크 하나에
 * 다 들어가서, 청크 단위 병렬로는 일이 한 스레드에 몰린다. 엔티티 단위로 나눠야 실제로 퍼진다.
 */
UCLASS()
class FEBRICULA_API UMassSubNpcPathFollowProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UMassSubNpcPathFollowProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;
};

// ---------------------------------------------------------------------------
// 원경 표현 (Mass 2단계)
//
// 측정해 보니 군중 비용의 대부분은 로직이 아니라 스켈레탈 메시의 애니메이션(1명당 약 42µs)과
// 렌더링(GPU 약 23µs)이었다. 멀리 있는 사람은 스켈레탈 메시를 숨기고, 걷기 애니메이션을 텍스처로
// 구워 둔 인스턴스드 스태틱 메시(AnimToTexture)로 대신 그린다. 수백 명이 드로우콜 하나로 그려지고
// 애니메이션 계산은 GPU 머티리얼이 한다.
//
//   crowd.FarInstances 1(기본) / 0          켜고 끄기
//   crowd.FarInstanceDistance 4000         이 거리(cm)보다 멀면 인스턴스로 그린다. 작은 맵에서 시험할 때 낮춘다
// ---------------------------------------------------------------------------

class ASubNpcFarCrowdInstancer;

/** 이 엔티티가 원경 인스턴스로 그려지고 있으면 그 칸과 재생 중인 애니메이션 */
USTRUCT()
struct FSubNpcRepresentationFragment : public FMassFragment
{
	GENERATED_BODY()

	/** 인스턴스드 메시의 칸 번호. -1이면 액터(스켈레탈 메시)로 그려지는 중 */
	int32 InstanceIndex = -1;

	/** 지금 재생 중인 애니메이션 (데이터 에셋의 Animations 인덱스). -1이면 아직 정하지 않음 */
	int32 AnimIndex = -1;

	/** 사람마다 걸음 박자를 다르게 하는 시간 오프셋 (초). 다 같이 발을 맞추면 행진처럼 보인다 */
	float TimeOffset = 0.f;

	/** 칸을 빌린 인스턴서. 행인 종류마다 원경 메시가 달라 인스턴서가 여러 개일 수 있다 */
	TWeakObjectPtr<ASubNpcFarCrowdInstancer> Instancer;
};

/**
 * 원경 행인을 그리는 인스턴스드 메시 하나를 들고 있는 액터. 원경 데이터(행인 종류)마다 하나, 처음 필요할 때 만든다.
 * 칸은 지우지 않고 재사용한다 - 인스턴스를 지우면 뒤 칸 번호가 당겨져서 다른 사람 칸이 바뀐다.
 */
UCLASS(Transient, NotPlaceable)
class FEBRICULA_API ASubNpcFarCrowdInstancer : public AActor
{
	GENERATED_BODY()

public:
	ASubNpcFarCrowdInstancer();

	/** 애니메이션 데이터로 메시·머티리얼을 준비한다. 데이터가 쓸 수 없으면 false */
	bool Initialize(UAnimToTextureDataAsset* InAnimData);

	/** 빈 칸 하나를 얻는다 (없으면 새로 만든다) */
	int32 AcquireSlot();

	/** 칸을 돌려준다. 안 보이게 치워 두고 다음에 재사용한다 */
	void ReleaseSlot(int32 Slot);

	UInstancedStaticMeshComponent* GetInstances() const { return Instances; }
	UAnimToTextureDataAsset* GetAnimData() const { return AnimData; }
	int32 GetWalkAnimIndex() const { return WalkAnimIndex; }
	int32 GetIdleAnimIndex() const { return IdleAnimIndex; }

private:
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	UPROPERTY()
	TObjectPtr<UAnimToTextureDataAsset> AnimData;

	int32 WalkAnimIndex = -1;
	int32 IdleAnimIndex = -1;
	TArray<int32> FreeSlots;
};

/**
 * 거리에 따라 행인을 액터(스켈레탈 메시) ↔ 원경 인스턴스로 바꾸고, 인스턴스 위치·애니메이션을 갱신한다.
 * 인스턴스 갱신은 게임 스레드 일이라 게임 스레드에서 실행한다. 이동 프로세서 뒤에 돈다(같은 프레임 위치를 쓴다).
 */
UCLASS()
class FEBRICULA_API UMassSubNpcRepresentationProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UMassSubNpcRepresentationProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	/** 이 데이터로 그리는 원경 인스턴서를 찾거나 만든다. 데이터를 쓸 수 없으면 nullptr (그 행인은 원경 표현을 쓰지 않는다) */
	ASubNpcFarCrowdInstancer* GetOrCreateInstancer(UWorld* World, UAnimToTextureDataAsset* AnimData);

	/** 행인이 원경 데이터를 따로 정하지 않았을 때 쓰는 스포너의 기본값 */
	UAnimToTextureDataAsset* GetDefaultAnimData(UWorld* World);

	FMassEntityQuery EntityQuery;

	// [skydodo] 2026-10-17 수정: 인스턴서 하나 → 원경 데이터마다 하나 (행인 종류마다 메시가 다르다)
	TMap<TWeakObjectPtr<UAnimToTextureDataAsset>, TWeakObjectPtr<ASubNpcFarCrowdInstancer>> Instancers;

	/** 쓸 수 없는 데이터는 매 프레임 다시 시도하지 않는다 */
	TSet<TWeakObjectPtr<UAnimToTextureDataAsset>> UnavailableData;

	TWeakObjectPtr<UAnimToTextureDataAsset> DefaultAnimData;
	bool bDefaultLookedUp = false;
};
