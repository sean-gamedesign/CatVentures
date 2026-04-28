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

	// ── Meow Time RPC ───────────────────────────────────────────────

	/** Server-fired multicast. Reaches every client (including the listen-server host) and forwards
	 *  to the ReceiveMeowTimeTriggered Blueprint event so BP can spawn the UMG + play the stinger.
	 *  Called from ACatGameMode::BeginMatchEnd at the moment match-end slow-mo kicks in. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_TriggerMeowTimeSpectacle();

	/** Implement on BP_CatGameState. Spawn the Meow Time UMG widget and play the audio stinger here.
	 *  Fires on every machine simultaneously via the multicast above.
	 *  Note: kept as a SEPARATE UFUNCTION from the multicast — stacking NetMulticast and
	 *  BlueprintImplementableEvent on one macro produces broken UHT reflection. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Match")
	void ReceiveMeowTimeTriggered();

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
