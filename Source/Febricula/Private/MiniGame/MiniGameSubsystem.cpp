// [skydodo] 2026-10-10 작성
// 요약: 미니게임 진행 구현
// Fill out your copyright notice in the Description page of Project Settings.

#include "MiniGame/MiniGameSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "MiniGame/MiniGameSettings.h"
#include "MiniGame/MiniGameWidget.h"
#include "Misc/PackageName.h"
#include "Progress/GameProgressSubsystem.h"
#include "TimerManager.h"
#include "Activity/ActivitySubsystem.h"
#include "Activity/ActivitySetting.h"
#include "GameMode/TimeSubsystem.h"
#include "Kismet/KismetSystemLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogMiniGame, Log, All);

namespace MiniGameStart
{
	/** 폰을 기다리는 간격 · 최대 횟수 (0.1초 × 30 = 3초) */
	constexpr float RetryInterval = 0.1f;
	constexpr int32 MaxAttempts = 30;

	// [skydodo] 2026-10-17 추가: 본편 대화(버스 정류장 → 편의점)로 들어오는 미니게임 전용 맵.
	// 문 상호작용 없이 레벨 이동만으로 들어오므로 예약이 없어도 바로 시작한다.
	static bool IsDedicatedLevel(FName LevelKey)
	{
		return LevelKey == TEXT("L_ConvenienceStore");
	}
}

// [skydodo] 2026-10-17 추가: 시연 영상용 일주일 흐름.
// 1일차는 이야기대로 진행하고(저녁이 되면 다음 날로), 2일차부터는 아침에 바로 버스 정류장(알바 선택지)으로 보낸다.
// 알바를 하나 끝내면 다음 날 정류장으로, 4일차 알바까지(알바 3개) 끝나면 게임을 끈다.
namespace DemoFlow
{
	constexpr int32 LastStoryDay = 1;
	constexpr int32 LastWorkDay = 4;
	constexpr float CheckInterval = 0.5f;

	/** 알바 기간에 끼어들면 안 되는 2일차 이후 메인 스토리 (DT_MainStroyEvent) */
	static const TCHAR* const SkippedStoryEvents[] = {
		TEXT("MainStory7"), TEXT("MainStory8"), TEXT("MainStory9"), TEXT("MainStory11"), TEXT("MainStory14"),
		TEXT("MainStory15"), TEXT("MainStory16"), TEXT("MainStory17"),
	};

	static bool IsMainFlowLevel(FName LevelKey)
	{
		return LevelKey != TEXT("L_MiniGameTest") && LevelKey != TEXT("FreeLoad");
	}

	static UTimeSubsystem* GetTime(const UWorld* World)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		return GI ? GI->GetSubsystem<UTimeSubsystem>() : nullptr;
	}

	static void OpenLevelByKey(UObject* WorldContext, FName Key)
	{
		if (const TSoftObjectPtr<UWorld>* Level = GetDefault<UActivitySetting>()->Levels.Find(Key))
		{
			UGameplayStatics::OpenLevelBySoftObjectPtr(WorldContext, *Level);
		}
	}

	/** 다음 날 아침으로 넘기고, 이야기 기간이면 방 · 알바 기간이면 정류장에서 시작한다 */
	static void GoNextDay(UWorld* World)
	{
		UTimeSubsystem* Time = GetTime(World);
		if (!Time)
		{
			return;
		}
		Time->ResetSchedule();
		UE_LOG(LogMiniGame, Log, TEXT("[시연] %d일차로 넘김"), Time->GetAbsoluteDay());
		OpenLevelByKey(World, Time->GetAbsoluteDay() > LastStoryDay ? FName(TEXT("MiddleBus")) : FName(TEXT("Room")));
	}

	static void OnLevelStarted(UWorld& World, FName LevelKey)
	{
		UTimeSubsystem* Time = GetTime(&World);
		if (!Time || !IsMainFlowLevel(LevelKey))
		{
			return;
		}

		if (Time->GetAbsoluteDay() > LastStoryDay)
		{
			if (UGameProgressSubsystem* Progress = World.GetGameInstance()->GetSubsystem<UGameProgressSubsystem>())
			{
				for (const TCHAR* EventId : SkippedStoryEvents)
				{
					Progress->MarkEventCompleted(EventId);
				}
			}
			// 알바 기간에 방에서 깨면 바로 정류장으로
			if (LevelKey == TEXT("Room"))
			{
				OpenLevelByKey(&World, TEXT("MiddleBus"));
				return;
			}
		}

		// 이야기 기간에는 저녁 시간대가 되면 다음 날로. 시간대 이동 액션이 저녁을 거쳐 지나가는 중일 수 있어
		// 바로 넘기지 않고 다음 검사 때도 저녁이면 넘긴다.
		if (Time->GetAbsoluteDay() <= LastStoryDay)
		{
			FTimerHandle Handle;
			const TWeakObjectPtr<UWorld> WeakWorld(&World);
			World.GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(&World, [WeakWorld]()
			{
				UWorld* W = WeakWorld.Get();
				UTimeSubsystem* T = GetTime(W);
				if (T && T->GetAbsoluteDay() <= LastStoryDay && T->GetNowScheduleName() == TEXT("Schedule.Dinner"))
				{
					W->GetTimerManager().ClearAllTimersForObject(W);
					GoNextDay(W);
				}
			}), CheckInterval, true, CheckInterval);
		}
	}

	/** 알바를 끝냈을 때. 시연 흐름이 처리했으면 true (원래 복귀 처리를 건너뛴다) */
	static bool OnMiniGameEnded(UWorld* World, FName LevelKey)
	{
		UTimeSubsystem* Time = GetTime(World);
		if (!Time || !IsMainFlowLevel(LevelKey) || Time->GetAbsoluteDay() <= LastStoryDay)
		{
			return false;
		}
		if (Time->GetAbsoluteDay() >= LastWorkDay)
		{
			UE_LOG(LogMiniGame, Log, TEXT("[시연] 마지막 알바 완료 - 게임 종료"));
			UKismetSystemLibrary::QuitGame(World, World->GetFirstPlayerController(), EQuitPreference::Quit, false);
			return true;
		}
		GoNextDay(World);
		return true;
	}
}

// ---------------------------------------------------------------------------
// UMiniGameTravelSubsystem
// ---------------------------------------------------------------------------

void UMiniGameTravelSubsystem::NotifyLevelStarted(const UWorld& World)
{
	const FString Path = UWorld::RemovePIEPrefix(World.GetOutermost()->GetName());
	PreviousLevel = CurrentLevel;
	CurrentLevel = Path;
}

FString UMiniGameTravelSubsystem::GetLevelPath(const UWorld& World)
{
	return UWorld::RemovePIEPrefix(World.GetOutermost()->GetName());
}

void UMiniGameTravelSubsystem::SetReturnPoint(const UWorld& World, const FTransform& Transform)
{
	ReturnPointLevel = GetLevelPath(World);
	ReturnPoint = Transform;
	bHasReturnPoint = true;
}

bool UMiniGameTravelSubsystem::ConsumeReturnPoint(const UWorld& World, FTransform& OutTransform)
{
	if (!bHasReturnPoint || ReturnPointLevel != GetLevelPath(World))
	{
		return false;
	}
	OutTransform = ReturnPoint;
	bHasReturnPoint = false;
	return true;
}

bool UMiniGameTravelSubsystem::ReturnToPreviousLevel(const TSoftObjectPtr<UWorld>& Fallback)
{
	if (!PreviousLevel.IsEmpty() && PreviousLevel != CurrentLevel)
	{
		UE_LOG(LogMiniGame, Log, TEXT("복귀: %s"), *PreviousLevel);
		UGameplayStatics::OpenLevel(this, FName(*PreviousLevel));
		return true;
	}
	if (!Fallback.IsNull())
	{
		UE_LOG(LogMiniGame, Log, TEXT("복귀(이전 레벨 모름): %s"), *Fallback.ToString());
		UGameplayStatics::OpenLevelBySoftObjectPtr(this, Fallback);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// UMiniGameSubsystem
// ---------------------------------------------------------------------------

bool UMiniGameSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

FName UMiniGameSubsystem::GetLevelShortName(const UWorld& World)
{
	const FString Path = UWorld::RemovePIEPrefix(World.GetOutermost()->GetName());
	return FName(*FPackageName::GetShortName(Path));
}

void UMiniGameSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (UGameInstance* GI = InWorld.GetGameInstance())
	{
		if (UMiniGameTravelSubsystem* Travel = GI->GetSubsystem<UMiniGameTravelSubsystem>())
		{
			Travel->NotifyLevelStarted(InWorld);

			// 미니게임에서 돌아온 레벨이면 들어간 자리로 (폰이 생길 때까지 잠깐 기다린다)
			if (Travel->ConsumeReturnPoint(InWorld, PendingReturnPoint))
			{
				ReturnAttempts = 0;
				InWorld.GetTimerManager().SetTimer(ReturnTimer, this, &UMiniGameSubsystem::TryRestoreReturnPoint, MiniGameStart::RetryInterval, true, 0.f);
			}
		}
	}

	const FName LevelKey = GetLevelShortName(InWorld);
	DemoFlow::OnLevelStarted(InWorld, LevelKey);

	const FMiniGameLevelEntry* Entry = GetDefault<UMiniGameSettings>()->Levels.Find(LevelKey);
	if (!Entry)
	{
		return;
	}

	ActiveEntry = *Entry;
	ActiveLevelKey = LevelKey;
	bHasEntry = true;
	StartAttempts = 0;

	if (UMiniGameTravelSubsystem* TravelSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UMiniGameTravelSubsystem>())
	{
		if (TravelSubsystem->GetMinigameBooking() || MiniGameStart::IsDedicatedLevel(LevelKey))
		{
			// BeginPlay 시점에 폰이 이미 있는 게 보통이지만, 게임모드에 따라 한 틱 늦게 생기기도 한다.
			InWorld.GetTimerManager().SetTimer(StartTimer, this, &UMiniGameSubsystem::TryStart, MiniGameStart::RetryInterval, true, 0.f);
			TravelSubsystem->SetMinigameBooking(false);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("MiniGame Subsystem: 미니게임이 예약되지 않음"))
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("MiniGame Subsystem: MiniGameTravelSubsystem이 존재하지 않음"))
	}
}

void UMiniGameSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StartTimer);
		World->GetTimerManager().ClearTimer(ReturnTimer);
	}
	ActiveWidget = nullptr;
	FixedCamera = nullptr;
	BGMComponent = nullptr;
	Super::Deinitialize();
}

void UMiniGameSubsystem::TryStart()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	const bool bReady = PC && (!ActiveEntry.bMovePlayer || PC->GetPawn());
	if (!bReady && ++StartAttempts < MiniGameStart::MaxAttempts)
	{
		return;
	}

	World->GetTimerManager().ClearTimer(StartTimer);

	if (!PC)
	{
		UE_LOG(LogMiniGame, Warning, TEXT("플레이어 컨트롤러가 없어 미니게임을 시작하지 못했다."));
		return;
	}
	
	// [skydodo] 2026-10-13 버그 수정: 이 호출이 주석 처리돼 미니게임 레벨에 들어가도 위젯이 뜨지 않았다
	if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
	{
		if (ActivitySubsystem->GetCurrentAP() > 0)
		{
			if (UGameProgressSubsystem* ProgressSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UGameProgressSubsystem>())
			{
				if (ProgressSubsystem->HasTag("State.DirectWork"))
				{
					ProgressSubsystem->RemoveTag("State.DirectWork");
				}
			}
			StartMiniGame(PC);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("MiniGame Subsystem: 행동력 부족"));
		}
	}
}

void UMiniGameSubsystem::TryRestoreReturnPoint()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn && ++ReturnAttempts < MiniGameStart::MaxAttempts)
	{
		return;
	}
	World->GetTimerManager().ClearTimer(ReturnTimer);
	if (!Pawn)
	{
		return;
	}

	const FRotator Facing(0.f, PendingReturnPoint.Rotator().Yaw, 0.f);
	Pawn->TeleportTo(PendingReturnPoint.GetLocation(), Facing);
	PC->SetControlRotation(FRotator(-10.f, Facing.Yaw, 0.f));

	// 미니게임 위젯이 키보드 포커스를 쥔 채 사라지면 레벨을 옮긴 뒤에도 게임 화면이 키를 못 받는다
	// (이동 키 · ESC가 안 먹힌다). 게임 화면으로 확실히 돌려준다.
	PC->SetInputMode(FInputModeGameOnly());
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
	UE_LOG(LogMiniGame, Log, TEXT("미니게임에서 복귀: 들어간 자리 %s"), *PendingReturnPoint.GetLocation().ToString());
}

void UMiniGameSubsystem::StartMiniGame(APlayerController* PC)
{
	if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
	{
		if (ActivitySubsystem->TryConsumeAP(1))
		{
			UClass* WidgetClass = ActiveEntry.WidgetClass.LoadSynchronous();
			if (!WidgetClass)
			{
				UE_LOG(LogMiniGame, Warning, TEXT("위젯 클래스를 불러오지 못했다: %s"), *ActiveEntry.WidgetClass.ToString());
				return;
			}

			if (ActiveEntry.bMovePlayer)
			{
				if (APawn* Pawn = PC->GetPawn())
				{
					const FRotator Facing(0.f, ActiveEntry.PlayerYaw, 0.f);
					Pawn->TeleportTo(ActiveEntry.PlayerLocation, Facing);
					PC->SetControlRotation(FRotator(-10.f, ActiveEntry.PlayerYaw, 0.f));
				}
			}

			if (ActiveEntry.bUseFixedCamera)
			{
				FActorSpawnParameters Params;
				Params.ObjectFlags |= RF_Transient;
				Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				if (ACameraActor* Camera = PC->GetWorld()->SpawnActor<ACameraActor>(ActiveEntry.CameraLocation, ActiveEntry.CameraRotation, Params))
				{
					Camera->GetCameraComponent()->SetFieldOfView(ActiveEntry.CameraFOV);
					// 화면 비율대로 그린다 (기본값은 검은 레터박스를 씌운다)
					Camera->GetCameraComponent()->SetConstraintAspectRatio(false);
					PC->SetViewTargetWithBlend(Camera, 0.f);
					FixedCamera = Camera;
				}
			}

			// 게임 중에는 캐릭터가 움직이지 않게 막고, 키 입력은 전부 위젯이 받는다.
			PC->SetIgnoreMoveInput(true);
			PC->SetIgnoreLookInput(true);

			ActiveWidget = CreateWidget<UMiniGameWidget>(PC, WidgetClass);
			if (!ActiveWidget)
			{
				return;
			}
			ActiveWidget->AddToViewport(50);

			FInputModeUIOnly InputMode;
			InputMode.SetWidgetToFocus(ActiveWidget->TakeWidget());
			InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			PC->SetInputMode(InputMode);
			PC->SetShowMouseCursor(true);

			if (USoundBase* Music = ActiveEntry.BGM.LoadSynchronous())
			{
				// 레벨을 넘어가면 같이 사라지게 (복귀한 레벨에서 계속 나오지 않도록)
				BGMComponent = UGameplayStatics::SpawnSound2D(this, Music, ActiveEntry.BGMVolume, 1.f, 0.f, nullptr, false, true);
			}
			else if (!ActiveEntry.BGM.IsNull())
			{
				UE_LOG(LogMiniGame, Warning, TEXT("BGM을 불러오지 못했다: %s"), *ActiveEntry.BGM.ToString());
			}

			bRewardGranted = false;
			UE_LOG(LogMiniGame, Log, TEXT("미니게임 시작: %s"), *WidgetClass->GetName());

		}
	}
}

FName UMiniGameSubsystem::GetPlayCounterId() const
{
	return FName(*FString::Printf(TEXT("MiniGame.%s.Played"), *ActiveLevelKey.ToString()));
}

int32 UMiniGameSubsystem::GetPlayCount() const
{
	const UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UGameProgressSubsystem* Progress = GI ? GI->GetSubsystem<UGameProgressSubsystem>() : nullptr;
	return (Progress && bHasEntry) ? Progress->GetCounter(GetPlayCounterId()) : 0;
}

FMiniGameReward UMiniGameSubsystem::PreviewReward(int32 Score) const
{
	FMiniGameReward Reward;
	if (!bHasEntry)
	{
		return Reward;
	}

	Reward.Money = ActiveEntry.MoneyReward;
	Reward.Stat = ActiveEntry.RewardStat;
	Reward.StatAmount = 1;
	for (const int32 Threshold : ActiveEntry.StatThresholds)
	{
		if (Score >= Threshold)
		{
			++Reward.StatAmount;
		}
	}
	return Reward;
}

FMiniGameReward UMiniGameSubsystem::GrantReward(const FMiniGameResult& Result)
{
	if (bRewardGranted)
	{
		return GrantedReward;
	}
	bRewardGranted = true;
	GrantedReward = PreviewReward(Result.Score);

	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UGameProgressSubsystem* Progress = GI ? GI->GetSubsystem<UGameProgressSubsystem>() : nullptr;
	if (!Progress)
	{
		UE_LOG(LogMiniGame, Warning, TEXT("GameProgressSubsystem이 없어 보상을 지급하지 못했다."));
		return GrantedReward;
	}

	const FName MoneyCounter = GetDefault<UMiniGameSettings>()->MoneyCounterId;
	const int32 MoneyNow = Progress->AddCounter(MoneyCounter, GrantedReward.Money);
	int32 StatNow = 0;
	if (!GrantedReward.Stat.IsNone() && GrantedReward.StatAmount > 0)
	{
		StatNow = Progress->AddStat(GrantedReward.Stat, GrantedReward.StatAmount);
	}

	// 끝까지 한 판만 센다. 건너뛰기는 "한 번은 해 봐야 건너뛸 수 있다"는 조건을 무너뜨리므로 제외.
	if (!Result.bSkipped)
	{
		Progress->AddCounter(GetPlayCounterId(), 1);
	}

	UE_LOG(LogMiniGame, Log, TEXT("보상 지급%s: 점수 %d → %s +%d (현재 %d), %s +%d (현재 %d), 플레이 %d회"),
		Result.bSkipped ? TEXT("(건너뛰기)") : TEXT(""),
		Result.Score, *MoneyCounter.ToString(), GrantedReward.Money, MoneyNow,
		*GrantedReward.Stat.ToString(), GrantedReward.StatAmount, StatNow, GetPlayCount());
	return GrantedReward;
}

void UMiniGameSubsystem::EndMiniGame()
{
	if (ActiveWidget)
	{
		ActiveWidget->RemoveFromParent();
		ActiveWidget = nullptr;
	}

	if (BGMComponent)
	{
		BGMComponent->FadeOut(0.5f, 0.f);
		BGMComponent = nullptr;
	}

	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (FixedCamera)
	{
		if (PC && PC->GetPawn())
		{
			PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
		}
		FixedCamera->Destroy();
		FixedCamera = nullptr;
	}
	if (PC)
	{
		PC->SetInputMode(FInputModeGameOnly());
		PC->SetShowMouseCursor(false);
		PC->ResetIgnoreMoveInput();
		PC->ResetIgnoreLookInput();
	}

	if (World && DemoFlow::OnMiniGameEnded(World, ActiveLevelKey))
	{
		return;
	}

	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	UMiniGameTravelSubsystem* Travel = GI ? GI->GetSubsystem<UMiniGameTravelSubsystem>() : nullptr;
	if (!Travel || !Travel->ReturnToPreviousLevel(ActiveEntry.FallbackReturnLevel))
	{
		UE_LOG(LogMiniGame, Warning, TEXT("돌아갈 레벨이 없어 이 레벨에 남는다."));
	}
}
