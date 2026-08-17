// CatObjectiveTypes.h — Objective System data schema (per-map checklists).
//
// Two-tier model, per Saved/.Aura/plans/objective-system-v1.md §2:
//   Tier 1 CONDITIONS are self-contained world-state checks (atoms).
//   Tier 2 OBJECTIVES are display string + array of conditions, ANDed (molecules).
// AND is NOT a condition type — it is the layer above them. Future operators
// (OR, ordering, temporal) become new cases in ACatGameMode::EvaluateObjective
// and nothing else; if they need to touch anything else, the two-tier split has
// been violated somewhere.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "CatObjectiveTypes.generated.h"

/** What a single condition checks. Growable — the schema is future-proofed now,
 *  the machinery is built only for what v1 uses (milestone 1a: Destroy only). */
UENUM(BlueprintType)
enum class EObjectiveConditionType : uint8
{
	/** Prop with the tag has been shattered. */
	Destroy		UMETA(DisplayName = "Destroy"),

	/** Prop with the tag overlaps the tagged trigger volume. */
	Relocate	UMETA(DisplayName = "Relocate"),

	/** Prop with the tag has left its start region. Structurally Relocate's
	 *  inverse (exited-volume rather than entered-volume) — same machinery. */
	KnockOff	UMETA(DisplayName = "Knock Off")
};

/** One atom of an objective. */
USTRUCT(BlueprintType)
struct FObjectiveCondition
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	EObjectiveConditionType Type = EObjectiveConditionType::Destroy;

	/** Matches UCatObjectiveTargetComponent::ObjectiveTag. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FName TargetTag;

	/** Relocate / KnockOff only. Unused by Destroy. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FName VolumeTag;

	/** 0 = ALL matching tags must satisfy the condition; >0 = any N of them.
	 *  Count-of respects latch-on-true per satisfied tag: each tag's contribution
	 *  latches individually and the condition completes when the latched count
	 *  reaches N. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	int32 CountRequired = 0;
};

/** One row of DT_MapObjectives — one checklist line the player reads. */
USTRUCT(BlueprintType)
struct FMapObjectiveRow : public FTableRowBase
{
	GENERATED_BODY()

	/** "The picnic is ruined" — the plain-English line shown on the HUD. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FText DisplayName;

	/** Objective is complete iff EVERY condition here is latched true (AND). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<FObjectiveCondition> Conditions;

	/** Exactly one row per map. Completing every NON-finale row unlocks it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bIsFinaleSetPiece = false;
};

/** Replicated per-objective state. ONE atomic struct per objective rather than
 *  parallel arrays of names/progress — that shape dodges the parallel-array
 *  desync class outright (the BB-14 lesson: verify the shape before it is copied).
 *
 *  ConditionProgress is NOT redundant with the bitmask: the mask carries one bit
 *  PER CONDITION, but a count-of condition is a SINGLE condition whose progress
 *  lives inside it. Without the counts, "any 4 of 6 portraits" would render 0/1
 *  until it snapped to 1/1 — no feedback across exactly the objective where a
 *  player most needs it, which is the "did that count?" contamination §5 says the
 *  HUD fraction exists to prevent. */
USTRUCT(BlueprintType)
struct FObjectiveState
{
	GENERATED_BODY()

	/** Row name in DT_MapObjectives. */
	UPROPERTY(BlueprintReadOnly)
	FName RowName;

	/** The checklist line the player reads, copied from the row at init.
	 *  Carried HERE rather than looked up client-side because DT_MapObjectives lives
	 *  on the GameMode, which clients do not have — and §11 requires that nothing
	 *  else hold a table reference. Copying it once makes the state self-describing. */
	UPROPERTY(BlueprintReadOnly)
	FText DisplayName;

	/** True for the finale set-piece row. Lets the HUD grey it without needing the table. */
	UPROPERTY(BlueprintReadOnly)
	bool bIsFinale = false;

	/** Bit N set = condition N has latched true. Latch-on-true: bits only ever
	 *  go false->true. The naive level-triggered re-check is WRONG — physics
	 *  props settle unpredictably and un-completing reads as a bug. */
	UPROPERTY(BlueprintReadOnly)
	int32 LatchedMask = 0;

	/** Bit N set = condition N completed via forgiveness rather than achievement.
	 *  Display-only; a forgiven condition is still complete. */
	UPROPERTY(BlueprintReadOnly)
	int32 ForgivenMask = 0;

	/** Per-condition satisfied-target count, parallel to the row's Conditions
	 *  array. Drives the HUD fraction for count-of conditions. */
	UPROPERTY(BlueprintReadOnly)
	TArray<uint8> ConditionProgress;

	/** Per-condition target count needed to latch (CountRequired, or the number of
	 *  registered targets when CountRequired is 0). Resolved once at init, after
	 *  registration closes — the denominator of the HUD fraction. */
	UPROPERTY(BlueprintReadOnly)
	TArray<uint8> ConditionRequired;

	/** True when every condition in the owning row has latched. */
	UPROPERTY(BlueprintReadOnly)
	bool bComplete = false;
};
