// [skydodo] 2026-08-17 작성 (최종 수정 2026-08-21)
// 요약: 대화 그래프 실행 + 이벤트 선택 + 액션 실행 런타임 코어
// Fill out your copyright notice in the Description page of Project Settings.
//
// 대화 그래프 실행 + 이벤트 선택 + 액션 실행을 담당하는 런타임 코어.
//
// 흐름:
//   1) 플레이어가 NPC와 상호작용 → TryStartEventForNpc(NpcId)
//   2) 이벤트 테이블에서 해당 NPC의 이벤트 중 전제조건을 만족하는 것을 Priority 순으로 고름
//   3) 그 이벤트의 RootNodeId부터 대화 테이블을 따라가며 진행
//   4) 노드마다 OnDialogueLine / OnDialogueChoices 델리게이트를 쏘고, HUD가 그걸 받아 표시
//   5) 대화가 끝나면 OnDialogueFinished → 이벤트 완료 처리 + NPC를 Talk 상태에서 해제

#pragma once

#include "CoreMinimal.h"
#include "CineCameraActor.h"
#include "Subsystems/WorldSubsystem.h"
#include "Dialogue/DialogueDataTypes.h"
#include "Dialogue/DialogueLogSubsystem.h"
#include "DialogueSubsystem.generated.h"

class ABaseNpcCharacter;
class UGameProgressSubsystem;
class UNpcAffinitySubsystem;


/** 대사 한 줄이 출력될 때. Speaker가 비어 있으면 나레이션(사물 조사 등) */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDialogueLine, const FText&, Speaker, const FText&, Body);

/** 선택지를 띄워야 할 때 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDialogueChoices, const TArray<FDialogueChoiceView>&, Choices, bool, bYesNo);

/** 대화가 완전히 끝났을 때 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDialogueFinished);

/**
 * 대화는 계속 진행 중이지만 창을 잠시 닫아야 할 때 (이동 대기 구간).
 * OnDialogueFinished와 달리 대화가 끝난 게 아니므로, HUD는 창만 숨기고 상태는 유지해야 한다.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDialogueBoxHidden);

USTRUCT(Blueprintable, BlueprintType)
struct FCharacterCache
{
	GENERATED_BODY()
public:
	TWeakObjectPtr<ACharacter> Character;
	FTransform Transform;
};

UCLASS()
class FEBRICULA_API UDialogueSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;	
	//~ Begin FTickableGameObject
	// 자동 진행(AutoAdvance) 조건을 매 틱 검사해야 해서 Tickable 서브시스템으로 둔다.
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject

	// ---- 데이터 테이블 주입 ----

	/**
	 * 사용할 DataTable을 등록한다. 보통 게임 시작 시 한 번(플레이어의 UInteractionComponent가 자동 호출).
	 * EventTable  : FNpcEventRow
	 * DialogueTable: FDialogueNodeRow
	 */
	UFUNCTION(BlueprintCallable, Category = "Dialogue")
	void SetDataTables(UDataTable* InEventTable, UDataTable* InDialogueTable);

	// ---- 대화 시작 ----

	/**
	 * NpcId에 해당하는 이벤트 중 지금 발생 가능한 것을 찾아 대화를 시작한다.
	 * 조건을 만족하는 이벤트가 하나도 없으면 false를 반환한다 (이때는 기본 잡담을 띄우면 된다).
	 */
	UFUNCTION(BlueprintCallable, Category = "Dialogue")
	bool TryStartEventForNpc(FName NpcId, AActor* InInstigator, FName DialogueNodeId = NAME_None);

	/**
	 * [skydodo] 2026-10-18 추가: Table에 RootNodeId로 시작하는 이벤트 중 지금 조건을 만족하는 게 있는지.
	 * 사물이 "지금 누를 수 있을 때만" 상호작용 문구를 띄우는 데 쓴다 (시작 조건은 TryStartEventForNpc와 같다)
	 */
	bool HasStartableEvent(const UDataTable* Table, FName RootNodeId) const;

	/** 특정 노드부터 직접 대화를 시작한다 (이벤트 테이블을 거치지 않는 경우) */
	UFUNCTION(BlueprintCallable, Category = "Dialogue")
	bool StartDialogueAtNode(FName NodeId, AActor* InInstigator);

	/**
	 * 사물 조사처럼 테이블에 없는 즉석 대사를 띄운다.
	 * ("시체인 것 같다." 같은 한 줄짜리 나레이션용)
	 */
	UFUNCTION(BlueprintCallable, Category = "Dialogue")
	bool StartSimpleLines(const FText& Speaker, const TArray<FText>& Lines, AActor* InInstigator);

	// ---- 진행 ----

	/** 대사창 클릭 등으로 다음으로 넘길 때 */
	UFUNCTION(BlueprintCallable, Category = "Dialogue")
	void Advance();

	/** 선택지를 골랐을 때 */
	UFUNCTION(BlueprintCallable, Category = "Dialogue")
	void SelectChoice(int32 ChoiceIndex, bool bYesNo = false);

	/** 강제 종료 */
	UFUNCTION(BlueprintCallable, Category = "Dialogue")
	void FinishDialogue();

	UFUNCTION(BlueprintPure, Category = "Dialogue")
	bool IsDialogueActive() const { return bDialogueActive; }

	/** 현재 대사 중인 NPC의 ID. 나레이션/SimpleLines면 NAME_None */
	UFUNCTION(BlueprintPure, Category = "Dialogue")
	FName GetCurrentSpeakerNpcId() const { return CurrentSpeakerNpcId; }

	/** 선택지 표시 중인가 (이 상태에선 Advance를 무시해야 한다) */
	UFUNCTION(BlueprintPure, Category = "Dialogue")
	bool IsWaitingForChoice() const { return bWaitingForChoice; }

	/**
	 * 자동 진행 조건을 기다리는 중인가.
	 * true면 "클릭해서 넘기기" 표시를 감추는 게 자연스럽다 (어차피 클릭이 안 먹으므로).
	 */
	UFUNCTION(BlueprintPure, Category = "Dialogue")
	bool IsWaitingForAutoAdvance() const { return PendingAutoAdvance != EDialogueAutoAdvance::None; }

	/**
	 * 대사 빠른 넘기기(스킵)를 켜거나 끈다.
	 * 켜져 있는 동안 Tick에서 일정 간격마다 Advance()를 대신 호출해준다.
	 * 선택지 대기 중이거나 자동 진행 대기 중에는 Advance() 자체가 무시하므로 자연히 멈춘다.
	 */
	UFUNCTION(BlueprintCallable, Category = "Dialogue | FastForward")
	void SetFastForward(bool bEnable);

	UFUNCTION(BlueprintPure, Category = "Dialogue | FastForward")
	bool IsFastForwarding() const { return bIsFastForwarding; }

	UFUNCTION(BlueprintCallable, Category = "Dialogue | Action")
	void ExecuteAction(const FGameAction& Action);
	void ChangeActorHidden(FName Name, bool bHidden);

	void ExecuteActions(const TArray<FGameAction>& Actions);

	UFUNCTION(BlueprintCallable, Category = "Dialogue | Input")
	void BlockPlayerInput(bool bLock);

	UFUNCTION(BlueprintCallable, Category = "Dialogue | Camera")
	void CameraToCinematicCam(const FVector Loc, const FRotator Rot, const float MoveTime, const float Focus);

	UFUNCTION(BlueprintCallable, Category = "Dialogue | Camera")
	void CameraToPlayerCam(const float MoveTime);

	//지연 생성된 위젯을 위한 함수
	UFUNCTION(BlueprintCallable, Category= "Dialogue | Dialogue Holder")
	bool GetCurrentLine(FText& OutSpeaker, FText& OutLine, TArray<FDialogueChoiceView>& OutChoiceViews);

	UFUNCTION(BlueprintCallable, Category = "Dialogue | Cahced Character")
	void AddCharacterByName(FName Name, ACharacter* Character);

	UFUNCTION(BlueprintCallable, Category = "Dialogue | Cahced Character")
	void RemoveCharacterByName(FName Name, ACharacter* Character);
	
	UFUNCTION(BlueprintCallable, Category = "Dialogue | Result")
	bool GetResult() { return RecentDialogueResult; }
	
	UFUNCTION(BlueprintCallable, Category = "Dialogue | ActionQueue")
	void ExecuteQueuedActions();
	
	void ActivityDialoguePrint(const FDialogueNodeRow& Row);
	

	// ---- HUD가 구독할 델리게이트 ----

	UPROPERTY(BlueprintAssignable, Category = "Dialogue")
	FOnDialogueLine OnDialogueLine;

	UPROPERTY(BlueprintAssignable, Category = "Dialogue")
	FOnDialogueChoices OnDialogueChoices;

	UPROPERTY(BlueprintAssignable, Category = "Dialogue")
	FOnDialogueFinished OnDialogueFinished;

	UPROPERTY(BlueprintAssignable, Category = "Dialogue")
	FOnDialogueBoxHidden OnDialogueBoxHidden;
	
	// UPROPERTY(BlueprintAssignable, Category = "Dialogue")
	// FOnChoiceSelected OnChoiceSelected;
	//
	TMap<FName, FCharacterCache> GetCachedCharacter() const;
	
	UFUNCTION(BlueprintCallable, Category = "Dialogue | Cache")
	FTransform GetDefaultTransform(FName NpcId);
	
	bool CheckNpcCached(FName NpcName) const;

protected:
	/** 노드로 진입해서 OnEnterActions 실행 + 대사 출력까지 */
	void EnterNode(FName NodeId);

	/** 대사를 다 읽은 뒤 다음 목적지(분기/다음노드)를 결정 */
	void ResolveNodeExit();

	/** 선택지를 HUD에 표시하고 입력을 선택 대기 상태로 바꾼다 */
	void PresentChoices(const TArray<FDialogueChoice>& InChoices, bool bYesNo = false);

	UGameProgressSubsystem* GetProgress() const;
	UNpcAffinitySubsystem* GetRelationship() const;

	/** 월드에서 NpcId가 일치하는 NPC 액터를 찾는다 (AI 액션 실행용) */
	ABaseNpcCharacter* FindNpcById(FName NpcId) const;

	/** 이벤트 참가자들을 Talk 상태로 넣는다 (여러 NPC가 함께 나오는 장면용) */
	void EnterTalkForParticipants(const TArray<FName>& NpcIds, AActor* Interactor);

	/** 참가자 전원을 Talk 상태에서 풀어준다 */
	void ReleaseParticipants();

	ACharacter* GetCharacterInCache(FName CharacterName) const;

	/** 이번 대화에 참여 중인 NPC들. 끝날 때 한꺼번에 풀어주기 위해 들고 있는다 */
	UPROPERTY()
	TArray<TObjectPtr<ABaseNpcCharacter>> ActiveParticipants;

	UPROPERTY()
	TObjectPtr<UDataTable> EventTable;

	UPROPERTY()
	TObjectPtr<UDataTable> DialogueTable;

	/** 지금 대화 중인 상대(NPC 또는 사물) */
	UPROPERTY()
	TObjectPtr<AActor> CurrentInstigator;

	/** 진행 중인 이벤트 행 이름. 끝날 때 완료 처리에 쓴다 */
	FName ActiveEventId;
	bool bCompleteEventOnFinish = false;

	/** 자동 진행 조건이 만족됐는지 검사한다 */
	bool IsAutoAdvanceSatisfied(float DeltaTime);

	FName CurrentNodeId;
	FName CurrentSpeakerNpcId;
	bool bDialogueActive = false;
	bool bWaitingForChoice = false;

	/** 현재 노드가 요구하는 자동 진행 방식. None이면 플레이어 클릭 대기 */
	EDialogueAutoAdvance PendingAutoAdvance = EDialogueAutoAdvance::None;
	float AutoAdvanceValue = 0.f;
	float AutoAdvanceElapsed = 0.f;

	/** 도착/근접 판정의 기준 NPC. 비어 있으면 CurrentInstigator 기준 */
	FName AutoAdvanceNpcId;

	/** 지금 표시 중인 선택지의 원본 (SelectChoice에서 참조) */
	TArray<FDialogueChoice> PendingChoices;

	/** 대사 빠른 넘기기(Ctrl 홀드 등) 상태 */
	bool bIsFastForwarding = false;

	/** 빠른 넘기기 중 다음 Advance()까지 누적된 시간 */
	float FastForwardElapsed = 0.f;

	/** 빠른 넘기기 시 한 줄 넘기는 간격(초). 값이 작을수록 더 빠르게 넘어간다 */
	UPROPERTY(EditDefaultsOnly, Category = "Dialogue | FastForward")
	float FastForwardInterval = 0.1f;

	/** StartSimpleLines로 띄운 즉석 대사 큐 */
	TArray<FText> SimpleLineQueue;
	FText SimpleLineSpeaker;
	bool bRunningSimpleLines = false;

	//인 게임 시네마틱용 카메라
	UPROPERTY()
	TObjectPtr<ACineCameraActor> CinematicCamera[2];

	int32 ActiveCamIndex;

	//자동재생 대사 홀더. 위젯 생성보다 Broadcast가 빨랐을 때 대비
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dialogue | Dialogue Holder")
	bool bHasHoldingLine;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dialogue | Dialogue Holder")
	FText HoldingSpeaker;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dialogue | Dialogue Holder")
	FText HoldingDialogue;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dialogue | Dialogue Holder")
	TArray<FDialogueChoiceView> HoldingChoices;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dialogue")
	bool bBlockPlayerInput;

	UPROPERTY()
	TMap<FName, FCharacterCache> CachedCharacter;
	
	UPROPERTY()
	TArray<FName> ClearedEvents;

	/** 대사 한 줄을 로그에 기록 + 델리게이트 브로드캐스트 */
	void BroadcastDialogueLine(const FText& Speaker, const FText& Body);
	
	UPROPERTY(BlueprintReadOnly, Category = "Dialogue | Result")
	bool RecentDialogueResult;
		
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Prop | Debug")
	TArray<FGameAction> QueuedLevelChange;
	
	int32 RoutingHopCount = 0;
	static constexpr int32 MaxRoutingHops = 16;
};

