# 원경 행인 메시 다시 굽기 (AnimToTexture)

성능 수치는 [서브NPC_성능측정_기록](서브NPC_성능측정_기록.md) 9장 · [CrowdPerformance.md](CrowdPerformance.md).

> **참고** 2026-10-17 행인이 남녀 학생 모델(`BP_Passerby_Boy` / `BP_Passerby_Girl`)로 바뀌면서 원경 데이터(`FarCrowdAnimData`)를 행인 BP마다 따로 지정하고, 원경 인스턴서도 데이터별로 만든다. 아래 Manny 기준 설명은 굽는 절차로 읽는다.

멀리 있는 행인(`crowd.FarInstanceDistance`, 기본 40m 밖)은 스켈레탈 메시 대신 인스턴스드 스태틱 메시 + 텍스처로 구운 애니메이션으로 그린다.
이 데이터는 `/Game/NPC/SubNpc/FarCrowd`에 있다. 행인 전용 복사본이고, 엔진 샘플과 팀 공용 에셋은 건드리지 않는다.

| 에셋 | 내용 |
|---|---|
| `DA_PasserbyFar` | 굽기 설정. 스켈레탈 메시 `SKM_Manny_Simple`, 애니메이션 `IdleBase`(대기)·`A_WalkFwd_Loop`(걷기), 뼈 모드 |
| `SM_SKM_Manny_Simple_Far` | Manny LOD2를 변환한 스태틱 메시 (버텍스 7,504 / 삼각형 12,998) |
| `TX_PasserbyFar_Bone*` | 구운 뼈 위치·회전·가중치 텍스처 |
| `M_PasserbyFar` | `M_Passerby`(행인 머티리얼) + `MF_BoneAnimation`. 머티리얼 어트리뷰트 방식 |
| `MI_PasserbyFar_01/02` | 위 머티리얼의 인스턴스. 굽기 결과(텍스처·프레임 수 등)가 들어간다 |

스포너의 `Far Crowd Anim Data` 기본값이 `DA_PasserbyFar`다.

## 다시 굽기

애니메이션이나 메시를 바꿨을 때만 한다.

1. `DA_PasserbyFar`의 `Anim Sequences`를 고친다. 이름에 `Walk`/`Idle`이 들어간 것을 걷기/대기로 쓴다.
2. **PIE를 멈춘 상태에서** 에디터 콘솔에 입력한다.
   ```
   crowd.BakeFarCrowd /Game/NPC/SubNpc/FarCrowd/DA_PasserbyFar
   ```
3. 출력 로그에서 `[FarBake] 완료: ... LOD 1/1개 구움`을 확인한다.
4. 바뀐 에셋을 저장한다: `DA_PasserbyFar`, 텍스처 3장, `MI_PasserbyFar_01/02`, `SM_SKM_Manny_Simple_Far`.

### 메시를 새로 만들 때

`DA_PasserbyFar`의 `Static Mesh`를 비우고 실행하면 스켈레탈 메시의 LOD 하나로 새 스태틱 메시를 만든다.
기본값은 마지막 LOD이고, 두 번째 인자로 바꿀 수 있다 (예: `crowd.BakeFarCrowd <경로> 1`).

새로 만든 메시에는 스켈레탈 메시의 공용 머티리얼(`MI_Manny_*`)이 붙어 있다. 명령은 원경 폴더 밖 머티리얼을 건드리지 않고 경고만 남긴다.
메시 슬롯에 `MI_PasserbyFar_01/02`를 넣고 명령을 한 번 더 실행해야 머티리얼이 채워진다.

## 주의

- 엔진 함수의 "LOD 전부 변환"(`ConvertSkeletalMeshToStaticMesh`에 -1)은 LOD 검사에 막혀 동작하지 않는다. 그래서 LOD 하나만 쓴다.
  원경은 40m 밖이라 LOD2 하나로 충분하다.
- 스태틱 메시의 라이트맵 UV 생성은 꺼 둔다. 켜면 구운 UV 채널(1)을 덮어쓸 수 있다. 명령이 자동으로 끈다.
- 걷기 재생 속도는 `SubNpcMassProcessors.cpp`의 `BakedWalkSpeed`(150cm/s)를 기준으로 행인 걸음 속도에 맞춰 조절된다.
