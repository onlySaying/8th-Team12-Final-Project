// [skydodo] 2026-09-01 작성 (최종 수정 2026-10-08)
// 요약: 행인 군중 감독 서브시스템 구현 - 풀링/스폰·회수, 거리 LOD 분할 갱신, 원경 표현, crowd.* 콘솔 명령
// Fill out your copyright notice in the Description page of Project Settings.

#include "NPC/SubNpc/SubNpcDirectorSubsystem.h"

#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
// TActorIterator를 쓰려면 반드시 필요하다. 없으면 "정의되지 않은 class TActorIterator" 에러가 난다.
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameMode/NPCSetting.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "NPC/SubNpc/NpcActivitySpot.h"
#include "NPC/SubNpc/NpcCrowdPath.h"
#include "NPC/SubNpc/SubNpcMassProcessors.h"
#include "NPC/SubNpc/SubNpcMassStateTree.h"
#include "MassStateTreeFragments.h"
#include "StateTree.h"
#include "StructUtils/InstancedStruct.h"
#include "NPC/SubNpc/NpcCrowdSpawner.h"
#include "NPC/SubNpc/SubNpcCharacter.h"
#include "NPC/SubNpc/SubNpcLog.h"
#include "NPC/SubNpc/SubNpcProfiling.h"
#include "ProfilingDebugging/MiscTrace.h"

// ---------------------------------------------------------------------------
// 측정용 콘솔 변수
//
// Director는 월드 서브시스템이라 디테일 패널이 없다. 그래서 LOD나 회피를
// 에디터에서 껐다 켤 수가 없고, 성능 비교를 하려면 값을 고쳐 다시 빌드해야 했다.
// 재생 중에 한 줄로 토글할 수 있게 콘솔 변수로 뺀다.
//
//   crowd.LOD 0          거리 LOD를 끈다 (전원 최고 상세도) - LOD가 벌어주는 양 측정
//   crowd.Avoidance 0    소프트 회피를 끈다               - 회피 비용 측정
//   crowd.Stats 1        1초마다 활성/풀 인원을 로그로 찍는다
//
// 되돌릴 때는 값을 1로 주면 된다. ECVF_Cheat이라 배포 빌드에는 안 들어간다.
// ---------------------------------------------------------------------------
DEFINE_LOG_CATEGORY(LogSubNpc);

/** SubNpcLog.h의 SUBNPC_LOG 스위치 */
int32 GSubNpcCrowdLog = 0;
static FAutoConsoleVariableRef CVarSubNpcCrowdLog(
	TEXT("crowd.Log"),
	GSubNpcCrowdLog,
	TEXT("1이면 행인(군중) 로그를 남긴다: 등장·회수, 경로 설정 경고, 겹침·속도·높이 요약 등. 기본은 꺼서 출력 로그를 깨끗하게 둔다."),
	ECVF_Cheat);

namespace SubNpcCrowdCVars
{
	static int32 EnableLod = 1;
	static FAutoConsoleVariableRef CVarEnableLod(
		TEXT("crowd.LOD"),
		EnableLod,
		TEXT("서브 NPC 거리 LOD. 0이면 전원을 Near로 고정한다 (측정용)."),
		ECVF_Cheat);

	static int32 EnableAvoidance = 1;
	static FAutoConsoleVariableRef CVarEnableAvoidance(
		TEXT("crowd.Avoidance"),
		EnableAvoidance,
		TEXT("서브 NPC 소프트 회피. 0이면 끈다 (측정용)."),
		ECVF_Cheat);

	static int32 AvoidPairsOverride = -1;
	static FAutoConsoleVariableRef CVarAvoidPairs(
		TEXT("crowd.AvoidPairs"),
		AvoidPairsOverride,
		TEXT("회피 계산에 참여시킬 최대 인원. -1이면 코드 기본값."),
		ECVF_Cheat);

	static float SpawnSpacingOverride = -1.f;
	static FAutoConsoleVariableRef CVarSpawnSpacing(
		TEXT("crowd.SpawnSpacing"),
		SpawnSpacingOverride,
		TEXT("등장 간격(cm) 덮어쓰기. -1이면 코드 기본값, 0이면 간격 없이 등장."),
		ECVF_Cheat);

	static int32 EnableMeshCollision = 0;
	static FAutoConsoleVariableRef CVarMeshCollision(
		TEXT("crowd.MeshCollision"),
		EnableMeshCollision,
		TEXT("서브 NPC 메시의 물리 콜리전. 1이면 켠다 (UpdateKinematicBonesToAnim 비용 비교용)."),
		ECVF_Cheat);

	/**
	 * LOD 밴드 경계의 히스테리시스 폭 (cm).
	 *
	 * 경계에 걸친 개체는 조금만 움직여도 밴드가 오락가락하는데, 밴드가 바뀌면
	 * 강제 LOD·그림자·애니메이션 주기가 전부 다시 적용되어 그 자체로 깜빡여 보인다.
	 * 이 값만큼 "지금 밴드에 머무는 쪽"으로 경계를 밀어서 진동을 없앤다.
	 *
	 * [주의] 클래스 멤버가 아니라 파일 스코프 상수다.
	 * USubNpcDirectorSubsystem에 UPROPERTY를 추가하면 그 뒤의 ActiveNpcs / PooledNpcs
	 * 오프셋이 밀린다. 빌드가 조금이라도 섞이면 배열 포인터를 엉뚱한 자리에서 읽어
	 * 해제 시점에 접근 위반으로 죽는다. 클래스 크기를 건드리지 않는 게 안전하다.
	 */
	constexpr float LodHysteresis = 500.f;

	/** 추월(옆걸음) 요청 유지 시간 (초). 회피 갱신 주기보다 길어야 끊기지 않는다 */
	constexpr float PassHoldTime = 0.4f;

	/** 속도 회피: 이 거리 안의 사람만 본다 (cm) */
	constexpr float AvoidScanRadius = 400.f;
	/** 속도 회피: 이 시간 안에 부딪히는 후보에 벌점 (초) */
	constexpr float AvoidHorizon = 1.6f;
	/** 속도 회피: 몸 반경 합에 더할 여유 (cm). 플레이어는 조금 더 넉넉히 */
	constexpr float AvoidMargin = 8.f;
	constexpr float AvoidPlayerMargin = 20.f;
	constexpr float AvoidSpotMargin = 25.f;
	/** 서 있을 때 옆걸음 속도 (cm/s). 이동 코드의 SidestepSpeed와 같다 */
	constexpr float AvoidSidestepSpeed = 70.f;
	/** 이만큼 막혀 있으면 길 폭을 넘어서라도 돌아간다 (초, cm). 넘는 폭은 이동 코드의 PassLaneOverflow 이하 */
	constexpr float AvoidStallRelaxTime = 1.5f;
	constexpr float AvoidStallOverflow = 45.f;
	/** 한번 받은 길 밖 허용을 유지하는 시간 (초). 비켜 가는 동안 계속 갱신된다 */
	constexpr float AvoidOverflowHold = 2.f;
	/** 비키기 시작한 방향을 유지하는 시간 (초). 같은 쪽으로 계속 비키면 계속 갱신된다 */
	constexpr float AvoidSideHold = 1.5f;
	/** 몸이 닿아 있을 때 이 속도(cm/s)보다 빨리 파고드는 후보만 충돌로 본다. 옆으로 돌아 나가는 걸음은 허용 */
	constexpr float AvoidContactClosingSpeed = 15.f;

	/**
	 * 1이면 PIE·게임 중에도 행인 경로를 그린다. 경로 스플라인과 방향 화살표는 에디터 전용이라
	 * 게임 중에는 숨겨져서, 행인이 왜 그쪽으로 걷는지·길 폭이 어디까지인지 볼 수 없었다.
	 *   흰색 = 경로 중심선, 초록 = 걸을 수 있는 폭(LaneHalfWidth), 회색 = 추월할 때 비집을 수 있는 최대 폭
	 *   하늘 화살표 = 진행 방향, 빨간 X = 퇴장구(여기서 사라진다)
	 */
	static int32 DrawPaths = 0;
	static FAutoConsoleVariableRef CVarDrawPaths(
		TEXT("crowd.DrawPaths"),
		DrawPaths,
		TEXT("PIE·게임 중에도 행인 경로를 그린다. 흰색=중심선, 초록=걷는 폭, 회색=추월 최대 폭, 하늘=방향, 빨간 X=퇴장구."),
		ECVF_Cheat);

	/** 1이면 2초마다 정체 요약(감속 이유별 인원, 가장 많이 막는 상대)을 로그로 남긴다. 튜닝할 때만 켠다 */
	static int32 LogBlock = 0;
	static FAutoConsoleVariableRef CVarLogBlock(
		TEXT("crowd.LogBlock"),
		LogBlock,
		TEXT("2초마다 정체 요약을 로그로 남긴다 (감속 이유별 인원, 가장 많이 막는 상대). 0이면 끈다."),
		ECVF_Cheat);

	static int32 DebugBlock = 0;
	static FAutoConsoleVariableRef CVarDebugBlock(
		TEXT("crowd.DebugBlock"),
		DebugBlock,
		TEXT("행인이 누구 때문에 느려졌는지 선으로 보여준다. 빨강=정지, 노랑=감속·비킴. 선 끝이 플레이어면 플레이어가 막고 있는 것."),
		ECVF_Cheat);

	/** 두 사람이 나란히 설 수 있는 최소 간격 (cm). 캡슐 지름(약 68cm)보다 약간 크다. 같은 경로 밀어내기의 범위 */
	constexpr float LaneClearance = 75.f;

	/** 비켜 갈 때 길 가장자리(LaneHalfWidth)를 넘어가도 되는 거리 (cm). crowd.DrawPaths의 회색 선 */
	constexpr float SqueezeOverflow = 45.f;

	/** 길 밖에서 움직이는 사람이 이 거리(cm) 안, 앞쪽에 움직이는 사람이 있으면 잠깐 양보한다 */
	constexpr float OffLaneYieldDistance = 85.f;

	/** "앞쪽"으로 보는 각도 (진행 방향과의 내적). 0.4면 약 66도 안쪽 */
	constexpr float OffLaneYieldFrontDot = 0.4f;

	/** 두 사람 진행 방향의 내적이 이보다 크면 같은 쪽으로 따라 걷는 것으로 보고 양보하지 않는다 (0.5 = 60도 안쪽) */
	constexpr float OffLaneFollowDot = 0.5f;

	/** 일행 대화를 굴려보는 간격 (초). 리더마다 이 사이에서 랜덤 */
	// [튜닝] 대화가 잘 안 보인다는 피드백으로 크게 늘렸다. 걸으면서 하므로 길을 막지 않는다.
	constexpr float GroupChatIntervalMin = 2.f;
	constexpr float GroupChatIntervalMax = 5.f;

	/** 굴렸을 때 실제로 이야기를 시작할 확률 */
	constexpr float GroupChatChance = 1.f;

	/** 이야기하는 시간 (초) */
	constexpr float GroupChatDurationMin = 6.f;
	constexpr float GroupChatDurationMax = 10.f;

	/** 리더에게서 이 거리(cm) 안에 있는 일행만 대화에 낀다. 떨어져 있는 사람까지 멈추면 어색하다 */
	// [튜닝] 300 → 450. 5명 무리의 맨 뒤 일행(리더 뒤 240~300cm)까지 끼게 한다.
	constexpr float GroupChatRadius = 450.f;

	/**
	 * 일행 동작 애니메이션 (NPC 블루프린트 AnimLibrary 키). 상체 슬롯에만 얹으므로
	 * 전신 동작이라도 걸으면서 팔·몸통만 움직인다. 무리 크기마다 다르게 해서 멀리서도 구별되게 한다.
	 *   2명: 번갈아 말하기(Talk) ↔ 듣기(Listen)
	 *   3명: 말하는 차례에 손을 뻗어 설명(Point) ↔ 듣기
	 *   5명: 앞 3명은 말하기 ↔ 듣기, 뒤 2명은 계속 휴대폰(Phone)
	 * 키가 AnimLibrary에 없으면 그 동작만 빠진다 (재생 실패는 무시된다).
	 */
	static const FName GroupTalkAnimId(TEXT("group_Talk"));
	static const FName GroupListenAnimId(TEXT("group_Listen"));
	static const FName GroupPointAnimId(TEXT("group_Point"));
	static const FName GroupPhoneAnimId(TEXT("group_Phone"));

	/** 5명 이상 무리에서 계속 휴대폰을 보는 사람 수 (뒤쪽 일행부터) */
	constexpr int32 GroupPhoneMembers = 2;

	/** 한 사람이 말하는(손짓하는) 시간 (초). 이만큼씩 번갈아 말한다 */
	constexpr float GroupChatTurnLength = 2.5f;

	/** 일행이 리더 옆에 서는 거리 (cm, 리더 차선 기준 좌우). 캡슐 지름(약 68cm)보다 작으면 어깨가 겹친다 */
	// [튜닝] 45~55 → 72~80. 캡슐 지름보다 작아서 일행끼리 어깨가 파묻혀 보였다.
	constexpr float GroupSideMin = 72.f;
	constexpr float GroupSideMax = 80.f;

	/** 첫 번째 일행의 앞뒤 간격 (cm). 0이면 리더와 완전히 나란히 걷는다 */
	constexpr float GroupAbreastGapMax = 30.f;

	/** 두 번째 이후 일행의 앞뒤 간격 (cm, 한 사람당) */
	// [튜닝] 50~80 → 80~100. 같은 쪽 앞사람과 앞뒤로 겹치지 않게 캡슐 지름보다 크게 둔다.
	constexpr float GroupBehindGapMin = 80.f;
	constexpr float GroupBehindGapMax = 100.f;

	/** 일행이 대열 간격보다 이만큼(cm) 더 뒤처지면 리더가 멈춰서 기다린다 */
	constexpr float GroupWaitStartLag = 300.f;

	/** 이만큼(cm) 안으로 따라오면 다시 걷는다. 시작 기준보다 작아야 서다 가다를 반복하지 않는다 */
	constexpr float GroupWaitResumeLag = 100.f;

	/** 이보다 오래(초) 기다렸는데도 안 오면 포기하고 걷는다 (일행이 스팟에 오래 머무는 경우 등) */
	constexpr float GroupWaitMaxTime = 8.f;

	/** 포기한 뒤 이만큼(초)은 다시 기다리지 않는다 */
	constexpr float GroupWaitCooldown = 6.f;

	/**
	 * 기다리는 동안 리더의 걸음 배율. 0이면 길 한가운데 서서 정체를 만든다.
	 * [튜닝] 0.2 → 0.6. 너무 느리면 뒤 인파가 리더 뒤로 몰려 정체가 번졌다. 늦춰 걷는 정도로만 기다린다.
	 */
	constexpr float GroupWaitWalkScale = 0.6f;

	/**
	 * 캡슐 반지름 합에 이만큼(cm) 더한 거리 안이면 "몸이 닿았다"로 본다.
	 * 회피 갱신이 0.1초 간격이라, 딱 맞닿는 순간을 놓치지 않게 약간 여유를 둔다.
	 */
	constexpr float LightBumpContactSlack = 12.f;

	static int32 TraceSpawn = 0;
	static FAutoConsoleVariableRef CVarTraceSpawn(
		TEXT("crowd.TraceSpawn"),
		TraceSpawn,
		TEXT("보충이 실패할 때 이유를 로그로 찍는다. 회수 후 재등장이 안 될 때 쓴다."),
		ECVF_Cheat);

	static int32 DebugDraw = 0;
	static FAutoConsoleVariableRef CVarDebugDraw(
		TEXT("crowd.Debug"),
		DebugDraw,
		TEXT("활성 서브 NPC 위에 디버그 표식을 그린다. 메시가 보이든 안 보이든 항상 그린다.\n")
		TEXT("표식은 있는데 사람이 안 보이면 = 렌더링 문제.\n")
		TEXT("표식까지 같이 사라지면 = 풀로 회수된 것(게임플레이 문제)."),
		ECVF_Cheat);

	static int32 PrintStats = 0;
	static FAutoConsoleVariableRef CVarPrintStats(
		TEXT("crowd.Stats"),
		PrintStats,
		TEXT("1이면 1초마다 활성/풀 인원을 로그로 찍는다."),
		ECVF_Cheat);
}

/**
 * 몸 겹침 금지 (최종 안전장치).
 *
 * 속도 회피는 0.1초마다 "감속·비켜 가기"를 정할 뿐이고, 일행 대열은 일부러 캡슐 지름(약 68cm)보다
 * 가까이 붙는 걸 허용한다. 오르막처럼 다 같이 느려지는 곳이나 갱신 사이에는 몸이 닿을 수 있다.
 *
 * 여기서는 이유를 따지지 않는다. 두 몸이 닿을 거리면 경로상 뒤에 있는 쪽을 세운다.
 * 옆으로 비켜서는 것(추월 차선·밀어내기)은 그대로 허용하므로 비킬 자리가 있으면 비켜서 빠진다.
 * 감속 요청은 최솟값을 따르므로 다른 어떤 요청(스치며 통과 등)보다 이 정지가 우선한다.
 */
/**
 * 경로 이음매에서 "누가 앞인가" 판정.
 *
 * Upstream이 Downstream 쪽으로 갈아타기 직전이고 Downstream은 막 갈아탄 참이면 true.
 * [버그 수정] 예전에는 "Upstream의 경로 출구가 Downstream의 경로인가"만 봤다. 가는 길·오는 길이 서로를
 * 출구로 갖는 순환 경로(테스트 레벨의 왕복로)에서는 양쪽 다 상대를 뒤로 봐서, 반환점에서 서로 양보하다
 * 55명이 한 덩어리로 겹쳐 갇혔다. 실제 이음매 근처(끝·시작에서 Window 안)에 있을 때만 앞뒤를 정한다.
 */
namespace SubNpcRoute
{
	constexpr float SeamWindow = 600.f;

	static float RemainingOnPath(const ASubNpcCharacter* Npc)
	{
		const ANpcCrowdPath* Path = Npc ? Npc->GetCurrentPath() : nullptr;
		if (!Path)
		{
			return TNumericLimits<float>::Max();
		}
		return Npc->IsReversedOnPath() ? Npc->GetPathDistance() : Path->GetPathLength() - Npc->GetPathDistance();
	}

	static float TravelledOnPath(const ASubNpcCharacter* Npc)
	{
		const ANpcCrowdPath* Path = Npc ? Npc->GetCurrentPath() : nullptr;
		if (!Path)
		{
			return TNumericLimits<float>::Max();
		}
		return Npc->IsReversedOnPath() ? Path->GetPathLength() - Npc->GetPathDistance() : Npc->GetPathDistance();
	}

	static bool FeedsInto(const ANpcCrowdPath* From, const ANpcCrowdPath* To)
	{
		if (!From || !To)
		{
			return false;
		}
		for (const FNpcPathExit& Exit : From->GetExits())
		{
			if (Exit.NextPath == To)
			{
				return true;
			}
		}
		return false;
	}

	/** Upstream이 이음매 바로 앞(경로 끝 근처)이고 그 경로가 Downstream의 경로로 이어지는가 */
	static bool IsUpstreamAtSeam(const ASubNpcCharacter* Upstream, const ANpcCrowdPath* DownstreamPath)
	{
		return Upstream && FeedsInto(Upstream->GetCurrentPath(), DownstreamPath) && RemainingOnPath(Upstream) < SeamWindow;
	}

	static bool IsSeamOrder(const ASubNpcCharacter* Upstream, const ASubNpcCharacter* Downstream)
	{
		return Downstream && IsUpstreamAtSeam(Upstream, Downstream->GetCurrentPath()) && TravelledOnPath(Downstream) < SeamWindow;
	}
}

/**
 * 행인 액터 ↔ Mass 엔티티 연결 (Mass 1단계).
 *
 * 행인마다 엔티티 하나를 붙인다. 엔티티는 액터를 가리키기만 하고(FSubNpcAgentFragment),
 * 이동 계산은 UMassSubNpcPathFollowProcessor가 맡는다. 풀에 들어간 행인도 엔티티는 그대로 둔다 -
 * 프로세서가 걷는 중인 행인만 고르므로 비용이 없고, 풀에서 꺼낼 때마다 만들었다 지우는 것보다 싸다.
 */
namespace SubNpcMassBinding
{
	static int32 UseStateTree = 1;
	static FAutoConsoleVariableRef CVarUseStateTree(
		TEXT("crowd.StateTree"),
		UseStateTree,
		TEXT("1이면 스포너에 지정된 Mass StateTree로 행인 상태를 정한다. 0이면 액터 코드가 정한다 (게임 시작 전에 설정, 비교용)."),
		ECVF_Cheat);

	/** 스포너에 지정된 행인 StateTree. 월드마다 한 번만 찾는다 (없으면 nullptr - 예전처럼 액터가 스스로 정한다) */
	static UStateTree* FindCrowdStateTree(UWorld* World)
	{
		// 명령줄 -NoCrowdStateTree: 콘솔 변수는 행인이 Mass에 등록된 뒤에야 적용될 수 있어 비교 측정에는 이걸 쓴다
		if (UseStateTree == 0 || FParse::Param(FCommandLine::Get(), TEXT("NoCrowdStateTree")))
		{
			return nullptr;
		}
		static TWeakObjectPtr<UWorld> CachedWorld;
		static TWeakObjectPtr<UStateTree> CachedTree;
		if (CachedWorld.Get() != World)
		{
			CachedWorld = World;
			CachedTree = nullptr;
			for (TActorIterator<ANpcCrowdSpawner> It(World); It; ++It)
			{
				CachedTree = It->LoadCrowdStateTree();
				break;
			}
			SUBNPC_LOG(Log, TEXT("[Crowd] 행인 상태 결정: %s"),
				CachedTree.IsValid() ? *FString::Printf(TEXT("Mass StateTree (%s)"), *GetNameSafe(CachedTree.Get())) : TEXT("액터 코드 (StateTree 없음)"));
		}
		return CachedTree.Get();
	}

	static void Bind(UWorld* World, ASubNpcCharacter* Npc)
	{
		if (!World || !IsValid(Npc) || Npc->GetMassEntity().IsSet())
		{
			return;
		}
		UMassEntitySubsystem* MassSubsystem = World->GetSubsystem<UMassEntitySubsystem>();
		if (!MassSubsystem)
		{
			return;
		}
		FMassEntityManager& EntityManager = MassSubsystem->GetMutableEntityManager();
		if (EntityManager.IsProcessing())
		{
			return;   // Mass가 도는 중에는 엔티티를 만들 수 없다. 다음 Director 틱에 다시 본다.
		}

		FSubNpcAgentFragment Agent;
		Agent.Actor = Npc;

		TArray<FInstancedStruct, TInlineAllocator<4>> Fragments;
		Fragments.Add(FInstancedStruct::Make(Agent));
		Fragments.Add(FInstancedStruct::Make(FSubNpcRepresentationFragment()));

		// [Mass 3단계] 스포너에 StateTree가 지정돼 있으면 상태 결정을 StateTree에 맡긴다.
		// 엔진의 UMassStateTreeActivationProcessor가 이 프래그먼트를 보고 트리를 시작한다.
		FMassArchetypeSharedFragmentValues Shared;
		UStateTree* StateTree = FindCrowdStateTree(World);
		if (StateTree)
		{
			Fragments.Add(FInstancedStruct::Make(FMassStateTreeInstanceFragment()));
			Fragments.Add(FInstancedStruct::Make(FSubNpcBehaviorFragment()));

			FMassStateTreeSharedFragment TreeFragment;
			TreeFragment.StateTree = StateTree;
			Shared.Add(EntityManager.GetOrCreateConstSharedFragment(TreeFragment));
			Shared.Sort();
		}

		// CreateEntity(프래그먼트 목록, 공유 값)은 아키타입 구성에 공유 프래그먼트를 넣지 않아서
		// 엔진의 StateTree 활성화 쿼리에 걸리지 않는다. 아키타입을 만들고 공유 값과 함께 엔티티를 만든 뒤 값을 채운다.
		TArray<const UScriptStruct*, TInlineAllocator<4>> FragmentTypes;
		for (const FInstancedStruct& Fragment : Fragments)
		{
			FragmentTypes.Add(Fragment.GetScriptStruct());
		}
		const FMassArchetypeHandle Archetype = EntityManager.CreateArchetype(FragmentTypes);
		const FMassEntityHandle Entity = EntityManager.CreateEntity(Archetype, Shared);
		EntityManager.SetEntityFragmentValues(Entity, Fragments);
		Npc->SetMassEntity(Entity);
		Npc->SetSpotDecisionByStateTree(StateTree != nullptr);
	}

	static void Unbind(UWorld* World, ASubNpcCharacter* Npc)
	{
		if (!Npc || !Npc->GetMassEntity().IsSet())
		{
			return;
		}
		const FMassEntityHandle Entity = Npc->GetMassEntity();
		Npc->SetMassEntity(FMassEntityHandle());

		// 월드가 정리되는 중이면 Mass도 같이 정리된다. 손대지 않는다.
		if (!World || World->bIsTearingDown)
		{
			return;
		}
		if (UMassEntitySubsystem* MassSubsystem = World->GetSubsystem<UMassEntitySubsystem>())
		{
			FMassEntityManager& EntityManager = MassSubsystem->GetMutableEntityManager();
			if (EntityManager.IsEntityValid(Entity))
			{
				if (EntityManager.IsProcessing())
				{
					EntityManager.Defer().DestroyEntity(Entity);
				}
				else
				{
					EntityManager.DestroyEntity(Entity);
				}
			}
		}
	}
}

/**
 * 겹침 검사 (crowd.CheckOverlap).
 *
 * 몸 겹침 금지가 제대로 동작하는지 눈 대신 숫자로 확인한다. 인원을 0 → 100으로 늘리면서
 * 가까운(Near/Mid) 행인·플레이어끼리 캡슐이 실제로 파고든 쌍을 세고, 2초마다 로그로 남긴다.
 * 걷는 사람만이 아니라 스팟에 서 있는 사람·반응 중인 사람도 센다 - 어디서 겹치든 화면에서는 똑같이 보인다.
 *
 *   [CrowdOverlap] 활성 60명 | 2초 중 12% 프레임에서 겹침, 최대 동시 2쌍 | 가장 깊은 겹침 18cm: A[...] <-> B[...]
 *
 * crowd.DebugOverlap 1이면 겹친 쌍을 빨간 선으로 그린다.
 */
namespace SubNpcOverlapCheck
{
	static int32 Enabled = 1;
	static FAutoConsoleVariableRef CVarCheckOverlap(
		TEXT("crowd.CheckOverlap"),
		Enabled,
		TEXT("행인끼리(플레이어 포함) 몸이 겹치는지 매 프레임 검사해서 2초마다 [CrowdOverlap] 로그로 남긴다. 0이면 끈다."),
		ECVF_Cheat);

	static int32 DebugDraw = 0;
	static FAutoConsoleVariableRef CVarDebugOverlap(
		TEXT("crowd.DebugOverlap"),
		DebugDraw,
		TEXT("겹친 쌍을 빨간 선으로 그린다."),
		ECVF_Cheat);

	/** 이만큼(cm) 이상 파고들어야 겹침으로 센다. 프레임 사이 오차로 살짝 닿는 건 보이지 않는다 */
	constexpr float Tolerance = 3.f;

	constexpr float LogInterval = 2.f;

	struct FWindow
	{
		float StartTime = -1.f;
		int32 Frames = 0;
		int32 OverlapFrames = 0;
		int32 MaxPairs = 0;
		float WorstDepth = 0.f;
		FString WorstDesc;
		int32 FastSamples = 0;
		float WorstSpeedRatio = 0.f;
		FString WorstSpeedDesc;
	};
	static FWindow Window;

	/** crowd.Bench 단계 요약용 (측정 구간 동안 누적) */
	struct FTotals
	{
		int32 Frames = 0;
		int32 OverlapFrames = 0;
		int32 MaxPairs = 0;
		float WorstDepth = 0.f;
	};
	static FTotals Totals;

	static FString Describe(const ASubNpcCharacter* Npc)
	{
		if (!Npc)
		{
			return TEXT("플레이어");
		}
		const TCHAR* State =
			Npc->IsRejoiningPath() ? TEXT("길로합류중") :
			Npc->IsWalkingOnPath() ? TEXT("걷기") :
			Npc->IsMovingOffLane() ? TEXT("스팟·줄로이동") :
			Npc->GetActivity() == ESubNpcActivity::AtSpot ? TEXT("스팟") :
			Npc->GetActivity() == ESubNpcActivity::Reacting ? TEXT("반응") : TEXT("기타");
		return FString::Printf(TEXT("%s[%s %s%s 차선%.0f 속도%.0f%%%s]"),
			*GetNameSafe(Npc), State, *GetNameSafe(Npc->GetCurrentPath()).Right(10),
			Npc->IsReversedOnPath() ? TEXT("/역") : TEXT(""), Npc->GetLaneOffset(),
			Npc->GetCurrentMoveSpeed() / FMath::Max(1.f, Npc->GetWalkSpeed()) * 100.f,
			Npc->GetGroupLeader() ? TEXT("/일행") : TEXT(""));
	}

	static void Run(const UWorld* World, const TArray<TObjectPtr<ASubNpcCharacter>>& Npcs)
	{
		if (Enabled == 0 || !World)
		{
			return;
		}
		const float Now = World->GetTimeSeconds();
		if (Window.StartTime < 0.f || Now < Window.StartTime)
		{
			Window = FWindow();
			Window.StartTime = Now;
		}

		struct FBody
		{
			const ASubNpcCharacter* Npc = nullptr;
			FVector Loc = FVector::ZeroVector;
			float Radius = 0.f;
			float ActualSpeed = 0.f;   // 지난 프레임부터 실제로 움직인 속도 (cm/s). 요청 속도와 다르면 옆걸음·끌림이다
		};
		TArray<FBody, TInlineAllocator<128>> Bodies;
		for (const ASubNpcCharacter* Npc : Npcs)
		{
			if (!IsValid(Npc) || Npc->IsPooled() || Npc->IsHidden())
			{
				continue;
			}
			const ESubNpcLodBand Band = Npc->GetLodBand();
			if (Band != ESubNpcLodBand::Near && Band != ESubNpcLodBand::Mid)
			{
				continue;
			}
			Bodies.Add({ Npc, Npc->GetActorLocation(), Npc->GetSimpleCollisionRadius() });
		}
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APawn* Player = PC->GetPawn())
			{
				Bodies.Add({ nullptr, Player->GetActorLocation(), Player->GetSimpleCollisionRadius() });
			}
		}

		// 실제 이동 속도 (겹침 원인 진단용). 중간 거리는 0.1초마다 몰아서 움직이므로 그 간격으로 잰다.
		{
			static TMap<TWeakObjectPtr<const ASubNpcCharacter>, TPair<FVector, float>> LastSample;
			for (FBody& Body : Bodies)
			{
				if (!Body.Npc)
				{
					continue;
				}
				TPair<FVector, float>& Sample = LastSample.FindOrAdd(Body.Npc, TPair<FVector, float>(Body.Loc, Now));
				const float Elapsed = Now - Sample.Value;
				const float Moved = FVector::Dist2D(Sample.Key, Body.Loc);
				if (Elapsed > 0.f && Moved < 300.f)
				{
					Body.ActualSpeed = Moved / Elapsed;
				}
				if (Elapsed >= 0.1f || Moved >= 300.f)
				{
					Sample = TPair<FVector, float>(Body.Loc, Now);
				}
			}
		}

		// 속도 검사: 걸음 속도보다 눈에 띄게 빨리 움직인 행인 (이음매에서 끌려가듯 빨라지는 문제 확인용).
		// 등장·순간이동처럼 한 프레임에 크게 튄 것은 빼고 본다.
		{
			static TMap<TWeakObjectPtr<const ASubNpcCharacter>, FVector> LastLoc;
			static float LastTime = -1.f;
			const float Dt = (LastTime >= 0.f && Now > LastTime) ? Now - LastTime : 0.f;
			for (const FBody& Body : Bodies)
			{
				// 매 프레임 움직이는 가까운(Near) 사람만 본다. 중간 거리는 0.1초에 한 번 움직여서 프레임 속도로 재면 튀어 보인다.
				if (!Body.Npc || !Body.Npc->IsWalkingOnPath() || Body.Npc->GetLodBand() != ESubNpcLodBand::Near)
				{
					LastLoc.Remove(Body.Npc);
					continue;
				}
				if (const FVector* Prev = LastLoc.Find(Body.Npc); Prev && Dt > 0.f)
				{
					const float Speed = FVector::Dist2D(*Prev, Body.Loc) / Dt;
					const float Walk = FMath::Max(1.f, Body.Npc->GetWalkSpeed());
					if (Speed > Walk * 1.3f && Speed < Walk * 5.f && Speed / Walk > Window.WorstSpeedRatio)
					{
						Window.WorstSpeedRatio = Speed / Walk;
						Window.WorstSpeedDesc = FString::Printf(TEXT("%s %.0fcm/s (걸음 %.0f) @(%.0f,%.0f)"),
							*Describe(Body.Npc), Speed, Walk, Body.Loc.X, Body.Loc.Y);
					}
					if (Speed > Walk * 1.3f && Speed < Walk * 5.f)
					{
						++Window.FastSamples;
					}
				}
				LastLoc.Add(Body.Npc, Body.Loc);
			}
			LastTime = Now;
		}

		int32 Pairs = 0;
		for (int32 i = 0; i < Bodies.Num(); ++i)
		{
			for (int32 j = i + 1; j < Bodies.Num(); ++j)
			{
				const FBody& A = Bodies[i];
				const FBody& B = Bodies[j];
				if (FMath::Abs(A.Loc.Z - B.Loc.Z) > 150.f)
				{
					continue;   // 경사로 위·아래, 다른 층
				}
				const float Depth = A.Radius + B.Radius - FVector::Dist2D(A.Loc, B.Loc);
				if (Depth < Tolerance)
				{
					continue;
				}
				++Pairs;
				if (Depth > Window.WorstDepth)
				{
					Window.WorstDepth = Depth;
					// 진단: 거리 단계, 실제 속도, 경로상 남은/지난 거리, 상대 쪽을 향하는 정도(1=정면)
					auto Detail = [](const FBody& Me, const FBody& Other)
					{
						if (!Me.Npc)
						{
							return FString::Printf(TEXT("{실제%.0f}"), Me.ActualSpeed);
						}
						const FVector ToOther = FVector(Other.Loc - Me.Loc).GetSafeNormal2D();
						const ESubNpcLodBand Band = Me.Npc->GetLodBand();
						const ANpcCrowdPath* Path = Me.Npc->GetCurrentPath();
						const float OffTarget = Path ? FVector::Dist2D(Me.Loc,
							Path->GetLocationAtDistance(Me.Npc->GetPathDistance(), Me.Npc->GetLaneOffset()) + Me.Npc->GetPathHandoffOffset()) : -1.f;
						const ASubNpcCharacter* Leader = Me.Npc->GetGroupLeader();
						const FString LeaderInfo = Leader
							? FString::Printf(TEXT(" 리더%s(경로%s 진행%.0f 몸거리%.0f)"), *GetNameSafe(Leader).RightChop(13),
								*GetNameSafe(Leader->GetCurrentPath()).Right(4), Leader->GetPathDistance(), FVector::Dist2D(Me.Loc, Leader->GetActorLocation()))
							: FString();
						return FString::Printf(TEXT("{%s 실제%.0f 목표와%.0f 진행%.0f 남은%.0f 지난%.0f 향함%.1f 정체%.1f%s}"),
							Band == ESubNpcLodBand::Near ? TEXT("N") : TEXT("M"), Me.ActualSpeed, OffTarget, Me.Npc->GetPathDistance(),
							FMath::Min(SubNpcRoute::RemainingOnPath(Me.Npc), 99999.f), FMath::Min(SubNpcRoute::TravelledOnPath(Me.Npc), 99999.f),
							FVector::DotProduct(Me.Npc->GetActorForwardVector().GetSafeNormal2D(), ToOther), Me.Npc->GetStalledTime(), *LeaderInfo);
					};
					Window.WorstDesc = FString::Printf(TEXT("%s%s <-> %s%s @(%.0f,%.0f,%.0f)"),
						*Describe(A.Npc), *Detail(A, B), *Describe(B.Npc), *Detail(B, A), A.Loc.X, A.Loc.Y, A.Loc.Z);
				}
				Totals.WorstDepth = FMath::Max(Totals.WorstDepth, Depth);
#if !UE_BUILD_SHIPPING
				if (DebugDraw != 0)
				{
					const FVector Up(0.f, 0.f, 120.f);
					DrawDebugLine(World, A.Loc + Up, B.Loc + Up, FColor::Red, false, 0.f, 0, 4.f);
					DrawDebugSphere(World, (A.Loc + B.Loc) * 0.5f + Up, 15.f, 8, FColor::Red, false, 0.f);
				}
#endif
			}
		}

		++Window.Frames;
		++Totals.Frames;
		if (Pairs > 0)
		{
			++Window.OverlapFrames;
			++Totals.OverlapFrames;
		}
		Window.MaxPairs = FMath::Max(Window.MaxPairs, Pairs);
		Totals.MaxPairs = FMath::Max(Totals.MaxPairs, Pairs);

		if (Now - Window.StartTime >= LogInterval)
		{
			// 높이 검사: 발이 바닥에서 떠 있거나 묻힌 행인 (머리 위로 걷기, 하늘 걷기, 바닥에 꺼짐).
			// 2초에 한 번만 본다 (행인마다 트레이스 한 번).
			int32 BadHeight = 0;
			float WorstGap = 0.f;
			FString WorstHeightDesc;
			for (const FBody& Body : Bodies)
			{
				if (!Body.Npc)
				{
					continue;
				}
				const UCapsuleComponent* Capsule = Body.Npc->GetCapsuleComponent();
				const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.f;
				const FVector FootLoc = Body.Loc - FVector(0.f, 0.f, HalfHeight);

				FCollisionObjectQueryParams Objects;
				Objects.AddObjectTypesToQuery(ECC_WorldStatic);
				Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
				FCollisionQueryParams Params(SCENE_QUERY_STAT(SubNpcHeightCheck), false, Body.Npc);
				TArray<FHitResult> Hits;
				World->LineTraceMultiByObjectType(Hits, FootLoc + FVector(0.f, 0.f, 60.f), FootLoc - FVector(0.f, 0.f, 500.f), Objects, Params);
				for (const FHitResult& Hit : Hits)
				{
					const UPrimitiveComponent* Comp = Hit.GetComponent();
					if (!Comp || Comp->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block || Cast<APawn>(Hit.GetActor()))
					{
						continue;
					}
					const float Gap = FootLoc.Z - Hit.ImpactPoint.Z;   // +면 떠 있음, -면 묻힘
					if (FMath::Abs(Gap) > 25.f)
					{
						++BadHeight;
						if (FMath::Abs(Gap) > FMath::Abs(WorstGap))
						{
							WorstGap = Gap;
							WorstHeightDesc = FString::Printf(TEXT("%s @(%.0f,%.0f,%.0f)"), *Describe(Body.Npc), Body.Loc.X, Body.Loc.Y, Body.Loc.Z);
						}
					}
					break;
				}
			}
			if (Window.FastSamples > 0)
			{
				SUBNPC_LOG(Warning, TEXT("[CrowdSpeed] 걸음 속도의 1.3배를 넘은 순간 %d번 | 가장 빠른 x%.2f: %s"),
					Window.FastSamples, Window.WorstSpeedRatio, *Window.WorstSpeedDesc);
			}

			if (BadHeight > 0)
			{
				SUBNPC_LOG(Warning, TEXT("[CrowdHeight] 발이 바닥에서 25cm 넘게 %s 행인 %d명 | 가장 심한 %+.0fcm: %s"),
					WorstGap > 0.f ? TEXT("뜬") : TEXT("묻힌"), BadHeight, WorstGap, *WorstHeightDesc);
			}

			// 상태별 인원: StateTree를 켜고 끌 때 스팟 들르기·반응이 같은 비율로 일어나는지 비교한다.
			{
				int32 Walking = 0, AtSpot = 0, Reacting = 0;
				for (const ASubNpcCharacter* Npc : Npcs)
				{
					if (!IsValid(Npc))
					{
						continue;
					}
					switch (Npc->GetActivity())
					{
					case ESubNpcActivity::Walking:  ++Walking;  break;
					case ESubNpcActivity::AtSpot:   ++AtSpot;   break;
					case ESubNpcActivity::Reacting: ++Reacting; break;
					default: break;
					}
				}
				SUBNPC_LOG(Log, TEXT("[CrowdActivity] 걷기 %d | 스팟·줄 %d | 반응 %d"), Walking, AtSpot, Reacting);
			}

			if (Window.OverlapFrames > 0)
			{
				SUBNPC_LOG(Warning, TEXT("[CrowdOverlap] 활성 %d명(가까움 %d) | %.1f초 중 %.0f%% 프레임에서 겹침, 최대 동시 %d쌍 | 가장 깊은 겹침 %.0fcm: %s"),
					Npcs.Num(), Bodies.Num(), Now - Window.StartTime,
					100.f * Window.OverlapFrames / FMath::Max(1, Window.Frames), Window.MaxPairs,
					Window.WorstDepth, *Window.WorstDesc);
			}
			else
			{
				SUBNPC_LOG(Log, TEXT("[CrowdOverlap] 활성 %d명(가까움 %d) | 겹침 없음"), Npcs.Num(), Bodies.Num());
			}
			Window = FWindow();
			Window.StartTime = Now;
		}
	}
}

namespace SubNpcBodySeparation
{
	/** 캡슐 반지름 합에 더하는 여유 (cm) */
	constexpr float Margin = 4.f;

	/** 내 진행 방향 기준 이 내적보다 앞에 있으면 "앞사람"이다 (약 72도 안쪽) */
	constexpr float FrontDot = 0.3f;

	/** 같은 길에서 겹쳤을 때 옆으로 떼어내는 세기 (cm, 회피 밀어내기에 더한다) */
	constexpr float AbreastPush = 30.f;

	/** 길 밖 이동하는 사람을 몸이 닿아 세워 두는 최대 시간 (초). 넘으면 스치며 지나가게 둔다 */
	constexpr float MaxOffLaneYieldSeconds = 2.5f;

	/** 겹친 몸을 떼어 내는 최대 속도 (cm/s). 걸음에 더해지므로 크면 달려 보인다 (200에서 걸음의 2배까지 튀었다) */
	constexpr float MaxPushSpeed = 60.f;

	/**
	 * 걸음 + 떼어 내기를 합한 속도 상한 (걸음 속도 대비). 행인 한 걸음 상한(SubNpcCrowdFlow::MaxSpeedRatio)과 같다.
	 * 걷는 중이면 남은 만큼만 민다 - 서 있는 사람은 MaxPushSpeed까지, 제 속도로 걷는 사람은 조금만.
	 */
	constexpr float MaxTotalSpeedRatio = 1.25f;

	/** 걷는 중이어도 이만큼은 민다 (cm/s). 0이면 나란히 걷다 겹친 두 사람이 영영 안 떨어진다 */
	constexpr float MinPushSpeed = 15.f;

	static int32 EnablePush = 1;
	static FAutoConsoleVariableRef CVarPush(TEXT("crowd.PushApart"), EnablePush,
		TEXT("겹친 몸을 떼어 낸다 (겹침 금지 마지막 단계). 0이면 끈다 (비교용)."), ECVF_Cheat);

	static int32 EnableOncoming = 1;
	static FAutoConsoleVariableRef CVarOncoming(TEXT("crowd.OncomingPass"), EnableOncoming,
		TEXT("마주 오는 두 사람은 앞뒤 순서 없이 천천히 걸으며 옆으로 비켜 간다. 0이면 예전 규칙 (비교용)."), ECVF_Cheat);

	/** 진행 방향 내적이 이보다 작으면 마주 오는 사람이다 (약 120도 이상). 직각 모서리는 같은 흐름으로 본다 */
	constexpr float OncomingDot = -0.5f;

	/** 마주 오다 몸이 닿으면 이 속도 배율로 걸으며 옆으로 비켜 간다. 둘 다 세우면 그 자리에 갇힌다 */
	constexpr float OncomingPassScale = 0.3f;

	/** 떼어 낼 때 목표로 하는 틈 (cm). 겹침 검사 허용치(3cm)보다 작게 둔다 */
	constexpr float PushClearance = 1.f;

	static void Enforce(const UWorld* World, const TArray<TObjectPtr<ASubNpcCharacter>>& Npcs, TSet<ASubNpcCharacter*>& OutOffLaneYielders)
	{
		struct FBody
		{
			ASubNpcCharacter* Npc = nullptr;   // nullptr이면 플레이어
			FVector Loc = FVector::ZeroVector;
			FVector Fwd = FVector::ZeroVector;
			float Radius = 0.f;
			bool bCanMove = false;
			bool bOffLane = false;   // 스팟·줄로 가거나 길로 돌아오는 중 (속도 배율이 아니라 양보 플래그로 세운다)
			bool bStander = false;   // 스팟에 서 있는 사람 (길 밖 이동하는 사람만 이 사람을 피한다)
			bool bPushable = false;  // 겹쳤을 때 떼어 낼 수 있다 (길 위를 걷거나 길 밖에서 움직이는 사람)
		};

		TArray<FBody, TInlineAllocator<64>> Bodies;
		for (ASubNpcCharacter* Npc : Npcs)
		{
			const bool bStander = IsValid(Npc) && Npc->GetActivity() == ESubNpcActivity::AtSpot && !Npc->IsMovingOffLane();
			if (!IsValid(Npc) || !(Npc->IsBlockingOnPath() || Npc->IsMovingOffLane() || bStander))
			{
				continue;
			}
			const ESubNpcLodBand Band = Npc->GetLodBand();
			if (Band != ESubNpcLodBand::Near && Band != ESubNpcLodBand::Mid)
			{
				continue;
			}
			FBody& Body = Bodies.AddDefaulted_GetRef();
			Body.Npc = Npc;
			Body.Loc = Npc->GetActorLocation();
			Body.Fwd = Npc->GetActorForwardVector().GetSafeNormal2D();
			Body.Radius = Npc->GetSimpleCollisionRadius();
			Body.bCanMove = Npc->IsWalkingOnPath() && !Npc->IsMovingOffLane();
			Body.bOffLane = Npc->IsMovingOffLane();
			Body.bStander = bStander;
			// 스팟에 선 사람·부딪힘 반응 중인 사람은 제자리를 지킨다 (자리·애니메이션이 정해져 있다).
			Body.bPushable = Body.bCanMove || Body.bOffLane;
		}

		if (const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
		{
			if (const APawn* Player = PC->GetPawn())
			{
				FBody& Body = Bodies.AddDefaulted_GetRef();
				Body.Loc = Player->GetActorLocation();
				Body.Radius = Player->GetSimpleCollisionRadius();
			}
		}

		TArray<FVector, TInlineAllocator<64>> Pushes;
		Pushes.SetNumZeroed(Bodies.Num());

		for (int32 i = 0; i < Bodies.Num(); ++i)
		{
			const FBody& A = Bodies[i];
			for (int32 j = i + 1; j < Bodies.Num(); ++j)
			{
				const FBody& B = Bodies[j];
				const float MinSep = A.Radius + B.Radius + Margin;

				// 높이 차가 크면(경사로 위·아래) 다른 층이다.
				if (FMath::Abs(A.Loc.Z - B.Loc.Z) > 150.f)
				{
					continue;
				}
				const FVector AB = FVector(B.Loc.X - A.Loc.X, B.Loc.Y - A.Loc.Y, 0.f);
				const float DistSq = AB.SizeSquared();
				if (DistSq >= FMath::Square(MinSep) || DistSq < KINDA_SMALL_NUMBER)
				{
					continue;
				}
				const FVector Dir = AB / FMath::Sqrt(DistSq);

				// [마지막 단계] 이미 몸이 겹쳤으면 떼어 낸다. 아래 규칙들은 "누가 설지"만 정해서,
				// 서 있는 사람이 옆으로 밀려 들어오거나(일행 자리 맞추기·밀어내기는 속도와 상관없이 차선을 옮긴다)
				// 0.1초마다 걷는 중간 거리 행인이 한 걸음 넘어선 경우에는 겹친 채로 남았다.
				// 움직일 수 있는 사람끼리는 반씩, 한쪽이 못 움직이면(스팟에 선 사람·플레이어·반응 중) 다른 쪽이 다 비킨다.
				// [마주 오는 사람] 길 위를 서로 반대로 걷는 두 사람은 앞뒤가 없다. 둘 다 세우면 그 자리에 갇히고,
				// 순서를 매기면 한쪽이 다른 쪽 몸을 뚫는다. 천천히 걸으며 서로 옆으로 비켜 간다.
				const bool bOncoming = EnableOncoming != 0
					&& A.Npc && B.Npc && A.bCanMove && B.bCanMove && FVector::DotProduct(A.Fwd, B.Fwd) < OncomingDot;
				FVector PushDir = Dir;
				if (bOncoming)
				{
					const FVector Side = FVector::CrossProduct(FVector::UpVector, A.Fwd).GetSafeNormal2D();
					float SideSign = FMath::Sign(FVector::DotProduct(Dir, Side));
					if (SideSign == 0.f)
					{
						SideSign = (GetTypeHash(A.Npc->GetFName()) < GetTypeHash(B.Npc->GetFName())) ? 1.f : -1.f;
					}
					PushDir = Side * SideSign;   // A에서 B 쪽 옆 방향. A는 반대로, B는 이쪽으로 비킨다
				}

				{
					const float Penetration = A.Radius + B.Radius + PushClearance - FMath::Sqrt(DistSq);
					if (EnablePush != 0 && Penetration > 0.f && (A.bPushable || B.bPushable))
					{
						// [줄 밀림] 길 위 행인과 스팟을 오가는 사람(줄·칸·일행 대기·복귀)이 겹치면 행인이 다 비킨다.
						// 반씩 밀면 줄 서러 가던 사람이 옆으로 밀려 줄 자리를 다시 찾느라 흔들리고 줄이 밀려났다.
						float ShareA = A.bPushable ? (B.bPushable ? 0.5f : 1.f) : 0.f;
						if (A.bPushable && B.bPushable && A.bCanMove != B.bCanMove)
						{
							ShareA = A.bCanMove ? 1.f : 0.f;
						}
						Pushes[i] -= PushDir * (Penetration * ShareA);
						Pushes[j] += PushDir * (Penetration * (1.f - ShareA));
					}
				}

				if (bOncoming)
				{
					A.Npc->RequestSlowDown(OncomingPassScale);
					B.Npc->RequestSlowDown(OncomingPassScale);
					continue;
				}

				// 스팟에 서 있는 사람은 길 밖에서 움직이는 사람만 피한다. 길 위 행인은 속도 회피가 맡는다
				// (서 있는 사람마다 길 위 행인을 세우면 스팟 옆을 지나는 줄이 통째로 선다).
				if ((A.bStander && !B.bOffLane) || (B.bStander && !A.bOffLane))
				{
					continue;
				}

				// [길 밖 이동] 스팟·줄로 가거나 길로 돌아오는 사람은 속도 배율을 쓰지 않고 자기 걸음으로 움직인다.
				// 예전에는 이 사람들이 겹침 금지를 받지 않아서 스팟 근처에서 서로 몸을 뚫었다 (로그: 스팟·줄로이동 ↔ 길로합류중).
				// 상대를 향해 걷고 있으면 그 자리에 서서 기다리게 한다 (SetOffLaneYield). 서로를 향하면 한 명만 선다.
				{
					// 같은 스팟 줄(줄 선 사람·칸으로 들어가는 사람)끼리는 서로 세우지 않는다 - 줄 간격은 줄 쪽 로직이 맞춘다.
					// 예전에는 한 칸씩 당길 때 앞사람이 아직 덜 비켰다고 뒷사람을 세워 줄 전체가 몇 초씩 멈췄다.
					auto SpotOf = [](const ASubNpcCharacter* N) -> const ANpcActivitySpot*
					{
						return !N ? nullptr : (N->GetQueuedSpot() ? N->GetQueuedSpot() : N->GetOccupiedSpot());
					};
					const ANpcActivitySpot* SpotA = SpotOf(A.Npc);
					if (SpotA && SpotA == SpotOf(B.Npc))
					{
						continue;
					}

					const bool bAInto = A.bOffLane && FVector::DotProduct(A.Fwd, Dir) > FrontDot;
					const bool bBInto = B.bOffLane && FVector::DotProduct(B.Fwd, -Dir) > FrontDot;
					if (bAInto && bBInto)
					{
						// 나가는 사람(길로 돌아가는 중)이 먼저 간다 (UpdateOffLaneYield와 같은 규칙)
						const bool bALeaving = A.Npc->IsRejoiningPath();
						const bool bBLeaving = B.Npc->IsRejoiningPath();
						ASubNpcCharacter* Yielder = (bALeaving != bBLeaving)
							? (bALeaving ? B.Npc : A.Npc)
							: ((GetTypeHash(A.Npc->GetFName()) < GetTypeHash(B.Npc->GetFName())) ? A.Npc : B.Npc);
						OutOffLaneYielders.Add(Yielder);
					}
					else if (bAInto || bBInto)
					{
						// 한쪽만 상대를 향해 걷는 경우: 보통은 그쪽이 서지만, 그쪽이 나가는 사람이고 상대가
						// 들어오는 중(길 밖 이동)이면 들어오는 쪽이 선다.
						const FBody& Into = bAInto ? A : B;
						const FBody& Other = bAInto ? B : A;
						const bool bIntoLeaving = Into.Npc->IsRejoiningPath();
						const bool bOtherEntering = Other.Npc && Other.bOffLane && !Other.Npc->IsRejoiningPath();
						OutOffLaneYielders.Add((bIntoLeaving && bOtherEntering) ? Other.Npc : Into.Npc);
					}
					if (A.bOffLane || B.bOffLane || A.bStander || B.bStander)
					{
						// 경로 순서 규칙은 길 위를 걷는 사람끼리만 쓴다. 길 위 행인이 길 밖 이동하는 사람 쪽으로
						// 걷고 있으면 길 위 행인도 선다 (예전과 같다).
						if (A.bCanMove && FVector::DotProduct(A.Fwd, Dir) > FrontDot)
						{
							A.Npc->RequestSlowDown(0.f);
						}
						if (B.bCanMove && FVector::DotProduct(B.Fwd, -Dir) > FrontDot)
						{
							B.Npc->RequestSlowDown(0.f);
						}
						continue;
					}
				}

				// 누가 양보할지는 "경로상 누가 뒤에 있나"로 정한다.
				// [버그 수정] 예전에는 몸이 향한 방향으로 앞사람을 정했다. 굽은 곳에서는 서로 몸이 향한 쪽에
				// 다른 사람이 있어 A→B→C→A 순환으로 다 같이 멈췄고, 영영 풀리지 않았다
				// (로그: C 경로 굽은 곳에서 50명 넘게 10~28초 정지, 이유는 "정체스침"인데 속도 0%).
				// 경로 진행 순서는 순환이 없다 - 맨 앞사람은 절대 멈추지 않으니 줄은 반드시 풀린다.
				int32 Order = 0;   // -1: A가 뒤 (A 양보), +1: B가 뒤 (B 양보), 0: 순서를 정할 수 없음
				if (A.Npc && B.Npc && A.bCanMove && B.bCanMove)
				{
					const ANpcCrowdPath* PathA = A.Npc->GetCurrentPath();
					const ANpcCrowdPath* PathB = B.Npc->GetCurrentPath();
					if (PathA && PathA == PathB && A.Npc->IsReversedOnPath() == B.Npc->IsReversedOnPath())
					{
						const float Along = A.Npc->IsReversedOnPath() ? -1.f : 1.f;
						const float Lead = (B.Npc->GetPathDistance() - A.Npc->GetPathDistance()) * Along;
						Order = (FMath::Abs(Lead) > KINDA_SMALL_NUMBER)
							? (Lead > 0.f ? -1 : 1)
							: ((GetTypeHash(A.Npc->GetFName()) < GetTypeHash(B.Npc->GetFName())) ? -1 : 1);
					}
					// [버그 수정] 이음매 앞뒤는 같은 쪽으로 걷는 사람끼리만 정한다.
					// 반환점(가는 길 끝 → 오는 길 시작)에서는 "다음 경로의 앞사람"이 사실은 마주 오는 사람이다.
					// 순서를 적용하면 도착하는 사람만 서고, 막 돌아선 사람이 순서상 앞이라며 그 몸을 뚫고 지나갔다
					// (로그: 테스트 레벨 반환점에 겹침의 절반, 서 있는 쪽 속도 0% ↔ 돌아선 쪽 90~100%).
					else if (EnableOncoming == 0 || FVector::DotProduct(A.Fwd, B.Fwd) >= OncomingDot)
					{
						if (SubNpcRoute::IsSeamOrder(A.Npc, B.Npc))
						{
							Order = -1;
						}
						else if (SubNpcRoute::IsSeamOrder(B.Npc, A.Npc))
						{
							Order = 1;
						}
					}
				}

				// [모서리] 경로 순서상 앞인 사람이 실제로는 뒤쪽 사람 몸을 향해 걸어가고 있으면 앞사람이 선다.
				// B→C 모서리에서 아직 B에 있는 사람(순서상 뒤)이 C 입구 쪽에 서 있으면, 이미 C로 넘어간 사람이
				// 순서만 믿고 그 몸을 뚫고 지나갔다 (로그: C 행인 속도 96~100% ↔ B 행인 속도 0%, 33~65cm 겹침).
				// 이때는 앞사람이 서고 뒤 사람이 먼저 지나간다. 둘 다 서로를 향하면 원래 순서를 따른다 (둘 다 서면 교착).
				if (Order != 0)
				{
					const FBody& Front = (Order < 0) ? B : A;
					const FBody& Rear = (Order < 0) ? A : B;
					const FVector FrontToRear = (Order < 0) ? -Dir : Dir;
					const bool bFrontWalksIntoRear = FVector::DotProduct(Front.Fwd, FrontToRear) > FrontDot;
					const bool bRearWalksIntoFront = FVector::DotProduct(Rear.Fwd, -FrontToRear) > FrontDot;
					if (bFrontWalksIntoRear && !bRearWalksIntoFront)
					{
						Order = -Order;
					}
				}

				if (Order != 0)
				{
					// 일행이 길 폭에 눌려 나란히 겹쳤을 때도 이 규칙으로 뒤쪽이 물러나 한 줄이 된다.
					(Order < 0 ? A.Npc : B.Npc)->RequestSlowDown(0.f);
				}
				else
				{
					// 순서를 정할 수 없는 상대(플레이어, 마주 오는 사람, 스팟으로 가는 사람)는
					// 그쪽을 향해 걷고 있는 쪽이 선다. 마주 오면 둘 다 서서 옆으로 비켜 간다.
					if (A.bCanMove && FVector::DotProduct(A.Fwd, Dir) > FrontDot)
					{
						A.Npc->RequestSlowDown(0.f);
					}
					if (B.bCanMove && FVector::DotProduct(B.Fwd, -Dir) > FrontDot)
					{
						B.Npc->RequestSlowDown(0.f);
					}
				}

				if (A.Npc && B.Npc && A.Npc->GetCurrentPath() == B.Npc->GetCurrentPath() && A.Npc->GetCurrentPath())
				{
					const float Sign = (A.Npc->GetLaneOffset() <= B.Npc->GetLaneOffset()) ? -1.f : 1.f;
					A.Npc->AddAvoidanceNudge(Sign * AbreastPush);
					B.Npc->AddAvoidanceNudge(-Sign * AbreastPush);
				}
			}
		}

		// 모은 밀어내기를 적용한다. 여러 명과 겹쳤으면 합해서 한 번에, 프레임당 최대 거리까지만.
		const float DeltaSeconds = World ? World->GetDeltaSeconds() : 1.f / 60.f;
		for (int32 i = 0; i < Bodies.Num(); ++i)
		{
			ASubNpcCharacter* Npc = Bodies[i].Npc;
			if (Npc && Bodies[i].bPushable && !Pushes[i].IsNearlyZero())
			{
				// 이미 걷고 있는 만큼은 빼고 민다. [버그 수정] 길 밖에서 걷는 사람(줄·칸·복귀)은 속도 배율이 아니라
				// 걸음 속도 그대로 걷는데 빼지 않아서, 밀리면 걸음의 1.3배 넘게 튀었다 (메인 맵 PathB 앞 복귀 중 11건).
				const float AlreadyMoving = Bodies[i].bCanMove ? Npc->GetCurrentMoveSpeed()
					: (Bodies[i].bOffLane ? Npc->GetWalkSpeed() : 0.f);
				const float Budget = Npc->GetWalkSpeed() * MaxTotalSpeedRatio - AlreadyMoving;
				const float PushSpeed = FMath::Clamp(Budget, MinPushSpeed, MaxPushSpeed);
				Npc->PushApart(Pushes[i].GetClampedToMaxSize2D(PushSpeed * DeltaSeconds));
			}
		}
	}
}

// ---------------------------------------------------------------------------
// 성능 측정 (crowd.Bench)
//
// Mass로 옮기기 전후를 같은 방식으로 비교하려고 만든 자동 측정이다.
// 인원을 단계별로 바꿔 가며, 각 단계마다 안정될 때까지 기다린 뒤 일정 시간 동안
//   - 프레임 시간 (평균 / p95 / p99)
//   - 군중 구간별 시간 (SubNpcProfiling의 구간들)
// 을 모아서 요약 CSV와 로그로 남긴다. 단계마다 CSV 프로파일러 캡처도 따로 남기므로
// 게임/렌더/GPU 스레드 시간은 그 파일에서 본다 (Saved/Profiling/CSV).
//
//   crowd.Bench 0 30 60 100 150          인원별로 차례대로 측정
//   crowd.Bench 60 100 secs=30 warmup=8 label=LodOff
//   crowd.BenchStop                      중단 (인원 설정을 원래대로 되돌린다)
//
// [측정할 때] PIE보다 Standalone(-game)이 정확하다. 카메라를 고정하고, stat 명령은 끈다
// (stat 표시 자체가 비용이라 결과를 흐린다).
// ---------------------------------------------------------------------------

CSV_DEFINE_CATEGORY(Crowd, true);

namespace SubNpcProfiling
{
	bool bRecording = false;
	int64 SectionCycles[static_cast<int32>(ESection::Count)] = {};

	const TCHAR* GetSectionName(ESection Section)
	{
		switch (Section)
		{
		case ESection::PathFollow:     return TEXT("PathFollow");
		case ESection::PathApply:      return TEXT("PathApply");
		case ESection::MassPathFollow: return TEXT("MassPathFollow");
		case ESection::GroundSnap:     return TEXT("GroundSnap");
		case ESection::WorldBlock:     return TEXT("WorldBlock");
		case ESection::Avoidance:      return TEXT("Avoidance");
		case ESection::BodySeparation: return TEXT("BodySeparation");
		case ESection::GroupLogic:     return TEXT("GroupLogic");
		case ESection::LodSlice:       return TEXT("LodSlice");
		default:                       return TEXT("?");
		}
	}
}

/** 측정 중 스포너 목표 인원 덮어쓰기. -1이면 스포너 설정대로 (NpcCrowdSpawner.cpp가 읽는다) */
int32 GSubNpcBenchTargetOverride = -1;

namespace SubNpcBench
{
	using SubNpcProfiling::ESection;
	constexpr int32 NumSections = static_cast<int32>(ESection::Count);

	enum class EPhase : uint8 { Idle, Settling, Recording };

	struct FResult
	{
		int32 Requested = 0;
		float AvgActive = 0.f;
		float AvgNear = 0.f;
		int32 Frames = 0;
		float AvgMs = 0.f;
		float P95Ms = 0.f;
		float P99Ms = 0.f;
		float SectionMs[NumSections] = {};
	};

	struct FState
	{
		EPhase Phase = EPhase::Idle;
		TWeakObjectPtr<UWorld> World;
		TArray<int32> Counts;
		int32 Step = -1;
		float PhaseTime = 0.f;
		float LastApplyTime = 0.f;
		float WarmupSeconds = 5.f;
		float RecordSeconds = 20.f;
		FString Label = TEXT("Base");
		FString Stamp;
		int32 SavedMaxActive = -1;

		/** 측정 동안 끈 프레임 제한 (끝나면 되돌린다) */
		FString SavedMaxFPS;
		FString SavedVSync;
		FString SavedVSyncEditor;
		FString SavedSmoothFrameRate;

		TArray<float> FrameMs;
		double ActiveSum = 0.0;
		double NearSum = 0.0;
		TArray<FResult> Results;
	};
	static FState State;

	static float Percentile(TArray<float> Values, float P)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(FMath::CeilToInt(P * Values.Num()) - 1, 0, Values.Num() - 1);
		return Values[Index];
	}

	/**
	 * 프레임 제한을 끈다. 첫 측정에서 100명일 때 프레임이 정확히 16.67ms(60 FPS)로 나왔다 -
	 * 수직동기·프레임 제한에 걸리면 여유가 얼마나 남았는지 알 수 없다.
	 */
	static void DisableFrameLimit()
	{
		IConsoleManager& Console = IConsoleManager::Get();
		auto Override = [&Console](const TCHAR* Name, FString& OutSaved, const TCHAR* Value)
		{
			if (IConsoleVariable* Var = Console.FindConsoleVariable(Name))
			{
				OutSaved = Var->GetString();
				Var->Set(Value, ECVF_SetByConsole);
			}
		};
		Override(TEXT("t.MaxFPS"), State.SavedMaxFPS, TEXT("0"));
		Override(TEXT("r.VSync"), State.SavedVSync, TEXT("0"));
		// PIE는 에디터 뷰포트에서 돌아서 r.VSync가 아니라 이 값을 따른다 (두 번째 측정도 60 FPS에 묶였다).
		Override(TEXT("r.VSyncEditor"), State.SavedVSyncEditor, TEXT("0"));
		Override(TEXT("r.SmoothFrameRate"), State.SavedSmoothFrameRate, TEXT("0"));
	}

	static void RestoreFrameLimit()
	{
		IConsoleManager& Console = IConsoleManager::Get();
		auto Restore = [&Console](const TCHAR* Name, const FString& Saved)
		{
			IConsoleVariable* Var = Console.FindConsoleVariable(Name);
			if (Var && !Saved.IsEmpty())
			{
				Var->Set(*Saved, ECVF_SetByConsole);
			}
		};
		Restore(TEXT("t.MaxFPS"), State.SavedMaxFPS);
		Restore(TEXT("r.VSync"), State.SavedVSync);
		Restore(TEXT("r.VSyncEditor"), State.SavedVSyncEditor);
		Restore(TEXT("r.SmoothFrameRate"), State.SavedSmoothFrameRate);
	}

	/** 인원을 N명으로 맞춘다. 풀이 모자라면 늘리고, 넘치면 바로 회수하고, 모자라면 바로 채운다 */
	static void ApplyCount(USubNpcDirectorSubsystem* Director, UWorld* World, int32 N)
	{
		GSubNpcBenchTargetOverride = N;
		Director->SetMaxActiveNpcs(N);

		TArray<ASubNpcCharacter*> Active;
		UClass* NpcClass = nullptr;
		for (TActorIterator<ASubNpcCharacter> It(World); It; ++It)
		{
			NpcClass = NpcClass ? NpcClass : It->GetClass();
			if (!It->IsPooled())
			{
				Active.Add(*It);
			}
		}

		const int32 Have = Director->GetActiveCount() + Director->GetPooledCount();
		if (Have < N && NpcClass)
		{
			Director->PrewarmPool(NpcClass, N - Have);
		}

		for (int32 i = N; i < Active.Num(); ++i)
		{
			Director->ReturnToPool(Active[i]);
		}

		// 모자란 인원은 모든 경로에 길이 비율로 고르게 뿌린다.
		// [변경] 예전에는 스포너의 FillImmediately를 불렀는데, 입구 경로(15m) 한 곳에만 넣어서
		// 겹치지 않게 세우는 규칙에 막혀 100명을 요청해도 48명밖에 못 채웠다.
		TArray<ANpcCrowdPath*> Paths;
		float TotalLength = 0.f;
		for (TActorIterator<ANpcCrowdPath> It(World); It; ++It)
		{
			if (It->GetPathLength() >= 200.f)
			{
				Paths.Add(*It);
				TotalLength += It->GetPathLength();
			}
		}
		int32 Deficit = N - Director->GetActiveCount();
		for (int32 Attempt = 0; Deficit > 0 && TotalLength > 0.f && Attempt < Deficit * 4 + 20; ++Attempt)
		{
			float Pick = FMath::FRandRange(0.f, TotalLength);
			ANpcCrowdPath* Path = Paths.Last();
			for (ANpcCrowdPath* Candidate : Paths)
			{
				Pick -= Candidate->GetPathLength();
				if (Pick <= 0.f)
				{
					Path = Candidate;
					break;
				}
			}
			const bool bReversed = Path->IsSpawnReversed();
			if (Director->SpawnOnPath(Path, FMath::FRandRange(0.f, Path->GetPathLength()), bReversed))
			{
				--Deficit;
			}
		}
	}

	static void BeginRecord(int32 N)
	{
		State.FrameMs.Reset();
		State.ActiveSum = 0.0;
		State.NearSum = 0.0;
		FMemory::Memzero(SubNpcProfiling::SectionCycles, sizeof(SubNpcProfiling::SectionCycles));
		SubNpcOverlapCheck::Totals = SubNpcOverlapCheck::FTotals();
		SubNpcProfiling::bRecording = true;

		TRACE_BOOKMARK(TEXT("CrowdBench %s N=%d 시작"), *State.Label, N);
		CSV_EVENT(Crowd, TEXT("Bench N=%d"), N);
#if CSV_PROFILER
		FCsvProfiler::Get()->BeginCapture(-1, FString(),
			FString::Printf(TEXT("CrowdBench_%s_%s_N%03d.csv"), *State.Label, *State.Stamp, N));
#endif
	}

	static void EndRecord(int32 N)
	{
		// 캡처는 여기서 끝낸다. Finish가 다시 끝내지 않게 단계를 먼저 바꿔 둔다.
		State.Phase = EPhase::Settling;
		SubNpcProfiling::bRecording = false;
#if CSV_PROFILER
		FCsvProfiler::Get()->EndCapture();
#endif
		TRACE_BOOKMARK(TEXT("CrowdBench %s N=%d 끝"), *State.Label, N);

		FResult& R = State.Results.AddDefaulted_GetRef();
		R.Requested = N;
		R.Frames = State.FrameMs.Num();
		if (R.Frames > 0)
		{
			float Sum = 0.f;
			for (float Ms : State.FrameMs)
			{
				Sum += Ms;
			}
			R.AvgMs = Sum / R.Frames;
			R.P95Ms = Percentile(State.FrameMs, 0.95f);
			R.P99Ms = Percentile(State.FrameMs, 0.99f);
			R.AvgActive = static_cast<float>(State.ActiveSum / R.Frames);
			R.AvgNear = static_cast<float>(State.NearSum / R.Frames);
			for (int32 s = 0; s < NumSections; ++s)
			{
				R.SectionMs[s] = static_cast<float>(FPlatformTime::ToMilliseconds64(static_cast<uint64>(SubNpcProfiling::SectionCycles[s])) / R.Frames);
			}
		}

		// 에디터 창이 포커스를 잃으면 "백그라운드에서 CPU 덜 쓰기"로 3 FPS(333ms)에 묶인다.
		// 첫 측정의 0명·30명 단계가 이것 때문에 쓸 수 없는 값이 됐다.
		if (R.P95Ms > 250.f)
		{
			UE_LOG(LogSubNpc, Warning, TEXT("[CrowdBench] N=%d 측정 중 프레임이 3 FPS 근처로 묶였습니다 - 창이 포커스를 잃은 것 같습니다. ")
				TEXT("이 단계 값은 버리고, 측정 중에는 게임 창을 클릭해 둔 채 두세요 ")
				TEXT("(또는 에디터 환경설정 > 일반 > 성능 > '백그라운드에 있을 때 CPU 사용량 줄이기'를 끄세요)."), N);
		}

		FString Sections;
		for (int32 s = 0; s < NumSections; ++s)
		{
			Sections += FString::Printf(TEXT(" %s=%.3f"), SubNpcProfiling::GetSectionName(static_cast<ESection>(s)), R.SectionMs[s]);
		}
		UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] %s N=%d (실제 평균 %.0f명, 가까움 %.0f명, %d프레임) | 프레임 평균 %.2fms p95 %.2fms p99 %.2fms (%.0f FPS) | 군중 구간(ms/프레임):%s"),
			*State.Label, N, R.AvgActive, R.AvgNear, R.Frames, R.AvgMs, R.P95Ms, R.P99Ms,
			R.AvgMs > 0.f ? 1000.f / R.AvgMs : 0.f, *Sections);

		const SubNpcOverlapCheck::FTotals& Overlap = SubNpcOverlapCheck::Totals;
		if (SubNpcOverlapCheck::Enabled != 0)
		{
			UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] %s N=%d 겹침: %s (겹친 프레임 %.1f%%, 최대 동시 %d쌍, 가장 깊은 %.0fcm)"),
				*State.Label, N, Overlap.OverlapFrames > 0 ? TEXT("있음") : TEXT("없음"),
				100.f * Overlap.OverlapFrames / FMath::Max(1, Overlap.Frames), Overlap.MaxPairs, Overlap.WorstDepth);
		}
	}

	static void Finish(USubNpcDirectorSubsystem* Director, bool bAborted)
	{
		if (State.Phase == EPhase::Recording)
		{
			SubNpcProfiling::bRecording = false;
#if CSV_PROFILER
			FCsvProfiler::Get()->EndCapture();
#endif
		}

		GSubNpcBenchTargetOverride = -1;
		if (Director && State.SavedMaxActive >= 0)
		{
			Director->SetMaxActiveNpcs(State.SavedMaxActive);
		}

		if (State.Results.Num() > 0)
		{
			FString Csv = TEXT("Label,N,AvgActive,AvgNear,Frames,AvgMs,P95Ms,P99Ms,FPS");
			for (int32 s = 0; s < NumSections; ++s)
			{
				Csv += FString::Printf(TEXT(",%sMs"), SubNpcProfiling::GetSectionName(static_cast<ESection>(s)));
			}
			Csv += TEXT("\n");

			UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] ===== 요약 (%s) ====="), *State.Label);
			UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench]    N | 평균ms |  p95ms |  p99ms |   FPS | 군중합계ms(GT) | 경로이동(GT) | 회피 | 겹침금지"));
			for (const FResult& R : State.Results)
			{
				Csv += FString::Printf(TEXT("%s,%d,%.1f,%.1f,%d,%.3f,%.3f,%.3f,%.1f"),
					*State.Label, R.Requested, R.AvgActive, R.AvgNear, R.Frames, R.AvgMs, R.P95Ms, R.P99Ms,
					R.AvgMs > 0.f ? 1000.f / R.AvgMs : 0.f);
				for (int32 s = 0; s < NumSections; ++s)
				{
					Csv += FString::Printf(TEXT(",%.4f"), R.SectionMs[s]);
				}
				Csv += TEXT("\n");

				// "게임 스레드가 실제로 붙잡힌 시간"으로 합친다.
				// Mass를 쓰면 경로 이동은 MassPathFollow(병렬 계산 + 적용의 벽시계 시간)이고,
				// PathFollow는 워커 스레드들의 CPU 시간 합이라 게임 스레드 비용이 아니다.
				// 액터 틱으로 돌면 PathFollow(+ PathApply)가 게임 스레드 비용이다.
				// GroundSnap·WorldBlock은 PathFollow 안에 포함돼 있어 따로 더하지 않는다.
				const float MassMs = R.SectionMs[static_cast<int32>(ESection::MassPathFollow)];
				const float PathGameThreadMs = (MassMs > 0.f)
					? MassMs
					: R.SectionMs[static_cast<int32>(ESection::PathFollow)] + R.SectionMs[static_cast<int32>(ESection::PathApply)];
				const float CrowdTotal = PathGameThreadMs
					+ R.SectionMs[static_cast<int32>(ESection::Avoidance)]
					+ R.SectionMs[static_cast<int32>(ESection::BodySeparation)]
					+ R.SectionMs[static_cast<int32>(ESection::GroupLogic)]
					+ R.SectionMs[static_cast<int32>(ESection::LodSlice)];
				UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] %4d | %6.2f | %6.2f | %6.2f | %5.0f | %10.3f | %8.3f | %4.3f | %8.3f"),
					R.Requested, R.AvgMs, R.P95Ms, R.P99Ms, R.AvgMs > 0.f ? 1000.f / R.AvgMs : 0.f, CrowdTotal,
					PathGameThreadMs,
					R.SectionMs[static_cast<int32>(ESection::Avoidance)],
					R.SectionMs[static_cast<int32>(ESection::BodySeparation)]);
			}

			const FString Path = FPaths::ProjectSavedDir() / TEXT("Profiling/CrowdBench")
				/ FString::Printf(TEXT("Summary_%s_%s.csv"), *State.Label, *State.Stamp);
			FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8);
			UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] 요약 파일: %s"), *FPaths::ConvertRelativePathToFull(Path));
			UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] 단계별 CSV 캡처(게임/렌더/GPU 스레드 포함): %s"),
				*FPaths::ConvertRelativePathToFull(FPaths::ProfilingDir() / TEXT("CSV")));
		}

		UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] %s"), bAborted ? TEXT("중단했습니다.") : TEXT("측정을 마쳤습니다."));
		RestoreFrameLimit();
		State = FState();
	}

	/** Director가 매 프레임 부른다 */
	static void Tick(USubNpcDirectorSubsystem* Director, const TArray<TObjectPtr<ASubNpcCharacter>>& ActiveNpcs)
	{
		if (State.Phase == EPhase::Idle)
		{
			return;
		}
		UWorld* World = State.World.Get();
		if (!World || World != Director->GetWorld())
		{
			return;
		}

		// 실제 프레임 간격 (타임 딜레이션·일시정지와 무관하게 벽시계 기준).
		const float FrameSeconds = static_cast<float>(FApp::GetDeltaTime());
		State.PhaseTime += FrameSeconds;
		const int32 N = State.Counts[State.Step];

		if (State.Phase == EPhase::Settling)
		{
			// 회수가 늦게 끝나거나 풀에서 덜 나왔으면 다시 맞춘다 (스포너는 타이머로만 채운다).
			// 매 프레임 하면 그 자체가 비용이라 0.5초에 한 번만 본다.
			if (Director->GetActiveCount() != N && State.PhaseTime - State.LastApplyTime >= 0.5f)
			{
				State.LastApplyTime = State.PhaseTime;
				ApplyCount(Director, World, N);
			}
			if (State.PhaseTime >= State.WarmupSeconds)
			{
				State.Phase = EPhase::Recording;
				State.PhaseTime = 0.f;
				BeginRecord(N);
			}
			return;
		}

		State.FrameMs.Add(FrameSeconds * 1000.f);
		State.ActiveSum += ActiveNpcs.Num();
		int32 Near = 0;
		for (const ASubNpcCharacter* Npc : ActiveNpcs)
		{
			Near += (IsValid(Npc) && Npc->GetLodBand() == ESubNpcLodBand::Near) ? 1 : 0;
		}
		State.NearSum += Near;

		if (State.PhaseTime >= State.RecordSeconds)
		{
			EndRecord(N);
			if (++State.Step >= State.Counts.Num())
			{
				Finish(Director, false);
				return;
			}
			State.Phase = EPhase::Settling;
			State.PhaseTime = 0.f;
			State.LastApplyTime = 0.f;
			ApplyCount(Director, World, State.Counts[State.Step]);
		}
	}

	static void Start(const TArray<FString>& Args, UWorld* World)
	{
		USubNpcDirectorSubsystem* Director = World ? World->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr;
		if (!Director)
		{
			UE_LOG(LogSubNpc, Warning, TEXT("[CrowdBench] 게임 월드에서 실행하세요 (PIE 또는 Standalone)."));
			return;
		}
		if (State.Phase != EPhase::Idle)
		{
			UE_LOG(LogSubNpc, Warning, TEXT("[CrowdBench] 이미 측정 중입니다. crowd.BenchStop으로 멈춘 뒤 다시 실행하세요."));
			return;
		}

		FState NewState;
		for (const FString& Arg : Args)
		{
			FString Key, Value;
			if (Arg.Split(TEXT("="), &Key, &Value))
			{
				if (Key.Equals(TEXT("secs"), ESearchCase::IgnoreCase))
				{
					NewState.RecordSeconds = FMath::Max(1.f, FCString::Atof(*Value));
				}
				else if (Key.Equals(TEXT("warmup"), ESearchCase::IgnoreCase))
				{
					NewState.WarmupSeconds = FMath::Max(0.f, FCString::Atof(*Value));
				}
				else if (Key.Equals(TEXT("label"), ESearchCase::IgnoreCase))
				{
					NewState.Label = Value;
				}
			}
			else if (Arg.IsNumeric())
			{
				NewState.Counts.Add(FMath::Max(0, FCString::Atoi(*Arg)));
			}
		}
		if (NewState.Counts.Num() == 0)
		{
			NewState.Counts = { 0, 30, 60, 100 };
		}

		NewState.World = World;
		NewState.Stamp = FDateTime::Now().ToString(TEXT("%m%d_%H%M%S"));
		NewState.SavedMaxActive = Director->GetMaxActiveNpcs();
		NewState.Step = 0;
		NewState.Phase = EPhase::Settling;
		State = MoveTemp(NewState);
		DisableFrameLimit();

		FString CountList;
		for (int32 C : State.Counts)
		{
			CountList += FString::Printf(TEXT(" %d"), C);
		}
		UE_LOG(LogSubNpc, Log, TEXT("[CrowdBench] 시작 (%s): 인원%s / 단계마다 안정화 %.0f초 + 측정 %.0f초. 카메라를 움직이지 마세요."),
			*State.Label, *CountList, State.WarmupSeconds, State.RecordSeconds);

		ApplyCount(Director, World, State.Counts[0]);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdBench(
		TEXT("crowd.Bench"),
		TEXT("군중 성능을 인원별로 자동 측정한다. 예) crowd.Bench 0 30 60 100 secs=20 warmup=5 label=Base\n")
		TEXT("결과: 로그 [CrowdBench], Saved/Profiling/CrowdBench/Summary_*.csv, 단계별 CSV 캡처(Saved/Profiling/CSV)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			Start(Args, World);
		}));

	static FAutoConsoleCommandWithWorldAndArgs CmdCount(
		TEXT("crowd.Count"),
		TEXT("행인 수를 바로 N명으로 맞추고 유지한다 (측정 없이 눈으로 볼 때). crowd.Count -1이면 스포너 설정으로 되돌린다."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			USubNpcDirectorSubsystem* Director = World ? World->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr;
			if (!Director || Args.Num() == 0 || State.Phase != EPhase::Idle)
			{
				UE_LOG(LogSubNpc, Warning, TEXT("[Crowd] crowd.Count <인원> - 게임 중에, crowd.Bench가 돌고 있지 않을 때 쓰세요."));
				return;
			}
			const int32 N = FCString::Atoi(*Args[0]);
			if (N < 0)
			{
				GSubNpcBenchTargetOverride = -1;
				Director->SetMaxActiveNpcs(Director->GetActiveCount() + Director->GetPooledCount());
				UE_LOG(LogSubNpc, Log, TEXT("[Crowd] 인원 덮어쓰기 해제 - 스포너 설정을 따릅니다."));
				return;
			}
			ApplyCount(Director, World, N);
			UE_LOG(LogSubNpc, Log, TEXT("[Crowd] 인원을 %d명으로 맞춥니다 (해제: crowd.Count -1)."), N);
		}));

	static FAutoConsoleCommandWithWorldAndArgs CmdBenchStop(
		TEXT("crowd.BenchStop"),
		TEXT("crowd.Bench 측정을 멈추고 인원 설정을 되돌린다. 그때까지 측정한 단계는 요약에 남긴다."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (State.Phase == EPhase::Idle)
			{
				return;
			}
			UWorld* BenchWorld = State.World.Get();
			Finish(BenchWorld ? BenchWorld->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr, true);
		}));
}

#if !UE_BUILD_SHIPPING
namespace SubNpcPathDebug
{
	/** crowd.DrawPaths: 레벨의 모든 행인 경로를 이번 프레임에만 그린다 (그리는 방식은 경로 액터가 갖고 있다) */
	static void DrawAllPaths(UWorld* World)
	{
		for (TActorIterator<ANpcCrowdPath> It(World); It; ++It)
		{
			It->DrawLaneDebug(SubNpcCrowdCVars::SqueezeOverflow);
		}
	}

	static int32 DebugSpot = 0;
	static FAutoConsoleVariableRef CVarDebugSpot(TEXT("crowd.DebugSpot"), DebugSpot,
		TEXT("스팟 근처 행인의 상태(줄N 이동/대기, 칸으로, 이벤트, 길로 복귀, 일행 대기, 양보)를 머리 위에 쓰고 가려는 곳까지 선을 긋는다."),
		ECVF_Cheat);

	static float DebugTextScale = 1.f;
	static FAutoConsoleVariableRef CVarDebugTextScale(TEXT("crowd.DebugTextScale"), DebugTextScale,
		TEXT("crowd.DebugSpot / crowd.DebugRole 머리 위 글자 배율 (시연 영상 녹화용). 기본 1."),
		ECVF_Cheat);

	static void DrawSpotStates(UWorld* World, const TArray<TObjectPtr<ASubNpcCharacter>>& Npcs)
	{
		// crowd.DebugSpot 2: 0.5초마다 스팟 근처 행인을 스팟 기준 좌표로 로그에 남긴다 (동선 재구성용).
		// 좌표 (앞, 옆): 앞 = 스팟이 바라보는 쪽(길 쪽)이 +, 옆 = 스팟 오른쪽이 +. 단위 cm.
		static float NextLogTime = 0.f;
		const float Now = World->GetTimeSeconds();
		const bool bLogNow = DebugSpot >= 2 && Now >= NextLogTime;
		if (bLogNow)
		{
			NextLogTime = Now + 0.5f;
			for (TActorIterator<ANpcActivitySpot> It(World); It; ++It)
			{
				const ANpcActivitySpot* Spot = *It;
				const FTransform SpotXf = Spot->GetActorTransform();
				FString Line;
				for (const ASubNpcCharacter* Npc : Npcs)
				{
					FString Label;
					FVector Target;
					if (!IsValid(Npc) || !Npc->GetSpotDebugState(Label, Target)
						|| FVector::DistSquared2D(Npc->GetActorLocation(), Spot->GetActorLocation()) > FMath::Square(600.f))
					{
						continue;
					}
					const FVector L = SpotXf.InverseTransformPosition(Npc->GetActorLocation());
					const FVector T = SpotXf.InverseTransformPosition(Target);
					Line += FString::Printf(TEXT(" | %s %s (%.0f,%.0f)->(%.0f,%.0f)"),
						*GetNameSafe(Npc).RightChop(15), *Label, L.X, L.Y, T.X, T.Y);
				}
				if (!Line.IsEmpty())
				{
					UE_LOG(LogSubNpc, Log, TEXT("[CrowdSpot] t=%.1f %s%s"), Now, *GetNameSafe(Spot).Right(10), *Line);
				}
			}
		}

		for (const ASubNpcCharacter* Npc : Npcs)
		{
			FString Label;
			FVector Target;
			if (!IsValid(Npc) || !Npc->GetSpotDebugState(Label, Target))
			{
				continue;
			}
			const FVector Loc = Npc->GetActorLocation();
			const FColor Color = Label.Contains(TEXT("줄")) ? FColor::Orange
				: Label.Contains(TEXT("일행")) ? FColor::Green
				: Label.Contains(TEXT("합류")) || Label.Contains(TEXT("복귀")) ? FColor::Cyan
				: FColor::Yellow;
			DrawDebugString(World, Loc + FVector(0.f, 0.f, 120.f), Label, nullptr, Color, 0.f, true, 1.1f * DebugTextScale);
			if (FVector::DistSquared2D(Loc, Target) > FMath::Square(10.f))
			{
				DrawDebugLine(World, Loc - FVector(0.f, 0.f, 80.f), FVector(Target.X, Target.Y, Loc.Z - 80.f), Color, false, 0.f, 0, 2.f);
			}
		}
	}

	// -1(기본) = 에디터(PIE)에서만 켠다. 패키징 빌드(Development 포함)에서는 꺼져 있고 콘솔로 켤 수 있다.
	// Shipping 빌드에서는 이 코드가 통째로 빠진다.
	static int32 DebugRole = -1;
	static FAutoConsoleVariableRef CVarDebugRole(TEXT("crowd.DebugRole"), DebugRole,
		TEXT("가까운 행인 머리 위에 역할(행인/그룹/줄/이벤트/일행 대기/복귀)과 회피 상태(감속·비킴·정지)를 쓴다. -1: 에디터에서만(기본), 0: 끔, 1: 켬."),
		ECVF_Cheat);

	static bool IsRoleDrawOn()
	{
		return DebugRole > 0 || (DebugRole < 0 && GIsEditor);
	}

	/** 일행 표시: 리더별 글자(A~Z)와 인원. GetRole 전에 한 번 채운다 */
	static TMap<const ASubNpcCharacter*, TPair<TCHAR, int32>> GroupTags;

	static void RebuildGroupTags(const TArray<TObjectPtr<ASubNpcCharacter>>& Npcs)
	{
		GroupTags.Reset();
		for (const ASubNpcCharacter* Npc : Npcs)
		{
			if (const ASubNpcCharacter* Leader = IsValid(Npc) ? Npc->GetGroupLeader() : nullptr)
			{
				TPair<TCHAR, int32>& Tag = GroupTags.FindOrAdd(Leader, TPair<TCHAR, int32>(TEXT('?'), 1));
				++Tag.Value;
			}
		}
		int32 Letter = 0;
		for (TPair<const ASubNpcCharacter*, TPair<TCHAR, int32>>& Pair : GroupTags)
		{
			Pair.Value.Key = TCHAR(TEXT('A') + (Letter++ % 26));
		}
	}

	/** 역할 글과 색. 스팟 상태가 있으면 그것, 아니면 그룹/행인. 그룹이면 글자·인원을 붙인다 (리더는 ★) */
	static FString GetRole(const ASubNpcCharacter* Npc, FColor* OutColor = nullptr)
	{
		FString Role;
		FVector Target;
		FColor Color = FColor::White;
		const bool bSpot = Npc->GetSpotDebugState(Role, Target);
		if (bSpot)
		{
			Color = Role.Contains(TEXT("이벤트")) ? FColor::Red
				: Role.Contains(TEXT("칸으로")) ? FColor(255, 120, 120)
				: Role.Contains(TEXT("줄")) ? FColor::Orange
				: Role.Contains(TEXT("일행")) ? FColor::Green
				: Role.Contains(TEXT("반응")) ? FColor::Magenta
				: FColor::Cyan;   // 길로 복귀
		}
		const ASubNpcCharacter* Leader = Npc->GetGroupLeader();
		const TPair<TCHAR, int32>* Tag = GroupTags.Find(Leader ? Leader : Npc);
		if (Tag)
		{
			const FString GroupText = FString::Printf(TEXT("그룹%c(%d)%s"), Tag->Key, Tag->Value, Leader ? TEXT("") : TEXT("★"));
			Role = bSpot ? GroupText + TEXT(" ") + Role : GroupText;
			if (!bSpot)
			{
				Color = FColor(80, 160, 255);
			}
		}
		else if (!bSpot)
		{
			Role = TEXT("행인");
		}
		if (OutColor)
		{
			*OutColor = Color;
		}
		return Role;
	}

	static void DrawRoles(UWorld* World, const TArray<TObjectPtr<ASubNpcCharacter>>& Npcs, const APawn* Player)
	{
		RebuildGroupTags(Npcs);
		for (const ASubNpcCharacter* Npc : Npcs)
		{
			if (!IsValid(Npc) || Npc->GetLodBand() != ESubNpcLodBand::Near)
			{
				continue;
			}
			FColor Color;
			FString Text = GetRole(Npc, &Color);
			// 회피 상태: 걷는 중인데 느리거나 비키는 중이면 덧붙인다
			if (Npc->IsWalkingOnPath())
			{
				const float Ratio = Npc->GetCurrentMoveSpeed() / FMath::Max(1.f, Npc->GetWalkSpeed());
				if (Npc->GetStalledTime() > 0.5f)
				{
					Text += FString::Printf(TEXT("\n정지 %.1f초"), Npc->GetStalledTime());
				}
				else if (Ratio < 0.9f)
				{
					Text += FString::Printf(TEXT("\n감속 %.0f%%"), Ratio * 100.f);
				}
				if (Npc->IsPassing())
				{
					Text += TEXT(" 비킴");
				}
			}
			DrawDebugString(World, Npc->GetActorLocation() + FVector(0.f, 0.f, 120.f), Text, nullptr, Color, 0.f, true, DebugTextScale);
		}
		// 행인이 없는 맵(미니게임 등)에서는 플레이어 글자도 띄우지 않는다
		if (Player && Npcs.Num() > 0)
		{
			DrawDebugString(World, Player->GetActorLocation() + FVector(0.f, 0.f, 120.f), TEXT("플레이어"), nullptr, FColor::Silver, 0.f, true, DebugTextScale);
		}
	}

	// crowd.BlockTest 1: "플레이어가 앞을 막으면 지나가는가"를 자동으로 잰다.
	// 걷는 행인 하나를 골라 그 앞길(같은 차선 1.8m 앞)에 플레이어를 세워 두고, 행인이 플레이어를 지나칠 때까지의
	// 시간을 [CrowdBlockTest]로 남긴다. 15초 안에 못 지나가면 실패. 끝나면 1초 뒤 다른 행인으로 반복한다.
	static int32 BlockTest = 0;
	static FAutoConsoleVariableRef CVarBlockTest(TEXT("crowd.BlockTest"), BlockTest,
		TEXT("1이면 걷는 행인 앞길에 플레이어를 세워 지나가는 데 걸린 시간을 잰다 ([CrowdBlockTest] 로그)."),
		ECVF_Cheat);

	static void TickBlockTest(UWorld* World, const TArray<TObjectPtr<ASubNpcCharacter>>& Npcs)
	{
		struct FState
		{
			TWeakObjectPtr<ASubNpcCharacter> Target;
			float StartTime = 0.f;
			float NextPickTime = 0.f;
			float PlayerDist = 0.f;
			float Along = 1.f;
			FVector PlayerLoc = FVector::ZeroVector;
			int32 Passed = 0, Failed = 0;
			float TotalTime = 0.f, MaxTime = 0.f;
		};
		static FState S;

		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Player = PC ? PC->GetPawn() : nullptr;
		if (!Player)
		{
			return;
		}
		const float Now = World->GetTimeSeconds();
		if (Now < S.StartTime - 1.f)
		{
			S = FState();   // 새 판
		}

		ASubNpcCharacter* Npc = S.Target.Get();
		if (Npc)
		{
			// 플레이어는 그 자리에 계속 서 있는다 (밀리거나 떨어져도 되돌린다)
			if (FVector::DistSquared2D(Player->GetActorLocation(), S.PlayerLoc) > FMath::Square(5.f))
			{
				Player->SetActorLocation(S.PlayerLoc, false, nullptr, ETeleportType::TeleportPhysics);
			}

			const ANpcCrowdPath* Path = Npc->GetCurrentPath();
			const bool bGone = !IsValid(Npc) || Npc->IsPooled() || !Path;
			const float Ahead = bGone ? 0.f : (Npc->GetPathDistance() - S.PlayerDist) * S.Along;
			const float Elapsed = Now - S.StartTime;
			if (bGone || Ahead > 80.f || Elapsed > 15.f || !Npc->IsWalkingOnPath())
			{
				const bool bPassed = !bGone && Ahead > 80.f;
				if (bPassed)
				{
					++S.Passed;
					S.TotalTime += Elapsed;
					S.MaxTime = FMath::Max(S.MaxTime, Elapsed);
				}
				else if (Elapsed > 15.f)
				{
					++S.Failed;
				}
				UE_LOG(LogSubNpc, Log, TEXT("[CrowdBlockTest] %s<%s> %s %.1f초 | 누적 통과 %d (평균 %.1f초, 최대 %.1f초) 실패 %d"),
					*GetNameSafe(Npc), bGone ? TEXT("-") : *GetRole(Npc).Replace(TEXT("\n"), TEXT(" ")),
					bPassed ? TEXT("통과") : (Elapsed > 15.f ? TEXT("실패(15초)") : TEXT("중단(경로 바뀜·스팟)")), Elapsed,
					S.Passed, S.Passed > 0 ? S.TotalTime / S.Passed : 0.f, S.MaxTime, S.Failed);
				S.Target = nullptr;
				S.NextPickTime = Now + 1.f;
			}
			return;
		}
		if (Now < S.NextPickTime)
		{
			return;
		}

		// 가까이 걷는 행인 중 하나 (경로 중간, 앞으로 3m 이상 남은 사람)
		RebuildGroupTags(Npcs);
		TArray<ASubNpcCharacter*, TInlineAllocator<64>> Candidates;
		for (ASubNpcCharacter* Candidate : Npcs)
		{
			const ANpcCrowdPath* Path = IsValid(Candidate) ? Candidate->GetCurrentPath() : nullptr;
			if (!Path || !Candidate->IsWalkingOnPath() || Candidate->IsMovingOffLane() || Candidate->GetLodBand() != ESubNpcLodBand::Near)
			{
				continue;
			}
			const float Along = Candidate->IsReversedOnPath() ? -1.f : 1.f;
			const float PlaceDist = Candidate->GetPathDistance() + Along * 180.f;
			if (PlaceDist < 100.f || PlaceDist > Path->GetPathLength() - 300.f)
			{
				continue;
			}
			Candidates.Add(Candidate);
		}
		if (Candidates.Num() == 0)
		{
			return;
		}
		Npc = Candidates[FMath::RandRange(0, Candidates.Num() - 1)];
		const ANpcCrowdPath* Path = Npc->GetCurrentPath();
		S.Along = Npc->IsReversedOnPath() ? -1.f : 1.f;
		S.PlayerDist = Npc->GetPathDistance() + S.Along * 180.f;
		const FVector Ground = Path->GetLocationAtDistance(S.PlayerDist, Npc->GetLaneOffset());
		S.PlayerLoc = Ground + FVector(0.f, 0.f, Player->GetSimpleCollisionHalfHeight() + 2.f);
		Player->SetActorLocation(S.PlayerLoc, false, nullptr, ETeleportType::TeleportPhysics);
		S.PlayerLoc = Player->GetActorLocation();
		S.Target = Npc;
		S.StartTime = Now;
	}

	/** crowd.DumpPaths: 경로 모양을 로그로 남긴다 (길이·폭·출구, 200cm마다 위치와 방향). 에디터 없이 배치를 볼 때 쓴다 */
	static FAutoConsoleCommandWithWorldAndArgs CmdDumpPaths(
		TEXT("crowd.DumpPaths"),
		TEXT("레벨의 행인 경로 모양을 로그로 남긴다 ([CrowdPath])."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			for (TActorIterator<ANpcCrowdPath> It(World); It; ++It)
			{
				const ANpcCrowdPath* Path = *It;
				FString Exits;
				for (const FNpcPathExit& Exit : Path->GetExits())
				{
					Exits += GetNameSafe(Exit.NextPath) + TEXT(" ");
				}
				UE_LOG(LogSubNpc, Log, TEXT("[CrowdPath] %s 길이%.0f 반폭%.0f 출구=[%s]"),
					*GetNameSafe(Path), Path->GetPathLength(), Path->GetLaneHalfWidth(), *Exits);
				for (float D = 0.f; D <= Path->GetPathLength() + 1.f; D += 200.f)
				{
					const FVector Loc = Path->GetLocationAtDistance(D);
					const FVector Dir = Path->GetDirectionAtDistance(D);
					UE_LOG(LogSubNpc, Log, TEXT("[CrowdPath]   %5.0f: (%6.0f,%6.0f,%5.0f) 방향 %4.0f도"),
						D, Loc.X, Loc.Y, Loc.Z, FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X)));
				}
			}
		}));
}
#endif


void USubNpcDirectorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// crowd.Count로 정한 인원은 전역이라 PIE를 다시 켜도 남는다. 새 판은 스포너 설정으로 시작한다.
	// 미리보기 월드 같은 다른 월드가 생길 때 돌고 있는 측정을 끊지 않도록 게임 월드에서만 한다.
	if (const UWorld* World = GetWorld(); World && World->IsGameWorld() && SubNpcBench::State.Phase == SubNpcBench::EPhase::Idle)
	{
		GSubNpcBenchTargetOverride = -1;
	}

	LodCursor = 0;
	LodTimer = 0.f;
}

void USubNpcDirectorSubsystem::Deinitialize()
{
	// [크래시 대응] 예전에는 여기서 풀과 활성 목록을 순회하며 Destroy()를 불렀다.
	//
	// 그런데 Destroy()는 그 자리에서 EndPlay를 부르고, ASubNpcCharacter::EndPlay는
	// Director->UnregisterNpc(this)를 호출한다. 그 함수가 하는 일이
	// ActiveNpcs.RemoveSingleSwap(this) 다 - 지금 순회 중인 바로 그 배열이다.
	// 순회 중 배열이 줄어들면 유효하지 않은 메모리를 읽게 되고, 인원이 많을수록
	// 반드시 터진다. ("사람이 적으면 안 터진다"의 정체)
	//
	// 애초에 여기서 Destroy를 부를 이유가 없다. 이 시점은 월드가 정리되는 중이고,
	// 월드는 자기 액터를 **전부 스스로 파괴한다.** 우리가 먼저 손대면 순서만 꼬인다.
	// 그래서 참조만 끊고 물러난다. 액터 수명은 월드에 맡긴다.
	//
	// 레벨 전환 후 메모리가 남는 걱정은 없다. 풀 액터는 RF_Transient로 스폰되고
	// PIE/레벨 월드와 함께 사라진다.
	ActiveNpcs.Reset();
	PooledNpcs.Reset();
	ArchetypeTable = nullptr;

	Super::Deinitialize();
}

TStatId USubNpcDirectorSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USubNpcDirectorSubsystem, STATGROUP_Tickables);
}

void USubNpcDirectorSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 회수 예약된 개체를 먼저 정리한다.
	// 순회 도중에 활성 배열이 바뀌지 않도록, 실제 회수는 항상 이 시점에만 일어난다.
	ProcessPendingRecycles();

	// 아직 Mass 엔티티가 없는 행인에게 붙인다 (풀에서 막 나왔거나 레벨에 직접 배치된 개체).
	// 이미 붙은 개체는 핸들 확인 한 번으로 끝난다.
	if (UWorld* World = GetWorld())
	{
		for (ASubNpcCharacter* Npc : ActiveNpcs)
		{
			SubNpcMassBinding::Bind(World, Npc);
		}
	}

#if !UE_BUILD_SHIPPING
	if (SubNpcCrowdCVars::DebugDraw != 0)
	{
		DrawDebugMarkers();
	}
	if (SubNpcCrowdCVars::DrawPaths != 0)
	{
		SubNpcPathDebug::DrawAllPaths(GetWorld());
	}
	if (SubNpcPathDebug::IsRoleDrawOn())
	{
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		SubNpcPathDebug::DrawRoles(GetWorld(), ActiveNpcs, PC ? PC->GetPawn() : nullptr);
	}
	else if (SubNpcPathDebug::DebugSpot != 0)
	{
		SubNpcPathDebug::DrawSpotStates(GetWorld(), ActiveNpcs);
	}
	if (SubNpcPathDebug::BlockTest != 0)
	{
		SubNpcPathDebug::TickBlockTest(GetWorld(), ActiveNpcs);
	}
#endif

	if (SubNpcCrowdCVars::PrintStats != 0)
	{
		StatsTimer += DeltaTime;
		if (StatsTimer >= 1.f)
		{
			StatsTimer = 0.f;
			UE_LOG(LogSubNpc, Log, TEXT("[Crowd] 활성 %d / 풀 %d / 상한 %d"),
				ActiveNpcs.Num(), PooledNpcs.Num(), MaxActiveNpcs);
		}
	}

	if (bEnableAvoidance && SubNpcCrowdCVars::EnableAvoidance != 0)
	{
		AvoidTimer += DeltaTime;
		if (AvoidTimer >= AvoidUpdateInterval)
		{
			AvoidTimer = 0.f;
			{
				SUBNPC_PROFILE_SCOPE(Avoidance);
				UpdateAvoidance();
			}
			{
				SUBNPC_PROFILE_SCOPE(GroupLogic);
				UpdateGroupChats();
				UpdateGroupCohesion();
				UpdateOffLaneYield();
			}
		}

		// 몸 겹침 금지는 매 프레임 본다. 회피 계산(0.1초 간격)만으로는 그 사이에 파고든다.
		// 회피 계산 뒤에 둔다 - UpdateOffLaneYield가 양보 플래그를 다시 정하므로, 겹침 금지의 양보가 마지막에 덮어써야 한다.
		{
			SUBNPC_PROFILE_SCOPE(BodySeparation);
			TSet<ASubNpcCharacter*> OffLaneYielders;
			SubNpcBodySeparation::Enforce(GetWorld(), ActiveNpcs, OffLaneYielders);

			// 길 밖 이동 양보. 너무 오래 서 있으면(배치상 비켜 갈 수 없는 자리) 더는 세우지 않는다 - 영영 못 움직이는 것보다 낫다.
			static TMap<TWeakObjectPtr<ASubNpcCharacter>, float> YieldSince;
			const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
			for (auto It = YieldSince.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid() || !OffLaneYielders.Contains(It.Key().Get()) || Now < It.Value())
				{
					It.RemoveCurrent();
				}
			}
			for (ASubNpcCharacter* Npc : OffLaneYielders)
			{
				const float Since = YieldSince.FindOrAdd(Npc, Now);
				if (Now - Since < SubNpcBodySeparation::MaxOffLaneYieldSeconds)
				{
					Npc->SetOffLaneYield(true);
				}
			}
		}
	}

	CSV_CUSTOM_STAT(Crowd, ActiveNpcs, ActiveNpcs.Num(), ECsvCustomStatOp::Set);
	SubNpcOverlapCheck::Run(GetWorld(), ActiveNpcs);
	SubNpcBench::Tick(this, ActiveNpcs);

	LodTimer += DeltaTime;
	if (LodTimer < LodUpdateInterval)
	{
		return;
	}
	LodTimer = 0.f;

	SUBNPC_PROFILE_SCOPE(LodSlice);
	UpdateLodSlice();
}

// ---------------------------------------------------------------------------
// 풀
// ---------------------------------------------------------------------------

void USubNpcDirectorSubsystem::PrewarmPool(TSubclassOf<ASubNpcCharacter> NpcClass, int32 Count)
{
	UWorld* World = GetWorld();
	if (!World || !NpcClass || Count <= 0)
	{
		return;
	}

	FActorSpawnParameters Params;
	// 스폰 위치가 지형에 겹쳐도 무조건 생성돼야 한다 (어차피 바로 숨길 것이므로).
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;

	// 화면 밖 아주 먼 곳에서 만든다.
	const FVector Nowhere(0.f, 0.f, -100000.f);

	for (int32 i = 0; i < Count; ++i)
	{
		ASubNpcCharacter* Npc = World->SpawnActor<ASubNpcCharacter>(NpcClass, Nowhere, FRotator::ZeroRotator, Params);
		if (!Npc)
		{
			SUBNPC_LOG(Warning, TEXT("[Crowd] 풀 생성 실패 (%d/%d). 클래스 설정을 확인하세요."), i, Count);
			break;
		}

		// [버그 수정] 런타임 SpawnActor는 **그 자리에서 BeginPlay를 호출한다.**
		// ASubNpcCharacter::BeginPlay 는 Director->RegisterNpc(this) 를 부르므로,
		// 방금 만든 이 개체는 이미 ActiveNpcs 안에 들어가 있다.
		//
		// 그대로 PooledNpcs에 넣으면 **같은 액터가 두 배열에 동시에 존재**하게 된다.
		// 그 상태에서 벌어지는 일:
		//   - GetActiveCount()가 처음부터 풀 크기만큼 나온다 (스포너가 "이미 찼다"고 판단)
		//   - UpdateLodSlice가 풀 액터를 순회하며 ApplyLodBand로 다시 보이게 만든다
		//     → Base Target Count가 0이어도 100명이 화면에 나타난다
		//   - 회수·파괴 경로에서 같은 액터가 두 번 처리된다
		//
		// 풀에 넣기 전에 활성 목록에서 확실히 뺀다.
		ActiveNpcs.RemoveSingleSwap(Npc, EAllowShrinking::No);

		Npc->DeactivateToPool();
		PooledNpcs.AddUnique(Npc);
	}

	SUBNPC_LOG(Log, TEXT("[Crowd] 풀 준비 완료: %d명"), PooledNpcs.Num());
}

void USubNpcDirectorSubsystem::SetArchetypeTable(UDataTable* InTable)
{
	// 스포너에 테이블이 없으면 프로젝트 설정의 기본 테이블을 쓴다.
	// 레벨마다 스포너를 일일이 세팅하지 않아도 대사·애니메이션이 한 곳에서 관리된다.
	if (!InTable)
	{
		InTable = GetDefault<UNPCSetting>()->DefaultSubNpcArchetypeTable.LoadSynchronous();
		if (!InTable)
		{
			SUBNPC_LOG(Warning,
				TEXT("[Crowd] 아키타입 테이블이 없습니다. 프로젝트 설정 > NPC Setting > Default Sub Npc Archetype Table을 지정하세요. ")
				TEXT("지정 전까지 행인은 기본값으로 움직이고, 대사가 없어 말 걸기가 비활성화됩니다."));
		}
	}

	ArchetypeTable = InTable;
}

bool USubNpcDirectorSubsystem::PickArchetype(FSubNpcArchetypeRow& OutRow) const
{
	if (!ArchetypeTable)
	{
		// 테이블이 없어도 기본값으로 돌아가야 한다 (세팅 안 됐다고 군중이 통째로 사라지면 곤란).
		OutRow = FSubNpcArchetypeRow();
		return true;
	}

	TArray<float> Weights;
	TArray<const FSubNpcArchetypeRow*> Rows;

	for (const TPair<FName, uint8*>& Pair : ArchetypeTable->GetRowMap())
	{
		const FSubNpcArchetypeRow* Row = reinterpret_cast<const FSubNpcArchetypeRow*>(Pair.Value);
		if (Row && Row->SpawnWeight > 0.f)
		{
			Rows.Add(Row);
			Weights.Add(Row->SpawnWeight);
		}
	}

	const int32 Picked = NpcWeightedPick::PickIndex(Weights);
	if (Picked == INDEX_NONE)
	{
		OutRow = FSubNpcArchetypeRow();
		return true;
	}

	OutRow = *Rows[Picked];
	return true;
}

ASubNpcCharacter* USubNpcDirectorSubsystem::SpawnOnPath(ANpcCrowdPath* Path, float StartDistance, bool bReversed)
{
	if (!Path || !Path->IsOpen())
	{
		return nullptr;
	}

	// 하드 캡. 이 한 줄이 메모리·GPU 예산을 지키는 마지막 방어선이다.
	if (ActiveNpcs.Num() >= MaxActiveNpcs || PooledNpcs.Num() == 0)
	{
		if (SubNpcCrowdCVars::TraceSpawn != 0)
		{
			UE_LOG(LogSubNpc, Warning,
				TEXT("[Crowd] 등장 실패: %s (활성 %d / 상한 %d / 풀 %d)"),
				(PooledNpcs.Num() == 0) ? TEXT("풀이 비었다") : TEXT("정원이 찼다"),
				ActiveNpcs.Num(), MaxActiveNpcs, PooledNpcs.Num());
		}
		return nullptr;
	}

	ASubNpcCharacter* Npc = PooledNpcs.Pop(EAllowShrinking::No);

	// [버그 수정] 예전에는 여기서 그냥 return nullptr 했다. 그러면 방금 꺼낸 개체가
	// 풀에도 활성 목록에도 없는 상태로 **영원히 사라진다.** 죽은 개체가 몇 번 섞이면
	// 풀이 조금씩 말라서 "한참 뒤부터 사람이 안 나온다"가 된다.
	// 유효하지 않은 항목은 버리고 다음 것으로 계속 시도한다.
	while (!IsValid(Npc) && PooledNpcs.Num() > 0)
	{
		Npc = PooledNpcs.Pop(EAllowShrinking::No);
	}

	if (!IsValid(Npc))
	{
		if (SubNpcCrowdCVars::TraceSpawn != 0)
		{
			UE_LOG(LogSubNpc, Warning, TEXT("[Crowd] 등장 실패: 풀에 유효한 개체가 없다."));
		}
		return nullptr;
	}

	// [개선] 등장 지점이 이미 붐비면 경로를 따라 조금 밀어서 세운다.
	// 같은 자리에 겹쳐 등장하면 한 덩어리로 몰려다니고, 보기에도 바로 티가 난다.
	const float EffectiveSpacing = (SubNpcCrowdCVars::SpawnSpacingOverride >= 0.f)
		? SubNpcCrowdCVars::SpawnSpacingOverride
		: SpawnSpacing;

	if (EffectiveSpacing > 0.f)
	{
		const float PathLength = Path->GetPathLength();
		const float Step = EffectiveSpacing;
		const float SpacingSq = FMath::Square(EffectiveSpacing);

		// 최대 8칸까지만 밀어본다. 그래도 자리가 없으면 이번 등장은 포기하고 풀에 되돌린다.
		//
		// [변경] 예전에는 자리를 못 찾아도 원래 자리에 그냥 세웠다 (입구가 차서 목표 인원에 못 닿는 걸 피하려고).
		// 그 결과 인원을 한 번에 늘리면 사람 위에 사람이 겹쳐 등장했다 (로그: 100명으로 올린 직후 44쌍 겹침).
		// 겹침은 허용하지 않는다. 자리가 없으면 스포너가 다음 주기에 다시 시도한다 - 입구가 막혀 있으면
		// 인원이 천천히 차는 게 맞는 그림이다.
		bool bFoundSpot = false;
		for (int32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			const float Candidate = FMath::Clamp(
				StartDistance + (bReversed ? -1.f : 1.f) * Step * Attempt, 0.f, PathLength);

			const FVector CandidateLoc = Path->GetLocationAtDistance(Candidate, 0.f);

			bool bBlocked = false;
			for (const ASubNpcCharacter* Other : ActiveNpcs)
			{
				if (IsValid(Other) &&
					FVector::DistSquared(Other->GetActorLocation(), CandidateLoc) < SpacingSq)
				{
					bBlocked = true;
					break;
				}
			}

			if (!bBlocked)
			{
				StartDistance = Candidate;
				bFoundSpot = true;
				break;
			}
		}

		if (!bFoundSpot)
		{
			PooledNpcs.Add(Npc);
			if (SubNpcCrowdCVars::TraceSpawn != 0)
			{
				UE_LOG(LogSubNpc, Warning, TEXT("[Crowd] 등장 보류: %s 근처에 빈자리가 없다 (겹쳐 세우지 않는다)."), *GetNameSafe(Path));
			}
			return nullptr;
		}
	}

	FSubNpcArchetypeRow Archetype;
	PickArchetype(Archetype);

	Npc->ActivateFromPool(Archetype, Path, StartDistance, bReversed);

	// 위의 빈자리 검사는 경로 중심선으로 봤다. 실제로 선 자리(차선)에서 누군가와 몸이 닿으면 되돌린다.
	{
		const FVector MyLoc = Npc->GetActorLocation();
		const float MyRadius = Npc->GetSimpleCollisionRadius();
		for (const ASubNpcCharacter* Other : ActiveNpcs)
		{
			if (IsValid(Other) && !Other->IsPooled()
				&& FMath::Abs(Other->GetActorLocation().Z - MyLoc.Z) < 150.f
				&& FVector::DistSquared2D(Other->GetActorLocation(), MyLoc) < FMath::Square(MyRadius + Other->GetSimpleCollisionRadius() + 4.f))
			{
				Npc->DeactivateToPool();
				PooledNpcs.Add(Npc);
				return nullptr;
			}
		}
	}

	ActiveNpcs.Add(Npc);

	return Npc;
}

TArray<ASubNpcCharacter*> USubNpcDirectorSubsystem::SpawnGroupOnPath(ANpcCrowdPath* Path, float StartDistance, bool bReversed, int32 GroupSize)
{
	TArray<ASubNpcCharacter*> Group;
	if (!Path || GroupSize <= 0)
	{
		return Group;
	}

	ASubNpcCharacter* Leader = SpawnOnPath(Path, StartDistance, bReversed);
	if (!Leader)
	{
		return Group;
	}
	Group.Add(Leader);


	const float Length = FMath::Max(1.f, Path->GetPathLength());

	for (int32 i = 1; i < GroupSize; ++i)
	{
		// [개선] 예전에는 전원을 리더와 **똑같은 지점**에 세우고 뒤에서 간격만 벌렸다.
		// 그러면 등장하는 순간 한 점에 겹쳐 있다가 서서히 풀리는 게 눈에 보인다.
		// 처음부터 조금씩 뒤에 세운다.
		const float MemberStart = FMath::Clamp(
			StartDistance + (bReversed ? 1.f : -1.f) * FMath::FRandRange(60.f, 140.f) * i,
			0.f, Length);

		ASubNpcCharacter* Member = SpawnOnPath(Path, MemberStart, bReversed);
		if (!Member)
		{
			break;
		}

		// [그룹핑] 리더 옆에 붙어 걷는다. 좌우 위치는 리더 기준 상대값이다 (SetGroupLeader).
		// 첫 번째 일행은 거의 나란히, 두 번째 일행은 반대쪽 살짝 뒤. 완전히 일직선이면 행진처럼 보인다.
		// 예전 값(옆 50~90, 앞뒤 60~140 x 순번)은 너무 벌어져서 일행으로 안 보였다.
		const float Side = (i % 2 == 1) ? -1.f : 1.f;
		const float SideOffset = Side * FMath::FRandRange(
			SubNpcCrowdCVars::GroupSideMin, SubNpcCrowdCVars::GroupSideMax);
		const float Gap = (i == 1)
			? FMath::FRandRange(0.f, SubNpcCrowdCVars::GroupAbreastGapMax)
			: FMath::FRandRange(SubNpcCrowdCVars::GroupBehindGapMin, SubNpcCrowdCVars::GroupBehindGapMax) * (i - 1);

		Member->SetGroupLeader(Leader, Gap, SideOffset);
		Group.Add(Member);
	}

	// 일행이 생겼으면 리더로 표시한다. 리더는 스팟에 들르지 않는다 (RollForActivitySpot).
	Leader->SetHasGroupFollowers(Group.Num() > 1);

	return Group;
}

void USubNpcDirectorSubsystem::ReturnToPool(ASubNpcCharacter* Npc)
{
	if (!IsValid(Npc))
	{
		return;
	}

	ActiveNpcs.RemoveSingleSwap(Npc, EAllowShrinking::No);

	Npc->DeactivateToPool();

	// 중복 반납 방지. 같은 개체가 풀에 두 번 들어가면 두 명이 동시에 활성화된다.
	PooledNpcs.AddUnique(Npc);
}

void USubNpcDirectorSubsystem::RegisterNpc(ASubNpcCharacter* Npc)
{
	if (!IsValid(Npc) || Npc->IsPooled())
	{
		return;
	}

	// [방어] 이미 풀에 들어있는 개체는 활성으로 올리지 않는다.
	// IsPooled() 검사만으로는 부족하다 - PrewarmPool은 BeginPlay(여기) 다음에
	// DeactivateToPool을 부르므로, 그 시점에는 아직 Pooled 상태가 아니다.
	if (PooledNpcs.Contains(Npc))
	{
		return;
	}

	ActiveNpcs.AddUnique(Npc);
}

void USubNpcDirectorSubsystem::UnregisterNpc(ASubNpcCharacter* Npc)
{
	SubNpcMassBinding::Unbind(GetWorld(), Npc);

	ActiveNpcs.RemoveSingleSwap(Npc, EAllowShrinking::No);
	PooledNpcs.RemoveSingleSwap(Npc, EAllowShrinking::No);
}

// ---------------------------------------------------------------------------
// LOD
// ---------------------------------------------------------------------------

bool USubNpcDirectorSubsystem::GetViewerLocation(FVector& OutLocation) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 카메라가 아니라 폰 위치를 기준으로 삼는다. 3인칭에서 카메라가 뒤로 빠져 있어도
	// "플레이어 주변"이 기준이어야 군중 밀도가 자연스럽다.
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn())
		{
			OutLocation = Pawn->GetActorLocation();
			return true;
		}
	}
	return false;
}

#if !UE_BUILD_SHIPPING
void USubNpcDirectorSubsystem::DrawDebugMarkers() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 이 표식은 액터 위치에 직접 그린다. 스켈레탈 메시가 숨겨져 있어도,
	// LOD가 최저여도, 애니메이션이 멈춰 있어도 상관없이 그려진다.
	// 그래서 "사람이 안 보이는" 원인이 렌더링인지 회수인지 눈으로 가를 수 있다.
	for (const TObjectPtr<ASubNpcCharacter>& Npc : ActiveNpcs)
	{
		if (!IsValid(Npc))
		{
			continue;
		}

		FColor Color = FColor::White;
		const TCHAR* BandName = TEXT("?");
		switch (Npc->GetLodBand())
		{
		case ESubNpcLodBand::Near:		Color = FColor::Green;	BandName = TEXT("N"); break;
		case ESubNpcLodBand::Mid:		Color = FColor::Yellow;	BandName = TEXT("M"); break;
		case ESubNpcLodBand::Far:		Color = FColor::Orange;	BandName = TEXT("F"); break;
		case ESubNpcLodBand::Culled:	Color = FColor::Red;	BandName = TEXT("C"); break;
		default: break;
		}

		const FVector Loc = Npc->GetActorLocation();

		// 발밑 원 - 위치가 어디인지
		DrawDebugCircle(World, Loc - FVector(0, 0, 88.f), 34.f, 12, Color,
			/*bPersistentLines=*/false, /*LifeTime=*/-1.f, 0, 2.f,
			FVector(1, 0, 0), FVector(0, 1, 0), /*bDrawAxis=*/false);

		// 머리 위 글자 - 지금 어느 LOD 밴드인지
		DrawDebugString(World, Loc + FVector(0, 0, 110.f), BandName, nullptr, Color, 0.f, true, 1.2f);
	}
}
#endif

void USubNpcDirectorSubsystem::ProcessPendingRecycles()
{
	// 뒤에서부터 순회한다. RemoveSingleSwap이 뒤 원소를 앞으로 당기므로
	// 앞에서부터 돌면 방금 당겨온 원소를 건너뛴다.
	for (int32 i = ActiveNpcs.Num() - 1; i >= 0; --i)
	{
		ASubNpcCharacter* Npc = ActiveNpcs[i];
		if (!IsValid(Npc))
		{
			ActiveNpcs.RemoveAtSwap(i, 1, EAllowShrinking::No);
			continue;
		}

		if (Npc->IsPendingRecycle())
		{
			Npc->ClearPendingRecycle();
			ReturnToPool(Npc);
		}
	}
}

namespace
{
	/** 속도 회피: 행인마다 기억하는 것 (비키던 방향, 길 밖 허용). 게임 스레드에서만 읽고 쓴다 */
	struct FAvoidMemory
	{
		float Side = 0.f;            // 진행 방향 기준 +1 오른쪽, -1 왼쪽
		float SideUntil = 0.f;
		float OverflowUntil = 0.f;
	};
	// 클래스 멤버로 두지 않는 이유는 LodHysteresis 주석 참고 (헤더를 바꾸면 핫 리로드 때 오프셋이 밀린다)
	TMap<const ASubNpcCharacter*, FAvoidMemory> AvoidMemory;
}

void USubNpcDirectorSubsystem::UpdateAvoidance()
{
	FVector ViewerLocation;
	if (!GetViewerLocation(ViewerLocation))
	{
		return;
	}

	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	// 다 지난 기억은 버린다. 시각이 지금보다 한참 뒤면 이전 판(PIE 재시작)의 것이다 - 포인터가 재사용됐을 수 있다.
	for (auto It = AvoidMemory.CreateIterator(); It; ++It)
	{
		const float Latest = FMath::Max(It.Value().SideUntil, It.Value().OverflowUntil);
		if (Latest < Now || Latest > Now + 10.f)
		{
			It.RemoveCurrent();
		}
	}

	// [측정 근거] stat game에서 Director 전체(TickableGameObjects Time)의
	// 0명 -> 90명 델타가 0.06 ms에 불과했다. 예전 기본값 24는 근거 없이 보수적이었고,
	// 그 탓에 90명 중 24명만 회피에 참여해 나머지가 그대로 겹쳐 보였다.
	const int32 PairBudget = (SubNpcCrowdCVars::AvoidPairsOverride >= 0)
		? SubNpcCrowdCVars::AvoidPairsOverride
		: MaxAvoidancePairs;

	// 가까이 보이는 개체만 모은다. 여기가 회피 비용을 결정한다.
	// [skydodo] 2026-10-17 버그 수정: 예산을 넘으면 ActiveNpcs 순서대로 앞 PairBudget명만 남겨,
	// 200명 장면에서 화면 바로 앞 사람이 회피(그리고 남의 장애물 목록)에서 통째로 빠져 마주 오는 무리가 엉켰다.
	// 후보를 전부 모은 뒤 시점에서 가까운 순으로 PairBudget명을 남긴다.
	TArray<ASubNpcCharacter*> Nearby;
	Nearby.Reserve(ActiveNpcs.Num());

	for (ASubNpcCharacter* Npc : ActiveNpcs)
	{
		// [버그 수정] 예전에는 걷는 사람만 모았다. 플레이어에게 부딪혀 멈춘(Reacting)
		// 앞사람이 목록에서 빠지니, 뒷사람은 제동을 못 걸고 그대로 뚫고 지나갔다.
		// 멈춘 사람도 장애물로는 넣고, 감속·밀어내기는 걷는 사람에게만 건다.
		// [추가] 스팟으로 가거나 줄 자리로 옮기는 사람(길 밖으로 움직이는 사람)도 장애물로 넣는다.
		// 이들은 길을 가로질러 가는데, 예전에는 목록에서 빠져 있어서 길 위 행인이 못 보고 엉켰다.
		// [추가] 스팟 칸·줄·일행 대기 자리에 서 있는 사람도 장애물로 넣는다. 길가에 서 있으면 길 위 행인이
		// 옆 차선으로 비켜 간다 (스팟 쪽 사람이 길 가는 사람을 피하면 줄·이벤트 동선이 꼬였다).
		const bool bSpotStander = IsValid(Npc) && Npc->GetActivity() == ESubNpcActivity::AtSpot && !Npc->IsMovingOffLane();
		if (!IsValid(Npc) || !(Npc->IsBlockingOnPath() || Npc->IsMovingOffLane() || bSpotStander))
		{
			continue;
		}

		const ESubNpcLodBand Band = Npc->GetLodBand();
		if (Band != ESubNpcLodBand::Near && Band != ESubNpcLodBand::Mid)
		{
			continue;
		}

		Nearby.Add(Npc);
	}
	if (Nearby.Num() > PairBudget)
	{
		const FVector2D View(ViewerLocation);
		Nearby.Sort([&View](const ASubNpcCharacter& A, const ASubNpcCharacter& B)
		{
			return FVector2D::DistSquared(FVector2D(A.GetActorLocation()), View)
				< FVector2D::DistSquared(FVector2D(B.GetActorLocation()), View);
		});
		Nearby.SetNum(FMath::Max(PairBudget, 0), EAllowShrinking::No);
	}

	// ---- 플레이어 접촉 ---------------------------------------------------------
	//
	// 행인은 스윕 없이 움직이므로 서 있는 플레이어에게 걸어 들어가도 NotifyHit이 안 불린다.
	// 그래서 몸이 닿았는지 여기서 거리로 직접 본다. 비켜 가는 건 아래 속도 회피가 맡는다.
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* PlayerPawn = PC ? PC->GetPawn() : nullptr;
	const float PlayerRadius = PlayerPawn ? PlayerPawn->GetSimpleCollisionRadius() : 0.f;

	if (PlayerPawn)
	{
		const FVector PlayerLoc = PlayerPawn->GetActorLocation();
		for (ASubNpcCharacter* Npc : Nearby)
		{
			if (!Npc->IsWalkingOnPath())
			{
				continue;
			}

			const float ContactDist = Npc->GetSimpleCollisionRadius() + PlayerRadius
				+ SubNpcCrowdCVars::LightBumpContactSlack;
			if (FVector::DistSquared2D(PlayerLoc, Npc->GetActorLocation()) >= FMath::Square(ContactDist))
			{
				continue;
			}

			// 누가 부딪혔는지 가른다. 플레이어가 밀고 들어온 경우만 반응하고,
			// 행인이 걸어와 닿은 거면 반응 없이 비켜 지나간다 (아래 속도 회피).
			if (ASubNpcCharacter::GetPlayerClosingSpeed(PlayerPawn, Npc) >= ASubNpcCharacter::PlayerBumpMinClosingSpeed)
			{
				Npc->ReactToPlayerBump(PlayerPawn, ASubNpcCharacter::GetPlayerBumpSpeed(PlayerPawn));
			}
		}
	}

	const float PassUntil = Now + SubNpcCrowdCVars::PassHoldTime;

	// crowd.LogBlock용 집계: 감속 이유별 인원, 남을 가장 많이 막은 상대.
	TMap<FString, int32> ReasonCounts;
	TMap<const AActor*, int32> BlockerCounts;
	static TArray<FString> StallChainLog;   // 2초 로그 사이에 모은 "누가 누구 때문에 오래 서 있나"
#if !UE_BUILD_SHIPPING
	if (SubNpcCrowdCVars::LogBlock != 0)
	{
		SubNpcPathDebug::RebuildGroupTags(ActiveNpcs);   // 정체 로그에 그룹 글자를 붙이려고
	}
#endif

	// crowd.DebugBlock 1: 누가 누구 때문에 느려졌는지 머리 위에 선을 긋는다. 다음 갱신까지만 보인다.
	auto DrawBlockDebug = [&](const ASubNpcCharacter* From, const ASubNpcCharacter* To, const APawn* Player, const FColor& Color)
	{
#if !UE_BUILD_SHIPPING
		if (SubNpcCrowdCVars::LogBlock != 0)
		{
			const TCHAR* Reason = (Color == FColor::Red) ? TEXT("정지") : TEXT("감속·비킴");
			ReasonCounts.FindOrAdd(Reason)++;
			if (const AActor* Target = To ? static_cast<const AActor*>(To) : static_cast<const AActor*>(Player))
			{
				BlockerCounts.FindOrAdd(Target)++;
			}

			// 오래 서 있는 사람이 누구 때문에 서 있는지 한 줄씩 남긴다 (역할·경로·방향·차선 포함).
			if (From && From->GetStalledTime() > 1.5f && StallChainLog.Num() < 8)
			{
				auto Describe = [](const ASubNpcCharacter* Npc)
				{
					return FString::Printf(TEXT("%s<%s>[%s%s 차선%.0f 속도%.0f%%%s%s] @(%.0f,%.0f)"),
						*GetNameSafe(Npc), *SubNpcPathDebug::GetRole(Npc).Replace(TEXT("\n"), TEXT(" ")), *GetNameSafe(Npc->GetCurrentPath()),
						Npc->IsReversedOnPath() ? TEXT("/역방향") : TEXT(""),
						Npc->GetLaneOffset(),
						Npc->GetCurrentMoveSpeed() / FMath::Max(1.f, Npc->GetWalkSpeed()) * 100.f,
						Npc->IsWalkingOnPath() ? TEXT("") : TEXT("/길밖"),
						Npc->GetStalledTime() > 0.5f ? *FString::Printf(TEXT("/정지%.1f초"), Npc->GetStalledTime()) : TEXT(""),
						Npc->GetActorLocation().X, Npc->GetActorLocation().Y);
				};
				const FString Other = To ? Describe(To)
					: (Player ? FString::Printf(TEXT("플레이어 @(%.0f,%.0f)"), Player->GetActorLocation().X, Player->GetActorLocation().Y) : TEXT("-"));
				StallChainLog.Add(FString::Printf(TEXT("  %s %s <- %s"), Reason, *Describe(From), *Other));
			}
		}
		if (SubNpcCrowdCVars::DebugBlock == 0 || !From)
		{
			return;
		}
		const AActor* Target = To ? static_cast<const AActor*>(To) : static_cast<const AActor*>(Player);
		if (!Target)
		{
			return;
		}
		const FVector Up(0.f, 0.f, 100.f);
		const FVector Start = From->GetActorLocation() + Up;
		const float Life = AvoidUpdateInterval + 0.05f;
		DrawDebugLine(World, Start, Target->GetActorLocation() + Up, Color, false, Life, 0, 3.f);
		DrawDebugPoint(World, Start, 12.f, Color, false, Life);
#endif
	};

	// ---- 속도 기반 회피 (길 위를 걷는 사람) ----------------------------------------
	//
	// [재설계] 예전 차선 계획은 경로마다 사람들을 차선 좌표로 옮겨 놓고 "내 앞을 막는 한 명"만 찾아
	// 따라가기 → 빈 차선 → 비집기 → 기다리기 단계를 밟았다. 앞사람 한 명만 봐서 옆·비스듬히 오는 사람
	// (스팟을 오가는 사람, 다른 경로, 플레이어)을 못 보고 부딪혔고, 빈 차선이 없으면 무한정 기다렸다
	// (플레이어가 길을 막고 서 있으면 영영 서 있었다).
	// 이제 주변 모두의 실제 위치·속도를 보고, 속도 5단계 × 옆걸음 5단계 후보 중 1.6초 안에 안 부딪히면서 빠르고
	// 덜 비키는 것을 고른다 (RVO류). 마주 오면 오른쪽으로 비키는 쪽, 직전 선택을 유지하는 쪽에 가산점을 준다.
	// 1.5초 넘게 막혀 있으면 길 폭을 조금 넘어서라도 돌아간다. 결과는 감속 요청 + 추월 차선으로 넘긴다
	// (이동 계산은 그대로 - Mass 병렬 이동·성능에 영향 없음).
	// 길로 돌아오는 사람도 기다리지 않고 걸어 들어온다 - 길 위 행인이 보고 비켜 간다.
	struct FAvoidBody
	{
		const ASubNpcCharacter* Npc = nullptr;   // nullptr이면 플레이어
		FVector2D Pos = FVector2D::ZeroVector;
		FVector2D Vel = FVector2D::ZeroVector;
		float Radius = 34.f;
		float Margin = SubNpcCrowdCVars::AvoidMargin;   // 몸 반경 합에 더할 여유
	};
	TArray<FAvoidBody, TInlineAllocator<128>> AvoidBodies;
	for (ASubNpcCharacter* Npc : Nearby)
	{
		Npc->SampleMeasuredVelocity(Now);

		FAvoidBody& B = AvoidBodies.AddDefaulted_GetRef();
		B.Npc = Npc;
		const FVector Loc = Npc->GetActorLocation();
		B.Pos = FVector2D(Loc.X, Loc.Y);
		B.Radius = Npc->GetSimpleCollisionRadius();
		B.Vel = Npc->GetMeasuredVelocity();   // 실제로 움직인 속도 (줄에서 서 있는 사람은 0)
		// 스팟 쪽 사람(줄·칸·이벤트·일행 대기·오가는 사람)은 넉넉히 피한다. 행인끼리처럼 바짝 스치면
		// 겹침 금지가 밀어내 줄이 밀려 보였다.
		if (Npc->GetActivity() == ESubNpcActivity::AtSpot || Npc->IsMovingOffLane())
		{
			B.Margin = SubNpcCrowdCVars::AvoidSpotMargin;
		}
	}
	if (PlayerPawn)
	{
		FAvoidBody& B = AvoidBodies.AddDefaulted_GetRef();
		const FVector Loc = PlayerPawn->GetActorLocation();
		B.Pos = FVector2D(Loc.X, Loc.Y);
		B.Vel = FVector2D(PlayerPawn->GetVelocity());
		B.Radius = PlayerRadius;
		B.Margin = SubNpcCrowdCVars::AvoidPlayerMargin;
	}

	for (ASubNpcCharacter* Npc : Nearby)
	{
		// 길 위를 걷는 사람만 계획한다. 스팟을 오가는 사람·서 있는 사람은 장애물로만 쓴다.
		const ANpcCrowdPath* Path = Npc->GetCurrentPath();
		if (!Path || !Npc->IsWalkingOnPath() || Npc->IsMovingOffLane())
		{
			continue;
		}

		const float Walk = FMath::Max(1.f, Npc->GetWalkSpeed());
		const float Along = Npc->IsReversedOnPath() ? -1.f : 1.f;   // 차선은 스플라인 오른쪽 기준이라 역방향이면 부호가 반대
		const float Lane = Npc->GetLaneOffset();
		const FVector MyDir3 = (Path->GetDirectionAtDistance(Npc->GetPathDistance()) * Along).GetSafeNormal2D();
		const FVector2D MyDir(MyDir3.X, MyDir3.Y);
		const FVector2D MyRight(-MyDir.Y, MyDir.X);
		const FVector2D MyPos(Npc->GetActorLocation());
		const float MyRadius = Npc->GetSimpleCollisionRadius();
		// [좌우 왔다 갔다 방지] 비키는 방향과 길 밖 허용을 행인마다 잠깐 기억한다.
		// 예전에는 매번 새로 정했다. 플레이어가 막고 서 있으면 (1) 길 가장자리에 닿는 순간 "비키던 방향"을 잃어
		// 반대쪽도 같은 점수가 되었고, (2) 1.5초 막혀 길 밖 허용을 받아도 움직이자마자 막힌 시간이 0이 되어
		// 허용이 사라지니 바깥 후보가 막혀 다시 플레이어 쪽으로 돌아왔다. 그래서 왼쪽·오른쪽을 반복하며 못 지나갔다.
		FAvoidMemory& Memory = AvoidMemory.FindOrAdd(Npc);
		if (Npc->GetStalledTime() > SubNpcCrowdCVars::AvoidStallRelaxTime)
		{
			Memory.OverflowUntil = Now + SubNpcCrowdCVars::AvoidOverflowHold;
		}
		const bool bOverflow = Now < Memory.OverflowUntil;
		const float LaneLimit = Path->GetLaneHalfWidth() + (bOverflow ? SubNpcCrowdCVars::AvoidStallOverflow : 0.f);

		// 주변 사람 (같은 일행은 대열 로직이 맡는다)
		TArray<const FAvoidBody*, TInlineAllocator<32>> Near;
		for (const FAvoidBody& B : AvoidBodies)
		{
			if (B.Npc == Npc || FVector2D::DistSquared(B.Pos, MyPos) > FMath::Square(SubNpcCrowdCVars::AvoidScanRadius))
			{
				continue;
			}
			if (B.Npc && Npc->IsSameGroup(B.Npc))
			{
				continue;
			}
			Near.Add(&B);
		}

		// 내가 From에서 V로 걸을 때 가장 먼저 부딪히는 시각. 상대는 Lead초 뒤 위치로 옮겨 놓고 잰다.
		auto MinTimeToCollision = [&](const FVector2D& From, const FVector2D& V, float Lead, const FAvoidBody** OutFirst = nullptr)
		{
			float MinTtc = 1000.f;
			for (const FAvoidBody* B : Near)
			{
				const FVector2D Rel = B->Pos + B->Vel * Lead - From;
				const FVector2D RelVel = V - B->Vel;
				const float R = MyRadius + B->Radius + B->Margin;
				const float C = Rel.SizeSquared() - R * R;
				float T = 1000.f;
				if (C <= 0.f)
				{
					// 닿아 있으면 파고드는 쪽만 막는다. [버그 수정] 예전에는 조금이라도 가까워지면 막아서,
					// 정면에 선 상대를 옆으로 돌아 나가는 걸음(아주 살짝 가까워진다)까지 막혀 그 자리에 갇혔다
					// (자동 측정: 길 가장자리에서 플레이어와 닿은 행인이 15초 넘게 정지).
					const float Closing = FVector2D::DotProduct(Rel.GetSafeNormal(), RelVel);
					T = (Closing > SubNpcCrowdCVars::AvoidContactClosingSpeed) ? 0.f : 1000.f;
				}
				else
				{
					const float A = RelVel.SizeSquared();
					if (A > 1.f)
					{
						const float Bq = -2.f * FVector2D::DotProduct(Rel, RelVel);
						const float Disc = Bq * Bq - 4.f * A * C;
						if (Disc >= 0.f)
						{
							const float T0 = (-Bq - FMath::Sqrt(Disc)) / (2.f * A);
							T = (T0 > 0.f) ? T0 : 1000.f;
						}
					}
				}
				if (T < MinTtc)
				{
					MinTtc = T;
					if (OutFirst)
					{
						*OutFirst = B;
					}
				}
			}
			return MinTtc;
		};

		static const float SpeedSteps[] = { 1.f, 0.75f, 0.5f, 0.25f, 0.f };
		static const float SideSteps[] = { 0.f, 0.5f, -0.5f, 1.f, -1.f };
		float BestCost = TNumericLimits<float>::Max();
		float BestScale = 1.f;
		float BestSide = 0.f;
		const float PrevSide = (Now < Memory.SideUntil) ? Memory.Side : 0.f;   // 진행 방향 기준 +1 오른쪽, -1 왼쪽
		for (const float S : SpeedSteps)
		{
			// 옆걸음 한도는 이동 코드와 같다: 전진의 0.45배, 서 있으면 초속 70cm
			const float MaxLat = FMath::Max(SubNpcCrowdCVars::AvoidSidestepSpeed, 0.45f * S * Walk);
			for (const float Side : SideSteps)
			{
				float Lat = Side * MaxLat;   // 진행 방향 기준 오른쪽 +
				// 0.6초 뒤 차선이 길 밖으로 나가면 가장자리까지만 옮긴다 (가장자리를 따라 걷는 후보로 남긴다).
				// 예전에는 후보를 통째로 버려서, 가장자리에 붙은 사람은 바깥쪽을 못 골라 안쪽으로 되돌아왔다.
				const float FutureLane = Lane + Lat * Along * 0.6f;
				if (FMath::Abs(FutureLane) > LaneLimit && FMath::Abs(FutureLane) > FMath::Abs(Lane))
				{
					const float EdgeLat = (FMath::Clamp(FutureLane, -LaneLimit, LaneLimit) - Lane) / (Along * 0.6f);
					if (Side != 0.f && FMath::Abs(EdgeLat) < 1.f)
					{
						continue;   // 더 나갈 자리가 없다 - 정면 후보와 같다
					}
					Lat = EdgeLat;
				}
				const FVector2D V = MyDir * (S * Walk) + MyRight * Lat;
				const float MinTtc = MinTimeToCollision(MyPos, V, 0.f);
				// [교착 방지] 오래 서 있을수록 느려지는 비용을 키운다 (최대 4배).
				// 그냥 두면 앞이 막혔을 때 "제자리 정지"가 충돌 없는 최선이라 영영 서 있었다
				// (마주 선 둘, 길을 막고 선 플레이어). 서 있을수록 천천히 옆으로 빠지는 후보가 싸진다.
				// [변경] 예전에는 "0.5초 뒤 자리에서 정면이 막혔나"로 같은 일을 하려 했는데, 천천히 옆으로 빠지는
				// 후보까지 똑같이 깎아서 서 있기와 점수가 같아졌다 (자동 측정: 플레이어가 막으면 6/28 실패).
				const float WaitWeight = 1.f + FMath::Min(Npc->GetStalledTime(), 3.f);
				const float Horizon = SubNpcCrowdCVars::AvoidHorizon;
				float Cost = (1.f - S) * WaitWeight                           // 느려지는 것
					+ FMath::Abs(Side) * 0.25f                                  // 옆으로 비키는 것
					+ FMath::Max(0.f, Horizon - MinTtc) / Horizon * 4.f;        // 곧 부딪히는 것
				if (Side < 0.f)
				{
					Cost += 0.05f;   // 비킬 때는 오른쪽 선호 (마주 오는 둘이 같은 쪽으로 피하는 왔다 갔다 방지)
				}
				if (PrevSide != 0.f && Side != 0.f && FMath::Sign(Side) != PrevSide)
				{
					Cost += 1.f;     // 비키던 쪽 유지 (반대쪽은 정말 막혔을 때만)
				}
				if (Cost < BestCost)
				{
					BestCost = Cost;
					BestScale = S;
					BestSide = Lat;
				}
			}
		}
		if (!FMath::IsNearlyZero(BestSide))
		{
			Memory.Side = FMath::Sign(BestSide);
			Memory.SideUntil = Now + SubNpcCrowdCVars::AvoidSideHold;
			if (bOverflow)
			{
				Memory.OverflowUntil = Now + SubNpcCrowdCVars::AvoidOverflowHold;   // 돌아가는 동안은 계속 허용
			}
		}

		if (BestScale < 1.f)
		{
			Npc->RequestSlowDown(BestScale);
		}
		if (!FMath::IsNearlyZero(BestSide))
		{
			const float TargetLane = FMath::Clamp(Lane + BestSide * Along * 0.6f, -LaneLimit, LaneLimit);
			Npc->RequestPassLane(TargetLane, PassUntil);
		}
		if (BestScale < 1.f || !FMath::IsNearlyZero(BestSide))
		{
			// 정면으로 걸으면 가장 먼저 부딪히는 상대 (디버그 선·정체 로그용)
			const FAvoidBody* Blocker = nullptr;
			if (SubNpcCrowdCVars::LogBlock != 0 || SubNpcCrowdCVars::DebugBlock != 0)
			{
				if (MinTimeToCollision(MyPos, MyDir * Walk, 0.f, &Blocker) >= SubNpcCrowdCVars::AvoidHorizon)
				{
					Blocker = nullptr;
				}
			}
			DrawBlockDebug(Npc, Blocker ? Blocker->Npc : nullptr, (Blocker && !Blocker->Npc) ? PlayerPawn : nullptr,
				BestScale <= 0.f ? FColor::Red : FColor::Yellow);
		}
	}

#if !UE_BUILD_SHIPPING
	// crowd.LogBlock: 2초마다 "누가 왜 느려졌고, 누가 가장 많이 막고 있나"를 한 덩어리로 찍는다.
	// 정체 원인을 화면 없이 로그만으로 찾기 위한 것이다.
	{
		static float LastBlockLogTime = -1000.f;
		// [버그 수정] static이라 PIE를 다시 켜도 이전 판의 시각이 남는다. 새 판은 시간이 0부터라 로그가 한동안 안 찍혔다.
		if (Now < LastBlockLogTime)
		{
			LastBlockLogTime = -1000.f;
			StallChainLog.Reset();
		}
		if (SubNpcCrowdCVars::LogBlock != 0 && Now - LastBlockLogTime >= 2.f)
		{
			LastBlockLogTime = Now;

			int32 Walking = 0, Stopped = 0;
			for (const ASubNpcCharacter* Npc : Nearby)
			{
				if (Npc->IsWalkingOnPath())
				{
					++Walking;
					if (Npc->GetStalledTime() > 0.5f)
					{
						++Stopped;
					}
				}
			}

			FString Reasons;
			for (const TPair<FString, int32>& R : ReasonCounts)
			{
				Reasons += FString::Printf(TEXT(" %s=%d"), *R.Key, R.Value);
			}

			BlockerCounts.ValueSort([](int32 A, int32 B) { return A > B; });
			FString Blockers;
			int32 Shown = 0;
			for (const TPair<const AActor*, int32>& B : BlockerCounts)
			{
				if (Shown++ >= 5)
				{
					break;
				}
				FString State = TEXT("플레이어");
				if (const ASubNpcCharacter* Npc = Cast<ASubNpcCharacter>(B.Key))
				{
					State = FString::Printf(TEXT("%s/속도%.0f%%%s%s%s%s"),
						Npc->IsWalkingOnPath() ? TEXT("걷기") : TEXT("멈춤(반응·스팟)"),
						Npc->GetCurrentMoveSpeed() / FMath::Max(1.f, Npc->GetWalkSpeed()) * 100.f,
						Npc->IsRejoiningPath() ? TEXT("/합류중") : TEXT(""),
						Npc->IsWaitingForGroup() ? TEXT("/일행대기") : TEXT(""),
						Npc->GetGroupLeader() ? TEXT("/일행") : TEXT(""),
						Npc->GetStalledTime() > 0.5f ? *FString::Printf(TEXT("/정지%.1f초"), Npc->GetStalledTime()) : TEXT(""));
				}
				Blockers += FString::Printf(TEXT(" [%s x%d %s @%s]"),
					*GetNameSafe(B.Key), B.Value, *State, *B.Key->GetActorLocation().ToCompactString());
			}

			UE_LOG(LogSubNpc, Log, TEXT("[CrowdBlock] 근처 걷는 사람 %d명 중 0.5초 넘게 정지 %d명 | 감속 이유:%s | 많이 막는 상대:%s"),
				Walking, Stopped, *Reasons, *Blockers);
			for (const FString& Line : StallChainLog)
			{
				UE_LOG(LogSubNpc, Log, TEXT("[CrowdStall]%s"), *Line);
			}
			StallChainLog.Reset();
		}
	}
#endif

	const float AvoidSq = FMath::Square(FMath::Min(AvoidDistance, SubNpcCrowdCVars::LaneClearance));

	for (int32 i = 0; i < Nearby.Num(); ++i)
	{
		ASubNpcCharacter* A = Nearby[i];
		const FVector LocA = A->GetActorLocation();

		for (int32 j = i + 1; j < Nearby.Num(); ++j)
		{
			ASubNpcCharacter* B = Nearby[j];

			// 앞뒤 간격과 비켜 가기는 위의 속도 회피가 처리했다 (다른 경로·스팟 사람·플레이어 포함).
			// 여기서는 같은 경로 위에서 그래도 가까이 붙은 몸을 살짝 떼어내는 일만 한다.
			if (A->GetCurrentPath() != B->GetCurrentPath())
			{
				continue;
			}

			// 둘 다 멈춰 있으면 서로 할 일이 없다.
			if (!A->IsWalkingOnPath() && !B->IsWalkingOnPath())
			{
				continue;
			}

			const bool bSameGroup = A->IsSameGroup(B);

			// [의도] 같은 그룹(친구)끼리는 좌우로 밀어내지 않는다.
			// "친구는 붙고 남남은 떨어진다"는 그림이 이 한 줄에서 나온다.
			if (bSameGroup)
			{
				continue;
			}

			// 추월 중인 쌍은 좌우로 밀어내지 않는다. 추월 차선이 이미 간격을 벌리고 있고,
			// 밀어내기까지 겹치면 두 힘이 싸워서 서로 좌우로 흔들린다.
			if (A->IsPassing() || B->IsPassing())
			{
				continue;
			}

			const float DistSq = FVector::DistSquared(LocA, B->GetActorLocation());
			if (DistSq >= AvoidSq || DistSq < KINDA_SMALL_NUMBER)
			{
				continue;
			}

			// 겹친 정도(0~1). 가까울수록 세게 민다.
			const float Overlap = 1.f - (FMath::Sqrt(DistSq) / AvoidDistance);
			const float Push = Overlap * AvoidDistance * AvoidStrength;

			// 차선 오프셋이 작은 쪽을 더 왼쪽으로, 큰 쪽을 더 오른쪽으로 민다.
			// 값이 같으면 인덱스로 갈라서 서로 반대 방향이 되게 한다.
			const float OffA = A->GetLaneOffset();
			const float OffB = B->GetLaneOffset();
			const float Sign = (OffA != OffB) ? (OffA < OffB ? -1.f : 1.f) : ((i % 2 == 0) ? -1.f : 1.f);

			// 멈춘 쪽은 밀지 않는다. 반응이 끝나는 순간 쌓인 양만큼 튀어나간다.
			if (A->IsWalkingOnPath())
			{
				A->AddAvoidanceNudge(Sign * Push);
			}
			if (B->IsWalkingOnPath())
			{
				B->AddAvoidanceNudge(-Sign * Push);
			}

		}
	}
}

void USubNpcDirectorSubsystem::UpdateOffLaneYield()
{
	const float YieldDistSq = FMath::Square(SubNpcCrowdCVars::OffLaneYieldDistance);

	for (ASubNpcCharacter* Mover : ActiveNpcs)
	{
		if (!IsValid(Mover) || Mover->IsPooled())
		{
			continue;
		}
		if (!Mover->IsMovingOffLane())
		{
			Mover->SetOffLaneYield(false);
			continue;
		}

		const FVector MyLoc = Mover->GetActorLocation();
		const FVector MyFwd = Mover->GetActorForwardVector().GetSafeNormal2D();
		bool bYield = false;

		for (const ASubNpcCharacter* Other : ActiveNpcs)
		{
			if (Other == Mover || !IsValid(Other) || Other->IsPooled())
			{
				continue;
			}

			// 길 밖에서 움직이는 사람끼리만 양보한다.
			//   - 서 있는 사람(스팟 칸·줄 자리)에게 양보하면 영영 못 움직인다. 그 자리는 처음부터 떨어져 있다.
			//   - [변경] 길 위를 걷는 사람에게도 양보하지 않는다. 길을 가로지르다 양보하면 길 한가운데 서서
			//     장애물이 되고, 길 위 행인은 또 이 사람을 피하느라 서로 기다리며 뭉쳤다.
			//     길 위 행인이 이 사람을 보고 비켜 가므로(속도 회피의 장애물) 이쪽은 계속 걸어간다.
			if (!Other->IsMovingOffLane())
			{
				continue;
			}

			FVector To = Other->GetActorLocation() - MyLoc;
			To.Z = 0.f;
			const float DistSq = To.SizeSquared();
			if (DistSq > YieldDistSq || DistSq < KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const FVector ToDir = To.GetSafeNormal();

			// 내가 가는 방향 앞에 있을 때만 양보한다.
			if (FVector::DotProduct(MyFwd, ToDir) < SubNpcCrowdCVars::OffLaneYieldFrontDot)
			{
				continue;
			}

			// [버그 수정] 같은 쪽으로 따라 걷는 사람에게는 양보하지 않는다. 줄이 한 칸씩 당겨질 때 모두 같은 쪽으로
			// 동시에 움직이는데, 뒷사람이 앞사람 때문에 서고 그 뒷사람이 또 서서 줄 전체가 1~8초씩 멈췄다
			// (crowd.DebugSpot 2 기록: 스팟 근처 움직임의 42%가 양보 정지, 대부분 줄1·줄2·줄3 이동).
			// 실제로 몸이 닿기 직전이면 겹침 금지(SubNpcBodySeparation)가 뒷사람을 세운다. 엇갈리는 사람에게만 양보한다.
			if (FVector::DotProduct(MyFwd, Other->GetActorForwardVector().GetSafeNormal2D()) > SubNpcCrowdCVars::OffLaneFollowDot)
			{
				continue;
			}

			// 같은 스팟 줄(줄 선 사람·칸으로 들어가는 사람)끼리는 양보하지 않는다 - 줄 간격은 줄 쪽 로직이 맞춘다.
			{
				const ANpcActivitySpot* MySpot = Mover->GetQueuedSpot() ? Mover->GetQueuedSpot() : Mover->GetOccupiedSpot();
				const ANpcActivitySpot* OtherSpot = Other->GetQueuedSpot() ? Other->GetQueuedSpot() : Other->GetOccupiedSpot();
				if (MySpot && MySpot == OtherSpot)
				{
					continue;
				}
			}

			// [출입 흐름] 스팟에서 나가는 사람(길로 돌아가는 중)이 먼저 간다. 나가야 자리·줄이 비기 때문이다.
			// 들어오는 사람(칸·줄·일행 대기로 가는 중)이 서서 기다린다. 예전에는 이름 순서로 아무나 세워서,
			// 나가는 사람이 서면 자리가 안 비고 들어오는 사람도 막혀 서로 길을 막았다.
			const bool bMeLeaving = Mover->IsRejoiningPath();
			const bool bOtherLeaving = Other->IsRejoiningPath();
			if (bMeLeaving && !bOtherLeaving)
			{
				continue;
			}
			// 둘 다 길 밖에서 서로를 향해 오고 있으면 한 명만 양보한다 (둘 다 서면 영영 못 지나간다).
			if (bMeLeaving == bOtherLeaving)
			{
				const FVector OtherFwd = Other->GetActorForwardVector().GetSafeNormal2D();
				const bool bOtherFacesMe = FVector::DotProduct(OtherFwd, -ToDir) >= SubNpcCrowdCVars::OffLaneYieldFrontDot;
				if (bOtherFacesMe && GetTypeHash(Mover->GetFName()) < GetTypeHash(Other->GetFName()))
				{
					continue;
				}
			}

			bYield = true;
			break;
		}

		Mover->SetOffLaneYield(bYield);
	}
}

void USubNpcDirectorSubsystem::UpdateGroupCohesion()
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();

	// 리더별로 가장 많이 뒤처진 일행의 거리를 잰다.
	// 대열 간격(GroupFollowGap)만큼은 원래 떨어져 있는 거리이므로 빼고 센다.
	// 경로 이음매를 넘는 순간에도 맞도록 진행도 대신 실제 거리로 잰다.
	TMap<ASubNpcCharacter*, float> MaxLag;
	for (ASubNpcCharacter* Npc : ActiveNpcs)
	{
		if (!IsValid(Npc))
		{
			continue;
		}
		ASubNpcCharacter* Leader = Npc->GetGroupLeader();
		if (!IsValid(Leader) || Leader->IsPooled())
		{
			continue;
		}
		const float Lag = FVector::Dist2D(Npc->GetActorLocation(), Leader->GetActorLocation()) - Npc->GetGroupFollowGap();
		float& Max = MaxLag.FindOrAdd(Leader, 0.f);
		Max = FMath::Max(Max, Lag);
	}

	for (const TPair<ASubNpcCharacter*, float>& Pair : MaxLag)
	{
		ASubNpcCharacter* Leader = Pair.Key;
		const float Lag = Pair.Value;

		if (!Leader->IsWalkingOnPath())
		{
			Leader->SetWaitingForGroup(false, Now);
			continue;
		}

		bool bWaiting = Leader->IsWaitingForGroup();
		const float WaitedFor = Now - Leader->GetGroupWaitStartTime();

		if (!bWaiting)
		{
			// 많이 벌어졌으면 기다리기 시작한다. 방금 기다리다 포기했으면 한동안은 다시 서지 않는다
			// (계속 못 따라오는 일행 때문에 리더가 서다 가다를 반복하지 않게).
			const bool bCooledDown = WaitedFor > SubNpcCrowdCVars::GroupWaitMaxTime + SubNpcCrowdCVars::GroupWaitCooldown;
			bWaiting = (Lag > SubNpcCrowdCVars::GroupWaitStartLag) && bCooledDown;
		}
		else if (Lag < SubNpcCrowdCVars::GroupWaitResumeLag || WaitedFor > SubNpcCrowdCVars::GroupWaitMaxTime)
		{
			// 다 따라왔으면, 또는 너무 오래 기다렸으면 다시 걷는다.
			bWaiting = false;
		}

		Leader->SetWaitingForGroup(bWaiting, Now);
		if (bWaiting)
		{
			// 완전히 서지 않고 아주 천천히 걸으며 기다린다. 길 한가운데 딱 서 있으면 그 자체가
			// 또 다른 정체가 된다. 느리게 걷는 리더는 뒷사람이 보통 앞사람처럼 추월해 간다.
			Leader->RequestSlowDown(SubNpcCrowdCVars::GroupWaitWalkScale);
		}
	}
}

void USubNpcDirectorSubsystem::StartGroupChatFor(ASubNpcCharacter* Leader, TConstArrayView<ASubNpcCharacter*> Members, int32 GroupSize, float Duration)
{
	if (!Leader || Members.Num() < 2)
	{
		return;
	}

	// [그룹 동작] 무리 크기(대화에 낀 인원이 아니라 무리 전체)로 동작을 고른다.
	// 5명 이상이면 뒤쪽 일행 몇 명은 대화에 끼지 않고 내내 휴대폰을 본다.
	const int32 PhoneCount = (GroupSize >= 5) ? FMath::Min(SubNpcCrowdCVars::GroupPhoneMembers, Members.Num() - 2) : 0;
	const int32 Talkers = Members.Num() - PhoneCount;
	const FName TalkAnim = (GroupSize == 3) ? SubNpcCrowdCVars::GroupPointAnimId : SubNpcCrowdCVars::GroupTalkAnimId;
	const int32 FirstSpeaker = FMath::RandRange(0, Talkers - 1);

	for (int32 i = 0; i < Members.Num(); ++i)
	{
		// 이야기 상대: 리더는 첫 번째 일행을, 일행은 리더를 본다.
		ASubNpcCharacter* Partner = (i == 0) ? Members[1] : Members[0];
		if (i >= Talkers)
		{
			// 혼자 계속 하는 동작 (차례 없음 = 늘 내 차례)
			Members[i]->StartGroupChat(Duration, SubNpcCrowdCVars::GroupPhoneAnimId, 0, 1,
				SubNpcCrowdCVars::GroupChatTurnLength, nullptr);
			continue;
		}
		const int32 Order = (i - FirstSpeaker + Talkers) % Talkers;
		Members[i]->StartGroupChat(Duration, TalkAnim, Order, Talkers, SubNpcCrowdCVars::GroupChatTurnLength,
			Partner, SubNpcCrowdCVars::GroupListenAnimId);
	}
}

void USubNpcDirectorSubsystem::UpdateGroupChats()
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();

	// 리더별로 일행을 모은다. 그룹 정보는 추종자 쪽(GroupLeader)에만 있다.
	TMap<ASubNpcCharacter*, TArray<ASubNpcCharacter*, TInlineAllocator<4>>> Groups;
	for (ASubNpcCharacter* Npc : ActiveNpcs)
	{
		if (!IsValid(Npc))
		{
			continue;
		}
		if (ASubNpcCharacter* Leader = Npc->GetGroupLeader())
		{
			Groups.FindOrAdd(Leader).Add(Npc);
		}
	}

	for (TPair<ASubNpcCharacter*, TArray<ASubNpcCharacter*, TInlineAllocator<4>>>& Pair : Groups)
	{
		ASubNpcCharacter* Leader = Pair.Key;
		if (!IsValid(Leader) || !Leader->IsWalkingOnPath())
		{
			continue;
		}

		// 보이는 거리에 있는 무리만. 멀리서 멈춰 봐야 아무도 못 본다.
		const ESubNpcLodBand Band = Leader->GetLodBand();
		if (Band != ESubNpcLodBand::Near && Band != ESubNpcLodBand::Mid)
		{
			continue;
		}

		// 처음 본 리더는 시각만 정해둔다. 등장하자마자 멈추면 어색하다.
		if (Leader->GetNextGroupChatTime() <= 0.f)
		{
			Leader->SetNextGroupChatTime(Now + FMath::FRandRange(
				SubNpcCrowdCVars::GroupChatIntervalMin, SubNpcCrowdCVars::GroupChatIntervalMax));
			continue;
		}
		if (Now < Leader->GetNextGroupChatTime())
		{
			continue;
		}
		Leader->SetNextGroupChatTime(Now + FMath::FRandRange(
			SubNpcCrowdCVars::GroupChatIntervalMin, SubNpcCrowdCVars::GroupChatIntervalMax));

		if (FMath::FRand() > SubNpcCrowdCVars::GroupChatChance)
		{
			continue;
		}

		// 리더 근처에서 같이 걷고 있는 일행만 낀다.
		TArray<ASubNpcCharacter*, TInlineAllocator<4>> Members;
		Members.Add(Leader);
		const float RadiusSq = FMath::Square(SubNpcCrowdCVars::GroupChatRadius);
		for (ASubNpcCharacter* Member : Pair.Value)
		{
			if (Member->IsWalkingOnPath()
				&& Member->GetCurrentPath() == Leader->GetCurrentPath()
				&& FVector::DistSquared(Member->GetActorLocation(), Leader->GetActorLocation()) < RadiusSq)
			{
				Members.Add(Member);
			}
		}
		if (Members.Num() < 2)
		{
			continue;
		}

		// [변경] 멈추지 않고 걸으면서 이야기한다. 같은 시간 동안, 정해진 순서로 번갈아 손짓한다.
		// 첫 번째로 말할 사람은 매번 바꿔서 늘 리더만 먼저 말하지 않게 한다.
		const float Duration = FMath::FRandRange(
			SubNpcCrowdCVars::GroupChatDurationMin, SubNpcCrowdCVars::GroupChatDurationMax);
		StartGroupChatFor(Leader, Members, Pair.Value.Num() + 1, Duration);

		// 대화가 끝난 뒤부터 다음 간격을 센다. 이야기 도중에 다시 시작되지 않게 한다.
		Leader->SetNextGroupChatTime(Now + Duration + FMath::FRandRange(
			SubNpcCrowdCVars::GroupChatIntervalMin, SubNpcCrowdCVars::GroupChatIntervalMax));
	}
}

void USubNpcDirectorSubsystem::UpdateLodSlice()
{
	FVector ViewerLocation;
	if (!GetViewerLocation(ViewerLocation))
	{
		return;
	}

	if (ActiveNpcs.Num() == 0)
	{
		LodCursor = 0;
		return;
	}

	// 거리 제곱으로 비교해서 제곱근 연산을 피한다. 인원이 많을수록 차이가 난다.
	// 밴드 경계는 개체마다 히스테리시스가 붙으므로 루프 안에서 계산한다.
	const float RecycleSq = FMath::Square(RecycleDistance);

	// [개선] 인원에 비례해 슬라이스를 키운다.
	// 고정 슬라이스면 인원이 늘수록 한 개체의 재검사 주기가 같이 늘어나서,
	// 플레이어가 달릴 때 LOD가 뒤늦게 따라온다.
	int32 SliceSize = LodSliceSize;
	if (LodFullSweepTime > 0.f)
	{
		const int32 SlicesPerSweep = FMath::Max(1, FMath::CeilToInt(LodFullSweepTime / LodUpdateInterval));
		SliceSize = FMath::Max(LodSliceSize, FMath::CeilToInt(static_cast<float>(ActiveNpcs.Num()) / SlicesPerSweep));
	}

	// crowd.LOD 0 이면 전원을 한 번에 Near로 올린다.
	// 슬라이스로 나눠 처리하면 "LOD를 껐는데도 일부는 아직 낮은" 상태가 섞여
	// 측정값이 흐려진다.
	const bool bLodEnabled = (SubNpcCrowdCVars::EnableLod != 0);
	const int32 Count = bLodEnabled ? FMath::Min(SliceSize, ActiveNpcs.Num()) : ActiveNpcs.Num();

	// 회수 대상은 순회 도중에 배열을 건드리지 않도록 모았다가 나중에 처리한다.
	TArray<ASubNpcCharacter*> ToRecycle;

	for (int32 i = 0; i < Count; ++i)
	{
		if (ActiveNpcs.Num() == 0)
		{
			break;
		}

		LodCursor = (LodCursor + 1) % ActiveNpcs.Num();
		ASubNpcCharacter* Npc = ActiveNpcs[LodCursor];

		if (!IsValid(Npc))
		{
			ActiveNpcs.RemoveAtSwap(LodCursor, 1, EAllowShrinking::No);
			continue;
		}

		const float DistSq = FVector::DistSquared(Npc->GetActorLocation(), ViewerLocation);

		// 회수는 "충분히 멀다"만으로는 부족하다. 플레이어가 보고 있는 동안 사라지면
		// 그게 제일 티가 나는 위화감이다. 최근에 그려진 개체는 거리와 무관하게 남겨둔다.
		if (DistSq > RecycleSq && !Npc->WasRecentlyRendered(0.5f))
		{
			ToRecycle.Add(Npc);
			continue;
		}

		if (!bLodEnabled)
		{
			// 측정 모드: 거리와 무관하게 최고 상세도.
			// 회수는 그대로 살려둔다 (안 그러면 맵 밖까지 걸어간 개체가 계속 쌓인다).
			Npc->ApplyMovementTickRate(ESubNpcLodBand::Near);
			Npc->ApplyLodBand(ESubNpcLodBand::Near);
			continue;
		}

		// 거리만으로 정한 밴드. 아래에서 렌더링 여부로 한 단계 더 내려갈 수 있지만,
		// **이동 틱 주기는 이 값으로 고정한다.**
		//
		// [히스테리시스] 경계에 딱 걸친 개체는 미세한 거리 변화만으로 밴드가 널뛴다.
		// 밴드가 바뀔 때마다 강제 LOD·그림자·애니메이션 주기가 전부 다시 적용되므로
		// 그 자체로 눈에 띄는 깜빡임이 된다.
		// 그래서 "지금 있는 밴드에 머무는 쪽"으로 경계를 LodHysteresis만큼 밀어준다.
		// 밖으로 나가려면 조금 더 멀어져야 하고, 들어오려면 조금 더 가까워져야 한다.
		const int32 CurBand = static_cast<int32>(Npc->GetLodBand());
		constexpr float H = SubNpcCrowdCVars::LodHysteresis;

		const float NearLimit = (CurBand <= 0) ? NearDistance + H : NearDistance - H;
		const float MidLimit  = (CurBand <= 1) ? MidDistance  + H : MidDistance  - H;
		const float FarLimit  = (CurBand <= 2) ? FarDistance  + H : FarDistance  - H;

		ESubNpcLodBand DistanceBand;
		if (DistSq <= FMath::Square(NearLimit))      { DistanceBand = ESubNpcLodBand::Near; }
		else if (DistSq <= FMath::Square(MidLimit))  { DistanceBand = ESubNpcLodBand::Mid; }
		else if (DistSq <= FMath::Square(FarLimit))  { DistanceBand = ESubNpcLodBand::Far; }
		else                                         { DistanceBand = ESubNpcLodBand::Culled; }

		// 카메라를 돌리면 뒤쪽 개체가 순간적으로 "안 그려짐" 판정을 받는다.
		// 그때 틱 주기까지 같이 널뛰면 위치 갱신이 뚝뚝 끊겨서 순간이동으로 보인다.
		// 이동은 보이든 말든 매끄러워야 하므로 거리 기준으로만 정한다.
		Npc->ApplyMovementTickRate(DistanceBand);

		ESubNpcLodBand Band = DistanceBand;

		// 실제로 화면에 안 그려지고 있으면 한 단계 낮춘다.
		// WasRecentlyRendered는 렌더 스레드가 채워주는 값이라 공짜에 가깝다.
		//
		// [버그 수정] 예전에는 이 강등이 Culled까지 갈 수 있었다. 그런데 Culled는
		// 메시를 SetVisibility(false)로 숨긴다. 숨은 메시는 영원히 렌더링되지 않으므로
		// WasRecentlyRendered가 계속 false로 남고, 다음 갱신에서 또 Culled로 떨어진다.
		// 한 번 숨으면 거리가 Mid 안쪽으로 들어올 때까지 스스로 못 빠져나오는 자물쇠였다.
		//
		// 3인칭이라 카메라를 돌릴 때마다 등 뒤 개체가 "안 그려짐" 판정을 받는다.
		// 그래서 경계 근처에서 사라졌다 나타났다를 반복했다.
		//
		// 이제 "안 그려진다"를 이유로 한 강등은 Far까지만 허용한다.
		// Culled(=숨김)는 오직 거리로만 판정한다. 거리는 숨겨도 계속 갱신되므로
		// 스스로 되돌아올 수 있다.
		if (bDemoteWhenNotRendered && Band != ESubNpcLodBand::Culled)
		{
			if (!Npc->WasRecentlyRendered(0.5f))
			{
				// uint8 + 1은 int로 승격되므로 FMath::Min의 두 인자 타입이 어긋난다.
				// 양쪽을 int32로 맞춰서 비교한 뒤 다시 enum으로 되돌린다.
				const int32 Demoted = FMath::Min(
					static_cast<int32>(Band) + 1,
					static_cast<int32>(ESubNpcLodBand::Far));
				Band = static_cast<ESubNpcLodBand>(Demoted);
			}
		}

		Npc->ApplyLodBand(Band);
	}

	for (ASubNpcCharacter* Npc : ToRecycle)
	{
		ReturnToPool(Npc);
	}

	// 정원이 줄어들었으면 초과분을 먼 쪽부터 천천히 회수한다.
	// 한 프레임에 몰아서 처리하면 눈앞에서 사람들이 사라지는 게 보인다.
	if (ActiveNpcs.Num() > MaxActiveNpcs)
	{
		ASubNpcCharacter* Farthest = nullptr;
		float FarthestDistSq = -1.f;

		for (ASubNpcCharacter* Npc : ActiveNpcs)
		{
			if (!IsValid(Npc))
			{
				continue;
			}
			const float DistSq = FVector::DistSquared(Npc->GetActorLocation(), ViewerLocation);
			if (DistSq > FarthestDistSq)
			{
				FarthestDistSq = DistSq;
				Farthest = Npc;
			}
		}

		// 정원 초과분도 마찬가지로, 화면에 보이는 개체는 건드리지 않는다.
		if (Farthest && !Farthest->WasRecentlyRendered(0.5f))
		{
			ReturnToPool(Farthest);
		}
	}
}
