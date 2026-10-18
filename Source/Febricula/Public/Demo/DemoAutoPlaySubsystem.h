// [skydodo] 2026-10-18 작성
// 요약: 시연 영상 녹화용 자동 플레이. -AutoPlay=A|B|C 로 실행하면 타이틀부터 엔딩까지 혼자 진행한다
//
// 사람이 하는 것과 같은 순서로만 움직인다:
//   타이틀의 "시작" 버튼 누르기 → 대사는 글자 수만큼 기다렸다 넘기기 → 선택지는 경로별 순서대로 고르기
//   → 목표 화살표 대상까지 걸어가기 (대상이 상호작용 물체면 상호작용) → 미니게임은 잠깐 보여 주고 끝내기
//   → 엔딩 후 타이틀로 돌아오면 게임 종료 (녹화를 끝낼 시점)
// 선택지 순서는 -AutoPlayChoices=0,1,0 처럼 직접 줄 수도 있다.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "DemoAutoPlaySubsystem.generated.h"

class UDialogueSubsystem;

UCLASS()
class FEBRICULA_API UDemoAutoPlaySubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	//~ Begin UGameInstanceSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	//~ End UGameInstanceSubsystem

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableWhenPaused() const override { return false; }
	virtual ETickableTickType GetTickableTickType() const override { return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Always; }
	//~ End FTickableGameObject

private:
	void TickTitle(UWorld& World);
	void TickDialogue(UDialogueSubsystem& Dialogue, float DeltaTime);
	/** 목표 대상까지 걷는다. 도착했으면 true */
	bool TickWalk(UWorld& World, float DeltaTime);
	void StopWalk(UWorld& World);

	/** 경로별 선택지 순서. 다 쓰면 0번을 고른다 */
	TArray<int32> ChoicePlan;
	int32 ChoiceCursor = 0;

	/** 레벨이 바뀌면 상태를 처음부터 */
	TWeakObjectPtr<UWorld> LastWorld;
	float WorldTime = 0.f;

	bool bRunStarted = false;

	FString LastLine;
	float LineTime = 0.f;
	float ChoiceTime = 0.f;
	float MiniGameTime = 0.f;

	float RepathTime = 0.f;
	float StuckTime = 0.f;
	float DirectMoveTime = 0.f;
	float BestDistance = TNumericLimits<float>::Max();
	float InteractCooldown = 0.f;
	TWeakObjectPtr<AActor> WalkTarget;

	/** 지금 목표까지의 길찾기 경유점 (0.5초마다 다시 구한다) */
	TArray<FVector> PathPoints;
};
