# 🌙 미열 (Febricula) — a quiet game

> **언리얼 엔진 5.8 · C++ · 비주얼 노벨 + 육성 시뮬레이션**  
> 내일배움캠프 Unreal 8기 Team 12 최종 프로젝트 · 4인 팀 (이지민 · 이상기 · 김태인 · 홍성돈)

[![전체 플레이 영상](https://img.youtube.com/vi/vAi9IOcNppw/0.jpg)](https://www.youtube.com/watch?v=vAi9IOcNppw)

---

## 📖 게임 소개

프로그래머 면허 제도가 시행된 근미래 대한민국. 플레이어는 소프트웨어 고등학교 3학년에 강제 전학되어, 1년간의 학교 생활 끝에 프로그래머 면허를 취득하는 것을 목표로 합니다.
학교와 고시원을 오가는 하루를 반복하며 사람들을 만나고, 스탯을 키워 평가를 통과해야 합니다.

| 항목 | 내용 |
|---|---|
| 장르 | 비주얼 노벨 + 육성 |
| 플랫폼 | PC (Windows) · 1인 |
| 플레이 타임 | 60 ~ 90분 |
| 구현 범위 | 1학기 진단평가까지 (프롤로그 한 달 분량) |

- **스토리 파트** — 대화를 따라 이야기를 전개합니다. 선택지와 호감도에 따라 만나는 사람과 장면이 달라집니다.
- **육성 파트** — 자유 시간에 활동과 알바 미니게임으로 스탯을 키워, 진단평가 같은 위기를 넘깁니다.

---

## 🙋 내 담당 — 이상기 (게임플레이 프로그래밍)

| 영역 | 내용 |
|---|---|
| **대화 시스템** | 대사 출력 · 연출 서브시스템, 대화 · 조건 · 액션 데이터 모델, NPC 이벤트 선택 |
| **상호작용** | 인터페이스 + 컴포넌트 기반 상호작용 구조 |
| **메인 NPC** | 스케줄 배치 스포너, 순찰 · 안내 Behavior Tree |
| **군중 시스템** | 수백 명의 행인 이동 · 회피 · LOD, MassEntity 도입, 측정 기반 최적화 (−73%, −46%) |
| **알바 미니게임** | 햄버거집 · 꽃집 · 편의점 미니게임 3종과 공통 프레임워크 |

---

## 💬 대화 시스템

![대화 이벤트](Docs/images/dialogue_hud_2026-10-17.png)

`UDialogueSubsystem`(Tickable World Subsystem)이 게임의 모든 대사와 연출을 맡습니다. 화면에 대사를 띄우고 사운드와 연출을 실행합니다.

- 이벤트 테이블(`FNpcEventRow`)에서 조건에 맞는 Row를 고르고, 그 Row가 가리키는 대화 노드(`FDialogueNodeRow`)를 실행
- 노드는 Branch(자동 분기)와 Choice(선택지)를 갖고, 각각 조건(`FGameCondition`)과 결과(`FGameAction`, 41종)를 가짐
- **이벤트 선택** — 조건 충족 + 최고 우선순위, 동점이면 테이블 순서
- **여러 NPC 동시 등장** — 참가자 전원 순찰 중지 → 대화 후 한꺼번에 해제
- **자리별 대화** — 같은 NPC라도 교실과 옥상에서 다른 대사

```cpp
// 조건을 만족하는 이벤트 중 Priority가 가장 높은 것
for (const auto& Pair : EventTable->GetRowMap())
{
    const FNpcEventRow* Row = (FNpcEventRow*)Pair.Value;
    if (Row->NpcId != NpcId && !Row->NpcId.IsNone()) continue;
    if (!Row->bRepeatable && Progress->IsEventCompleted(Pair.Key)) continue;
    if (!Progress->EvaluateConditions(Row->Preconditions)) continue;
    if (!BestRow || Row->Priority > BestRow->Priority)
    { BestRow = Row; BestRowName = Pair.Key; }
}
// 함께 나오는 NPC 전원을 Talk 상태로
Participants.AddUnique(NpcId);
EnterTalkForParticipants(Participants, InInstigator);
```

**트러블슈팅 — 주말이 지나면 날짜가 초기화돼 이벤트가 어긋남**
- 원인: 주 단위 상대 날짜로 주말을 판정했고, 초기화되지 않은 값이 남아 있었음
- 해결: 절대 일차 기준으로 계산하고, 날짜 테스트 테이블로 검증

---

## 🖐 상호작용 시스템

![상호작용 프롬프트](Docs/images/interaction_prompt.jpg)

**문제** — 문과 소품 같은 오브젝트마다 상호작용 처리가 달라, 새 오브젝트를 일관된 방식으로 붙일 수 없었습니다.

**해결** — `IInteractable` 인터페이스(`CanInteract` · `Prompt` · `Interact`)와 플레이어 쪽 `UInteractionComponent`로 구조를 바꿨습니다.
- 대상은 인터페이스만 구현하면 자동으로 탐색됨 → NPC, 소품, 문, 미니게임 문을 같은 방식으로 처리
- 포커스 선택 순서: 반경 내 오버랩 → 인터페이스 구현 → `CanInteract` → 정면 각도 통과 → 가장 가까운 대상
- 포커스가 바뀔 때만 HUD를 갱신하고, 대화 중에는 탐색을 멈춰 프롬프트 깜빡임을 방지
- 어떤 Pawn에든 컴포넌트만 붙이면 동작 → 이후 추가된 미니게임 문은 **추가 코드 없이** 자동 감지

---

## 🧑‍🏫 메인 NPC — 스케줄 배치 · 순찰 AI

[![메인 NPC 스케줄 스포너](Docs/images/mainnpc_spawner.png)](https://www.youtube.com/watch?v=vAi9IOcNppw)

**스케줄 배치 스포너** — 기획자가 표(`FMainNpcSpawnRow`)만 고쳐서 "1일차 아침엔 교실, 신뢰 이상이면 옥상"을 정합니다.

| 누가 | 언제 | 조건 | 어디 |
|---|---|---|---|
| NpcId | 일차 + 시간대 태그 | 호감도 · 진행 (`FGameCondition`) | 레벨 + SpawnPoint ID |

- **한 NPC는 한 곳에만** — 모든 레벨의 스포너가 같은 표로 같은 계산 → 같은 행이 뽑힌 레벨만 스폰
- **여러 행이 맞을 때** — 조건 있는 행 > Priority > 시간대 지정 행 > 테이블 순서

**순찰 · 안내 Behavior Tree** — Chase / Guide(`WaitForPlayerNearby`) / Patrol(`AdvanceRouteTarget`) / Idle. 상태 전환은 이벤트 액션(`NpcStartPatrol` / `NpcStartGuide`)이 호출해 스토리 연출이 AI를 제어합니다.

**트러블슈팅 — 날이 바뀔 때 NPC가 깜빡이거나 밀려남**
- 원인: ① 행이 바뀌면 무조건 내리고 다시 세움 ② 내리기와 세우기를 NPC별로 번갈아 처리해, 앞 NPC가 자리를 비우기 전에 다음 NPC가 생김
- 해결: ① 자리 · 애니메이션 · AIState · 경로 · 대화가 같으면 그대로 둠(`IsSamePlacement`) ② 전부 내린 뒤에 세우는 두 단계로 분리

---

## 🚶 군중 시스템 — 측정 기반 최적화

![군중 시스템](Docs/images/crowd_overview.jpg)

수백 명의 행인을 싸게 돌리는 설계. `USubNpcDirectorSubsystem`이 행인 전원의 이동 · 회피 · LOD를 한 곳에서 처리하고(Active/Pooled 풀), `ASubNpcCharacter`는 상태만 보유합니다.

### 가벼운 이동과 직접 만든 회피

`CharacterMovement` 틱을 끄고 필요한 기능만 재구현했습니다.

| 기능 | 구현 |
|---|---|
| GroundSnap | 발밑 라인 트레이스 1회로 바닥에 붙이기 |
| WorldBlock | 지형만 캡슐 스윕 1회, 가까운 LOD만 |
| Avoidance | 0.1초마다 가까운 쌍(최대 128쌍)만 감속 · 비키기, 우측 통행 |
| BodySeparation | 매 프레임 몸 겹침 금지 |
| 플레이어 회피 | 플레이어도 장애물로 포함 (RVO 설정과 무관) |

> RVO는 구현 비용을, 직접 만든 회피는 실행 비용을 아끼는 도구입니다. 수백 명 규모라 후자를 골랐습니다. ([비교 문서](Docs/RVO_회피_비교.md))

![스팟 이벤트와 일행](Docs/images/crowd_spot_event.png)

일행 중 한 명이 줄을 서서 이벤트를 하는 동안 나머지는 옆 대기 자리에서 기다렸다가 함께 길로 복귀합니다. 행동 결정은 Mass StateTree(`ST_Passerby`)로 이전했습니다.

### 성능: 측정부터 시작

`crowd.Bench <인원...>`로 인원을 단계별로 맞추고 안정화 후 CSV로 측정했습니다 (`t.MaxFPS 0`, VSync 끔, 같은 세션의 끔/켬끼리만 비교).

| 100명 기준 게임 스레드 비용 | |
|---|---:|
| 군중 로직 | **0.85 ms** |
| 애니메이션 워커 | **4.3 ms** |

> **결론** — 비싼 건 로직이 아니라 애니메이션 · 렌더링. Mass로 로직을 옮기는 것보다 "멀리 있는 사람을 다르게 그리는 것"이 효과가 크다.

### 군중 전용 애니메이션 블루프린트 — 1명당 비용 −73%

서드퍼슨 템플릿 ABP가 행인에게 필요 없는 계산을 매 프레임 하고 있어, 행인용 `ABP_Passerby`로 경량화했습니다.

| 지표 | 결과 |
|---|---|
| 1명당 한계비용 | **0.124 → 0.034 ms (−73%)** |
| 90명 기준 게임 스레드 | 22.06 → 15.38 ms (−30.3%) |
| AnimGameThreadTime | 2.98 → 1.22 ms (−59%) |

### 원경 인스턴싱과 Mass 병렬화 — 애니메이션 워커 −46%

![원경 인스턴싱](Docs/images/crowd_far_instancing.png)

40 m 밖 행인은 스켈레탈 메시를 숨겨 애니메이션 계산을 멈추고, 걷기 · 대기를 텍스처로 구운(AnimToTexture) 인스턴스드 메시 한 장으로 그립니다.

| 200명 CSV 중앙값 (ms) | 끔 | 켬 |
|---|---:|---:|
| 애니메이션 워커 | 6.21 | **3.35 (−46%)** |
| 게임 스레드 전체 | 8.01 | **6.91 (−14%)** |
| GPU (LOD2 재굽기 후) | 6.75 | 6.71 (추가 비용 없음) |

Mass 이동 병렬화(계산은 워커 스레드, 적용만 게임 스레드)는 0.55 → 0.38 ms (−31%). 1명당 몇 µs라 이득이 작다는 것도 수치로 확인했습니다.

![Mass 구조](Docs/images/crowd_mass_diagram.png)

### 성능 트러블슈팅

| 문제 | 원인 | 해결 |
|---|---|---|
| 원경을 켜자 GPU +1.5 ms | 원경용 샘플 메시가 LOD 1개 · 삼각형 41,052개 | 행인 메시 LOD2(12,998개)로 다시 구움 → GPU 추가 비용 소멸 |
| 원경을 켜면 60 → 22~31 FPS | 매 프레임 `MarkRenderStateDirty()`로 인스턴스 메시 전체 재생성 | `UpdateInstanceTransform(…, bMarkRenderStateDirty=false)`로 바뀐 인스턴스만 전송 |

### 군중 트러블슈팅

| 증상 | 원인 | 해결 |
|---|---|---|
| 칸 22 cm 앞에서 21초 정지 | 칸으로 가는 사람과 줄 선 사람이 서로 기다리는 교착 | 목표로 곧장 가는 방향은 도착 뒤에야 닿는 충돌 무시 → **21초 → 3.6초**, 82/82 이벤트 연결 |
| 줄이 당겨질 때 줄 전체 1~8초 정지 (양보 정지 42%) | 같은 방향으로 움직이는데 뒷사람이 연쇄 양보 | 같은 쪽으로 따라 걷는 사람 · 같은 줄끼리는 양보하지 않음 |
| 굽은 곳에서 50명 넘게 10~28초 정지 | 몸이 향한 방향으로 양보 결정 → A→B→C→A 순환 | 경로 진행 순서로 양보 (순환 없음) |
| 플레이어가 길을 막으면 28번 중 6번 실패 | "제자리 정지"가 충돌 없는 최선이라 계속 선택 | 오래 설수록 정지 비용 증가 → 옆으로 빠지는 후보 선택 |

> 군중 정체는 대부분 "서로 기다림"에서 생긴다. 양보 규칙은 순환하지 않는 기준으로 정해야 한다.

### Mass 첫 적용 실패에서 배운 것

플러그인 3개와 약 2,000줄을 한 번에 넣었다가 링크 에러로 1,139줄을 롤백했습니다. 표현 전환 · ZoneGraph 이동 · StateTree를 동시에 넣어 어디서 깨지는지 나눠 볼 수 없었고, 어디가 비싼지 모른 채 시작했기 때문입니다.
→ **측정 → 1단계 이동 병렬화 → 2단계 원경 표현 → 3단계 StateTree**로 나누고, 단계마다 켜고 끌 스위치(`crowd.Mass`, `crowd.FarInstances`)를 뒀습니다.

### 디버그 도구

| 명령 | 용도 |
|---|---|
| `crowd.DebugRole` | 머리 위에 역할과 회피 상태 표시 |
| `crowd.DebugSpot 2` | 스팟 근처 상태 · 좌표 로그 → 상태별 지속 시간 분석 |
| `crowd.LogBlock` | 정체 요약 (감속 이유별 인원, 가장 많이 막는 상대) |
| `crowd.BlockTest` | 플레이어가 길을 막는 상황 자동 측정 |
| `crowd.Bench` | 인원별 성능 측정 + 겹침 · 발 뜸 · 과속 검사 로그 |

> 상세 측정 기록: [서브NPC_성능측정_기록.md](Docs/서브NPC_성능측정_기록.md) · [CrowdPerformance.md](Docs/CrowdPerformance.md) · [CrowdFarLOD.md](Docs/CrowdFarLOD.md)

---

## 🍔 알바 미니게임

| 햄버거집 | 꽃집 | 편의점 |
|---|---|---|
| ![햄버거집](Docs/images/minigame_burger.png) | ![꽃집](Docs/images/minigame_flower.png) | ![편의점](Docs/images/minigame_convenience.png) |
| 주문대로 쌓기 · 급여 + 손재주 | 같은 꽃 합치기 · 급여 + 체력 | 진상 손님 사이에서 버티기 · 급여 + 말솜씨 |

### 공통 프레임워크

튜토리얼 카드 → 연습 판 → 본 게임 → 결과 · 보상. 새 미니게임은 **위젯 1개 + 설정 한 줄 + 문 배치**로 추가합니다 (꽃집 · 편의점을 이 방법으로 추가).

| 클래스 | 역할 |
|---|---|
| `UMiniGameSettings` | 레벨 이름 → 위젯 클래스 · 시작 위치 · 보상 (Project Settings에서 기획자가 수정) |
| `UMiniGameSubsystem` | 띄우기, 입력 잠금, 보상 지급(중복 방지), 복귀 |
| `UMiniGameTravelSubsystem` | 들어오기 전 레벨 기억 |
| `UMiniGameWidget` | FinishGame / PreviewReward / ExitGame |
| `AMiniGameDoor` | E키로 지정 레벨 이동 |

### 화면을 그리는 방식 — 의사결정

기획 HTML 시안을 언리얼로 옮길 방식을 비교했습니다.

| 방식 | 시안을 숫자 그대로 | 수가 변하는 오브젝트 | 팀 작업 · 빌드 |
|---|---|---|---|
| WBP + BindWidget | 다시 배치해야 함 | 재료마다 위젯 생성 · 풀링 | 바이너리라 git 병합 불가 |
| Web Browser 위젯 | 이식 작업 없음 | HTML이 처리 | CEF 플러그인 용량 · 시작 시간 증가, JS↔C++ 연동 불안정 |
| **C++ NativePaint (채택)** | HTML 캔버스와 같은 좌표 · 크기 | 매 프레임 배열을 그림, 위젯 생성 없음 | 코드라 병합 가능, 플러그인 없음 |

### 꽃집 — 직접 만든 2D 물리와 시뮬레이션 밸런싱

- `FlowerPhysics.h`: 꽃 실루엣에 맞춘 원 3~5개짜리 강체. 회전 · 마찰 · 반발 충격량을 1/120초 고정 간격으로 풀어 비스듬히 걸치고 무너짐
- **트러블슈팅 — 한 자리 연타로 최대 보상**: 무작위로 놓아도 90초에 주문 26건. 물리 헤더를 그대로 쓰는 **헤드리스 시뮬레이터로 전략별 30~40판 측정** → 떨어지는 꽃 4종, 묶음 주문, 보상 기준 7 · 10 · 12 · 14건으로 조정. 결과: 천천히 9건, 쉬지 않고 11~13건

### 편의점 — 서바이벌 액션

- 손님 4종(취객 · 돌진 · 투척 · 단체) + 블랙 컨슈머 보스, 레벨업마다 서비스 스킬 12종 중 무작위 3택
- **의사결정**: 넣고 싶던 스킬을 시작 스킬로 고정하면 매 판 같은 빌드가 되어, 모든 스킬은 무작위 3택에서만 얻도록 결정

---

## 🤖 AI 활용 (Claude Code + Unreal MCP)

AI 초안 → 직접 검증과 판단 → 수치로 확인. 거절한 제안까지 기록했습니다.

| 사례 | AI 초안 | 내 검증과 판단 |
|---|---|---|
| Mass 도입 방향 | 성능 측정 도구와 "로직만 옮기면 1 ms도 못 아낀다"는 결론 | 창 포커스 손실로 오염된 첫 측정 폐기 후 재측정 → 결론 확인 후 Mass 목적을 원경 표현으로 변경 |
| 행인 회피 | 1차 "앞사람이 멈추면 선다", 2차 "옆 차선으로 80 cm 비켜 간다" | 두 번 반려 (정체 · 좌우 흔들림). "앞으로 가면서 비킨다"는 기준을 정해 옆 이동을 전진 거리에 묶음 |
| 교착 재현 | 디버그 로그로 상태별 지속 시간 집계 | 한 판에서 재현이 안 돼도 넘기지 않고 같은 조건 4판 측정 후 수정 · 커밋 |
| 로그 정리 | 팀원 코드의 로그 · 바인딩 삭제 | 반려. 팀원 파일은 주석 3줄만, 내 로그는 `crowd.Log` 조건부 출력으로 정리 |

---

## 📂 주요 코드

| 파일 | 내용 |
|---|---|
| [`DialogueSubsystem.cpp`](Source/Febricula/Private/Dialogue/DialogueSubsystem.cpp) | 대사 · 연출 서브시스템, 이벤트 선택 |
| [`InteractionComponent.cpp`](Source/Febricula/Private/Interaction/InteractionComponent.cpp) | 상호작용 대상 탐색 · 포커스 |
| [`MainNpcSpawner.cpp`](Source/Febricula/Private/NPC/MainNpc/MainNpcSpawner.cpp) | 메인 NPC 스케줄 스포너 |
| [`SubNpcDirectorSubsystem.cpp`](Source/Febricula/Private/NPC/SubNpc/SubNpcDirectorSubsystem.cpp) | 군중 감독: 경로 이동, 회피, LOD |
| [`SubNpcMassProcessors.cpp`](Source/Febricula/Private/NPC/SubNpc/SubNpcMassProcessors.cpp) | Mass 이동 병렬화 · 원경 표현 프로세서 |
