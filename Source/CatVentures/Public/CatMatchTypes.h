// CatMatchTypes.h — Shared enums and structs for the match-end sequence.

#pragma once

#include "CoreMinimal.h"
#include "CatMatchTypes.generated.h"

/** Phases of the match-end sequence. Replicated via ACatGameState. */
UENUM(BlueprintType)
enum class ECatMatchPhase : uint8
{
	Playing,
	Warning,     // Phase 1: slow-mo, movement input stripped, look preserved
	FinalCut,    // Phase 2: cinematic camera on break location
	Fade,        // Phase 2b: transition fade-to-black
	Aftermath    // Phase 3: scoreboard + panning cameras over destruction sites
};

/** Server-only record of a destroyed item (for top-3 ranking). */
USTRUCT(BlueprintType)
struct FDestroyedItemRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	FVector Location = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly)
	float Value = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	FString ItemName;
};

/** Per-player score entry, replicated on GameState for the scoreboard. */
USTRUCT(BlueprintType)
struct FCatPlayerScore
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly)
	int32 Score = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 ItemsDestroyed = 0;
};
