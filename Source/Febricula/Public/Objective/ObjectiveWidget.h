// [skydodo] 2026-10-18 작성
// 요약: 화면 왼쪽 위에 지금 목표를 한 줄로 보여 주는 위젯. 에셋 없이 C++로 위젯 트리를 만든다

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ObjectiveWidget.generated.h"

class UTextBlock;

UCLASS()
class FEBRICULA_API UObjectiveWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetObjectiveText(const FText& InText);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UPROPERTY()
	TObjectPtr<UTextBlock> Label;
};
