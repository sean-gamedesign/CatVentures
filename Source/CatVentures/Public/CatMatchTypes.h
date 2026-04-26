// CatMatchTypes.h — Shared enums and structs for the match-end sequence.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
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

/** DataTable row describing what happens when a given prop type is destroyed.
 *  The table asset is assigned on ACatGameMode; each BPC_ChaosItem references
 *  a row by FName key. */
USTRUCT(BlueprintType)
struct FChaosRewardData : public FTableRowBase
{
	GENERATED_BODY()

	/** Score added to the Chaos Meter when a prop with this row key is destroyed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.0"))
	float ChaosValue = 10.0f;

	/** UI-facing name (e.g., "Porcelain Vase") shown in destruction toasts + scoreboard. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FText DisplayName;

	/** Optional audio stinger override. If unset, the default break sound plays.
	 *  Soft pointer so unreferenced sounds aren't loaded with the table. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TSoftObjectPtr<class USoundBase> MeowStinger;

	/** Optional "Meow Time" reward window in seconds. 0 = no stinger window. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.0"))
	float MeowTimeDuration = 0.0f;

	/** Optional scoreboard icon. Soft pointer for the same reason as MeowStinger. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TSoftObjectPtr<class UTexture2D> Icon;
};
