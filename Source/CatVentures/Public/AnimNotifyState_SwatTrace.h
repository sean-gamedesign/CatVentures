// AnimNotifyState_SwatTrace.h — Stateless anim notify for The Swat active-frame sweep

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AnimNotifyState_SwatTrace.generated.h"

/**
 * Stateless AnimNotifyState that defines the active hit window for The Swat.
 *
 * This class holds ZERO mutable state (CDO-safe for multiplayer).
 * All per-instance trace data lives on ACatBase. Each tick, this notify
 * simply calls through to the owning character's trace interface:
 *   NotifyBegin → ACatBase::BeginSwatTrace()
 *   NotifyTick  → ACatBase::ProcessSwatTraceTick()
 *   NotifyEnd   → ACatBase::EndSwatTrace()
 */
UCLASS(meta = (DisplayName = "Swat Trace"))
class CATVENTURES_API UAnimNotifyState_SwatTrace : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	/** Socket on the skeleton mesh to trace from (e.g. the right front paw). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Swat")
	FName SocketName = TEXT("socket_paw_r");

	/** Radius of the sphere sweep in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Swat", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float SweepRadius = 15.0f;

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;

	virtual FString GetNotifyName_Implementation() const override;
};
