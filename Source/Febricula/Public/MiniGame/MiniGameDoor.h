// [skydodo] 2026-10-10 작성
// 요약: 미니게임 레벨로 들어가는 문 - E키 상호작용으로 레벨 이동
// Fill out your copyright notice in the Description page of Project Settings.
//
// 레벨에 놓고 TargetLevel만 지정하면 된다. 플레이어의 InteractionComponent가 잡아서
// "E - 햄버거집 알바 가기" 같은 프롬프트를 띄우고, 누르면 그 레벨을 연다.
// 도착한 레벨에서 무엇을 할지는 MiniGame Setting이 정한다 (UMiniGameSubsystem).

#pragma once

#include "CoreMinimal.h"
#include "Dialogue/DialogueDataTypes.h"
#include "GameFramework/Actor.h"
#include "Interaction/Interactable.h"
#include "MiniGameDoor.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS()
class FEBRICULA_API AMiniGameDoor : public AActor, public IInteractable
{
	GENERATED_BODY()

public:
	AMiniGameDoor();

	virtual void OnConstruction(const FTransform& Transform) override;

	//~ Begin IInteractable
	virtual bool CanInteract_Implementation(AActor* Interactor) const override;
	virtual void Interact_Implementation(AActor* Interactor) override;
	virtual FText GetInteractionPrompt_Implementation(AActor* Interactor) const override;
	//~ End IInteractable

protected:
	/** 넘어갈 레벨 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	TSoftObjectPtr<UWorld> TargetLevel;

	/**
	 * [skydodo] 2026-10-18 추가: 비우지 않으면 레벨을 옮기지 않고 지금 레벨에서 이 미니게임을 띄운다 (TargetLevel보다 우선).
	 * MiniGame Setting의 키를 쓴다 (예: Burger, Flower, L_ConvenienceStore)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	FName MiniGameKey;

	/** 상호작용 프롬프트 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	FText Prompt;

	/** 문 위에 띄울 이름. 비우면 Prompt를 쓴다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	FText Label;

	/** 상호작용 감지용. 메시가 없어도 잡힌다 */
	UPROPERTY(VisibleAnywhere, Category = "MiniGame")
	TObjectPtr<UBoxComponent> Box;

	/** 겉모습 (선택). 비워 두면 박스와 이름만 보인다 */
	UPROPERTY(VisibleAnywhere, Category = "MiniGame")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "MiniGame")
	TObjectPtr<UTextRenderComponent> LabelText;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MiniGame")
	TArray<FGameCondition> InteractConditions; 

	/** 한 번 눌렀으면 레벨이 넘어갈 때까지 다시 받지 않는다 */
	bool bTraveling = false;
};
