// [skydodo] 2026-08-17 작성
// 요약: 조사 가능한 사물 - NPC와 같은 HUD로 나레이션을 띄운다
// Fill out your copyright notice in the Description page of Project Settings.
//
// 조사 가능한 사물. NPC와 똑같은 HUD를 써서 "시체인 것 같다." 같은 나레이션을 띄운다.
// 화자 이름을 비워두면 HUD가 이름칸을 감추고 나레이션처럼 보여준다.
//
// 두 가지 방식을 지원한다:
//   1) InspectLines에 문장을 직접 적기 (간단한 사물)
//   2) DialogueNodeId를 지정해 대화 테이블의 노드를 재생 (선택지가 필요한 사물)

#pragma once

#include "CoreMinimal.h"
#include "InteractionBranch.h"
#include "GameFramework/Actor.h"
#include "Interaction/Interactable.h"
#include "InteractableProp.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UUserWidget;

UCLASS()
class FEBRICULA_API AInteractableProp : public AActor, public IInteractable
{
	GENERATED_BODY()

public:
	AInteractableProp();

	//~ Begin IInteractable
	virtual bool CanInteract_Implementation(AActor* Interactor) const override;
	virtual void Interact_Implementation(AActor* Interactor) override;
	virtual FText GetInteractionPrompt_Implementation(AActor* Interactor) const override;
	//~ End IInteractable

protected:
	/**
	 * 이지민 수정(2026. 08. 27)
	 * InteractableProp이 가진 Action과 Condition을 조회하고 실행해주는 로직 
	 * DT를 추가로 생성하지 않고도 상호작용하면 로직 실행 개념 구현
	 */

	UFUNCTION(BlueprintCallable, BlueprintCallable)
	virtual void ExecuteActionsWrapper();
	
	/*
	 * 이지민 수정(2026. 08. 27)
	 * 플레이어 쪽에 부착된 로직이 완료되는 것을 기다릴지 / 즉시 실행할지
	 * 문 개폐 로직과 같은 것을 처리하기 위해 추가
	 */
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prop")
	bool bInteractable;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prop")
	bool bWaitForSequence;
	
	/*
	 * 연관된 이벤트 DT. Interaction Component에서 여기로 이동
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prop")
	TObjectPtr<UDataTable> ConnectedEventTable;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prop")
	TSoftObjectPtr<UDataTable> DefaultDialogueTable;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Prop")
	TObjectPtr<USceneComponent> SceneComponent;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Prop")
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** 상호작용 감지용. 메시가 없어도 잡히도록 별도 콜리전을 둔다 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Prop")
	TObjectPtr<UBoxComponent> InteractionVolume;

	/** "E - 조사하기" 처럼 표시할 프롬프트 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop")
	FText InteractionPrompt;

	/** 화면에 표시할 화자. 비워두면 나레이션 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop")
	FText SpeakerName;

	/** 순서대로 출력할 조사 문구. 예: "시체인 것 같다." */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop", meta = (MultiLine = true))
	TArray<FText> InspectLines;

	/**
	 * 비워두지 않으면 InspectLines 대신 대화 테이블의 이 노드를 재생한다.
	 * 선택지가 필요한 사물(잠긴 문, 청소도구함 등)에 쓴다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop")
	FName DialogueNodeId;

	/** 한 번만 조사 가능하게 할지 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop")
	bool bSingleUse = false;

	/**
	 * [skydodo] 2026-10-18 추가: 켜면 ConnectedEventTable에 DialogueNodeId로 시작하는 이벤트를
	 * 지금 시작할 수 있을 때만 상호작용 문구를 띄운다 (예: 아침에만 열리는 방문)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop")
	bool bOnlyWhenEventAvailable = false;

	/** 상호작용 시 함께 띄울 위젯 클래스. 비워두면 아무것도 열지 않는다 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop")
	TSubclassOf<UUserWidget> InspectWidgetClass;


	/**
	 * 이지민 수정(2026. 08. 27)
	 * InteractableProp이 Action과 Condition을 가짐으로써 기존의 Dialogue를 거치지 않고도 
	 * 특정 로직을 실행할 수 있도록 변경
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prop | Interaction Branch")
	TArray<FInteractionBranch> InteractionBranches;
	
	UPROPERTY(Transient)
	bool bUsed = false;
	
	UPROPERTY(Transient)
	bool DialogueResult = false;
};
