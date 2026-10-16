// [skydodo] 2026-08-17 작성 (최종 수정 2026-08-26)
// 요약: 대화 그래프 실행, 이벤트 선택, 액션 실행 런타임 구현
// Fill out your copyright notice in the Description page of Project Settings.

#include "Dialogue/DialogueSubsystem.h"

#include "CineCameraComponent.h"
#include "Engine/DataTable.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FebriculaCharacter.h"
#include "Activity/ActivitySetting.h"
#include "Activity/ActivitySubsystem.h"
#include "Animation/CharacterAnimationComponent.h"
#include "Cache/ActorCacheSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "NPC/SubNpc/NpcCrowdSpawner.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameMode/FebriculaGameMode.h"
#include "GameMode/SoundSubsystem.h"
#include "GameMode/TargetTypeComponent.h"
#include "GameMode/TargetTypeRole.h"
#include "GameMode/TimeSubsystem.h"
#include "Interaction/InteractionComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Level/MovableMapActor.h"
#include "Progress/GameProgressSubsystem.h"
#include "NPC/BaseNpcCharacter.h"
#include "NPC/MainNpc/MainNpcAIController.h"
#include "NPC/MainNpc/MainNpcCharacter.h"
#include "NPC/MainNpc/NpcAffinitySubsystem.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "Dialogue/DialogueLogSubsystem.h"

TStatId UDialogueSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UDialogueSubsystem, STATGROUP_Tickables);
}

void UDialogueSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogTemp, Warning, TEXT("Dialogue Subsystem: Initialize"));

	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	UGameInstance* GI = World->GetGameInstance();
	if (!GI)
	{
		return;
	}

	if (UActivitySubsystem* ActivitySubsystem = GI->GetSubsystem<UActivitySubsystem>())
	{
		ActivitySubsystem->BindDialogue();
	}
}

void UDialogueSubsystem::Deinitialize()
{
	Super::Deinitialize();
	FinishDialogue();
	UE_LOG(LogTemp, Warning, TEXT("Dialogue Subsystem: Deinitialize"));
}

void UDialogueSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// [skydodo] 2026-10-13 수정: 캐시된 캐릭터 수를 매 프레임 화면에 찍던 디버그 출력 제거 (확인은 GetCachedCharacter 치트로)

	// 빠른 넘기기(Ctrl 홀드 등) 중이면 일정 간격마다 Advance()를 대신 호출해준다.
	// Advance() 자체가 선택지/자동진행 대기 중엔 아무것도 안 하고 리턴하므로,
	// 그런 상황에서는 이 로직도 자연히 아무 효과 없이 대기하게 된다.
	if (bIsFastForwarding && bDialogueActive && !bWaitingForChoice)
	{
		FastForwardElapsed += DeltaTime;
		if (FastForwardElapsed >= FastForwardInterval)
		{
			FastForwardElapsed = 0.f;
			Advance();
		}
	}

	// 자동 진행을 기다리는 중일 때만 일이 있다.
	if (!bDialogueActive || bWaitingForChoice || PendingAutoAdvance == EDialogueAutoAdvance::None)
	{
		return;
	}

	if (!IsAutoAdvanceSatisfied(DeltaTime))
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("Pass!"));
	PendingAutoAdvance = EDialogueAutoAdvance::None;
	AutoAdvanceNpcId = NAME_None;
	AutoAdvanceElapsed = 0.f;

	if (bRunningSimpleLines)
	{
		// 즉석 대사 모드에서도 자동 진행을 쓸 수 있게 같은 경로를 탄다.
		if (SimpleLineQueue.Num() > 0)
		{
			const FText Next = SimpleLineQueue[0];
			SimpleLineQueue.RemoveAt(0);
			CurrentSpeakerNpcId = NAME_None;
			BroadcastDialogueLine(SimpleLineSpeaker, Next);
		}
		else
		{
			FinishDialogue();
		}
		return;
	}

	ResolveNodeExit();
}

bool UDialogueSubsystem::IsAutoAdvanceSatisfied(float DeltaTime)
{
	switch (PendingAutoAdvance)
	{
	case EDialogueAutoAdvance::AfterDelay:
		AutoAdvanceElapsed += DeltaTime;
		return AutoAdvanceElapsed >= AutoAdvanceValue;

	case EDialogueAutoAdvance::WhenPlayerNearNpc:
	{
		const APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
		// 기준 NPC를 따로 지정했으면 그쪽, 아니면 상호작용한 상대
		const AActor* Reference = AutoAdvanceNpcId.IsNone()
			? ToRawPtr(CurrentInstigator)
			: Cast<AActor>(GetCharacterInCache(AutoAdvanceNpcId));

		if (!PlayerPawn || !Reference)
		{
			return false;
		}


		const float RadiusSq = FMath::Square(FMath::Max(AutoAdvanceValue, 1.f));

		float Temp = FVector::DistSquared2D(PlayerPawn->GetActorLocation(), Reference->GetActorLocation());
		UE_LOG(LogTemp, Warning, TEXT("AutoAdvanceValue: %f : %f : %d"), RadiusSq, Temp, Temp <= RadiusSq);

		return FVector::DistSquared2D(PlayerPawn->GetActorLocation(), Reference->GetActorLocation()) <= RadiusSq;
	}

	case EDialogueAutoAdvance::WhenNpcArrived:
	{
		// 여러 NPC가 동시에 이동할 때 누구의 도착을 기다릴지 고를 수 있다.
		const ABaseNpcCharacter* Npc = AutoAdvanceNpcId.IsNone()
			? Cast<ABaseNpcCharacter>(CurrentInstigator)
			: Cast<ABaseNpcCharacter>(GetCharacterInCache(AutoAdvanceNpcId));



		if (!Npc)
		{
			return false;
		}
		const AMainNpcAIController* AI = Cast<AMainNpcAIController>(Npc->GetController());
		if (!AI)
		{
			return false;
		}
		//UE_LOG(LogTemp,Warning, TEXT("Remain: %d"), AI->HasReachedRouteTarget(FMath::Max(AutoAdvanceValue, 1.f)));
		return AI->HasReachedRouteTarget(FMath::Max(AutoAdvanceValue, 1.f));
	}


	default:
		return false;
	}
}

void UDialogueSubsystem::SetDataTables(UDataTable* InEventTable, UDataTable* InDialogueTable)
{
	if (InEventTable)
	{
		EventTable = InEventTable;
	}
	if (InDialogueTable)
	{
		DialogueTable = InDialogueTable;
	}
}

// ---------------------------------------------------------------------------
// 대화 시작
// ---------------------------------------------------------------------------




// [skydodo] 2026-10-13 버그 수정: DialogueNodeId로 고른 경우 BestRowName이 비어 이벤트 완료 기록이 빠졌다. Row가 null이면 역참조하던 것도 막음
bool UDialogueSubsystem::TryStartEventForNpc(FName NpcId, AActor* InInstigator, FName DialogueNodeId)
{
 	if (bDialogueActive || !EventTable)
	{
		return false;
	}

	const UGameProgressSubsystem* Progress = GetProgress();
	if (!Progress)
	{
		return false;
	}

	// 조건을 만족하는 이벤트 중 Priority가 가장 높은 것을 고른다.
	// 동점이면 테이블에 먼저 나온 쪽이 이긴다 (데이터 순서 = 이벤트 트리 순서).
	const FNpcEventRow* BestRow = nullptr;
	FName BestRowName = NAME_None;

	static const FString ContextString(TEXT("TryStartEventForNpc"));
	for (const TPair<FName, uint8*>& Pair : EventTable->GetRowMap())
	{
		const FNpcEventRow* Row = reinterpret_cast<const FNpcEventRow*>(Pair.Value);
		if (!Row)
		{
			continue;
		}
		if (Row->NpcId != NpcId)
		{
			if (!Row->NpcId.IsNone())
			{
				continue;
			}
		}

		// 반복 불가 이벤트인데 이미 완료했으면 후보에서 제외
		if (!Row->bRepeatable && Progress->IsEventCompleted(Pair.Key))
		{
			continue;
		}

		if (!Progress->EvaluateConditions(Row->Preconditions))
		{
			continue;
		}			
		
		if (DialogueNodeId != NAME_None)
		{
			if (Row->RootNodeId == DialogueNodeId)
			{
				BestRow = Row;
				BestRowName = Pair.Key;	// 비워 두면 끝날 때 이벤트 완료 기록이 빠진다
				break;
			}
		}
		if (!BestRow || Row->Priority > BestRow->Priority)
		{
			BestRow = Row;
			BestRowName = Pair.Key;
		}
	}
	if (!BestRow)
	{
		return false;
	}

	CurrentInstigator = InInstigator;
	ActiveEventId = BestRowName;
	bCompleteEventOnFinish = BestRow->bCompleteOnFinish;
	bDialogueActive = true;
	bRunningSimpleLines = false;

	// 여러 NPC가 함께 나오는 장면이면 전원을 Talk 상태로 세운다.
	// 상호작용한 NPC는 이미 AMainNpcCharacter::Interact에서 StartTalk을 호출했지만,
	// StartTalk은 중복 호출해도 안전하므로 목록에 같이 넣어 끝날 때 한꺼번에 풀어준다.
	if (!BestRow->NpcId.IsNone())
	{
		TArray<FName> Participants = BestRow->ParticipantNpcIds;
		Participants.AddUnique(NpcId);
		EnterTalkForParticipants(Participants, InInstigator);
	}

	if (UDataTable* DialogueDT = BestRow->DialogueTable.LoadSynchronous())
	{
		SetDataTables(EventTable, DialogueDT);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("DialogueSubsystem: 이벤트 '%s'가 오버라이딩 한 Dialogue Table '%s'을(를) 찾지 못했습니다."), *BestRowName.ToString(), *BestRow->DialogueTable.ToSoftObjectPath().GetAssetName());
	}

	EnterNode(BestRow->RootNodeId);
	return true;
}

void UDialogueSubsystem::EnterTalkForParticipants(const TArray<FName>& NpcIds, AActor* Interactor)
{
	UE_LOG(LogTemp, Warning, TEXT("UDialogueSubsystem::EnterTalkForParticipants"));
	for (const FName& ParticipantId : NpcIds)
	{
		ABaseNpcCharacter* Npc = Cast<ABaseNpcCharacter>(GetCharacterInCache(ParticipantId));
		if (!Npc)
		{
			UE_LOG(LogTemp, Warning, TEXT("UDialogueSubsystem: 참가자 NPC '%s'를 월드에서 찾지 못했습니다."),
				*ParticipantId.ToString());
			continue;	
		}

		ActiveParticipants.AddUnique(Npc);

		// 메인 NPC만 Talk 상태(=BT 정지 + 상대 응시)를 가진다.
		// 서브 NPC는 그냥 플레이어 쪽을 보게만 해준다.
		// if (ALevelAutoEventSetting* AES = Cast<ALevelAutoEventSetting>(GetWorld()->GetWorldSettings()))
		// {
		// 	if (EventTable == AES->AutoEventRow)
		// 	{
		// 		return;
		// 	}
		// }
		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(Npc))
		{
			MainNpc->StartTalk(Interactor);
		}
		else
		{
			Npc->FaceTowards(Interactor);
		}
	}
}

void UDialogueSubsystem::ReleaseParticipants()
{
	for (ABaseNpcCharacter* Npc : ActiveParticipants)
	{
		if (!Npc)
		{
			continue;
		}

		// 대사 중에 재생시킨 슬롯 애니메이션이 남아있지 않도록 정리한다.
		Npc->StopNpcAnim(.25f, true);

		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(Npc))
		{
			MainNpc->EndTalk();
		}
	}
	ActiveParticipants.Reset();
}

ACharacter* UDialogueSubsystem::GetCharacterInCache(FName CharacterName) const
{
	if (const FCharacterCache* CharPtr = CachedCharacter.Find(CharacterName))
	{
		if (ACharacter* Character = CharPtr->Character.Get())
		{
			return Character;
		}
	}
	return nullptr;
}

bool UDialogueSubsystem::StartDialogueAtNode(FName NodeId, AActor* InInstigator)
{
	if (bDialogueActive || !DialogueTable || NodeId.IsNone())
	{
		return false;
	}

	CurrentInstigator = InInstigator;
	ActiveEventId = NAME_None;
	bCompleteEventOnFinish = false;
	bDialogueActive = true;
	bRunningSimpleLines = false;

	EnterNode(NodeId);
	return true;
}

bool UDialogueSubsystem::StartSimpleLines(const FText& Speaker, const TArray<FText>& Lines, AActor* InInstigator)
{
	if (bDialogueActive || Lines.Num() == 0)
	{
		return false;
	}

	CurrentInstigator = InInstigator;
	ActiveEventId = NAME_None;
	bCompleteEventOnFinish = false;
	bDialogueActive = true;
	bWaitingForChoice = false;

	// 테이블을 거치지 않는 즉석 대사 모드. 사물 조사 문구 등에 쓴다.
	bRunningSimpleLines = true;
	SimpleLineSpeaker = Speaker;
	SimpleLineQueue = Lines;

	BlockPlayerInput(true);

	const FText First = SimpleLineQueue[0];
	SimpleLineQueue.RemoveAt(0);
	CurrentSpeakerNpcId = NAME_None;
	BroadcastDialogueLine(SimpleLineSpeaker, First);
	return true;
}

// ---------------------------------------------------------------------------
// 진행
// ---------------------------------------------------------------------------

void UDialogueSubsystem::Advance()
{
	if (!bDialogueActive)
	{
		return;
	}

	// 선택지가 떠 있는 동안은 클릭으로 넘길 수 없다 (반드시 선택해야 함).
	if (bWaitingForChoice)
	{
		return;
	}

	// 자동 진행 대기 중에도 클릭을 막는다.
	// 걸어가는 도중에 대사를 건너뛰면 "도착했을 때 나와야 할 대사"가 미리 나와서 연출이 어긋난다.
	if (PendingAutoAdvance != EDialogueAutoAdvance::None)
	{
		return;
	}

	if (bRunningSimpleLines)
	{
		if (SimpleLineQueue.Num() > 0)
		{
			const FText Next = SimpleLineQueue[0];
			SimpleLineQueue.RemoveAt(0);
			CurrentSpeakerNpcId = NAME_None;
			BroadcastDialogueLine(SimpleLineSpeaker, Next);
		}
		else
		{
			FinishDialogue();
		}
		return;
	}

	ResolveNodeExit();
}

void UDialogueSubsystem::SetFastForward(bool bEnable)
{
	bIsFastForwarding = bEnable;
	FastForwardElapsed = 0.f;
}

void UDialogueSubsystem::SelectChoice(int32 ChoiceIndex, bool bYesNo)
{
	if (!bDialogueActive || !bWaitingForChoice)
	{
		return;
	}
	
	if (!PendingChoices.IsValidIndex(ChoiceIndex))
	{
		return;
	}
	
	bool RunResult = true;
	if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
	{
		if (ActivitySubsystem->bWaitActivityChoice)
		{	
			ActivitySubsystem->bWaitActivityChoice = false;
			RunResult = ActivitySubsystem->RunActivityByIndex(ChoiceIndex); 
		}
	}

	const FDialogueChoice Chosen = PendingChoices[ChoiceIndex];
	
	// 조건 미달 선택지는 UI에서 비활성으로 보이지만, 혹시 모르니 여기서도 막는다.
	const UGameProgressSubsystem* Progress = GetProgress();
	if (!RunResult && Progress && !Progress->EvaluateConditions(Chosen.Conditions))
	{
		return;
	}

	if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UDialogueLogSubsystem* Log = GI->GetSubsystem<UDialogueLogSubsystem>())
		{
			Log->AddPlayerChoice(Chosen.Text);
		}
	}

	if (bYesNo == true)
	{
		if (ChoiceIndex == PendingChoices.Num() - 1)
		{
			RecentDialogueResult = false;
		}
		else
		{
			RecentDialogueResult = true;
		}
	}

	PendingChoices.Reset();
	bWaitingForChoice = false;
	
	ExecuteActions(Chosen.Actions);
		
	if (Chosen.NextNodeId.IsNone())
	{
		if (Chosen.bFreeActivity)
		{
			if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
			{
				ActivitySubsystem->bFreeActivity = true;
			}
		}
		FinishDialogue();
	}
	else
	{
		EnterNode(Chosen.NextNodeId);
	}

}

void UDialogueSubsystem::FinishDialogue()
{
	if (!bDialogueActive)
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("Finish Dialogue"));

	// 이벤트 완료 기록. 반복 가능 이벤트도 "한 번은 봤다"는 기록은 남겨두면
	// 후속 이벤트의 EventCompleted 조건에서 쓸 수 있다.
	if (bCompleteEventOnFinish && !ActiveEventId.IsNone())
	{
		if (UGameProgressSubsystem* Progress = GetProgress())
		{
			Progress->MarkEventCompleted(ActiveEventId);
		}
	}

	bDialogueActive = false;
	bWaitingForChoice = false;
	bRunningSimpleLines = false;
	CurrentNodeId = NAME_None;
	CurrentSpeakerNpcId = NAME_None;
	ActiveEventId = NAME_None;
	PendingAutoAdvance = EDialogueAutoAdvance::None;
	RoutingHopCount = 0;
	AutoAdvanceNpcId = NAME_None;
	AutoAdvanceElapsed = 0.f;
	PendingChoices.Reset();
	SimpleLineQueue.Reset();

	// 참가자 전원을 Talk 상태에서 풀어준다 (BT도 여기서 다시 돌기 시작한다).
	ReleaseParticipants();

	// 참가자 목록에 안 들어간 경우(사물 조사, 단발 노드 재생 등)를 위한 보정.
	if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(CurrentInstigator))
	{
		MainNpc->EndTalk();
		// if (UCharacterMovementComponent* CharacterMovement = MainNpc->GetCharacterMovement())
		// {
		// 	CharacterMovement->SetMovementMode(MOVE_Walking);
		// 	MainNpc->SetActorEnableCollision(true);
		// }
	}
	
	// if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
	// {
	// 	int32 NowMaxAp = ActivitySubsystem->GetMaxAP();
	// 	
	// 	if (NowMaxAp > 0 && ActivitySubsystem->GetCurrentAP() <= 0)
	// 	{
	// 		if (UTimeSubsystem* TimeSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UTimeSubsystem>())
	// 		{
	// 			TimeSubsystem->SpendSchedule();
	// 		}
	// 	}
	// }

	
	CurrentInstigator = nullptr;
	BlockPlayerInput(false);
	OnDialogueFinished.Broadcast();
}

// ---------------------------------------------------------------------------
// 노드 진입 / 이탈
// ---------------------------------------------------------------------------

TMap<FName, FCharacterCache> UDialogueSubsystem::GetCachedCharacter() const
{
	return CachedCharacter;
}

FTransform UDialogueSubsystem::GetDefaultTransform(FName NpcId)		
{
	if (FCharacterCache* Cache = CachedCharacter.Find(NpcId))
	{
		return Cache->Transform;
	}                                                                                                                       
	return FTransform();
}

bool UDialogueSubsystem::CheckNpcCached(FName NpcName) const
{
	return CachedCharacter.Contains(NpcName); 
}

void UDialogueSubsystem::EnterNode(FName NodeId)
{
	if (!DialogueTable || NodeId.IsNone())
	{
		FinishDialogue();
		return;
	}

	static const FString ContextString(TEXT("EnterNode"));
	const FDialogueNodeRow* Row = DialogueTable->FindRow<FDialogueNodeRow>(NodeId, ContextString, /*bWarnIfMissing=*/false);
	if (!Row)
	{
		UE_LOG(LogTemp, Warning, TEXT("UDialogueSubsystem: 대화 노드 '%s'를 대화 테이블에서 찾지 못했습니다."), *NodeId.ToString());
		FinishDialogue();
		return;
	}

	CurrentNodeId = NodeId;
	bWaitingForChoice = false;

	// 이 대사를 말하는 NPC가 지정돼 있으면 플레이어를 바라보게 한다.
	// 여러 NPC가 함께 있는 장면에서 "지금 말하는 쪽"이 자연스럽게 드러난다.
	if (!Row->SpeakerNpcId.IsNone())
	{
		// if (ABaseNpcCharacter* Speaker = Cast<ABaseNpcCharacter>(GetCharacterInCache(Row->SpeakerNpcId)))
		// {
		// 	Speaker->FaceTowards(UGameplayStatics::GetPlayerPawn(this, 0));
		// }
	}

	switch (Row->Camera.CameraType)


	{
	case EGameCameraType::None:
		break;
	case EGameCameraType::Player:
		CameraToPlayerCam(Row->Camera.MoveTime);
		break;
	case EGameCameraType::Cinematic:
		CameraToCinematicCam(Row->Camera.Location, Row->Camera.Rotation, Row->Camera.MoveTime, Row->Camera.Focus);
		break;
	}
	
	if (Row->SoundLocation.SoundChannel != NAME_None && Row->SoundLocation.SoundName != NAME_None)
	{
		if (USoundSubsystem* SoundSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USoundSubsystem>())
		{
			if (UActorCacheSubsystem* ActorCache = GetWorld()->GetSubsystem<UActorCacheSubsystem>())
			{
				SoundSubsystem->PlaySE3d(Row->SoundLocation.SoundChannel, SoundSubsystem->FoundSoundBase(Row->SoundLocation.SoundName),  ActorCache->GetTargetLocation(Row->SoundLocation.Target, ETargetTypeRole::Sound), Row->SoundLocation.Blend);
			}
		}
	}
		
	ExecuteActions(Row->OnEnterActions);

	if (Row->Text.IsEmpty() && Row->Choices.Num() == 0
	&& Row->AutoAdvance == EDialogueAutoAdvance::None)
	{
		if (++RoutingHopCount > MaxRoutingHops)
		{
			UE_LOG(LogTemp, Error, TEXT("UDialogueSubsystem: 분기 노드 순환 의심 (%s)"), *NodeId.ToString());
			FinishDialogue();
			return;
		}
		PendingAutoAdvance = EDialogueAutoAdvance::None;
		ResolveNodeExit();   // CurrentNodeId는 위에서 이미 설정됨
		return;
	}
	RoutingHopCount = 0;
	
	// 선택지가 있는 노드는 반드시 골라야 하므로 자동 진행을 걸지 않는다.
	// if (Row->Choices.Num() > 0)
	// {
	// 	PendingAutoAdvance = EDialogueAutoAdvance::None;
	// }
	PendingAutoAdvance = Row->AutoAdvance;
	AutoAdvanceValue = Row->AutoAdvanceValue;
	AutoAdvanceNpcId = Row->AutoAdvanceNpcId;
	AutoAdvanceElapsed = 0.f;

	// 이동 대기 노드: 대사를 띄우지 않고 창만 닫아둔 채 조건이 만족될 때까지 기다린다.
	// 플레이어는 그동안 자유롭게 NPC를 따라간다.
	if (Row->bHideBoxWhileWaiting && PendingAutoAdvance != EDialogueAutoAdvance::None) //&& PendingAutoAdvance != EDialogueAutoAdvance::AfterDelay)
	{
		if (PendingAutoAdvance != EDialogueAutoAdvance::AfterDelay)
		{
			BlockPlayerInput(false);
		}
		OnDialogueBoxHidden.Broadcast();
		return;
	}
	BlockPlayerInput(true);

	bHasHoldingLine = true;
	HoldingSpeaker = Row->SpeakerName;
	HoldingDialogue = Row->Text;

	CurrentSpeakerNpcId = Row->SpeakerNpcId;
	BroadcastDialogueLine(Row->SpeakerName, Row->Text);

	// 선택지가 있는 노드는 질문 대사와 선택지를 "동시에" 띄운다.
	// 대사를 먼저 보여주고 한 번 더 클릭해야 선택지가 나오면 클릭이 낭비되고,
	// 질문을 읽은 플레이어는 선택지가 바로 나올 거라고 기대하기 때문이다.
	if (Row->Choices.Num() > 0)
	{
		PresentChoices(Row->Choices, Row->bYesNo);
	}
}

void UDialogueSubsystem::ActivityDialoguePrint(const FDialogueNodeRow& Row)
{
	if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
	{
		if (ActivitySubsystem->bFreeActivity) return;
		BlockPlayerInput(true);
		bHasHoldingLine = true;
		HoldingSpeaker = Row.SpeakerName;
		HoldingDialogue = Row.Text;
		bDialogueActive = true;
		CurrentSpeakerNpcId = NAME_None;
		BroadcastDialogueLine(Row.SpeakerName, Row.Text);
		UE_LOG(LogTemp, Error, TEXT("IsBound=%d"), OnDialogueLine.IsBound());

		if (Row.Choices.Num() > 0)
		{
			PresentChoices(Row.Choices);
		}
	}
}

void UDialogueSubsystem::PresentChoices(const TArray<FDialogueChoice>& InChoices, bool bYesNo)
{
	const UGameProgressSubsystem* Progress = GetProgress();

	PendingChoices = InChoices;
	bWaitingForChoice = true;

	TArray<FDialogueChoiceView> Views;
	Views.Reserve(PendingChoices.Num());
	for (const FDialogueChoice& Choice : PendingChoices)
	{
		FDialogueChoiceView View;
		View.Text = Choice.Text;
		// 조건 미달이면 목록엔 보이되 못 고르게 한다 (매력/담력 부족 선택지 노출용)
		View.bEnabled = Progress ? Progress->EvaluateConditions(Choice.Conditions) : true;
		UE_LOG(LogTemp, Warning, TEXT("View Name: %s"), *View.Text.ToString());
		UE_LOG(LogTemp, Warning, TEXT("View: %s, Enabled=%d"), *View.Text.ToString(), View.bEnabled);
		Views.Add(View);
	}
	for (FDialogueChoiceView Choice : Views)
	{
		UE_LOG(LogTemp, Warning, TEXT("View Name2: %s"), *Choice.Text.ToString());		
	}
	HoldingChoices = Views;
	if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UDialogueLogSubsystem* Log = GI->GetSubsystem<UDialogueLogSubsystem>())
		{
			TArray<FText> ChoiceTexts;
			for (const FDialogueChoiceView& V : Views)
			{
				ChoiceTexts.Add(V.Text);
			}
			Log->AttachChoices(ChoiceTexts);
		}
	}
	OnDialogueChoices.Broadcast(Views, bYesNo);
	UE_LOG(LogTemp, Error, TEXT("IsBound=%d"), OnDialogueChoices.IsBound());
	bWaitingForChoice = true;
}

void UDialogueSubsystem::ResolveNodeExit()
{
	if (!DialogueTable || CurrentNodeId.IsNone())
	{
		FinishDialogue();
		return;
	}

	static const FString ContextString(TEXT("ResolveNodeExit"));
	const FDialogueNodeRow* Row = DialogueTable->FindRow<FDialogueNodeRow>(CurrentNodeId, ContextString, /*bWarnIfMissing=*/false);
	if (!Row)
	{
		FinishDialogue();
		return;
	}

	const UGameProgressSubsystem* Progress = GetProgress();

	// 1) 선택지는 EnterNode에서 이미 띄웠으므로 여기 오면 안 되지만, 안전장치로 남겨둔다
	if (Row->Choices.Num() > 0)
	{
		PresentChoices(Row->Choices);
		return;
	}

	// 2) 자동 분기. 위에서부터 검사해 첫 통과 갈래를 탄다
	if (Row->Branches.Num() > 0)
	{
		for (const FDialogueBranch& Branch : Row->Branches)
		{
			if (!Progress || Progress->EvaluateConditions(Branch.Conditions))
			{
				ExecuteActions(Branch.Actions);

				if (Branch.NextNodeId.IsNone())
				{
					FinishDialogue();
				}
				else
				{
					EnterNode(Branch.NextNodeId);
				}
				return;
			}
		}

		// 어느 갈래도 통과 못하면 대화를 끝낸다 (데이터에 else 갈래를 넣어두는 걸 권장)
		FinishDialogue();
		return;
	}

	// 3) 단순 다음 노드
	if (Row->NextNodeId.IsNone())
	{
		FinishDialogue();
	}
	else
	{
		EnterNode(Row->NextNodeId);
	}
}

// ---------------------------------------------------------------------------
// 액션 실행
// ---------------------------------------------------------------------------

// [skydodo] 2026-10-13 버그 수정: AddStat case에 break가 없어 CompleteEvent까지 실행돼 스탯 키가 완료 이벤트로 기록됐다
void UDialogueSubsystem::ExecuteActions(const TArray<FGameAction>& Actions)
{
	for (const FGameAction& Action : Actions)
	{
		ExecuteAction(Action);
	}
}

void UDialogueSubsystem::BlockPlayerInput(bool bLock)
{
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
	{
		PC->ResetIgnoreMoveInput();
		PC->ResetIgnoreLookInput();

		if (bLock)
		{
			PC->SetIgnoreMoveInput(true);
			PC->SetIgnoreLookInput(true);
			if (ACharacter* Char = Cast<ACharacter>(PC->GetPawn()))
			{
				Char->GetCharacterMovement()->StopMovementImmediately();
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Block Disable!!!"));
			if (ACharacter* Char = Cast<ACharacter>(PC->GetPawn()))
			{
				if (UCharacterMovementComponent* Movement = Char->GetCharacterMovement())
				{
					Char->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
					Char->SetActorEnableCollision(true);
					Movement->SetComponentTickEnabled(true);
					Char->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					Char->GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				}
			}
		}
	}
}

void UDialogueSubsystem::CameraToCinematicCam(const FVector Loc, const FRotator Rot, const float MoveTime, const float Focus)
{
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!PC || !PC->GetPawn()) return;

	ActiveCamIndex = 1 - ActiveCamIndex;          // 번갈아
	TObjectPtr<ACineCameraActor>& Cam = CinematicCamera[ActiveCamIndex];


	if (!IsValid(Cam) || Cam->GetWorld() != GetWorld())
	{
		Cam = GetWorld()->SpawnActor<ACineCameraActor>();
	}

	Cam->SetActorLocationAndRotation(Loc, Rot);
	if (Focus >= 0)
	{
		UCineCameraComponent* CineCameraComponent = Cam->GetCineCameraComponent();
		CineCameraComponent->FocusSettings.ManualFocusDistance = Focus;
	}
	PC->SetViewTargetWithBlend(Cam, MoveTime, VTBlend_EaseInOut, 2.f, true);
}

void UDialogueSubsystem::CameraToPlayerCam(const float MoveTime)
{
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
	{
		if (PC->GetPawn())
		{
			PC->SetViewTargetWithBlend(PC->GetPawn(), MoveTime, VTBlend_EaseInOut, 2.f, true);
		}
	}
}

	bool UDialogueSubsystem::GetCurrentLine(FText& OutSpeaker, FText& OutLine, TArray<FDialogueChoiceView>& OutChoiceViews)
	{
		OutSpeaker = HoldingSpeaker;
		OutLine = HoldingDialogue;
		UE_LOG(LogTemp, Warning, TEXT("Holding Line Count: %d"), HoldingChoices.Num());
		OutChoiceViews = HoldingChoices;
		return bHasHoldingLine;	
}

void UDialogueSubsystem::AddCharacterByName(FName Name, ACharacter* Character)
{
	if (Character && !Name.IsNone())
	{
		if (const FCharacterCache* Found = CachedCharacter.Find(Name))
		{
			if (Found->Character.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("Character %s already exists"), *Name.ToString());
				return;
			}
		}
		FVector Temp = Character->GetActorLocation();
		UE_LOG(LogTemp, Warning, TEXT("캐릭터 기본 위치: %f : %f : %f"), Temp.X, Temp.Y, Temp.Z);
		CachedCharacter.Add(Name, FCharacterCache(Character, Character->GetTransform()));
	}
}

void UDialogueSubsystem::RemoveCharacterByName(FName Name, ACharacter* Character)
{
	if (const FCharacterCache* Found = CachedCharacter.Find(Name))
	{
		if (Found->Character.Get() == Character)
		{
			CachedCharacter.Remove(Name);
		}
	}
}

void UDialogueSubsystem::ExecuteQueuedActions()
{
	if (QueuedLevelChange.Num() > 0)
	{
		ExecuteAction(QueuedLevelChange.Pop());
	}
	QueuedLevelChange.Reset();
}

void UDialogueSubsystem::ExecuteAction(const FGameAction& Action)
{
	UGameProgressSubsystem* Progress = GetProgress();

	switch (Action.Type)
	{
	case EGameActionType::None:
		break;

	case EGameActionType::AddAffinity:
		if (UNpcAffinitySubsystem* Relationship = GetRelationship())
		{
			Relationship->AddAffinity(Action.Key, static_cast<float>(Action.Value));
		}
		break;

	case EGameActionType::GrantTag:
		if (Progress) { Progress->GrantTag(Action.Key); }
		break;

	case EGameActionType::RemoveTag:
		if (Progress) { Progress->RemoveTag(Action.Key); }
		break;

	case EGameActionType::AddCounter:
		if (Progress) { Progress->AddCounter(Action.Key, Action.Value); }
		break;
		
	case EGameActionType::AddStat:
		if (Progress) { Progress->AddStat(Action.Key, Action.Value); }
		break;

	case EGameActionType::CompleteEvent:
		if (Progress) { Progress->MarkEventCompleted(Action.Key); }
		break;

	case EGameActionType::GrantTopic:
		if (Progress) { Progress->GrantTopic(Action.Key); }
		break;

		// ---- 시간 진행 ----

	case EGameActionType::TimeAdvanceDay:
	case EGameActionType::TimeAdvanceSchedule:
	{
		const UWorld* World = GetWorld();
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UTimeSubsystem* Time = GameInstance ? GameInstance->GetSubsystem<UTimeSubsystem>() : nullptr;
		if (!Time)
		{
			UE_LOG(LogTemp, Warning, TEXT("시간 진행 액션: UTimeSubsystem을 찾지 못했습니다."));
			break;
		}

		// Value를 비워두면(0) 한 번만 진행한다. 실수로 0을 넣어도 아무 일도 안 일어나는 것보다
		// 1회 진행이 의도에 가깝기 때문이다.
		const int32 Steps = FMath::Clamp(Action.Value <= 0 ? 1 : Action.Value, 1, 100);
		for (int32 i = 0; i < Steps; ++i)
		{
			if (Action.Type == EGameActionType::TimeAdvanceDay)
			{
				Time->SpendDay();
			}
			else
			{
				Time->SpendSchedule();
			}
		}
	}
	break;
		
	case EGameActionType::TimeSetSchedule:
		if (UTimeSubsystem* Time = GetWorld()->GetGameInstance()->GetSubsystem<UTimeSubsystem>())
		{
			// [skydodo] 2026-10-13 버그 수정: 넘기다 하루가 지나 평일<->주말 시간표가 바뀌고 그쪽에 목표 시간대가 없으면
			// while이 끝나지 않았다 -> 이틀치 시간표 길이만큼만 넘기고, 그래도 못 만나면 경고 후 멈춘다
			const int32 TargetIndex = Time->GetTargetScheduleIndex(Action.Key);
			if (TargetIndex > -1)
			{
				const int32 MaxSteps = FMath::Max(Time->GetScheduleDefs().Num(), 1) * 2;
				int32 Steps = 0;
				while (Time->GetNowScheduleName() != Action.Key && Steps++ < MaxSteps)
				{
					Time->SpendSchedule();
				}
				if (Time->GetNowScheduleName() != Action.Key)
				{
					UE_LOG(LogTemp, Warning, TEXT("시간대 변경 액션: %d칸을 넘겨도 '%s'에 도달하지 못해 멈췄습니다."),
						MaxSteps, *Action.Key.ToString());
				}
			}
		}
		break;

	case EGameActionType::TimeResetSchedule:
		if (UTimeSubsystem* Time = GetWorld()->GetGameInstance()->GetSubsystem<UTimeSubsystem>())
		{
			Time->ResetSchedule();
		}
		break;
		
		// ---- AI 연동 ----

	case EGameActionType::NpcSetAIState:
		if (const ABaseNpcCharacter* Npc = FindNpcById(Action.Key))
		{
			if (AMainNpcAIController* AI = Cast<AMainNpcAIController>(Npc->GetController()))
			{
				AI->SetAIState(static_cast<ENpcAIState>(Action.Value));
			}
		}
		break;

	case EGameActionType::NpcFollowPlayer:
		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(FindNpcById(Action.Key)))
		{
			if (APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0))
			{
				MainNpc->StartFollowTarget(PlayerPawn);
			}
		}
		break;

	case EGameActionType::NpcStopFollow:
		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(FindNpcById(Action.Key)))
		{
			MainNpc->StopFollowTarget();
		}
		break;

	case EGameActionType::NpcStartPatrol:
		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(FindNpcById(Action.Key)))
		{
			if (AMainNpcAIController* AI = Cast<AMainNpcAIController>(MainNpc->GetController()))
			{
				// Key2 = RouteId. 비워두면 NPC의 기본 경로를 쓴다.
				AI->StartPatrol(MainNpc->GetRoute(Action.Key2), static_cast<bool>(Action.Value));
			}
		}
		break;

	case EGameActionType::NpcStartGuide:
		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(GetCharacterInCache(Action.Key)))
		{
			if (AMainNpcAIController* AI = Cast<AMainNpcAIController>(MainNpc->GetController()))
			{
				// 이벤트마다 다른 경로를 쓸 수 있도록 Key2로 경로를 지정한다.
				AI->StartGuide(MainNpc->GetRoute(Action.Key2));
			}
		}
		break;

	case EGameActionType::NpcStopRouteMove:
		if (const ABaseNpcCharacter* Npc = FindNpcById(Action.Key))
		{
			if (AMainNpcAIController* AI = Cast<AMainNpcAIController>(Npc->GetController()))
			{
				AI->StopRouteMovement();
			}
		}
		break;

	case EGameActionType::NpcResumeAI:
		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(FindNpcById(Action.Key)))
		{
			MainNpc->ResumeAIDuringTalk();
		}
		break;

	case EGameActionType::NpcPauseAI:
		if (AMainNpcCharacter* MainNpc = Cast<AMainNpcCharacter>(FindNpcById(Action.Key)))
		{
			MainNpc->PauseAIDuringTalk();
		}
		break;
		
	case EGameActionType::StartCrowdSpawn:
		// 레벨에 하나뿐인 행인 스포너가 Key2 이름의 행렬을 내보낸다. Key는 쓰지 않는다.
		if (Action.Key2.IsNone())
		{
			UE_LOG(LogTemp, Warning, TEXT("StartCrowdSpawn: 행렬 이름(Key2)이 지정되지 않음"));
		}
		else if (ANpcCrowdSpawner* Spawner = Cast<ANpcCrowdSpawner>(UGameplayStatics::GetActorOfClass(this, ANpcCrowdSpawner::StaticClass())))
		{
			Spawner->PlayEventCrowd(Action.Key2);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("StartCrowdSpawn: NpcCrowdSpawner를 찾을 수 없음"));
		}
		break;
		
		// ---- 애니메이션 ----

	case EGameActionType::PlayAnim:
	{
		UCharacterAnimationComponent* AnimationComponent = nullptr;
		if (Action.Key.IsNone() || Action.Key == "Player")
		{
			if (AFebriculaCharacter* FC = Cast<AFebriculaCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0)))
			{
				AnimationComponent = FC->GetComponentByClass<UCharacterAnimationComponent>();
			}
		}
		else
		{
			if (ABaseNpcCharacter* Npc = FindNpcById(Action.Key))
			{

				AnimationComponent = Npc->GetComponentByClass<UCharacterAnimationComponent>();
			}
		}
		if (AnimationComponent)
		{
			AnimationComponent->PlayAnim(Action.Key2, Action.Value != 0);
			//AnimationComponent->PlayNpcAnim(Action.Key2, /*bLoop=*/Action.Value != 0);
		}
		break;
	}
	case EGameActionType::StopAnim:
	{
		UCharacterAnimationComponent* AnimationComponent = nullptr;
		if (Action.Key.IsNone() || Action.Key == "Player")
		{
			if (AFebriculaCharacter* FC = Cast<AFebriculaCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0)))
			{
				AnimationComponent = FC->GetComponentByClass<UCharacterAnimationComponent>();
			}
		}
		else
		{
			if (ABaseNpcCharacter* Npc = Cast<ABaseNpcCharacter>(GetCharacterInCache(Action.Key)))
			{
				AnimationComponent = Npc->GetComponentByClass<UCharacterAnimationComponent>();
			}
		}

		if (AnimationComponent)
		{
			AnimationComponent->StopAnim();
		}
		break;
	}
	case EGameActionType::NpcFacePlayer:
		if (ABaseNpcCharacter* Npc = Cast<ABaseNpcCharacter>(GetCharacterInCache(Action.Key)))
		{
			Npc->FaceTowards(UGameplayStatics::GetPlayerPawn(this, 0));
		}
		break;

	case EGameActionType::NPCFaceNPC:
		if (Action.Key == "Player")
		{
			if (ACharacter* Player = UGameplayStatics::GetPlayerCharacter(this, 0))
			{
				if (AFebriculaCharacter* PlayerCharacter = Cast<AFebriculaCharacter>(Player))
				{
					if (ABaseNpcCharacter* Npc = Cast<ABaseNpcCharacter>(GetCharacterInCache(Action.Key2)))
					{
						PlayerCharacter->FaceTowards(Npc);
					}
				}
			}
		}
		else
		{
			if (FCharacterCache* NpcPtr = CachedCharacter.Find(Action.Key))
			{
				if (ABaseNpcCharacter* Npc = Cast<ABaseNpcCharacter>(NpcPtr->Character.Get()))
				{
					if (FCharacterCache* Character = CachedCharacter.Find(Action.Key2))
					{
						Npc->FaceTowards(Character->Character.Get());
					}
				}
			}
		}
		break;


	case EGameActionType::ChangeLevel:
		if (const UActivitySetting* ActivitySetting = GetDefault<UActivitySetting>())
		{
			OnDialogueBoxHidden.Broadcast();
			if (Action.Key == NAME_None)
			{
				if (ACharacter* Player = UGameplayStatics::GetPlayerCharacter(this, 0))
				{
					if (UInteractionComponent* InteractionComponent = Player->GetComponentByClass<UInteractionComponent>())
					{
						FName TargetLevelName = InteractionComponent->GetFocusedActorTag();
						if (TargetLevelName == NAME_None)
						{
							UE_LOG(LogTemp, Warning, TEXT("레벨 자동 식별 실패"));
							break;
						}
						if (const TSoftObjectPtr<UWorld>* Level = ActivitySetting->Levels.Find(TargetLevelName))
						{
							FGameAction QueuedAction = FGameAction(EGameActionType::ChangeLevel, TargetLevelName, TEXT("false"), 0);
							QueuedLevelChange.Add(QueuedAction);
						}
					}
				}
			}
			if (const TSoftObjectPtr<UWorld>* Level = ActivitySetting->Levels.Find(Action.Key))
			{
				if (!Action.Key2.ToString().ToBool())
				{
					UGameplayStatics::OpenLevelBySoftObjectPtr(this, *Level);
					if (Progress)
					{
						Progress->AddMoveHistory(FName(Action.Key));
					}
					if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
					{
						ActivitySubsystem->bFreeActivity = false;
					}
				}
				else
				{
					FGameAction QueuedAction = FGameAction(EGameActionType::ChangeLevel, Action.Key, TEXT("false"), 0);
					QueuedLevelChange.Add(QueuedAction);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("존재하지 않는 레벨: %s"), *Action.Key.ToString());
			}
		}
		
		
		break;

	case EGameActionType::FadeOut:
		if (APlayerController* PC = (UGameplayStatics::GetPlayerController(this, 0)))
		{
			if (Action.Value > 0)
			{
				if (Action.Key == "Black")
				{
					PC->PlayerCameraManager->StartCameraFade(0.f, 1.f, Action.Value * .1f, FColor::Black, false, true);
				}
				else if (Action.Key == "White")
				{
					PC->PlayerCameraManager->StartCameraFade(0.f, 1.f, Action.Value * .1f, FColor::White, false, true);
				}
			}
			else
			{
				if (Action.Key == "Black")
				{
					PC->PlayerCameraManager->SetManualCameraFade(1.f, FColor::Black, false);
				}
				else if (Action.Key == "White")
				{
					PC->PlayerCameraManager->SetManualCameraFade(1.f, FColor::White, false);
				}

			}
		}
		break;

	case EGameActionType::FadeIn:
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
		{
			PC->PlayerCameraManager->StartCameraFade(1.f, .0f, 1.f, FColor::Black, false, false);
		}
		break;

	case EGameActionType::SpawnActor:
		if (AFebriculaGameMode* GameMode = Cast<AFebriculaGameMode>(GetWorld()->GetAuthGameMode()))
		{
			FCharacterCache* Found = CachedCharacter.Find(Action.Key);
			if (Found && Found->Character.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("%s: 이미 존재하는 NPC"), *Action.Key.ToString());
				break;
			}
			UClass* SpawnClass = GameMode->GetNpcViaName(Action.Key).LoadSynchronous();
			if (!SpawnClass)
			{
				UE_LOG(LogTemp, Warning, TEXT("%s: 등록되지 않은 NPC Id"), *Action.Key.ToString());
				break;
			}
			if (Action.Key2.IsNone())
			{
				UE_LOG(LogTemp, Warning, TEXT("%s: 위치 정보 누락됨"), *Action.Key.ToString());
				break;
			}
			
			if (UActorCacheSubsystem* CacheSubsystem = GetWorld()->GetSubsystem<UActorCacheSubsystem>())
			{
				FTransform SpawnTransform = CacheSubsystem->GetTargetTransform(Action.Key2);
				// if (FoundedLocation.IsEmpty())
				// {w
				// 	UE_LOG(LogTemp, Warning, TEXT("%s: 지정된 스폰 위치 %s를 찾지 못함 "), *Action.Key.ToString(), *Action.Key2.ToString());
				// 	break;
				// }
				
				ABaseNpcCharacter* Character = GetWorld()->SpawnActorDeferred<ABaseNpcCharacter>(SpawnClass, SpawnTransform);
				if (Character)
				{
					Character->SetNpcId(Action.Key);
					Character->FinishSpawning(SpawnTransform);

				}
				break;
			}
		}
		break;

	case EGameActionType::ShowActor:
		ChangeActorHidden(Action.Key, false);
		break;

	case EGameActionType::HideActor:
		ChangeActorHidden(Action.Key, true);
		break;

	case EGameActionType::Teleport:
		{
			AActor* NowActor = nullptr;
			if (Action.Key.IsNone() || Action.Key == "Player")
			{
				NowActor = UGameplayStatics::GetPlayerCharacter(this, 0);
			}
			else
			{
				NowActor = GetCharacterInCache(Action.Key);	
			}
		
			if (NowActor)
			{
				if (ACharacter* NowCharacter = Cast<ACharacter>(NowActor))
				{
					if (UCharacterMovementComponent* Movement = NowCharacter->GetCharacterMovement())
					{
						Movement->StopMovementImmediately();
						Movement->DisableMovement();
						Movement->SetMovementMode(MOVE_None);
						Movement->SetComponentTickEnabled(false);
						NowCharacter->SetActorEnableCollision(false);
						NowCharacter->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						NowCharacter->GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						NowCharacter->GetMesh()->SetSimulatePhysics(false);
						NowCharacter->GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						TArray<AActor*> Found;
						UGameplayStatics::GetAllActorsWithTag(this, Action.Key2, Found);
						if (Found.Num() > 0)
						{
							NowActor->SetActorLocationAndRotation(Found[0]->GetActorLocation(), Found[0]->GetActorRotation());
						}
					}
				}
			}
			break;
		}
	case EGameActionType::Interact:
		{
			if (UActorCacheSubsystem* CacheSubsystem = GetWorld()->GetSubsystem<UActorCacheSubsystem>())
			{
				if (UTargetTypeComponent* Comp = CacheSubsystem->GetTargetTypeComponent(Action.Key))
				{
					Comp->ActivateTarget();
					break;
				}
			}
			TArray<AActor*> Founds;
			UGameplayStatics::GetAllActorsWithTag(this, Action.Key, Founds);
			if (Founds.IsEmpty()) break;
			if (AMovableMapActor* MapActor = Cast<AMovableMapActor>(Founds[0]))
			{
				if (!Action.Key2.IsNone())
				{
					UE_LOG(LogTemp, Warning, TEXT("Interact Instant: %s : %d"), *Action.Key2.ToString(), Action.Key2.ToString().ToBool())
					MapActor->StartMoveWrapper(Founds[0], EMoveState::OPENING, Action.Key2.ToString().ToBool());
					break;
				}
				MapActor->StartMoveWrapper(Founds[0], EMoveState::OPENING, false);
			}
			break;
		}
	case EGameActionType::UnInteract:
		{
			TArray<AActor*> Founds;
			UGameplayStatics::GetAllActorsWithTag(this, Action.Key, Founds);
			if (Founds.IsEmpty()) break;
			if (AMovableMapActor* MapActor = Cast<AMovableMapActor>(Founds[0]))
			{
				if (!Action.Key2.IsNone())
				{
					MapActor->StartMove(Founds[0], Action.Key2.ToString().ToBool());
					break;
				}
				MapActor->StartMoveWrapper(Founds[0], EMoveState::OPENING, false);
			}
			break;
		}
		
	case EGameActionType::ConsumeAP:
		if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
		{
			ActivitySubsystem->TryConsumeAP(Action.Value);
		}
		break;
		
	case EGameActionType::PlayBGM:
		if (USoundSubsystem* SoundSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USoundSubsystem>())
		{
			if (USoundBase* Sound = SoundSubsystem->FoundSoundBase(Action.Key))
			{
				SoundSubsystem->PlayBGM(Sound, FCString::Atof(*Action.Key2.ToString()));
			}
		}
		break;
		
	case EGameActionType::StopBGM:
		if (USoundSubsystem* SoundSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USoundSubsystem>())
		{
			SoundSubsystem->StopBGM(FCString::Atof(*Action.Key.ToString()));
		}
		break;
		
	case EGameActionType::PlaySE:
		if (USoundSubsystem* SoundSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USoundSubsystem>())
		{
			if (USoundBase* Sound = SoundSubsystem->FoundSoundBase(Action.Key2))
			{
				SoundSubsystem->PlaySE(Action.Key, Sound, Action.Value);
			}
		}
		break;
	
	case EGameActionType::SetSEVolume:
		if (USoundSubsystem* SoundSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USoundSubsystem>())
		{
			SoundSubsystem->SetSEVolume(Action.Key, FCString::Atoi(*Action.Key2.ToString()));
		}

		break;
		
	case EGameActionType::StopSE:
		if (USoundSubsystem* SoundSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USoundSubsystem>())
		{
			SoundSubsystem->StopSE(Action.Key, FCString::Atoi(*Action.Key2.ToString()));
		}
		break;
		
	case EGameActionType::FinishActivity:
		if (UActivitySubsystem* ActivitySubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UActivitySubsystem>())
		{
			// [skydodo] 2026-10-13 버그 수정: Value 0(고정 액티비티) 분기가 주석 처리돼 "Key_0" NPC 이벤트로 처리됐다.
			// 고정 액티비티는 레벨 이름으로 태그를 만드는 ActivityFinished(FName)으로 끝내야 NowPlayedActivity와 맞는다
			if (Action.Value == 0)
			{
				ActivitySubsystem->ActivityFinished(Action.Key);
				break;
			}
			ActivitySubsystem->ActivityFinished(Action.Key, Action.Value);
		}
		break;
		
	default:
		break;
	}
}

void UDialogueSubsystem::ChangeActorHidden(FName ActorName, bool bHidden)
{
	ACharacter* NowChar = nullptr;

	if (ActorName.IsNone() || ActorName == "Player")
	{
		NowChar = UGameplayStatics::GetPlayerCharacter(this, 0);
	}
	else
	{
		if (!CachedCharacter.Contains(ActorName)) return;
		NowChar = GetCharacterInCache(ActorName);
	}

	if (NowChar)
	{
		NowChar->SetActorHiddenInGame(bHidden);
	}
}


// ---------------------------------------------------------------------------
// 내부 헬퍼
// ---------------------------------------------------------------------------

UGameProgressSubsystem* UDialogueSubsystem::GetProgress() const
{
	if (const UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			return GI->GetSubsystem<UGameProgressSubsystem>();
		}
	}
	return nullptr;
}

UNpcAffinitySubsystem* UDialogueSubsystem::GetRelationship() const
{
	if (const UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			return GI->GetSubsystem<UNpcAffinitySubsystem>();
		}
	}
	return nullptr;
}

ABaseNpcCharacter* UDialogueSubsystem::FindNpcById(FName NpcId) const
{
	UWorld* World = GetWorld();
	if (!World || NpcId.IsNone())
	{
		return nullptr;
	}

	for (TActorIterator<ABaseNpcCharacter> It(World); It; ++It)
	{
		if (It->GetNpcId() == NpcId)
		{
			return *It;
		}
	}
	return nullptr;
}

void UDialogueSubsystem::BroadcastDialogueLine(const FText& Speaker, const FText& Body)
{
	if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UDialogueLogSubsystem* Log = GI->GetSubsystem<UDialogueLogSubsystem>())
		{
			Log->AddLine(Speaker, Body);
		}
	}
	OnDialogueLine.Broadcast(Speaker, Body);
}

#if !UE_BUILD_SHIPPING
// [skydodo] 2026-10-17 추가: 테스트용 콘솔 명령. 키 입력 없이 메인 NPC와 대화를 시작해 대화창을 확인한다
// 사용: dialogue.TalkNpc [NpcId]  (NpcId를 빼면 플레이어와 가장 가까운 메인 NPC)
static FAutoConsoleCommandWithWorldAndArgs GDialogueTalkNpcCommand(
	TEXT("dialogue.TalkNpc"),
	TEXT("dialogue.TalkNpc [NpcId] - 지정한 메인 NPC(없으면 가장 가까운 NPC)와 대화를 시작한다"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Player = PC ? PC->GetPawn() : nullptr;
		if (!Player)
		{
			UE_LOG(LogTemp, Warning, TEXT("dialogue.TalkNpc: 플레이어 폰이 없습니다. PIE 중에 실행하세요."));
			return;
		}

		const FName WantedId = Args.Num() > 0 ? FName(*Args[0]) : NAME_None;
		AMainNpcCharacter* Best = nullptr;
		double BestDistSq = TNumericLimits<double>::Max();
		for (TActorIterator<AMainNpcCharacter> It(World); It; ++It)
		{
			if (!WantedId.IsNone() && It->GetNpcId() != WantedId)
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(It->GetActorLocation(), Player->GetActorLocation());
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = *It;
			}
		}

		if (!Best)
		{
			UE_LOG(LogTemp, Warning, TEXT("dialogue.TalkNpc: 메인 NPC를 찾지 못했습니다 (%s)"), *WantedId.ToString());
			return;
		}
		IInteractable::Execute_Interact(Best, Player);
	}));
#endif
