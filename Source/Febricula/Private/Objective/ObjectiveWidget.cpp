// [skydodo] 2026-10-18 작성
// 요약: 목표 문구 위젯 구현

#include "Objective/ObjectiveWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"

TSharedRef<SWidget> UObjectiveWidget::RebuildWidget()
{
	// 블루프린트 없이 쓰는 위젯이라 처음 그릴 때 트리를 직접 만든다
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		WidgetTree->RootWidget = Root;

		Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ObjectiveLabel"));
		FSlateFontInfo Font = Label->GetFont();
		Font.Size = 26;
		Label->SetFont(Font);
		Label->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.3f)));
		Label->SetShadowOffset(FVector2D(1.5f, 1.5f));
		Label->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f));

		if (UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(Label))
		{
			CanvasSlot->SetAutoSize(true);
			CanvasSlot->SetPosition(FVector2D(48.f, 40.f));
		}
	}
	return Super::RebuildWidget();
}

void UObjectiveWidget::SetObjectiveText(const FText& InText)
{
	if (Label)
	{
		Label->SetText(FText::Format(NSLOCTEXT("Objective", "Format", "▶ {0}"), InText));
	}
}
