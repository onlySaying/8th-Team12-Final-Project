// [skydodo] 2026-10-18 작성
// 요약: 목표 표시 구현 - 대상 찾기, 화살표 위치, 화면 문구

#include "Objective/ObjectiveSubsystem.h"

#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "NPC/BaseNpcCharacter.h"
#include "Objective/ObjectiveMarker.h"
#include "Objective/ObjectiveWidget.h"

namespace ObjectiveLayout
{
	/** 머리 위로 띄우는 간격 */
	constexpr float HeadGap = 35.f;
}

void UObjectiveSubsystem::SetObjective(FName InTargetId, const FText& Text)
{
	TargetId = InTargetId;
	ObjectiveText = Text;
	CachedTarget.Reset();
	UpdateWidget();
}

void UObjectiveSubsystem::ClearObjective()
{
	TargetId = NAME_None;
	ObjectiveText = FText::GetEmpty();
	CachedTarget.Reset();
	UpdateWidget();
}

TStatId UObjectiveSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UObjectiveSubsystem, STATGROUP_Tickables);
}

bool UObjectiveSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UObjectiveSubsystem::Deinitialize()
{
	if (Widget)
	{
		Widget->RemoveFromParent();
		Widget = nullptr;
	}
	Super::Deinitialize();
}

void UObjectiveSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UpdateMarker(ResolveTarget(), DeltaTime);
}

AActor* UObjectiveSubsystem::ResolveTarget()
{
	if (TargetId.IsNone())
	{
		return nullptr;
	}

	if (CachedTarget.IsValid())
	{
		return CachedTarget.Get();
	}

	// 대사 액션이 NPC를 막 스폰한 직후엔 아직 없을 수 있어, 못 찾으면 다음 프레임에 다시 본다
	for (TActorIterator<ABaseNpcCharacter> It(GetWorld()); It; ++It)
	{
		if (It->GetNpcId() == TargetId)
		{
			CachedTarget = *It;
			return *It;
		}
	}

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(TargetId))
		{
			CachedTarget = *It;
			return *It;
		}
	}
	return nullptr;
}

void UObjectiveSubsystem::UpdateMarker(AActor* Target, float DeltaTime)
{
	if (!Target)
	{
		if (Marker)
		{
			Marker->SetActorHiddenInGame(true);
		}
		return;
	}

	if (!Marker)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Marker = GetWorld()->SpawnActor<AObjectiveMarker>(AObjectiveMarker::StaticClass(), FTransform::Identity, Params);
		if (!Marker)
		{
			return;
		}
	}

	// 캐릭터는 캡슐 꼭대기, 그 밖의 액터는 바운드 꼭대기를 기준으로 삼는다
	FVector Top;
	if (const ACharacter* Character = Cast<ACharacter>(Target))
	{
		Top = Character->GetActorLocation() + FVector(0.f, 0.f, Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	}
	else
	{
		FVector Origin, Extent;
		Target->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
		Top = Origin + FVector(0.f, 0.f, Extent.Z);
	}
	Marker->SetAnchor(Top + FVector(0.f, 0.f, ObjectiveLayout::HeadGap));
	Marker->SetActorHiddenInGame(false);
}

void UObjectiveSubsystem::UpdateWidget()
{
	if (ObjectiveText.IsEmpty())
	{
		if (Widget)
		{
			Widget->RemoveFromParent();
		}
		return;
	}

	if (!Widget)
	{
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		if (!PC)
		{
			return;
		}
		Widget = CreateWidget<UObjectiveWidget>(PC, UObjectiveWidget::StaticClass());
	}
	if (Widget)
	{
		if (!Widget->IsInViewport())
		{
			Widget->AddToViewport(5);
		}
		Widget->SetObjectiveText(ObjectiveText);
	}
}
