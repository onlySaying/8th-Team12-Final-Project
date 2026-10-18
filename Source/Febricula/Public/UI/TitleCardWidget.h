// [skydodo] 2026-10-18 작성
// 요약: 화면 전체를 덮는 검은 카드에 큰 제목과 작은 부제를 띄웠다가 사라지는 위젯 (날짜 넘김, 엔딩 이름)
// 대사 데이터의 ShowTitleCard 액션이 띄운다. 에셋 없이 C++로 위젯 트리를 만든다

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "TitleCardWidget.generated.h"

class UTextBlock;

UCLASS()
class FEBRICULA_API UTitleCardWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** HoldSeconds 동안 보여 준 뒤 사라지고 스스로 화면에서 빠진다 */
	void Show(const FText& InTitle, const FText& InSubtitle, float InHoldSeconds);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY()
	TObjectPtr<UTextBlock> Title;

	UPROPERTY()
	TObjectPtr<UTextBlock> Subtitle;

	float Elapsed = 0.f;
	float HoldSeconds = 2.f;

	static constexpr float FadeInSeconds = 0.2f;
	static constexpr float FadeOutSeconds = 0.6f;
};
