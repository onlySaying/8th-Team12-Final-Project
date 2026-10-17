// [skydodo] 2026-10-18 작성
// 요약: 목표 대상 머리 위에서 위아래로 흔들리며 도는 화살표

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ObjectiveMarker.generated.h"

class UStaticMeshComponent;

UCLASS(NotPlaceable)
class FEBRICULA_API AObjectiveMarker : public AActor
{
	GENERATED_BODY()

public:
	AObjectiveMarker();

	/** 화살표 끝이 가리킬 위치. 흔들림은 여기서 위쪽으로만 더한다 */
	void SetAnchor(const FVector& InAnchor) { Anchor = InAnchor; }

	virtual void Tick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, Category = "Objective")
	TObjectPtr<UStaticMeshComponent> Arrow;

private:
	FVector Anchor = FVector::ZeroVector;
	float Elapsed = 0.f;
};
