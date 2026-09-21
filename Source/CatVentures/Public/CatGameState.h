// CatGameState.h — Replicated match state visible to all clients.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "CatMatchTypes.h"
#include "CatObjectiveTypes.h"
#include "CatGameState.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMatchPhaseChanged, ECatMatchPhase, NewPhase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnObjectiveStatesChanged);

class ACatCenterpiece;

UCLASS()
class CATVENTURES_API ACatGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ── Replicated Match State ──────────────────────────────────────

	/** Current phase of the match-end sequence. Drives all client-side behaviour. */
	UPROPERTY(ReplicatedUsing = OnRep_MatchPhase, BlueprintReadOnly, Category = "Match")
	ECatMatchPhase MatchPhase = ECatMatchPhase::Playing;

	/** Accumulated chaos score — pushed from GameMode on every destruction event. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Match")
	float ChaosScore = 0.0f;

	/** Score required to trigger the match-end sequence. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Match")
	float ChaosThreshold = 100.0f;

	/** World location of the final object that broke (set at Phase 1 start). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Match")
	FVector FinalBreakLocation = FVector::ZeroVector;

	/** Top 3 most valuable destroyed item locations (set at Phase 3 start). */
	UPROPERTY(ReplicatedUsing = OnRep_TopDestroyedLocations, BlueprintReadOnly, Category = "Match")
	TArray<FVector> TopDestroyedLocations;

	/** Per-player scores for the scoreboard. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Match")
	TArray<FCatPlayerScore> PlayerScores;

	/** The map's checklist state — one entry per DT_MapObjectives row. Pushed by
	 *  the GameMode whenever a condition latches. The HUD derives its display
	 *  strings from the table and its fractions from these bitmasks + counts. */
	UPROPERTY(ReplicatedUsing = OnRep_ObjectiveStates, BlueprintReadOnly, Category = "Objectives")
	TArray<FObjectiveState> ObjectiveStates;

	/** Weighted-centroid of the densest cluster of destroyed props.
	 *  Computed by GameMode at Phase 3 entry; clients use it as the orbit pivot
	 *  for the Aftermath panning camera. Also passed via the phase RPC for
	 *  immediate read-availability — this field is the durable copy. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Match")
	FVector AftermathHotspot = FVector::ZeroVector;

	/** The map's finale centerpiece, if one exists (set by the GameMode on registration).
	 *  Replicated so clients can render its HUD line from the actor's own replicated state.
	 *  Null on maps without one — those keep the meter-threshold match end. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Finale")
	TObjectPtr<ACatCenterpiece> Centerpiece;

	// ── Delegates ───────────────────────────────────────────────────

	/** Broadcast locally when MatchPhase replicates — UI widgets bind to this. */
	UPROPERTY(BlueprintAssignable, Category = "Match")
	FOnMatchPhaseChanged OnMatchPhaseChanged;

	/** Broadcast locally when ObjectiveStates replicates — the checklist HUD binds
	 *  to this rather than polling on Tick. */
	UPROPERTY(BlueprintAssignable, Category = "Objectives")
	FOnObjectiveStatesChanged OnObjectiveStatesChanged;

	// ── Helpers ─────────────────────────────────────────────────────

	/** Returns ChaosScore / ChaosThreshold, clamped [0, 1]. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	float GetChaosPercent() const;

	/** True once every NON-finale objective is complete — the finale set piece's gate.
	 *  Derived from replicated state, so clients answer it without the GameMode. */
	UFUNCTION(BlueprintPure, Category = "Objectives")
	bool AreNonFinaleObjectivesComplete() const;

	/** The whole checklist as ONE newline-joined block, ready for a single TextBlock.
	 *
	 *  Formatting lives in C++ deliberately: §5 specifies a "deliberately ugly" debug
	 *  list, and one SetText call is a far smaller Blueprint surface than building
	 *  and destroying TextBlocks per row. When the list becomes real UI this is the
	 *  function to delete, not to grow. */
	UFUNCTION(BlueprintPure, Category = "Objectives")
	FText GetObjectiveChecklistText() const;

	/** True iff at least one player exists and every ACatPlayerState in PlayerArray
	 *  has bWantsRematch == true. Host's "Play Again" button polls or binds this
	 *  to gate ServerTravel — prevents the host accidentally yanking unready players. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Match")
	bool AllPlayersReadyForRematch() const;

protected:
	UFUNCTION()
	void OnRep_MatchPhase();

	UFUNCTION()
	void OnRep_TopDestroyedLocations();

	UFUNCTION()
	void OnRep_ObjectiveStates();
};
