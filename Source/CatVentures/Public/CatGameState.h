// CatGameState.h — Replicated match state visible to all clients.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "CatMatchTypes.h"
#include "CatGameState.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMatchPhaseChanged, ECatMatchPhase, NewPhase);

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

	// ── Delegates ───────────────────────────────────────────────────

	/** Broadcast locally when MatchPhase replicates — UI widgets bind to this. */
	UPROPERTY(BlueprintAssignable, Category = "Match")
	FOnMatchPhaseChanged OnMatchPhaseChanged;

	// ── Helpers ─────────────────────────────────────────────────────

	/** Returns ChaosScore / ChaosThreshold, clamped [0, 1]. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	float GetChaosPercent() const;

protected:
	UFUNCTION()
	void OnRep_MatchPhase();

	UFUNCTION()
	void OnRep_TopDestroyedLocations();
};
