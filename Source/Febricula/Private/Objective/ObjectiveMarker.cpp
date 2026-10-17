// [skydodo] 2026-10-18 작성
// 요약: 목표 화살표 구현. 엔진 기본 원뿔을 뒤집어 쓴다

#include "Objective/ObjectiveMarker.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace ObjectiveMarkerStyle
{
	constexpr float BobHeight = 12.f;
	constexpr float BobSpeed = 3.f;
	constexpr float SpinSpeed = 90.f;
	const FLinearColor Color(1.f, 0.75f, 0.1f);
}

AObjectiveMarker::AObjectiveMarker()
{
	PrimaryActorTick.bCanEverTick = true;

	Arrow = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Arrow"));
	RootComponent = Arrow;
	Arrow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Arrow->SetCastShadow(false);
	Arrow->SetGenerateOverlapEvents(false);
	// 원뿔 끝이 아래를 향하게 뒤집고, 사람 머리 위에 맞는 크기로 줄인다
	Arrow->SetRelativeRotation(FRotator(180.f, 0.f, 0.f));
	Arrow->SetRelativeScale3D(FVector(0.22f, 0.22f, 0.3f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (ConeMesh.Succeeded())
	{
		Arrow->SetStaticMesh(ConeMesh.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ShapeMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (ShapeMaterial.Succeeded())
	{
		Arrow->SetMaterial(0, ShapeMaterial.Object);
	}
}

void AObjectiveMarker::BeginPlay()
{
	Super::BeginPlay();

	if (UMaterialInstanceDynamic* MID = Arrow->CreateDynamicMaterialInstance(0))
	{
		MID->SetVectorParameterValue(TEXT("Color"), ObjectiveMarkerStyle::Color);
	}
}

void AObjectiveMarker::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	Elapsed += DeltaTime;
	// 원뿔 피벗이 가운데라 높이의 절반만큼 올려야 끝이 Anchor에 닿는다
	const float HalfHeight = 50.f * GetActorScale3D().Z;
	const float Bob = (FMath::Sin(Elapsed * ObjectiveMarkerStyle::BobSpeed) + 1.f) * 0.5f * ObjectiveMarkerStyle::BobHeight;
	SetActorLocation(Anchor + FVector(0.f, 0.f, HalfHeight + Bob));
	SetActorRotation(FRotator(180.f, Elapsed * ObjectiveMarkerStyle::SpinSpeed, 0.f));
}
