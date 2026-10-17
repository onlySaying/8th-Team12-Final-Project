// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "FebriculaCharacter.generated.h"

class UCharacterAnimationComponent;
class UInteractionComponent;
class AInteractableProp;
class AInteractableActor;
class USphereComponent;
class USpringArmComponent;
class UCameraComponent;
class UInputAction;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);

/**
 *  A simple player-controllable third person character
 *  Implements a controllable orbiting camera
 */
UCLASS(abstract)
class AFebriculaCharacter : public ACharacter
{
	GENERATED_BODY()

	/** Camera boom positioning the camera behind the character */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;

protected:
	UPROPERTY(EditAnywhere, Category = "Speed")
	float WalkSpeed;
	UPROPERTY(EditAnywhere, Category = "Speed")
	float SprintSpeed;


	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components | Interaction", meta = (AllowPrivateAccess = "true"))
	TArray<TObjectPtr<AInteractableProp>> OverlappedActors;

	/**
	 *	여기 적힌 레벨에서는 **스프린트가 안 먹는다.** 비우면 어디서나 된다.
	 *
	 *	## 왜 입력을 안 끊고 여기서 막나
	 *
	 *	스프린트는 전용 IMC 가 없고 `Move`·`Look` 과 **같은 입력 컴포넌트**에
	 *	묶여 있다 (`SetupPlayerInputComponent`). 매핑을 떼면 이동까지 죽는다.
	 *	그래서 바인딩은 그대로 두고 **동작만** 막는다.
	 *
	 *	⚠ 이름은 `UGameplayStatics::GetCurrentLevelName(..., true)` 와 비교한다 —
	 *	  PIE 접두사(`UEDPIE_0_`)는 그 함수가 떼어 준다. 경로가 아니라 **이름만**
	 *	  넣을 것 (`Room`, `/Game/Levels/Room` 아님).
	 *
	 *	⚠⚠ **퍼시스턴트 레벨 이름**이다. 그 레벨을 다른 맵의 서브레벨로
	 *	  스트리밍해 넣으면 이름이 안 맞아 스프린트가 **다시 풀린다.** 그때는
	 *	  퍼시스턴트 쪽 이름을 넣거나 판정을 바꿔야 한다.
	 */
	UPROPERTY(EditAnywhere, Category="Speed")
	TArray<FName> SprintBlockedLevels;

	/** 지금 레벨에서 스프린트가 허용되는가. */
	UFUNCTION(BlueprintPure, Category="Speed")
	bool IsSprintAllowed() const;

	/**
	 *	레벨별 걷기 속도 덮어쓰기. 없는 레벨은 `WalkSpeed` 를 쓴다.
	 *
	 *	⚠ 레벨 이름 규칙은 `SprintBlockedLevels` 와 **같다** (경로가 아니라 이름,
	 *	  퍼시스턴트 레벨 기준).
	 *
	 *	⚠⚠ **`WalkSpeed` 를 직접 바꾸면 안 된다.** 그러면 모든 레벨이 같이
	 *	  바뀐다. 한 레벨만 느리게 하려고 만든 것이 이 맵이다.
	 */
	UPROPERTY(EditAnywhere, Category="Speed")
	TMap<FName, float> LevelWalkSpeedOverrides;

	/**
	 *	지금 레벨에서 쓸 걷기 속도. 오버라이드가 있으면 그 값, 없으면 `WalkSpeed`.
	 *
	 *	⚠ `MaxWalkSpeed` 를 되돌리는 곳은 **전부 이걸 써야 한다.** `WalkSpeed` 를
	 *	  그대로 쓰면 스프린트를 한 번 하고 나서 오버라이드가 풀린다.
	 */
	UFUNCTION(BlueprintPure, Category="Speed")
	float GetEffectiveWalkSpeed() const;

	/** Jump Input Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* JumpAction;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* SprintAction;

	/** ��� ���� �ѱ��(��ŵ) Input Action. ������ ���� ��ȭ�� ������ ����ȴ� */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* FastForwardAction;

	/** Look Input Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* LookAction;

	/** Mouse Look Input Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MouseLookAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* InteractAction;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Character Animation Component")
	TObjectPtr<UCharacterAnimationComponent> CharacterAnimationComponent;

public:
	AFebriculaCharacter();


	UFUNCTION()
	void OnComponentBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep,
		const FHitResult& SweepResult);
	UFUNCTION()
	void OnComponentEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);
	/** Constructor */
	virtual void BeginPlay() override;

	void FaceTowards(const AActor* TargetActor);


protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components | Interaction", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInteractionComponent> InteractComponent;

protected:


	/** Initialize input action bindings */
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	void FixedMove(const FInputActionValue& Value);
	
	/** Called for looking input */
	void Look(const FInputActionValue& Value);

	void StartSprint(const FInputActionValue& Value);
	void EndSprint(const FInputActionValue& Value);

	/** Ctrl ���� ������ ���� ��縦 ������ �ѱ�� */
	void StartFastForward(const FInputActionValue& Value);
	/** Ű�� ���� ���� �ӵ��� �ǵ����� */
	void EndFastForward(const FInputActionValue& Value);

public:

	/** Handles move inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoMove(float Right, float Forward);

	/** Handles look inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoLook(float Yaw, float Pitch);

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoJumpStart();

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoJumpEnd();

public:

	/** Returns CameraBoom subobject **/
	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	/** Returns FollowCamera subobject **/
	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components | Interaction | Debug", meta = (AllowPrivateAccess = "true"))
	float InteractionCos;

};

