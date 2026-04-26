// CatGameMode.h — Server-authoritative match state machine.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "CatMatchTypes.h"
#include "CatGameMode.generated.h"

class UDataTable;

UCLASS()
class CATVENTURES_API ACatGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	// ── Score Reporting ─────────────────────────────────────────────

	/** Called by BPC_ChaosItem (Blueprint) when a GC actor is destroyed.
	 *  ChaosRewardKey names a row in ChaosRewardTable; the GameMode resolves the score,
	 *  display name, and stinger authoritatively. Triggers match-end if threshold is reached. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	void ReportItemDestroyed(AActor* Item, FVector Location, FName ChaosRewardKey);

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

	/** How long Phase 1 (slow-mo warning) lasts in wall-clock seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning")
	float WarningDuration = 3.0f;

	/** How long Phase 2 (cinematic camera hold) lasts in wall-clock seconds before the fade begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float CinematicHoldDuration = 2.5f;

	/** How long the fade-to-black transition lasts in wall-clock seconds before the scoreboard appears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float FadeDuration = 2.0f;

	/** Time dilation applied during Phases 1, 2, and Fade. 0.2 = 5× slow-mo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SlowMoDilation = 0.2f;

protected:
	virtual void BeginPlay() override;

private:
	// ── Phase Transitions ───────────────────────────────────────────

	void BeginMatchEnd();
	void TransitionToFinalCut();
	void TransitionToFade();
	void TransitionToAftermath();

	/** Sends Client_OnMatchPhaseChanged to every connected PlayerController. */
	void NotifyAllControllersPhaseChanged(ECatMatchPhase NewPhase, AActor* TargetActor);

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
