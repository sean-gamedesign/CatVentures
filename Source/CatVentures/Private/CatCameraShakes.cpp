// CatCameraShakes.cpp

#include "CatCameraShakes.h"
#include "Shakes/PerlinNoiseCameraShakePattern.h"

UCatShrineHitShake::UCatShrineHitShake(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSingleInstance = false;

	UPerlinNoiseCameraShakePattern* Pattern = CreateDefaultSubobject<UPerlinNoiseCameraShakePattern>(TEXT("Pattern"));
	Pattern->Duration     = 0.28f;
	Pattern->BlendInTime  = 0.02f;
	Pattern->BlendOutTime = 0.18f;

	Pattern->X.Amplitude = 5.0f;  Pattern->X.Frequency = 32.0f;
	Pattern->Y.Amplitude = 5.0f;  Pattern->Y.Frequency = 30.0f;
	Pattern->Z.Amplitude = 3.5f;  Pattern->Z.Frequency = 36.0f;

	Pattern->Pitch.Amplitude = 1.4f; Pattern->Pitch.Frequency = 30.0f;
	Pattern->Yaw.Amplitude   = 0.9f; Pattern->Yaw.Frequency   = 26.0f;
	Pattern->Roll.Amplitude  = 0.7f; Pattern->Roll.Frequency  = 22.0f;

	SetRootShakePattern(Pattern);
}

UCatShrineStageShake::UCatShrineStageShake(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSingleInstance = true;   // a new rumble restarts the old one rather than stacking

	UPerlinNoiseCameraShakePattern* Pattern = CreateDefaultSubobject<UPerlinNoiseCameraShakePattern>(TEXT("Pattern"));
	Pattern->Duration     = 1.1f;
	Pattern->BlendInTime  = 0.05f;
	Pattern->BlendOutTime = 0.6f;

	Pattern->X.Amplitude = 14.0f; Pattern->X.Frequency = 14.0f;
	Pattern->Y.Amplitude = 14.0f; Pattern->Y.Frequency = 12.0f;
	Pattern->Z.Amplitude = 10.0f; Pattern->Z.Frequency = 18.0f;

	Pattern->Pitch.Amplitude = 3.0f; Pattern->Pitch.Frequency = 12.0f;
	Pattern->Yaw.Amplitude   = 2.0f; Pattern->Yaw.Frequency   = 10.0f;
	Pattern->Roll.Amplitude  = 2.5f; Pattern->Roll.Frequency  = 9.0f;

	Pattern->FOV.Amplitude = 1.5f; Pattern->FOV.Frequency = 8.0f;

	SetRootShakePattern(Pattern);
}
