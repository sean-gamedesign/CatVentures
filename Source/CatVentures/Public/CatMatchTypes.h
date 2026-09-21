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

	/** Hits this player landed on the finale centerpiece (convergence loop, 2026-09-20). */
	UPROPERTY(BlueprintReadOnly)
	int32 FinaleHits = 0;

	/** True on the top scorer — the scoreboard marks them. */
	UPROPERTY(BlueprintReadOnly)
	bool bMVP = false;
};

// ── Finale centerpiece (convergence loop, 2026-09-20) ───────────────────────────

/** How a hit reached the centerpiece. */
UENUM(BlueprintType)
enum class ECatFinaleHitKind : uint8
{
	Swat,
	Charge,
	Impact      // a hurled / knocked prop striking it
};

/** Feedback beats the centerpiece multicasts to every machine. */
UENUM(BlueprintType)
enum class ECatFinaleEvent : uint8
{
	LockedHit,      // a hit bounced off while locked
	WrongSection,   // legacy top-only mode: the hit landed below the top section and was rejected
	LowHit,         // counted, but on a lower tier — small damage ("the glowing tier hits harder")
	Hit,            // a counted hit on the glowing top tier
	Unlocked,       // the meter crossed the unlock line
	WindowExpired,  // not every cat hit in time — reset
	StageComplete,  // a section came down
	Destroyed       // the base came down — match end follows
};

/** Replicated state of the finale centerpiece — owned by ACatCenterpiece, read by the HUD. */
USTRUCT(BlueprintType)
struct FCatFinaleState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	bool bUnlocked = false;

	UPROPERTY(BlueprintReadOnly)
	bool bDestroyed = false;

	UPROPERTY(BlueprintReadOnly)
	int32 StagesDone = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 NumStages = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 RequiredCats = 1;

	/** Names of the cats that have landed a hit in the current co-op window. */
	UPROPERTY(BlueprintReadOnly)
	TArray<FString> CatsHitThisWindow;

	/** Server world time the current co-op window closes; <= 0 = no window open. */
	UPROPERTY(BlueprintReadOnly)
	float WindowEndsAt = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float UnlockPercent = 0.6f;

	// ── Tier health (round 6, 2026-09-21: "any damage counts, top to bottom is the real dealer") ──

	/** Damage dealt to the current top tier so far; the tier falls at StageHP. */
	UPROPERTY(BlueprintReadOnly)
	float StageDamage = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float StageHP = 100.0f;

	/** Distinct cats that hit inside the current window, and the damage multiplier that earns. */
	UPROPERTY(BlueprintReadOnly)
	int32 CoopCats = 1;

	UPROPERTY(BlueprintReadOnly)
	float CoopMultiplier = 1.0f;

	/** Damage of the last counted hit, for the readout. */
	UPROPERTY(BlueprintReadOnly)
	float LastHitDamage = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	bool bLastHitTop = false;

	// ── Presence (2026-09-21, Sean: "all cats needed to be around") ──

	/** Cats of the match currently within the shrine's ConvergenceRadius (RequiredCats = all of them). */
	UPROPERTY(BlueprintReadOnly)
	int32 CatsNear = 1;

	/** Damage scale from presence: (CatsNear / RequiredCats)^2, floored — 1 when everyone is here. */
	UPROPERTY(BlueprintReadOnly)
	float PresenceScale = 1.0f;
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

	// ── Heavy tier (Objective System §3) ────────────────────────────────
	//
	// Heavies are the map's untouchables — "you'll get to break that later",
	// legible with zero tutorial text. Swat accumulation and the bumper's
	// guaranteed shatter are BOTH off for them (the bumper must not leak the
	// finale's toy); only the hard-impact path can break a Heavy.

	/** Marks this prop type as a Heavy. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Heavy")
	bool bHeavyTier = false;

	/** NON-Heavy impact threshold, in cm/s of the VICTIM's own speed — the
	 *  quantity the existing path measures, kept for props already tuned to it.
	 *  0 = use the GameMode's project default. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Heavy", meta = (ClampMin = "0.0"))
	float ImpactVelocityThreshold = 0.0f;

	/** HEAVY impact threshold, in impulse units of the IMPACTOR.
	 *
	 *  Two named columns rather than one float whose units flip on bHeavyTier:
	 *  a single value differing by orders of magnitude between rows, meaning
	 *  different things depending on a sibling bool, is a designer trap. And
	 *  moving every prop to impulse would silently re-tune every shipped
	 *  breakable's playtested threshold, which is the wrong side of that trade.
	 *  0 = use the GameMode's project default. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Heavy", meta = (ClampMin = "0.0"))
	float ImpactImpulseThreshold = 0.0f;

	/** Swats needed to shatter this prop (the swat-accumulation break path). 0 = use
	 *  ACatGameMode::DefaultSwatsToBreak. Data-driven since 2026-09-13 — BPC_ChaosItem
	 *  used to hardcode 4, which made a vase as stubborn as a fridge. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Break", meta = (ClampMin = "0"))
	int32 SwatsToBreak = 0;

	/** False = AMBIENT: breaking it scores points for the cat but does NOT move the chaos
	 *  meter, so it can never unlock the finale. The block-wide filler props (convergence
	 *  loop, 2026-09-20 — Sean: "seeded with destructibles that don't count toward the shrine"). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Break")
	bool bFeedsMeter = true;
};
