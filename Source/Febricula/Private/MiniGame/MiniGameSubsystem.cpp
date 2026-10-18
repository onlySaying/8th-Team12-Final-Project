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
#include "MiniGame/MiniGameBackdropWidget.h"
#include "Misc/PackageName.h"
#include "Progress/GameProgressSubsystem.h"
#include "TimerManager.h"
#include "Activity/ActivitySubsystem.h"
#include "Activity/ActivitySetting.h"
#include "GameMode/TimeSubsystem.h"
#include "GameMode/TimeSettings.h"
#include "GameMode/DayScheduleAsset.h"
#include "Dialogue/DialogueLogSubsystem.h"
#include "NPC/MainNpc/NpcAffinitySubsystem.h"

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

	/** 대화가 끝나고 입력이 풀린 다음 위젯이 입력을 가져가도록 조금 늦게 시작한다 */
	constexpr float StartHereDelay = 0.5f;
}

// [skydodo] 2026-10-18 변경: 나흘짜리 본편 흐름 (검정고시 시나리오).
// 이야기 진행은 DT_MainStroyEvent 데이터가 맡고, 여기서는 데이터로 표현하기 어려운 두 가지만 한다.
// - 알바를 끝내면 그날 밤 방으로 돌려보낸다 (다음 날은 침대의 "다음날로"로 넘긴다)
// - 타이틀(FreeLoad)에 들어오면 진행 상태를 지워 새 게임으로 만든다 (엔딩 후 다시 시작할 때)
namespace DemoFlow
{
	static const FName NightSchedule(TEXT("Schedule.Night"));

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

	/** 알바를 마친 그날 밤으로 맞추고 방에서 시작한다 */
	static void GoHomeTonight(UWorld* World)
	{
		UTimeSubsystem* Time = GetTime(World);
		if (!Time)
		{
			return;
		}
		// TimeSetSchedule 액션과 같은 방식. 시간표 두 바퀴 안에 못 만나면 멈춘다
		const int32 MaxSteps = FMath::Max(Time->GetScheduleDefs().Num(), 1) * 2;
		for (int32 Step = 0; Time->GetNowScheduleName() != NightSchedule && Step < MaxSteps; ++Step)
		{
			Time->SpendSchedule();
		}
		UE_LOG(LogMiniGame, Log, TEXT("[본편] %d일차 밤, 방으로"), Time->GetAbsoluteDay());
		OpenLevelByKey(World, TEXT("Room"));
	}

	/** 시간·진행·호감도·대화 기록을 게임을 처음 켰을 때로 되돌린다 */
	static void ResetForNewGame(UWorld& World)
	{
		UGameInstance* GI = World.GetGameInstance();
		if (!GI)
		{
			return;
		}

		if (UTimeSubsystem* Time = GI->GetSubsystem<UTimeSubsystem>())
		{
			const UTimeSettings* Settings = GetDefault<UTimeSettings>();
			FTimeSaveData Fresh;
			Fresh.NowMonth = Settings->StartMonth;
			if (const UDayScheduleAsset* WeekDay = Settings->WeekDaySchedule.LoadSynchronous())
			{
				if (WeekDay->ScheduleDefs.Num() > 0)
				{
					Fresh.NowSchedule = WeekDay->ScheduleDefs[0];
				}
			}
			Time->LoadSaveData(Fresh);
		}
		if (UGameProgressSubsystem* Progress = GI->GetSubsystem<UGameProgressSubsystem>())
		{
			Progress->LoadSaveData(FGameProgressSaveData());
		}
		if (UNpcAffinitySubsystem* Affinity = GI->GetSubsystem<UNpcAffinitySubsystem>())
		{
			Affinity->LoadSaveData(FNpcRelationshipSaveData());
		}
		if (UDialogueLogSubsystem* Log = GI->GetSubsystem<UDialogueLogSubsystem>())
		{
			Log->ClearDialogueLog();
		}
		UE_LOG(LogMiniGame, Log, TEXT("[본편] 타이틀 - 새 게임 상태로 초기화"));
	}

	static void OnLevelStarted(UWorld& World, FName LevelKey)
	{
		if (LevelKey == TEXT("FreeLoad"))
		{
			ResetForNewGame(World);
		}
	}

	/** 알바를 끝냈을 때. 본편 흐름이 처리했으면 true (원래 복귀 처리를 건너뛴다) */
	static bool OnMiniGameEnded(UWorld* World, FName LevelKey)
	{
		if (!World || !GetTime(World) || !IsMainFlowLevel(LevelKey))
		{
			return false;
		}
		GoHomeTonight(World);
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

bool UMiniGameSubsystem::StartHere(FName EntryKey)
{
	UWorld* World = GetWorld();
	const FMiniGameLevelEntry* Entry = GetDefault<UMiniGameSettings>()->Levels.Find(EntryKey);
	if (!World || !Entry || IsMiniGameActive())
	{
		UE_LOG(LogMiniGame, Warning, TEXT("미니게임 '%s'를 시작하지 못했다 (설정에 없거나 이미 진행 중)."), *EntryKey.ToString());
		return false;
	}

	ActiveEntry = *Entry;
	ActiveLevelKey = EntryKey;
	bHasEntry = true;
	bStartedHere = true;
	StartAttempts = 0;
	World->GetTimerManager().SetTimer(StartTimer, this, &UMiniGameSubsystem::TryStart, MiniGameStart::RetryInterval, true, MiniGameStart::StartHereDelay);
	return true;
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
			if (ActiveEntry.BackdropColor.A > 0.f)
			{
				Backdrop = CreateWidget<UMiniGameBackdropWidget>(PC, UMiniGameBackdropWidget::StaticClass());
				if (Backdrop)
				{
					Backdrop->SetColor(ActiveEntry.BackdropColor);
					Backdrop->AddToViewport(49);
				}
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
	if (Backdrop)
	{
		Backdrop->RemoveFromParent();
		Backdrop = nullptr;
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

	// [skydodo] 2026-10-18 수정: 지금 레벨에서 띄운 미니게임은 ActiveLevelKey가 미니게임 키(Burger 등)라
	// 레벨 판정은 실제 레벨 이름으로 한다 (테스트 맵에서 끝내도 본편처럼 방으로 보내던 문제)
	if (World && DemoFlow::OnMiniGameEnded(World, GetLevelShortName(*World)))
	{
		bStartedHere = false;
		return;
	}

	// 레벨 이동 없이 띄운 미니게임은 끝나도 그 레벨에 그대로 남는다
	if (bStartedHere)
	{
		bStartedHere = false;
		return;
	}

	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	UMiniGameTravelSubsystem* Travel = GI ? GI->GetSubsystem<UMiniGameTravelSubsystem>() : nullptr;
	if (!Travel || !Travel->ReturnToPreviousLevel(ActiveEntry.FallbackReturnLevel))
	{
		UE_LOG(LogMiniGame, Warning, TEXT("돌아갈 레벨이 없어 이 레벨에 남는다."));
	}
}
