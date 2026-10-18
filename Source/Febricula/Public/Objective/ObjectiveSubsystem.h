// [skydodo] 2026-10-18 작성
// 요약: 지금 어디로 가서 무엇과 상호작용해야 하는지 보여 주는 목표 표시 (화살표 + 화면 문구)
//
// 대사 데이터의 SetObjective 액션이 목표를 정한다 (Key = NpcId 또는 액터 태그, Key2 = 화면 문구).
// 목표는 레벨에 묶여 있어 레벨이 바뀌면 사라진다.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ObjectiveSubsystem.generated.h"

class AObjectiveMarker;
class UObjectiveWidget;

UCLASS()
class FEBRICULA_API UObjectiveSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** TargetId(NpcId 또는 액터 태그) 위에 화살표를 띄우고 화면에 문구를 쓴다 */
	void SetObjective(FName TargetId, const FText& Text);

	void ClearObjective();

	/** 지금 목표 대상 (없으면 nullptr). 자동 플레이가 걸어갈 곳을 정할 때 쓴다 */
	AActor* GetTargetActor() { return TargetId.IsNone() ? nullptr : ResolveTarget(); }

	//~ Begin UTickableWorldSubsystem
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;
	//~ End UTickableWorldSubsystem

private:
	AActor* ResolveTarget();
	void UpdateMarker(AActor* Target, float DeltaTime);
	void UpdateWidget();

	FName TargetId;
	FText ObjectiveText;

	/** 태그로 찾은 액터는 매 프레임 다시 찾지 않는다 */
	TWeakObjectPtr<AActor> CachedTarget;

	UPROPERTY()
	TObjectPtr<AObjectiveMarker> Marker;

	UPROPERTY()
	TObjectPtr<UObjectiveWidget> Widget;
};
