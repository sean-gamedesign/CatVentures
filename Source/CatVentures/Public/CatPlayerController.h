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
class UPanelWidget;

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

	/** Filtered Center of Mass — averages every broken chunk that survives the abyss
	 *  filter (chunk Z >= AftermathPivotMinZ). If the surviving chunk count is below
	 *  AftermathMinChunkCount, OutActiveCount is reported as zero so the director
	 *  treats the prop as "not enough debris to look at" and skips it. Returns the
	 *  actor's location as a neutral fallback whenever OutActiveCount is zero. */
	FVector GetChaosActiveChunkCentroid(AActor* TargetActor, int32& OutActiveCount) const;

	/** Captures the possessed pawn's active UCameraComponent world transform, spawns an
	 *  ACameraActor at that exact lens pose (FOV + post-process copied for seamless handoff),
	 *  and blends the view target to it. Returns the spawned camera so Blueprint can drive
	 *  it per-tick (e.g., track the debris centroid). Returns nullptr if no pawn or camera. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	ACameraActor* SpawnCinematicTrackerCamera(float BlendTime = 0.0f);

	/** Spawns the Aftermath orbit camera at the chaos hotspot, instant-cuts the view target
	 *  to it (we're under fade-to-black), and starts the orbit motion timer. Returns the
	 *  spawned actor so Blueprint can introspect it if needed. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	ACameraActor* SpawnAftermathCamera(FVector Hotspot);

	// ── Scoreboard Population ───────────────────────────────────────

	/** Static BP-callable: clears Container and creates one row per FCatPlayerScore in the
	 *  current GameState. The row class is expected to expose two TextBlock children named
	 *  "TxtPlayerName" and "TxtScore" (the convention WBP_ScoreRow follows). Bypasses the
	 *  need for a ForEachLoop macro from BP, which the VibeUE node API can't surface. */
	UFUNCTION(BlueprintCallable, Category = "Match",
	          meta = (WorldContext = "WorldContextObject"))
	static void PopulateScoreboard(UObject* WorldContextObject,
	                               UPanelWidget* Container,
	                               TSubclassOf<UUserWidget> RowClass);

	// ── Rematch / Travel ────────────────────────────────────────────

	/** Client → Server: toggles bWantsRematch on this controller's PlayerState.
	 *  Validated server-side (only honored during the Aftermath phase). */
	UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Match")
	void Server_SetRematchReady(bool bReady);

	/** Host-only BP wrapper around UWorld::ServerTravel — drags every connected
	 *  client to the new map. Pass "?Restart" to restart the current map, or a full
	 *  package path like "/Game/Maps/MainMenu". Silently no-ops on clients. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	void Host_ServerTravel(const FString& URL);

	// ── Aftermath Cosmetic Tuning (client-side, designer-tunable) ───

	/** Wall-clock seconds to fade to black during Phase Fade. Internally compensated
	 *  for the active SlowMoDilation so the fade completes in real time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float MatchFadeOutDuration = 2.0f;

	/** Wall-clock seconds to fade back from black at Aftermath entry. Dilation is 1.0
	 *  here so no compensation is applied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float MatchFadeInDuration = 0.7f;

	/** Seconds the orbit camera reveals destruction before the scoreboard widget is added. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.0"))
	float AftermathHoldDuration = 3.0f;

	/** Horizontal radius (uu) of the orbit around the hotspot. Tuned to frame a group
	 *  of debris chunks, not a single point. Raise for wider establishing sweeps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "50.0"))
	float AftermathOrbitRadius = 500.0f;

	/** Vertical offset (uu) above the hotspot. Clamped non-negative so the camera can't
	 *  drop under the floor by accident. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.0"))
	float AftermathOrbitHeight = 150.0f;

	/** Yaw degrees per second around the hotspot. 6° = full orbit in 60s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning")
	float AftermathPanRateDeg = 6.0f;

	/** VInterpTo rate the orbit pivot uses to chase the lead-chunk target. Higher =
	 *  snappier tracking (camera doesn't lag fast-moving chunks); lower = lazier and
	 *  more cinematic. Bumped to 5.0 to keep up with single-chunk lead-target tracking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.1"))
	float AftermathOrbitFollowSpeed = 5.0f;

	/** Draws a debug sphere on the live orbit pivot every tick. Stripped from Shipping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning")
	bool bDrawAftermathDebug = false;

	/** Per-actor abyss filter: any debris actor whose live chunk-centroid Z is below
	 *  this threshold is skipped during centroid averaging. Stops a single fallen chunk
	 *  (or an entire prop that fell through the floor) from poisoning the average and
	 *  dragging the orbit camera into the basement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning")
	float AftermathDebrisFloorZ = -500.0f;

	/** Final-line defense: the smoothed orbit pivot Z is clamped to be at least this
	 *  value, regardless of what the live centroid reports. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning")
	float AftermathPivotMinZ = -100.0f;

	/** Minimum number of broken chunks (after the abyss filter) a prop must have for
	 *  GetChaosActiveChunkCentroid to consider its hero pile cinematically interesting.
	 *  Below this threshold the helper reports OutActiveCount=0, which makes the
	 *  director skip the prop at cut time. Stops the camera from framing single
	 *  invisible rogue shards on the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "1"))
	int32 AftermathMinChunkCount = 5;

	/** Wall-clock seconds the director holds on each prop before cutting to the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.5"))
	float AftermathShotIntervalSec = 8.0f;

	/** PCM blend duration when cutting between shots. (Currently unused — dip-to-black
	 *  replaces the live-blend transition.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.0"))
	float AftermathShotBlendTimeSec = 1.0f;

	/** Wall-clock seconds the screen takes to fade to black before a cut. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.0"))
	float AftermathFadeOutDuration = 0.4f;

	/** Wall-clock seconds the screen takes to fade back from black after a cut. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match|Tuning", meta = (ClampMin = "0.0"))
	float AftermathFadeInDuration = 0.4f;

	// ── Debris registration (called from BPC_ChaosItem on every machine that breaks a prop) ──

	/** Adds DebrisActor to the per-client live-tracking list. Called from BP on Native_Shatter
	 *  via the local PlayerController so each client builds its own list of broken props. */
	UFUNCTION(BlueprintCallable, Category = "Match")
	void RegisterDebrisActor(AActor* DebrisActor);

protected:
	virtual void SetupInputComponent() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

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
	void HandlePhase_Aftermath(FVector Hotspot);

	// ── Aftermath orbit state ───────────────────────────────────────

	/** Spawned camera actor used for the Aftermath orbit. UPROPERTY for GC safety. */
	UPROPERTY()
	TObjectPtr<ACameraActor> AftermathCameraActor;

	/** Driven by AftermathOrbitTimerHandle — recomputes camera pose around the hotspot. */
	void TickAftermathOrbit();

	/** Fires the OnShowScoreboard BIE after the orbit reveal hold completes. */
	void ShowScoreboardDeferred();

	FTimerHandle AftermathOrbitTimerHandle;
	FTimerHandle ScoreboardRevealTimerHandle;
	float AftermathOrbitElapsed = 0.0f;

	// ── Director-cut state ──────────────────────────────────────────

	/** Index into ActiveDebrisActors of the prop currently being orbited. */
	int32 CurrentTargetIndex = 0;

	/** Seconds elapsed in the current shot. When it crosses AftermathShotIntervalSec
	 *  we cut to the next prop. */
	float ShotTimer = 0.0f;

	/** True between fade-out start and fade-in completion. While true, ShotTimer is
	 *  paused so each prop gets a full AftermathShotIntervalSec of CLEAR viewing — fade
	 *  time doesn't eat into the shot. */
	bool bIsCutting = false;

	/** Returns the actor at CurrentTargetIndex, skipping forward over GC'd weak refs.
	 *  Returns nullptr only when ActiveDebrisActors is fully empty/invalid. */
	AActor* ResolveCurrentTarget() const;

	/** Step 1 of a director cut: fades the screen to black. Schedules PerformCutSwap. */
	void StartCutFadeOut();

	/** Step 2 of a director cut: with the screen fully black, advances the index, swaps
	 *  the camera (hard-cut view target), destroys the old camera, and starts the
	 *  fade-in. Schedules FinishCut. */
	void PerformCutSwap();

	/** Step 2 variant for the initial Aftermath entry: same as PerformCutSwap but does
	 *  NOT advance CurrentTargetIndex (it was already set to the lead-chunk index by
	 *  HandlePhase_Aftermath). Both paths funnel into SwapToCurrentTargetCameraAndFadeIn. */
	void PerformAftermathEntrySwap();

	/** Shared swap-and-fade-in body used by both PerformCutSwap and PerformAftermathEntrySwap.
	 *  Spawns the new camera at the current target's pre-clamped pivot, hard-cuts the
	 *  view target under black, destroys the old camera, fades back in, schedules FinishCut. */
	void SwapToCurrentTargetCameraAndFadeIn();

	/** Picks the prop with the biggest hero pile — the one whose filtered Center of
	 *  Mass has the most contributing chunks. Tiebreaks by max distance from the
	 *  hotspot, so when two props have equally-sized piles we open on the one whose
	 *  debris flew further. Returns 0 if the list is empty. */
	int32 ResolveLeadChunkIndex() const;

	/** Step 3: clears bIsCutting and resets ShotTimer so the new shot's clear-viewing
	 *  countdown begins fresh. */
	void FinishCut();

	/** Schedules PerformCutSwap (after fade-out) and FinishCut (after fade-in). */
	FTimerHandle CutSwapTimerHandle;
	FTimerHandle CutFinishTimerHandle;

	/** Cached locally from the Aftermath phase RPC argument. The orbit tick reads this
	 *  instead of GameState->AftermathHotspot to avoid a replication race — the RPC is
	 *  reliable and ordered, but the GameState property may not have replicated yet
	 *  by the time the first orbit tick runs. Also serves as the fallback pivot when
	 *  no live debris is registered. */
	FVector CachedAftermathHotspot = FVector::ZeroVector;

	/** Smoothed live pivot the orbit camera actually targets. Each tick we VInterpTo
	 *  toward the live-debris centroid, so the camera lazily chases scattering chunks
	 *  instead of staring at the original break-time location. Seeded in
	 *  HandlePhase_Aftermath to avoid a snap from the world origin. */
	FVector AftermathOrbitPivot = FVector::ZeroVector;

	/** Per-client list of actors whose chaos has broken locally. Weak refs so destroyed
	 *  actors are skipped automatically. Cleared in EndPlay. */
	TArray<TWeakObjectPtr<AActor>> ActiveDebrisActors;

	/** Averages GetChaosTargetLocation across every valid entry in ActiveDebrisActors.
	 *  Falls back to CachedAftermathHotspot when the list is empty. */
	FVector ComputeLiveDebrisCentroid() const;

protected:
	// ── Blueprint Implementable Events ──────────────────────────────
	// C++ handles engine-level work (input mode, dilation).
	// These events let the Blueprint subclass wire up visuals: camera blends, fades, UI.

	/** Phase 1: Blueprint spawns the WBP_MeowTime widget + plays the audio stinger.
	 *  Fires per-client at the moment slow-mo engages, before the cinematic camera takeover. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Match")
	void OnMeowTimeTriggered();

	/** Phase 2: Blueprint spawns a camera actor, positions it to track TargetActor,
	 *  and calls SetViewTargetWithBlend. TargetLocation is the actor's location at break time. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Match")
	void OnCinematicTakeover(FVector TargetLocation, AActor* TargetActor);

	/** Phase 3: Blueprint unlocks mouse and adds the WBP_RaidScoreboard widget.
	 *  Fired AFTER the orbit camera has revealed the destruction (post fade-in + hold);
	 *  the widget is responsible for its own fade-in animation only. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Match")
	void OnShowScoreboard();
};
