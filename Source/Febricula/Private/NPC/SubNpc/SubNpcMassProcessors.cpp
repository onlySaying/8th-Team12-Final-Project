// [skydodo] 2026-10-07 작성 (최종 수정 2026-10-08)
// 요약: 행인 이동 Mass 프로세서 구현 - 경로 이동 병렬 계산, 원경 인스턴스 갱신
// Fill out your copyright notice in the Description page of Project Settings.

#include "NPC/SubNpc/SubNpcMassProcessors.h"

#include "AnimToTextureDataAsset.h"
#include "AnimToTextureInstancePlaybackHelpers.h"
#include "Async/ParallelFor.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MassExecutionContext.h"
#include "NPC/SubNpc/NpcCrowdSpawner.h"
#include "NPC/SubNpc/SubNpcCharacter.h"
#include "NPC/SubNpc/SubNpcLog.h"
#include "NPC/SubNpc/SubNpcProfiling.h"

int32 GSubNpcUseMass = 1;
static FAutoConsoleVariableRef CVarSubNpcUseMass(
	TEXT("crowd.Mass"),
	GSubNpcUseMass,
	TEXT("1이면 걷는 행인의 이동을 Mass 프로세서가 워커 스레드에서 병렬로 계산한다. 0이면 예전처럼 액터 틱에서 한 명씩 계산한다."),
	ECVF_Cheat);

namespace SubNpcMassTuning
{
	/**
	 * 워커 스레드 하나가 한 번에 맡는 행인 수.
	 * 한 명 계산이 수 µs라 너무 잘게 나누면 작업 분배 비용이 계산보다 커진다.
	 */
	constexpr int32 MinBatchSize = 4;
}

UMassSubNpcPathFollowProcessor::UMassSubNpcPathFollowProcessor()
	: EntityQuery(*this)
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::AllNetModes);
	ProcessingPhase = EMassProcessingPhase::PrePhysics;

	// 게임 스레드에서 실행하고 안에서 ParallelFor로 나눈다 (헤더 설명 참고).
	// 계산이 끝날 때까지 게임 스레드가 기다리므로, 액터 틱·Director와 같은 행인을 동시에 건드리지 않는다.
	bRequiresGameThreadExecution = true;
}

void UMassSubNpcPathFollowProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddRequirement<FSubNpcAgentFragment>(EMassFragmentAccess::ReadWrite);
}

void UMassSubNpcPathFollowProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	if (GSubNpcUseMass == 0)
	{
		return;
	}

	SUBNPC_PROFILE_SCOPE(MassPathFollow);

	struct FJob
	{
		ASubNpcCharacter* Npc = nullptr;
		float DeltaTime = 0.f;
		FSubNpcPathStep Step;
	};
	TArray<FJob> Jobs;

	// 1) 이번 프레임에 한 걸음 걸을 행인을 고른다 (게임 스레드).
	const float FrameDelta = Context.GetDeltaTimeSeconds();
	EntityQuery.ForEachEntityChunk(Context, [&Jobs, FrameDelta](FMassExecutionContext& ChunkContext)
	{
		const TArrayView<FSubNpcAgentFragment> Agents = ChunkContext.GetMutableFragmentView<FSubNpcAgentFragment>();
		for (FMassExecutionContext::FEntityIterator It = ChunkContext.CreateEntityIterator(); It; ++It)
		{
			FSubNpcAgentFragment& Agent = Agents[It];
			ASubNpcCharacter* Npc = Agent.Actor.Get();
			if (!Npc || !Npc->CanMassStep())
			{
				Agent.TimeSinceStep = 0.f;
				continue;
			}

			// 액터 틱 주기(LOD)를 지킨다. 멀리 있는 사람까지 매 프레임 계산하면 예전보다 오히려 비싸진다.
			Agent.TimeSinceStep += FrameDelta;
			if (Agent.TimeSinceStep < Npc->GetActorTickInterval())
			{
				continue;
			}

			FJob& Job = Jobs.AddDefaulted_GetRef();
			Job.Npc = Npc;
			Job.DeltaTime = Agent.TimeSinceStep;
			Agent.TimeSinceStep = 0.f;
		}
	});

	if (Jobs.Num() == 0)
	{
		return;
	}

	// 2) 계산 - 워커 스레드에서 병렬로. 각 행인은 자기 멤버만 바꾼다 (ComputePathStep 설명 참고).
	ParallelFor(TEXT("SubNpcPathCompute"), Jobs.Num(), SubNpcMassTuning::MinBatchSize, [&Jobs](int32 Index)
	{
		FJob& Job = Jobs[Index];
		Job.Npc->ComputePathStep(Job.DeltaTime, Job.Step);
	});

	// 3) 적용 - 게임 스레드에서 차례로 (위치 설정, 경로 갈아타기, 스팟 판정).
	for (FJob& Job : Jobs)
	{
		if (IsValid(Job.Npc))
		{
			Job.Npc->ApplyPathStep(Job.Step);
		}
	}
}

// ---------------------------------------------------------------------------
// 원경 표현
// ---------------------------------------------------------------------------

static int32 GSubNpcFarInstances = 1;
static FAutoConsoleVariableRef CVarSubNpcFarInstances(
	TEXT("crowd.FarInstances"),
	GSubNpcFarInstances,
	TEXT("1이면 멀리 있는 행인을 인스턴스드 메시(텍스처로 구운 애니메이션)로 그린다. 0이면 모두 스켈레탈 메시로 그린다."),
	ECVF_Cheat);

static float GSubNpcFarInstanceDistance = 4000.f;
static FAutoConsoleVariableRef CVarSubNpcFarInstanceDistance(
	TEXT("crowd.FarInstanceDistance"),
	GSubNpcFarInstanceDistance,
	TEXT("이 거리(cm)보다 멀리 있는 행인을 인스턴스드 메시로 그린다. 기본 4000 (Mid 밴드 끝). 작은 맵에서 시험할 때 낮춘다."),
	ECVF_Cheat);

namespace SubNpcFarCrowd
{
	/** 경계에서 액터와 인스턴스가 왔다 갔다 하지 않게, 돌아올 때는 이만큼(cm) 더 가까워져야 한다 */
	constexpr float Hysteresis = 300.f;

	/** 이보다 느리면(cm/s) 대기 애니메이션으로 바꾼다 */
	constexpr float IdleSpeedThreshold = 20.f;

	/** 구운 걷기 애니메이션의 원래 걸음 속도 (cm/s). 행인 걸음 속도와의 비율로 재생 속도를 정한다 */
	constexpr float BakedWalkSpeed = 150.f;

	// [skydodo] 2026-10-17 버그 수정: 전역 static FTransform이 Live Coding 패치 모듈에서 초기화되지 않은 채(회전 0,0,0,0) 쓰여
	// AddInstance의 IsRotationNormalized 검사에서 크래시. 쓸 때마다 만들어 돌려준다.
	/** 쓰지 않는 칸을 치워 두는 자리. 크기 0이라 보이지 않는다 */
	static FTransform HiddenTransform()
	{
		return FTransform(FQuat::Identity, FVector(0.f, 0.f, -100000.f), FVector::ZeroVector);
	}
}

ASubNpcFarCrowdInstancer::ASubNpcFarCrowdInstancer()
{
	PrimaryActorTick.bCanEverTick = false;

	Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("FarCrowdInstances"));
	SetRootComponent(Instances);
	Instances->SetMobility(EComponentMobility::Movable);
	Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Instances->SetCanEverAffectNavigation(false);
	// 멀리 있는 사람의 그림자는 거의 안 보이는데 비용은 크다 (가까운 행인도 Mid부터 그림자를 끈다).
	Instances->SetCastShadow(false);
}

bool ASubNpcFarCrowdInstancer::Initialize(UAnimToTextureDataAsset* InAnimData)
{
	if (!InAnimData)
	{
		return false;
	}
	UStaticMesh* Mesh = InAnimData->StaticMesh.LoadSynchronous();
	if (!Mesh || InAnimData->Animations.Num() == 0)
	{
		SUBNPC_LOG(Warning, TEXT("[Crowd] 원경 표현 데이터 %s 에 스태틱 메시나 구운 애니메이션이 없습니다. 원경 표현을 쓰지 않습니다."),
			*GetNameSafe(InAnimData));
		return false;
	}

	AnimData = InAnimData;
	Instances->SetStaticMesh(Mesh);
	Instances->ClearInstances();
	// 인스턴스마다 자동 재생 데이터(시간 오프셋, 재생 속도, 시작·끝 프레임)를 넣는다.
	Instances->NumCustomDataFloats = sizeof(FAnimToTextureAutoPlayData) / sizeof(float);

	// 걷기·대기 애니메이션 찾기. Animations는 켜져 있는(bEnabled) AnimSequences만 순서대로 담는다.
	int32 BakedIndex = 0;
	for (const FAnimToTextureAnimSequenceInfo& Info : InAnimData->AnimSequences)
	{
		if (!Info.bEnabled)
		{
			continue;
		}
		const FString Name = GetNameSafe(Info.AnimSequence);
		if (WalkAnimIndex < 0 && Name.Contains(TEXT("Walk")))
		{
			WalkAnimIndex = BakedIndex;
		}
		else if (IdleAnimIndex < 0 && Name.Contains(TEXT("Idle")))
		{
			IdleAnimIndex = BakedIndex;
		}
		++BakedIndex;
	}
	if (WalkAnimIndex < 0)
	{
		WalkAnimIndex = 0;
	}
	if (IdleAnimIndex < 0)
	{
		IdleAnimIndex = WalkAnimIndex;
	}

	SUBNPC_LOG(Log, TEXT("[Crowd] 원경 표현 준비: 메시 %s, 애니메이션 %d개 (걷기 %d, 대기 %d)"),
		*GetNameSafe(Mesh), InAnimData->Animations.Num(), WalkAnimIndex, IdleAnimIndex);
	return true;
}

int32 ASubNpcFarCrowdInstancer::AcquireSlot()
{
	if (FreeSlots.Num() > 0)
	{
		return FreeSlots.Pop(EAllowShrinking::No);
	}
	return Instances->AddInstance(SubNpcFarCrowd::HiddenTransform(), /*bWorldSpace=*/true);
}

void ASubNpcFarCrowdInstancer::ReleaseSlot(int32 Slot)
{
	if (Slot < 0)
	{
		return;
	}
	Instances->UpdateInstanceTransform(Slot, SubNpcFarCrowd::HiddenTransform(), /*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
	FreeSlots.Add(Slot);
}

UMassSubNpcRepresentationProcessor::UMassSubNpcRepresentationProcessor()
	: EntityQuery(*this)
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::AllNetModes);
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	bRequiresGameThreadExecution = true;   // 인스턴스드 메시·액터 컴포넌트를 바꾼다

	// 이동이 끝난 위치로 인스턴스를 옮겨야 한 프레임 늦게 따라가지 않는다.
	ExecutionOrder.ExecuteAfter.Add(UMassSubNpcPathFollowProcessor::StaticClass()->GetFName());
}

void UMassSubNpcRepresentationProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddRequirement<FSubNpcAgentFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FSubNpcRepresentationFragment>(EMassFragmentAccess::ReadWrite);
}

UAnimToTextureDataAsset* UMassSubNpcRepresentationProcessor::GetDefaultAnimData(UWorld* World)
{
	if (!bDefaultLookedUp && World)
	{
		bDefaultLookedUp = true;
		for (TActorIterator<ANpcCrowdSpawner> It(World); It; ++It)
		{
			DefaultAnimData = It->LoadFarCrowdAnimData();
			break;
		}
	}
	return DefaultAnimData.Get();
}

ASubNpcFarCrowdInstancer* UMassSubNpcRepresentationProcessor::GetOrCreateInstancer(UWorld* World, UAnimToTextureDataAsset* AnimData)
{
	if (!World || !AnimData || UnavailableData.Contains(AnimData))
	{
		return nullptr;
	}
	if (const TWeakObjectPtr<ASubNpcFarCrowdInstancer>* Found = Instancers.Find(AnimData))
	{
		if (ASubNpcFarCrowdInstancer* Existing = Found->Get())
		{
			return Existing;
		}
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	ASubNpcFarCrowdInstancer* NewInstancer = World->SpawnActor<ASubNpcFarCrowdInstancer>(Params);
	if (!NewInstancer || !NewInstancer->Initialize(AnimData))
	{
		if (NewInstancer)
		{
			NewInstancer->Destroy();
		}
		UnavailableData.Add(AnimData);
		SUBNPC_LOG(Log, TEXT("[Crowd] 원경 데이터 %s 를 쓸 수 없어 이 행인들은 멀리서도 스켈레탈 메시로 그립니다."), *GetNameSafe(AnimData));
		return nullptr;
	}
	Instancers.Add(AnimData, NewInstancer);
	return NewInstancer;
}

void UMassSubNpcRepresentationProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	UWorld* World = EntityManager.GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// 기준점: 카메라 위치 (3인칭이라 캐릭터보다 카메라 거리가 화면과 맞는다).
	FVector Viewer = FVector::ZeroVector;
	bool bHasViewer = false;
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager)
		{
			Viewer = PC->PlayerCameraManager->GetCameraLocation();
			bHasViewer = true;
		}
	}

	const bool bEnabled = GSubNpcFarInstances != 0 && bHasViewer;
	// 꺼져 있고 빌려 간 칸도 없으면 할 일이 없다
	if (!bEnabled && Instancers.Num() == 0)
	{
		return;
	}
	UAnimToTextureDataAsset* DefaultData = bEnabled ? GetDefaultAnimData(World) : nullptr;

	const float EnterSq = FMath::Square(GSubNpcFarInstanceDistance);
	const float LeaveSq = FMath::Square(FMath::Max(0.f, GSubNpcFarInstanceDistance - SubNpcFarCrowd::Hysteresis));
	bool bChanged = false;

	EntityQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& ChunkContext)
	{
		const TConstArrayView<FSubNpcAgentFragment> Agents = ChunkContext.GetFragmentView<FSubNpcAgentFragment>();
		const TArrayView<FSubNpcRepresentationFragment> Reps = ChunkContext.GetMutableFragmentView<FSubNpcRepresentationFragment>();

		for (FMassExecutionContext::FEntityIterator It = ChunkContext.CreateEntityIterator(); It; ++It)
		{
			ASubNpcCharacter* Npc = Agents[It].Actor.Get();
			FSubNpcRepresentationFragment& Rep = Reps[It];
			const bool bIsInstanced = Rep.InstanceIndex >= 0;

			// 인스턴스로 그릴까? 풀에 있거나 숨겨졌거나(퇴장 끝) 사라지는 중이면 액터 쪽이다.
			bool bWant = bEnabled && IsValid(Npc) && !Npc->IsPooled() && !Npc->IsHidden() && !Npc->IsExitFading();
			if (bWant)
			{
				const float DistSq = FVector::DistSquared(Npc->GetActorLocation(), Viewer);
				bWant = DistSq > (bIsInstanced ? LeaveSq : EnterSq);
			}

			if (bWant && !bIsInstanced)
			{
				// 행인 종류마다 원경 메시가 다를 수 있어, 그 행인의 데이터로 인스턴서를 고른다
				UAnimToTextureDataAsset* NpcData = Npc->GetFarCrowdAnimData();
				ASubNpcFarCrowdInstancer* Target = GetOrCreateInstancer(World, NpcData ? NpcData : DefaultData);
				if (Target)
				{
					Rep.Instancer = Target;
					Rep.InstanceIndex = Target->AcquireSlot();
					Rep.AnimIndex = -1;
					Rep.TimeOffset = FMath::FRandRange(0.f, 10.f);
					Npc->SetInstancedRepresentation(true);
				}
			}
			else if (!bWant && bIsInstanced)
			{
				if (ASubNpcFarCrowdInstancer* Owner = Rep.Instancer.Get())
				{
					Owner->ReleaseSlot(Rep.InstanceIndex);
				}
				Rep.InstanceIndex = -1;
				Rep.Instancer.Reset();
				if (IsValid(Npc) && !Npc->IsPooled())
				{
					Npc->SetInstancedRepresentation(false);
				}
				bChanged = true;
			}

			ASubNpcFarCrowdInstancer* FarCrowd = Rep.Instancer.Get();
			UInstancedStaticMeshComponent* Instances = FarCrowd ? FarCrowd->GetInstances() : nullptr;
			if (Rep.InstanceIndex < 0 || !Instances)
			{
				continue;
			}

			// 위치: 스켈레탈 메시 컴포넌트 자리 그대로 (발 위치·방향·체격 배율이 이미 들어 있다).
			Instances->UpdateInstanceTransform(Rep.InstanceIndex, Npc->GetMesh()->GetComponentTransform(),
				/*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
			bChanged = true;

			// 애니메이션: 걷는 중이면 걷기(걸음 속도에 맞춘 재생 속도), 서 있으면 대기.
			// 재생 속도를 감속에 따라 매번 바꾸면 박자가 튀므로 걷기/대기가 바뀔 때만 다시 넣는다.
			const bool bMoving = Npc->GetAnimSpeed() > SubNpcFarCrowd::IdleSpeedThreshold;
			const int32 Anim = bMoving ? FarCrowd->GetWalkAnimIndex() : FarCrowd->GetIdleAnimIndex();
			if (Anim != Rep.AnimIndex)
			{
				const float PlayRate = bMoving ? Npc->GetWalkSpeed() / SubNpcFarCrowd::BakedWalkSpeed : 1.f;
				FAnimToTextureAutoPlayData AutoPlay;
				if (UAnimToTextureInstancePlaybackLibrary::GetAutoPlayDataFromDataAsset(FarCrowd->GetAnimData(), Anim, AutoPlay, Rep.TimeOffset, PlayRate))
				{
					UAnimToTextureInstancePlaybackLibrary::UpdateInstanceAutoPlayData(Instances, Rep.InstanceIndex, AutoPlay, /*bMarkRenderStateDirty=*/false);
				}
				Rep.AnimIndex = Anim;
			}
		}
	});

	// [버그 수정] 여기서 MarkRenderStateDirty()를 부르지 않는다.
	// 인스턴스 위치·데이터를 바꾸면 엔진(FPrimitiveInstanceDataManager)이 바뀐 인스턴스만 렌더 쪽에 보낸다.
	// 예전에는 매 프레임 MarkRenderStateDirty()로 인스턴스 메시 전체를 처음부터 다시 만들어서,
	// 원경을 켜면 오히려 60 FPS → 22~31 FPS로 떨어졌다.
	(void)bChanged;
}
