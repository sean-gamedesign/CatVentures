// AnimNotifyState_SwatTrace.cpp

#include "AnimNotifyState_SwatTrace.h"
#include "CatBase.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotifyState_SwatTrace::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	if (!MeshComp) return;

	if (ACatBase* Cat = Cast<ACatBase>(MeshComp->GetOwner()))
	{
		Cat->BeginSwatTrace(MeshComp, SocketName);
	}
}

void UAnimNotifyState_SwatTrace::NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyTick(MeshComp, Animation, FrameDeltaTime, EventReference);

	if (!MeshComp) return;

	if (ACatBase* Cat = Cast<ACatBase>(MeshComp->GetOwner()))
	{
		Cat->ProcessSwatTraceTick(MeshComp, SocketName, SweepRadius, FrameDeltaTime);
	}
}

void UAnimNotifyState_SwatTrace::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (!MeshComp) return;

	if (ACatBase* Cat = Cast<ACatBase>(MeshComp->GetOwner()))
	{
		Cat->EndSwatTrace();
	}
}

FString UAnimNotifyState_SwatTrace::GetNotifyName_Implementation() const
{
	return TEXT("Swat Trace");
}
