# 서브 NPC 군중 시스템 — 성능 측정 기록

프로젝트 Febricula / UE 5.8.1 / 측정일 2026-09-10

> **참고** 이 문서는 2026-09-10 측정(ABP 교체) 기록이다. 이후 Mass 전환 · 원경 인스턴싱 결과는 맨 아래 [9. 이후 진행 (2026-10)](#9-이후-진행-2026-10) 참고.

포트폴리오 인용용. 여기 있는 수치는 전부 `stat unit` / `stat anim` 실측값이며,
근거가 확보되지 않은 주장은 의도적으로 제외했다.

---

## 1. 측정 조건

측정 내내 고정한 값들. 하나라도 다르면 비교가 성립하지 않는다.

| 항목 | 값 |
|---|---|
| 실행 방식 | Standalone Game (PIE 아님 — 에디터 비용 배제) |
| 해상도 | 1160×684, RenderRes 100% |
| `t.MaxFPS` | 0 |
| `r.VSync` | 0 |
| Smooth Frame Rate | 꺼짐 |
| 카메라 | 군중이 보이는 고정 지점 1곳 |
| 기록 지표 | **Game (ms)** |

**Game만 기록하는 이유** — `Frame`은 모니터 주사율·윈도우 컴포지팅에 걸려 16.67 / 33.3에
붙어버린다. `Draw`·`GPU`는 화면에 뭐가 잡히느냐에 좌우된다. 게임 스레드 로직 비용을 재는
목적에는 `Game` 하나가 맞다.

**측정 오차** — 같은 조건을 두 번 재서 확인했다.

| 인원 | 1회차 | 2회차 | 편차 |
|---:|---:|---:|---:|
| 30 | 12.96 | 13.35 | 0.39 |
| 60 | 13.98 | 14.00 | **0.02** |

실효 오차 **0.4 ms 이내**. 아래 개선폭은 이 범위를 크게 벗어난다.

---

## 2. 측정 결과

두 조건을 비교한다.

- **개선 전** — 서드퍼슨 템플릿 ABP(`ABP_Male`)를 그대로 연결
- **개선 후** — 군중 전용 ABP(`ABP_Passerby`)로 교체

거리 LOD와 소프트 회피는 **양쪽 모두 동일하게 켜져 있었다**(`crowd.LOD` 기본값 1,
측정 중 토글하지 않음). 따라서 아래 차이는 ABP 차이에서 온 것으로 본다.

| 인원 | 개선 전 | 개선 후 | 절감 | 절감률 |
|---:|---:|---:|---:|---:|
| 30 | 14.62 | 13.35 | 1.27 | 8.7% |
| 60 | 17.80 | 14.00 | 3.80 | 21.3% |
| 90 | 22.06 | **15.38** | **6.68** | **30.3%** |

인원이 증가할수록 개선 전후의 차이가 확대되었으며, 30~90명 구간에서
게임 스레드 비용 증가율이 감소하는 경향을 확인했다. 정량화는 3장에서 한다.

> **측정 세션 주의** — 개선 전 수치는 이전 세션에서, 개선 후 수치는 이후 세션에서
> 측정했다. 해상도·콘솔 변수·카메라 위치·LOD 설정은 동일하게 맞췄으나
> 세션과 빌드가 달라 완전한 동일 조건 A/B는 아니다.
> 같은 세션 내 A/B 재측정이 남은 과제다.

> 0명 조건은 측정값이 10.10과 13.36으로 흔들려 기준선으로 쓰지 않았다.
> 스포너가 풀 액터를 생성하는 시점과 겹치면 값이 부풀려지는 것으로 보인다.
> 아래 계산은 전부 30~90명 구간의 기울기만 사용한다.

---

## 3. 마리당 한계비용 (핵심 지표)

절대 시간은 하드웨어·맵·해상도에 좌우된다. 시스템의 확장성을 보여주는 건
**인원 1명이 늘 때 추가되는 비용**이다.

```
개선 전 = (22.06 − 14.62) / 60 = 0.124 ms / 명
개선 후 = (15.38 − 13.35) / 60 = 0.034 ms / 명
```

## **0.124 ms → 0.034 ms (−73%)**

60명 → 90명 구간으로 계산해도 0.142 → 0.046으로 같은 경향이다.

즉 **NPC 수 증가에 따른 게임 스레드 비용 증가율을 약 73% 낮췄다.**
NPC 1명당 추가되는 게임 스레드 비용이 기존의 약 27% 수준이 됐다는 뜻이다.

> 이것을 "3.6배 더 세울 수 있다"로 환산하지 않는다. 고정비가 존재하고
> GPU·드로우콜이 먼저 한계에 닿을 수 있어서, 기울기 비율과 실제 배치 가능 인원은
> 같은 이야기가 아니다. 실제로 3.6배를 세워서 측정한 적도 없다.

> 게임 스레드 기준이다. 실제 한계는 GPU·드로우콜이 먼저 잡을 수 있고,
> 메타휴먼으로 교체하면 전부 다시 재야 한다.

---

## 4. `stat anim` 세부 항목 (90명 기준)

| 항목 | 개선 전 | 개선 후 | 판정 |
|---|---:|---:|---|
| `AnimGameThreadTime` | 2.98 | **1.22** | −59% |
| `UpdateKinematicBonesToAnim` | 2.81 | 상위 목록 밖 | 상위 비용 항목에서 제외 |
| `PerformAnimEvaluation_WorkerThread` | 7.51 | 7.75 | 변화 없음 (워커 스레드) |
| `ControlRig_Evaluate` | 4.56 | 4.64 | 변화 없음 — 미해결 |
| `ControlRig_UpdateInput` | 0.62 | 0.69 | 변화 없음 — 미해결 |
| `PBIK Solve` | 0.44 | 0.46 | 변화 없음 — 미해결 |

절감의 실체는 위 두 줄이다. `AnimGameThreadTime`이 절반 이하로 떨어졌고,
`UpdateKinematicBonesToAnim`이 상위 비용 항목에서 제외될 수준으로 감소했다.

> 상위 목록에서 사라졌다는 것만으로 0에 수렴했다고 확정할 수는 없다.
> `stat anim`은 상위 N개만 표시하므로, 정확한 잔여값은 별도 측정이 필요하다.

아래 네 줄은 손대지 못했다. 6장 참고.

---

## 5. 무엇을 바꿨는가

### 5.1 군중 전용 애니메이션 블루프린트

서드퍼슨 템플릿 ABP는 행인에게 필요 없는 것을 매 프레임 계산한다.
`ABP_Male`을 복제해 `ABP_Passerby`를 만들고 다음을 제거했다.

| 제거 대상 | 이유 |
|---|---|
| `Event Blueprint Update Animation` 전체 | CharacterMovement 기반 — 행인은 CMC를 쓰지 않음 |
| Jump / Fall Loop / Land 갱신 로직 | 행인은 낙하하지 않음 |
| `CalculateDirection` + `GetActorRotation` | 행인은 제자리 회전을 하지 않음 |
| `Cast To BP_ThirdPersonCharacter` 분기 | 상속 관계 없음 — 항상 실패하던 죽은 코드 |

### 5.2 CharacterMovement 없는 캐릭터의 로코모션 구동

**문제** — 서브 NPC는 `CharacterMovement`를 쓰지 않고 스플라인 위 진행 거리를
직접 갱신한다. 따라서 `GetVelocity()`가 **항상 0**이고, 속도 기반 로코모션 그래프는
영원히 대기 포즈만 낸다. 템플릿 ABP를 그대로 이식하는 것이 불가능한 이유다.

**해결** — C++에서 ABP 전용 프로퍼티를 노출했다.

```cpp
UPROPERTY(Transient, BlueprintReadOnly, Category = "Crowd|Anim")
float CurrentSpeed = 0.f;

UPROPERTY(Transient, BlueprintReadOnly, Category = "Crowd|Anim")
bool bIsMoving = false;
```

```cpp
// Tick 내부
bIsMoving = (CurrentActivity == ESubNpcActivity::Walking)
         || (CurrentActivity == ESubNpcActivity::Reacting
             && CurrentReaction == ESubNpcReaction::Dodge);
CurrentSpeed = bIsMoving ? WalkSpeed : 0.f;
```

함수 호출이 아니라 **프로퍼티 읽기**로 설계한 것이 핵심이다.
`Property Access` 노드로 접근할 수 있고, 따라서
`Blueprint Thread Safe Update Animation` 안에서 사용 가능하다.

즉 기존에 `Event Blueprint Update Animation`(게임 스레드)에서 수행하던
**변수 갱신 로직**을 워커 스레드에서 처리 가능한 구조로 옮긴 것이다.
애니메이션 평가 전체가 워커 스레드로 이동했다는 뜻은 아니다 —
개선 후에도 `AnimGameThreadTime`은 1.22 ms 남아 있다.

```
Function Entry ──► SET GroundSpeed ──► SET ShouldMove
                      ▲                    ▲
        Property Access                Property Access
        (Character.CurrentSpeed)       (Character.bIsMoving)
```

`IsFalling`, `Direction`, `Velocity` 등 나머지 변수는 Set 노드를 두지 않고
기본값(false / 0)으로 고정했다. 행인에게는 상수로 충분하다.

### 5.3 함께 적용한 것 (개별 효과 미측정)

아래는 시스템에 들어가 있지만 이번 측정에서 단독 효과를 분리하지 못했다.
정직하게 "적용했다"까지만 기록한다.

**거리 기반 애니메이션 갱신률 LOD** — `SubNpcDirectorSubsystem`이 카메라 거리로
Near / Mid / Far 대역을 나누고 대역별로 갱신률을 조정한다.

```cpp
void ASubNpcCharacter::ApplyAnimUpdateRate(USkeletalMeshComponent* MeshComp, int32 FrameSkip)
{
    FAnimUpdateRateParameters* Params = MeshComp->AnimUpdateRateParams;
    Params->bShouldUseLodMap          = false;
    Params->UpdateRate                = FMath::Max(1, FrameSkip);
    Params->EvaluationRate            = FMath::Max(1, FrameSkip);
    Params->bInterpolateSkippedFrames = (FrameSkip > 1);
}
```

Near 매 프레임 / Mid 2프레임 / Far 4프레임. 건너뛴 프레임은 보간한다.
LOD 판정 자체도 한 프레임에 전부 훑지 않고 1초에 걸쳐 분산한다(`LodFullSweepTime = 1.0`).

토글용 콘솔 변수(`crowd.LOD`)를 만들어 두었으나, 테스트 맵이 좁아 카메라 거리를
충분히 벌릴 수 없어 **유의미한 A/B 측정을 하지 못했다.** 넓은 맵에서 재측정이 필요하다.

**소프트 회피** — 같은 경로 위 근접 개체를 차선 오프셋으로 밀어낸다(`crowd.Avoidance`).
현재 `MaxAvoidancePairs = 24` 상한 때문에 90명 규모에서는 겹침이 충분히 해소되지 않는다.
수치 조정 예정.

---

## 6. 남은 작업

90명 기준으로 아직 붙어 있는 비용.

| 항목 | 비용 | 조치 |
|---|---:|---|
| `ControlRig_Evaluate` + `UpdateInput` + `PBIK Solve` | **5.79 ms** | 군중 전용 메시 복제 후 `Post Process Anim Blueprint` = None |

**이게 왜 남아 있는가** — UE5 템플릿 스켈레탈 메시는 **애셋 자체에** 포스트 프로세스
애니메이션 블루프린트가 물려 있고, 그 안에서 Control Rig으로 발 배치(Foot IK)를 돌린다.
AnimBP 그래프를 아무리 정리해도 이건 남는다.

**진단 과정** — `ABP_Passerby`의 AnimGraph에 Control Rig 노드가 없는데도
`stat anim`에 `ControlRig_Evaluate`가 계속 잡혔다. 0명일 때 CallCount가 0이라는 점에서
행인이 출처임을 특정했고, `PBIK Solve`가 항상 동반된다는 점에서 포스트 프로세스 리그로
좁혔다.

**시도했으나 실패** — `BP_Passerby`의 `Disable Post Process Blueprint` 체크박스를
켰으나 `ControlRig_Evaluate`는 그대로였다. 원인 미규명.
스켈레탈 메시 애셋 자체를 복제해 `Post Process Anim Blueprint`를 비우는 방향으로
전환해야 한다.

**다음 계획** — 군중 전용 스켈레탈 메시를 분리(`SKM_Passerby`)하고
`Post Process Anim Blueprint`를 제거하여 Control Rig 비용 감소 여부를
별도 A/B로 측정할 예정이다. 결과가 나오면 이 문서에 실측값으로 추가한다.

원본 메시는 메인 NPC·플레이어와 공유하므로 건드리지 않는다.

> **참고** 이후 `SKM_Passerby` 분리 · Control Rig A/B 결과는 저장소(Docs, 커밋 로그)에서 찾지 못했다. 미완료로 본다.

---

## 7. 알려진 한계

포트폴리오에 쓸 때 과장하지 않기 위해 남겨둔다. 먼저 밝히는 편이 유리하다.

- **워커 스레드 비용은 줄지 않았다.** `PerformAnimEvaluation_WorkerThread`는
  7.51 → 7.75로 사실상 동일하다. 이번 개선은 **게임 스레드** 절감이다.
  스켈레탈 메시 평가 자체를 줄이려면 표현(Representation) 단계에서
  원거리 개체를 정적 메시로 교체해야 한다.
- **LOD의 단독 효과는 측정하지 못했다.** 맵이 좁아 거리 조건을 만들 수 없었다.
  적용 사실만 기록하고 절감 수치는 주장하지 않는다.
- **0명 기준값이 불안정하다.** 그래서 절대 한계 인원(몇 명까지 가능한가)은
  계산하지 않았다. 마리당 비용비만 사용한다.
- **군중 분포가 균일하지 않았다.** 스폰 간격 처리 때문에 입구 경로에 뭉치는
  현상이 있었다. 개선 전/후 동일 조건이라 비교에는 문제없지만, 분포가 고르면
  LOD 효과가 더 크게 나올 가능성이 있다.
- **개선 전/후가 같은 세션이 아니다.** 측정 조건은 맞췄지만 세션과 빌드가 다르다.
  같은 세션 내에서 Anim Class만 바꿔가며 재는 A/B 재측정이 남아 있다.
- **MassEntity 전환은 미완이다.** 링크 에러가 해결되지 않아 현재 구조는 액터 기반이다.
  Mass로 가더라도 프로모트된 액터의 애니메이션 비용은 줄지 않으므로,
  "Mass가 이 수치를 만들었다"고 말해서는 안 된다.
  → (2026-10 갱신) Mass 1~3단계는 이후 완료됐다. 9장 참고. 위 문장의 원칙(이 문서의 −30% / −73%는 ABP 교체 효과)은 그대로다.

---

## 8. 한 줄 요약 (이력서·발표용)

> UE5 군중 NPC 90명 환경에서 Game Thread **22.06 ms → 15.38 ms (−30.3%)** 로 개선.
> NPC당 추가 Game Thread 비용을 **0.124 ms → 0.034 ms (−73%)** 로 감소시켰습니다.
> CharacterMovement를 사용하지 않는 스플라인 기반 군중 NPC에 맞춰 불필요한 AnimBP
> 로직을 제거하고, Property Access 기반 Thread Safe Animation Update 구조로
> 재설계했습니다.

구성 의도

- **−30.3%** = 결과
- **−73%** = 확장성
- **AnimBP 정리 + Thread Safe 재설계** = 방법

세 가지가 분리되어 있어 면접에서 어느 쪽을 물어도 답이 나온다.

### 예상 질문 대비

**"LOD로는 몇 ms 줄었나요?"**
→ LOD 자체의 A/B는 현재 맵의 거리 조건 때문에 신뢰할 수치를 확보하지 못했습니다.
그래서 30% 개선을 LOD 효과라고 주장하지 않았습니다.

**"애니메이션이 30% 빨라진 건가요?"**
→ 아닙니다. `PerformAnimEvaluation_WorkerThread`는 7.51 → 7.75로 변화가 없습니다.
게임 스레드에 있던 애니메이션 갱신 작업을 줄이고 워커 스레드 친화적으로
재설계한 결과입니다. `AnimGameThreadTime`이 2.98 → 1.22로 −59% 입니다.

**"두 수치는 같은 조건에서 쟀나요?"**
→ 측정 조건(해상도·콘솔 변수·카메라·LOD)은 동일하게 맞췄으나 세션과 빌드가
다릅니다. 같은 세션 내 A/B 재측정이 남은 과제입니다.

---

## 9. 이후 진행 (2026-10)

출처: [CrowdPerformance.md](CrowdPerformance.md) (2026-10-06 ~ 10-07, `crowd.Bench`로 측정, CSV 중앙값 또는 프레임 평균). 위 1~8장과는 **맵 · 실행 방식 · 측정 도구가 달라** 숫자를 섞어 비교하지 않는다.

| 단계 | 내용 | 결과 |
|---|---|---|
| Mass 전 기준 (PIE, `Lvl_ThirdPerson`) | 1명당 애니메이션 워커 약 42µs, 게임 스레드 약 33µs (그중 군중 로직 8.5µs), GPU 약 23µs | 군중 로직은 싸고(100명 0.85ms), 비싼 건 애니메이션 · 렌더링 |
| 1단계: 이동 계산 병렬화 (`crowd.Mass`) | `UMassSubNpcPathFollowProcessor`가 경로 이동 계산을 워커 스레드에서 병렬로, 적용만 게임 스레드에서 | 약 65명: 게임 스레드 0.55 → **0.38ms (−31%)**. 단 모든 스레드 CPU 합계는 0.55 → **1.03ms**로 늘었다 (나누는 비용) |
| 2단계: 원경 인스턴싱 (`crowd.FarInstances`) | 40m 밖 행인의 스켈레탈 메시를 숨기고 AnimToTexture로 구운 인스턴스드 메시로 그림 → [CrowdFarLOD](CrowdFarLOD.md) | `Lvl_CrowdLODTest` 200명: 애니메이션 워커 6.21 → **3.35ms (−46%)**, 게임 스레드 8.01 → 6.91ms (−14%). 처음엔 GPU +1.5ms (샘플 메시 LOD 없음) |
| 2단계 보완: 원경 메시 Manny LOD2로 다시 굽기 | 삼각형 41,052 → 12,998 | 200명 GPU 추가 비용 +1.58 → **−0.04ms** (사라짐). 게임 스레드 −1.5ms, 애니메이션 −3.1ms 유지 |
| 3단계: StateTree (`ST_Passerby`) | 행인 상태 결정을 Mass StateTree로 | 120명: 군중 합계 0.786 → 0.784ms — 비용 차이는 오차 안. 동작 동일 유지가 목적 |

- 모든 측정이 60 FPS(16.67ms)에 묶여 있어 FPS가 아니라 스레드별 시간으로 판단했다 (`Docs/CrowdPerformance.md` 주의 문구).
- 7장의 "워커 스레드 비용은 줄지 않았다"는 ABP 교체에 대한 이야기다. 워커 스레드 절감은 2단계(원경 인스턴싱)에서 나왔다 — 7장에서 말한 "원거리 개체를 정적 메시로 교체" 방향 그대로.
- Mass로 옮긴 1단계 자체의 이득은 작다(0.17ms). 면접에서는 "Mass로 크게 빨라졌다"가 아니라 "측정해 보니 로직은 싸고 애니메이션 · 렌더링이 비싸서, 원경 표현을 바꾼 쪽이 효과가 컸다"로 말한다.
