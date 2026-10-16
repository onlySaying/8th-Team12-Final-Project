// [skydodo] 2026-09-01 작성 (최종 수정 2026-10-13)
// 요약: 행인 스포너 구현 - 풀 예열, 시작 인원 분산 배치, 시간대별 인원 보충
// Fill out your copyright notice in the Description page of Project Settings.

#include "NPC/SubNpc/NpcCrowdSpawner.h"

#include "AnimToTextureDataAsset.h"
#include "StateTree.h"
#include "Engine/GameInstance.h"
#include "Misc/ScopeExit.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameMode/TimeSubsystem.h"
#include "NPC/SubNpc/NpcCrowdPath.h"
#include "NPC/SubNpc/SubNpcCharacter.h"
#include "NPC/SubNpc/SubNpcDirectorSubsystem.h"
#include "NPC/SubNpc/SubNpcLog.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

ANpcCrowdSpawner::ANpcCrowdSpawner()
{
	// 인원 점검은 타이머로 돌린다. 매 프레임 할 이유가 전혀 없다.
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// 행인 Manny를 LOD별로 구운 데이터 (crowd.BakeFarCrowd, Docs/CrowdFarLOD.md).
	FarCrowdAnimData = TSoftObjectPtr<UAnimToTextureDataAsset>(
		FSoftObjectPath(TEXT("/Game/NPC/SubNpc/FarCrowd/DA_PasserbyFar.DA_PasserbyFar")));
}

UAnimToTextureDataAsset* ANpcCrowdSpawner::LoadFarCrowdAnimData() const
{
	return FarCrowdAnimData.IsNull() ? nullptr : FarCrowdAnimData.LoadSynchronous();
}

UStateTree* ANpcCrowdSpawner::LoadCrowdStateTree() const
{
	return CrowdStateTree.IsNull() ? nullptr : CrowdStateTree.LoadSynchronous();
}

void ANpcCrowdSpawner::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	USubNpcDirectorSubsystem* Director = World->GetSubsystem<USubNpcDirectorSubsystem>();
	if (!Director)
	{
		SUBNPC_LOG(Warning, TEXT("[Crowd] Director 서브시스템을 찾지 못했습니다."));
		return;
	}

	// [skydodo] 2026-10-17 추가: 행인 종류(남학생/여학생 등)를 섞어서 내보낸다. 비어 있으면 Npc Class 하나만 쓴다
	TArray<TSubclassOf<ASubNpcCharacter>> Classes;
	for (const TSubclassOf<ASubNpcCharacter>& Variant : NpcClassVariants)
	{
		if (Variant)
		{
			Classes.Add(Variant);
		}
	}
	if (Classes.Num() == 0 && NpcClass)
	{
		Classes.Add(NpcClass);
	}
	if (Classes.Num() == 0)
	{
		SUBNPC_LOG(Warning,
			TEXT("[Crowd] %s: Npc Class가 지정되지 않아 군중이 생성되지 않습니다."), *GetNameSafe(this));
		return;
	}

	Director->SetArchetypeTable(ArchetypeTable);
	Director->SetMaxActiveNpcs(FMath::Max(BaseTargetCount, PoolSize));

	// 풀은 여기서 한 번만 만든다. 이후 런타임에는 액터가 새로 생기지 않는다.
	// 종류가 여럿이면 같은 수만큼 만들고 순서를 섞는다 - 풀에서 꺼내는 순서가 곧 등장 순서라 몰려 나오지 않게 한다.
	if (Classes.Num() == 1)
	{
		Director->PrewarmPool(Classes[0], PoolSize);
	}
	else
	{
		TArray<int32> Order;
		Order.Reserve(PoolSize);
		for (int32 i = 0; i < PoolSize; ++i)
		{
			Order.Add(i % Classes.Num());
		}
		for (int32 i = Order.Num() - 1; i > 0; --i)
		{
			Order.Swap(i, FMath::RandRange(0, i));
		}
		for (const int32 ClassIndex : Order)
		{
			Director->PrewarmPool(Classes[ClassIndex], 1);
		}
	}

	// 입구 경로를 지정 안 했으면 레벨에서 자동으로 찾는다.
	if (EntryPaths.Num() == 0)
	{
		for (TActorIterator<ANpcCrowdPath> It(World); It; ++It)
		{
			if (It->IsSpawnEntry())
			{
				EntryPaths.Add(*It);
			}
		}

		if (EntryPaths.Num() == 0)
		{
			SUBNPC_LOG(Warning,
				TEXT("[Crowd] 입구 경로가 없습니다. NpcCrowdPath 액터 중 하나 이상에서 'Spawn Entry'를 켜세요."));
		}
	}

	// 시작 시점에 거리를 미리 채운다. 입구에서 90명이 걸어 나오는 그림을 피한다.
	if (bSpreadInitialPopulation)
	{
		SpreadInitialPopulation();
	}

	// 반복 타이머가 아니라 매번 다시 건다. 간격을 흔들려면 이 방법뿐이다.
	RearmSpawnTimer();
}

void ANpcCrowdSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 타이머를 안 지우면 레벨이 바뀐 뒤에도 죽은 액터를 향해 콜백이 날아간다.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SpawnTimerHandle);
	}

	EntryPaths.Reset();

	Super::EndPlay(EndPlayReason);
}

/** crowd.Bench가 측정 중에 정한 목표 인원. -1이면 쓰지 않는다 (SubNpcDirectorSubsystem.cpp) */
extern int32 GSubNpcBenchTargetOverride;

int32 ANpcCrowdSpawner::GetCurrentTargetCount() const
{
	if (GSubNpcBenchTargetOverride >= 0)
	{
		return GSubNpcBenchTargetOverride;
	}

	float Multiplier = 1.f;

	// 시간대 배율. 시간 시스템이 없으면 그냥 기본값을 쓴다.
	if (DensityBySchedule.Num() > 0)
	{
		if (const UWorld* World = GetWorld())
		{
			if (const UGameInstance* GI = World->GetGameInstance())
			{
				if (const UTimeSubsystem* Time = GI->GetSubsystem<UTimeSubsystem>())
				{
					const FName NowTag = Time->GetNowScheduleTag().GetTagName();
					for (const FCrowdDensityByHour& Entry : DensityBySchedule)
					{
						if (Entry.ScheduleTagName == NowTag)
						{
							Multiplier = Entry.DensityMultiplier;
							break;
						}
					}
				}
			}
		}
	}

	return FMath::Max(0, FMath::RoundToInt(BaseTargetCount * Multiplier));
}

ANpcCrowdPath* ANpcCrowdSpawner::PickEntryPath() const
{
	// 죽은 참조를 건너뛰며 랜덤으로 하나 고른다.
	TArray<ANpcCrowdPath*> Valid;
	Valid.Reserve(EntryPaths.Num());
	for (ANpcCrowdPath* Path : EntryPaths)
	{
		if (IsValid(Path))
		{
			Valid.Add(Path);
		}
	}

	if (Valid.Num() == 0)
	{
		return nullptr;
	}
	return Valid[FMath::RandRange(0, Valid.Num() - 1)];
}

int32 ANpcCrowdSpawner::RollGroupSize() const
{
	if (FMath::FRand() < SoloChance)
	{
		return 1;
	}
	// [그룹 동작] 무리는 2·3·5명만 만든다. 크기마다 동작이 달라서(SubNpcDirectorSubsystem의 GroupTalk/Point/Phone)
	// 멀리서도 무리가 구별된다. MaxGroupSize보다 큰 크기는 빼고 고른다.
	static constexpr int32 Sizes[] = { 2, 3, 5 };
	static constexpr float Weights[] = { 0.5f, 0.3f, 0.2f };
	float Total = 0.f;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Sizes); ++i)
	{
		Total += (Sizes[i] <= MaxGroupSize) ? Weights[i] : 0.f;
	}
	float Roll = FMath::FRand() * Total;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Sizes); ++i)
	{
		if (Sizes[i] > MaxGroupSize)
		{
			continue;
		}
		Roll -= Weights[i];
		if (Roll <= 0.f)
		{
			return Sizes[i];
		}
	}
	return 2;
}

bool ANpcCrowdSpawner::GetViewerLocation(FVector& OutLocation) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn())
		{
			OutLocation = Pawn->GetActorLocation();
			return true;
		}
	}
	return false;
}

void ANpcCrowdSpawner::RearmSpawnTimer()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// [중요] 월드가 정리되는 중이거나 이 액터가 죽는 중이면 타이머를 다시 걸지 않는다.
	//
	// 반복 타이머를 없애고 매번 다시 거는 방식으로 바꾸면서 생긴 위험이다.
	// EndPlay가 타이머를 지운 뒤에 마지막 TickSpawn이 빠져나오며 다시 걸면,
	// 타이머 매니저가 **이미 파괴된 액터를 가리킨 채 남는다.**
	// PIE를 끌 때 엉뚱한 곳에서 터지는 원인이 된다.
	if (!IsValid(this) || IsActorBeingDestroyed() || World->bIsTearingDown)
	{
		return;
	}

	// 간격을 매번 흔든다. 정확히 같은 간격으로 내보내면 사람들이 등간격으로
	// 줄지어 걷는 게 눈에 보인다 - 행렬처럼 느껴지는 원인 중 하나다.
	const float Base = FMath::Max(SpawnInterval, 0.1f);
	const float Jitter = FMath::Clamp(SpawnIntervalJitter, 0.f, 0.9f);
	const float Next = Base * FMath::FRandRange(1.f - Jitter, 1.f + Jitter);

	World->GetTimerManager().SetTimer(
		SpawnTimerHandle, this, &ANpcCrowdSpawner::TickSpawn, Next, /*bLoop=*/false);
}

void ANpcCrowdSpawner::SpreadInitialPopulation()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	USubNpcDirectorSubsystem* Director = World->GetSubsystem<USubNpcDirectorSubsystem>();
	if (!Director)
	{
		return;
	}

	// 레벨의 모든 경로를 모은다. 입구뿐 아니라 전 구간에 뿌려야
	// "이미 사람이 다니고 있는 거리"로 보인다.
	TArray<ANpcCrowdPath*> Paths;
	float TotalLength = 0.f;
	for (TActorIterator<ANpcCrowdPath> It(World); It; ++It)
	{
		// 입구 경로만이 아니라 모든 경로에 흩뿌린다. 연출 전용 맵(일반 행인이 없어야 하는 맵)은
		// 그 맵 스포너의 bSpreadInitialPopulation을 끈다.
		const float Len = It->GetPathLength();
		if (Len >= 200.f)
		{
			Paths.Add(*It);
			TotalLength += Len;
		}
	}

	if (Paths.Num() == 0 || TotalLength <= 0.f)
	{
		SUBNPC_LOG(Warning, TEXT("[Crowd] 초기 배치: 쓸 만한 경로가 없습니다."));
		return;
	}

	const int32 Target = GetCurrentTargetCount();
	int32 Remaining = Target;

	// [진단] 군중이 "뭉쳐 보이는" 가장 흔한 원인은 코드가 아니라 **경로가 짧은 것**이다.
	// 사람을 아무리 고르게 뿌려도 1미터에 한 명씩이면 그건 인파다.
	// 편하게 걷는 거리의 밀도는 대략 3미터에 한 명(0.3명/m) 수준이다.
	const float Density = Target / (TotalLength / 100.f);   // 명/m
	SUBNPC_LOG(Log,
		TEXT("[Crowd] 경로 총 길이 %.0fm / 목표 %d명 → 밀도 %.2f명/m"),
		TotalLength / 100.f, Target, Density);

	if (Density > 0.5f)
	{
		SUBNPC_LOG(Warning,
			TEXT("[Crowd] 밀도가 높습니다(%.2f명/m). 이 인원이 겹쳐 보이는 건 정상입니다. ")
			TEXT("경로를 더 길게 그리거나(권장 총 %.0fm 이상) 목표 인원을 %d명 이하로 줄이세요."),
			Density, Target / 0.3f / 1.f, FMath::RoundToInt(TotalLength / 100.f * 0.3f));
	}

	for (ANpcCrowdPath* Path : Paths)
	{
		if (Remaining <= 0)
		{
			break;
		}

		// 길이에 비례해 인원을 나눈다. 짧은 골목에 30명이 몰리지 않게.
		const float Len = Path->GetPathLength();
		const int32 Quota = (Path == Paths.Last())
			? Remaining
			: FMath::Min(Remaining, FMath::RoundToInt(Target * (Len / TotalLength)));

		if (Quota <= 0)
		{
			continue;
		}

		// 이 경로에 몇 무리를 세울지. 무리 하나의 평균 인원으로 나눈다.
		int32 Placed = 0;
		int32 GroupIndex = 0;
		const int32 EstimatedGroups = FMath::Max(1, Quota / FMath::Max(1, (MaxGroupSize + 1) / 2));
		const float Slot = Len / static_cast<float>(EstimatedGroups + 1);

		while (Placed < Quota && GroupIndex < EstimatedGroups * 2)
		{
			++GroupIndex;

			// 균등 배치 + 흔들기. 완전 랜덤으로 뿌리면 우연히 뭉치는 자리가 생기고,
			// 완전 균등이면 등간격으로 늘어선 게 눈에 보인다. 둘을 섞는다.
			const float Center = Slot * GroupIndex;
			const float StartDistance = FMath::Clamp(
				Center + FMath::FRandRange(-Slot * 0.4f, Slot * 0.4f), 0.f, Len);

			const int32 GroupSize = FMath::Min(RollGroupSize(), Quota - Placed);

			// [버그 수정] 예전에는 여기서 방향을 반반 랜덤으로 굴렸다.
			// "거리니까 양방향이 자연스럽다"고 생각했는데, 이 경로들은 방향이
			// 정해진 일방 경로다. 그래서 절반이 역주행하는 것처럼 보였다.
			// 경로가 정한 방향을 그대로 따른다.
			const bool bReversed = Path->IsSpawnReversed();

			const TArray<ASubNpcCharacter*> Spawned =
				(GroupSize <= 1)
				? TArray<ASubNpcCharacter*>{ Director->SpawnOnPath(Path, StartDistance, bReversed) }
				: Director->SpawnGroupOnPath(Path, StartDistance, bReversed, GroupSize);

			int32 Actual = 0;
			for (ASubNpcCharacter* Npc : Spawned)
			{
				if (Npc)
				{
					++Actual;
				}
			}

			if (Actual == 0)
			{
				// 풀이 비었거나 정원이 찼다. 더 세울 수 없다.
				Remaining = 0;
				break;
			}

			Placed += Actual;
		}

		Remaining -= Placed;
	}

	SUBNPC_LOG(Log, TEXT("[Crowd] 초기 배치 완료: 경로 %d개에 %d명"),
		Paths.Num(), Target - FMath::Max(0, Remaining));
}

void ANpcCrowdSpawner::TickSpawn()
{
	// 이 함수는 반복 타이머가 아니라 매번 다시 걸린다. 어디로 빠져나가든
	// 타이머를 다시 걸어야 하므로 스코프 가드로 보장한다.
	ON_SCOPE_EXIT{ RearmSpawnTimer(); };

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	USubNpcDirectorSubsystem* Director = World->GetSubsystem<USubNpcDirectorSubsystem>();
	if (!Director)
	{
		return;
	}

	const int32 Target = GetCurrentTargetCount();
	int32 Deficit = Target - Director->GetActiveCount();
	if (Deficit <= 0)
	{
		return;
	}

	// 한 주기에 한 무리씩만 내보내는 게 기본이다. 사람이 크게 모자랄 때만
	// (예: 시간대가 바뀌어 목표가 확 늘었을 때) 여러 무리까지 허용한다.
	const int32 GroupBudget = (Deficit > Target / 3)
		? FMath::Max(1, MaxGroupsPerTick)
		: 1;

	// 한 주기에 등장시킬 수 있는 인원 (MaxSpawnPerTick). 무리는 쪼개지 않으므로 첫 무리는 크기와 상관없이
	// 내보내고, 그 뒤 무리는 남은 인원 안에 들어갈 때만 내보낸다.
	int32 PersonBudget = FMath::Max(1, MaxSpawnPerTick);

	for (int32 g = 0; g < GroupBudget && Deficit > 0 && PersonBudget > 0; ++g)
	{
		ANpcCrowdPath* Entry = PickEntryPath();
		if (!Entry)
		{
			SUBNPC_LOG(Warning,
				TEXT("[Crowd] 보충 불가: 입구 경로가 없습니다. ")
				TEXT("NpcCrowdPath 중 하나 이상에서 'Spawn Entry'를 켜거나 ")
				TEXT("스포너의 Entry Paths에 직접 넣으세요. (목표 %d / 활성 %d)"),
				Target, Director->GetActiveCount());
			return;
		}

		const bool bReversed = Entry->IsSpawnReversed();
		const float Length = FMath::Max(1.f, Entry->GetPathLength());

		// 입구 경로의 "시작 쪽" 어딘가. 항상 같은 지점이면 한 자리에서 계속 튀어나온다.
		float StartDistance = bReversed
			? FMath::FRandRange(Length * 0.8f, Length)
			: FMath::FRandRange(0.f, Length * 0.2f);

		// 플레이어 눈앞에서 사람이 생기는 것을 되도록 피한다.
		//
		// [중요] 금지가 아니라 선호다. 후보를 몇 번 굴려보고 조건을 만족하는 게
		// 있으면 그걸 쓰고, 없으면 그중 가장 먼 자리를 쓴다. 조건을 못 채웠다고
		// 등장을 막으면 입구가 화면 안에 있는 레벨에서 군중이 아예 안 찬다.
		if (MinSpawnDistanceFromViewer > 0.f)
		{
			FVector ViewerLocation;
			if (GetViewerLocation(ViewerLocation))
			{
				const float MinDistSq = FMath::Square(MinSpawnDistanceFromViewer);

				float BestDistance = StartDistance;
				float BestDistSq = FVector::DistSquared(
					Entry->GetLocationAtDistance(StartDistance), ViewerLocation);

				for (int32 Attempt = 0; Attempt < 5 && BestDistSq < MinDistSq; ++Attempt)
				{
					const float TryDistance = bReversed
						? FMath::FRandRange(Length * 0.8f, Length)
						: FMath::FRandRange(0.f, Length * 0.2f);

					const float TryDistSq = FVector::DistSquared(
						Entry->GetLocationAtDistance(TryDistance), ViewerLocation);

					if (TryDistSq > BestDistSq)
					{
						BestDistSq = TryDistSq;
						BestDistance = TryDistance;
					}
				}

				StartDistance = BestDistance;
			}
		}

		const int32 GroupSize = FMath::Min(RollGroupSize(), Deficit);
		if (g > 0 && GroupSize > PersonBudget)
		{
			return;   // 이번 주기 몫을 다 썼다. 다음 주기에 나온다
		}
		const TArray<ASubNpcCharacter*> Spawned =
			(GroupSize <= 1)
			? TArray<ASubNpcCharacter*>{ Director->SpawnOnPath(Entry, StartDistance, bReversed) }
			: Director->SpawnGroupOnPath(Entry, StartDistance, bReversed, GroupSize);

		int32 Actual = 0;
		for (ASubNpcCharacter* Npc : Spawned)
		{
			if (Npc)
			{
				++Actual;
			}
		}

		if (Actual == 0)
		{
			// 풀이 비었거나 정원이 찼다. 다음 주기에 다시 시도한다.
			return;
		}

		Deficit -= Actual;
		PersonBudget -= Actual;
	}
}

void ANpcCrowdSpawner::FillImmediately()
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	USubNpcDirectorSubsystem* Director = World->GetSubsystem<USubNpcDirectorSubsystem>();
	if (!Director)
	{
		return;
	}

	const int32 Target = GetCurrentTargetCount();

	// 테스트용이므로 경로 전체에 고르게 뿌린다.
	int32 Guard = 0;
	while (Director->GetActiveCount() < Target && Guard++ < 500)
	{
		ANpcCrowdPath* Entry = PickEntryPath();
		if (!Entry)
		{
			return;
		}

		const float StartDistance = FMath::FRandRange(0.f, Entry->GetPathLength());
		if (!Director->SpawnOnPath(Entry, StartDistance, /*bReversed=*/false))
		{
			return;
		}
	}
}

void ANpcCrowdSpawner::PlayEventCrowd(FName Name)
{
	const FCrowdSequence* Sequence = EventCrowds.Find(Name);
	if (!Sequence)
	{
		SUBNPC_LOG(Warning, TEXT("[Crowd] %s 이름의 이벤트 행렬이 없습니다."), *Name.ToString());
		return;
	}
	if (!IsValid(Sequence->CrowdPath))
	{
		SUBNPC_LOG(Warning, TEXT("[Crowd] %s 이벤트 행렬의 경로가 지정되지 않았습니다."), *Name.ToString());
		return;
	}

	const UWorld* World = GetWorld();
	USubNpcDirectorSubsystem* Director = World ? World->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr;
	if (!Director)
	{
		return;
	}

	for (int32 i = 0; i < Sequence->SpawnCount; ++i)
	{
		const int32 Row = i / Sequence->NpcPerRow;
		const float Distance = Sequence->StartOffset + Row * Sequence->NpcGap;
		if (!Director->SpawnOnPath(Sequence->CrowdPath, Distance, /*bReversed=*/false))
		{
			SUBNPC_LOG(Warning, TEXT("[Crowd] %s 이벤트 행렬 스폰 실패 (%d/%d)"), *Name.ToString(), i, Sequence->SpawnCount);
			return;
		}
	}
}
