// [skydodo] 2026-08-17 작성
// 요약: 조사 가능한 사물 구현 - 나레이션 출력, 액션 실행
// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/InteractableProp.h"

#include "Blueprint/UserWidget.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dialogue/DialogueSubsystem.h"
#include "Engine/World.h"
#include "NPC/MainNpc/MainNpcCharacter.h"
#include "Progress/GameProgressSubsystem.h"

AInteractableProp::AInteractableProp()
{
	PrimaryActorTick.bCanEverTick = false;
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneComponent"));
	RootComponent = SceneComponent;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(RootComponent);
	//SetRootComponent(Mesh);

	InteractionVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("InteractionVolume"));
	InteractionVolume->SetupAttachment(Mesh);
	InteractionVolume->SetBoxExtent(FVector(60.f));
	// 상호작용 컴포넌트의 오버랩 검사에만 걸리면 되므로 물리 충돌은 끈다.
	InteractionVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	InteractionVolume->SetCollisionResponseToAllChannels(ECR_Overlap);

	InteractionPrompt = NSLOCTEXT("Prop", "InspectPrompt", "E - 조사하기");
}

bool AInteractableProp::CanInteract_Implementation(AActor* Interactor) const
{
	if (bSingleUse && bUsed)
	{
		return false;
	}

	// 이미 대화 중이면 새 조사를 시작하지 않는다.
	if (const UWorld* World = GetWorld())
	{
		if (const UDialogueSubsystem* Dialogue = World->GetSubsystem<UDialogueSubsystem>())
		{
			if (Dialogue->IsDialogueActive())
			{
				return false;
			}
			if (bOnlyWhenEventAvailable && !DialogueNodeId.IsNone())
			{
				return Dialogue->HasStartableEvent(ConnectedEventTable, DialogueNodeId);
			}
		}
	}

	return InspectLines.Num() > 0 || !DialogueNodeId.IsNone() || bInteractable;
}

void AInteractableProp::Interact_Implementation(AActor* Interactor)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UDialogueSubsystem* Dialogue = World->GetSubsystem<UDialogueSubsystem>();
	if (!Dialogue)
	{
		return;
	}

	bool bStarted = false;
	
	if (!DialogueNodeId.IsNone())
	{
		if (UDataTable* DialogueDT = DefaultDialogueTable.LoadSynchronous())
		{
			Dialogue->SetDataTables(ConnectedEventTable, DialogueDT);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("DialogueSubsystem: 이벤트 테이블 '%s'의 기본 Dialogue Table '%s'을(를) 찾지 못했습니다."), *GetNameSafe(ConnectedEventTable), *DefaultDialogueTable.GetAssetName());	
			return;
		}
		
		// 선택지가 필요한 사물은 대화 테이블 노드를 그대로 재생한다.
		if (AMainNpcCharacter* Npc = Cast<AMainNpcCharacter>(GetOwner()))
		{
			bStarted = Dialogue->TryStartEventForNpc(Npc->GetNpcId(), this, DialogueNodeId);
		}
		else
		{
			bStarted = Dialogue->TryStartEventForNpc(NAME_None, this, DialogueNodeId);
		}
	}
	else
	{
		bStarted = Dialogue->StartSimpleLines(SpeakerName, InspectLines, this);
	}
	if (bStarted)
	{
		bUsed = true;
	}
	if (!bWaitForSequence)
	{
		ExecuteActionsWrapper();
	}

	if (InspectWidgetClass)
	{
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			if (UUserWidget* Widget = CreateWidget<UUserWidget>(PC, InspectWidgetClass))
			{
				Widget->AddToViewport();
			}
		}
	}
}

FText AInteractableProp::GetInteractionPrompt_Implementation(AActor* Interactor) const
{
	return InteractionPrompt;
}

void AInteractableProp::ExecuteActionsWrapper()
{
	//FInteractionBranch NowBranch;
	// if (const UGameInstance* GI = GetGameInstance())
	// {
		// if (const UGameProgressSubsystem* GSP = GI->GetSubsystem<UGameProgressSubsystem>())
		// {
		// 	for (FInteractionBranch Branch : InteractionBranches)
		// 	{
		// 		if (GSP->EvaluateConditions(Branch.Conditions))
		// 		{
		// 			NowBranch = Branch;
		// 		}
		// 	}
		// }
	// }
	if (UDialogueSubsystem* Dialogue = GetWorld()->GetSubsystem<UDialogueSubsystem>())
	{
		if (Dialogue->GetResult())
		{
			Dialogue->ExecuteQueuedActions();
		}
	}
}
