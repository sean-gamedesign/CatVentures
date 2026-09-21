// CatCameraShakes.h — C++-defined camera shakes for the finale centerpiece.
//
// Defined in code (Perlin patterns configured in the constructor) so there is no
// Blueprint asset to author and the values are Live-Coding tunable.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraShakeBase.h"
#include "CatCameraShakes.generated.h"

/** Short, sharp thud — a hit landing on the shrine (locked or armed). */
UCLASS()
class CATVENTURES_API UCatShrineHitShake : public UCameraShakeBase
{
	GENERATED_BODY()
public:
	UCatShrineHitShake(const FObjectInitializer& ObjectInitializer);
};

/** Long rolling rumble — a section coming down, the unlock, the final collapse. */
UCLASS()
class CATVENTURES_API UCatShrineStageShake : public UCameraShakeBase
{
	GENERATED_BODY()
public:
	UCatShrineStageShake(const FObjectInitializer& ObjectInitializer);
};
