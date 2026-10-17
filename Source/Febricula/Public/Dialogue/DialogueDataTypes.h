// [skydodo] 2026-08-17 작성 (최종 수정 2026-08-26)
// 요약: 대화/이벤트 트리 공용 타입 - 조건(FGameCondition)과 액션(FGameAction)을 데이터로 표현
// Fill out your copyright notice in the Description page of Project Settings.
//
// 대화 / 이벤트 트리 데이터의 공용 타입 모음.
//
// 설계 핵심: "조건(Condition)"과 "액션(Action)"을 데이터로 표현해서,
// 이벤트 트리 문서에 적힌 [전제 조건] / [분기] / [친밀도 상승] / [Tag 획득] 같은 항목을
// C++ 코드를 새로 짜지 않고 DataTable만 고쳐서 늘릴 수 있게 한다.
//
// 데이터는 DataTable 2개로 나뉜다:
//   - 이벤트 테이블 (FNpcEventRow)     : "언제 어떤 대화를 시작할지"
//   - 대화 테이블   (FDialogueNodeRow) : "그 대화가 어떻게 흘러가는지"

#pragma once

// 파일명 주의: 엔진에 이미 Sound/DialogueTypes.h가 있어서 이름이 겹치면 UHT가 빌드를 거부한다.
// 그래서 DialogueTypes.h가 아니라 DialogueDataTypes.h로 둔다.

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Engine/DataTable.h"
#include "Engine/TargetPoint.h"
#include "DialogueDataTypes.generated.h"

/**
 * 조건 종류. 이벤트 전제조건, 대화 자동분기, 선택지 노출 조건에 공통으로 쓴다.
 * 각 항목 옆 주석은 FGameCondition의 Key / Value를 어떻게 채우는지를 뜻한다.
 */
UENUM(BlueprintType)
enum class EGameConditionType : uint8
{
	None					UMETA(DisplayName = "없음(항상 참)"),

	/** Value일차 이상 진행됐는가 (예: 14일차 이상). 기준은 UTimeSubsystem의 누적 일차 */
	DayAtLeast				UMETA(DisplayName = "N일차 이상"),

	/** Key 이벤트를 이미 완료했는가 */
	EventCompleted			UMETA(DisplayName = "이벤트 완료함"),

	/** Key 이벤트를 아직 완료하지 않았는가 */
	EventNotCompleted		UMETA(DisplayName = "이벤트 미완료"),

	/** Key(NpcId)와의 관계가 Value(ENpcRelationshipLevel) 이상인가 */
	RelationshipAtLeast		UMETA(DisplayName = "관계 O 이상"),

	/** Key 태그를 보유했는가 (예: 일름보, 철두철미) */
	HasTag					UMETA(DisplayName = "태그 보유"),

	/** Key 태그가 없는가 */
	NotHasTag				UMETA(DisplayName = "태그 미보유"),

	/** Key 카운터가 Value 이상인가 (예: Violation >= 3) */
	CounterAtLeast			UMETA(DisplayName = "카운터 N 이상"),

	/** Key 카운터가 Value 이하인가 */
	CounterAtMost			UMETA(DisplayName = "카운터 N 이하"),
	
	/* 잔여 AP가 N 이상인가 */
	APAtLeat				UMETA(DisplayName = "잔여 AP N 이상"),
	
	APAtMost				UMETA(DisplayName = "잔여 AP N 이하"),

	/** Key 능력치가 Value 이상인가 (예: Charm >= 5) - 매력/담력 선택지 조건용 */
	StatAtLeast				UMETA(DisplayName = "능력치 N 이상"),

	/** 주사위 판정. Key 능력치를 보정으로 쓰고 Value를 목표치로 삼는다 */
	RollCheck				UMETA(DisplayName = "판정(주사위)"),

	// ---- 시간/날짜 (UTimeSubsystem 연동) ----
	// 여기서 말하는 "일차"는 전부 누적 일차(GetAbsoluteDay)다.
	// 달이 바뀌어도 리셋되지 않으므로 "14일차 이벤트"를 그대로 표현할 수 있다.

	/** 정확히 Value일차인가 (예: 3일차에만 발생하는 이벤트) */
	DayEquals				UMETA(DisplayName = "N일차 정확히"),

	/** Value일차 이하인가 (예: 7일차까지만 유효한 튜토리얼) */
	DayAtMost				UMETA(DisplayName = "N일차 이하"),
	
	/** 현재 달이 Value인가 */
	MonthEquals				UMETA(DisplayName = "N월"),

	/**
	 * 현재 시간대가 Key와 같은가.
	 * Key에는 스케줄의 GameplayTag 전체 이름을 그대로 적는다 (예: Schedule.MorningClass).
	 */
	ScheduleIs				UMETA(DisplayName = "현재 시간대가 O"),

	ScheduleAfter			UMETA(DisplayName = "현재 시간대가 O 이후"),
	
	ScheduleBefore			UMETA(DisplayName = "현재 시간대가 O 이전"),
	
	NextScheduleIs			UMETA(DisplayName = "다음 시간대가 0"),
	
	/** 현재 시간대가 Key 태그의 하위인가 (예: Key=Schedule.FreeTime 이면 FreeTime.1/2/3 전부 통과) */
	ScheduleIsChildOf		UMETA(DisplayName = "현재 시간대가 O 계열"),

	/**
	 * 오늘의 "날 종류"가 Key와 같은가. Key에는 DayType.WeekDay 같은 태그 이름을 적는다.
	 * 시간대(Schedule.*)와는 독립된 축이라, 시험기간/방학처럼 종류가 늘어나도 조건은 그대로 쓴다.
	 */
	DayTypeIs				UMETA(DisplayName = "오늘이 O 종류의 날"),

	/** 오늘의 날 종류가 Key 태그 계열인가 (예: Key=DayType.Holiday 면 그 하위 전부) */
	DayTypeIsChildOf		UMETA(DisplayName = "오늘이 O 계열의 날"),

	/** 주말인가 (누적 일차 기준 6, 7일째). 단순 요일 계산이라 DayTypeIs와는 별개다 */
	IsWeekend				UMETA(DisplayName = "주말"),

	/** 평일인가 */
	IsWeekday				UMETA(DisplayName = "평일"),
	
	/** 현재 레벨이 N인가 */
	NowLevel				UMETA(DisplayName = "현재 레벨이 N"),
	
	NotNowLevel				UMETA(DisplayName = "현재 레벨이 N이 아니면"),
	
	PreviousLevelIs			UMETA(DisplayName = "이전 레벨이 N")

};

/**
 * 액션 종류. 대화 노드 진입 시 / 선택지 선택 시 / 분기 확정 시 실행된다.
 */
UENUM(BlueprintType)
enum class EGameActionType : uint8
{
	None					UMETA(DisplayName = "없음"),

	/** Key(NpcId) 친밀도를 Value만큼 증감 (음수 가능) */
	AddAffinity				UMETA(DisplayName = "친밀도 증감"),

	/** Key 태그 부여 */
	GrantTag				UMETA(DisplayName = "태그 획득"),

	/** Key 태그 제거 */
	RemoveTag				UMETA(DisplayName = "태그 제거"),

	/** Key 카운터를 Value만큼 증감 */
	AddCounter				UMETA(DisplayName = "카운터 증감"),
	
	AddStat					UMETA(DisplayName = "스탯 증감"),

	/** Key 이벤트를 완료 처리 */
	CompleteEvent			UMETA(DisplayName = "이벤트 완료 처리"),

	/** Key 대화 주제 획득 (다른 NPC와 이 주제로 대화 가능해짐) */
	GrantTopic				UMETA(DisplayName = "대화 주제 획득"),

	// ---- AI 연동 ----

	/** Key(NpcId) NPC를 Value(ENpcAIState: 0=Idle,1=Patrol,2=Guide,3=Chase)로 전환 */
	NpcSetAIState			UMETA(DisplayName = "NPC AI 상태 변경"),

	/** Key(NpcId) NPC가 플레이어를 졸졸 따라오게 함 (NPC가 뒤를 따름) */
	NpcFollowPlayer			UMETA(DisplayName = "NPC 플레이어 추적 시작"),

	/** Key(NpcId) NPC 추적 중단 */
	NpcStopFollow			UMETA(DisplayName = "NPC 추적 중단"),

	/**
	 * Key(NpcId) NPC가 경로를 혼자 계속 돈다. 지점에 도착하면 바로 다음 지점으로 간다.
	 * Key2 = RouteId (비우면 NPC의 DefaultRoute). 이벤트마다 다른 경로를 쓸 수 있다.
	 */
	NpcStartPatrol			UMETA(DisplayName = "NPC 순찰 시작 (혼자)"),

	/**
	 * Key(NpcId) NPC가 경로를 따라가되, 각 지점에서 플레이어가 따라올 때까지 기다린다.
	 * 학교 안내, 산책 동행 같은 "데리고 다니는" 연출용.
	 * Key2 = RouteId (비우면 NPC의 DefaultRoute).
	 */
	NpcStartGuide			UMETA(DisplayName = "NPC 안내 시작 (플레이어 대기)"),

	/** Key(NpcId) 순찰/안내 공통 중단 → Idle */
	NpcStopRouteMove		UMETA(DisplayName = "NPC 순찰·안내 중단"),

	/**
	 * Key(NpcId) 대화 중에도 BehaviorTree를 돌린다.
	 * 기본적으로 대화가 시작되면 NPC는 제자리에 멈추는데("대화 중엔 안 움직인다"가 보통 맞으므로),
	 * "걸으면서 대화하는" 연출에서는 이 액션으로 AI를 다시 켜 줘야 한다.
	 */
	NpcResumeAI				UMETA(DisplayName = "대화 중 NPC AI 재개"),

	/** Key(NpcId) 다시 제자리에 세운다 */
	NpcPauseAI				UMETA(DisplayName = "대화 중 NPC AI 정지"),

	StartCrowdSpawn			UMETA(DisplayName = "군중 소환 개시"),
	
	// ---- 애니메이션 ----

	/**
	 * Key(NpcId) NPC가 Key2(AnimId) 애니메이션을 재생한다.
	 * AnimId는 NPC 블루프린트의 AnimLibrary 맵에 등록해둔 이름이다.
	 * Value: 0 = 한 번만, 1 = 반복 (앉아있기 같은 지속 포즈)
	 * AnimSequence를 슬롯에 얹어 재생하므로 이동/로코모션은 아래에서 계속 돈다.
	 */
	
	/* 
	 * NPC 외 플레이어도 공유할 수 있도록 리팩토링
	 */
	PlayAnim				UMETA(DisplayName = "애니메이션 재생"),

	/** Key(NpcId) 재생 중인 슬롯 애니메이션을 중단하고 원래 상태로 블렌드 */
	StopAnim				UMETA(DisplayName = "애니메이션 중단"),

	/** Key(NpcId) NPC가 플레이어 쪽을 바라보게 한다 */
	NpcFacePlayer			UMETA(DisplayName = "NPC가 플레이어 바라보기"),
	
	/** Key(NpcId) NPC가 Key2(NpcId) NPC 쪽을 바라보게 한다 */
	NPCFaceNPC				UMETA(DisplayName = "NPC가 NPC 바라보기"),

	// ---- 시간 진행 (UTimeSubsystem 연동) ----

	/**
	 * 하루를 넘긴다 (UTimeSubsystem::SpendDay). Value가 2 이상이면 그만큼 반복.
	 * 이벤트 마지막 노드에 걸어두면 "이벤트가 끝나면 다음 날"이 되어 날짜별 테스트가 쉬워진다.
	 * 시간대 인덱스는 그대로이므로, 시간표 순서까지 정확히 맞춰야 하면 TimeAdvanceSchedule을 쓴다.
	 */
	TimeAdvanceDay			UMETA(DisplayName = "다음 날로"),

	/**
	 * 시간대를 한 칸 넘긴다 (UTimeSubsystem::SpendSchedule). Value가 2 이상이면 그만큼 반복.
	 * 시간표 끝에 닿으면 내부에서 알아서 하루가 넘어간다.
	 */
	TimeAdvanceSchedule		UMETA(DisplayName = "다음 시간대로"),
	TimeSetSchedule			UMETA(DisplayName = "시간대 변경"),
	
	TimeResetSchedule		UMETA(DisplayName = "시간대 리셋"),
	
	
	/**  
	 *  NPC가 아닌 상호작용 가능한 일반 액터와의 액션도 처리하기 위해 추가된 Action	 
	 *  문 / 노트북 / 기타 액티비티용 액터 등이 FGameAction 체계 공유하기 위해 추가됨
	 */
	
	/**
	 * 특정 레벨로 이동한다.
	 * 레벨명은 Project Setting - Activity Setting - Levels 에 맵핑되어 있다.
	 * Key는 목적지가 되는 레벨명
	 */
		
	ChangeLevel				UMETA(DisplayName= "레벨 이동"),
	
	MoveCamera				UMETA(DisplayName= "카메라 이동"),
	
	FadeOut					UMETA(DisplayName= "페이드 아웃"),
	
	FadeIn					UMETA(DisplayName = "페이드 인"),
	
	SpawnActor				UMETA(DisplayName = "캐릭터 스폰"),
	
	ControlActor			UMETA(DisplayName = "액터 제어"),
	
	ShowActor				UMETA(DisplayName = "캐릭터 보임"),
	
	HideActor				UMETA(DisplayName = "캐릭터 숨김"),	
	
	Teleport				UMETA(DisplayName = "특정 위치로 순간이동"),

	Interact				UMETA(DisplayName = "상호작용 후 상태로 설정"),
	
	UnInteract				UMETA(DisplayName = "상호작용 전 상태로 설정"),
	
	ConsumeAP				UMETA(DisplayName = "AP 소모"),
	
	PlayBGM					UMETA(DisplayName = "BGM 재생"),
	
	StopBGM					UMETA(DisplayName = "BGM 정지"),
	
	PlaySE					UMETA(DisplayName = "SE 재생"),
	
	StopSE					UMETA(DisplayName = "SE 정지"),
	
	SetSEVolume				UMETA(DisplayName = "SE 볼륨"),
	
	FinishActivity			UMETA(DisplayName = "액티비티 종료"),

	/**
	 * Key(NpcId 또는 액터 태그) 위에 목표 화살표를 띄우고 Key2를 화면 왼쪽 위에 쓴다.
	 * Key2는 FName이라 띄어쓰기 대신 '_'로 쓰면 공백으로 바꿔 보여 준다 (도윤에게_말_걸기)
	 */
	SetObjective			UMETA(DisplayName = "목표 표시"),

	ClearObjective			UMETA(DisplayName = "목표 지우기"),

	/** Key(MiniGame Setting의 키, 예: Burger) 미니게임을 레벨 이동 없이 지금 레벨에서 띄운다 */
	StartMiniGame			UMETA(DisplayName = "미니게임 시작")
};

/**
 * 대사를 다음으로 넘기는 방식.
 * 기본은 플레이어 클릭이지만, 이동 연출과 대사를 맞물리게 하려면 자동 진행이 필요하다.
 */
UENUM(BlueprintType)
enum class EDialogueAutoAdvance : uint8
{
	/** 플레이어가 클릭(또는 E)해야 넘어간다 */
	None				UMETA(DisplayName = "클릭 대기 (기본)"),

	/** AutoAdvanceValue 초가 지나면 자동으로 넘어간다 */
	AfterDelay			UMETA(DisplayName = "N초 후 자동"),

	/** 플레이어가 대화 상대에게 AutoAdvanceValue 이내로 접근하면 넘어간다 */
	WhenPlayerNearNpc	UMETA(DisplayName = "플레이어가 NPC에 접근하면"),

	/**
	 * NPC가 현재 경로 지점에 도착하면 넘어간다 (AutoAdvanceValue = 도착 판정 반경).
	 * "따라와" 대사를 띄운 채로 NPC가 다음 지점까지 걸어가게 만들 때 쓴다.
	 */
	WhenNpcArrived		UMETA(DisplayName = "NPC가 경로 지점에 도착하면"),
};

UENUM(BlueprintType)
enum class EGameCameraType : uint8
{
	None,	
	Player,
	Cinematic
};

/**
 * 데이터로 표현하는 단일 조건.
 * 여러 개를 배열로 두면 전부 만족해야 통과(AND)로 동작한다.
 */
USTRUCT(BlueprintType)
struct FGameCondition
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	EGameConditionType Type = EGameConditionType::None;

	/** 대상 식별자. NpcId / 태그 이름 / 카운터 이름 / 능력치 이름 등 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	FName Key;

	/** 비교값. 일차, 카운터 수치, 관계 등급(ENpcRelationshipLevel의 정수값) 등 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	int32 Value = 0;
	
	FGameCondition() = default;
	
	FGameCondition(EGameConditionType Type, FName Key, int32 Value) : Type(Type), Key(Key), Value(Value) {};
};

/**
 * 데이터로 표현하는 단일 액션.
 */
USTRUCT(BlueprintType)
struct FGameAction
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action")
	EGameActionType Type = EGameActionType::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action")
	FName Key;

	/**
	 * 식별자가 두 개 필요한 액션에서 쓰는 보조 키.
	 * 예: NpcPlayAnim은 Key=NpcId, Key2=AnimId. 나머지 액션에서는 비워둔다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action")
	FName Key2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action")
	int32 Value = 0;
	
	FGameAction() = default;
	
	FGameAction(EGameActionType Type, FName Key, FName Key2, int32 Value) : Type(Type), Key(Key), Key2(Key2), Value(Value) {}
};

USTRUCT(BlueprintType)
struct FGameCamera
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	EGameCameraType CameraType = EGameCameraType::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector Location = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float MoveTime = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float Focus = -1000.f;

};

USTRUCT(BlueprintType)
struct FSoundLocation
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound")
	FName SoundChannel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound")
	FName SoundName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound")
	FName Target;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound")
	float Blend = 0.f;
	
};

/**
 * 플레이어 선택지 하나.
 * Conditions를 만족하지 못하면 목록에 보이되 회색으로 비활성화된다
 * ("매력이 부족해서 못 고르는 선택지"가 있다는 걸 플레이어가 알 수 있어야 하므로).
 */
USTRUCT(BlueprintType)
struct FDialogueChoice
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	FText Text;

	/** 선택 가능 조건. 비어 있으면 항상 선택 가능 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	TArray<FGameCondition> Conditions;

	/** 이 선택지를 골랐을 때 실행할 액션 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	TArray<FGameAction> Actions;
	
	/** 다음 노드. None이면 대화 종료 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	FName NextNodeId;
	
	/** FreeAction 여부 */ 
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Choice")
	bool bFreeActivity = false;
	
	/** 만약 액티비티면 액티비티의 Key */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	FGameplayTag ConnectedTag = FGameplayTag::EmptyTag;
	
	FDialogueChoice() = default;
	
	FDialogueChoice(const FText& Text, const TArray<FGameCondition>& Conditions, const TArray<FGameAction>& Actions,
		const FName& NextNodeId)
		: Text(Text),
		  Conditions(Conditions),
		  Actions(Actions),
		  NextNodeId(NextNodeId)
	{
	}
	
};

/**
 * 자동 분기 한 갈래. 선택지 없이 "지금까지의 행적"으로 갈라질 때 쓴다.
 * 배열 위에서부터 검사해 처음으로 조건을 만족하는 갈래를 탄다 (= else if 사슬).
 */
USTRUCT(BlueprintType)
struct FDialogueBranch
{
	GENERATED_BODY()

	/** 비어 있으면 무조건 통과 → 배열 맨 끝에 두면 else 역할 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branch")
	TArray<FGameCondition> Conditions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branch")
	TArray<FGameAction> Actions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branch")
	FName NextNodeId;
};

/**
 * 대화 노드 한 개 = DataTable 한 행.
 *
 * 진행 순서:
 *   1) OnEnterActions 실행
 *   2) SpeakerName / Text 를 HUD에 출력
 *   3) AutoAdvance가 None이면 플레이어 클릭을 기다리고, 아니면 해당 조건이 만족될 때까지 기다린다
 *   4) 넘어갈 때:
 *      - Choices가 있으면  → 선택지 표시
 *      - Branches가 있으면 → 위에서부터 조건 검사해 첫 통과 갈래로 이동
 *      - 둘 다 없으면      → NextNodeId로 이동 (None이면 대화 종료)
 */
USTRUCT(BlueprintType)
struct FDialogueNodeRow : public FTableRowBase
{
	GENERATED_BODY()

	/** 화면에 표시할 화자 이름. 비우면 나레이션(사물 조사 문구 등)으로 취급 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	FText SpeakerName;

	/**
	 * 이 대사를 말하는 NPC의 NpcId. 여러 NPC가 함께 등장하는 장면에서 누가 말하는지 지정한다.
	 * 지정하면 그 NPC가 플레이어를 바라보도록 자동으로 회전한다.
	 * 비워두면 아무도 회전하지 않는다 (나레이션이거나 회전이 불필요한 경우).
	 * SpeakerName(표시용 텍스트)과는 별개다 - 이쪽은 실제 월드의 액터를 찾기 위한 키다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	FName SpeakerNpcId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	FText Text;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	TArray<FGameAction> OnEnterActions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	bool bYesNo = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	TArray<FDialogueChoice> Choices;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	TArray<FDialogueBranch> Branches;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue | Camera")
	FGameCamera Camera;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue | SoundLoc")
	FSoundLocation SoundLocation;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue")
	FName NextNodeId;

	/**
	 * 이 대사를 어떻게 넘길지. 자동 진행 중에는 플레이어 클릭이 무시된다
	 * (걸어가는 도중에 대사를 건너뛰어 연출이 어긋나는 걸 막기 위함).
	 * Choices가 있는 노드에서는 무시된다 - 선택지는 반드시 골라야 하므로.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue|AutoAdvance")
	EDialogueAutoAdvance AutoAdvance = EDialogueAutoAdvance::None;

	/** AfterDelay면 초, WhenPlayerNearNpc / WhenNpcArrived면 거리(cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue|AutoAdvance")
	float AutoAdvanceValue = 0.f;

	/**
	 * WhenNpcArrived / WhenPlayerNearNpc 판정의 기준이 될 NPC.
	 * 비워두면 상호작용한 NPC를 기준으로 삼는다.
	 *
	 * 여러 NPC가 동시에 이동하는 장면에서 "누가 도착할 때까지 기다릴지"를 고를 수 있다.
	 * (SpeakerNpcId와 달리 회전에는 영향을 주지 않는다 - 걷는 도중에 홱 돌아보면 어색하므로)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue|AutoAdvance")
	FName AutoAdvanceNpcId;

	/**
	 * 자동 진행을 기다리는 동안 대화창을 닫아둔다. 이 노드의 Text는 출력되지 않는다.
	 *
	 * "따라와" -> (대화창 닫힘, 플레이어가 자유롭게 따라감) -> 도착하면 대화 재개
	 * 같은 이동 구간을 만들 때 쓰는 "이동 대기 노드"용 플래그다.
	 * OnEnterActions는 정상적으로 실행되므로 여기서 NpcStartGuide / NpcResumeAI를 걸면 된다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dialogue|AutoAdvance")
	bool bHideBoxWhileWaiting = false;
};

/**
 * NPC 이벤트 한 개 = 이벤트 DataTable 한 행.
 * "이 NPC와 상호작용했을 때 어떤 대화를 시작할지"를 조건과 함께 정의한다.
 */
USTRUCT(BlueprintType)
struct FNpcEventRow : public FTableRowBase
{
	GENERATED_BODY()

	/** 이 이벤트의 주인 NPC (ABaseNpcCharacter의 NpcId와 일치해야 함) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	FName NpcId;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	TSoftObjectPtr<UDataTable> DialogueTable;

	/** 여러 이벤트가 동시에 조건을 만족하면 이 값이 큰 쪽이 먼저 실행된다 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	int32 Priority = 0;

	/** true면 완료 후에도 조건만 맞으면 다시 발생 (4번 이벤트처럼 반복 가능한 것) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	bool bRepeatable = false;

	/** 전제 조건 + 발생 조건. 전부 만족해야 이 이벤트가 후보가 된다 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	TArray<FGameCondition> Preconditions;

	/**
	 * 이 이벤트에 함께 등장하는 NPC들의 NpcId 목록 (상호작용 대상 NPC는 자동 포함).
	 * 여기 적힌 NPC들은 이벤트가 시작될 때 전부 Talk 상태로 들어가 제자리에 멈추고,
	 * 이벤트가 끝나면 한꺼번에 풀려난다.
	 * 예: 체력단련실 장면 = [NPC_HanSeoYun, NPC_KimDoYun]
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	TArray<FName> ParticipantNpcIds;
	
	/** 시작할 대화 노드 (대화 DataTable의 행 이름) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	FName RootNodeId;

	/** true면 대화가 끝날 때 이 이벤트를 자동으로 완료 처리한다 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Event")
	bool bCompleteOnFinish = true;
};

/**
 * HUD로 넘기는 선택지 표시용 뷰 데이터.
 * bEnabled=false면 "매력 부족" 같은 이유로 못 고르는 선택지 → 회색 처리한다.
 */
USTRUCT(BlueprintType)
struct FDialogueChoiceView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Dialogue")
	FText Text;

	UPROPERTY(BlueprintReadOnly, Category = "Dialogue")
	bool bEnabled = true;
};
