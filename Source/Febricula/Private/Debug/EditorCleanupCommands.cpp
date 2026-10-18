// [skydodo] 2026-10-18 작성
// 요약: 에셋을 지운 뒤 남은 빈 참조를 정리하는 에디터 전용 콘솔 명령
// [에디터 전용] 저장은 하지 않고 패키지만 더럽힌다. 확인한 뒤 에디터에서 저장할 것

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "Animation/Skeleton.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogEditorCleanup, Log, All);

namespace EditorCleanup
{
	/**
	 * febricula.CleanSkeletonPreview <스켈레톤 경로...>
	 * 스켈레톤 미리보기에 붙여 둔 에셋(소켓 미리보기 메시 등) 중 이미 지워진 것을 목록에서 뺀다.
	 * 이 목록은 파이썬에 노출돼 있지 않아 C++에서만 고칠 수 있다.
	 */
	static FAutoConsoleCommand CleanSkeletonPreview(
		TEXT("febricula.CleanSkeletonPreview"),
		TEXT("스켈레톤 미리보기 부착 목록에서 지워진 에셋을 뺀다. 인자: 스켈레톤 경로 (예: /Game/Character/Cheon/Cheon_Skeleton)"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			for (const FString& Path : Args)
			{
				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *Path);
				if (!Skeleton)
				{
					UE_LOG(LogEditorCleanup, Warning, TEXT("스켈레톤을 찾지 못함: %s"), *Path);
					continue;
				}
				Skeleton->Modify();
				const int32 Removed = Skeleton->PreviewAttachedAssetContainer.ValidatePreviewAttachedObjects();
				if (Removed > 0)
				{
					Skeleton->MarkPackageDirty();
				}
				UE_LOG(LogEditorCleanup, Log, TEXT("%s: 지워진 미리보기 부착 에셋 %d개 정리"), *Path, Removed);
			}
		}));
}
#endif // WITH_EDITOR
