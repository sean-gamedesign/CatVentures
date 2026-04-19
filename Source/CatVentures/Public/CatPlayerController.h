// CatPlayerController.h — Handles pause menu toggling and match-end phase orchestration.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "CatMatchTypes.h"
#include "PauseMenuWidget.h"
#include "CatPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UUserWidget;
class ACameraActor;

UCLASS()
class CATVENTURES_API ACatPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	/** Widget class for the pause menu. Assign WBP_PauseMenu in the Blueprint subclass. */
	UPROPERTY(EditAnywhere, Category = "UI")
	TSubclassOf<UUserWidget> PauseMenuClass;

	/** Opens or closes the pause menu. Called by IA_ToggleMenu binding and by UPauseMenuWidget on Escape. */
	void ToggleMenu();

	// ── Match Phase Orchestration ───────────────────────────────────

	/** Client RPC — single entry point for all match-end phase transitions.
	 *  Called by ACatGameMode::NotifyAllControllersPhaseChanged. */
	UFUNCTION(Client, Reliable)
	void Client_OnMatchPhaseChanged(ECatMatchPhase NewPhase, FVector PhaseLocation, AActor* TargetActor);

	/** Look-only mapping context — contains only IA_Look.
	 *  Swapped in during Phase 1 to strip movement while preserving camera look.
	 *  Assign IMC_LookOnly in the Blueprint subclass. */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputMappingContext> LookOnlyMappingContext;

	/** Returns the averaged world position of broken-off leaf fragments on the target's
	 *  GeometryCollectionComponent. Gives the cinematic camera a tracking point that follows
	 *  the debris cloud. Falls back to GetActorLocation() if no fragments have broken off. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	FVector GetChaosTargetLocation(AActor* TargetActor) const;

	/** Captures the possessed pawn's active UCameraComponent world transform, spawns an
	 *  ACameraActor at that exact lens pose (FOV + post-process copied for seamless handoff),
	 *  and blends the view target to it. Returns the spawned camera so Blueprint can drive
	 *  it per-tick (e.g., track the debris centroid). Returns nullptr if no pawn or camera. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	ACameraActor* SpawnCinematicTrackerCamera(float BlendTime = 0.0f);

protected:
	virtual void SetupInputComponent() override;

	/** Server-side: resets input mode when the pawn is possessed (covers listen-server host). */
	virtual void OnPossess(APawn* InPawn) override;

	/** Client-side: resets input mode once the server confirms possession via ClientRestart. */
	virtual void AcknowledgePossession(APawn* P) override;

private:
	/** Instantiated widget — UPROPERTY keeps it GC-safe while off-screen. */
	UPROPERTY()
	TObjectPtr<UUserWidget> PauseMenuInstance;

	/** IA_ToggleMenu input action asset. Assign in the Blueprint subclass. */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> ToggleMenuAction;

	// ── Phase Handlers ──────────────────────────────────────────────

	void HandlePhase_Warning();
	void HandlePhase_FinalCut(FVector BreakLocation, AActor* TargetActor);
	void HandlePhase_Fade();
	void HandlePhase_Aftermath();

protected:
	// ── Blueprint Implementable Events ──────────────────────────────
	// C++ handles engine-level work (input mode, dilation).
	// These events let the Blueprint subclass wire up visuals: camera blends, fades, UI.

	/** Phase 2: Blueprint spawns a camera actor, positions it to track TargetActor,
	 *  and calls SetViewTargetWithBlend. TargetLocation is the actor's location at break time. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Match")
	void OnCinematicTakeover(FVector TargetLocation, AActor* TargetActor);

	/** Phase 2b: Blueprint plays a fade-to-black widget animation. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Match")
	void OnMatchTransitionFade();

	/** Phase 3: Blueprint unlocks mouse, shows scoreboard widget,
	 *  and cuts to the level panning camera. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Match")
	void OnShowScoreboard();
};
