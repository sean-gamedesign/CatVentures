// CatGameMode.h — Server-authoritative match state machine.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "CatMatchTypes.h"
#include "CatObjectiveTypes.h"
#include "CatGameMode.generated.h"

class UDataTable;
class UCatObjectiveTargetComponent;

UCLASS()
class CATVENTURES_API ACatGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ACatGameMode();

	// ── Score Reporting ─────────────────────────────────────────────

	/** Called by BPC_ChaosItem (Blueprint) when a GC actor is destroyed.
	 *  ChaosRewardKey names a row in ChaosRewardTable; the GameMode resolves the score,
	 *  display name, and stinger authoritatively. Triggers match-end if threshold is reached. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	void ReportItemDestroyed(AActor* Item, FVector Location, FName ChaosRewardKey);

	// ── Objectives ──────────────────────────────────────────────────

	/** Called from UCatObjectiveTargetComponent::BeginPlay. Registration CLOSES at
	 *  StartPlay — see the comment there for why that is the correct anchor. */
	void RegisterObjectiveTarget(UCatObjectiveTargetComponent* Target);
	void UnregisterObjectiveTarget(UCatObjectiveTargetComponent* Target);

	/** A target's condition state changed (destroyed / entered volume / left region).
	 *  Re-evaluates every objective that references the target's tag. */
	void NotifyObjectiveTargetChanged(UCatObjectiveTargetComponent* Target);

	/** DataTable of FMapObjectiveRow rows — the map's checklist.
	 *  Assign DT_MapObjectives on GM_CatVentures. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Match|Objectives",
	          meta = (RequiredAssetDataTags = "RowStructure=/Script/CatVentures.MapObjectiveRow"))
	TObjectPtr<UDataTable> MapObjectiveTable;

	// ── Tuning ──────────────────────────────────────────────────────

	/** DataTable of FChaosRewardData rows — one per breakable prop type.
	 *  Assign DT_ChaosRewards on BP_CatGameMode. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Match|Tuning",
	          meta = (RequiredAssetDataTags = "RowStructure=/Script/CatVentures.ChaosRewardData"))
	TObjectPtr<UDataTable> ChaosRewardTable;

	/** Fallback score applied when a prop's key is None or missing from the table. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.0"))
	float DefaultChaosValue = 5.0f;

	/** Total chaos score required to end the match. Pushed to GameState for HUD display. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "1.0"))
	float ChaosThreshold = 100.0f;

	/** How long Phase 1 (slow-mo warning) lasts in wall-clock seconds.
	 *  ClampMin added 2026-08-09 (BB-03): this was the only Match|Tuning float without
	 *  one, and a 0.01 debug value sat in the GM_CatVentures CDO unnoticed — at
	 *  SlowMoDilation 0.2 that scheduled 0.002 game-seconds, so the entire slow-mo
	 *  beat and the Meow Time widget were created and instantly overtaken by FinalCut. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float WarningDuration = 3.0f;

	/** How long Phase 2 (cinematic camera hold) lasts in wall-clock seconds before the fade begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float CinematicHoldDuration = 2.5f;

	/** How long the fade-to-black transition lasts in wall-clock seconds before the scoreboard appears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float FadeDuration = 2.0f;

	/** Real-time hold (wall-clock seconds) AFTER slow-mo ends but BEFORE the fade RPC fires.
	 *  Lets the destruction physics settle at full speed so players actually see the aftermath
	 *  before the screen darkens. Tuned per-feel; designers usually want 1.5–3.0s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.0"))
	float PostCinematicHoldDuration = 2.0f;

	/** Time dilation applied during Phases 1, 2, and Fade. 0.2 = 5× slow-mo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SlowMoDilation = 0.2f;

	/** Spatial-bucket size (unreal units) for the chaos hotspot density calc.
	 *  All destroyed-prop locations are bucketed into cells of this size; the cell
	 *  with the highest summed Value wins, and its weighted centroid becomes the
	 *  Aftermath orbit pivot. Larger = looser clustering, smaller = tighter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "100.0"))
	float AftermathCellSize = 800.0f;

protected:
	virtual void BeginPlay() override;

	/** Closes objective-target registration, runs startup validation, and performs
	 *  the first evaluation.
	 *
	 *  AGameModeBase::StartPlay calls GetWorldSettings()->NotifyBeginPlay(), which
	 *  dispatches BeginPlay to every spawned actor — so the point immediately AFTER
	 *  Super::StartPlay() is the concrete "world init has settled" anchor, and no
	 *  condition is ever evaluated against a possibly-partial registry.
	 *
	 *  NOT HandleMatchHasStarted: that hook is declared on AGameMode, and this class
	 *  derives from AGameModeBase. Do not reparent to obtain it — AGameMode drags in
	 *  a MatchState machine that would sit alongside ECatMatchPhase as a second owner
	 *  of "what phase is the match in". */
	virtual void StartPlay() override;

	/** Override AGameModeBase to force AdjustIfPossibleButAlwaysSpawn on the pawn spawn.
	 *  Prevents the "black screen on join" failure when multiple players share a PlayerStart. */
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform) override;

private:
	// ── Phase Transitions ───────────────────────────────────────────

	void BeginMatchEnd();
	void TransitionToFinalCut();
	void TransitionToFade();
	/** Fired after PostCinematicHoldDuration elapses inside the Fade phase.
	 *  Sends the Fade RPC (which triggers PCM StartCameraFade on each client) and
	 *  schedules TransitionToAftermath. Splitting this from TransitionToFade is what
	 *  gives players the "real-time settle" window the designer asked for. */
	void BeginActualFade();
	void TransitionToAftermath();

	/** Sends Client_OnMatchPhaseChanged to every connected PlayerController.
	 *  PhaseLocation is repurposed per phase: FinalBreakLocation for Warning/FinalCut/Fade,
	 *  AftermathHotspot for Aftermath. The RPC carries the value alongside the GameState
	 *  field, eliminating the property-replication-vs-RPC race on phase entry. */
	void NotifyAllControllersPhaseChanged(ECatMatchPhase NewPhase, FVector PhaseLocation, AActor* TargetActor);

	/** Bucket-histograms DestroyedItems and returns the weighted centroid of the
	 *  highest-value cluster. Called once per match at Aftermath entry.
	 *  Returns FinalBreakLocation as a sane fallback if the destruction list is empty. */
	FVector ComputeChaosHotspot() const;

	// ── Objectives ──────────────────────────────────────────────────

	/** Validates every condition resolves against the registry. Catches the
	 *  VACUOUS-COMPLETION hole: "all matching tags" over an EMPTY set is trivially
	 *  true, so a typo'd TargetTag would complete conditions at spawn and unlock the
	 *  finale before anyone moved. Warnings only — a bad table must not hard-fail a
	 *  playtest, but it must be loud. */
	void ValidateObjectives();

	/** Builds one FObjectiveState per row, sized to that row's condition count. */
	void InitializeObjectiveStates();

	/** THE single evaluation point. Latch-on-true: bits only ever go false->true.
	 *  Future operators (OR / ordering / temporal) become new cases HERE and
	 *  nowhere else. Returns true if anything changed. */
	bool EvaluateObjective(FName RowName);

	/** Counts registered targets carrying Tag, and how many satisfy Condition. */
	void CountTargetsForCondition(const FObjectiveCondition& Condition,
	                              int32& OutTotal, int32& OutSatisfied) const;

	/** Copies the server's objective state onto the GameState for replication. */
	void PushObjectiveStatesToGameState();

	/** Registered objective targets. Weak: a target destroyed mid-match must not
	 *  keep its component alive, and the evaluator skips stale entries. */
	TArray<TWeakObjectPtr<UCatObjectiveTargetComponent>> ObjectiveTargets;

	/** Server-side authoritative objective state; mirrored to GameState on change. */
	TArray<FObjectiveState> ObjectiveStates;

	/** Set in StartPlay. Late registrations after this are a level-authoring error
	 *  (a streamed objective target) and are warned about rather than silently
	 *  changing the HUD denominator mid-match. */
	bool bObjectiveRegistrationClosed = false;

	// ── State ───────────────────────────────────────────────────────

	ECatMatchPhase CurrentPhase = ECatMatchPhase::Playing;
	float TotalChaosScore = 0.0f;

	/** Every destroyed item recorded during the match (sorted at match end for top-3). */
	TArray<FDestroyedItemRecord> DestroyedItems;

	/** Location of the final object that triggered the match end. */
	FVector FinalBreakLocation = FVector::ZeroVector;

	/** The actor whose destruction triggered the match end (for cinematic tracking). */
	TWeakObjectPtr<AActor> FinalBreakActor;

	FTimerHandle PhaseTimerHandle;
};
