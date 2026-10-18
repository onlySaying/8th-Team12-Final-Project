// [skydodo] 2026-10-18 작성
// 요약: 시연 영상 녹화용 자동 플레이 구현

#include "Demo/DemoAutoPlaySubsystem.h"

#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Dialogue/DialogueSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/Interactable.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "MiniGame/MiniGameSubsystem.h"
#include "Misc/CommandLine.h"
#include "NPC/BaseNpcCharacter.h"
#include "Objective/ObjectiveSubsystem.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogDemoAutoPlay, Log, All);

namespace DemoAutoPlay
{
	/**
	 * 엔딩별 선택지 순서: 1일차 3개 → 침대 확인 → 2일차 아침(→ 알바 장소) → 침대 확인 → 3일차 아침(→ 알바 장소)
	 * → 도서관 두 번째 방문이면 서윤 선택지
	 */
	const TCHAR* PlanFor(const FString& Route)
	{
		if (Route == TEXT("A")) return TEXT("0,0,0,0,0,0,0,0");		// 도서관 이틀 - 엔딩 A, 에필로그 둘 다
		if (Route == TEXT("B")) return TEXT("1,1,1,0,1,0,0,1,1");	// 햄버거집, 꽃집 - 엔딩 B, 에필로그 없음
		if (Route == TEXT("C")) return TEXT("0,1,0,0,0,0,1,2");		// 도서관 하루, 편의점 하루 - 엔딩 C
		return TEXT("");
	}

	constexpr float TitleWait = 3.f;
	constexpr float QuitWait = 4.f;
	constexpr float ChoiceWait = 3.f;
	constexpr float MiniGameShowSeconds = 12.f;
	constexpr float NpcReach = 150.f;
	constexpr float PropReach = 120.f;
}

bool UDemoAutoPlaySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	FString Route;
	return FParse::Value(FCommandLine::Get(), TEXT("AutoPlay="), Route);
}

void UDemoAutoPlaySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	FString Route, Plan;
	FParse::Value(FCommandLine::Get(), TEXT("AutoPlay="), Route);
	if (!FParse::Value(FCommandLine::Get(), TEXT("AutoPlayChoices="), Plan))
	{
		Plan = DemoAutoPlay::PlanFor(Route.ToUpper());
	}

	TArray<FString> Parts;
	Plan.ParseIntoArray(Parts, TEXT(","));
	for (const FString& Part : Parts)
	{
		ChoicePlan.Add(FCString::Atoi(*Part));
	}
	UE_LOG(LogDemoAutoPlay, Log, TEXT("자동 플레이 시작: 경로 %s, 선택지 %s"), *Route, *Plan);
}

TStatId UDemoAutoPlaySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UDemoAutoPlaySubsystem, STATGROUP_Tickables);
}

void UDemoAutoPlaySubsystem::Tick(float DeltaTime)
{
	UGameInstance* GI = GetGameInstance();
	UWorld* World = GI ? GI->GetWorld() : nullptr;
	if (!World || !World->HasBegunPlay())
	{
		return;
	}

	if (LastWorld.Get() != World)
	{
		LastWorld = World;
		WorldTime = 0.f;
		LastLine.Reset();
		LineTime = ChoiceTime = MiniGameTime = 0.f;
		WalkTarget.Reset();
		InteractCooldown = 0.f;
	}
	WorldTime += DeltaTime;
	InteractCooldown -= DeltaTime;

	if (UGameplayStatics::GetCurrentLevelName(World) == TEXT("FreeLoad"))
	{
		TickTitle(*World);
		return;
	}

	UMiniGameSubsystem* MiniGame = World->GetSubsystem<UMiniGameSubsystem>();
	if (MiniGame && MiniGame->IsMiniGameActive())
	{
		MiniGameTime += DeltaTime;
		if (MiniGameTime > DemoAutoPlay::MiniGameShowSeconds)
		{
			MiniGameTime = 0.f;
			FMiniGameResult Result;
			Result.Score = 5;
			MiniGame->GrantReward(Result);
			MiniGame->EndMiniGame();
		}
		return;
	}
	MiniGameTime = 0.f;

	UDialogueSubsystem* Dialogue = World->GetSubsystem<UDialogueSubsystem>();
	if (Dialogue && Dialogue->IsDialogueActive() && !Dialogue->IsWaitingForAutoAdvance())
	{
		StopWalk(*World);
		TickDialogue(*Dialogue, DeltaTime);
		return;
	}
	LastLine.Reset();

	// 대화가 없거나, "NPC 근처까지 오기"를 기다리는 중 - 목표까지 걷는다
	const bool bArrived = TickWalk(*World, DeltaTime);
	if (!bArrived || InteractCooldown > 0.f || (Dialogue && Dialogue->IsDialogueActive()))
	{
		return;
	}

	// 침대, 시험장 문처럼 상호작용으로 넘어가는 목표
	AActor* Target = WalkTarget.Get();
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (Target && Pawn && !Target->IsA<ABaseNpcCharacter>() && Target->Implements<UInteractable>()
		&& IInteractable::Execute_CanInteract(Target, Pawn))
	{
		UE_LOG(LogDemoAutoPlay, Log, TEXT("상호작용: %s"), *Target->GetName());
		IInteractable::Execute_Interact(Target, Pawn);
		InteractCooldown = 3.f;
	}
}

void UDemoAutoPlaySubsystem::TickTitle(UWorld& World)
{
	if (bRunStarted)
	{
		// 엔딩을 보고 타이틀로 돌아왔다 - 녹화를 끝낼 수 있게 종료
		if (WorldTime > DemoAutoPlay::QuitWait)
		{
			UE_LOG(LogDemoAutoPlay, Log, TEXT("자동 플레이 끝 - 종료"));
			bRunStarted = false;
			UKismetSystemLibrary::QuitGame(&World, World.GetFirstPlayerController(), EQuitPreference::Quit, false);
		}
		return;
	}

	if (WorldTime < DemoAutoPlay::TitleWait)
	{
		return;
	}

	// "시작" 글자가 쓰인 버튼을 찾아 누른다
	for (TObjectIterator<UTextBlock> It; It; ++It)
	{
		if (It->GetWorld() != &World || !It->GetText().ToString().Contains(TEXT("시작")))
		{
			continue;
		}
		for (UWidget* Parent = It->GetParent(); Parent; Parent = Parent->GetParent())
		{
			if (UButton* Button = Cast<UButton>(Parent))
			{
				UE_LOG(LogDemoAutoPlay, Log, TEXT("타이틀 시작 버튼 누름"));
				bRunStarted = true;
				Button->OnClicked.Broadcast();
				return;
			}
		}
	}
}

void UDemoAutoPlaySubsystem::TickDialogue(UDialogueSubsystem& Dialogue, float DeltaTime)
{
	if (Dialogue.IsWaitingForChoice())
	{
		ChoiceTime += DeltaTime;
		if (ChoiceTime > DemoAutoPlay::ChoiceWait)
		{
			const int32 Index = ChoicePlan.IsValidIndex(ChoiceCursor) ? ChoicePlan[ChoiceCursor] : 0;
			UE_LOG(LogDemoAutoPlay, Log, TEXT("선택지 %d번째 -> %d"), ChoiceCursor, Index);
			++ChoiceCursor;
			ChoiceTime = 0.f;
			LastLine.Reset();
			Dialogue.SelectChoice(Index, false);
		}
		return;
	}
	ChoiceTime = 0.f;

	FText Speaker, Line;
	TArray<FDialogueChoiceView> Choices;
	Dialogue.GetCurrentLine(Speaker, Line, Choices);
	const FString Text = Line.ToString();
	if (Text != LastLine)
	{
		LastLine = Text;
		LineTime = 0.f;
	}
	LineTime += DeltaTime;

	// 글자가 다 찍히고 읽을 시간만큼 기다린다
	const float ReadSeconds = FMath::Clamp(1.8f + Text.Len() * 0.11f, 2.8f, 7.f);
	if (LineTime > ReadSeconds)
	{
		LineTime = 0.f;
		LastLine.Reset();
		Dialogue.Advance();
	}
}

bool UDemoAutoPlaySubsystem::TickWalk(UWorld& World, float DeltaTime)
{
	UObjectiveSubsystem* Objective = World.GetSubsystem<UObjectiveSubsystem>();
	AActor* Target = Objective ? Objective->GetTargetActor() : nullptr;
	APlayerController* PC = World.GetFirstPlayerController();
	ACharacter* Character = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Target || !Character)
	{
		StopWalk(World);
		return false;
	}

	if (WalkTarget.Get() != Target)
	{
		WalkTarget = Target;
		RepathTime = 0.f;
		StuckTime = 0.f;
		DirectMoveTime = 0.f;
		BestDistance = TNumericLimits<float>::Max();
	}

	const FVector From = Character->GetActorLocation();
	const FVector To = Target->GetActorLocation();
	const float Distance = FVector::Dist2D(From, To);
	const float Reach = Target->IsA<ABaseNpcCharacter>() ? DemoAutoPlay::NpcReach : DemoAutoPlay::PropReach;
	if (Distance <= Reach)
	{
		PC->StopMovement();
		return true;
	}

	// 길찾기로 걷고, 2초 넘게 가까워지지 않으면 잠깐 곧장 밀어 준다
	if (Distance < BestDistance - 30.f)
	{
		BestDistance = Distance;
		StuckTime = 0.f;
	}
	else
	{
		StuckTime += DeltaTime;
	}
	if (StuckTime > 2.f)
	{
		StuckTime = 0.f;
		BestDistance = Distance;
		DirectMoveTime = 1.5f;
		PC->StopMovement();
	}

	// 길만 길찾기로 구하고, 움직임은 플레이어 입력과 같은 AddMovementInput으로 준다.
	// (SimpleMoveToLocation은 가속도 없이 속도만 바꿔서 걷기 애니메이션이 안 나오고 미끄러져 보였다)
	RepathTime -= DeltaTime;
	if (RepathTime <= 0.f)
	{
		RepathTime = 0.5f;
		PathPoints.Reset();
		if (const UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(&World, From, To, Character))
		{
			if (Path->IsValid())
			{
				PathPoints = Path->PathPoints;
			}
		}
	}

	FVector Goal = To;
	if (DirectMoveTime > 0.f)
	{
		DirectMoveTime -= DeltaTime;
	}
	else
	{
		// 이미 지나온 경유점은 건너뛰고 다음 점을 향한다
		for (const FVector& Point : PathPoints)
		{
			if (FVector::Dist2D(From, Point) > 60.f)
			{
				Goal = Point;
				break;
			}
		}
	}
	Character->AddMovementInput((Goal - From).GetSafeNormal2D(), 1.f);

	// 카메라가 걷는 방향 뒤를 따라가게 (녹화 화면용).
	// 목표 바로 앞에서는 옆으로 꺾는 걸음에 카메라가 따라 돌아 좁은 복도 벽에 막히므로 돌리지 않는다
	const FVector Velocity = Character->GetVelocity();
	if (Velocity.SizeSquared2D() > 100.f && Distance > 350.f)
	{
		const FRotator Current = PC->GetControlRotation();
		const FRotator Wanted(Current.Pitch, Velocity.Rotation().Yaw, 0.f);
		PC->SetControlRotation(FMath::RInterpTo(Current, Wanted, DeltaTime, 2.5f));
	}
	return false;
}

void UDemoAutoPlaySubsystem::StopWalk(UWorld& World)
{
	if (WalkTarget.IsValid())
	{
		if (APlayerController* PC = World.GetFirstPlayerController())
		{
			PC->StopMovement();
		}
		WalkTarget.Reset();
	}
}
