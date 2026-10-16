// Fill out your copyright notice in the Description page of Project Settings.

#include "Activity/ActivitySubsystem.h"

#include <string>

#include "Activity/ActivitySetting.h"
#include "Dialogue/DialogueDataTypes.h"
#include "Dialogue/DialogueSubsystem.h"
#include "Engine/World.h"
#include "EntitySystem/MovieSceneEntitySystemRunner.h"
#include "GameMode/TimeSubsystem.h"
#include "Kismet/GameplayStatics.h"

#if !UE_BUILD_SHIPPING
// [skydodo] 2026-10-17 추가: 미니게임 테스트 맵에서는 시간대와 상관없이 AP를 채워 둔다.
// 기상 시간대 AP가 0이라 테스트 맵에서 AP 조건이 걸린 대화와 미니게임 문을 시험할 수 없었다.
namespace
{
	constexpr int32 TestMapAP = 3;

	bool IsMiniGameTestMap(const UWorld* World)
	{
		return World && UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) == TEXT("/Game/MiniGame/Maps/L_MiniGameTest");
	}
}
#endif


void UActivitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UTimeSubsystem>();
	Super::Initialize(Collection);
	const auto* S = GetDefault<UActivitySetting>();

	FixedEvents = S->FixedEventAsset.LoadSynchronous();
	FixedDialogues = S->FixedDialogueAsset.LoadSynchronous();

	// [수정] FixedEventAsset이 프로젝트 세팅에 지정되지 않았거나 로드에 실패하면
	// FixedEvents가 nullptr인 채로 GetAllRows()를 호출해서 즉시 크래시가 났다.
	// 애셋이 아직 없는 레벨/프로젝트에서도 최소한 에디터는 열려야 하므로 방어 코드를 추가한다.
	if (!FixedEvents)
	{
		UE_LOG(LogTemp, Error, TEXT("UActivitySubsystem::Initialize: FixedEventAsset이 설정되지 않았거나 로드에 실패했습니다. "
			"Project Settings > Activity Setting > Event Default > Fixed Event Asset을 확인하세요."));
	}
	else
	{
		TArray<FActivityDefinition*> Rows;
		FixedEvents->GetAllRows(TEXT("Fixed Event"), Rows);
	

		for (const FName& RowName : FixedEvents->GetRowNames())
		{
			if (const FActivityDefinition* Row = FixedEvents->FindRow<FActivityDefinition>(RowName, TEXT("Make Activity Map"), false))
			{
				AllActivities.Add(Row->ActivityNameTag, *Row);
			}
		}
	}

	if (UTimeSubsystem* TimeSub = GetGameInstance()->GetSubsystem<UTimeSubsystem>())
	{
		TimeSub->OnScheduleChanged.AddDynamic(this, &UActivitySubsystem::OnChangeSchedule);
		NowSchedule = TimeSub->GetNowScheduleTag();
	}

	// 레벨 로드 완료 시 활동 목록 재검사
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UActivitySubsystem::OnLevelLoaded);
	
	CurrentAP = 0;
	MaxAP = 0;

#if !UE_BUILD_SHIPPING
	// PIE 시작 맵은 PostLoadMapWithWorld가 오지 않으므로 월드 액터 초기화 시점에 채운다
	FWorldDelegates::OnWorldInitializedActors.AddWeakLambda(this, [this](const FActorsInitializedParams& Params)
	{
		if (IsMiniGameTestMap(Params.World) && CurrentAP < TestMapAP)
		{
			MaxAP = FMath::Max(MaxAP, TestMapAP);
			CurrentAP = TestMapAP;
		}
	});
#endif
}


void UActivitySubsystem::BindDialogue()
{
}
 
void UActivitySubsystem::FindRunnableActivities()
{
	const UWorld* World = GetWorld();
	if (!World || !FixedEvents) return;

	const FName NowLevelName = FName(*UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));

	RunnableActivities.Empty();

	for (const auto& Pair : AllActivities)
	{
		for (const FActivityLocation& Loc : Pair.Value.ActivityLocations)
		{
			UE_LOG(LogTemp, Warning, TEXT("Tag=%s Valid=%d Now=%s"), *Loc.ConnectedScheduleTag.ToString(), Loc.ConnectedScheduleTag.IsValid(), *NowSchedule.ToString());
			if (Loc.ConnectedScheduleTag.IsValid() && !Loc.ConnectedScheduleTag.HasTagExact(NowSchedule))
			{
				continue;
			}

			for (const TSoftObjectPtr<UWorld>& Level : Loc.ConnectedLevel)
			{
				const FName TargetLevelName = Level.ToSoftObjectPath().GetLongPackageFName();
				if (TargetLevelName == NowLevelName)
				{
					RunnableActivities.Add(Pair.Key, Pair.Value);
				}
			}
		}
	}
	SortRunnableActivity();
	// [skydodo] 2026-10-13 수정: 화면 출력 대신 Output Log로 (확인은 GetRunnableActivities 치트로)
	for (const auto& Pair : RunnableActivities)
	{
		UE_LOG(LogTemp, Log, TEXT("ActivitySubsystem: Runnable %s"), *Pair.Key.GetTagName().ToString());
	}
}

void UActivitySubsystem::PrintFixedActivities()
{
	TArray<FDialogueChoice> DialogueChoices;
	
	for (TPair<FGameplayTag, FActivityDefinition> Activity : RunnableActivities)
	{
		UE_LOG(LogTemp, Warning, TEXT("Activity Tag: %s"), *FText::FromName(Activity.Key.GetTagName()).ToString());
		UE_LOG(LogTemp, Warning, TEXT("Next Node Id: %s"), *Activity.Value.NodeId.ToString());

		TArray<FGameCondition> Condition;
		Condition.Add(FGameCondition(EGameConditionType::APAtLeat, NAME_None, Activity.Value.ActionPointCost));
		DialogueChoices.Add(FDialogueChoice(FText::FromName(Activity.Key.GetTagName()), Condition, TArray<FGameAction>(), Activity.Value.NodeId));
	}
	DialogueChoices.Add(FDialogueChoice(INVTEXT("자유행동"), TArray<FGameCondition>(), TArray<FGameAction>(), NAME_None));
	
	
	if (UDialogueSubsystem* DialogueSubsystem = GetWorld()->GetSubsystem<UDialogueSubsystem>())
	{
		//DialogueSubsystem->TryStartEventForNpc(FName(""), nullptr);
		FDialogueNodeRow Row = FDialogueNodeRow();
		for (int i = 0; i < DialogueChoices.Num() - 1; i++)
		{
			UE_LOG(LogTemp, Warning, TEXT("Choice Name: %s"), *DialogueChoices[i].Text.ToString());
		}
		DialogueChoices[DialogueChoices.Num() - 1].bFreeActivity = true;
		if (DialogueChoices.Num() == 0) return;
		Row.Choices = DialogueChoices;
		Row.Text = NSLOCTEXT("Dialogue", "Dialogue", "어떻게 할까...");

		DialogueSubsystem->SetDataTables(FixedEvents, FixedDialogues);
		DialogueSubsystem->ActivityDialoguePrint(Row);
		bWaitActivityChoice = true;
		bFreeActivity = false;
	}
	
}

bool UActivitySubsystem::RunActivityByIndex(int32 Index)
{
	TArray<FActivityDefinition> RunnableActivityValues;
	TArray<FGameplayTag> RunnableActivitiesTags;
	RunnableActivities.GenerateValueArray(RunnableActivityValues);
	RunnableActivities.GenerateKeyArray(RunnableActivitiesTags);
	
	if (Index < 0 || Index >= RunnableActivityValues.Num()) return false;
	if (!TryConsumeAP(RunnableActivityValues[Index].ActionPointCost)) return false;
	NowPlayedActivity = RunnableActivitiesTags[Index];

	OnActivityRequested.Broadcast(RunnableActivityValues[Index]);
	return true;
	// 	const FActivityDefinition* Definition = RunnableActivities.Find(ActivityTag);
	// 	if (!Definition)
	// 	{
	// 		UE_LOG(LogTemp, Warning, TEXT("RunActivity: Activity not runnable - %s"), *ActivityTag.ToString());
	// 		return;
	// 	}
	//
	// 	if (!TryConsumeAP(Definition->ActionPointCost))
	// 	{
	// 		return;
	// 	}
	//
	// 	OnActivityRequested.Broadcast(ActivityTag, *Definition);
	// 	GoHome();
}

FGameplayTag UActivitySubsystem::GetNowScheduleTag() const
{  
	return NowSchedule;
}

// [skydodo] 2026-10-13 버그 수정: 헤더만 Shipping 가드가 있고 정의는 없어 Shipping 빌드가 깨졌다
#if !UE_BUILD_SHIPPING
const TMap<FGameplayTag, FActivityDefinition>& UActivitySubsystem::GetAllActivities() const
{
	return AllActivities;
}

const TMap<FGameplayTag, FActivityDefinition>& UActivitySubsystem::GetRunnableActivities() const
{
	return RunnableActivities;
}
#endif

void UActivitySubsystem::OnChangeSchedule(FGameplayTag ChangedSchedule)
{
	NowSchedule = ChangedSchedule;
	if (UTimeSubsystem* TimeSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UTimeSubsystem>())
	{
		MaxAP = TimeSubsystem->GetNowScheduleAp();
		CurrentAP = MaxAP;
	}
#if !UE_BUILD_SHIPPING
	if (IsMiniGameTestMap(GetWorld()) && CurrentAP < TestMapAP)
	{
		MaxAP = FMath::Max(MaxAP, TestMapAP);
		CurrentAP = TestMapAP;
	}
#endif
	UE_LOG(LogTemp, Warning, TEXT("EventSubsystem: Detect Schedule Change"));
	UE_LOG(LogTemp, Warning, TEXT("EventSubsystem: To %s"), *NowSchedule.ToString());
	UE_LOG(LogTemp, Warning, TEXT("EventSubsystem: NowAp:  %d"), CurrentAP);

	FindRunnableActivities();
}

void UActivitySubsystem::OnLevelLoaded(UWorld* LoadedWorld)
{
	FindRunnableActivities();
}

void UActivitySubsystem::SortRunnableActivity()
{
	RunnableActivities.KeySort([](const FGameplayTag& A, const FGameplayTag& B)
	{
		return A.GetTagName().LexicalLess(B.GetTagName());
	});
}

void UActivitySubsystem::CheckCurrentAP()
{
	if (MaxAP > 0 && CurrentAP <= 0)
	{
		if (UTimeSubsystem* TimeSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UTimeSubsystem>())
		{
			TimeSubsystem->SpendSchedule();
		}
	}
}

FString UActivitySubsystem::FindLevelKey(const UWorld* World, const TMap<FName, TSoftObjectPtr<UWorld>>& Levels)
{
	if (!World) return FString();

	const FString Current = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
	for (const TPair<FName, TSoftObjectPtr<UWorld>>& Pair : Levels)
	{
		if (Pair.Value.ToSoftObjectPath().GetLongPackageName() == Current)
		{
			return Pair.Key.ToString();
		}
	}
	return FString();
}

int32 UActivitySubsystem::GetRunnableLength()
{
	return RunnableActivities.Num();
}

TArray<FGameplayTag> UActivitySubsystem::GetRunnableActivityTag() const
{
	TArray<FGameplayTag> Tags;
	RunnableActivities.GenerateKeyArray(Tags);
	return Tags;
}

bool UActivitySubsystem::TryConsumeAP(int32 Cost)
{
	if (CurrentAP < Cost)
	{
		UE_LOG(LogTemp, Warning, TEXT("TryConsumeAP: Not enough AP (Current: %d, Required: %d)"), CurrentAP, Cost);
		return false;
	}

	CurrentAP -= Cost;
	UE_LOG(LogTemp, Log, TEXT("TryConsumeAP: Consumed %d AP (Remaining: %d)"), Cost, CurrentAP);
	CheckCurrentAP();
	return true;
}

void UActivitySubsystem::GoHome()
{
	if (CurrentAP == 0)
	{
		const UActivitySetting* AS = GetDefault<UActivitySetting>();
		UGameplayStatics::OpenLevelBySoftObjectPtr(this, *AS->Levels.Find("Room"));
	}
}

int32 UActivitySubsystem::GetCurrentAP() const
{
	return CurrentAP;
}

void UActivitySubsystem::ActivityFinished(FName ActivityName)
{	
	FString LevelName = FindLevelKey(GetWorld(), GetDefault<UActivitySetting>()->Levels);
	if (LevelName.IsEmpty())
	{
		  LevelName = UGameplayStatics::GetCurrentLevelName(this);
	}
	ActivityName = FName("Activity." + LevelName + "." + ActivityName.ToString());
	FGameplayTag ActivityTag = FGameplayTag::RequestGameplayTag(ActivityName);
	UE_LOG(LogTemp, Warning, TEXT("ActivitySubsystem: Finished\n%s\n%s"), *ActivityName.ToString(), *ActivityTag.ToString());
	UE_LOG(LogTemp, Warning, TEXT("ActivitySubsystem: Finished: %s"), *NowPlayedActivity.ToString());
	if (NowPlayedActivity != ActivityTag) return;
	bFreeActivity = false;
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UActivitySubsystem::PrintFixedActivities);
	OnActivityFinished.Broadcast(FName("Fixed"), ActivityName);
}

void UActivitySubsystem::ActivityFinished(FName NpcName, int32 EventNumber)
{
	if (UDialogueSubsystem* DialogueSubsystem = GetWorld()->GetSubsystem<UDialogueSubsystem>())
	{
		if (DialogueSubsystem->CheckNpcCached(NpcName))
		{
			FName ActivityName = FName(FString::Printf(TEXT("%s_%d"), *NpcName.ToString(), EventNumber));
			UE_LOG(LogTemp, Warning, TEXT("ActivitySubsystem: Finished: %s"), *ActivityName.ToString());
			
			GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UActivitySubsystem::PrintFixedActivities);
			bFreeActivity = false;
			CheckCurrentAP();
			OnActivityFinished.Broadcast(NpcName, ActivityName);

		}	
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ActivityFinished: Spawn되지 않은 NPC %s"), *NpcName.ToString());
		}
	}
}

