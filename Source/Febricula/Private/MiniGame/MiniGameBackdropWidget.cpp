// [skydodo] 2026-10-18 작성
// 요약: 미니게임 배경 색지 구현

#include "MiniGame/MiniGameBackdropWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"

TSharedRef<SWidget> UMiniGameBackdropWidget::RebuildWidget()
{
	// 블루프린트 없이 쓰는 위젯이라 처음 그릴 때 트리를 직접 만든다
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		Fill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Fill"));
		Fill->SetBrushColor(Color);
		// 클릭이 미니게임 위젯으로 가도록 배경은 입력을 받지 않는다
		Fill->SetVisibility(ESlateVisibility::HitTestInvisible);
		WidgetTree->RootWidget = Fill;
	}
	return Super::RebuildWidget();
}

void UMiniGameBackdropWidget::SetColor(const FLinearColor& InColor)
{
	Color = InColor;
	if (Fill)
	{
		Fill->SetBrushColor(Color);
	}
}
