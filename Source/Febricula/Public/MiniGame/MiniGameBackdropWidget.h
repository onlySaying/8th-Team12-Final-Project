// [skydodo] 2026-10-18 작성
// 요약: 미니게임 위젯 뒤에 화면 전체로 까는 단색 배경. 에셋 없이 C++로 위젯 트리를 만든다

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MiniGameBackdropWidget.generated.h"

class UBorder;

UCLASS()
class FEBRICULA_API UMiniGameBackdropWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetColor(const FLinearColor& InColor);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UPROPERTY()
	TObjectPtr<UBorder> Fill;

	FLinearColor Color = FLinearColor::Black;
};
