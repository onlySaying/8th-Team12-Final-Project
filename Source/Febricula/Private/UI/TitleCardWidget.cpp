// [skydodo] 2026-10-18 작성
// 요약: 타이틀 카드 위젯 구현

#include "UI/TitleCardWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

namespace
{
	UTextBlock* MakeLabel(UWidgetTree* Tree, FName Name, int32 Size, const FLinearColor& Color)
	{
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = Size;
		Text->SetFont(Font);
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetJustification(ETextJustify::Center);
		return Text;
	}
}

TSharedRef<SWidget> UTitleCardWidget::RebuildWidget()
{
	// 블루프린트 없이 쓰는 위젯이라 처음 그릴 때 트리를 직접 만든다
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
		Root->SetBrushColor(FLinearColor::Black);
		Root->SetHorizontalAlignment(HAlign_Center);
		Root->SetVerticalAlignment(VAlign_Center);
		WidgetTree->RootWidget = Root;

		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Box"));
		Root->SetContent(Box);

		Title = MakeLabel(WidgetTree, TEXT("Title"), 54, FLinearColor(0.95f, 0.95f, 0.95f));
		Subtitle = MakeLabel(WidgetTree, TEXT("Subtitle"), 24, FLinearColor(0.6f, 0.6f, 0.6f));

		if (UVerticalBoxSlot* TitleSlot = Box->AddChildToVerticalBox(Title))
		{
			TitleSlot->SetHorizontalAlignment(HAlign_Center);
		}
		if (UVerticalBoxSlot* SubSlot = Box->AddChildToVerticalBox(Subtitle))
		{
			SubSlot->SetHorizontalAlignment(HAlign_Center);
			SubSlot->SetPadding(FMargin(0.f, 18.f, 0.f, 0.f));
		}
	}
	return Super::RebuildWidget();
}

void UTitleCardWidget::Show(const FText& InTitle, const FText& InSubtitle, float InHoldSeconds)
{
	// 트리가 아직 없으면 지금 만든다 (AddToViewport 전에 불러도 글자가 들어가게)
	TakeWidget();
	Title->SetText(InTitle);
	Subtitle->SetText(InSubtitle);
	Subtitle->SetVisibility(InSubtitle.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);

	HoldSeconds = FMath::Max(InHoldSeconds, 0.f);
	Elapsed = 0.f;
	SetVisibility(ESlateVisibility::HitTestInvisible);
	SetRenderOpacity(0.f);
}

void UTitleCardWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	Elapsed += InDeltaTime;
	const float FadeOutStart = FadeInSeconds + HoldSeconds;

	if (Elapsed < FadeInSeconds)
	{
		SetRenderOpacity(Elapsed / FadeInSeconds);
	}
	else if (Elapsed < FadeOutStart)
	{
		SetRenderOpacity(1.f);
	}
	else if (Elapsed < FadeOutStart + FadeOutSeconds)
	{
		SetRenderOpacity(1.f - (Elapsed - FadeOutStart) / FadeOutSeconds);
	}
	else
	{
		RemoveFromParent();
	}
}
