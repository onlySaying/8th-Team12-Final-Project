// [skydodo] 2026-09-01 작성 (최종 수정 2026-10-08)
// 요약: 행인(서브 NPC) 구현 - 스플라인 경로 추종, 그룹 이동, 스팟 행동, 부딪힘 반응, 지면 스냅
// Fill out your copyright notice in the Description page of Project Settings.

#include "NPC/SubNpc/SubNpcCharacter.h"

#include "Animation/CharacterAnimationComponent.h"
#include "AnimToTextureDataAsset.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dialogue/DialogueSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "NPC/SubNpc/NpcActivitySpot.h"
#include "NPC/SubNpc/NpcCrowdPath.h"
#include "NPC/SubNpc/SubNpcDirectorSubsystem.h"
#include "NPC/SubNpc/SubNpcLog.h"
#include "NPC/SubNpc/SubNpcProfiling.h"

/** crowd.Mass - 1이면 걷는 행인의 이동을 Mass 프로세서가 병렬로 계산한다 (SubNpcMassProcessors.cpp) */
extern int32 GSubNpcUseMass;

// ---------------------------------------------------------------------------
// 그룹 추종 튜닝값.
//
// [왜 UPROPERTY가 아닌가]
//   ASubNpcCharacter에 데이터 멤버를 추가하면 그 뒤 프로퍼티들의 메모리 오프셋이
//   전부 밀린다. WalkSpeed / bIsMoving / CurrentSpeed가 뒤쪽에 있는데,
//   AnimBP의 Property Access는 컴파일 시점 오프셋을 들고 있어서
//   ABP를 다시 컴파일하기 전까지 엉뚱한 값을 읽는다.
//   실제로 이것 때문에 bIsMoving이 false로 읽혀 걷기 애니메이션이 깨졌다.
//
//   값 조정 빈도가 낮으므로 상수로 둔다. 에디터 노출이 필요해지면
//   반드시 클래스 "맨 끝"에 추가하고 ABP를 다시 컴파일할 것.
// ---------------------------------------------------------------------------
namespace SubNpcGroundSnap
{
	/**
	 * 지면에 붙여 세우기.
	 *
	 * 행인은 CharacterMovement를 안 쓰므로 중력이 없다. 원래 설계는 "스플라인의
	 * 높이가 곧 바닥"이었는데, 그러면 경로를 그릴 때 지형 높이를 일일이 맞춰야 한다.
	 *
	 * 대신 발밑으로 짧은 라인 트레이스를 한 번 쏴서 바닥 높이에 맞춘다.
	 * 경로는 대충 그려도 되고, 계단·경사로 위에서도 제대로 선다.
	 *
	 * 비용은 가까운 개체당 프레임에 트레이스 1회다. 부담되면 crowd.GroundSnap 0.
	 */
	static int32 Enabled = 1;
	static FAutoConsoleVariableRef CVarGroundSnap(
		TEXT("crowd.GroundSnap"),
		Enabled,
		TEXT("행인을 지면에 붙여 세운다. 0이면 스플라인 높이를 그대로 쓴다."),
		ECVF_Cheat);

	/** 발밑에서 위로 이만큼 올린 지점에서 아래로 쏜다 (cm) */
	constexpr float TraceUp = 120.f;

	/** 아래로 이만큼까지 찾는다 (cm). 못 찾으면 스플라인 높이를 그대로 쓴다 */
	constexpr float TraceDown = 400.f;
}

namespace SubNpcPathHandoff
{
	/**
	 * 경로 전환 시 어긋남을 흡수하는 감쇠율 (1/초). 클수록 빨리 따라붙는다.
	 *
	 * [버그 수정] 예전에는 FMath::VInterpTo를 썼다. 그 함수는 내부적으로
	 * Dist * Clamp(DeltaTime * Speed, 0, 1) 만큼 움직인다. 그런데 행인의 틱 간격은
	 * LOD 밴드마다 다르다 - Far는 0.3초다. 0.3 * 5 = 1.5 가 1.0으로 클램프되면
	 * **한 틱에 곧바로 0이 되어 보간이 통째로 사라진다.**
	 * 실제로 이 보간은 Near에서만 동작했고 Far에서는 예전 그대로 순간이동이었다.
	 *
	 * 지수 감쇠는 틱 간격이 아무리 커도 0에 닿지 않는다. 프레임률·틱 주기와 무관하게
	 * 같은 시간 상수를 유지한다.
	 */
	// [튜닝] 4 → 2. 이음매에서 남은 어긋남을 너무 빨리 메워 옆으로 휙 움직이는 것처럼 보였다.
	constexpr float BlendRate = 2.f;

	/**
	 * 다음 경로 입구가 이 경로 끝에서 이 거리(cm) 안에 있으면 그 지점에서 일찍 갈아탄다.
	 * 경로 중간을 지나가는 다른 경로 입구에 걸려 엉뚱한 데서 빠지지 않게 끝 근처로만 한정한다.
	 */
	constexpr float MaxEarlyHandoff = 400.f;

	/** 이보다 크게 벌어진 이음매는 흡수를 포기한다 (cm). 레벨 배치가 잘못된 경우다 */
	constexpr float MaxBlendDistance = 2000.f;

	/** 이보다 작아지면 0으로 끊는다 (cm) */
	constexpr float SnapThreshold = 1.f;

	/** 갈아탈 때 어긋남을 메우는 최대 속도 (걸음 속도 대비). 0.25면 이음매에서 최대 1.25배로 움직인다 */
	constexpr float MaxCatchUpRatio = 0.25f;
}

namespace SubNpcCrowdFlow
{
	/** 감속 배율이 목표값을 따라가는 속도 (1/초). 크면 급브레이크처럼 보인다 */
	constexpr float SpeedScaleInterpRate = 6.f;

	/**
	 * 감속 요청이 원래 속도로 되돌아가는 속도 (1/초).
	 *
	 * [버그 수정] 예전에는 매 틱 PendingSpeedScale = 1.f 로 초기화했다.
	 * 그런데 요청을 넣는 회피 계산은 AvoidUpdateInterval(기본 0.1초)마다만 돈다.
	 * 60 FPS면 6프레임 중 5프레임은 "요청 없음"이 되어 다시 가속해버렸고,
	 * 결국 감속이 거의 적용되지 않았다.
	 * 이제는 천천히 회복시켜서, 다음 갱신까지 감속이 유지되게 한다.
	 */
	constexpr float SpeedScaleRecoverRate = 1.5f;

	/**
	 * 추월할 때 옆 차선으로 옮겨 가는 속도 (1/초). 밀어내기(AvoidInterpSpeed)보다 빨라야 한다.
	 * [튜닝] 5 → 3. 피할 때 옆으로 휙 꺾여서 부자연스러웠다.
	 */
	constexpr float PassLaneInterpRate = 3.f;

	/**
	 * 전진 1cm당 옆으로 움직일 수 있는 최대 거리. 0.45면 약 24도로 비스듬히 빠진다.
	 * 이 값이 없으면 멈춘 채 옆으로만 미끄러지는 게걸음이 된다.
	 * [튜닝] 0.7(약 35도) → 0.45. 사람은 그렇게 급하게 꺾지 않는다.
	 */
	constexpr float MaxSteerRatio = 0.45f;

	/**
	 * 앞이 막혀 멈춰 있어도 옆으로 비켜설 수 있는 속도 (cm/s).
	 *
	 * 옆 이동을 전진 거리에만 묶어두면, 멈춘 사람은 옆으로도 못 움직인다. 그래서 예전에는
	 * 추월하는 사람을 최소 속도로 계속 걷게 했는데, 옆으로 다 빠지기 전에 앞사람 몸을
	 * 그대로 뚫고 지나갔다. 서서 반 걸음 옆으로 비켜서는 정도는 허용하고, 전진은 막는다.
	 */
	constexpr float SidestepSpeed = 70.f;   // [튜닝] 45 → 70. 플레이어가 막았을 때 90cm 비키는 데 2초씩 걸렸다

	/**
	 * 추월하는 동안 길 가장자리(LaneHalfWidth)를 넘어갈 수 있는 거리 (cm).
	 * Director의 SqueezeOverflow와 같거나 커야 비집고 지나가는 차선에 실제로 닿는다.
	 */
	constexpr float PassLaneOverflow = 45.f;

	/** 한 걸음에 몸이 실제로 움직일 수 있는 최대 속도 (걸음 속도 대비). 이음매·따라잡기에서 달려 보이지 않게 한다 */
	constexpr float MaxSpeedRatio = 1.25f;

	/** 목표점(진행도)이 몸보다 이만큼(cm) 넘게 앞서 있으면 진행도를 더 올리지 않는다. 몸이 따라붙을 때까지 기다린다 */
	constexpr float MaxTargetLead = 80.f;
}

namespace SubNpcLightBump
{
	/**
	 * 살짝 부딪혔을 때 멈칫하는 시간 (초). 큰 반응(ReactionDuration)보다 짧아야 한다.
	 * 길면 가볍게 스쳤는데도 한참 서 있어서 길을 막는다.
	 */
	constexpr float Duration = 1.1f;
}

namespace SubNpcBumpDebug
{
	/**
	 * 부딪힘 반응을 화면과 출력 로그에 찍는다. 속도 구간(BumpTiers) 튜닝용.
	 *
	 * 반응이 났을 때: 속도 → 뽑힌 구간 → 반응 → 애니메이션 키 → 재생 성공 여부
	 * 반응이 안 났을 때: 왜 무시됐는지 (쿨다운 / 이미 반응 중 / 해당 구간 없음)
	 *
	 * 튜닝이 끝나면 crowd.DebugBump 0 으로 끈다.
	 */
	static int32 Enabled = 1;
	static FAutoConsoleVariableRef CVarDebugBump(
		TEXT("crowd.DebugBump"),
		Enabled,
		TEXT("행인 부딪힘 반응(속도·구간·애니메이션·재생 결과)을 화면과 로그에 표시한다. 0이면 끈다."),
		ECVF_Cheat);

	/**
	 * 디버그 출력용 반응 이름.
	 * StaticEnum<ESubNpcReaction>()을 쓰면 빌드는 되지만, Rider 코드 분석이 생성 헤더의 API 매크로를
	 * 못 읽어서 "deleted function" 에러로 표시한다. 디버그 문자열 하나라 직접 적는다.
	 */
	static const TCHAR* ReactionToString(ESubNpcReaction Reaction)
	{
		switch (Reaction)
		{
		case ESubNpcReaction::None:      return TEXT("무반응");
		case ESubNpcReaction::Dodge:     return TEXT("피하기");
		case ESubNpcReaction::Angry:     return TEXT("화내기");
		case ESubNpcReaction::Surprised: return TEXT("놀라기");
		case ESubNpcReaction::LightBump: return TEXT("살짝 부딪힘");
		case ESubNpcReaction::Talking:   return TEXT("대화 중");
		case ESubNpcReaction::GroupChat: return TEXT("일행과 대화");
		default:                         return TEXT("?");
		}
	}

	static void Print(const AActor* Npc, const FString& Msg, const FColor& Color)
	{
		if (Enabled == 0)
		{
			return;
		}

		const FString Line = FString::Printf(TEXT("[Bump] %s: %s"), *GetNameSafe(Npc), *Msg);
		UE_LOG(LogSubNpc, Log, TEXT("%s"), *Line);

#if !UE_BUILD_SHIPPING
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 4.f, Color, Line);
		}
#endif
	}
}

namespace SubNpcTalk
{
	/**
	 * 대화 중 멈춰 있는 최대 시간 (초). 대화가 끝나면 그 즉시 풀리므로 보통은 닿지 않는다.
	 * 대화 종료 신호를 놓쳐도 행인이 영원히 서 있지 않게 하는 안전장치다.
	 */
	constexpr float MaxDuration = 60.f;
}

namespace SubNpcWorldBlock
{
	/**
	 * 1이면 행인이 벽·지형을 뚫지 않게 이동 전에 캡슐로 쓸어 본다 (가까운 개체만).
	 * 행인끼리·플레이어와는 막지 않는다 - 서로 막으면 인파가 정체된다.
	 */
	static int32 Enabled = 1;
	static FAutoConsoleVariableRef CVarWorldBlock(
		TEXT("crowd.WorldBlock"),
		Enabled,
		TEXT("행인이 벽·지형에 막히게 한다 (1=켬). 끄면 예전처럼 지형을 통과한다."),
		ECVF_Cheat);

	/** 이 높이(cm)까지의 턱·계단은 막힌 걸로 보지 않는다 */
	constexpr float StepHeight = 45.f;

	/** 면의 법선 Z가 이보다 크면(약 45도 이하 경사) 걸을 수 있는 바닥으로 보고 막지 않는다 */
	constexpr float WalkableNormalZ = 0.7f;

	/** 벽에 막히고 있으면 차선을 이 비율만큼 길 가운데 쪽으로 당긴다 (틱마다). 계속 벽을 긁지 않게 한다 */
	constexpr float BlockedLanePull = 0.85f;
}

namespace SubNpcGroupChat
{
	/** 걸으면서 손짓을 얹을 상체 슬롯. ABP_Passerby의 Slot 노드 이름과 같아야 한다 */
	static const FName UpperBodySlot(TEXT("UpperBody"));

	/** 차례가 끝나 손짓을 거둘 때의 블렌드 시간 (초) */
	constexpr float GestureBlendOut = 0.35f;

	/** 이야기 상대 쪽으로 몸을 돌리는 최대 각도 (진행 방향 기준, 도) */
	constexpr float MaxFaceTurnDegrees = 35.f;
}

namespace SubNpcSpotWalk
{
	/** 설 자리·차선에 이만큼(cm) 안으로 들어오면 도착으로 본다 */
	constexpr float ArriveTolerance = 5.f;

	/** 줄 반응 지연 (초). 앞자리가 비거나 칸이 빈 뒤 이 사이 랜덤 시간이 지나야 움직인다 */
	constexpr float QueueReactMin = 0.3f;
	constexpr float QueueReactMax = 1.0f;

	/** 칸에 들어가기 전, 칸 둘레 이 반경(cm) 안에 다른 사람(나가는 앞사람)이 없어야 한다 */
	constexpr float StandClearRadius = 80.f;

	/**
	 * 스팟에서 차선으로 돌아갈 때의 걸음 속도 배율.
	 * [튜닝] 1.5 → 1.0. 1.5배로 걸으니 행동을 마친 사람만 뛰듯이 빨라 보였다.
	 * 평소 속도로 걷고, 대신 아래 RejoinPathProgressScale로 목표점을 늦춰 따라잡는다.
	 */
	constexpr float RejoinSpeedScale = 1.f;

	/**
	 * 차선으로 돌아가는 동안 경로 진행(목표점이 앞으로 가는 속도)의 배율.
	 * 1보다 작아야 평소 속도로 걷는 몸이 목표점을 따라잡는다.
	 * [튜닝] 0.6 → 0.85. 0.6이면 따라잡은 뒤 0.6배로 걸어서, 길로 나가는 사람만 느리고 발이 미끄러져 보였다.
	 * 합류 지점을 앞쪽으로 잡으므로(RejoinLeadDistance) 0.85로도 비스듬히 따라잡는다.
	 */
	constexpr float RejoinPathProgressScale = 0.85f;

	/** 길 가장자리(LaneHalfWidth)에서 이 거리(cm) 안이면 이미 길 위에 있는 것으로 본다 */
	constexpr float OnPathTolerance = 25.f;

	/**
	 * 스팟·줄에서 길로 돌아갈 때 진행 방향 앞쪽으로 이만큼(cm) 떨어진 곳으로 비스듬히 합류한다.
	 * 들어오는 사람은 뒤쪽(상류)에서 오므로 나가는 흐름과 엇갈리지 않는다 (가게 출입구처럼 한 방향으로 돈다).
	 * 0이면 예전처럼 가장 가까운 길 지점으로 곧장 나가서, 스팟 앞에서 들어오는 사람과 서로 길을 막았다.
	 */
	constexpr float RejoinLeadDistance = 150.f;

	/** 서 있는 사람을 돌아갈 때 그 사람 중심에서 떨어지는 거리 (cm). 캡슐 두 개(약 68cm)보다 커야 몸이 닿지 않는다 */
	constexpr float DetourClearance = 80.f;

	/** 이 반경(cm) 안에 서 있는 사람만 장애물로 본다 */
	constexpr float DetourScanRadius = 500.f;

	/**
	 * 스팟 근처를 걸어갈 때 서 있는 사람(이벤트·줄·일행 대기)을 장애물로 보고 돌아가는 중간 목표를 준다.
	 * [버그 수정] 줄이 이벤트하는 사람 뒤로 서게 되면서, 길에서 줄 끝으로 곧장 걸어가는 선이 이벤트하는 사람·
	 * 줄 선 사람 몸을 관통했다. 겹침 금지가 그 앞에서 세워서, 줄로 안 가고 이벤트하는 사람 앞에 서서 기다리는
	 * 모양이 됐다. 매 틱 다시 계산한다 - 가장 먼저 부딪힐 사람 옆을 경유하고, 지나가면 다음 사람, 다 지나가면 목표로.
	 * 길찾기(내비메시)를 다시 돌리지 않는다: 장애물이 몇 명뿐이고 계속 바뀌어서 매 틱 반응하는 쪽이 싸고 덜 흔들린다.
	 */
	static FVector DetourAroundStanders(TConstArrayView<FVector> Obstacles, const FVector& From, const FVector& To)
	{
		const FVector D(To.X - From.X, To.Y - From.Y, 0.f);
		const float LenSq = D.SizeSquared();
		if (LenSq < 1.f)
		{
			return To;
		}
		float BestT = 2.f;
		FVector BestWaypoint = To;
		for (const FVector& C : Obstacles)
		{
			// 목표 바로 옆에 선 사람(줄에서 내 앞사람)은 장애물이 아니다 - 그 뒤에 서야 한다.
			if (FVector::DistSquared2D(To, C) < FMath::Square(DetourClearance))
			{
				continue;
			}
			const FVector FromToC(C.X - From.X, C.Y - From.Y, 0.f);
			const float T = FVector::DotProduct(FromToC, D) / LenSq;
			if (T <= 0.f || T >= 1.f || T >= BestT)
			{
				continue;
			}
			const FVector Closest = FVector(From.X, From.Y, 0.f) + D * T;
			FVector Away = Closest - FVector(C.X, C.Y, 0.f);
			if (Away.SizeSquared() >= FMath::Square(DetourClearance))
			{
				continue;   // 충분히 떨어져 지나간다
			}
			// 지금 서 있는 쪽(직선 기준 왼쪽/오른쪽)으로 그 사람 옆을 돌아간다.
			if (!Away.Normalize())
			{
				const FVector Perp(-D.Y, D.X, 0.f);
				Away = (FVector::DotProduct(Perp, -FromToC) >= 0.f ? Perp : -Perp).GetSafeNormal();
			}
			BestT = T;
			BestWaypoint = FVector(C.X, C.Y, To.Z) + Away * (DetourClearance + 10.f);
		}
		return BestWaypoint;
	}

	/**
	 * Self가 Target으로 걸어갈 때 서 있는 사람(스팟 칸·줄·일행 대기)을 돌아갈 중간 목표.
	 * 걷는 사람은 여기서 보지 않는다 - 길 밖에서 움직이는 사람은 ChooseSteerDirection이, 길 위 행인은
	 * Director의 속도 회피(길 가는 사람이 비킨다)이 맡는다.
	 */
	static FVector DetourForSpotWalk(const ASubNpcCharacter* Self, const FVector& Target)
	{
		const UWorld* World = Self ? Self->GetWorld() : nullptr;
		const USubNpcDirectorSubsystem* Director = World ? World->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr;
		if (!Director)
		{
			return Target;
		}
		const FVector From = Self->GetActorLocation();
		TArray<FVector, TInlineAllocator<16>> Obstacles;
		for (const ASubNpcCharacter* Other : Director->GetActiveNpcs())
		{
			if (Other == Self || !IsValid(Other) || Other->IsPooled())
			{
				continue;
			}
			const FVector Loc = Other->GetActorLocation();
			const float DistSq = FVector::DistSquared2D(Loc, From);
			const bool bStanding = Other->GetActivity() == ESubNpcActivity::AtSpot && !Other->IsMovingOffLane();
			if (bStanding)
			{
				if (DistSq <= FMath::Square(DetourScanRadius))
				{
					Obstacles.Add(Loc);
				}
				continue;
			}
			// 걷는 사람은 경유점이 아니라 ChooseSteerDirection(속도 기반)이 피한다. 길 위 행인은 그쪽이 비킨다.
		}
		return DetourAroundStanders(Obstacles, From, Target);
	}

	// ---- 속도 기반 회피 (길 밖에서 걷는 사람: 줄·칸·일행 대기로 가기, 길로 돌아가기) ----

	/** 두 사람 몸이 닿는 거리 (cm). 캡슐 두 개 + 약간의 여유 */
	constexpr float SteerBodyDistance = 72.f;

	/** 이 시간(초) 안에 부딪히면 그 방향은 피한다. 길면 일찍·크게 돌고, 짧으면 바짝 붙어 지나간다 */
	constexpr float SteerHorizon = 1.5f;

	/** 이 시간(초) 안에 부딪히는 방향뿐이면 서서 기다린다 */
	constexpr float SteerStopTtc = 0.35f;

	/** 이 반경(cm) 안의 사람만 본다 */
	constexpr float SteerScanRadius = 350.f;

	/** 지금 속도 Vel로 걸을 때 Rel 위치·RelVelOther 속도의 상대와 부딪히기까지의 시간 (초). 안 부딪히면 큰 값 */
	static float TimeToCollision(const FVector2D& Rel, const FVector2D& RelVel)
	{
		// |Rel - RelVel·t| = R 의 가장 작은 양의 해.
		const float R = SteerBodyDistance;
		const float C = Rel.SizeSquared() - R * R;
		if (C <= 0.f)
		{
			// [버그 수정] 이미 몸이 닿아 있을 때 모든 방향을 "지금 부딪힘(0초)"으로 보면 어느 쪽도 못 골라 영영 섰다
			// (기록: 줄 당길 때 뒤에서 온 사람과 겹친 사람이 4초 넘게 그 자리에 정지). 멀어지는 방향은 충돌이 아니다.
			return (FVector2D::DotProduct(Rel, RelVel) > 0.f) ? 0.f : 1000.f;
		}
		const float A = RelVel.SizeSquared();
		if (A < 1.f)
		{
			return 1000.f;
		}
		const float B = -2.f * FVector2D::DotProduct(Rel, RelVel);
		const float Disc = B * B - 4.f * A * C;
		if (Disc < 0.f)
		{
			return 1000.f;
		}
		const float T = (-B - FMath::Sqrt(Disc)) / (2.f * A);
		return T > 0.f ? T : 1000.f;
	}

	/**
	 * 길 밖에서 Target으로 걸어갈 방향을 고른다. 0이면 서서 기다린다.
	 * [개선] 예전 돌아가기(DetourForSpotWalk)는 상대 위치 하나만 봐서, 걷는 사람과 엇갈릴 때 언제·어느 쪽으로
	 * 비킬지 몰라 그대로 직진하다 부딪혔다. 목표 방향과 좌우 ±20·40·60·90도를 후보로 두고, 주변 사람이
	 * 지금 속도로 계속 움직인다고 보고 각 방향의 충돌 시간을 잰다. 1.5초 안에 안 부딪히면서 목표에 가장 가까운
	 * 방향으로 걷는다. 다가오는 사람이 있으면 그 사람 뒤로 비켜 지나간다. 모든 방향이 곧 부딪히면 그때만 선다.
	 * 목표 바로 옆에 선 사람(줄에서 내 앞사람)은 장애물로 치지 않는다 - 그 뒤에 서야 한다.
	 */
	static FVector ChooseSteerDirection(const ASubNpcCharacter* Self, const FVector& Target, float MySpeed, FVector2D& HoldDir)
	{
		const FVector From = Self->GetActorLocation();
		FVector2D Desired(Target.X - From.X, Target.Y - From.Y);
		const float DesiredLen = Desired.Size();
		if (DesiredLen < 1.f)
		{
			return FVector::ZeroVector;
		}
		Desired /= DesiredLen;

		const UWorld* World = Self->GetWorld();
		const USubNpcDirectorSubsystem* Director = World ? World->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr;
		if (!Director)
		{
			return FVector(Desired.X, Desired.Y, 0.f);
		}

		struct FNeighbor { FVector2D Pos; FVector2D Vel; };
		TArray<FNeighbor, TInlineAllocator<24>> Neighbors;
		for (const ASubNpcCharacter* Other : Director->GetActiveNpcs())
		{
			if (Other == Self || !IsValid(Other) || Other->IsPooled())
			{
				continue;
			}
			const FVector Loc = Other->GetActorLocation();
			if (FVector::DistSquared2D(Loc, From) > FMath::Square(SteerScanRadius)
				|| FVector::DistSquared2D(Loc, Target) < FMath::Square(DetourClearance))
			{
				continue;
			}
			// 길 위를 걷는 사람은 그쪽이 비킨다 (Director의 속도 회피이 스팟을 오가는 사람을 장애물로 본다).
			// 여기서까지 피하면 스팟 쪽 동선(줄·칸·일행 대기)이 길 가는 사람마다 휘어 꼬였다.
			if (Other->IsWalkingOnPath() && !Other->IsMovingOffLane())
			{
				continue;
			}
			// 실제로 움직인 속도로 예측한다 (줄에서 서서 기다리는 사람은 0 - 예전엔 걷는 속도로 가정해 헛피했다)
			Neighbors.Add({ FVector2D(Loc.X - From.X, Loc.Y - From.Y), Other->GetMeasuredVelocity() });
		}
		if (Neighbors.Num() == 0)
		{
			HoldDir = Desired;
			return FVector(Desired.X, Desired.Y, 0.f);
		}

		// 목표까지 남은 거리보다 멀리 보면 목표 너머 사람까지 피한다
		const float Horizon = FMath::Min(SteerHorizon, DesiredLen / FMath::Max(MySpeed, 1.f) + 0.2f);
		static const float Angles[] = { 0.f, 20.f, -20.f, 40.f, -40.f, 60.f, -60.f, 90.f, -90.f };
		float BestCost = TNumericLimits<float>::Max();
		FVector2D BestDir = FVector2D::ZeroVector;
		float BestTtc = 0.f;
		for (const float Deg : Angles)
		{
			const FVector2D Dir = Desired.GetRotated(Deg);
			const FVector2D MyVel = Dir * MySpeed;
			float MinTtc = 1000.f;
			for (const FNeighbor& N : Neighbors)
			{
				MinTtc = FMath::Min(MinTtc, TimeToCollision(N.Pos, MyVel - N.Vel));
			}
			// [버그 수정] 목표로 곧장 가는 방향은 도착한 뒤에 닿는 충돌을 보지 않는다 (그 전에 멈춰 선다).
			// 칸 22cm 앞에서 줄1(칸에서 83cm)과 "0.23초 뒤 닿는다"로 판정해 서 버렸고, 줄1은 칸 둘레가
			// 비기를 기다려서 서로 20초 넘게 멈췄다 (crowd.DebugSpot 2 기록: 칸으로 상태 21초).
			if (Deg == 0.f && MinTtc * MySpeed >= DesiredLen)
			{
				MinTtc = 1000.f;
			}
			// 목표에서 벗어나는 각도 + 곧 부딪히는 정도
			float Cost = FMath::Abs(Deg) / 90.f + FMath::Max(0.f, Horizon - MinTtc) * 3.f;
			// [흔들림 방지] 직전에 걷던 방향에서 많이 꺾일수록 벌점. 없으면 비용이 비슷한 좌우 후보를
			// 프레임마다 번갈아 골라 줄로 들어오는 사람이 좌우로 크게 흔들렸다.
			if (!HoldDir.IsNearlyZero())
			{
				Cost += 0.5f * (1.f - FVector2D::DotProduct(Dir, HoldDir));
			}
			if (Cost < BestCost)
			{
				BestCost = Cost;
				BestDir = Dir;
				BestTtc = MinTtc;
			}
		}
		if (BestTtc < SteerStopTtc)
		{
			return FVector::ZeroVector;   // 어느 쪽도 곧 부딪힌다 - 잠깐 선다
		}
		// 방향도 한 번에 꺾지 않고 조금씩 돌린다 (사람은 걸으며 부드럽게 방향을 튼다)
		if (!HoldDir.IsNearlyZero())
		{
			const FVector2D Blended = (HoldDir * 0.75f + BestDir * 0.25f).GetSafeNormal();
			if (!Blended.IsNearlyZero())
			{
				BestDir = Blended;
			}
		}
		HoldDir = BestDir;
		return FVector(BestDir.X, BestDir.Y, 0.f);
	}

	/**
	 * 길 밖에서 Target으로 걸어갈 방향 (0이면 이번 틱은 선다).
	 * 서 있는 사람(이벤트·줄·일행 대기)은 옆을 경유해 돌아가고, 길 밖에서 움직이는 사람은 속도 기반으로 피한다.
	 * 길 위 행인은 피하지 않는다 - 그쪽이 비킨다.
	 */
	static FVector SteerForSpotWalk(const ASubNpcCharacter* Self, const FVector& Target, float Speed, FVector2D& HoldDir)
	{
		return ChooseSteerDirection(Self, DetourForSpotWalk(Self, Target), Speed, HoldDir);
	}
}

namespace SubNpcGroupTuning
{
	/** 이만큼(cm) 벌어지면 최대 속도차를 낸다 */
	constexpr float CatchUpRange = 150.f;

	/** 추종자 속도 배율 범위. 하한이 0보다 커야 뒷걸음질이 없다 */
	// [튜닝] 0.55~1.2 → 0.8~1.15. 일행은 되도록 리더와 같은 속도로 걷는다 (빨라졌다 느려졌다 하면 일행으로 안 보인다).
	// 벌어진 간격은 조금 더 천천히 메운다.
	constexpr float SpeedScaleMin = 0.8f;
	// [튜닝] 1.65 → 1.2 → 1.15. 경로를 갈아탄 직후 리더와의 간격이 크게 잡히면 1.65배로 달려가서
	// 앞사람에 닿아 서고, 풀리면 또 달려가기를 반복했다 (이음매에서 빨라지고 겹치는 원인 중 하나).
	constexpr float SpeedScaleMax = 1.15f;   // 걸음 속도 상한(MaxSpeedRatio 1.25)보다 낮아야 뒤처짐이 쌓이지 않는다

	/** 일행 걸음 속도를 리더 속도에서 이 비율(±)만큼 다르게 준다. 0이면 리더와 같은 속도 */
	constexpr float MemberSpeedJitter = 0.f;

	/** 자기 자리보다 앞서 있을 때의 최소 속도 배율. 0이면 길 한가운데 서서 정체를 만든다 */
	constexpr float AheadSpeedScaleMin = 0.6f;

	/** 일행이 리더 옆 자리로 돌아가는 속도 (1/초) */
	constexpr float LaneFollowRate = 2.f;
}

ASubNpcCharacter::ASubNpcCharacter()
{
	// 행인은 Director가 LOD에 따라 틱 간격을 조절한다.
	// 기본값은 꺼둔 상태로 시작해서, 풀에서 꺼낼 때만 켠다.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// 행인은 길찾기를 하지 않는다. AIController 자체가 필요 없다.
	// (베이스가 BehaviorTree를 돌리려 하지만 BehaviorTreeAsset이 비어 있으면 아무 일도 안 한다)
	AutoPossessAI = EAutoPossessAI::Disabled;
	AIControllerClass = nullptr;

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		// 행인은 이동을 직접 계산해서 위치를 세팅한다 (TickPathFollow 참고).
		// 그래서 캐릭터 무브먼트는 통째로 꺼둔다 - 마리당 중력·바닥 스윕 비용이 전부 사라진다.
		// 컴포넌트 자체는 ACharacter가 요구하므로 지우지 않고 틱만 멈춘다.
		Move->bUseRVOAvoidance = false;
		Move->PrimaryComponentTick.bCanEverTick = false;
		Move->PrimaryComponentTick.bStartWithTickEnabled = false;
	}

	bUseControllerRotationYaw = false;

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		// [측정으로 찾은 낭비] 캡슐의 오버랩 이벤트를 끈다.
		//
		// stat game 프로파일링(90명):
		//   MoveComponent(Primitive)   0.59 ms  (92 calls)
		//   UpdateOverlaps Time        0.23 ms  (92 calls)
		//   PerformOverlapQuery Time   0.19 ms  (92 calls)
		//                              -------
		//                              1.01 ms
		//
		// 호출 수가 행인 수와 정확히 일치한다. 매 틱 SetActorLocationAndRotation을
		// 부를 때마다 캡슐이 오버랩 재계산을 하고 있었다. bSweep은 이미 false라
		// 스윕 비용은 없었지만, 오버랩 갱신은 스윕과 무관하게 돈다.
		//
		// 행인은 오버랩 이벤트를 쓰지 않는다:
		//   - 플레이어와 부딪히는 판정은 NotifyHit(블로킹 히트)이 담당한다
		//   - E키 상호작용은 플레이어 쪽에서 쏘는 쿼리라 응답 채널만 있으면 된다
		// 둘 다 오버랩 이벤트와 무관하므로 꺼도 동작이 바뀌지 않는다.
		Capsule->SetGenerateOverlapEvents(false);

		// 물리 볼륨(물속·중력 구역) 판정도 필요 없다. 이동할 때마다 도는 검사다.
		Capsule->SetShouldUpdatePhysicsVolume(false);
	}

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		// 메타휴먼 기준 이게 제일 큰 절약이다: 화면에 안 보이면 본 계산 자체를 하지 않는다.
		MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;

		// 애니메이션 업데이트 레이트 최적화 - 멀수록 포즈 갱신 빈도를 자동으로 낮춘다.
		MeshComp->bEnableUpdateRateOptimizations = true;

		// [측정으로 찾은 낭비] 메시의 물리 본 갱신을 끈다.
		//
		// 프로파일링(stat anim)에서 UpdateKinematicBonesToAnim이 90명 기준 2.96ms로
		// 잡혔다. 애니메이션 결과를 피직스 애셋의 본에 반영하는 작업인데,
		// 행인은 충돌을 전부 캡슐로 처리하고 메시의 물리 본은 아무도 조회하지 않는다.
		// 플레이어와 부딪히는 판정도, E키 상호작용도 캡슐이 담당한다.
		//
		// 즉 계산해놓고 아무도 안 쓰는 값이라 통째로 버릴 수 있다.
		MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComp->SetGenerateOverlapEvents(false);
		MeshComp->SetSimulatePhysics(false);

		// 물리와 애니메이션을 섞지 않는다 (래그돌 블렌딩을 안 쓰므로).
		MeshComp->bBlendPhysics = false;

		// 본 트랜스폼을 물리로 되돌려 쓰지 않는다.
		MeshComp->bUpdateJointsFromAnimation = false;
	}
}

void ASubNpcCharacter::BeginPlay()
{
	Super::BeginPlay();

	// [버그 수정] 카메라가 행인 캡슐에 걸리지 않게 한다.
	// Pawn 프로필은 Camera 채널을 막는다. 그래서 행인 무리 속으로 들어가면 플레이어
	// 카메라 붐(SpringArm)이 행인에 막혀 카메라가 앞으로 확 당겨지거나 떨렸다.
	// BP에 저장된 콜리전 응답이 생성자 값을 덮어쓰므로 런타임에 건다.
	// 플레이어와의 부딪힘(Pawn 채널)과 E키 상호작용에는 영향이 없다.
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	}

	// 레벨에 직접 배치한 경우에도 Director가 관리하도록 등록한다.
	if (const UWorld* World = GetWorld())
	{
		if (USubNpcDirectorSubsystem* Director = World->GetSubsystem<USubNpcDirectorSubsystem>())
		{
			Director->RegisterNpc(this);
		}
	}
}

void ASubNpcCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 레벨 종료 경로에서도 스팟 점유·타이머가 남지 않도록 같은 정리 함수를 탄다.
	ClearRuntimeState();

	if (const UWorld* World = GetWorld())
	{
		if (USubNpcDirectorSubsystem* Director = World->GetSubsystem<USubNpcDirectorSubsystem>())
		{
			Director->UnregisterNpc(this);
		}
	}

	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// 풀 수명
// ---------------------------------------------------------------------------

void ASubNpcCharacter::ActivateFromPool(const FSubNpcArchetypeRow& InArchetype, ANpcCrowdPath* StartPath, float StartDistance, bool bReversed)
{
	if (!StartPath)
	{
		return;
	}

	// 이전 개체의 상태가 조금이라도 남으면 "가끔 이상하게 행동하는 행인"이 생긴다.
	// 활성화 시점에 무조건 전부 새로 세팅한다.
	ClearRuntimeState();

	Archetype = InArchetype;
	CurrentPath = StartPath;
	PathDistance = FMath::Clamp(StartDistance, 0.f, StartPath->GetPathLength());
	bReversedOnPath = bReversed;
	GroupLeader = nullptr;
	GroupFollowGap = 0.f;

	// 개체별 랜덤화. 같은 메시라도 속도와 체격이 다르면 다른 사람처럼 보인다.
	WalkSpeed = FMath::FRandRange(Archetype.WalkSpeedMin, Archetype.WalkSpeedMax);
	const float Scale = FMath::FRandRange(Archetype.ScaleMin, Archetype.ScaleMax);
	SetActorScale3D(FVector(Scale));

	const float HalfWidth = StartPath->GetLaneHalfWidth();
	LaneOffset = FMath::FRandRange(-HalfWidth, HalfWidth);

	// 다음 경로와 갈아탈 지점을 미리 정해 둔다.
	PlanNextPath();

	// 시작 위치로 순간이동시킨 뒤 보이게 한다 (순서가 반대면 옛 위치에서 한 프레임 깜빡인다).
	// [버그 수정] 등장하는 순간에도 바닥에 붙인다. 예전에는 경로 높이로 세워 두고 첫 걸음에서야 붙여서,
	// 경로가 바닥보다 높은 곳(오르막)에서는 공중에 나타났다가 툭 떨어졌다.
	FVector StartLoc = StartPath->GetLocationAtDistance(PathDistance, LaneOffset);
	SnapToGround(StartLoc);
	const FVector Dir = StartPath->GetDirectionAtDistance(PathDistance) * (bReversedOnPath ? -1.f : 1.f);
	SetActorLocationAndRotation(StartLoc, Dir.Rotation());

	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);

	// [중요] 부모 ABaseNpcCharacter가 생성자에서 bCanEverTick = false로 꺼둔다
	// (메인 NPC는 BehaviorTree로 돌아서 틱이 필요 없기 때문).
	// ASubNpcCharacter 생성자에서 다시 켜지만, 블루프린트가 그 값을 자기 기본값으로
	// 저장해버리면 틱 함수가 아예 등록되지 않아 SetActorTickEnabled가 무력해진다.
	// 행인은 틱이 곧 이동이므로 여기서 등록 여부까지 확실히 보장한다.
	PrimaryActorTick.bCanEverTick = true;
	if (!PrimaryActorTick.IsTickFunctionRegistered())
	{
		PrimaryActorTick.Target = this;
		PrimaryActorTick.RegisterTickFunction(GetLevel());
	}
	SetActorTickEnabled(true);

	CurrentActivity = ESubNpcActivity::Walking;
	SetMovementState(ENpcMovementState::Walk);

	// LOD는 Director가 다음 갱신에서 정한다. 그때까지는 가장 싼 상태로 둔다.
	CurrentLodBand = ESubNpcLodBand::Culled;
	ApplyMovementTickRate(ESubNpcLodBand::Far);
	ApplyLodBand(ESubNpcLodBand::Far);

	// 블루프린트에서 외형(메시/머티리얼/의상)을 갈아끼울 기회.
	OnArchetypeApplied(Archetype);

	NextSpotCheckDistance = PathDistance;
	if (!bSpotDecisionByStateTree)   // StateTree가 맡으면 들를지는 StateTree가 정한다
	{
		RollForActivitySpot();
	}
}

void ASubNpcCharacter::DeactivateToPool()
{
	// 블루프린트가 만들어둔 것(소품, 파티클 등)을 먼저 정리하게 한다.
	OnReturnedToPool();

	ClearRuntimeState();

	CurrentActivity = ESubNpcActivity::Pooled;

	// 숨기고, 충돌을 끄고, 틱을 멈춘다. 이 세 개가 모두 꺼져야 실제로 비용이 0이 된다.
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		// 스켈레탈 메시 컴포넌트는 숨겨도 애니메이션 틱이 살아있을 수 있어서 명시적으로 끈다.
		MeshComp->SetComponentTickEnabled(false);
		MeshComp->bPauseAnims = true;
	}

	// 화면 밖 먼 곳으로 치워둔다. 안 그러면 마지막 위치에서 충돌/오버랩 질의에 계속 걸린다.
	SetActorLocation(FVector(0.f, 0.f, -100000.f));
}

void ASubNpcCharacter::ClearRuntimeState()
{
	// 퇴장 페이드로 바꿔 끼운 머티리얼을 되돌린다. 안 하면 다음에 풀에서 나올 때 투명한 채로 나온다.
	RestoreMaterialsAfterExitFade();

	// 원경 인스턴스 표현을 끝낸다. 인스턴스 칸은 표현 프로세서가 이 사람이 풀로 들어간 걸 보고 돌려받는다.
	// 메시 가시성은 다음 등장 때 ApplyLodBand가 다시 정한다.
	bInstancedRepresentation = false;

	// [누수 방지 핵심] 스팟 점유를 안 풀면 그 스팟은 영영 정원이 찬 상태로 남는다.
	if (OccupiedSpot)
	{
		OccupiedSpot->ReleaseOccupancy(this);
		OccupiedSpot = nullptr;
	}

	// 재생 중인 슬롯 애니메이션 정리 (몽타주 인스턴스가 남지 않게)
	StopNpcAnim(0.f);

	// 외부 액터를 가리키는 참조는 전부 끊는다.
	// 풀에 들어간 NPC가 경로/리더를 계속 붙잡고 있으면 그쪽 액터가 GC되지 않는다.
	CurrentPath = nullptr;
	GroupLeader = nullptr;

	PathDistance = 0.f;
	bReversedOnPath = false;
	LaneOffset = 0.f;
	PathHandoffOffset = FVector::ZeroVector;
	PendingSpeedScale = 1.f;
	CurrentSpeedScale = 1.f;
	PassLaneTarget = 0.f;
	PassLaneUntil = -1.f;
	GroupFollowGap = 0.f;
	ActivityEndTime = 0.f;
	DodgeTargetOffset = 0.f;

	// [중요] 부딪힘 기록도 반드시 지운다.
	// 이 액터는 풀에서 재사용되므로, 안 지우면 방금 회수된 개체의 기록을 물려받은 채
	// 새 행인이 등장해서 "방금 부딪혀놓고..." 같은 말을 한다.
	// 플레이어 입장에서는 처음 보는 사람이 없던 일을 따지는 셈이라 바로 티가 난다.
	LastBumpTime = -1000.f;
	LastReactionTime = -1000.f;

	// [누수 방지] 새로 추가된 런타임 상태도 반드시 여기서 지운다.
	// 하나라도 빠지면 재사용된 개체가 이전 개체의 행동을 물려받는다.
	CurrentReaction = ESubNpcReaction::None;
	bIsMoving = false;
	CurrentSpeed = 0.f;
	CurrentSpotOption = FNpcActivityOption();
	CurrentSpotStep = -1;
	bResumeSpotAfterReaction = false;
	bApproachingSpot = false;
	bRejoiningPath = false;
	NextGroupChatTime = 0.f;
	StalledTime = 0.f;
	bWaitingForGroup = false;
	bHasGroupFollowers = false;
	GroupLaneOffset = 0.f;
	bOffLaneYield = false;
	PlannedNextPath = nullptr;
	bPlannedNextReversed = false;
	PlannedHandoffDistance = -1.f;
	RolledSpots.Reset();
	GroupWaitStartTime = -1000.f;
	StopGroupChat();
	if (ANpcActivitySpot* Queued = QueuedSpot.Get())
	{
		Queued->LeaveQueue(this);
	}
	QueuedSpot = nullptr;
	bQueueMoving = false;
	PendingAvoidNudge = 0.f;
	bPendingRecycle = false;
	NextSpotCheckDistance = 0.f;
	bGroupWaiting = false;
	bGroupWaitMoving = false;
	GroupWaitSpot = nullptr;
	GroupSpotVisitor = nullptr;
	GroupVisitDeadline = 0.f;
}

// ---------------------------------------------------------------------------
// LOD
// ---------------------------------------------------------------------------

void ASubNpcCharacter::ApplyMovementTickRate(ESubNpcLodBand DistanceBand)
{
	// 거리만 본다. 렌더링 여부로 내려간 밴드를 쓰면 카메라 회전마다 주기가 널뛴다.
	float Interval = 0.f;
	switch (DistanceBand)
	{
	case ESubNpcLodBand::Near:	Interval = 0.f;					break;
	case ESubNpcLodBand::Mid:	Interval = MidTickInterval;		break;
	default:					Interval = FarTickInterval;		break;
	}

	// 사라지는 중에는 매 프레임 갱신한다 (BeginExitFade 참고).
	if (bExitFading)
	{
		Interval = 0.f;
	}

	// 같은 값이면 건드리지 않는다. 틱 함수를 매번 다시 등록시킬 이유가 없다.
	if (!FMath::IsNearlyEqual(PrimaryActorTick.TickInterval, Interval))
	{
		SetActorTickInterval(Interval);
	}
}

void ASubNpcCharacter::ApplyLodBand(ESubNpcLodBand NewBand)
{
	if (NewBand == CurrentLodBand)
	{
		// 같은 값을 매번 적용하면 컴포넌트 갱신 비용이 오히려 더 든다.
		return;
	}

	CurrentLodBand = NewBand;

	USkeletalMeshComponent* MeshComp = GetMesh();

	switch (NewBand)
	{
	case ESubNpcLodBand::Near:
		if (MeshComp)
		{
			MeshComp->SetComponentTickEnabled(true);
			MeshComp->bPauseAnims = false;
			MeshComp->SetForcedLOD(0);                       // 0 = 자동 LOD에 맡김
			MeshComp->SetCastShadow(true);
			MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
			ApplyAnimUpdateRate(MeshComp, 1);   // 매 프레임
		}
		break;

	case ESubNpcLodBand::Mid:
		if (MeshComp)
		{
			MeshComp->SetComponentTickEnabled(true);
			MeshComp->bPauseAnims = false;
			MeshComp->SetForcedLOD(MidForcedLod + 1);        // SetForcedLOD는 1-based (0은 자동)
			// 그림자는 메타휴먼에서 가장 비싼 항목 중 하나다. 중간 거리부터 끈다.
			MeshComp->SetCastShadow(false);
			ApplyAnimUpdateRate(MeshComp, MidAnimFrameSkip);
		}
		break;

	case ESubNpcLodBand::Far:
		if (MeshComp)
		{
			MeshComp->SetComponentTickEnabled(true);
			MeshComp->bPauseAnims = false;
			MeshComp->SetForcedLOD(FarForcedLod + 1);
			MeshComp->SetCastShadow(false);
			ApplyAnimUpdateRate(MeshComp, FarAnimFrameSkip);
		}
		break;

	case ESubNpcLodBand::Culled:
	default:
		if (MeshComp)
		{
			// 렌더링도 애니메이션도 멈춘다. 위치 갱신(액터 틱)만 살려두면
			// 다시 보이게 됐을 때 "그동안 걸어온 것처럼" 자연스럽게 이어진다.
			MeshComp->SetComponentTickEnabled(false);
			MeshComp->bPauseAnims = true;
			MeshComp->SetCastShadow(false);
			MeshComp->SetVisibility(false, /*bPropagateToChildren=*/true);
		}
		break;
	}

	// Culled에서 빠져나올 때 다시 보이게 한다.
	if (MeshComp && NewBand != ESubNpcLodBand::Culled)
	{
		MeshComp->SetVisibility(true, /*bPropagateToChildren=*/true);
	}

	// 인스턴스드 메시로 그려지는 중이면 밴드가 바뀌어도 스켈레탈 메시는 계속 숨긴다.
	if (bInstancedRepresentation && MeshComp)
	{
		MeshComp->SetVisibility(false, /*bPropagateToChildren=*/true);
		MeshComp->SetComponentTickEnabled(false);
		MeshComp->bPauseAnims = true;
	}
}

UAnimToTextureDataAsset* ASubNpcCharacter::GetFarCrowdAnimData() const
{
	if (FarCrowdAnimData.IsNull())
	{
		return nullptr;
	}
	// 매 프레임 불리므로 이미 로드돼 있으면 바로 돌려준다
	UAnimToTextureDataAsset* Loaded = FarCrowdAnimData.Get();
	return Loaded ? Loaded : FarCrowdAnimData.LoadSynchronous();
}

void ASubNpcCharacter::SetInstancedRepresentation(bool bInstanced)
{
	if (bInstancedRepresentation == bInstanced)
	{
		return;
	}
	bInstancedRepresentation = bInstanced;

	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp)
	{
		return;
	}

	if (bInstanced)
	{
		// 스켈레탈 메시를 숨기고 애니메이션 계산(포즈 평가·스키닝)을 멈춘다. 이게 원경 표현으로 아끼는 비용이다.
		MeshComp->SetVisibility(false, /*bPropagateToChildren=*/true);
		MeshComp->SetComponentTickEnabled(false);
		MeshComp->bPauseAnims = true;
	}
	else
	{
		// 지금 LOD 밴드 설정을 다시 적용해서 보이게 되돌린다 (같은 밴드면 ApplyLodBand가 건너뛰므로 잠시 바꿔 둔다).
		const ESubNpcLodBand Band = CurrentLodBand;
		CurrentLodBand = (Band == ESubNpcLodBand::Near) ? ESubNpcLodBand::Mid : ESubNpcLodBand::Near;
		ApplyLodBand(Band);
	}
}

void ASubNpcCharacter::ApplyAnimUpdateRate(USkeletalMeshComponent* MeshComp, int32 FrameSkip)
{
	// [측정으로 찾은 개선] 거리 밴드마다 포즈 갱신 빈도를 다르게 준다.
	//
	// 예전에는 LOD 밴드가 틱 간격·강제 메시LOD·그림자만 바꿨다. 그런데 프로파일링해보니
	// 군중 비용의 대부분이 애니메이션이었고, 그 부분은 밴드와 무관하게 매 프레임 돌고 있었다.
	// LOD를 껐다 켜도 게임 스레드가 거의 안 변하던 이유가 이것이다.
	//
	// UpdateRateOptimizations는 "N프레임에 한 번만 평가하고 사이는 보간"하는 기능이다.
	// 멀리 있는 개체는 4~8프레임에 한 번만 평가해도 눈에 안 띈다.
	if (!MeshComp)
	{
		return;
	}

	FAnimUpdateRateParameters* Params = MeshComp->AnimUpdateRateParams;
	if (!Params)
	{
		return;
	}

	// 엔진이 거리로 자동 계산하는 것을 끄고 우리가 직접 정한다.
	// Director가 이미 거리 판정을 하고 있으므로 두 번 할 이유가 없다.
	Params->bShouldUseLodMap = false;
	Params->UpdateRate = FMath::Max(1, FrameSkip);
	Params->EvaluationRate = FMath::Max(1, FrameSkip);

	// 건너뛴 프레임을 보간해서 채운다. 안 켜면 뚝뚝 끊겨 보인다.
	Params->bInterpolateSkippedFrames = (FrameSkip > 1);
}

// ---------------------------------------------------------------------------
// 이동
// ---------------------------------------------------------------------------

void ASubNpcCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (CurrentActivity == ESubNpcActivity::Pooled)
	{
		return;
	}

	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	// 퇴장구에서 사라지는 중이면 그것만 한다 (경로·스팟·반응 모두 끝났다).
	if (bExitFading)
	{
		bIsMoving = true;
		CurrentSpeed = WalkSpeed;
		TickExitFade(DeltaTime);
		return;
	}

	// 걸으면서 일행과 이야기하는 중이면 차례에 맞춰 상체 손짓을 켜고 끈다.
	TickGroupChat(Now);

	// ABP가 읽을 값을 갱신한다.
	// 프로퍼티라 Property Access로 스레드 안전하게 읽힌다.
	bIsMoving = (CurrentActivity == ESubNpcActivity::Walking) ||
		(CurrentActivity == ESubNpcActivity::AtSpot && (bApproachingSpot || bQueueMoving || bGroupWaitMoving)) ||
		(CurrentActivity == ESubNpcActivity::Reacting && CurrentReaction == ESubNpcReaction::Dodge);
	// 감속까지 반영해야 걷기 애니메이션 재생 속도가 실제 이동과 맞는다.
	CurrentSpeed = bIsMoving
		? WalkSpeed * ((CurrentActivity == ESubNpcActivity::AtSpot) ? 1.f : CurrentSpeedScale)
		: 0.f;

	// [버그 수정] 길로 돌아오는 동안은 몸이 감속과 상관없이 제 속도로 걷는다.
	// 애니메이션 속도를 감속 값에 맞추면, 몸은 움직이는데 서 있는 자세로 미끄러졌다.
	// 다른 사람에게 양보하며 서 있는 중이면 실제로 서 있으므로 0이다.
	if (CurrentActivity == ESubNpcActivity::Walking && bRejoiningPath)
	{
		bIsMoving = !bOffLaneYield;
		CurrentSpeed = bOffLaneYield ? 0.f : WalkSpeed * SubNpcSpotWalk::RejoinSpeedScale;
	}
	// 스팟·줄로 걸어가다 양보하며 서 있으면 서 있는 자세로 보여야 한다.
	else if (CurrentActivity == ESubNpcActivity::AtSpot && bOffLaneYield && (bApproachingSpot || bQueueMoving || bGroupWaitMoving))
	{
		bIsMoving = false;
		CurrentSpeed = 0.f;
	}

	switch (CurrentActivity)
	{
	case ESubNpcActivity::AtSpot:
		if (bGroupWaiting)
		{
			TickGroupWait(DeltaTime, Now);
		}
		else if (QueuedSpot.IsValid() && !OccupiedSpot)
		{
			TickSpotQueue(DeltaTime, Now);
		}
		else if (bApproachingSpot)
		{
			TickApproachSpot(DeltaTime);
		}
		else if (Now >= ActivityEndTime)
		{
			// 한 동작으로 끝내지 않고 다음 단계가 있으면 이어서 재생한다.
			AdvanceSpotStep();
		}
		return;

	case ESubNpcActivity::Reacting:
		if (CurrentReaction == ESubNpcReaction::Talking)
		{
			// 대화창이 닫히면 다시 걷는다. 시간은 안전장치일 뿐, 보통은 대화 종료로 끝난다.
			const UDialogueSubsystem* Dialogue = World ? World->GetSubsystem<UDialogueSubsystem>() : nullptr;
			if (!Dialogue || !Dialogue->IsDialogueActive() || Now >= ActivityEndTime)
			{
				EndReaction();
			}
		}
		else if (Now >= ActivityEndTime)
		{
			EndReaction();
		}
		else if (CurrentReaction == ESubNpcReaction::Dodge)
		{
			// 피하기는 걸으면서 비킨다. 차선 오프셋을 목표치로 부드럽게 민다.
			LaneOffset = FMath::FInterpTo(LaneOffset, DodgeTargetOffset, DeltaTime, 4.f);
			TickPathFollow(DeltaTime);
		}
		// [버그 수정] 화내기·놀라기는 "멈춰서 플레이어를 쳐다본다"가 연출인데,
		// 예전에는 여기서도 TickPathFollow를 불러 계속 전진했다.
		// 게다가 FaceTowards로 돌려둔 회전을 진행 방향으로 매 틱 덮어써서,
		// 화내는 애니메이션이 나오는 동안 옆을 보며 걸어가는 그림이 됐다.
		return;

	case ESubNpcActivity::Walking:
	default:
		// StateTree(걷기 태스크)가 요청한 스팟 판정. 스팟 점유·애니메이션을 바꾸므로 게임 스레드인 여기서 한다.
		if (bSpotRollRequested)
		{
			bSpotRollRequested = false;
			if (bSpotDecisionByStateTree)
			{
				RollForActivitySpot();
				if (CurrentActivity != ESubNpcActivity::Walking)
				{
					return;
				}
			}
		}
		// Mass가 맡고 있으면 이동은 UMassSubNpcPathFollowProcessor가 여러 명을 병렬로 계산한다.
		if (!IsMovementDrivenByMass())
		{
			TickPathFollow(DeltaTime);
		}
		return;
	}
}

void ASubNpcCharacter::PushApart(const FVector& WorldPush)
{
	const FVector Push(WorldPush.X, WorldPush.Y, 0.f);
	if (Push.IsNearlyZero())
	{
		return;
	}

	// 길 위를 걷는 중이면 목표점(진행도·차선)을 옮기고, 몸은 목표점이 실제로 옮겨진 만큼만 옮긴다.
	// [버그 수정] 몸을 민 만큼 그대로 옮기면, 길 가장자리에서 차선이 길 폭에 잘린 만큼 몸과 목표점이 어긋나
	// 다음 걸음에 목표점으로 끌려 돌아갔다 (서 있는 일행이 290cm/s로 움직이고 다시 겹치기를 반복).
	// 진행도는 스플라인 정방향 기준이라 역방향으로 걷는 사람도 같은 식이다.
	FVector Applied = Push;
	const bool bOnPath = IsWalkingOnPath() && !bRejoiningPath;
	if (bOnPath)
	{
		const FVector OldTarget = CurrentPath->GetLocationAtDistance(PathDistance, LaneOffset);
		const FVector Base = CurrentPath->GetLocationAtDistance(PathDistance, 0.f);
		const FVector Right = (CurrentPath->GetLocationAtDistance(PathDistance, 1.f) - Base).GetSafeNormal2D();
		const FVector Tangent = CurrentPath->GetDirectionAtDistance(PathDistance).GetSafeNormal2D();
		const float HalfWidth = CurrentPath->GetLaneHalfWidth();
		LaneOffset = FMath::Clamp(LaneOffset + FVector::DotProduct(Push, Right), -HalfWidth, HalfWidth);
		PathDistance = FMath::Clamp(PathDistance + FVector::DotProduct(Push, Tangent), 0.f, CurrentPath->GetPathLength());
		const FVector NewTarget = CurrentPath->GetLocationAtDistance(PathDistance, LaneOffset);
		Applied = FVector(NewTarget.X - OldTarget.X, NewTarget.Y - OldTarget.Y, 0.f).GetClampedToMaxSize2D(Push.Size2D());
	}

	// 가까운(Near) 행인은 목표점만 옮긴다. 매 프레임 걸음을 계산하므로 다음 걸음에 따라오고, 걸음 상한(1.25배)이
	// 전체 속도를 묶어 준다 - 몸까지 옮기면 옆걸음(추월·회피)에 더해져 1.4~1.5배로 튀었다.
	// 중간 거리 행인은 0.1초마다만 걸음을 계산하므로 몸도 바로 옮긴다 (안 그러면 그동안 겹친 채로 있다).
	// 길 밖에서 움직이는 사람(스팟·줄·합류)은 목표점이 따로 없으니 몸을 옮긴다.
	if (!Applied.IsNearlyZero() && (!bOnPath || CurrentLodBand != ESubNpcLodBand::Near))
	{
		SetActorLocation(GetActorLocation() + Applied);
	}
}

bool ASubNpcCharacter::IsPassing() const
{
	const UWorld* World = GetWorld();
	return World && World->GetTimeSeconds() < PassLaneUntil;
}

// [Mass] 예전 TickPathFollow의 본체. 월드를 바꾸는 일(위치 설정·경로 갈아타기·스팟 판정)은
// Out에 적어 두고 ApplyPathStep에서 한다. 그 덕에 이 함수는 워커 스레드에서 여러 명을 동시에 돌릴 수 있다.
//   - 바꾸는 것: 자기 멤버(진행도·차선·속도 배율 등)뿐
//   - 읽는 것: 경로 스플라인, 일행 리더의 진행도·차선, 대화 상대 위치 (읽기만 한다)
//   - 트레이스: 지면 스냅·벽 스윕 (물리 장면 질의는 어느 스레드에서나 읽기 가능)
void ASubNpcCharacter::ComputePathStep(float DeltaTime, FSubNpcPathStep& Out)
{
	SUBNPC_PROFILE_SCOPE(PathFollow);

	Out = FSubNpcPathStep();
	if (!CurrentPath)
	{
		return;
	}

	// 앞사람 때문에 걸린 제동을 반영한다. 목표 배율로 곧장 튀면 급브레이크로 보이므로
	// 부드럽게 따라가고, 요청값은 매 틱 1로 되돌려 Director가 다시 채우게 한다.
	// [버그 수정] 멈추라는 요청(0)은 보간하지 않고 바로 따른다.
	// 보간하면 서서히 감속하는 동안 앞으로 미끄러져, 줄 선 사람끼리 몸이 겹쳤다.
	// 멈추는 거리(FollowStopDistance)는 캡슐 지름보다 크게 잡혀 있으므로 바로 서면 겹치지 않는다.
	CurrentSpeedScale = (PendingSpeedScale <= KINDA_SMALL_NUMBER)
		? 0.f
		: FMath::FInterpTo(CurrentSpeedScale, PendingSpeedScale, DeltaTime, SubNpcCrowdFlow::SpeedScaleInterpRate);

	// 앞이 막혀 거의 멈춘 시간을 잰다. Director가 오래 막힌 사람부터 정체를 풀어준다.
	// [버그 수정] 한번 막혀 선 사람은 정체 해소로 천천히 비집고 나가는 동안(배율 0.3)에도 계속 "막힘"으로 센다.
	// 예전에는 0.08만 넘으면 0으로 돌아가서, 서 있다 → 0.3으로 한 발 → 다시 0부터 세며 정지를
	// 반복했다. 사람들이 뚝뚝 끊기며 덜컥거리는 걸음으로 보였다.
	const bool bStillStalled = (CurrentSpeedScale < 0.08f) || (StalledTime > 0.f && CurrentSpeedScale < 0.35f);
	StalledTime = bStillStalled ? StalledTime + DeltaTime : 0.f;

	// 요청은 회피 갱신 주기마다만 들어온다. 매 틱 1로 되돌리면 그 사이 프레임에서
	// 다시 가속해버리므로, 천천히 회복시켜 다음 갱신까지 감속을 유지한다.
	PendingSpeedScale = FMath::Min(
		1.f, PendingSpeedScale + DeltaTime * SubNpcCrowdFlow::SpeedScaleRecoverRate);

	// 그룹으로 다니는 경우 앞사람의 진행도에서 간격만큼 뒤를 따라간다.
	// 각자 독립적으로 걸으면 신호에 걸릴 때마다 대열이 흩어진다.
	// 진행 부호. 역방향이면 PathDistance가 줄어드는 쪽이 "앞"이다.
	const float Along = bReversedOnPath ? -1.f : 1.f;
	const float PathDistanceBefore = PathDistance;

	if (ASubNpcCharacter* Leader = GroupLeader.Get())
	{
		if (Leader->CurrentPath == CurrentPath && !Leader->IsPooled())
		{
			// [버그 수정] 리더 위치를 그대로 대입하지 않는다.
			//
			// 예전에는 PathDistance = Leader->PathDistance - Gap 으로 매 틱 덮어썼다.
			// 그래서 세 가지가 눈에 보이는 결함으로 나타났다.
			//   1) 리더가 스팟에 들어가 멈추면 추종자도 그 자리에 얼어붙는다
			//   2) 리더가 막다른 길에서 방향을 돌리면 추종자가 순간이동으로 뒤로 간다
			//   3) 리더의 회피 흔들림이 추종자에게 그대로 복사돼 대열이 떤다
			//
			// 대신 추종자도 자기 속도로 걷되, 벌어진 만큼 속도만 조절한다.
			// 위치를 직접 건드리지 않으므로 뒷걸음질도 순간이동도 없다.
			const float Behind = bReversedOnPath ? GroupFollowGap : -GroupFollowGap;
			const float Desired = Leader->PathDistance + Behind;

			// 진행 방향 기준 오차. 양수면 뒤처진 상태다.
			const float Error = (Desired - PathDistance) * Along;

			// 내 자리보다 앞서 있으면(Error < 0) 천천히 걸으며 리더를 기다린다.
			// [튜닝] 0(완전 정지)으로 했더니 리더가 멈출 때마다 일행이 길 한가운데 서서 정체를 만들었다.
			const float SpeedScale = FMath::Clamp(
				1.f + Error / SubNpcGroupTuning::CatchUpRange,
				(Error < 0.f) ? SubNpcGroupTuning::AheadSpeedScaleMin : SubNpcGroupTuning::SpeedScaleMin,
				SubNpcGroupTuning::SpeedScaleMax);

			PathDistance += WalkSpeed * SpeedScale * CurrentSpeedScale * DeltaTime * Along;
		}
		else if (!Leader->IsPooled() && Leader->CurrentPath == PlannedNextPath.Get())
		{
			// [그룹핑] 리더가 이음매를 먼저 넘어 내가 갈 다음 경로에 들어섰다. 해산하지 않고 따라간다.
			// 예전에는 여기서 해산해서, 이음매를 하나 지날 때마다 일행이 남남이 됐다.
			// 경로가 달라 진행도로 간격을 잴 수 없으니 이음매를 넘을 때까지는 평소 속도로 걷는다.
			PathDistance += WalkSpeed * CurrentSpeedScale * DeltaTime * Along;
		}
		else
		{
			// 리더가 회수됐거나 엉뚱한 경로로 갔으면 그룹을 해산하고 혼자 걷는다.
			GroupLeader = nullptr;
			PathDistance += WalkSpeed * CurrentSpeedScale * DeltaTime * Along;
		}
	}
	else
	{
		PathDistance += WalkSpeed * CurrentSpeedScale * DeltaTime * Along;
	}

	// [정체 해소] 굽은 곳에서도 차선과 상관없이 실제 걸음 속도가 같게 진행도를 보정한다.
	// 예전에는 중심선 진행도를 걸음 속도로 올려서, 직각에 가까운 모서리 안쪽 차선 사람은
	// 실제로는 거의 서 있었고(바깥쪽은 뛰듯이 빨랐다) 그 뒤로 줄줄이 막혔다
	// (로그: B 끝 모서리와 C의 굽은 곳에 정지 인원이 몰림).
	PathDistance = PathDistanceBefore
		+ (PathDistance - PathDistanceBefore) * CurrentPath->GetLaneProgressScale(PathDistanceBefore, LaneOffset, Along);

	// 스팟·줄에서 길로 돌아오는 동안은 진행을 늦춘다. 몸은 평소 속도로 걸으면서
	// 늦게 가는 목표점을 따라잡으므로 서두르는 것처럼 보이지 않는다.
	if (bRejoiningPath)
	{
		PathDistance = PathDistanceBefore + (PathDistance - PathDistanceBefore) * SubNpcSpotWalk::RejoinPathProgressScale;
	}

	const float HalfWidth = CurrentPath->GetLaneHalfWidth();
	const float LaneBefore = LaneOffset;
	const bool bPassing = IsPassing();

	if (bPassing)
	{
		// 막힌 앞사람을 추월하는 중이면 옆 차선으로 옮겨 간다.
		// 추월 중에는 이웃의 밀어내기를 받지 않는다. 두 힘이 서로 싸우면 좌우로 떤다.
		LaneOffset = FMath::FInterpTo(LaneOffset, PassLaneTarget, DeltaTime, SubNpcCrowdFlow::PassLaneInterpRate);
	}
	else if (!FMath::IsNearlyZero(PendingAvoidNudge))
	{
		// 이웃이 밀어낸 양을 반영한다. Director가 이번 프레임에 누적해 둔 값이다.
		// 즉시 더하지 않고 목표를 향해 보간하는 이유: 서로 밀면 진동하기 때문이다.
		const float Target = LaneOffset + FMath::Clamp(PendingAvoidNudge, -MaxAvoidOffset, MaxAvoidOffset);
		LaneOffset = FMath::FInterpTo(LaneOffset, Target, DeltaTime, AvoidInterpSpeed);
	}
	PendingAvoidNudge = 0.f;

	// [그룹핑] 일행은 리더 옆 정해진 자리(리더 차선 + GroupLaneOffset)로 계속 돌아간다.
	// 추월·회피로 잠깐 벌어져도 다시 붙어 걸어야 일행으로 보인다.
	if (!bPassing)
	{
		if (const ASubNpcCharacter* Leader = GroupLeader.Get())
		{
			if (Leader->CurrentPath == CurrentPath)
			{
				// [버그 수정] 리더가 길 가장자리에 붙어 내 자리가 길 폭 밖이면 반대편에 선다.
				// 예전에는 길 폭으로 잘라서 리더와 같은 차선에 눌려 몸이 겹쳤다 (로그: 경로 A 입구, 일행끼리 겹침).
				// 양쪽 다 들어가면 지금 서 있는 쪽을 유지한다 (경계에서 좌우로 왔다 갔다 하며 리더 몸을 가로지르지 않게).
				const bool bOwnSideFits = FMath::Abs(Leader->LaneOffset + GroupLaneOffset) <= HalfWidth;
				const bool bOtherSideFits = FMath::Abs(Leader->LaneOffset - GroupLaneOffset) <= HalfWidth;
				const bool bCurrentlyOnOtherSide = (LaneOffset - Leader->LaneOffset) * GroupLaneOffset < 0.f;
				float Side = GroupLaneOffset;
				if (bOtherSideFits && (!bOwnSideFits || bCurrentlyOnOtherSide))
				{
					Side = -GroupLaneOffset;
				}
				const float Desired = FMath::Clamp(Leader->LaneOffset + Side, -HalfWidth, HalfWidth);
				LaneOffset = FMath::FInterpTo(LaneOffset, Desired, DeltaTime, SubNpcGroupTuning::LaneFollowRate);
			}
		}
	}

	// [게걸음 방지] 옆으로는 앞으로 나간 만큼만 움직인다.
	//
	// 예전에는 차선 이동이 전진과 무관했다. 앞이 막혀 거의 멈춘 사람이 옆으로만
	// 미끄러지고, 이웃끼리 서로 밀어내며 좌우로 흔들렸다. 사람은 피할 때
	// 앞으로 걸으면서 비스듬히 빠진다. 옆 이동을 전진 거리에 묶으면 그 그림이 된다.
	const float ForwardStep = FMath::Abs(PathDistance - PathDistanceBefore);
	// 멈춰 있어도 반 걸음 옆으로 비켜설 수는 있다 (SidestepSpeed 참고).
	const float MaxLateral = FMath::Max(
		ForwardStep * SubNpcCrowdFlow::MaxSteerRatio,
		SubNpcCrowdFlow::SidestepSpeed * DeltaTime);

	// 병목에서 비집고 추월할 때는 길 가장자리를 조금 넘어가도 된다 (Director의 SqueezeOverflow).
	// 추월이 끝나면 한 번에 튀어 들어오지 않고, 옆 이동 속도만큼 천천히 길 안으로 돌아온다.
	const float LaneLimit = bPassing
		? HalfWidth + SubNpcCrowdFlow::PassLaneOverflow
		: FMath::Max(HalfWidth, FMath::Abs(LaneBefore) - MaxLateral);
	LaneOffset = FMath::Clamp(
		LaneBefore + FMath::Clamp(LaneOffset - LaneBefore, -MaxLateral, MaxLateral),
		-LaneLimit, LaneLimit);
	const float LateralStep = LaneOffset - LaneBefore;

	// 스팟 판정은 일정 거리마다 다시 굴린다 (경로 진입 시 1회 판정의 부작용 제거).
	// 판정 자체(스팟 점유)는 게임 스레드 일이라 적용 단계(ApplyPathStep)에서 한다.
	// 스팟에 들어가면 그쪽에서 이번 걸음의 이동을 버린다.
	if (FMath::Abs(PathDistance - NextSpotCheckDistance) >= SpotCheckInterval)
	{
		NextSpotCheckDistance = PathDistance;
		Out.bSpotCheck = true;
	}

	const float Length = CurrentPath->GetPathLength();

	// 경로 끝(또는 역방향이면 시작)에 닿았으면 다음 경로로.
	// [버그 수정] 다음 경로 입구가 이 경로 끝보다 앞에 있으면 거기서 일찍 갈아탄다 (PlanNextPath).
	// 예전에는 끝까지 걸은 뒤에 갈아타서, 끝이 다음 경로 입구를 지나 뻗어 있으면(B 끝이 C 시작보다
	// 95cm 더 감) 다들 끝까지 갔다가 되돌아오며 이음매에 뭉쳤다.
	const float HandoffAt = (PlannedHandoffDistance >= 0.f)
		? GetLaneHandoffDistance()
		: (bReversedOnPath ? 0.f : Length);
	if ((!bReversedOnPath && PathDistance >= HandoffAt) || (bReversedOnPath && PathDistance <= HandoffAt))
	{
		Out.Result = FSubNpcPathStep::EResult::Handoff;   // 갈아타기는 적용 단계에서 (다음 경로 추첨·스팟 판정이 있다)
		return;
	}

	FVector Target = CurrentPath->GetLocationAtDistance(PathDistance, LaneOffset);
	FVector Dir = CurrentPath->GetDirectionAtDistance(PathDistance) * (bReversedOnPath ? -1.f : 1.f);

	// 지면에 붙여 세운다. 경로 높이를 대충 그려도 계단·경사로 위에 제대로 선다.
	// [버그 수정] 거리와 상관없이 모두 붙인다. 예전에는 가까운(Near/Mid) 개체만 붙였는데,
	// 카메라 뒤라 Far로 내려가거나 막 등장해 LOD가 정해지기 전인 사람은 경로 높이 그대로 걸었다.
	// 경로 C 오르막 끝은 바닥보다 2m 가까이 높아서 "하늘을 걷다가 공중에서 사라지는" 사람이 보였다.
	// 멀리 있는 사람은 0.3초에 한 번만 걸음을 계산하므로 트레이스 비용은 작다.
	const bool bGroundSnapped = SnapToGround(Target);

	// 경로를 막 갈아탔다면, 그때 어긋난 양을 조금씩 갚아나가며 따라붙는다.
	// 이게 없으면 전환 프레임에 위치가 뚝 끊겨서 순간이동으로 보인다.
	if (!PathHandoffOffset.IsNearlyZero())
	{
		// 지수 감쇠. DeltaTime이 커도(Far는 0.3초 틱) 0으로 튀지 않는다.
		// [버그 수정] 한 틱에 메우는 양을 걸음에 묶는다. 예전에는 걷는 것과 상관없이 1초 남짓 만에 메워서,
		// 이음매를 지나는 순간 1.4~1.6배 빨라졌고(측정: A→B, B→C 이음매), 앞사람 때문에 서 있어도
		// 이 보정만으로 미끄러져 앞사람 몸에 파고들었다. 이제 걸음의 일부만큼만 더 움직이고, 서 있으면 메우지 않는다.
		const float OffsetSize = PathHandoffOffset.Size();
		const float Desired = OffsetSize * (1.f - FMath::Exp(-SubNpcPathHandoff::BlendRate * DeltaTime));
		const float MaxCatchUp = WalkSpeed * CurrentSpeedScale * SubNpcPathHandoff::MaxCatchUpRatio * DeltaTime;
		const float Reduce = FMath::Min(Desired, MaxCatchUp);
		PathHandoffOffset *= (OffsetSize > KINDA_SMALL_NUMBER) ? (OffsetSize - Reduce) / OffsetSize : 0.f;

		if (PathHandoffOffset.SizeSquared() < FMath::Square(SubNpcPathHandoff::SnapThreshold))
		{
			PathHandoffOffset = FVector::ZeroVector;
		}

		// [버그 수정] 지면에 붙였으면 높이 어긋남은 더하지 않는다.
		// 어긋남은 스냅 전 경로 높이로 잰 값이라, 경로끼리 높이가 다르면(B 88 → C 158) 스냅한
		// 바닥 높이에 -70cm가 더해져 이음매를 지나는 순간 몸이 바닥 속으로 꺼졌다가 천천히 올라왔다.
		Target += bGroundSnapped ? FVector(PathHandoffOffset.X, PathHandoffOffset.Y, 0.f) : PathHandoffOffset;
	}

	// 스팟에서 나오는 중이면 차선 위치를 걸음 속도로 따라잡는다.
	// 진행도는 평소대로 오르므로, 차선으로 돌아오면서 자연스럽게 앞으로도 걷는다.
	if (bRejoiningPath)
	{
		const FVector Location = GetActorLocation();
		const FVector ToTarget = Target - Location;
		const float Remaining = ToTarget.Size2D();
		const float Step = WalkSpeed * SubNpcSpotWalk::RejoinSpeedScale * DeltaTime;

		if (Remaining <= FMath::Max(Step, SubNpcSpotWalk::ArriveTolerance))
		{
			bRejoiningPath = false;
		}
		else if (bOffLaneYield)
		{
			// 다른 사람이 앞을 지나가는 중이다. 서서 기다린다.
			const FVector Dir2D = FVector(ToTarget.X, ToTarget.Y, 0.f) / Remaining;
			Out.Result = FSubNpcPathStep::EResult::RotateOnly;
			Out.Rotation = FMath::RInterpTo(GetActorRotation(), Dir2D.Rotation(), DeltaTime, 6.f);
			return;
		}
		else
		{
			// 서 있는 사람(줄·이벤트)은 돌아가고, 길 밖에서 움직이는 사람은 피해서 길로 나간다
			const FVector Dir2D = SubNpcSpotWalk::SteerForSpotWalk(this, Target, Step / FMath::Max(DeltaTime, KINDA_SMALL_NUMBER), SteerHoldDir);
			if (Dir2D.IsNearlyZero())
			{
				Out.Result = FSubNpcPathStep::EResult::RotateOnly;   // 곧 부딪힌다 - 이번 틱은 선다
				Out.Rotation = GetActorRotation();
				return;
			}
			FVector Next = Location + Dir2D * Step;
			// [버그 수정] 높이는 목표점이 아니라 내 발밑 바닥에 맞춘다. 목표점(진행도)은 계속 앞으로 가서
			// 오르막 위에 있을 수 있다 - 그 높이를 따라가면 평지를 걸으면서 허공으로 올라갔다
			// (로그: 길로합류중 행인이 바닥에서 94~200cm 뜸, 아래 사람을 밟는 것처럼 보임).
			Next.Z = Location.Z;
			if (!SnapToGround(Next))
			{
				Next.Z = FMath::FInterpConstantTo(Location.Z, Target.Z, DeltaTime, WalkSpeed);
			}

			bool bBlockedByWorld = false;
			Out.Result = FSubNpcPathStep::EResult::Move;
			Out.Location = ResolveWorldBlocking(Location, Next, bBlockedByWorld);
			Out.Rotation = FMath::RInterpTo(GetActorRotation(), Dir2D.Rotation(), DeltaTime, 8.f);
			Out.bSweep = false;
			return;
		}
	}

	// [설계] 행인은 CharacterMovement를 쓰지 않고 위치를 직접 세팅한다.
	//
	// 처음에는 가까운 거리에서만 AddMovementInput을 쓰려 했는데, 두 가지 이유로 바꿨다.
	//   1) 컨트롤러가 없는 폰에서는 입력 경로가 통째로 무시된다 (bForce로도 실제 이동이 안 됨)
	//   2) 행인 수십 명에게 캐릭터 무브먼트(중력·바닥 스윕)를 돌리는 건 애초에 너무 비싸다
	//
	// 중력이 필요 없는 이유는 **경로의 높이가 곧 바닥**이기 때문이다. 스플라인을 지면에 맞춰
	// 그려두면 행인은 항상 그 위를 걷는다. 지형을 따라 오르내려야 하면 경로를 그렇게 그리면 된다.
	//
	// 플레이어와의 충돌은 그대로 살아 있다. 캡슐 콜리전은 무브먼트와 무관하게 동작하고,
	// 플레이어가 뛰어와 부딪히면 NotifyHit이 정상적으로 불린다.
	const bool bIsClose =
		(CurrentLodBand == ESubNpcLodBand::Near || CurrentLodBand == ESubNpcLodBand::Mid);

	// 회전은 살짝 보간한다. 스플라인 방향을 그대로 대입하면 코너에서 홱 돌아 로봇처럼 보인다.
	// 비스듬히 비켜 갈 때는 실제로 움직이는 방향을 본다. 경로 방향만 보면
	// 몸은 정면을 향한 채 옆으로 미끄러지는 게걸음이 된다.
	FVector FacingDir = Dir;
	if (bIsClose && ForwardStep > KINDA_SMALL_NUMBER && !FMath::IsNearlyZero(LateralStep))
	{
		// 차선 오프셋은 스플라인 진행 방향(역방향 여부와 무관)의 오른쪽 기준이다.
		const FVector PathRight = FVector::CrossProduct(FVector::UpVector, Dir * Along).GetSafeNormal2D();
		FacingDir = (Dir * ForwardStep + PathRight * LateralStep).GetSafeNormal2D();
		if (FacingDir.IsNearlyZero())
		{
			FacingDir = Dir;
		}
	}
	// 걸으면서 일행과 이야기하는 중이면 상대 쪽으로 몸을 살짝 돌린다.
	// 손짓만으로는 멀리서 대화로 안 읽힌다. 서로를 향해 몸이 기울어 있어야 "같이 이야기하며 걷는" 그림이 된다.
	// 너무 돌리면 앞으로 걷는 다리와 어긋나므로 진행 방향 기준 일정 각도까지만 돌린다.
	if (bGroupChatting && bIsClose)
	{
		if (const ASubNpcCharacter* Partner = GroupChatPartner.Get())
		{
			FVector ToPartner = Partner->GetActorLocation() - GetActorLocation();
			ToPartner.Z = 0.f;
			if (ToPartner.Normalize())
			{
				const float WalkYaw = FacingDir.Rotation().Yaw;
				const float Delta = FMath::Clamp(
					FMath::FindDeltaAngleDegrees(WalkYaw, ToPartner.Rotation().Yaw),
					-SubNpcGroupChat::MaxFaceTurnDegrees, SubNpcGroupChat::MaxFaceTurnDegrees);
				FacingDir = FRotator(0.f, WalkYaw + Delta, 0.f).Vector();
			}
		}
	}

	const FRotator TargetRot = FacingDir.Rotation();
	const FRotator NewRot = bIsClose
		? FMath::RInterpTo(GetActorRotation(), TargetRot, DeltaTime, 8.f)
		: TargetRot;

	// [스윕을 끄는 이유]
	//
	// 스윕을 켜면 행인끼리 캡슐로 서로를 막는다. 같은 지점에 두 명이 등장하는 순간
	// 둘 다 영영 못 움직이고, PathDistance만 계속 올라 몸과 진행도가 어긋난다.
	// 인파가 많아질수록 이 정체는 반드시 생긴다.
	//
	// 스윕을 꺼도 **플레이어와의 부딪힘 반응은 그대로 동작한다.**
	// 히트 이벤트를 만드는 건 플레이어 쪽 무브먼트의 스윕이고, 행인 캡슐은 그냥
	// 막는 물체로 존재하기만 하면 되기 때문이다. E키 상호작용(오버랩 질의)도 마찬가지다.
	//
	// 잃는 건 "행인이 벽을 뚫을 수 있다" 하나뿐인데, 경로는 어차피 사람이 그리는 것이라
	// 벽을 통과하는 선을 안 그리면 그만이다. (레퍼런스 영상의 군중도 서로 통과한다)
	//
	// [추가] 그런데 추월·비집기로 길 가장자리를 넘거나 경로가 지형에 붙어 있으면 벽을 뚫었다.
	// 그래서 캡슐 스윕 대신 지형(WorldStatic/WorldDynamic)만 따로 쓸어 본다 (ResolveWorldBlocking).
	// 행인끼리는 여전히 막지 않으므로 위의 정체 문제는 생기지 않는다.
	// [속도 상한] 실제로 몸이 움직이는 거리를 걸음 속도의 일정 배수로 묶는다.
	// 일행 따라잡기·경로 갈아타기 보정·굽은 곳 차선 보정이 겹치면 한 걸음이 걸음 속도보다 커져서,
	// 이음매에서 1.4~1.6배로 끌려가듯 빨라졌다 (측정: [CrowdSpeed]). 남은 거리는 다음 걸음들에서 따라잡는다.
	{
		const FVector Current = GetActorLocation();
		const FVector Delta2D(Target.X - Current.X, Target.Y - Current.Y, 0.f);
		const float MaxStep = WalkSpeed * SubNpcCrowdFlow::MaxSpeedRatio * DeltaTime;
		const float StepSize = Delta2D.Size();

		// 몸이 목표점보다 많이 뒤처졌으면(목표점이 진행 방향 앞) 진행도를 묶어 둔다. 진행도가 몸보다 몇 미터씩
		// 앞서 나가면 몸은 그 목표점까지 사람들 사이를 직선으로 가로지르고, 진행도로 정하는 앞뒤 판정도 틀어진다.
		// 목표점이 몸 뒤에 있으면 묶지 않는다 - 묶으면 몸이 뒤로 걸어간다.
		if (StepSize > SubNpcCrowdFlow::MaxTargetLead && FVector::DotProduct(Delta2D, Dir) > 0.f)
		{
			PathDistance = PathDistanceBefore;
		}

		if (StepSize > MaxStep && StepSize > KINDA_SMALL_NUMBER)
		{
			const FVector Clamped = Delta2D * (MaxStep / StepSize);
			Target.X = Current.X + Clamped.X;
			Target.Y = Current.Y + Clamped.Y;
		}
	}

	bool bBlockedByWorld = false;
	const FVector Resolved = ResolveWorldBlocking(GetActorLocation(), Target, bBlockedByWorld);
	if (bBlockedByWorld)
	{
		// 벽에 막혔으면 차선을 길 가운데 쪽으로 당긴다. 그대로 두면 계속 벽을 긁으며 걷는다.
		LaneOffset *= SubNpcWorldBlock::BlockedLanePull;

		// [버그 수정] 막혀 있는 동안은 진행도도 올리지 않는다.
		// 예전에는 몸은 벽(경사로 옆면 등)에 막혀 서 있는데 진행도만 계속 올라 목표점이 멀리 달아났다.
		// 차선이 안쪽으로 당겨져 벽에서 빠지는 순간 그 먼 목표점으로 한 번에 옮겨져 순간이동처럼 보였다
		// (오르막 가장자리 차선 행인이 못 오르다가 휙 올라감).
		// 길 한가운데까지 당겨졌는데도 막히면 경로 자체가 벽을 지나는 배치 문제다. 그때는 영영 서 있지 않게 진행시킨다.
		if (FMath::Abs(LaneOffset) > 10.f)
		{
			PathDistance = PathDistanceBefore;
		}
	}
	Out.Result = FSubNpcPathStep::EResult::Move;
	Out.Location = Resolved;
	Out.Rotation = NewRot;
	Out.bSweep = bSweepWhileMoving;
}

namespace SubNpcExitFade
{
	/** 사라지는 데 걸리는 시간 (초). 이 동안 끝 너머로 계속 걷는다 (걸음 속도 1.4m/s면 약 1.1m) */
	constexpr float Duration = 0.8f;

	/** 페이드 머티리얼의 스칼라 파라미터 이름. 1 = 다 보임, 0 = 다 사라짐 */
	static const FName ParamName(TEXT("Fade"));
}

bool ASubNpcCharacter::BeginExitFade()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp || !CurrentPath || ExitFadeMaterials.Num() == 0 || bExitFading)
	{
		return false;
	}

	// 바꿔 끼우기 전 머티리얼을 기억한다. 블루프린트(OnArchetypeApplied)가 외형으로 바꾼 것일 수 있다.
	MaterialsBeforeExitFade.Reset();
	const int32 NumSlots = MeshComp->GetNumMaterials();
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		MaterialsBeforeExitFade.Add(MeshComp->GetMaterial(Slot));
	}
	for (int32 Slot = 0; Slot < NumSlots && Slot < ExitFadeMaterials.Num(); ++Slot)
	{
		if (ExitFadeMaterials[Slot])
		{
			MeshComp->SetMaterial(Slot, ExitFadeMaterials[Slot]);
		}
	}
	MeshComp->SetScalarParameterValueOnMaterials(SubNpcExitFade::ParamName, 1.f);

	// 경로 끝의 진행 방향으로 계속 걷는다. 끝점에서 멈춰 서서 사라지면 어색하다.
	const float EndDistance = bReversedOnPath ? 0.f : CurrentPath->GetPathLength();
	const FVector Dir = (CurrentPath->GetDirectionAtDistance(EndDistance) * (bReversedOnPath ? -1.f : 1.f)).GetSafeNormal2D();
	ExitVelocity = Dir * WalkSpeed;

	bExitFading = true;
	ExitFadeElapsed = 0.f;
	CurrentSpeedScale = 1.f;   // 걷기 애니메이션이 제 속도로 나오게 한다

	// 멀리서 0.1초 간격으로 움직이면 사라지는 모습이 뚝뚝 끊긴다. 사라지는 동안은 매 프레임 갱신한다.
	SetActorTickInterval(0.f);
	return true;
}

void ASubNpcCharacter::TickExitFade(float DeltaTime)
{
	ExitFadeElapsed += DeltaTime;
	const float Alpha = FMath::Clamp(1.f - ExitFadeElapsed / SubNpcExitFade::Duration, 0.f, 1.f);

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetScalarParameterValueOnMaterials(SubNpcExitFade::ParamName, Alpha);
	}

	// 똑바로 걸어 나간다. 바닥에 붙이고, 벽이 있으면 뚫지 않는다.
	const FVector From = GetActorLocation();
	FVector To = From + ExitVelocity * DeltaTime;
	SnapToGround(To);
	bool bBlocked = false;
	SetActorLocation(ResolveWorldBlocking(From, To, bBlocked));

	if (Alpha <= 0.f)
	{
		// 다 사라졌다. 회수는 Director가 안전한 시점에 한다 (머티리얼은 풀로 돌아갈 때 되돌린다).
		SetActorHiddenInGame(true);
		bExitFading = false;
		bPendingRecycle = true;
		CurrentPath = nullptr;
	}
}

void ASubNpcCharacter::RestoreMaterialsAfterExitFade()
{
	bExitFading = false;
	ExitFadeElapsed = 0.f;
	if (MaterialsBeforeExitFade.Num() == 0)
	{
		return;
	}
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		for (int32 Slot = 0; Slot < MaterialsBeforeExitFade.Num(); ++Slot)
		{
			MeshComp->SetMaterial(Slot, MaterialsBeforeExitFade[Slot]);
		}
	}
	MaterialsBeforeExitFade.Reset();
}

bool ASubNpcCharacter::SnapToGround(FVector& InOutCenter) const
{
	if (SubNpcGroundSnap::Enabled == 0)
	{
		return false;
	}
	SUBNPC_PROFILE_SCOPE(GroundSnap);

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const float HalfHeight = GetCapsuleComponent()
		? GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
		: 88.f;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SubNpcGroundSnap), /*bTraceComplex=*/false, this);

	const FVector Foot = InOutCenter - FVector(0.f, 0.f, HalfHeight);
	const FVector Start = Foot + FVector(0.f, 0.f, SubNpcGroundSnap::TraceUp);
	const FVector End = Foot - FVector(0.f, 0.f, SubNpcGroundSnap::TraceDown);

	// [버그 수정] 지형(WorldStatic/WorldDynamic)만 바닥으로 본다.
	// 예전에는 Visibility 채널로 쏴서 바로 아래에 있는 다른 행인·플레이어 몸에 맞았고,
	// 그 사람 머리를 바닥으로 알고 위에 올라서서 "머리 위로 걷는" 사람이 생겼다.
	// 트리거 같은 겹침 전용 볼륨도 건너뛴다 (사람을 실제로 막는 것만 바닥).
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);

	TArray<FHitResult> Hits;
	World->LineTraceMultiByObjectType(Hits, Start, End, Objects, Params);
	for (const FHitResult& Hit : Hits)   // 가까운 것부터 정렬돼 있다
	{
		const UPrimitiveComponent* Comp = Hit.GetComponent();
		if (!Comp || Comp->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block
			|| Cast<APawn>(Hit.GetActor()))
		{
			continue;
		}
		InOutCenter.Z = Hit.ImpactPoint.Z + HalfHeight;
		return true;
	}
	return false;
}

void ASubNpcCharacter::ApplyPathStep(const FSubNpcPathStep& Step)
{
	SUBNPC_PROFILE_SCOPE(PathApply);

	if (Step.bSpotCheck && !bSpotDecisionByStateTree)   // StateTree가 맡으면 "행인: 스팟 들르기 판정" 태스크가 정한다
	{
		RollForActivitySpot();
		if (CurrentActivity != ESubNpcActivity::Walking)
		{
			return;   // 스팟에 들어갔으면 이번 걸음의 이동은 버린다
		}
	}

	switch (Step.Result)
	{
	case FSubNpcPathStep::EResult::Handoff:
		AdvanceToNextPath();
		break;
	case FSubNpcPathStep::EResult::RotateOnly:
		SetActorRotation(Step.Rotation);
		break;
	case FSubNpcPathStep::EResult::Move:
		SetActorLocationAndRotation(Step.Location, Step.Rotation, Step.bSweep);
		break;
	default:
		break;
	}
}

void ASubNpcCharacter::TickPathFollow(float DeltaTime)
{
	FSubNpcPathStep Step;
	ComputePathStep(DeltaTime, Step);
	ApplyPathStep(Step);
}

bool ASubNpcCharacter::IsMovementDrivenByMass() const
{
	return MassEntity.IsSet() && GSubNpcUseMass != 0;
}

void ASubNpcCharacter::AdvanceToNextPath()
{
	// [방어] 길이가 사실상 0인 경로는 "도착"이 매 틱 성립해서 무한 재활용을 일으킨다.
	// 등장하자마자 회수되고 스포너가 다시 채우는 게 반복되어, 겉으로는
	// "안 움직이는데 계속 다른 사람으로 바뀐다"로 보인다.
	const float PathLength = CurrentPath ? CurrentPath->GetPathLength() : 0.f;
	if (PathLength < MinUsablePathLength)
	{
		// 같은 경로를 밟는 행인마다 찍히면 로그가 도배된다. 경로마다 한 번만 알린다.
		static TSet<TWeakObjectPtr<const ANpcCrowdPath>> ReportedShortPaths;
		if (!ReportedShortPaths.Contains(CurrentPath))
		{
			ReportedShortPaths.Add(CurrentPath);
			SUBNPC_LOG(Warning,
				TEXT("[Crowd] 경로 '%s'의 길이가 %.0fcm뿐입니다. 스플라인 점을 벌려 최소 %.0fcm 이상으로 만드세요."),
				*GetNameSafe(CurrentPath), PathLength, MinUsablePathLength);
		}

		CurrentPath = nullptr;
		CurrentActivity = ESubNpcActivity::Walking;
		SetMovementState(ENpcMovementState::Idle);
		return;
	}

	// 들어설 때 미리 정해 둔 다음 경로를 쓴다 (갈아탈 지점이 그 경로 기준으로 계산돼 있다).
	bool bNextReversed = bPlannedNextReversed;
	ANpcCrowdPath* Next = PlannedNextPath.Get();
	if (!Next || !Next->IsOpen())
	{
		Next = CurrentPath->PickNextPath(bNextReversed);
	}

	if (!Next)
	{
		// 막다른 길이다.
		//
		// [설계] 두 가지 끝이 있다.
		//   1) 퇴장구 (bDespawnAtEnd) - 맵 밖, 출입문, 골목 끝처럼 나가면 사라지는 게 맞는 곳.
		//      보이든 말든 즉시 회수한다. 사라지는 순간을 가리는 건 경로를 그리는 쪽의 몫이다.
		//      (문 안쪽으로 몇 미터 더 그려두면 된다)
		//   2) 그냥 끝난 구간 - 이어붙이는 걸 깜빡했거나 아직 안 만든 길.
		//      여기서 무조건 회수하면 플레이어 눈앞에서 사람이 뻥 사라지므로,
		//      보이는 거리면 방향만 돌려서 왔던 길을 되돌아가게 한다.
		const bool bIsExit = CurrentPath->IsDespawnAtEnd();

		if (!bIsExit &&
			(CurrentLodBand == ESubNpcLodBand::Near || CurrentLodBand == ESubNpcLodBand::Mid))
		{
			bReversedOnPath = !bReversedOnPath;

			// 방향을 돌렸으니 앞서 정한 다음 경로·갈아탈 지점은 더 이상 맞지 않는다. 끝까지 간다.
			PlannedNextPath = nullptr;
			PlannedHandoffDistance = -1.f;

			// 끝점에 딱 붙어 있으면 다음 틱에 또 도착 판정이 나므로 살짝 안쪽으로 밀어준다.
			PathDistance = FMath::Clamp(PathDistance, 0.f, PathLength);
			PathDistance += bReversedOnPath ? -TurnAroundNudge : TurnAroundNudge;
			return;
		}

		// 퇴장구이거나, 안 보이는 거리다. 회수 대상으로 표시만 해둔다.
		//
		// [버그 수정] 예전에는 여기서 Director->ReturnToPool(this)를 바로 불렀다.
		// 그러면 Director가 활성 목록을 순회하는 도중에 그 목록에서 자기를 빼내게 되어,
		// 순회 인덱스가 어긋날 수 있다. LOD 갱신 쪽은 이미 모았다가 처리하도록
		// 되어 있었는데 이 경로만 보호를 안 받고 있었다.
		//
		// 지금은 표시만 하고 실제 회수는 Director가 안전한 시점에 처리한다.
		// 표시된 개체는 이동을 멈춰야 하므로 경로 참조를 끊어 둔다.
		//
		// [퇴장 페이드] 퇴장구가 보이는 곳에 있으면(경로 C 끝은 오르막 위 평지 한가운데) 끝에 닿는 순간
		// 사람이 툭 사라져 보였다. 끝 너머로 몇 걸음 더 걸으며 서서히 사라진 뒤 회수한다.
		// 거리와 상관없이 한다 - 한 명당 0.8초뿐이고, 카메라 뒤라 Far로 내려간 사람도 돌아보면 보인다.
		if (bIsExit && BeginExitFade())
		{
			return;
		}
		bPendingRecycle = true;
		CurrentPath = nullptr;
		return;
	}

	// [수정] 예전에는 다음 경로의 0 지점(역방향이면 끝점)으로 무조건 보냈다.
	// 두 스플라인의 이음매가 딱 붙어 있지 않으면 그 간격만큼 순간이동한다.
	// 「걷다가 갑자기 사라졌다가 엉뚱한 데서 나타난다」의 정체가 이것이다.
	//
	// 지금은 지금 서 있는 자리에서 가장 가까운 지점으로 들어간다.
	// 이음매가 붙어 있으면 그 값이 사실상 0이라 동작이 같고,
	// 떨어져 있어도 튀는 거리를 최소로 줄인다.
	const ANpcCrowdPath* PrevPath = CurrentPath;
	const FVector HandoffLocation = GetActorLocation();

	CurrentPath = Next;
	bReversedOnPath = bNextReversed;

	float EnterDistance = bNextReversed ? Next->GetPathLength() : 0.f;
	const float GapSq = Next->FindClosestPointOnPath(HandoffLocation, EnterDistance);

	PathDistance = EnterDistance;
	NextSpotCheckDistance = PathDistance;

#if !UE_BUILD_SHIPPING
	// 그래도 멀다면 레벨 배치 문제다. 어느 경로 쌍이 벌어져 있는지 찍어준다.
	constexpr float AcceptableGapSq = 300.f * 300.f;
	if (GapSq > AcceptableGapSq)
	{
		SUBNPC_LOG(Warning,
			TEXT("[Crowd] 경로 이음매가 %.0f cm 벌어져 있습니다: %s -> %s. 스플라인 끝점을 서로 붙여주세요."),
			FMath::Sqrt(GapSq), *GetNameSafe(PrevPath), *GetNameSafe(Next));
	}
#endif

	// [버그 수정] 차선은 지금 몸이 서 있는 자리를 새 경로 기준으로 다시 잰다.
	// 경로가 꺾이면 "오른쪽"의 방향이 바뀌어서, 예전처럼 차선 값을 그대로 가져가면 새 경로에서 설 자리가
	// 실제 몸과 1m 가까이 어긋났고, 그걸 짧은 시간에 따라잡느라 이음매에서 옆으로 휙 미끄러졌다.
	// 몸 위치 그대로 이어 받으면 어긋남이 거의 없다. 예전 ±30cm 재추첨도 엉킴만 만들어서 뺐다.
	const float HalfWidth = Next->GetLaneHalfWidth();
	{
		const FVector Base = Next->GetLocationAtDistance(PathDistance, 0.f);
		const FVector Right = (Next->GetLocationAtDistance(PathDistance, 1.f) - Base).GetSafeNormal2D();
		LaneOffset = FMath::Clamp(FVector::DotProduct(HandoffLocation - Base, Right), -HalfWidth, HalfWidth);
	}
	DodgeTargetOffset = LaneOffset;

	// [부드러운 전환] 새 경로에서의 첫 위치와 지금 서 있는 자리의 차이를 기억해둔다.
	//
	// 다음 틱부터 이 오프셋이 더해진 자리에 서고, 그 값이 매 틱 0을 향해 줄어든다.
	// 그래서 사람이 새 경로로 **미끄러져 들어가지, 순간이동하지 않는다.**
	// 차선 오프셋을 다시 굴린 것까지 여기서 함께 흡수된다.
	const FVector NewTarget = Next->GetLocationAtDistance(PathDistance, LaneOffset);
	const FVector Gap = HandoffLocation - NewTarget;

	PathHandoffOffset = (Gap.SizeSquared() <= FMath::Square(SubNpcPathHandoff::MaxBlendDistance))
		? Gap
		: FVector::ZeroVector;   // 너무 멀면 흡수를 포기한다 (위 경고 로그가 이미 알려준다)

	// 새 경로의 스팟은 처음 보는 것이므로 판정 기록을 비운다.
	RolledSpots.Reset();

	PlanNextPath();
	if (!bSpotDecisionByStateTree)
	{
		RollForActivitySpot();
	}
}

float ASubNpcCharacter::GetLaneHandoffDistance() const
{
	// [정체 해소] 갈아타는 지점을 차선마다 다르게 잡는다.
	//
	// 예전에는 모두 같은 진행도(다음 경로 입구에서 가장 가까운 점)에서 갈아탔다. 직각으로 꺾이는
	// 이음매(B→C)에서는 B의 좌우 폭이 C의 앞뒤 방향이라, 같은 지점에서 갈아탄 사람들은 C 기준으로
	// 전부 차선 0 근처를 받았다 - 넓게 퍼져 오던 사람들이 모서리에서 한 줄로 합쳐지며 뭉쳤다.
	//
	// 지금은 "B의 내 차선 선"이 "C의 같은 쪽 차선 선"과 만나는 지점에서 갈아탄다.
	// 안쪽 사람은 일찍 안쪽 차선으로, 바깥쪽 사람은 늦게 바깥쪽 차선으로 꺾는다 (차가 모서리를 도는 것처럼).
	const ANpcCrowdPath* Next = PlannedNextPath.Get();
	if (!CurrentPath || !Next)
	{
		return PlannedHandoffDistance;
	}

	const float OldAlong = bReversedOnPath ? -1.f : 1.f;
	const float NewAlong = bPlannedNextReversed ? -1.f : 1.f;
	const float EntryDist = bPlannedNextReversed ? Next->GetPathLength() : 0.f;

	const FVector MyDir = (CurrentPath->GetDirectionAtDistance(PlannedHandoffDistance) * OldAlong).GetSafeNormal2D();
	const FVector EntryBase = Next->GetLocationAtDistance(EntryDist, 0.f);
	const FVector NextRight = (Next->GetLocationAtDistance(EntryDist, 1.f) - EntryBase).GetSafeNormal2D();

	// 거의 같은 방향으로 이어지는 이음매면 보정할 게 없다.
	const float Denom = FVector::DotProduct(MyDir, NextRight);
	if (FMath::Abs(Denom) < 0.2f)
	{
		return PlannedHandoffDistance;
	}

	// 진행 방향 기준 같은 쪽(안쪽은 안쪽, 바깥쪽은 바깥쪽)을 유지한 새 경로 차선.
	const float NextLane = FMath::Clamp(LaneOffset * OldAlong * NewAlong, -Next->GetLaneHalfWidth(), Next->GetLaneHalfWidth());
	const FVector NextLanePoint = EntryBase + NextRight * NextLane;
	const FVector MyLanePoint = CurrentPath->GetLocationAtDistance(PlannedHandoffDistance, LaneOffset);

	const float Shift = FVector::DotProduct(NextLanePoint - MyLanePoint, NextRight) / Denom;
	const float Clamped = FMath::Clamp(Shift, -SubNpcPathHandoff::MaxEarlyHandoff, SubNpcPathHandoff::MaxEarlyHandoff);
	return FMath::Clamp(PlannedHandoffDistance + Clamped * OldAlong, 0.f, CurrentPath->GetPathLength());
}

void ASubNpcCharacter::PlanNextPath()
{
	PlannedNextPath = nullptr;
	bPlannedNextReversed = false;
	PlannedHandoffDistance = -1.f;

	if (!CurrentPath)
	{
		return;
	}

	// 일행은 리더와 같은 길로 간다. 갈림길에서 따로 고르면 그룹이 찢어진다.
	bool bNextReversed = false;
	ANpcCrowdPath* Next = nullptr;
	if (const ASubNpcCharacter* Leader = GroupLeader.Get())
	{
		if (Leader->CurrentPath == CurrentPath && Leader->PlannedNextPath.IsValid() && Leader->PlannedNextPath->IsOpen())
		{
			Next = Leader->PlannedNextPath.Get();
			bNextReversed = Leader->bPlannedNextReversed;
		}
	}
	if (!Next)
	{
		Next = CurrentPath->PickNextPath(bNextReversed);
	}
	if (!Next)
	{
		return;   // 막다른 길: 끝까지 간다
	}

	PlannedNextPath = Next;
	bPlannedNextReversed = bNextReversed;

	// 다음 경로 입구가 이 경로의 어디쯤인지 잰다. 입구가 끝 근처(진행 방향 기준 마지막 몇 m)에 있을 때만
	// 그 지점에서 갈아탄다. 경로 중간을 지나가는 다른 경로 입구에 걸려 일찍 빠지면 안 된다.
	const FVector Entry = Next->GetLocationAtDistance(bNextReversed ? Next->GetPathLength() : 0.f, 0.f);
	float EntryDist = 0.f;
	CurrentPath->FindClosestPointOnPath(Entry, EntryDist);

	const float Length = CurrentPath->GetPathLength();
	if (!bReversedOnPath)
	{
		PlannedHandoffDistance = (EntryDist >= Length - SubNpcPathHandoff::MaxEarlyHandoff) ? EntryDist : Length;
	}
	else
	{
		PlannedHandoffDistance = (EntryDist <= SubNpcPathHandoff::MaxEarlyHandoff) ? EntryDist : 0.f;
	}
}

// ---------------------------------------------------------------------------
// 활동 스팟
// ---------------------------------------------------------------------------

void ASubNpcCharacter::RollForActivitySpot()
{
	// [버그 수정] 일행을 이끄는 리더도 스팟에 들르지 않는다. 리더가 스팟·줄에서 수십 초를 보내는 동안
	// 일행은 길 한가운데서 리더를 기다리고, 그 뒤로 정체가 생겼다 (측정: 스팟 옆에 20명 넘게 정지).
	if (!CurrentPath || OccupiedSpot || QueuedSpot.IsValid() || bHasGroupFollowers || bGroupWaiting)
	{
		return;
	}

	// [일행 스팟 들르기] 일행(리더 아님)은 "일행 들르기"를 켠 스팟에만 들른다 (자리가 없으면 줄을 선다).
	// 들르면 리더와 나머지 일행이 스팟 옆 대기 자리에 비켜서서 기다린다 (BeginGroupSpotVisit).
	// 리더는 들르지 않는다 - 무리 전체를 세우는 사람이라 리더가 빠지면 대열을 다시 짜야 한다.
	ASubNpcCharacter* Leader = GroupLeader.Get();
	if (GroupLeader.IsValid() && (!Leader || Leader->IsHostingGroupVisit() || !Leader->IsWalkingOnPath()
		|| Leader->GetCurrentPath() != CurrentPath))
	{
		return;
	}

	// [버그 수정] 예전에는 배열을 앞에서부터 훑고 첫 성공에서 멈췄다.
	// 그래서 한 경로에 스팟이 여러 개면 0번이 압도적으로 자주 걸리고,
	// 뒤쪽 스팟은 앞이 꽉 찼을 때만 쓰였다. 순서를 섞어서 고르게 분산시킨다.
	TArray<ANpcActivitySpot*> Candidates;
	Candidates.Reserve(CurrentPath->GetActivitySpots().Num());
	for (ANpcActivitySpot* Spot : CurrentPath->GetActivitySpots())
	{
		if (Spot)
		{
			Candidates.Add(Spot);
		}
	}
	for (int32 i = Candidates.Num() - 1; i > 0; --i)
	{
		Candidates.Swap(i, FMath::RandRange(0, i));
	}

	const FVector MyLocation = GetActorLocation();
	const float EngageRadiusSq = FMath::Square(SpotEngageRadius);

	for (ANpcActivitySpot* Spot : Candidates)
	{
		// [버그 수정] 거리 조건이 없으면 이미 지나쳐 온 스팟에도 당첨돼서
		// 뒤로 순간이동하는 것처럼 보인다.
		if (FVector::DistSquared(Spot->GetActorLocation(), MyLocation) > EngageRadiusSq)
		{
			continue;
		}

		// 이번 경로에서 이 스팟은 한 번만 굴린다 (RolledSpots 참고). 반경 안에 들어온 첫 판정이 그 스팟의 운명이다.
		if (RolledSpots.Contains(Spot))
		{
			continue;
		}
		RolledSpots.Add(Spot);

		if (Leader && !Spot->AllowsGroupVisit())
		{
			continue;
		}

		if (FMath::FRand() > Spot->GetEngageChance())
		{
			continue;
		}

		// 자리가 있고 기다리는 사람도 없으면 바로 들어간다.
		if (TryEnterSpot(Spot))
		{
			if (Leader)
			{
				Leader->BeginGroupSpotVisit(this, Spot);
			}
			return;
		}

		// [변경] 일행도 줄을 선다. 예전에는 줄을 안 서서, 1인용 스팟에 줄이 늘 있으니 일행은 거의 못 들렀다.
		// 들른 사람만 줄에 서고 나머지는 일행 대기 자리에서 기다린다 (MaxGroupVisitWait를 넘으면 그 사람만 일행에서 빠진다).

		// [줄은 길 가장자리를 따라 상류 쪽으로 선다] 줄 끝이 이미 내 뒤에 있으면 거슬러 걸어가야 하니 서지 않는다.
		// 줄 끝이 내 앞에 있을 때만 옆으로 한 걸음 비켜 붙는다 - 걷던 흐름 그대로 줄에 들어간다.
		{
			const FVector QueueEnd = Spot->GetQueueLocation(Spot->GetQueueLength());
			const FVector Along = CurrentPath->GetDirectionAtDistance(PathDistance).GetSafeNormal2D() * (bReversedOnPath ? -1.f : 1.f);
			if (FVector::DotProduct(FVector(QueueEnd - MyLocation).GetSafeNormal2D(), Along) < 0.f)
			{
				continue;
			}
		}

		// [줄 서기] 자리가 없거나 먼저 기다리는 사람이 있으면 줄 끝에 선다.
		// 예전에는 그냥 지나갔다가 다음 판정에서 운 좋게 걸린 사람이 들어가서,
		// 먼저 와 있던 사람보다 뒤에 온 사람이 먼저 하는 일이 생겼다.
		if (Spot->TryJoinQueue(this))
		{
			BeginSpotQueue(Spot);
			if (Leader)
			{
				Leader->BeginGroupSpotVisit(this, Spot);
			}
			return;
		}
	}
}

bool ASubNpcCharacter::TryEnterSpot(ANpcActivitySpot* Spot)
{
	if (!Spot || OccupiedSpot || !Spot->CanEnterNow(this))
	{
		return false;
	}

	FNpcActivityOption Option;
	int32 ActivityIndex = INDEX_NONE;
	if (!Spot->PickActivity(Option, ActivityIndex))
	{
		return false;
	}

	int32 SlotIndex = 0;
	if (!Spot->TryOccupy(this, ActivityIndex, SlotIndex))
	{
		return false;
	}

	// 줄에서 차례가 와서 들어가는 경우. 스팟이 줄에서는 이미 뺐다.
	QueuedSpot = nullptr;
	bQueueMoving = false;

	BeginSpotActivity(Spot, Option, SlotIndex);
	return true;
}

void ASubNpcCharacter::BeginSpotQueue(ANpcActivitySpot* Spot)
{
	const UWorld* World = GetWorld();
	QueuedSpot = Spot;
	QueueGiveUpTime = (World ? World->GetTimeSeconds() : 0.f) + Spot->GetMaxQueueWait();
	CurrentActivity = ESubNpcActivity::AtSpot;
	bApproachingSpot = false;
	bRejoiningPath = false;
	bQueueMoving = true;
	SetMovementState(ENpcMovementState::Walk);
}

void ASubNpcCharacter::TickSpotQueue(float DeltaTime, float Now)
{
	ANpcActivitySpot* Spot = QueuedSpot.Get();
	if (!Spot)
	{
		LeaveSpotQueue();
		return;
	}

	// 차례가 왔으면 들어간다. 줄의 맨 앞만 통과하므로 순서가 지켜진다.
	// [자연스러운 차례] 칸이 비었다고 바로 들어가지 않는다. 앞사람이 칸에서 완전히 벗어난 걸 보고,
	// 잠깐(랜덤) 뒤에 들어간다. 예전에는 앞사람이 나가는 순간 다음 사람이 같은 자리로 들어가 겹치고 어색했다.
	if (Spot->CanEnterNow(this))
	{
		if (!IsSpotStandClear(Spot))
		{
			SpotEnterReadyTime = -1.f;
		}
		else if (SpotEnterReadyTime < 0.f)
		{
			SpotEnterReadyTime = Now + FMath::FRandRange(SubNpcSpotWalk::QueueReactMin, SubNpcSpotWalk::QueueReactMax);
		}
		else if (Now >= SpotEnterReadyTime && TryEnterSpot(Spot))
		{
			SpotEnterReadyTime = -1.f;
			QueueStepReadyTime = -1.f;
			return;
		}
	}
	else
	{
		SpotEnterReadyTime = -1.f;
	}

	const int32 Index = Spot->GetQueueIndex(this);
	if (Index == INDEX_NONE || Now >= QueueGiveUpTime)
	{
		LeaveSpotQueue();
		return;
	}

	// 줄에서 내 자리로 걸어간다. 앞사람이 빠지면 한 칸씩 당겨 선다.
	const FVector Target = Spot->GetQueueLocation(Index);
	const FVector Location = GetActorLocation();
	FVector ToTarget = Target - Location;
	ToTarget.Z = 0.f;
	const float Remaining = ToTarget.Size();
	const float Step = WalkSpeed * DeltaTime;

	if (Remaining <= FMath::Max(Step, SubNpcSpotWalk::ArriveTolerance))
	{
		if (bQueueMoving)
		{
			// 도착하면 스팟 쪽을 보고 선다.
			bQueueMoving = false;
			FVector ToSpot = Spot->GetActorLocation() - Target;
			ToSpot.Z = 0.f;
			const FRotator Facing = ToSpot.IsNearlyZero() ? GetActorRotation() : ToSpot.Rotation();
			SetActorLocationAndRotation(FVector(Target.X, Target.Y, Location.Z), Facing, /*bSweep=*/false);
			SetMovementState(ENpcMovementState::Idle);
		}
		return;
	}

	if (!bQueueMoving)
	{
		// 앞자리가 비었다. 앞사람이 걸어가는 걸 잠깐 보고 나서 따라 당긴다 (줄 전체가 한꺼번에 움직이지 않게).
		if (QueueStepReadyTime < 0.f)
		{
			QueueStepReadyTime = Now + FMath::FRandRange(SubNpcSpotWalk::QueueReactMin, SubNpcSpotWalk::QueueReactMax);
		}
		if (Now < QueueStepReadyTime)
		{
			return;
		}
		QueueStepReadyTime = -1.f;
		bQueueMoving = true;
		SetMovementState(ENpcMovementState::Walk);
	}

	// [줄 안에서 당겨 서기] 이미 줄 위에 서 있다가 앞자리로 당기는 중이면 줄을 따라 똑바로 걷는다.
	// 앞사람이 비킨 만큼만 따라붙는다 (간격의 85%보다 가까우면 기다린다). 피하기·양보를 쓰지 않는다 -
	// 예전에는 줄 안 이동도 길 밖 걸음으로 취급해서, 앞사람이 아직 안 비켰을 때 피하기·양보·겹침 금지가
	// 서로 세워 줄 전체가 몇 초씩 멈췄다 (crowd.DebugSpot 2 기록).
	{
		const FVector Q0 = Spot->GetQueueLocation(0);
		FVector Axis = Spot->GetQueueLocation(1) - Q0;   // 스팟에서 멀어지는 쪽
		Axis.Z = 0.f;
		const float Spacing = Axis.Size();
		if (Spacing > KINDA_SMALL_NUMBER)
		{
			Axis /= Spacing;
			const FVector Origin = Q0 - Axis * Spacing;   // 줄 0번 앞 = 이벤트 칸
			const FVector Rel = Location - Origin;
			const float MyAlong = FVector::DotProduct(Rel, Axis);
			const float MyLateral = FMath::Abs(FVector::DotProduct(Rel, FVector(-Axis.Y, Axis.X, 0.f)));
			const float TargetAlong = FVector::DotProduct(Target - Origin, Axis);
			if (MyLateral < 40.f && MyAlong >= TargetAlong - 5.f)
			{
				// 내 바로 앞사람(줄 Index-1번, 0번이면 칸으로 들어가는 사람)과의 간격
				float AheadGap = TNumericLimits<float>::Max();
				if (const UWorld* World = GetWorld())
				{
					if (const USubNpcDirectorSubsystem* Director = World->GetSubsystem<USubNpcDirectorSubsystem>())
					{
						for (const ASubNpcCharacter* Other : Director->GetActiveNpcs())
						{
							// 같은 스팟 줄·칸 사람 + 스팟에서 나와 줄 옆을 지나 길로 돌아가는 사람 (예전엔 안 봐서 그대로 밀고 들어갔다)
							const bool bSameSpot = Other->GetQueuedSpot() == Spot || Other->GetOccupiedSpot() == Spot;
							if (Other == this || !IsValid(Other) || !(bSameSpot || Other->IsRejoiningPath()))
							{
								continue;
							}
							const FVector OtherRel = Other->GetActorLocation() - Origin;
							const float OtherAlong = FVector::DotProduct(OtherRel, Axis);
							if (!bSameSpot && FMath::Abs(FVector::DotProduct(OtherRel, FVector(-Axis.Y, Axis.X, 0.f))) > 60.f)
							{
								continue;   // 줄 선에서 벗어난 사람은 상관없다
							}
							if (OtherAlong < MyAlong)
							{
								AheadGap = FMath::Min(AheadGap, MyAlong - OtherAlong);
							}
						}
					}
				}
				if (AheadGap < Spacing * 0.85f)
				{
					// 앞사람이 아직 덜 비켰다. 서 있는 자세로 기다린다 (걷는 자세로 제자리걸음하지 않게).
					bIsMoving = false;
					CurrentSpeed = 0.f;
					return;
				}
				const FVector Dir = (Target - Location).GetSafeNormal2D();
				FVector Next = Location + Dir * FMath::Min(Step, Remaining);
				Next.Z = Location.Z;
				if (!SnapToGround(Next))
				{
					Next.Z = Location.Z;
				}
				// 줄에서는 앞(스팟 쪽)을 보고 걷는다
				const FRotator NewRot = FMath::RInterpTo(GetActorRotation(), (-Axis).Rotation(), DeltaTime, 8.f);
				SetActorLocationAndRotation(Next, NewRot, /*bSweep=*/false);
				return;
			}
		}
	}

	// 다른 사람이 앞을 지나가는 중이면 잠깐 기다린다 (Director의 길 밖 양보).
	if (bOffLaneYield)
	{
		return;
	}

	// 서 있는 사람(이벤트하는 사람·줄 앞사람들)은 옆으로 돌아서 간다 (줄은 이벤트하는 사람 뒤에 선다)
	const FVector Dir = SubNpcSpotWalk::SteerForSpotWalk(this, Target, WalkSpeed, SteerHoldDir);
	if (Dir.IsNearlyZero())
	{
		bIsMoving = false;   // 곧 부딪힌다 - 이번 틱은 서 있는 자세로 선다
		CurrentSpeed = 0.f;
		return;
	}
	FVector Next = Location + Dir * Step;
	// 줄 자리로 옮길 때도 발밑 바닥에 붙인다 (길로 돌아갈 때와 같은 이유).
	Next.Z = Location.Z;
	if (!SnapToGround(Next))
	{
		Next.Z = FMath::FInterpConstantTo(Location.Z, Target.Z, DeltaTime, WalkSpeed);
	}
	const FRotator NewRot = FMath::RInterpTo(GetActorRotation(), Dir.Rotation(), DeltaTime, 8.f);
	bool bBlockedByWorld = false;
	SetActorLocationAndRotation(ResolveWorldBlocking(Location, Next, bBlockedByWorld), NewRot, /*bSweep=*/false);
}

void ASubNpcCharacter::LeaveSpotQueue()
{
	if (ANpcActivitySpot* Spot = QueuedSpot.Get())
	{
		Spot->LeaveQueue(this);
	}
	const bool bWasQueued = QueuedSpot.IsValid() || bQueueMoving;
	QueuedSpot = nullptr;
	bQueueMoving = false;
	QueueStepReadyTime = -1.f;
	SpotEnterReadyTime = -1.f;

	// 줄에만 서 있던 경우(아직 스팟을 차지하지 않음) 길로 걸어서 돌아간다.
	if (CurrentActivity == ESubNpcActivity::AtSpot && !OccupiedSpot)
	{
		CurrentActivity = ESubNpcActivity::Walking;
		SetMovementState(ENpcMovementState::Walk);
		BeginRejoinPath();
	}
	else if (bWasQueued)
	{
		BeginRejoinPath();
	}
}

void ASubNpcCharacter::BeginRejoinPath()
{
	bRejoiningPath = true;

	// [버그 수정] 예전에는 길을 벗어났던 지점(최대 SpotEngageRadius 앞)으로 돌아갔다.
	// 줄은 행인이 걸어오는 쪽으로 늘어서므로, 스팟에서 나온 사람이 그 대각선 위의
	// 줄 선 사람들을 그대로 뚫고 지나갔다. 줄 선 사람은 회피 계산에서도 빠져 있다.
	// 진행 위치를 지금 자리에서 가장 가까운 경로 지점으로 옮기면, 칸에서 길 쪽으로
	// 곧장 걸어 나가므로 옆으로 늘어선 줄을 가로지르지 않는다.
	if (CurrentPath)
	{
		float Closest = PathDistance;
		CurrentPath->FindClosestPointOnPath(GetActorLocation(), Closest);
		PathDistance = Closest;

		// [버그 수정] 가까운 쪽 길 가장자리 차선으로 합류한다.
		// 예전에는 원래 걷던 차선으로 돌아가서, 그게 길 건너편이면 걷는 사람들 사이를 가로질렀다.
		const FVector Base = CurrentPath->GetLocationAtDistance(PathDistance, 0.f);
		const FVector Right = (CurrentPath->GetLocationAtDistance(PathDistance, 1.f) - Base).GetSafeNormal2D();
		const float HalfWidth = CurrentPath->GetLaneHalfWidth();
		const float Lateral = FVector::DotProduct(GetActorLocation() - Base, Right);
		LaneOffset = FMath::Clamp(Lateral, -HalfWidth, HalfWidth);
		DodgeTargetOffset = LaneOffset;
		PassLaneUntil = -1.f;

		// [버그 수정] 이미 길 위에 서 있으면 합류할 필요가 없다.
		// 예전에는 부딪힘 반응이 끝난 사람처럼 원래 길 위에 있던 사람까지 "합류 중"이 되어
		// 합류할 틈을 기다리며 길 한가운데 서 있었고, 그 뒤로 정체가 생겼다.
		if (FMath::Abs(Lateral) <= HalfWidth + SubNpcSpotWalk::OnPathTolerance)
		{
			bRejoiningPath = false;
		}
		else
		{
			// 길 밖에서 돌아가는 중이면 진행 방향 앞쪽으로 비스듬히 합류한다 (들어오는 흐름과 엇갈리지 않게)
			const float Along = bReversedOnPath ? -1.f : 1.f;
			PathDistance = FMath::Clamp(PathDistance + Along * SubNpcSpotWalk::RejoinLeadDistance, 0.f, CurrentPath->GetPathLength());
		}
	}
	NextSpotCheckDistance = PathDistance;
}

void ASubNpcCharacter::BeginSpotActivity(ANpcActivitySpot* Spot, const FNpcActivityOption& Option, int32 SlotIndex)
{
	OccupiedSpot = Spot;
	CurrentActivity = ESubNpcActivity::AtSpot;

	// 단계 배열을 이어서 재생하려면 고른 행동을 들고 있어야 한다.
	CurrentSpotOption = Option;
	CurrentSpotStep = -1;   // -1 = 첫 동작(Option.AnimId)

	// 스팟 앞에 세우고, 그 행동이 바라봐야 할 지점을 향하게 한다.
	// "무슨 애니메이션 + 어디를 보는가" 이 두 개가 스팟 연출의 전부다.
	const FVector Stand = Spot->GetStandLocation(SlotIndex);
	const FVector LookAt = Spot->GetLookAtLocation(Option);

	FVector Facing = LookAt - Stand;
	Facing.Z = 0.f;
	if (!Facing.Normalize())
	{
		Facing = GetActorForwardVector();
	}

	// [버그 수정] 설 자리로 순간이동하지 않고 걸어간다.
	// 행인은 반경 SpotEngageRadius 안에서 스팟을 잡으므로, 예전에는 행동이 시작되는
	// 순간 몇 미터를 한 프레임에 튀었다. 도착하면 TickApproachSpot이 첫 동작을 시작한다.
	SpotStandLocation = Stand;
	SpotStandRotation = Facing.Rotation();
	bApproachingSpot = true;
	bRejoiningPath = false;

	SetMovementState(ENpcMovementState::Walk);
}

void ASubNpcCharacter::TickApproachSpot(float DeltaTime)
{
	const FVector Location = GetActorLocation();
	FVector ToStand = SpotStandLocation - Location;
	ToStand.Z = 0.f;
	const float Remaining = ToStand.Size();
	const float Step = WalkSpeed * DeltaTime;

	if (Remaining <= FMath::Max(Step, SubNpcSpotWalk::ArriveTolerance))
	{
		// 도착. 행동 방향으로 돌아서 첫 동작을 시작한다.
		bApproachingSpot = false;
		SetActorLocationAndRotation(SpotStandLocation, SpotStandRotation, /*bSweep=*/false);
		SetMovementState(ENpcMovementState::Interacting);
		PlaySpotStep(CurrentSpotOption.AnimId, CurrentSpotOption.MinDuration,
			CurrentSpotOption.MaxDuration, CurrentSpotOption.bLoop);
		return;
	}

	// 다른 사람이 앞을 지나가는 중이면 잠깐 기다린다 (Director의 길 밖 양보).
	if (bOffLaneYield)
	{
		return;
	}

	// 걸어가는 방향을 본다. 높이는 설 자리 쪽으로 같이 맞춰 간다.
	// 서 있는 사람은 돌아가고, 길 밖에서 움직이는 사람은 피해서 간다
	const FVector Dir = SubNpcSpotWalk::SteerForSpotWalk(this, SpotStandLocation, WalkSpeed, SteerHoldDir);
	if (Dir.IsNearlyZero())
	{
		bIsMoving = false;   // 곧 부딪힌다 - 이번 틱은 서 있는 자세로 선다
		CurrentSpeed = 0.f;
		return;
	}
	FVector Next = Location + Dir * Step;
	// 스팟까지 걸어가는 동안에도 발밑 바닥에 붙인다 (오르막을 가로지를 때 허공을 걷지 않게).
	Next.Z = Location.Z;
	if (!SnapToGround(Next))
	{
		Next.Z = FMath::FInterpConstantTo(Location.Z, SpotStandLocation.Z, DeltaTime, WalkSpeed);
	}

	const FRotator NewRot = FMath::RInterpTo(GetActorRotation(), Dir.Rotation(), DeltaTime, 8.f);
	bool bBlockedByWorld = false;
	SetActorLocationAndRotation(ResolveWorldBlocking(Location, Next, bBlockedByWorld), NewRot, /*bSweep=*/false);
}

void ASubNpcCharacter::PlaySpotStep(FName AnimId, float MinDuration, float MaxDuration, bool bLoop)
{
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;
	ActivityEndTime = Now + FMath::FRandRange(MinDuration, FMath::Max(MinDuration, MaxDuration));

	if (!AnimId.IsNone())
	{
		PlayNpcAnim(AnimId, bLoop);
	}
	else
	{
		// 애니메이션 없는 단계 = 그냥 그 시간만큼 서 있는다.
		StopNpcAnim();
	}
}

void ASubNpcCharacter::AdvanceSpotStep()
{
	const int32 NextStep = CurrentSpotStep + 1;
	if (!CurrentSpotOption.ExtraSteps.IsValidIndex(NextStep))
	{
		EndSpotActivity();
		return;
	}

	CurrentSpotStep = NextStep;
	const FNpcActivityStep& Step = CurrentSpotOption.ExtraSteps[NextStep];
	PlaySpotStep(Step.AnimId, Step.MinDuration, Step.MaxDuration, Step.bLoop);
}

void ASubNpcCharacter::EndSpotActivity()
{
	if (ANpcActivitySpot* Queued = QueuedSpot.Get())
	{
		Queued->LeaveQueue(this);
	}
	QueuedSpot = nullptr;
	bQueueMoving = false;

	if (OccupiedSpot)
	{
		OccupiedSpot->ReleaseOccupancy(this);
		OccupiedSpot = nullptr;
	}

	CurrentSpotOption = FNpcActivityOption();
	CurrentSpotStep = -1;

	// 스팟 자리에서 차선으로 순간이동하지 않고 걸어서 돌아간다 (TickPathFollow 참고).
	// 걸어가던 도중 취소된 경우도 몸이 차선 밖에 있으므로 똑같이 처리한다.
	bApproachingSpot = false;
	BeginRejoinPath();

	StopNpcAnim();
	CurrentActivity = ESubNpcActivity::Walking;
	SetMovementState(ENpcMovementState::Walk);

	// 방금 떠난 스팟에 곧바로 다시 걸리지 않도록 판정 기준을 현재 위치로 옮긴다.
	NextSpotCheckDistance = PathDistance;
}

// ---------------------------------------------------------------------------
// 플레이어 반응
// ---------------------------------------------------------------------------

void ASubNpcCharacter::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp,
	bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);

	const APawn* OtherPawn = Cast<APawn>(Other);
	if (!OtherPawn || !OtherPawn->IsPlayerControlled())
	{
		return;
	}

	// 플레이어가 나에게 밀고 들어온 경우만 반응한다. 내가 걸어가 닿은 거면 비켜 갈 일이다.
	if (GetPlayerClosingSpeed(OtherPawn, this) < PlayerBumpMinClosingSpeed)
	{
		return;
	}

	ReactToPlayerBump(Other, GetPlayerBumpSpeed(OtherPawn));
}

float ASubNpcCharacter::GetPlayerClosingSpeed(const APawn* Player, const AActor* Target)
{
	if (!Player || !Target)
	{
		return 0.f;
	}

	const FVector ToTarget = (Target->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal2D();
	float Closing = FVector::DotProduct(Player->GetVelocity(), ToTarget);

	const ACharacter* PlayerCharacter = Cast<ACharacter>(Player);
	const UCharacterMovementComponent* Movement = PlayerCharacter ? PlayerCharacter->GetCharacterMovement() : nullptr;
	if (Movement && Movement->GetMaxAcceleration() > KINDA_SMALL_NUMBER)
	{
		const FVector Intended = Movement->GetCurrentAcceleration() / Movement->GetMaxAcceleration() * Movement->GetMaxSpeed();
		Closing = FMath::Max(Closing, FVector::DotProduct(Intended, ToTarget));
	}

	return Closing;
}

float ASubNpcCharacter::GetPlayerBumpSpeed(const APawn* Player)
{
	if (!Player)
	{
		return 0.f;
	}

	float Speed = Player->GetVelocity().Size2D();

	const ACharacter* PlayerCharacter = Cast<ACharacter>(Player);
	const UCharacterMovementComponent* Movement = PlayerCharacter ? PlayerCharacter->GetCharacterMovement() : nullptr;
	if (Movement && Movement->GetMaxAcceleration() > KINDA_SMALL_NUMBER)
	{
		// 입력 세기(0~1). 아날로그 스틱을 살짝 밀면 그만큼만 친다.
		const float InputRatio = FMath::Clamp(
			Movement->GetCurrentAcceleration().Size2D() / Movement->GetMaxAcceleration(), 0.f, 1.f);
		Speed = FMath::Max(Speed, Movement->GetMaxSpeed() * InputRatio);
	}

	return Speed;
}

void ASubNpcCharacter::ReactToPlayerBump(AActor* Player, float ImpactSpeed)
{
	const UWorld* World = GetWorld();
	// 사라지는 중인 사람은 반응하지 않는다 (페이드가 끊기면 반쯤 투명한 채로 남는다).
	if (!World || !Player || bExitFading)
	{
		return;
	}

	// 걷거나 스팟에서 뭔가 하던 중일 때만. 풀에 있거나 이미 반응·대화 중이면 무시한다.
	if (CurrentActivity != ESubNpcActivity::Walking && CurrentActivity != ESubNpcActivity::AtSpot)
	{
		// 반응 중에는 매 프레임 닿으므로 여기서 찍으면 도배된다. 조용히 넘긴다.
		return;
	}

	// 붙어 선 채로 매 프레임 반응하면 안 된다.
	const float Now = World->GetTimeSeconds();
	if (Now - LastReactionTime < ReactionCooldown)
	{
		return;
	}

	// 줄 서서 기다리던 중에 부딪혔으면 줄에서 빠진다. 반응이 끝나면 길로 돌아간다.
	if (QueuedSpot.IsValid() && !OccupiedSpot)
	{
		LeaveSpotQueue();
	}

	// [설계] 반응은 **부딪힌 속도만으로** 고른다. 랜덤도, 무게·에너지 같은 다른 값도 없다.
	//
	// 예전에는 기준 속도를 넘으면 피하기/화내기/놀라기를 가중치 랜덤으로 뽑았다.
	// 그래서 같은 속도로 부딪혀도 매번 달랐고, 절반은 걸을 때와 거의 같은 가벼운
	// 모션(피하기)이 나와서 "빨리 달려도 똑같다"로 보였다.
	// 이제 아키타입 테이블의 BumpTiers(속도 구간 → 반응·애니메이션)가 전부 정한다.
	const FSubNpcBumpTier* Tier = Archetype.FindBumpTier(ImpactSpeed);
	if (!Tier || Tier->Reaction == ESubNpcReaction::None)
	{
		SubNpcBumpDebug::Print(this,
			FString::Printf(TEXT("속도 %.0f → 해당 구간 없음 (BumpTiers %d개). 반응 안 함"),
				ImpactSpeed, Archetype.BumpTiers.Num()),
			FColor::Red);
		return;
	}

	const ESubNpcReaction Reaction = Tier->Reaction;
	CurrentReaction = Reaction;

	LastReactionTime = Now;
	LastBumpTime = Now;

	// ---- 부딪힌 방향 판정 ---------------------------------------------------
	//
	// 플레이어가 내 오른쪽에 있으면 오른쪽에서 부딪힌 것이므로 **왼쪽으로** 밀린다.
	// 그 반대도 마찬가지다. 좌우가 반대로 나오면 여기 부호만 뒤집으면 된다.
	const FVector ToPlayerDir = Player->GetActorLocation() - GetActorLocation();
	const float SideDot = FVector::DotProduct(ToPlayerDir, GetActorRightVector());

	// 정확히 정면이면 판정이 흔들린다. 이름 해시로 한쪽을 정해 매번 같게 만든다.
	const bool bPushedLeft = (FMath::Abs(SideDot) > 1.f)
		? (SideDot > 0.f)
		: ((GetTypeHash(GetFName()) & 1) != 0);

	// 왼쪽으로 밀리면 차선 오프셋은 음수 방향으로 간다.
	const float PushSign = bPushedLeft ? -1.f : 1.f;

	// 걷던 중이면 "비켜서는" 동작, 서 있었으면 "밀리는" 동작이 자연스럽다.
	const bool bWasWalking = (CurrentActivity == ESubNpcActivity::Walking);

	// [개선] 스팟에서 뭔가 하던 중이었으면 반응이 끝난 뒤 하던 일로 되돌아간다.
	//
	// 예전에는 점유를 즉시 풀고 걷기로 빠져나갔다. 그래서 앉아서 책을 보던 사람이
	// 한 번 부딪히면 벌떡 일어나 걸어가 버렸다. 놀란 뒤 다시 앉는 쪽이 자연스럽고,
	// 스팟이 비지 않으므로 그 자리를 다른 개체가 가로채지도 않는다.
	bResumeSpotAfterReaction = (CurrentActivity == ESubNpcActivity::AtSpot && OccupiedSpot != nullptr);

	CurrentActivity = ESubNpcActivity::Reacting;
	ActivityEndTime = Now + ReactionDuration;

	FName AnimId = Tier->AnimId;
	switch (Reaction)
	{
	case ESubNpcReaction::Dodge:
		{
			// 방향 × 상태(걷는 중/서 있음)로 네 가지 중 하나를 고른다.
			// 비어 있으면 구간의 AnimId를 쓴다 - 애니메이션을 하나만
			// 만들어도 동작하고, 나중에 하나씩 채워 넣을 수 있다.
			const FName Specific = bWasWalking
				? (bPushedLeft ? Archetype.DodgeLeftAnimId : Archetype.DodgeRightAnimId)
				: (bPushedLeft ? Archetype.ShoveLeftAnimId : Archetype.ShoveRightAnimId);

			if (!Specific.IsNone())
			{
				AnimId = Specific;
			}

			// 밀린 쪽으로 실제로 몸을 옮긴다. 애니메이션만 재생하고 제자리면
			// "맞았는데 안 밀리는" 어색함이 남는다.
			DodgeTargetOffset = LaneOffset + PushSign * DodgeSideStep;
		}
		break;

	case ESubNpcReaction::LightBump:
		// 제자리에서 잠깐 멈칫한다. 옆으로 밀리지 않고, 큰 반응보다 짧게 끝난다.
		// 길면 가볍게 스쳤는데도 한참 서 있어서 길을 막는다.
		ActivityEndTime = Now + SubNpcLightBump::Duration;
		DodgeTargetOffset = LaneOffset;
		FaceTowards(Player);
		break;

	case ESubNpcReaction::Angry:
		// 화를 내도 몸은 부딪힌 만큼 밀린다. 물리적 반응과 감정 반응은 별개다.
		DodgeTargetOffset = LaneOffset + PushSign * DodgeSideStep * 0.4f;
		// 화낼 때는 멈춰서 플레이어를 쳐다본다.
		FaceTowards(Player);
		break;

	case ESubNpcReaction::Surprised:
	default:
		DodgeTargetOffset = LaneOffset + PushSign * DodgeSideStep * 0.6f;
		FaceTowards(Player);
		break;
	}

	const bool bPlayed = !AnimId.IsNone() && PlayNpcAnim(AnimId, /*bLoop=*/false);

	{
		const FString ReactionName = SubNpcBumpDebug::ReactionToString(Reaction);
		const FString Result = AnimId.IsNone()
			? TEXT("애니메이션 키 비어 있음")
			: (bPlayed ? TEXT("재생 OK") : TEXT("재생 실패 - BP AnimLibrary에 이 키가 있는지 확인"));

		SubNpcBumpDebug::Print(this,
			FString::Printf(TEXT("속도 %.0f → 구간 %.0f+ → %s / 애니 '%s' → %s"),
				ImpactSpeed, Tier->MinSpeed, *ReactionName, *AnimId.ToString(), *Result),
			bPlayed ? FColor::Green : FColor::Orange);
	}

	SetMovementState(ENpcMovementState::Interacting);

	// 블루프린트 연출 훅. 몽타주·사운드·이펙트를 붙이고 싶을 때 쓴다.
	OnBumpReaction(Reaction, bPushedLeft, bWasWalking);

	OnBumped.Broadcast(Player);
}

void ASubNpcCharacter::EndReaction()
{
	StopNpcAnim();
	CurrentReaction = ESubNpcReaction::None;

	// [개선] 하던 스팟 행동이 있었으면 그리로 되돌아간다.
	// 점유는 계속 들고 있었으므로 자리를 뺏겼을 걱정은 없다.
	if (bResumeSpotAfterReaction && OccupiedSpot)
	{
		bResumeSpotAfterReaction = false;
		CurrentActivity = ESubNpcActivity::AtSpot;

		// 아직 설 자리로 걸어가던 중이었으면 마저 걸어간다. 동작은 도착해서 시작한다.
		if (bApproachingSpot)
		{
			SetMovementState(ENpcMovementState::Walk);
			return;
		}

		SetMovementState(ENpcMovementState::Interacting);

		// 처음부터 다시 하지 않고 하던 단계를 이어서 재생한다.
		const int32 Step = CurrentSpotStep;
		if (Step < 0)
		{
			PlaySpotStep(CurrentSpotOption.AnimId, CurrentSpotOption.MinDuration,
				CurrentSpotOption.MaxDuration, CurrentSpotOption.bLoop);
		}
		else if (CurrentSpotOption.ExtraSteps.IsValidIndex(Step))
		{
			const FNpcActivityStep& S = CurrentSpotOption.ExtraSteps[Step];
			PlaySpotStep(S.AnimId, S.MinDuration, S.MaxDuration, S.bLoop);
		}
		else
		{
			EndSpotActivity();
		}
		return;
	}

	bResumeSpotAfterReaction = false;

	// 일행 대기 중에 부딪혔으면 대기 자리로 돌아간다 (일행이 아직 기다리는 중일 때만)
	if (bGroupWaiting && GroupWaitSpot.IsValid())
	{
		CurrentActivity = ESubNpcActivity::AtSpot;
		bGroupWaitMoving = true;
		SetMovementState(ENpcMovementState::Walk);
		return;
	}
	bGroupWaiting = false;
	bGroupWaitMoving = false;

	CurrentActivity = ESubNpcActivity::Walking;
	SetMovementState(ENpcMovementState::Walk);

	// 차선 밖(줄 자리·스팟으로 가던 길)에서 반응했을 수 있으므로 걸어서 차선으로 돌아간다.
	// 이미 차선 위에 있었다면 첫 틱에 바로 끝난다.
	bApproachingSpot = false;
	BeginRejoinPath();

	// 비켰던 차선은 그대로 두고 계속 걷는다 (원래 자리로 홱 돌아오면 어색하다).
	DodgeTargetOffset = LaneOffset;
}

// ---------------------------------------------------------------------------
// 상호작용 (한 줄 대사)
// ---------------------------------------------------------------------------

bool ASubNpcCharacter::CanInteract_Implementation(AActor* Interactor) const
{
	if (!bAllowTalk || CurrentActivity == ESubNpcActivity::Pooled || bExitFading)
	{
		return false;
	}

	// 멀리 있는(저LOD) 행인에게는 프롬프트를 띄우지 않는다.
	if (CurrentLodBand == ESubNpcLodBand::Far || CurrentLodBand == ESubNpcLodBand::Culled)
	{
		return false;
	}

	// [버그 수정] 할 말이 없으면 프롬프트도 띄우지 않는다.
	// 예전에는 대사가 비어 있어도 E 프롬프트가 떠서, 눌러도 아무 일이 없었다.
	// 대사는 아키타입 테이블(프로젝트 설정 > NPC Setting)의 IdleBarks / BumpedBarks에서 온다.
	FText Unused;
	if (!PickBarkLine(Unused))
	{
		return false;
	}

	return Super::CanInteract_Implementation(Interactor);
}

FText ASubNpcCharacter::GetInteractionPrompt_Implementation(AActor* Interactor) const
{
	return NSLOCTEXT("Npc", "SubNpcTalkPrompt", "E - 말 걸기");
}

void ASubNpcCharacter::Interact_Implementation(AActor* Interactor)
{
	Super::Interact_Implementation(Interactor);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UDialogueSubsystem* Dialogue = World->GetSubsystem<UDialogueSubsystem>();
	if (!Dialogue || Dialogue->IsDialogueActive())
	{
		return;
	}

	FText Line;
	if (!PickBarkLine(Line))
	{
		return;
	}

	// 행인은 대화 트리를 갖지 않는다. 한 줄만 띄우고 끝.
	// DataTable도, 이벤트 등록도 필요 없어서 행인이 몇 명이든 데이터 비용이 늘지 않는다.
	FaceTowards(Interactor);

	const FText Speaker = Archetype.DisplayName.IsEmpty()
		? NSLOCTEXT("Npc", "SubNpcDefaultName", "행인")
		: Archetype.DisplayName;

	if (!Dialogue->StartSimpleLines(Speaker, { Line }, this))
	{
		return;
	}

	// 줄 서서 기다리던 사람에게 말을 걸었으면 줄에서 빠진다. 대화가 끝나면 길로 돌아간다.
	if (QueuedSpot.IsValid() && !OccupiedSpot)
	{
		LeaveSpotQueue();
	}

	// [버그 수정] 대화창이 떠 있는 동안 멈춰 선다.
	// 예전에는 말을 걸어도 계속 걸어가서, 대사를 읽는 사이 화면 밖으로 사라졌다.
	// 대화가 끝났는지는 Tick에서 확인하고, 끝나면 EndReaction이 하던 일로 돌려놓는다.
	if (CurrentActivity == ESubNpcActivity::Walking || CurrentActivity == ESubNpcActivity::AtSpot)
	{
		bResumeSpotAfterReaction = (CurrentActivity == ESubNpcActivity::AtSpot && OccupiedSpot != nullptr);
	}
	// 반응 도중에 말을 걸었으면 그 반응이 정해둔 복귀 경로(bResumeSpotAfterReaction)를 그대로 쓴다.

	CurrentActivity = ESubNpcActivity::Reacting;
	CurrentReaction = ESubNpcReaction::Talking;
	ActivityEndTime = World->GetTimeSeconds() + SubNpcTalk::MaxDuration;
	DodgeTargetOffset = LaneOffset;
	SetMovementState(ENpcMovementState::Interacting);
}

bool ASubNpcCharacter::PickBarkLine(FText& OutLine) const
{
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	// 방금 부딪혔던 상대가 말을 걸면 다른 대사가 나온다.
	// 비용은 배열 하나인데 "얘가 나를 기억한다"는 인상이 커서 가성비가 제일 좋다.
	const bool bRecentlyBumped = (Now - LastBumpTime) < BumpMemoryDuration;
	const TArray<FText>& Pool = (bRecentlyBumped && Archetype.BumpedBarks.Num() > 0)
		? Archetype.BumpedBarks
		: Archetype.IdleBarks;

	if (Pool.Num() == 0)
	{
		return false;
	}

	OutLine = Pool[FMath::RandRange(0, Pool.Num() - 1)];
	return true;
}

// ---------------------------------------------------------------------------
// 그룹
// ---------------------------------------------------------------------------

FVector ASubNpcCharacter::ResolveWorldBlocking(const FVector& From, const FVector& To, bool& bOutBlocked) const
{
	SUBNPC_PROFILE_SCOPE(WorldBlock);

	bOutBlocked = false;

	const UWorld* World = GetWorld();
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (SubNpcWorldBlock::Enabled == 0 || !World || !Capsule
		|| (CurrentLodBand != ESubNpcLodBand::Near && CurrentLodBand != ESubNpcLodBand::Mid))
	{
		return To;
	}

	const FVector Delta = To - From;
	if (Delta.SizeSquared2D() < FMath::Square(0.1f))
	{
		return To;
	}

	// 발밑의 계단·턱·바닥에 걸리지 않게 캡슐 아래쪽을 StepHeight만큼 들어 올려서 쓸어 본다.
	// 높이는 지면 스냅이 따로 맞추므로 여기서는 수평으로 막히는지만 본다.
	const float Radius = Capsule->GetScaledCapsuleRadius() * 0.9f;
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float SweepHalfHeight = FMath::Max(Radius, HalfHeight - SubNpcWorldBlock::StepHeight * 0.5f);
	const FVector Lift(0.f, 0.f, SubNpcWorldBlock::StepHeight * 0.5f);
	const FVector Start = FVector(From.X, From.Y, To.Z) + Lift;
	const FVector End = To + Lift;

	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SubNpcWorldBlock), /*bTraceComplex=*/false, this);
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius, SweepHalfHeight);

	// 오브젝트 타입 검사는 충돌 응답을 보지 않아서 트리거 박스 같은 겹침 전용 볼륨에도 걸린다.
	// 걸린 것들 중 사람(Pawn)을 실제로 막는 것만 벽으로 친다.
	auto SweepBlocking = [&](const FVector& A, const FVector& B, FHitResult& OutHit)
	{
		TArray<FHitResult> Hits;
		World->SweepMultiByObjectType(Hits, A, B, FQuat::Identity, Objects, Shape, Params);
		const FHitResult* Best = nullptr;
		for (const FHitResult& H : Hits)
		{
			const UPrimitiveComponent* Comp = H.GetComponent();
			if (!Comp || Comp->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block)
			{
				continue;
			}
			// 위를 향한 면은 걸을 수 있는 바닥·경사로다. 높이는 지면 스냅이 맞추므로 벽으로 치지 않는다.
			if (H.ImpactNormal.Z > SubNpcWorldBlock::WalkableNormalZ)
			{
				continue;
			}
			if (!Best || H.Time < Best->Time)
			{
				Best = &H;
			}
		}
		if (Best)
		{
			OutHit = *Best;
			return true;
		}
		return false;
	};

	FHitResult Hit;
	if (!SweepBlocking(Start, End, Hit))
	{
		return To;
	}

	// 이미 벽 안에 끼어 있으면 막지 않는다. 막으면 영영 못 빠져나온다.
	if (Hit.bStartPenetrating)
	{
		return To;
	}

	bOutBlocked = true;

	// 벽까지 가고, 남은 이동은 벽면을 따라 미끄러진다 (벽을 따라 걷는 그림).
	const FVector HitPoint = Start + (End - Start) * Hit.Time - Lift;
	const FVector Remaining = (End - Start) * (1.f - Hit.Time);
	FVector Normal2D = Hit.ImpactNormal;
	Normal2D.Z = 0.f;
	if (!Normal2D.Normalize())
	{
		return HitPoint;
	}
	const FVector Slide = FVector::VectorPlaneProject(Remaining, Normal2D);

	FHitResult SlideHit;
	const FVector SlideStart = HitPoint + Lift + Normal2D * 0.5f;
	if (SweepBlocking(SlideStart, SlideStart + Slide, SlideHit) && !SlideHit.bStartPenetrating)
	{
		return SlideStart + Slide * SlideHit.Time - Lift;
	}
	return SlideStart + Slide - Lift;
}

bool ASubNpcCharacter::StartGroupChat(float Duration, FName TalkAnimId, int32 MemberIndex, int32 MemberCount, float TurnLength,
	ASubNpcCharacter* Partner, FName ListenAnimId)
{
	const UWorld* World = GetWorld();
	// 걷는 중이거나, 일행이 스팟에 들러 길가에서 기다리는 중이어야 한다
	if (!World || (!IsWalkingOnPath() && !bGroupWaiting) || TalkAnimId.IsNone())
	{
		return false;
	}
	// 이미 다른 동작을 걸어 두었으면 거둔다 (새 대화의 역할로 다시 건다)
	if (bGroupChatting)
	{
		StopGroupChat();
	}

	// [변경] 예전에는 길에서 멈춰 서로 마주 보고 이야기했다. 걸음을 멈추면 뒤가 막히고
	// 대화로도 잘 안 읽혀서, 이제는 걸으면서 이야기한다. 상체 슬롯에만 손짓을 얹으므로
	// 하체는 그대로 걷는다 (ABP_Passerby의 본별 레이어 블렌딩, spine_01부터 위).
	const float Now = World->GetTimeSeconds();
	bGroupChatting = true;
	bGroupChatGesturing = false;
	GroupChatStartTime = Now;
	GroupChatEndTime = Now + Duration;
	GroupChatTurnLength = FMath::Max(0.5f, TurnLength);
	GroupChatIndex = MemberIndex;
	GroupChatCount = FMath::Max(1, MemberCount);
	GroupChatAnimId = TalkAnimId;
	GroupListenAnimId = ListenAnimId;
	GroupChatShownRole = -1;
	GroupChatPartner = Partner;

	TickGroupChat(Now);
	return true;
}

void ASubNpcCharacter::TickGroupChat(float Now)
{
	if (!bGroupChatting)
	{
		return;
	}

	// 걷기를 그만뒀으면(스팟·반응·대화창) 일행 대화도 끝낸다. 상체 손짓이 그 동작 위에 얹히면 어색하다.
	// 일행 대기(상태는 AtSpot)는 예외 - 기다리는 동안 이야기하는 게 연출이다.
	if (Now >= GroupChatEndTime || (CurrentActivity != ESubNpcActivity::Walking && !bGroupWaiting))
	{
		StopGroupChat();
		return;
	}

	// 번갈아 말한다. 한 사람씩 TurnLength초 동안 손짓하고 다음 사람에게 넘긴다.
	// 모두 동시에 손짓하면 대화가 아니라 단체 체조처럼 보인다.
	const int32 Turn = FMath::FloorToInt((Now - GroupChatStartTime) / GroupChatTurnLength);
	const bool bMyTurn = (Turn % GroupChatCount) == GroupChatIndex;

	// [그룹 동작] 말하는 차례면 말하기, 아니면 듣기 동작을 상체에 건다. 듣기 동작이 없으면 손짓을 거둔다.
	// 같은 슬롯에 다시 걸면 앞 동작에서 블렌드로 넘어간다. 역할이 바뀔 때만 건다.
	const int8 WantRole = bMyTurn ? 1 : (GroupListenAnimId.IsNone() ? -1 : 0);
	if (WantRole == GroupChatShownRole || !CharacterAnimationComponent)
	{
		return;
	}
	if (WantRole < 0)
	{
		CharacterAnimationComponent->StopAnimOnSlot(SubNpcGroupChat::UpperBodySlot, SubNpcGroupChat::GestureBlendOut);
		bGroupChatGesturing = false;
	}
	else
	{
		const FName AnimId = (WantRole == 1) ? GroupChatAnimId : GroupListenAnimId;
		bGroupChatGesturing = CharacterAnimationComponent->PlayAnimOnSlot(AnimId, SubNpcGroupChat::UpperBodySlot, /*bLoop=*/true);
	}
	GroupChatShownRole = WantRole;
}

void ASubNpcCharacter::StopGroupChat()
{
	if (bGroupChatGesturing && CharacterAnimationComponent)
	{
		CharacterAnimationComponent->StopAnimOnSlot(SubNpcGroupChat::UpperBodySlot, SubNpcGroupChat::GestureBlendOut);
	}
	bGroupChatting = false;
	bGroupChatGesturing = false;
	GroupChatShownRole = -1;
	GroupChatPartner = nullptr;
}

// ---------------------------------------------------------------------------
// 일행 스팟 들르기
//
// 일행 중 한 명(리더 아님)이 "일행 들르기"를 켠 스팟에 들어가면, 리더와 나머지 일행은 스팟 옆 대기 자리
// (스팟의 초록 원)로 비켜서서 그룹 동작을 하며 기다린다. 들른 사람이 스팟에서 나오면 모두 길로 돌아가
// 같이 떠난다. 너무 오래(스팟의 MaxGroupVisitWait) 걸리면 들른 사람만 일행에서 빠지고 나머지는 떠난다.
// 예전처럼 길 한가운데서 기다리면 그 뒤로 정체가 생겼으므로(측정: 스팟 옆에 20명 넘게 정지) 길 밖에서 기다린다.
// ---------------------------------------------------------------------------

bool ASubNpcCharacter::GetSpotDebugState(FString& OutLabel, FVector& OutTarget) const
{
	const TCHAR* Yield = bOffLaneYield ? TEXT(" (양보)") : TEXT("");
	OutTarget = GetActorLocation();
	if (bGroupWaiting)
	{
		OutLabel = FString::Printf(TEXT("일행 대기%s%s"), bGroupWaitMoving ? TEXT(" 이동") : TEXT(""), Yield);
		OutTarget = GroupWaitLocation;
		return true;
	}
	if (const ANpcActivitySpot* Queued = QueuedSpot.Get(); Queued && !OccupiedSpot)
	{
		const int32 Index = Queued->GetQueueIndex(this);
		OutLabel = FString::Printf(TEXT("줄%d %s%s"), Index + 1, bQueueMoving ? TEXT("이동") : TEXT("대기"), Yield);
		OutTarget = (Index != INDEX_NONE) ? Queued->GetQueueLocation(Index) : GetActorLocation();
		return true;
	}
	if (OccupiedSpot)
	{
		OutLabel = bApproachingSpot ? FString::Printf(TEXT("칸으로%s"), Yield) : FString(TEXT("이벤트"));
		OutTarget = bApproachingSpot ? SpotStandLocation : GetActorLocation();
		return true;
	}
	if (CurrentActivity == ESubNpcActivity::Walking && bRejoiningPath)
	{
		OutLabel = FString::Printf(TEXT("길로 복귀%s"), Yield);
		if (CurrentPath)
		{
			OutTarget = CurrentPath->GetLocationAtDistance(PathDistance, LaneOffset);
		}
		return true;
	}
	if (CurrentActivity == ESubNpcActivity::Reacting)
	{
		OutLabel = TEXT("반응");
		return true;
	}
	return false;
}

void ASubNpcCharacter::BeginGroupSpotVisit(ASubNpcCharacter* Visitor, ANpcActivitySpot* Spot)
{
	UWorld* World = GetWorld();
	USubNpcDirectorSubsystem* Director = World ? World->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr;
	if (!Visitor || !Spot || !Director || IsHostingGroupVisit())
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	GroupSpotVisitor = Visitor;
	GroupVisitDeadline = Now + Spot->GetMaxGroupVisitWait();

	// 기다릴 사람: 리더(나) + 들른 사람을 뺀 일행
	TArray<ASubNpcCharacter*, TInlineAllocator<5>> Waiters;
	Waiters.Add(this);
	int32 GroupSize = 1;
	for (ASubNpcCharacter* Npc : Director->GetActiveNpcs())
	{
		if (IsValid(Npc) && Npc->GroupLeader.Get() == this)
		{
			++GroupSize;
			if (Npc != Visitor && Npc->IsWalkingOnPath())
			{
				Waiters.Add(Npc);
			}
		}
	}

	// 대기 중심을 둘러싸고 서서 서로 마주 본다 (모여 이야기하는 무리). 스팟 쪽 자리부터 채운다.
	const FVector WaitCenter = Spot->GetGroupWaitCenter();
	for (int32 i = 0; i < Waiters.Num(); ++i)
	{
		const FVector Location = Spot->GetGroupWaitLocation(i, Waiters.Num());
		FVector ToCenter = WaitCenter - Location;
		ToCenter.Z = 0.f;
		if (ToCenter.IsNearlyZero())
		{
			ToCenter = Spot->GetActorLocation() - Location;   // 혼자 기다리면 스팟을 본다
			ToCenter.Z = 0.f;
		}
		const FRotator Facing = ToCenter.IsNearlyZero() ? Spot->GetActorRotation() : ToCenter.Rotation();
		Waiters[i]->BeginGroupWait(Spot, Location, Facing);
	}

	// 기다리는 동안 이야기한다 (무리 크기별 동작). 기다리는 시간 내내 이어진다.
	if (Waiters.Num() >= 2)
	{
		Director->StartGroupChatFor(this, Waiters, GroupSize, Spot->GetMaxGroupVisitWait());
	}

	SUBNPC_LOG(Log, TEXT("[CrowdGroup] 일행 스팟 들르기 시작: %s 무리 %d명 | %s 이(가) %s 에 들어감, %d명이 기다림"),
		*GetName(), GroupSize, *Visitor->GetName(), *Spot->GetName(), Waiters.Num());
}

void ASubNpcCharacter::BeginGroupWait(ANpcActivitySpot* Spot, const FVector& WaitLocation, const FRotator& WaitRotation)
{
	StopGroupChat();
	bGroupWaiting = true;
	bGroupWaitMoving = true;
	GroupWaitSpot = Spot;
	GroupWaitLocation = WaitLocation;
	GroupWaitRotation = WaitRotation;

	CurrentActivity = ESubNpcActivity::AtSpot;
	bApproachingSpot = false;
	bRejoiningPath = false;
	SetMovementState(ENpcMovementState::Walk);
}

void ASubNpcCharacter::EndGroupWait()
{
	if (!bGroupWaiting)
	{
		return;
	}
	StopGroupChat();
	bGroupWaiting = false;
	bGroupWaitMoving = false;
	GroupWaitSpot = nullptr;

	// 반응 중이면 반응이 끝나고 알아서 길로 돌아간다 (EndReaction)
	if (CurrentActivity == ESubNpcActivity::AtSpot)
	{
		CurrentActivity = ESubNpcActivity::Walking;
		SetMovementState(ENpcMovementState::Walk);
		BeginRejoinPath();
	}
}

void ASubNpcCharacter::TickGroupWait(float DeltaTime, float Now)
{
	// 리더: 들른 사람이 끝났는지(스팟을 떠남·회수됨), 너무 오래 걸리는지 본다.
	if (IsHostingGroupVisit() || GroupSpotVisitor.IsStale())
	{
		const ASubNpcCharacter* Visitor = GroupSpotVisitor.Get();
		// 줄에 서 있는 동안도 아직 끝나지 않은 것이다 (일행도 줄을 선다)
		const bool bVisitorDone = !IsValid(Visitor) || Visitor->IsPooled()
			|| (Visitor->OccupiedSpot != GroupWaitSpot.Get() && Visitor->QueuedSpot.Get() != GroupWaitSpot.Get()
				&& Visitor->CurrentActivity != ESubNpcActivity::Reacting);
		if (bVisitorDone || Now >= GroupVisitDeadline || !GroupWaitSpot.IsValid())
		{
			EndGroupSpotVisit(!bVisitorDone);
			return;
		}
	}
	else
	{
		// 일행: 리더가 사라졌거나 기다리기를 끝냈으면 나도 그만 기다린다 (영영 서 있지 않게)
		const ASubNpcCharacter* Leader = GroupLeader.Get();
		if (!IsValid(Leader) || Leader->IsPooled() || !Leader->IsHostingGroupVisit())
		{
			EndGroupWait();
			return;
		}
	}

	if (!bGroupWaitMoving)
	{
		return;
	}

	// 대기 자리로 걸어간다 (스팟·줄로 가는 걸음과 같다)
	const FVector Location = GetActorLocation();
	FVector ToTarget = GroupWaitLocation - Location;
	ToTarget.Z = 0.f;
	const float Remaining = ToTarget.Size();
	const float Step = WalkSpeed * DeltaTime;
	if (Remaining <= FMath::Max(Step, SubNpcSpotWalk::ArriveTolerance))
	{
		bGroupWaitMoving = false;
		SetActorLocationAndRotation(FVector(GroupWaitLocation.X, GroupWaitLocation.Y, Location.Z), GroupWaitRotation, /*bSweep=*/false);
		SetMovementState(ENpcMovementState::Idle);
		return;
	}

	// 다른 사람이 앞을 지나가는 중이면 잠깐 기다린다 (Director의 길 밖 양보)
	if (bOffLaneYield)
	{
		return;
	}

	// 서 있는 사람(이벤트·줄·먼저 온 일행)은 돌아가고, 길 밖에서 움직이는 사람은 피해서 간다
	const FVector Dir = SubNpcSpotWalk::SteerForSpotWalk(this, GroupWaitLocation, WalkSpeed, SteerHoldDir);
	if (Dir.IsNearlyZero())
	{
		bIsMoving = false;   // 곧 부딪힌다 - 이번 틱은 서 있는 자세로 선다
		CurrentSpeed = 0.f;
		return;
	}
	FVector Next = Location + Dir * Step;
	Next.Z = Location.Z;
	if (!SnapToGround(Next))
	{
		Next.Z = FMath::FInterpConstantTo(Location.Z, GroupWaitLocation.Z, DeltaTime, WalkSpeed);
	}
	const FRotator NewRot = FMath::RInterpTo(GetActorRotation(), Dir.Rotation(), DeltaTime, 8.f);
	bool bBlockedByWorld = false;
	SetActorLocationAndRotation(ResolveWorldBlocking(Location, Next, bBlockedByWorld), NewRot, /*bSweep=*/false);
}

void ASubNpcCharacter::EndGroupSpotVisit(bool bTimedOut)
{
	ASubNpcCharacter* Visitor = GroupSpotVisitor.Get();
	GroupSpotVisitor = nullptr;

	const UWorld* World = GetWorld();
	SUBNPC_LOG(Log, TEXT("[CrowdGroup] 일행 스팟 들르기 끝: %s | %s, %.1f초 기다림"),
		*GetName(), bTimedOut ? TEXT("시간 초과 - 들른 사람은 일행에서 빠짐") : TEXT("들른 사람이 나옴 - 같이 출발"),
		(World && GroupWaitSpot.IsValid())
			? World->GetTimeSeconds() - (GroupVisitDeadline - GroupWaitSpot->GetMaxGroupVisitWait()) : 0.f);

	// 너무 오래 걸렸으면 들른 사람만 일행에서 빠진다 (혼자 다니는 행인이 된다)
	if (bTimedOut && IsValid(Visitor) && Visitor->GroupLeader.Get() == this)
	{
		Visitor->GroupLeader = nullptr;
	}

	// 나(리더)부터 길로 돌아가고, 일행은 다음 틱에 리더가 기다리기를 끝낸 걸 보고 따라 나온다
	EndGroupWait();
}

void ASubNpcCharacter::SetGroupLeader(ASubNpcCharacter* Leader, float FollowGap, float InLaneOffset)
{
	GroupLeader = Leader;
	GroupFollowGap = FollowGap;

	// [변경] 차선 오프셋은 리더 기준 상대값이다. 예전에는 길 기준 절대값이라, 리더가 길 한쪽 끝에
	// 있으면 일행이 반대쪽 끝에 배정돼 150cm씩 떨어져 걸었다 (일행으로 안 보였다).
	GroupLaneOffset = InLaneOffset;
	LaneOffset = Leader ? Leader->LaneOffset + InLaneOffset : InLaneOffset;

	if (Leader)
	{
		// 리더와 같은 경로·방향을 공유해야 간격 계산이 맞는다.
		// [버그 수정] 이미 같은 경로·방향에 서 있으면 진행도를 덮어쓰지 않는다. 등장할 자리가 붐비면 SpawnOnPath가
		// 경로를 따라 앞으로 밀어 세우는데, 여기서 진행도를 "리더 - 간격"으로 덮으면 몸은 수 미터 앞에 있고
		// 목표점은 리더 뒤라서, 몸이 사람들 사이를 거슬러 걸어 목표점으로 돌아갔다
		// (로그: 경로 A 입구에서 일행 진행도 0~60, 몸은 리더 옆, 목표점과 2~9m 차이 → 겹침의 대부분).
		// 앞에 섰으면 간격 보정이 천천히 걷게 해서 리더 뒤로 자리를 잡는다.
		const bool bSamePath = (CurrentPath == Leader->CurrentPath && bReversedOnPath == Leader->bReversedOnPath);
		CurrentPath = Leader->CurrentPath;
		bReversedOnPath = Leader->bReversedOnPath;
		if (!bSamePath)
		{
			PathDistance = Leader->PathDistance + (bReversedOnPath ? FollowGap : -FollowGap);
		}

		// 등장한 그 프레임에 부르므로 몸을 새 목표점(리더 옆 차선)으로 바로 옮긴다. 안 옮기면 그 차이만큼 걸어서 따라간다.
		if (CurrentPath)
		{
			PathDistance = FMath::Clamp(PathDistance, 0.f, CurrentPath->GetPathLength());
			FVector Location = CurrentPath->GetLocationAtDistance(PathDistance, LaneOffset);
			SnapToGround(Location);
			SetActorLocation(Location);
		}

		// 일행은 리더와 거의 같은 속도로 걷되 아주 조금씩 다르게 한다.
		// 완전히 같으면 발걸음까지 똑같아 행진처럼 보인다. 벌어지는 건 위의 간격 보정이 잡아준다.
		WalkSpeed = Leader->WalkSpeed * FMath::FRandRange(
			1.f - SubNpcGroupTuning::MemberSpeedJitter, 1.f + SubNpcGroupTuning::MemberSpeedJitter);

		// 리더와 같은 다음 경로·갈아탈 지점을 쓴다.
		PlanNextPath();
	}
}

void ASubNpcCharacter::SampleMeasuredVelocity(float Now)
{
	const FVector Location = GetActorLocation();
	const float Dt = Now - LastMeasureTime;
	if (LastMeasureTime >= 0.f && Dt > KINDA_SMALL_NUMBER && Dt < 0.5f)
	{
		const FVector2D Raw = FVector2D(Location - LastMeasureLocation) / Dt;
		// 순간이동(풀에서 꺼냄·일행 맞추기)은 속도가 아니다
		MeasuredVelocity = (Raw.SizeSquared() < FMath::Square(WalkSpeed * 2.f + 100.f)) ? Raw : FVector2D::ZeroVector;
	}
	else
	{
		MeasuredVelocity = FVector2D::ZeroVector;
	}
	LastMeasureLocation = Location;
	LastMeasureTime = Now;
}

bool ASubNpcCharacter::IsSpotStandClear(const ANpcActivitySpot* Spot) const
{
	const UWorld* World = GetWorld();
	const USubNpcDirectorSubsystem* Director = World ? World->GetSubsystem<USubNpcDirectorSubsystem>() : nullptr;
	if (!Spot || !Director)
	{
		return true;
	}
	const FVector Stand = Spot->GetStandLocation(0);
	for (const ASubNpcCharacter* Other : Director->GetActiveNpcs())
	{
		if (Other != this && IsValid(Other) && !Other->IsPooled()
			&& FVector::DistSquared2D(Other->GetActorLocation(), Stand) < FMath::Square(SubNpcSpotWalk::StandClearRadius))
		{
			return false;
		}
	}
	return true;
}
