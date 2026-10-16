// [skydodo] 2026-09-01 작성 (최종 수정 2026-10-08)
// 요약: 행인(서브 NPC) 캐릭터 - 경로 추종/그룹/스팟/LOD 상태
// Fill out your copyright notice in the Description page of Project Settings.
//
// 행인(서브 NPC).
//
// ABaseNpcCharacter의 식별/애니메이션/상호작용을 그대로 물려받고,
// 군중에 필요한 세 가지를 더한다:
//   1) 스플라인 경로 추종 (NavMesh·BehaviorTree 안 씀)
//   2) 활동 스팟에서의 제스처
//   3) 플레이어와 부딪혔을 때의 반응
//
// [수명 관리]
//   이 액터는 절대 런타임에 Destroy되지 않는다. USubNpcDirectorSubsystem이 풀로 관리하며
//   Activate / Deactivate만 반복한다. 메타휴먼은 스폰/소멸 비용이 커서
//   그때그때 만들었다 지우면 프레임이 튀고 메모리가 파편화된다.
//   대신 Deactivate에서 **모든 참조와 타이머를 확실히 끊는 것**이 이 클래스의 계약이다.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Mass/EntityHandle.h"
#include "NPC/BaseNpcCharacter.h"
#include "NPC/SubNpc/SubNpcTypes.h"
#include "SubNpcCharacter.generated.h"

class ANpcCrowdPath;
class ANpcActivitySpot;
class UAnimToTextureDataAsset;

/**
 * 경로 이동 한 걸음의 계산 결과.
 *
 * 이동을 "계산"과 "적용"으로 나눈다. 계산(ComputePathStep)은 수학·스플라인 조회·트레이스뿐이라
 * 워커 스레드에서 여러 명을 동시에 돌릴 수 있고(UMassSubNpcPathFollowProcessor),
 * 적용(ApplyPathStep)은 위치 설정·경로 갈아타기·스팟 판정이라 게임 스레드에서만 한다.
 */
struct FSubNpcPathStep
{
	enum class EResult : uint8
	{
		None,         // 움직이지 않는다 (경로 없음)
		Move,         // Location/Rotation으로 옮긴다
		RotateOnly,   // 제자리에서 돌기만 한다 (합류할 틈을 기다리는 중)
		Handoff       // 다음 경로로 갈아탄다
	};

	EResult Result = EResult::None;

	/** 스팟 판정 거리에 닿았다. 적용 단계에서 RollForActivitySpot을 부른다 (들어가면 이동은 버린다) */
	bool bSpotCheck = false;

	bool bSweep = false;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};

UCLASS()
class FEBRICULA_API ASubNpcCharacter : public ABaseNpcCharacter
{
	GENERATED_BODY()

public:
	ASubNpcCharacter();

	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	//~ Begin IInteractable
	virtual bool CanInteract_Implementation(AActor* Interactor) const override;
	virtual void Interact_Implementation(AActor* Interactor) override;
	virtual FText GetInteractionPrompt_Implementation(AActor* Interactor) const override;
	//~ End IInteractable

	// ---- 풀 수명 -----------------------------------------------------------

	/**
	 * 풀에서 꺼내 경로 위에 세운다.
	 * 이전 개체의 상태가 남아 있으면 안 되므로, 여기서 모든 런타임 값을 새로 세팅한다.
	 */
	void ActivateFromPool(const FSubNpcArchetypeRow& Archetype, ANpcCrowdPath* StartPath, float StartDistance, bool bReversed);

	/**
	 * 풀로 되돌린다. 숨기고, 틱을 끄고, **들고 있던 모든 외부 참조를 끊는다.**
	 * 스팟 점유 해제와 타이머 정리가 여기서 반드시 일어나야 누수가 없다.
	 */
	void DeactivateToPool();

	UFUNCTION(BlueprintPure, Category = "Crowd")
	bool IsPooled() const { return CurrentActivity == ESubNpcActivity::Pooled; }

	UFUNCTION(BlueprintPure, Category = "Crowd")
	ESubNpcActivity GetActivity() const { return CurrentActivity; }

	// ---- LOD ---------------------------------------------------------------

	/**
	 * 상세도 단계를 적용한다. Director가 주기적으로 호출한다.
	 *
	 * 단계별로 바꾸는 것:
	 *   틱 간격 / 애니메이션 포즈 계산 방식 / 강제 LOD / 그림자 / 이동 처리 방식
	 * 같은 값이 다시 들어오면 아무것도 하지 않는다 (매 갱신마다 컴포넌트를 건드리면 그게 더 비싸다).
	 */
	void ApplyLodBand(ESubNpcLodBand NewBand);

	/**
	 * 이동 틱 주기를 정한다. **거리만** 본다.
	 *
	 * 예전에는 ApplyLodBand가 틱 주기까지 같이 정했다. 그런데 그 밴드는
	 * "지금 화면에 그려지고 있나"로도 한 단계 내려간다. 카메라를 돌리면
	 * 뒤쪽 개체가 순식간에 Far로 떨어졌다가 다시 올라오고, 그때마다 틱 주기가
	 * 0초 <-> 0.3초를 오가면서 위치 갱신이 뚝뚝 끊겼다.
	 * 애니메이션은 계속 재생되니 "걷는데 순간이동하는" 그림이 된다.
	 *
	 * 이동은 화면에 보이든 말든 매끄러워야 한다. 그래서 분리했다.
	 * 절약 효과가 큰 쪽(포즈 평가, 강제 LOD, 그림자)은 ApplyLodBand가 그대로 맡는다.
	 */
	void ApplyMovementTickRate(ESubNpcLodBand DistanceBand);

	UFUNCTION(BlueprintPure, Category = "Crowd|LOD")
	ESubNpcLodBand GetLodBand() const { return CurrentLodBand; }

	// ---- 경로 --------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Crowd|Path")
	ANpcCrowdPath* GetCurrentPath() const { return CurrentPath; }

	/** 그룹으로 다닐 때 앞사람. 있으면 앞사람의 경로 진행도를 따라간다 */
	void SetGroupLeader(ASubNpcCharacter* Leader, float FollowGap, float LaneOffset);

	// ---- 스팟 ----------------------------------------------------------------

	/** 지정한 스팟에 들어가 본다 */
	UFUNCTION(BlueprintCallable, Category = "Crowd|Spot")
	bool TryEnterSpot(ANpcActivitySpot* Spot);

	// ---- 반응 --------------------------------------------------------------

	/**
	 * 플레이어와 부딪혔을 때 호출. **ImpactSpeed(플레이어 수평 속도)만으로**
	 * 아키타입의 BumpTiers에서 반응과 애니메이션을 골라 재생한다.
	 * 쿨다운 중이거나 이미 반응·대화 중이면 무시한다.
	 *
	 * 두 곳에서 불린다.
	 *   1) 플레이어가 와서 부딪혔을 때 (NotifyHit)
	 *   2) 행인이 걸어오다 플레이어와 닿았을 때 (Director의 거리 판정)
	 *      행인은 스윕 없이 움직이므로 이 경우 NotifyHit이 안 불린다.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowd|Reaction")
	void ReactToPlayerBump(AActor* Player, float ImpactSpeed);

	/**
	 * 부딪힘 판정에 쓸 플레이어 속도 (cm/s). ReactToPlayerBump의 ImpactSpeed로 넘긴다.
	 *
	 * 실제 속도만 보면 안 된다. 뛰어와 행인 몸에 막히는 순간 속도가 뚝 떨어져서,
	 * 달려서 부딪혀도 걷기 구간으로 판정됐다. 입력을 누르고 있는 동안은
	 * "내려던 속도"(지금 MaxSpeed × 입력 세기)도 함께 보고 큰 쪽을 쓴다.
	 */
	static float GetPlayerBumpSpeed(const APawn* Player);

	/**
	 * 플레이어가 Target 쪽으로 다가오는 속도 (cm/s). 멀어지거나 멈춰 있으면 0 이하.
	 * 누가 누구에게 부딪혔는지 가르는 데 쓴다. 행인이 걸어와 닿은 거면 행인은 반응하지 않고 비켜 간다.
	 * 막혀서 실제 속도가 떨어져도 밀고 들어오는 중이면 잡히도록 입력 방향도 함께 본다.
	 */
	static float GetPlayerClosingSpeed(const APawn* Player, const AActor* Target);

	/**
	 * 플레이어가 행인 쪽으로 이 속도(cm/s) 이상 다가오고 있어야 "플레이어가 부딪혔다"로 본다.
	 * 그보다 느리면 행인이 걸어와 닿은 것이므로 반응 없이 비켜 지나간다.
	 */
	static constexpr float PlayerBumpMinClosingSpeed = 50.f;

	/**
	 * 부딪힘 반응이 결정된 순간 블루프린트에 알린다.
	 *
	 * C++가 고른 애니메이션으로 충분하면 구현하지 않아도 된다.
	 * 몽타주·사운드·나이아가라 등 연출을 붙이고 싶을 때 쓴다.
	 *
	 * @param Reaction      고른 반응 종류 (피하기 / 화내기 / 놀라기)
	 * @param bPushedLeft   true면 왼쪽으로 밀린다 (= 오른쪽에서 부딪혔다)
	 * @param bWasWalking   부딪힐 때 걷고 있었는가. false면 서 있던 중
	 *
	 * [주의] UFUNCTION은 클래스 크기에 영향이 없다. 여기에 변수를 추가하면
	 * WalkSpeed / bIsMoving / CurrentSpeed 오프셋이 밀려 AnimBP가 깨진다.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Crowd|Reaction")
	void OnBumpReaction(ESubNpcReaction Reaction, bool bPushedLeft, bool bWasWalking);

	UPROPERTY(BlueprintAssignable, Category = "Crowd|Reaction")
	FOnNpcInteracted OnBumped;

protected:
	virtual void BeginPlay() override;

	/**
	 * 풀에서 꺼내 아키타입을 적용한 직후 불리는 블루프린트 훅.
	 *
	 * 여기서 AppearanceTag를 보고 메시·머티리얼·의상을 바꾸면 한 클래스로
	 * 여러 유형의 행인을 만들 수 있다. 풀은 하나만 유지되므로 메모리가 늘지 않는다.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Crowd")
	void OnArchetypeApplied(const FSubNpcArchetypeRow& InArchetype);

	/** 풀로 회수되기 직전 훅. 블루프린트에서 만든 것(파티클, 소품 등)을 여기서 정리한다 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Crowd")
	void OnReturnedToPool();

	virtual void NotifyHit(class UPrimitiveComponent* MyComp, AActor* Other, class UPrimitiveComponent* OtherComp,
		bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

	/** 거리 밴드에 맞는 애니메이션 갱신 빈도를 적용한다 (N프레임에 한 번 평가) */
	static void ApplyAnimUpdateRate(class USkeletalMeshComponent* MeshComp, int32 FrameSkip);

	/** 경로를 따라 전진. LOD에 따라 이동 방식이 달라진다 */
	void TickPathFollow(float DeltaTime);

	/** 현재 경로가 끝났을 때 다음 경로를 고르거나 회수 요청 */
	void AdvanceToNextPath();

	/**
	 * 지금 경로에 들어설 때 다음 경로를 미리 정하고, 어디서 갈아탈지 계산한다.
	 * 다음 경로의 입구가 지금 경로 끝보다 앞쪽에 있으면 그 지점에서 일찍 갈아탄다.
	 */
	void PlanNextPath();

	/**
	 * 내 차선 기준으로 실제 갈아탈 진행도. 꺾이는 이음매에서 안쪽 차선은 일찍, 바깥쪽은 늦게 갈아타
	 * 새 경로에서도 같은 쪽 차선에 선다 (모두 한 점에서 갈아타면 한 줄로 합쳐져 뭉친다).
	 */
	float GetLaneHandoffDistance() const;

	/** 이 경로 구간의 스팟들에 대해 "들를까" 판정 */
	void RollForActivitySpot();

	/** 스팟의 설 자리까지 걸어간다. 도착하면 첫 동작을 시작한다 */
	void TickApproachSpot(float DeltaTime);

	/**
	 * From → To로 옮길 때 벽·바닥 같은 지형(WorldStatic/WorldDynamic)에 막히는지 캡슐로 쓸어 본다.
	 * 막히면 벽면을 따라 미끄러진 위치를 돌려준다. 행인·플레이어(Pawn)는 보지 않는다.
	 * 가까운(Near/Mid) 개체만 검사한다 - 멀리 있는 사람의 벽 스침은 화면에서 안 보인다.
	 */
	FVector ResolveWorldBlocking(const FVector& From, const FVector& To, bool& bOutBlocked) const;

	/**
	 * 캡슐 중심 위치를 바닥(지형)에 붙인다. 바닥을 찾으면 Z를 바꾸고 true.
	 * 다른 행인·플레이어 몸은 바닥으로 보지 않는다. 워커 스레드에서 불러도 된다 (읽기 전용 트레이스).
	 */
	bool SnapToGround(FVector& InOutCenter) const;

	/** 스팟 행동 시작/종료. SlotIndex는 스팟이 정해준 칸 */
	void BeginSpotActivity(ANpcActivitySpot* Spot, const FNpcActivityOption& Option, int32 SlotIndex);
	void EndSpotActivity();

	/** 자리가 없는 스팟의 줄 끝에 선다 */
	void BeginSpotQueue(ANpcActivitySpot* Spot);

	/** 줄에서 내 자리로 걸어가 기다리다가, 차례가 오면 들어간다 */
	void TickSpotQueue(float DeltaTime, float Now);

	/** 줄에서 빠져 다시 길로 돌아간다 (포기·반응·대화·회수). 줄에 없으면 아무 일도 없다 */
	void LeaveSpotQueue();

	/**
	 * 차선 밖(스팟·줄·스팟으로 가던 길)에서 걸어서 길로 돌아가기 시작한다.
	 * 경로 진행 위치를 지금 서 있는 곳에서 가장 가까운 지점으로 다시 잡으므로 길 쪽으로 곧장 나간다.
	 */
	void BeginRejoinPath();

	/** 다음 단계로 넘어간다. 남은 단계가 없으면 EndSpotActivity를 부른다 */
	void AdvanceSpotStep();

	/** 단계 하나를 재생한다 (애니메이션 + 종료 시각 설정) */
	void PlaySpotStep(FName AnimId, float MinDuration, float MaxDuration, bool bLoop);

	/** 반응 종료 후 다시 걷기 */
	void EndReaction();

	/** 타이머·애니메이션·스팟 점유 등 진행 중인 모든 것을 정리한다 (풀 회수·EndPlay 공용) */
	void ClearRuntimeState();

	/** 지금 상황에 맞는 대사 한 줄을 고른다 */
	bool PickBarkLine(FText& OutLine) const;

	// ---- 설정 --------------------------------------------------------------

	/**
	 * 상호작용(E)으로 한 줄 대사를 보여줄지.
	 *
	 * 행인에게 말을 걸 수 있게 하면 "살아있는 거리" 느낌이 크게 올라간다.
	 * 대사 트리가 아니라 한 줄짜리라 데이터 비용도 거의 없다.
	 * 반대로 군중 밀도가 높은 구역에서는 E 프롬프트가 계속 떠서 방해가 되므로,
	 * 구역별로 끌 수 있게 옵션으로 뒀다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Interaction")
	bool bAllowTalk = true;

	/**
	 * 부딪힌 뒤 이 시간(초) 안에 말을 걸면 BumpedBarks 대사가 나온다.
	 *
	 * 짧게 잡는 게 맞다. "방금 부딪힌 일에 대한 반응"이라는 전제로 대사를 쓰기 때문에,
	 * 창이 길면 한참 뒤에 말을 걸었는데 방금 일처럼 말하는 어색한 상황이 나온다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Interaction", meta = (ClampMin = "0.0"))
	float BumpMemoryDuration = 12.f;

	/** 반응 후 다시 반응하기까지의 쿨다운 (초). 없으면 벽에 끼었을 때 무한 반복된다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Reaction", meta = (ClampMin = "0.0"))
	float ReactionCooldown = 4.f;

	/** 반응 지속 시간 (초) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Reaction", meta = (ClampMin = "0.0"))
	float ReactionDuration = 2.f;

	/** 피하기 반응에서 옆으로 비키는 거리 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Reaction")
	float DodgeSideStep = 90.f;

	/**
	 * 이동할 때 캡슐 스윕을 쓸지.
	 *
	 * **기본값 false를 권장한다.** 켜면 행인끼리 서로를 막아서, 같은 지점에 두 명이
	 * 등장하는 순간 둘 다 영영 멈춘다 (진행도만 오르고 몸은 제자리).
	 * 끄더라도 플레이어와의 부딪힘 반응과 E키 상호작용은 정상 동작한다.
	 *
	 * 벽을 절대 통과하면 안 되는 구역에서만, 행인 밀도를 낮춘 뒤 켜는 걸 고려한다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Path")
	bool bSweepWhileMoving = false;

	/**
	 * 이보다 짧은 경로는 쓸 수 없는 것으로 본다 (cm).
	 * 길이가 0에 가까우면 도착 판정이 매 틱 성립해서 무한 재활용이 일어난다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Path", meta = (ClampMin = "1.0"))
	float MinUsablePathLength = 50.f;

	/** 막다른 길에서 되돌아설 때 끝점에서 안쪽으로 밀어줄 거리 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Path", meta = (ClampMin = "1.0"))
	float TurnAroundNudge = 20.f;

	/**
	 * 스팟에 들를지 판정하는 간격 (cm).
	 *
	 * 예전에는 경로에 진입할 때 한 번만 판정했다. 그러면 경로 중간이나 끝에 있는 스팟도
	 * 진입 순간에 당첨될 수 있어서, 이미 지나쳐 걸어간 개체가 뒤에 있는 스팟으로
	 * 순간이동하는 것처럼 보였다. 일정 거리마다 다시 굴려서 "근처에 왔을 때" 들르게 한다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Path", meta = (ClampMin = "10.0"))
	float SpotCheckInterval = 150.f;

	/** 이 거리 안에 있는 스팟만 후보로 본다 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Path", meta = (ClampMin = "0.0"))
	float SpotEngageRadius = 400.f;

	// ---- 소프트 회피 ---------------------------------------------------------

	/** 회피 오프셋이 목표값을 향해 움직이는 속도 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Avoidance", meta = (ClampMin = "0.1"))
	float AvoidInterpSpeed = 3.f;

	/** 한 프레임에 밀릴 수 있는 최대량 (cm). 너무 크면 옆으로 튄다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Avoidance", meta = (ClampMin = "0.0"))
	float MaxAvoidOffset = 60.f;

	/** LOD 단계별 틱 간격 (초). Near는 매 프레임 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|LOD")
	float MidTickInterval = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|LOD")
	float FarTickInterval = 0.3f;

	/**
	 * Mid / Far에서 몇 프레임에 한 번 포즈를 평가할지.
	 *
	 * 프로파일링에서 군중 비용의 86%가 애니메이션으로 나왔고, 그 부분은 거리와
	 * 무관하게 매 프레임 돌고 있었다. 멀리 있는 개체를 4~8프레임에 한 번만
	 * 평가하게 하면 그만큼 그대로 절약된다. 사이 프레임은 보간으로 채운다.
	 *
	 * 1이면 매 프레임 (절약 없음).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|LOD", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MidAnimFrameSkip = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|LOD", meta = (ClampMin = "1", ClampMax = "16"))
	int32 FarAnimFrameSkip = 4;

	/** Mid / Far에서 강제할 스켈레탈 메시 LOD 인덱스 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|LOD")
	int32 MidForcedLod = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|LOD")
	int32 FarForcedLod = 3;

	// ---- 런타임 ------------------------------------------------------------

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Crowd")
	ESubNpcActivity CurrentActivity = ESubNpcActivity::Pooled;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Crowd|LOD")
	ESubNpcLodBand CurrentLodBand = ESubNpcLodBand::Culled;

	UPROPERTY(Transient)
	TObjectPtr<ANpcCrowdPath> CurrentPath;

	/** 지금 점유 중인 스팟. 풀 회수 시 반드시 해제해야 한다 */
	UPROPERTY(Transient)
	TObjectPtr<ANpcActivitySpot> OccupiedSpot;

	/** 그룹으로 다닐 때의 앞사람 (약한 참조 - 앞사람이 먼저 회수돼도 안전해야 한다) */
	UPROPERTY(Transient)
	TWeakObjectPtr<ASubNpcCharacter> GroupLeader;

	/** 현재 경로 위 진행 거리 (cm) */
	float PathDistance = 0.f;

	/** 경로를 역방향으로 타는 중인가 */
	bool bReversedOnPath = false;

	/** 좌우 차선 오프셋 (cm). 개체마다 달라야 한 줄로 겹치지 않는다 */
	float LaneOffset = 0.f;

	/** 그룹 추종 시 앞사람과 벌릴 간격 (cm) */
	float GroupFollowGap = 0.f;

	/**
	 * 이 개체의 걷기 속도 (cm/s).
	 *
	 * [애님 블루프린트에 노출하는 이유]
	 *   서브 NPC는 CharacterMovement를 쓰지 않고 위치를 직접 세팅한다.
	 *   그래서 GetVelocity()가 항상 0이고, 서드퍼슨 ABP처럼 속도로 로코모션을
	 *   판단하는 그래프는 영원히 대기 포즈만 낸다.
	 *
	 *   ABP가 이 값을 직접 읽게 하면 해결된다. 함수 호출이 아니라 프로퍼티 읽기라
	 *   Property Access로 접근할 수 있고, 그러면 Thread Safe Update Animation
	 *   안에서도 쓸 수 있다.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Crowd|Anim")
	float WalkSpeed = 160.f;

	/**
	 * 지금 실제로 걷고 있는가. ABP의 걷기/대기 전환에 쓴다.
	 * 스팟에서 활동 중이거나 반응 중이면 false다.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Crowd|Anim")
	bool bIsMoving = false;

	/**
	 * ABP가 읽을 현재 속력 (cm/s). 걷는 중이면 WalkSpeed, 아니면 0.
	 * 로코모션 블렌드스페이스에 그대로 넣을 수 있는 값이다.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Crowd|Anim")
	float CurrentSpeed = 0.f;

	/** 마지막으로 반응한 시각. 쿨다운 판정용 */
	float LastReactionTime = -1000.f;

	/** 마지막으로 부딪힌 시각. 대사 분기용 */
	float LastBumpTime = -1000.f;

	/** 스팟 행동 / 반응이 끝날 시각 */
	float ActivityEndTime = 0.f;

	/** 반응 중 옆으로 비킬 목표 오프셋 */
	float DodgeTargetOffset = 0.f;

	/** 지금 재생 중인 반응 종류. 피하기만 걸으면서 비키고 나머지는 멈춘다 */
	ESubNpcReaction CurrentReaction = ESubNpcReaction::None;

	/** 이 개체에 적용된 아키타입 (대사·반응 가중치 조회용) */
	FSubNpcArchetypeRow Archetype;

	// ---- 소프트 회피 (Director가 호출) --------------------------------------

public:
	/** 회피 계산에 필요한 값들. Director가 이웃끼리 비교할 때 읽는다 */
	float GetPathDistance() const { return PathDistance; }
	float GetLaneOffset() const { return LaneOffset; }

	/** 그룹 리더 (없으면 nullptr) */
	ASubNpcCharacter* GetGroupLeader() const { return GroupLeader.Get(); }

	/**
	 * 같은 그룹인가.
	 *
	 * 친구끼리는 회피로 밀어내면 안 된다. 붙어 다니라고 묶어놓고 떼어놓으면
	 * 대형이 계속 흔들려서 오히려 더 어색하다.
	 *
	 * [주의] 이 클래스에 데이터 멤버를 새로 추가하지 않는다.
	 * WalkSpeed / bIsMoving / CurrentSpeed 앞에 멤버가 끼면 오프셋이 밀려서
	 * AnimBP의 Property Access가 엉뚱한 값을 읽는다 (걷기 애니메이션이 깨진다).
	 * 메서드 추가는 레이아웃에 영향이 없으므로 안전하다.
	 */
	bool IsSameGroup(const ASubNpcCharacter* Other) const
	{
		if (!Other || Other == this)
		{
			return false;
		}
		const ASubNpcCharacter* MyLeader = GroupLeader.Get();
		const ASubNpcCharacter* ItsLeader = Other->GroupLeader.Get();
		return (MyLeader && MyLeader == ItsLeader)
			|| (MyLeader == Other)
			|| (ItsLeader == this);
	}
	bool IsWalkingOnPath() const { return CurrentActivity == ESubNpcActivity::Walking && CurrentPath != nullptr; }

	/**
	 * 경로 위에서 뒷사람을 막는 몸이 있는가.
	 *
	 * 걷는 중이 아니어도 부딪혀 멈춘(Reacting) 사람은 경로 위에 그대로 서 있다.
	 * 이걸 빼면 뒷사람이 앞사람을 못 보고 뚫고 지나간다.
	 * 스팟에서 반응 중인 사람은 경로 밖에 서 있으므로 제외한다.
	 */
	bool IsBlockingOnPath() const
	{
		return IsWalkingOnPath()
			|| (CurrentActivity == ESubNpcActivity::Reacting && CurrentPath != nullptr && !bResumeSpotAfterReaction);
	}

	/**
	 * 좌우로 밀어낼 양을 누적한다. 실제 반영은 다음 틱에서 보간으로 이뤄진다.
	 * 즉시 위치를 바꾸지 않는 이유: 두 개체가 서로를 동시에 밀면 진동하기 때문이다.
	 */
	void AddAvoidanceNudge(float Amount) { PendingAvoidNudge += Amount; }

	/**
	 * 앞사람이 막고 있으니 속도를 줄이라는 요청 (0~1 배율).
	 *
	 * 행인은 1차원 경로 위를 걷기 때문에, 좌우로만 밀어내면 앞사람이 느릴 때
	 * **그대로 뚫고 지나간다.** 진행 방향으로도 제동이 필요하다.
	 * 여러 요청이 오면 가장 강한(가장 느린) 것을 따른다.
	 */
	void RequestSlowDown(float Scale)
	{
		PendingSpeedScale = FMath::Min(PendingSpeedScale, FMath::Clamp(Scale, 0.f, 1.f));
	}

	/**
	 * 겹친 몸을 떼어 낸다 (월드 수평 방향 이동량). Director의 겹침 금지 마지막 단계에서 부른다.
	 * 길 위를 걷는 중이면 진행도·차선도 같이 옮긴다 - 몸만 옮기면 다음 걸음에서 원래 자리로 돌아간다.
	 */
	void PushApart(const FVector& WorldPush);

	/** 경로를 갈아탈 때 생긴 위치 어긋남 (진단용). 몸이 따라가는 목표점 = 경로 위치 + 이 값 */
	FVector GetPathHandoffOffset() const { return PathHandoffOffset; }

	bool IsReversedOnPath() const { return bReversedOnPath; }

	/**
	 * 일행과 걸으면서 이야기한다. Director가 그룹 단위로 같은 시간에 부른다.
	 * 멈추지 않고, 상체 슬롯(UpperBody)에만 손짓을 얹는다 - 하체는 계속 걷는다.
	 * 일행이 TurnLength초씩 번갈아 말한다 (MemberIndex 차례에만 손짓).
	 * 걷는 중이 아니면 아무것도 하지 않고 false.
	 */
	bool StartGroupChat(float Duration, FName TalkAnimId, int32 MemberIndex, int32 MemberCount, float TurnLength,
		ASubNpcCharacter* Partner, FName ListenAnimId = NAME_None);

	/** 일행 대화를 끝낸다 (손짓을 거둔다). 대화 중이 아니면 아무 일도 없다 */
	void StopGroupChat();

	/**
	 * [일행 스팟 들르기] 일행 중 한 명(Visitor)이 스팟에 들어갔다. 리더가 부른다.
	 * 리더와 나머지 일행은 스팟의 일행 대기 자리로 비켜서서 그룹 동작을 하며 기다린다.
	 */
	void BeginGroupSpotVisit(ASubNpcCharacter* Visitor, ANpcActivitySpot* Spot);

	/** 일행 대기 자리로 가서 기다린다 */
	void BeginGroupWait(ANpcActivitySpot* Spot, const FVector& WaitLocation, const FRotator& WaitRotation);

	/** 기다리기를 그만두고 길로 돌아간다 */
	void EndGroupWait();

	/** 리더: 일행 한 명이 스팟에 들러 나머지가 기다리는 중인가 */
	bool IsHostingGroupVisit() const { return GroupSpotVisitor.IsValid(); }

	/** 줄 선 스팟 (줄에 없으면 nullptr). 같은 줄 사람끼리는 겹침 금지가 서로를 세우지 않는다 */
	const ANpcActivitySpot* GetQueuedSpot() const { return QueuedSpot.Get(); }

	/** 이벤트 중이거나 칸으로 가는 스팟 (없으면 nullptr) */
	const ANpcActivitySpot* GetOccupiedSpot() const { return OccupiedSpot; }

	/**
	 * 스팟 근처 상태 (디버그 표시 crowd.DebugSpot). 길 위를 그냥 걷는 중이면 false.
	 * OutLabel: "줄2 이동"·"이벤트"·"길로 복귀" 등, OutTarget: 지금 가려는 지점
	 */
	bool GetSpotDebugState(FString& OutLabel, FVector& OutTarget) const;

	/** 다음에 일행 대화를 굴려볼 시각. 0이면 아직 정하지 않았다 (리더만 쓴다) */
	float GetNextGroupChatTime() const { return NextGroupChatTime; }
	void SetNextGroupChatTime(float Time) { NextGroupChatTime = Time; }

	/** 이 개체 고유의 걸음 속도 (cm/s). 감속 전 값이다 */
	float GetWalkSpeed() const { return WalkSpeed; }

	/** 지금 실제로 걷고 있는 속도 (cm/s). 앞사람 때문에 줄인 것까지 반영한다 */
	float GetCurrentMoveSpeed() const { return WalkSpeed * CurrentSpeedScale; }

	/** 걷는 중에 앞이 막혀 거의 멈춰 서 있은 시간 (초). 정체를 풀 때 쓴다 */
	float GetStalledTime() const { return StalledTime; }

	/** 리더 뒤 대열 간격 (cm). 일행이 얼마나 뒤처졌는지 잴 때 쓴다 */
	float GetGroupFollowGap() const { return GroupFollowGap; }

	/** 일행을 이끄는 리더인가 (Director가 그룹을 만들 때 정한다) */
	void SetHasGroupFollowers(bool bHas) { bHasGroupFollowers = bHas; }

	/** 스팟·줄에서 나와 길로 돌아오는 중인가 (아직 차선에 합류하지 않음) */
	bool IsRejoiningPath() const { return bRejoiningPath; }

	/**
	 * 길 밖에서 움직이는 중인가: 스팟으로 가는 중, 줄 자리로 옮기는 중, 길로 돌아오는 중.
	 * 이때는 길 위 속도 회피를 쓰지 않으므로 Director가 서로 겹치지 않게 따로 양보시킨다.
	 */
	bool IsMovingOffLane() const
	{
		return (CurrentActivity == ESubNpcActivity::AtSpot && (bApproachingSpot || bQueueMoving || bGroupWaitMoving))
			|| (CurrentActivity == ESubNpcActivity::Walking && bRejoiningPath);
	}

	/** 길 밖에서 움직이다 다른 사람과 부딪히겠으면 Director가 true로 세운다. 그 자리에서 잠깐 기다린다 */
	void SetOffLaneYield(bool bYield) { bOffLaneYield = bYield; }

	/** 리더가 뒤처진 일행을 기다리는 중인가 (Director가 정한다) */
	bool IsWaitingForGroup() const { return bWaitingForGroup; }
	float GetGroupWaitStartTime() const { return GroupWaitStartTime; }
	void SetWaitingForGroup(bool bWaiting, float Now)
	{
		if (bWaiting && !bWaitingForGroup)
		{
			GroupWaitStartTime = Now;
		}
		bWaitingForGroup = bWaiting;
	}

	/**
	 * 막힌 앞사람을 추월하려고 옆 차선으로 옮겨 가라는 요청.
	 *
	 * 좌우 밀어내기(PendingAvoidNudge)는 한 번 반영하고 사라지는 약한 힘이라
	 * 앞사람 옆으로 빠져나가기엔 부족하다. 그래서 목표 차선을 따로 주고,
	 * Director가 계속 갱신하는 동안(Until까지) 그쪽으로 옮겨 간다.
	 */
	void RequestPassLane(float TargetLane, float Until)
	{
		PassLaneTarget = TargetLane;
		PassLaneUntil = Until;
	}

	/** 추월 중인가. 추월하는 동안에는 방향을 바꾸지 않아야 좌우로 떨지 않는다 */
	bool IsPassing() const;

	/** 회수 예약 여부. 순회 중에 배열이 바뀌지 않도록 Director가 나중에 처리한다 */
	bool IsPendingRecycle() const { return bPendingRecycle; }
	void ClearPendingRecycle() { bPendingRecycle = false; }

	// ---- Mass (이동 계산 병렬화) ---------------------------------------------

	/**
	 * 경로 이동 한 걸음을 계산만 한다. 자기 멤버(진행도·차선·속도)만 바꾸고 월드는 건드리지 않는다.
	 * 워커 스레드에서 불러도 된다 - 단, 같은 개체를 두 스레드에서 동시에 부르면 안 된다.
	 */
	void ComputePathStep(float DeltaTime, FSubNpcPathStep& Out);

	/** 계산 결과를 적용한다 (위치 설정, 경로 갈아타기, 스팟 판정). 게임 스레드 전용 */
	void ApplyPathStep(const FSubNpcPathStep& Step);

	/** 이번 틱에 경로를 따라 걸어야 하는 상태인가 (Mass 프로세서가 대상을 고를 때 본다) */
	bool CanMassStep() const { return CurrentActivity == ESubNpcActivity::Walking && CurrentPath != nullptr && !bExitFading; }

	/** 이 행인을 대신 움직이는 Mass 엔티티. 설정돼 있고 crowd.Mass가 켜져 있으면 액터 틱은 이동을 건너뛴다 */
	FMassEntityHandle GetMassEntity() const { return MassEntity; }
	void SetMassEntity(FMassEntityHandle InEntity) { MassEntity = InEntity; }

	/** 지금 Mass가 이동을 맡고 있는가 */
	bool IsMovementDrivenByMass() const;

	// ---- 원경 인스턴스 표현 (UMassSubNpcRepresentationProcessor) -------------

	/**
	 * 멀리 있을 때 인스턴스드 메시로 그리도록 바꾼다. true면 스켈레탈 메시를 숨기고 애니메이션 계산을 멈춘다.
	 * 액터 자체(이동·충돌·상호작용)는 그대로 돈다 - 그리는 방식만 바뀐다.
	 */
	void SetInstancedRepresentation(bool bInstanced);

	/** ABP에 넘기는 현재 걸음 속도 (cm/s). 인스턴스 쪽 걷기/대기 애니메이션을 고를 때 쓴다 */
	float GetAnimSpeed() const { return CurrentSpeed; }

	/** 퇴장구에서 사라지는 중인가 (그동안은 인스턴스로 바꾸지 않는다 - 페이드가 보여야 한다) */
	bool IsExitFading() const { return bExitFading; }

	/** 원경 표현 데이터. 지정하지 않았으면 nullptr (스포너의 기본값을 쓴다) */
	UAnimToTextureDataAsset* GetFarCrowdAnimData() const;

protected:
	// [skydodo] 2026-10-17 추가: 행인 종류(남학생/여학생 등)마다 메시가 달라 원경 데이터도 따로 둔다
	/** 멀리 있을 때 그릴 원경 애니메이션 데이터. 비우면 스포너의 Far Crowd Anim Data를 쓴다 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SubNpc|FarCrowd")
	TSoftObjectPtr<UAnimToTextureDataAsset> FarCrowdAnimData;

	/** 이번 프레임에 이웃들이 밀어낸 양의 합 */
	float PendingAvoidNudge = 0.f;

	/** 경로 끝에 닿아 회수를 기다리는 중 */
	bool bPendingRecycle = false;

	// ---- 스팟 단계 진행 ------------------------------------------------------

	/** 지금 재생 중인 스팟 행동 (단계 배열을 들고 있어야 이어서 재생할 수 있다) */
	FNpcActivityOption CurrentSpotOption;

	/** 지금 몇 번째 단계인가. -1 = 첫 동작(AnimId), 0부터는 ExtraSteps 인덱스 */
	int32 CurrentSpotStep = -1;

	/**
	 * 반응이 끝난 뒤 하던 스팟 행동으로 되돌아갈지.
	 * 앉아 있다가 부딪혀 놀란 사람은 다시 앉는 게 자연스럽다.
	 */
	bool bResumeSpotAfterReaction = false;

	/**
	 * 스팟까지 걸어가는 중인가. 도착하면 첫 동작을 재생한다.
	 * 예전에는 스팟 위치로 순간이동해서, 행동이 시작되는 순간 몇 미터씩 튀어 보였다.
	 */
	bool bApproachingSpot = false;

	/** 스팟에서 설 자리와 도착 후 바라볼 방향 */
	FVector SpotStandLocation = FVector::ZeroVector;
	FRotator SpotStandRotation = FRotator::ZeroRotator;

	/**
	 * 스팟에서 나와 차선으로 걸어 돌아가는 중인가.
	 * 진행도는 평소처럼 오르고, 몸만 차선 위치를 걸음 속도로 따라잡는다.
	 */
	bool bRejoiningPath = false;

	/** 다음에 일행 대화를 굴려볼 시각 (리더만 쓴다). 0이면 Director가 처음 볼 때 정한다 */
	float NextGroupChatTime = 0.f;

	/** 줄을 서 있는 스팟. 줄에 있는 동안 CurrentActivity는 AtSpot이고 OccupiedSpot은 비어 있다 */
	TWeakObjectPtr<ANpcActivitySpot> QueuedSpot;

	/** 이 시각까지 차례가 안 오면 포기한다 */
	float QueueGiveUpTime = 0.f;

	/** 줄에서 내 자리로 걸어가는 중인가 (걷기 애니메이션용) */
	bool bQueueMoving = false;

	// ---- 걸으면서 일행과 대화 ----------------------------------------------------

	/** 대화 중인가 */
	bool bGroupChatting = false;

	/** 지금 내 차례라 손짓을 재생하고 있는가 */
	bool bGroupChatGesturing = false;

	float GroupChatStartTime = 0.f;
	float GroupChatEndTime = 0.f;
	float GroupChatTurnLength = 2.5f;
	int32 GroupChatIndex = 0;
	int32 GroupChatCount = 1;
	FName GroupChatAnimId;

	/** 이야기 상대. 걸으면서 몸을 이 사람 쪽으로 살짝 돌린다 */
	TWeakObjectPtr<ASubNpcCharacter> GroupChatPartner;

	/** 걷는 중에 거의 멈춰 서 있은 시간 (초) */
	float StalledTime = 0.f;

	/** 일행을 이끄는 리더인가. 리더는 스팟에 들르지 않는다 (일행이 길에서 기다리며 길을 막는다) */
	bool bHasGroupFollowers = false;

	/** (일행) 리더 차선 기준 좌우 위치 (cm). 리더가 차선을 바꾸면 따라 옮긴다 */
	float GroupLaneOffset = 0.f;

	/** 길 밖에서 움직이다 다른 사람에게 양보하는 중인가 (Director가 정한다) */
	bool bOffLaneYield = false;

	/**
	 * 이번 경로에서 이미 "들를까" 판정을 한 스팟. 경로를 바꾸면 비운다.
	 * 예전에는 1.5m마다 반경 4m 안의 스팟을 다시 굴려서, 스팟 옆을 지나는 동안 3~4번 판정했다.
	 * EngageChance가 25%여도 실제로는 절반 넘게 들러서 스팟 주변이 붐볐다.
	 */
	TArray<TWeakObjectPtr<ANpcActivitySpot>> RolledSpots;

	/** 미리 정해 둔 다음 경로와 갈아탈 진행 거리 (PlanNextPath). 거리가 음수면 경로 끝에서 갈아탄다 */
	TWeakObjectPtr<ANpcCrowdPath> PlannedNextPath;
	bool bPlannedNextReversed = false;
	float PlannedHandoffDistance = -1.f;

	/** (리더) 뒤처진 일행을 기다리는 중인가, 언제부터 */
	bool bWaitingForGroup = false;
	float GroupWaitStartTime = -1000.f;

	/** 대화 중이면 차례에 맞춰 손짓을 켜고 끈다 */
	void TickGroupChat(float Now);

	/** 다음 스팟 판정을 할 경로 진행 거리. 경로 진입 시 한 번만 보는 문제를 없앤다 */
	float NextSpotCheckDistance = 0.f;

	/**
	 * 경로를 갈아탈 때 생기는 위치 튐을 흡수하는 잔여 오프셋 (월드 공간).
	 *
	 * 경로 전환은 본질적으로 불연속이다. 새 스플라인의 어느 지점으로 옮겨 앉는 순간
	 * 위치가 한 프레임 만에 바뀌고, 그게 「사라졌다가 다른 데서 나타남」으로 보인다.
	 *
	 * 그래서 전환 순간의 어긋난 양을 여기에 담아두고, 매 틱 조금씩 0으로 줄인다.
	 * 결과적으로 사람이 새 경로 쪽으로 **부드럽게 미끄러져 들어간다.**
	 *
	 * [주의] 이 멤버에는 UPROPERTY를 붙이지 않는다. 리플렉션 프로퍼티를 추가하면
	 * ABP_Passerby의 Property Access가 읽는 WalkSpeed / bIsMoving / CurrentSpeed의
	 * 오프셋 계산에 영향을 줄 수 있다. 여기 순수 C++ 멤버로 두면 안전하다.
	 */
	FVector PathHandoffOffset = FVector::ZeroVector;

	/** 이번 갱신에서 요청된 감속 배율 (1 = 감속 없음). Director가 채우고 틱이 소비한다 */
	float PendingSpeedScale = 1.f;

	/** 실제로 적용 중인 감속 배율. 급브레이크로 보이지 않게 보간한다 */
	float CurrentSpeedScale = 1.f;

	// [주의] 아래 멤버는 반드시 클래스 맨 끝에 둔다 (위쪽 프로퍼티 오프셋을 밀지 않도록).

	/** 추월할 때 옮겨 갈 목표 차선 오프셋 */
	float PassLaneTarget = 0.f;

	/** 이 시각까지 PassLaneTarget 쪽으로 옮겨 간다. Director가 갱신을 멈추면 자연히 끝난다 */
	float PassLaneUntil = -1.f;

	/** 이 행인을 움직이는 Mass 엔티티 (Director가 만들고 지운다). 클래스 맨 끝에 둔다 */
	FMassEntityHandle MassEntity;

	// ---- 퇴장 페이드 ---------------------------------------------------------
	// [주의] WalkSpeed / bIsMoving / CurrentSpeed 오프셋을 밀지 않도록 클래스 맨 끝에 둔다.

	/**
	 * 퇴장구(bDespawnAtEnd 경로의 끝)에 닿았을 때 서서히 사라지게 할 머티리얼. 메시 슬롯 순서대로.
	 *
	 * 행인 메시의 원래 머티리얼은 불투명이라 투명도를 조절할 수 없다. 사라지는 동안만 이 머티리얼로
	 * 바꿔 끼우고 "Fade" 스칼라 값(1 → 0)을 내린다. 마스크(디더) 머티리얼이어야 한다.
	 * 비워 두면 예전처럼 끝에 닿는 순간 사라진다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowd|Exit")
	TArray<TObjectPtr<UMaterialInterface>> ExitFadeMaterials;

	/** 페이드 전에 끼워져 있던 머티리얼. 풀로 돌아갈 때 되돌린다 (블루프린트가 외형으로 바꾼 머티리얼일 수 있다) */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> MaterialsBeforeExitFade;

	/** 퇴장 페이드 중인가. 이 동안은 경로 끝 너머로 똑바로 걸으며 사라진다 */
	bool bExitFading = false;
	float ExitFadeElapsed = 0.f;
	FVector ExitVelocity = FVector::ZeroVector;

	/** 퇴장 페이드를 시작한다. 페이드 머티리얼이 없으면 false (바로 회수한다) */
	bool BeginExitFade();

	/** 페이드 한 틱. 다 사라지면 회수를 예약한다 */
	void TickExitFade(float DeltaTime);

	/** 페이드 때 바꾼 머티리얼을 되돌린다 (풀 회수·EndPlay) */
	void RestoreMaterialsAfterExitFade();

	/** 인스턴스드 메시로 그려지는 중인가 (스켈레탈 메시는 숨겨져 있다) */
	bool bInstancedRepresentation = false;

	/**
	 * 스팟에 들를지를 Mass StateTree가 정한다 (Mass 3단계).
	 * true면 걷는 중의 자동 스팟 판정(RollForActivitySpot)을 하지 않고, StateTree의
	 * "행인: 스팟 들르기 판정" 태스크가 RequestSpotRoll로 요청할 때만 들른다.
	 */
	bool bSpotDecisionByStateTree = false;

	/**
	 * StateTree가 "스팟에 들를지 굴려 봐"라고 요청했다. 액터 틱(게임 스레드)에서 처리한다.
	 * Mass StateTree 태스크는 게임 스레드가 아닌 곳에서 돌 수 있어서, 스팟 점유처럼 월드를 바꾸는 일은
	 * 태스크가 직접 하지 않고 요청만 남긴다.
	 */
	FThreadSafeBool bSpotRollRequested = false;

public:
	void SetSpotDecisionByStateTree(bool bEnable) { bSpotDecisionByStateTree = bEnable; }

	/** 스팟 들르기 판정을 요청한다 (어느 스레드에서나 불러도 된다) */
	void RequestSpotRoll() { bSpotRollRequested = true; }

	// [AnimBP 오프셋 안전] 새 멤버는 클래스 끝에 둔다 (Docs·메모리: 헤더 변경 시 에디터 재시작).
protected:
	/** 일행 대화에서 내 차례가 아닐 때 상체에 거는 동작 (듣기). 없으면 손짓을 거둔다 */
	FName GroupListenAnimId;

	/** 지금 상체에 걸어 둔 일행 동작. -1 없음, 0 듣기, 1 말하기 */
	int8 GroupChatShownRole = -1;

	// ---- 일행 스팟 들르기 (BeginGroupSpotVisit) ----

	/** 일행 대기 중 (상태는 AtSpot). 대기 자리로 걸어가는 중이면 bGroupWaitMoving도 true */
	bool bGroupWaiting = false;
	bool bGroupWaitMoving = false;
	FVector GroupWaitLocation = FVector::ZeroVector;
	FRotator GroupWaitRotation = FRotator::ZeroRotator;
	TWeakObjectPtr<ANpcActivitySpot> GroupWaitSpot;

	/** 리더: 지금 스팟에 들른 일행과, 그 사람을 기다리는 마감 시각 (넘으면 그 사람만 일행에서 빠진다) */
	TWeakObjectPtr<ASubNpcCharacter> GroupSpotVisitor;
	float GroupVisitDeadline = 0.f;

	/** 대기 자리로 걸어가고, 리더면 끝났는지 본다 */
	void TickGroupWait(float DeltaTime, float Now);

	/** 리더: 기다리기를 끝내고 모두 길로 돌아간다. 시간 초과면 들른 사람은 일행에서 빠진다 */
	void EndGroupSpotVisit(bool bTimedOut);

	// ---- 실제 속도 (회피 예측용) ---------------------------------------------

public:
	/**
	 * 실제로 움직인 속도 (cm/s, 수평). Director가 회피 갱신 때 위치 차이로 잰다.
	 * 회피는 상대가 이 속도로 계속 간다고 보고 피한다. 예전에는 "길 밖에서 움직이는 중이면 걷는 속도"로 가정해서
	 * 줄에서 서서 기다리는 사람도 다가오는 걸로 보여 행인이 멈췄다.
	 */
	FVector2D GetMeasuredVelocity() const { return MeasuredVelocity; }
	void SampleMeasuredVelocity(float Now);

private:
	/** 길 밖 걸음에서 직전에 고른 방향 (수평 단위 벡터). 매 프레임 방향을 새로 골라 좌우로 흔들리지 않게 이어 쓴다 */
	FVector2D SteerHoldDir = FVector2D::ZeroVector;

	/**
	 * 줄 반응 지연. 앞자리가 비어도(앞사람이 칸으로 들어감·한 칸 당김) 바로 따라 움직이지 않고 잠깐 뒤에 움직인다.
	 * 줄 전체가 동시에 움직이면 기계처럼 보였다. -1이면 타이머 없음.
	 */
	float QueueStepReadyTime = -1.f;
	/** 줄 맨 앞: 칸이 비고 앞사람이 칸을 벗어난 뒤, 이 시각에 들어간다 */
	float SpotEnterReadyTime = -1.f;

	/** 스팟 칸 둘레에 나 말고 아무도 없는가 (앞사람이 칸을 완전히 벗어났나) */
	bool IsSpotStandClear(const ANpcActivitySpot* Spot) const;

	FVector2D MeasuredVelocity = FVector2D::ZeroVector;
	FVector LastMeasureLocation = FVector::ZeroVector;
	float LastMeasureTime = -1.f;
};
