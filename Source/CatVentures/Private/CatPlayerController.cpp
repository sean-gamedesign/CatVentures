// CatPlayerController.cpp

#include "CatPlayerController.h"
#include "CatBase.h"
#include "CatGameState.h"
#include "CatPlayerState.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "GeometryCollectionProxyData.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "TimerManager.h"

void ACatPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (UEnhancedInputComponent* EIC = CastChecked<UEnhancedInputComponent>(InputComponent))
	{
		EIC->BindAction(ToggleMenuAction, ETriggerEvent::Started, this, &ACatPlayerController::ToggleMenu);
	}
}

void ACatPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
}

void ACatPlayerController::AcknowledgePossession(APawn* P)
{
	Super::AcknowledgePossession(P);
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
}

void ACatPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Stop the Aftermath orbit cleanly on travel / disconnect.
	GetWorldTimerManager().ClearTimer(AftermathOrbitTimerHandle);
	GetWorldTimerManager().ClearTimer(ScoreboardRevealTimerHandle);
	GetWorldTimerManager().ClearTimer(CutSwapTimerHandle);
	GetWorldTimerManager().ClearTimer(CutFinishTimerHandle);

	// Drop weak refs to any debris collected during this match.
	ActiveDebrisActors.Empty();

	Super::EndPlay(EndPlayReason);
}

void ACatPlayerController::ToggleMenu()
{
	if (PauseMenuInstance && PauseMenuInstance->IsInViewport())
	{
		// Close menu — restore full game input
		PauseMenuInstance->RemoveFromParent();
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
	}
	else
	{
		// Open menu — lazy-construct on first use
		if (!PauseMenuInstance && PauseMenuClass)
		{
			PauseMenuInstance = CreateWidget<UUserWidget>(this, PauseMenuClass);
		}

		if (PauseMenuInstance)
		{
			PauseMenuInstance->AddToViewport();
			bShowMouseCursor = true;
			// UIOnly: blocks all pawn input while the menu is up.
			// Do NOT use SetPause(true) — it halts the server tick and breaks multiplayer.
			// SetWidgetToFocus ensures PauseMenuWidget receives Slate key events (e.g. Escape).
			FInputModeUIOnly InputMode;
			InputMode.SetWidgetToFocus(PauseMenuInstance->TakeWidget());
			InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			SetInputMode(InputMode);
		}
	}
}

// ── Match Phase Orchestration ───────────────────────────────────────

void ACatPlayerController::Client_OnMatchPhaseChanged_Implementation(ECatMatchPhase NewPhase, FVector PhaseLocation, AActor* TargetActor)
{
	switch (NewPhase)
	{
	case ECatMatchPhase::Warning:
		HandlePhase_Warning();
		break;

	case ECatMatchPhase::FinalCut:
		HandlePhase_FinalCut(PhaseLocation, TargetActor);
		break;

	case ECatMatchPhase::Fade:
		HandlePhase_Fade();
		break;

	case ECatMatchPhase::Aftermath:
		// PhaseLocation carries the AftermathHotspot for this phase (see GameMode::TransitionToAftermath).
		HandlePhase_Aftermath(PhaseLocation);
		break;

	default:
		break;
	}
}

void ACatPlayerController::HandlePhase_Warning()
{
	// Swap mapping contexts: strip movement, keep camera look.
	ACatBase* CatPawn = Cast<ACatBase>(GetPawn());
	if (!CatPawn) return;

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->RemoveMappingContext(CatPawn->DefaultMappingContext);

		if (LookOnlyMappingContext)
		{
			Subsystem->AddMappingContext(LookOnlyMappingContext, 0);
		}
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow,
			TEXT("Phase 1: THE WARNING — slow-mo active, movement stripped"));
	}

	// Hand off to Blueprint — spawn the Meow Time UMG and play the audio stinger.
	OnMeowTimeTriggered();
}

void ACatPlayerController::HandlePhase_FinalCut(FVector BreakLocation, AActor* TargetActor)
{
	// Hand off to Blueprint — it spawns the camera and calls SetViewTargetWithBlend.
	OnCinematicTakeover(BreakLocation, TargetActor);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan,
			FString::Printf(TEXT("Phase 2: THE FINAL CUT — camera target: %s"), *BreakLocation.ToString()));
	}
}

void ACatPlayerController::HandlePhase_Fade()
{
	// PlayerCameraManager owns the fade — bypasses the previous (broken) widget animation path.
	// Server resets dilation to 1.0 in TransitionToFade *before* this RPC fires, so we don't need
	// the old dilation-compensation factor here. World-settings replication may lag by one tick;
	// trade-off accepted (worst case is a single-frame visual stutter on the client).
	if (PlayerCameraManager)
	{
		PlayerCameraManager->StartCameraFade(
			0.0f, 1.0f,
			MatchFadeOutDuration,
			FLinearColor::Black,
			/*bShouldFadeAudio*/ false,
			/*bHoldWhenFinished*/ true);
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Magenta,
			TEXT("Phase 2b: FADE — fading to black via PlayerCameraManager"));
	}
}

void ACatPlayerController::HandlePhase_Aftermath(FVector Hotspot)
{
	// Server has already restored time dilation to 1.0 — straight wall-clock seconds from here.

	// Cache the RPC-supplied hotspot. The orbit tick reads this directly so we never depend
	// on GameState->AftermathHotspot replication catching up before the first tick fires.
	CachedAftermathHotspot = Hotspot;

	// Seed the smoothed live pivot to the RPC value so the first tick doesn't VInterpTo
	// from the world origin.
	AftermathOrbitPivot   = Hotspot;
	AftermathOrbitElapsed = 0.0f;

	// Director state: we're entering shot 0 of a fresh sequence. bIsCutting=true gates
	// ShotTimer until the entry fade-in completes, so the first prop gets a full
	// AftermathShotIntervalSec of CLEAR viewing — same contract as inter-shot cuts.
	ShotTimer  = 0.0f;
	bIsCutting = true;

	// Lead-chunk pick: scan registered debris for the prop whose chunks have flown the
	// furthest from the original break centroid, and open the sequence on THAT.
	CurrentTargetIndex = ResolveLeadChunkIndex();

	// Defense from the prior view-target debugging pass. Set once at sequence entry so
	// no state-change can re-snap the view target back to the controlled pawn.
	bAutoManageActiveCameraTarget = false;

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] CLIENT HandlePhase_Aftermath  RPC.Hotspot=(%.1f, %.1f, %.1f)  Cached=(%.1f, %.1f, %.1f)  ActiveDebris=%d  LeadIndex=%d"),
		Hotspot.X, Hotspot.Y, Hotspot.Z,
		CachedAftermathHotspot.X, CachedAftermathHotspot.Y, CachedAftermathHotspot.Z,
		ActiveDebrisActors.Num(), CurrentTargetIndex);

	// 1. Fade to black. Continue from whatever the current fade alpha is so we don't
	//    flash if the prior phase already brought us to black; if the screen WAS clear,
	//    this fades it down over AftermathFadeOutDuration.
	if (PlayerCameraManager)
	{
		const float CurrentFade = PlayerCameraManager->FadeAmount;
		PlayerCameraManager->StartCameraFade(
			CurrentFade, 1.0f,
			AftermathFadeOutDuration,
			FLinearColor::Black,
			/*bShouldFadeAudio*/ false,
			/*bHoldWhenFinished*/ true);
	}

	// 2. Schedule the camera spawn + view-target swap to happen UNDER black.
	GetWorldTimerManager().SetTimer(CutSwapTimerHandle, this,
		&ACatPlayerController::PerformAftermathEntrySwap,
		FMath::Max(AftermathFadeOutDuration, 0.001f), false);

	// 3. Begin the orbit tick at 60 Hz. The tick early-returns until AftermathCameraActor
	//    exists (which happens inside PerformAftermathEntrySwap), so it's safe to start
	//    the timer now and have the first few ticks no-op.
	GetWorldTimerManager().SetTimer(AftermathOrbitTimerHandle, this,
		&ACatPlayerController::TickAftermathOrbit, 1.0f / 60.0f, true);

	// 4. Schedule the scoreboard reveal: after the entry transition (fade-out + fade-in)
	//    plus AftermathHoldDuration of clear viewing on the lead-chunk shot.
	const float TotalEntryTransition = AftermathFadeOutDuration + AftermathFadeInDuration;
	GetWorldTimerManager().SetTimer(ScoreboardRevealTimerHandle, this,
		&ACatPlayerController::ShowScoreboardDeferred,
		TotalEntryTransition + AftermathHoldDuration, false);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green,
			FString::Printf(TEXT("Phase 3: AFTERMATH — entry fade, lead idx=%d"), CurrentTargetIndex));
	}
}

void ACatPlayerController::ShowScoreboardDeferred()
{
	// Restore mouse cursor + UI input mode for scoreboard interaction.
	bShowMouseCursor = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);

	OnShowScoreboard();
}

ACameraActor* ACatPlayerController::SpawnAftermathCamera(FVector Hotspot)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	// Initial pose — angle 0 of the orbit. TickAftermathOrbit will drive it from here.
	const FVector InitialOffset(AftermathOrbitRadius, 0.f, AftermathOrbitHeight);
	const FVector CamLocation = Hotspot + InitialOffset;
	const FRotator CamRotation = UKismetMathLibrary::FindLookAtRotation(CamLocation, Hotspot);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = this;

	AftermathCameraActor = World->SpawnActor<ACameraActor>(
		ACameraActor::StaticClass(), CamLocation, CamRotation, Params);

	if (AftermathCameraActor)
	{
		// Belt-and-braces: guarantee the camera lives in world space.
		AftermathCameraActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

		// CRITICAL: force the lens off pawn-control-rotation. If this flag is true, the
		// CameraComponent overrides whatever we Set on the actor with the controller's
		// control rotation every frame — making the orbit math invisible from the player's POV.
		if (UCameraComponent* Lens = AftermathCameraActor->GetCameraComponent())
		{
			Lens->bUsePawnControlRotation = false;
			Lens->SetRelativeRotation(FRotator::ZeroRotator);
		}

		// Force-teleport to the intended pose in case SpawnActor's initial transform was clobbered.
		AftermathCameraActor->SetActorLocationAndRotation(
			CamLocation, CamRotation, /*bSweep*/ false, /*OutSweepHitResult*/ nullptr,
			ETeleportType::TeleportPhysics);

		const FVector  ActualLoc = AftermathCameraActor->GetActorLocation();
		const FRotator ActualRot = AftermathCameraActor->GetActorRotation();
		UE_LOG(LogTemp, Warning,
			TEXT("[CatMatch] SpawnAftermathCamera  Hotspot=(%.1f, %.1f, %.1f)  Intended Loc=(%.1f, %.1f, %.1f) Rot=(P=%.1f, Y=%.1f, R=%.1f)  Actual Loc=(%.1f, %.1f, %.1f) Rot=(P=%.1f, Y=%.1f, R=%.1f)  Attached=%s"),
			Hotspot.X, Hotspot.Y, Hotspot.Z,
			CamLocation.X, CamLocation.Y, CamLocation.Z,
			CamRotation.Pitch, CamRotation.Yaw, CamRotation.Roll,
			ActualLoc.X, ActualLoc.Y, ActualLoc.Z,
			ActualRot.Pitch, ActualRot.Yaw, ActualRot.Roll,
			AftermathCameraActor->GetAttachParentActor() ? *AftermathCameraActor->GetAttachParentActor()->GetName() : TEXT("<none>"));

		// Layer 2: stop the PC from auto-snapping the view target back to the controlled pawn
		// during state changes (end-of-match in particular). Without this, our view target set
		// can be silently reverted before the next frame.
		bAutoManageActiveCameraTarget = false;

		// Instant cut — screen is black, no need to blend.
		SetViewTargetWithBlend(AftermathCameraActor, 0.f);

		// Layer 1: prove whether PCM accepted the view target right after we set it.
		const AActor* VT = PlayerCameraManager ? PlayerCameraManager->GetViewTarget() : nullptr;
		UE_LOG(LogTemp, Warning,
			TEXT("[CatMatch] PostSetView  VT=%s  Pawn=%s  Camera=%s"),
			VT       ? *VT->GetName()                          : TEXT("<null>"),
			GetPawn()? *GetPawn()->GetName()                   : TEXT("<null>"),
			AftermathCameraActor ? *AftermathCameraActor->GetName() : TEXT("<null>"));
	}

	return AftermathCameraActor;
}

void ACatPlayerController::TickAftermathOrbit()
{
	if (!AftermathCameraActor) return;

	const float DeltaTime = 1.0f / 60.0f;
	AftermathOrbitElapsed += DeltaTime;

	// ShotTimer only advances during clear-viewing time — paused while a fade-to-black
	// cut is in progress, so each prop gets a full AftermathShotIntervalSec of unobscured
	// screen time. Total wall-clock per cycle = ShotInterval + FadeOut + FadeIn.
	if (!bIsCutting)
	{
		ShotTimer += DeltaTime;
	}

	// Director cut: every AftermathShotIntervalSec we kick off a dip-to-black transition.
	if (!bIsCutting && ShotTimer >= AftermathShotIntervalSec && ActiveDebrisActors.Num() > 0)
	{
		bIsCutting = true;
		StartCutFadeOut();
	}

	// Resolve the current shot's target — the prop the camera is orbiting THIS shot.
	// Filtered Center of Mass: the average position of every chunk that survives the
	// abyss / snagged / platform-height filters. ActiveCount==0 means no hero pile,
	// so we hold on the broader carnage area until the cut timer rotates props.
	// VInterpTo smooths any transition. (Per-actor abyss is now a per-chunk filter
	// inside GetChaosActiveChunkCentroid; AftermathPivotMinZ remains the final-clamp
	// safety net on the smoothed orbit pivot.)
	AActor* CurrentTarget = ResolveCurrentTarget();
	int32   ActiveCount   = 0;
	FVector TargetPivot   = CurrentTarget
		? GetChaosActiveChunkCentroid(CurrentTarget, ActiveCount)
		: CachedAftermathHotspot;

	if (CurrentTarget && ActiveCount == 0)
	{
		TargetPivot = CachedAftermathHotspot;
	}

	AftermathOrbitPivot = FMath::VInterpTo(AftermathOrbitPivot, TargetPivot, DeltaTime, AftermathOrbitFollowSpeed);

	// First-tick diagnostic — confirms we seeded the pivot from the RPC value.
	if (AftermathOrbitElapsed <= DeltaTime + KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[CatMatch] TickAftermathOrbit FIRST TICK  Cached=(%.1f, %.1f, %.1f)  Target=(%.1f, %.1f, %.1f)  Pivot=(%.1f, %.1f, %.1f)  ActiveDebris=%d  CurrentIndex=%d"),
			CachedAftermathHotspot.X, CachedAftermathHotspot.Y, CachedAftermathHotspot.Z,
			TargetPivot.X, TargetPivot.Y, TargetPivot.Z,
			AftermathOrbitPivot.X, AftermathOrbitPivot.Y, AftermathOrbitPivot.Z,
			ActiveDebrisActors.Num(), CurrentTargetIndex);
	}

	if (bDrawAftermathDebug)
	{
		DrawDebugSphere(GetWorld(), AftermathOrbitPivot, 25.f, 12, FColor::Red, false, -1.f, 0, 2.f);
	}

	const float AngleRad = FMath::DegreesToRadians(AftermathPanRateDeg * AftermathOrbitElapsed);
	const FVector OrbitOffset(
		FMath::Cos(AngleRad) * AftermathOrbitRadius,
		FMath::Sin(AngleRad) * AftermathOrbitRadius,
		AftermathOrbitHeight);
	const FVector  CamLocation = AftermathOrbitPivot + OrbitOffset;
	const FRotator CamRotation = UKismetMathLibrary::FindLookAtRotation(CamLocation, AftermathOrbitPivot);

	// Teleport-physics + no sweep: bypass any encroachment / attachment-relative interpretation
	// that could silently swallow the move. We want absolute world-space placement every frame.
	AftermathCameraActor->SetActorLocationAndRotation(
		CamLocation, CamRotation, /*bSweep*/ false, /*OutSweepHitResult*/ nullptr,
		ETeleportType::TeleportPhysics);

	// Layer 3: keep-alive re-assertion. If anything reverts our view target (state change,
	// pawn re-possession, etc.), snap it back within one tick. Conditional, so it's a no-op
	// in the common case.
	if (PlayerCameraManager && PlayerCameraManager->GetViewTarget() != AftermathCameraActor)
	{
		SetViewTarget(AftermathCameraActor);
	}

	// Throttled drift check — once per second, log intended vs actual for location, rotation,
	// AND the current view target so we can confirm the keep-alive is winning.
	static int32 TickCounter = 0;
	if ((++TickCounter % 60) == 1)
	{
		const FVector  ActualLoc = AftermathCameraActor->GetActorLocation();
		const FRotator ActualRot = AftermathCameraActor->GetActorRotation();
		const AActor*  VT        = PlayerCameraManager ? PlayerCameraManager->GetViewTarget() : nullptr;
		const bool     bMatch    = (VT == AftermathCameraActor);
		UE_LOG(LogTemp, Warning,
			TEXT("[CatMatch] OrbitTick  Shot=%d/%d  ShotT=%.1f/%.1f  Pivot=(%.1f, %.1f, %.1f)  Target=(%.1f, %.1f, %.1f)  CamLoc=(%.1f, %.1f, %.1f) Rot=(P=%.1f, Y=%.1f, R=%.1f)  VT=%s  Match=%d"),
			CurrentTargetIndex, ActiveDebrisActors.Num(),
			ShotTimer, AftermathShotIntervalSec,
			AftermathOrbitPivot.X, AftermathOrbitPivot.Y, AftermathOrbitPivot.Z,
			TargetPivot.X, TargetPivot.Y, TargetPivot.Z,
			ActualLoc.X, ActualLoc.Y, ActualLoc.Z,
			ActualRot.Pitch, ActualRot.Yaw, ActualRot.Roll,
			VT ? *VT->GetName() : TEXT("<null>"), bMatch ? 1 : 0);
	}
}

// ── Scoreboard Population ───────────────────────────────────────────

void ACatPlayerController::PopulateScoreboard(UObject* WorldContextObject,
                                              UPanelWidget* Container,
                                              TSubclassOf<UUserWidget> RowClass)
{
	if (!Container || !RowClass || !WorldContextObject) return;

	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull)
		: nullptr;
	if (!World) return;

	const ACatGameState* GS = World->GetGameState<ACatGameState>();
	if (!GS) return;

	Container->ClearChildren();
	for (const FCatPlayerScore& Entry : GS->PlayerScores)
	{
		UUserWidget* Row = CreateWidget<UUserWidget>(World, RowClass);
		if (!Row) continue;

		// Bind by widget-name convention. WBP_ScoreRow exposes these two TextBlocks as variables.
		if (UTextBlock* NameText = Cast<UTextBlock>(Row->GetWidgetFromName(TEXT("TxtPlayerName"))))
		{
			NameText->SetText(FText::FromString(Entry.PlayerName));
		}
		if (UTextBlock* ScoreText = Cast<UTextBlock>(Row->GetWidgetFromName(TEXT("TxtScore"))))
		{
			ScoreText->SetText(FText::AsNumber(Entry.Score));
		}

		Container->AddChild(Row);
	}
}

// ── Rematch / Travel ────────────────────────────────────────────────

void ACatPlayerController::Server_SetRematchReady_Implementation(bool bReady)
{
	// Only honored during the Aftermath phase — prevents a malicious client toggling mid-game.
	if (const ACatGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACatGameState>() : nullptr)
	{
		if (GS->MatchPhase != ECatMatchPhase::Aftermath) return;
	}

	if (ACatPlayerState* PS = GetPlayerState<ACatPlayerState>())
	{
		PS->bWantsRematch = bReady;
		// Notify locally on the server too — host UI watches the same delegate.
		PS->OnRematchReadyChanged.Broadcast(bReady);
	}
}

void ACatPlayerController::Host_ServerTravel(const FString& URL)
{
	if (!HasAuthority() || !GetWorld()) return;
	GetWorld()->ServerTravel(URL);
}

// ── Live Debris Tracking ────────────────────────────────────────────

void ACatPlayerController::RegisterDebrisActor(AActor* DebrisActor)
{
	if (!DebrisActor) return;

	// Skip dupes — BPC_ChaosItem.Native_Shatter is gated by a Do Once on the BP side, but
	// belt-and-braces in case multiple GC components on the same actor each fire the event.
	for (const TWeakObjectPtr<AActor>& Existing : ActiveDebrisActors)
	{
		if (Existing.Get() == DebrisActor) return;
	}

	ActiveDebrisActors.Emplace(DebrisActor);

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] RegisterDebrisActor  Actor=%s  TotalActive=%d"),
		*DebrisActor->GetName(), ActiveDebrisActors.Num());
}

FVector ACatPlayerController::ComputeLiveDebrisCentroid() const
{
	// Two parallel sums:
	//   ActiveSum tracks per-actor centroids of *fast-moving* chunks only.
	//   AllSum    tracks per-actor centroids of every broken chunk (settled + flying).
	// We prefer ActiveSum so the camera focuses on the action; we fall back to AllSum
	// once everything settles, and finally to CachedAftermathHotspot if even that's empty.
	FVector ActiveSum = FVector::ZeroVector;  int32 ActiveCount = 0;
	FVector AllSum    = FVector::ZeroVector;  int32 AllCount    = 0;

	for (const TWeakObjectPtr<AActor>& Ptr : ActiveDebrisActors)
	{
		AActor* A = Ptr.Get();
		if (!A) continue;

		// Simple centroid (broken chunks regardless of velocity) — used for the abyss
		// filter and as the fall-back centroid when nothing's moving.
		const FVector SimpleCentroid = GetChaosTargetLocation(A);

		// Abyss filter — skip actors whose chunk centroid has fallen through the floor.
		if (SimpleCentroid.Z < AftermathDebrisFloorZ) continue;

		AllSum += SimpleCentroid;
		++AllCount;

		// Velocity-filtered pass: only chunks moving faster than AftermathChunkActiveSpeed.
		int32 ChunksActive = 0;
		const FVector ActiveCentroid = GetChaosActiveChunkCentroid(A, ChunksActive);
		if (ChunksActive > 0)
		{
			ActiveSum += ActiveCentroid;
			++ActiveCount;
		}
	}

	FVector Centroid;
	if (ActiveCount > 0)
	{
		Centroid = ActiveSum / static_cast<float>(ActiveCount);   // chase the action
	}
	else if (AllCount > 0)
	{
		Centroid = AllSum / static_cast<float>(AllCount);         // settle on the pile
	}
	else
	{
		return CachedAftermathHotspot;                            // nothing in scope
	}

	// Final clamp — never let the returned pivot dip below AftermathPivotMinZ.
	Centroid.Z = FMath::Max(Centroid.Z, AftermathPivotMinZ);
	return Centroid;
}

// ── Director-cut helpers ────────────────────────────────────────────

AActor* ACatPlayerController::ResolveCurrentTarget() const
{
	if (ActiveDebrisActors.Num() == 0) return nullptr;

	// Walk forward from CurrentTargetIndex, modulo size, returning the first valid
	// weak ref. Tolerates GC'd actors without skipping shots permanently.
	for (int32 Step = 0; Step < ActiveDebrisActors.Num(); ++Step)
	{
		const int32 Idx = (CurrentTargetIndex + Step) % ActiveDebrisActors.Num();
		if (AActor* A = ActiveDebrisActors[Idx].Get())
		{
			return A;
		}
	}
	return nullptr;
}

void ACatPlayerController::StartCutFadeOut()
{
	// Step 1 of a dip-to-black cut. Just kicks off the PCM fade and schedules the swap.
	if (PlayerCameraManager)
	{
		PlayerCameraManager->StartCameraFade(
			0.0f, 1.0f,
			AftermathFadeOutDuration,
			FLinearColor::Black,
			/*bShouldFadeAudio*/ false,
			/*bHoldWhenFinished*/ true);
	}

	GetWorldTimerManager().SetTimer(CutSwapTimerHandle, this,
		&ACatPlayerController::PerformCutSwap, FMath::Max(AftermathFadeOutDuration, 0.001f), false);

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] CutFadeOut  Idx=%d  ShotT=%.2f  FadeOut=%.2fs"),
		CurrentTargetIndex, ShotTimer, AftermathFadeOutDuration);
}

void ACatPlayerController::PerformCutSwap()
{
	// Director cut path: advance the index, then run the shared swap routine.
	if (ActiveDebrisActors.Num() > 0)
	{
		CurrentTargetIndex = (CurrentTargetIndex + 1) % ActiveDebrisActors.Num();
	}
	SwapToCurrentTargetCameraAndFadeIn();
}

void ACatPlayerController::PerformAftermathEntrySwap()
{
	// Aftermath entry path: index was already set to the lead-chunk pick in
	// HandlePhase_Aftermath, so we run the shared swap routine without advancing.
	SwapToCurrentTargetCameraAndFadeIn();
}

void ACatPlayerController::SwapToCurrentTargetCameraAndFadeIn()
{
	// Screen is fully black at this point (either from the prior phase fade or the
	// cut's own fade-out). Hard-cut to the current shot, destroy the previous camera
	// (no visual artifact under black), then start fading back in.
	AActor* NewTarget = ResolveCurrentTarget();
	int32   NewActiveCount = 0;
	FVector NewPivot = NewTarget
		? GetChaosActiveChunkCentroid(NewTarget, NewActiveCount)
		: CachedAftermathHotspot;

	// ── Director skip-ghost ─────────────────────────────────────────
	// Total filter wipeout (zero contributing chunks) means this prop has no hero pile
	// to look at. Walk forward through the registered debris until we find one with
	// a real pile. If every prop is a ghost, fall through with the carnage-area pivot.
	const int32 NumDebris = ActiveDebrisActors.Num();
	if (NumDebris > 1 && NewTarget && NewActiveCount == 0)
	{
		for (int32 Skip = 1; Skip < NumDebris; ++Skip)
		{
			const int32 CandidateIdx = (CurrentTargetIndex + Skip) % NumDebris;
			AActor* CandidateActor   = ActiveDebrisActors[CandidateIdx].Get();
			if (!CandidateActor) continue;

			int32 CandidateCount = 0;
			const FVector CandidatePivot = GetChaosActiveChunkCentroid(CandidateActor, CandidateCount);
			if (CandidateCount > 0)
			{
				CurrentTargetIndex = CandidateIdx;
				NewTarget          = CandidateActor;
				NewPivot           = CandidatePivot;
				NewActiveCount     = CandidateCount;
				UE_LOG(LogTemp, Warning,
					TEXT("[CatMatch] Director Skip-Ghost  Advanced to idx=%d  ActiveCount=%d  Pivot=(%.1f, %.1f, %.1f)"),
					CurrentTargetIndex, CandidateCount, NewPivot.X, NewPivot.Y, NewPivot.Z);
				break;
			}
		}
	}

	// All ghosts — explicit fallback to the carnage-area pivot before pre-spawn safety.
	if (NewActiveCount == 0)
	{
		NewPivot = CachedAftermathHotspot;
	}

	// ── Pre-spawn abyss safety ──────────────────────────────────────
	// Apply the same two-step Z defense the tick uses, but BEFORE we spawn the new
	// camera — otherwise a target whose chunks fell into the abyss spawns the
	// camera deep underground and the player sees it "fly up" as the tick clamps.
	if (NewPivot.Z < AftermathDebrisFloorZ)
	{
		const float OrigZ = NewPivot.Z;
		NewPivot = CachedAftermathHotspot;
		UE_LOG(LogTemp, Warning,
			TEXT("[CatMatch] Abyss Pivot Detected! Clamping Z from %f to %f (fallback to CachedAftermathHotspot)"),
			OrigZ, NewPivot.Z);
	}
	if (NewPivot.Z < AftermathPivotMinZ)
	{
		const float OrigZ = NewPivot.Z;
		NewPivot.Z = AftermathPivotMinZ;
		UE_LOG(LogTemp, Warning,
			TEXT("[CatMatch] Abyss Pivot Detected! Clamping Z from %f to %f"),
			OrigZ, NewPivot.Z);
	}

	UWorld* World = GetWorld();
	if (World)
	{
		const FVector  InitialOffset(AftermathOrbitRadius, 0.f, AftermathOrbitHeight);
		const FVector  CamLocation = NewPivot + InitialOffset;
		const FRotator CamRotation = UKismetMathLibrary::FindLookAtRotation(CamLocation, NewPivot);

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = this;

		ACameraActor* NewCamera = World->SpawnActor<ACameraActor>(
			ACameraActor::StaticClass(), CamLocation, CamRotation, Params);

		if (NewCamera)
		{
			NewCamera->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			if (UCameraComponent* Lens = NewCamera->GetCameraComponent())
			{
				Lens->bUsePawnControlRotation = false;
				Lens->SetRelativeRotation(FRotator::ZeroRotator);
			}

			ACameraActor* OldCamera = AftermathCameraActor;
			AftermathCameraActor = NewCamera;

			// Hard cut — we're under a black screen.
			SetViewTargetWithBlend(AftermathCameraActor, 0.0f);

			if (OldCamera && OldCamera != NewCamera)
			{
				OldCamera->Destroy();
			}

			AftermathOrbitElapsed = 0.0f;
			AftermathOrbitPivot   = NewPivot;
		}
	}

	// Start the fade back in.
	if (PlayerCameraManager)
	{
		PlayerCameraManager->StartCameraFade(
			1.0f, 0.0f,
			AftermathFadeInDuration,
			FLinearColor::Black,
			/*bShouldFadeAudio*/ false,
			/*bHoldWhenFinished*/ false);
	}

	GetWorldTimerManager().SetTimer(CutFinishTimerHandle, this,
		&ACatPlayerController::FinishCut, FMath::Max(AftermathFadeInDuration, 0.001f), false);

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] CutSwap  Idx=%d/%d  Target=%s  NewPivot=(%.1f, %.1f, %.1f)  FadeIn=%.2fs"),
		CurrentTargetIndex, ActiveDebrisActors.Num(),
		NewTarget ? *NewTarget->GetName() : TEXT("<null>"),
		NewPivot.X, NewPivot.Y, NewPivot.Z,
		AftermathFadeInDuration);
}

int32 ACatPlayerController::ResolveLeadChunkIndex() const
{
	// Pick the prop with the biggest hero pile — most contributing chunks past the
	// filters wins. Tiebreak by max-distance from the hotspot so when two props have
	// equally-sized piles, the one whose debris flew further opens the sequence.
	// Props with zero contributing chunks (pure ghosts) are ineligible.
	if (ActiveDebrisActors.Num() == 0) return 0;

	int32 BestIdx    = 0;
	int32 BestCount  = -1;
	float BestDistSq = -1.0f;

	for (int32 i = 0; i < ActiveDebrisActors.Num(); ++i)
	{
		AActor* A = ActiveDebrisActors[i].Get();
		if (!A) continue;

		int32 ActiveCount = 0;
		const FVector Pivot = GetChaosActiveChunkCentroid(A, ActiveCount);
		if (ActiveCount == 0) continue;

		const float DistSq = FVector::DistSquared(Pivot, CachedAftermathHotspot);
		if (ActiveCount > BestCount || (ActiveCount == BestCount && DistSq > BestDistSq))
		{
			BestIdx    = i;
			BestCount  = ActiveCount;
			BestDistSq = DistSq;
		}
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] LeadChunkIndex=%d  ActiveCount=%d  Dist=%.1f  ActiveDebris=%d"),
		BestIdx, BestCount, FMath::Sqrt(FMath::Max(BestDistSq, 0.f)), ActiveDebrisActors.Num());

	return BestIdx;
}

void ACatPlayerController::FinishCut()
{
	// Step 3: fade-in just completed; the new shot is fully visible. Reset the timer
	// so this prop gets the full AftermathShotIntervalSec of clear viewing.
	bIsCutting = false;
	ShotTimer  = 0.0f;
}

// ── Lead-chunk targeting helper ─────────────────────────────────────

FVector ACatPlayerController::GetChaosActiveChunkCentroid(AActor* TargetActor, int32& OutActiveCount) const
{
	// Filtered Center of Mass — averages every broken chunk that survives the abyss
	// filter, then gates on a minimum mass threshold so we never frame a single rogue
	// shard. OutActiveCount reports the contributing chunk count; zero means "skip
	// this prop" (either nothing in scope or below threshold).
	//
	//   Filter 1 (Abyss):  ChunkLoc.Z >= AftermathPivotMinZ
	//   Mass threshold:    PassCount >= AftermathMinChunkCount
	OutActiveCount = 0;
	if (!TargetActor) return FVector::ZeroVector;

	UGeometryCollectionComponent* GCComp = TargetActor->FindComponentByClass<UGeometryCollectionComponent>();
	if (!GCComp) return TargetActor->GetActorLocation();

	FGeometryDynamicCollection* DynCollection = GCComp->GetDynamicCollection();
	if (!DynCollection) return TargetActor->GetActorLocation();

	FGeometryCollectionDynamicStateFacade StateFacade(*DynCollection);
	const TArray<FTransform3f>& Transforms = GCComp->GetComponentSpaceTransforms3f();
	const FTransform CompToWorld = GCComp->GetComponentTransform();

	FVector Sum       = FVector::ZeroVector;
	int32   PassCount = 0;

	for (int32 i = 0; i < Transforms.Num(); ++i)
	{
		// Skip cluster parents and fragments still attached to the cluster.
		if (StateFacade.HasChildren(i))   continue;
		if (!StateFacade.HasBrokenOff(i)) continue;

		const FTransform WorldTransform = FTransform(Transforms[i]) * CompToWorld;
		const FVector ChunkLoc = WorldTransform.GetLocation();

		// Filter 1 — abyss: chunks below the pivot floor are dead to us.
		if (ChunkLoc.Z < AftermathPivotMinZ) continue;

		Sum += ChunkLoc;
		++PassCount;
	}

	// Mass threshold — too few fragments means the prop barely shattered (or most
	// flew into the abyss). Don't frame a single dust mote.
	if (PassCount < AftermathMinChunkCount)
	{
		OutActiveCount = 0;
		return TargetActor->GetActorLocation();
	}

	OutActiveCount = PassCount;
	return Sum / static_cast<float>(PassCount);
}

// ── Chaos Debris Centroid ───────────────────────────────────────────

FVector ACatPlayerController::GetChaosTargetLocation(AActor* TargetActor) const
{
	if (!TargetActor) return FVector::ZeroVector;

	UGeometryCollectionComponent* GCComp = TargetActor->FindComponentByClass<UGeometryCollectionComponent>();
	if (!GCComp) return TargetActor->GetActorLocation();

	FGeometryDynamicCollection* DynCollection = GCComp->GetDynamicCollection();
	if (!DynCollection) return TargetActor->GetActorLocation();

	FGeometryCollectionDynamicStateFacade StateFacade(*DynCollection);
	const TArray<FTransform3f>& Transforms = GCComp->GetComponentSpaceTransforms3f();
	const FTransform CompToWorld = GCComp->GetComponentTransform();

	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;

	for (int32 i = 0; i < Transforms.Num(); ++i)
	{
		// Skip cluster parents — they aren't visible leaf fragments.
		if (StateFacade.HasChildren(i)) continue;
		// Only count fragments that have actually separated from the cluster.
		if (!StateFacade.HasBrokenOff(i)) continue;

		const FTransform WorldTransform = FTransform(Transforms[i]) * CompToWorld;
		Sum += WorldTransform.GetLocation();
		++Count;
	}

	return (Count > 0) ? (Sum / Count) : TargetActor->GetActorLocation();
}

// ── Cinematic Tracker Camera Spawn ──────────────────────────────────

ACameraActor* ACatPlayerController::SpawnCinematicTrackerCamera(float BlendTime)
{
	APawn* MyPawn = GetPawn();
	if (!MyPawn) return nullptr;

	// Prefer an explicitly-active camera; fall back to the first one found.
	UCameraComponent* LensCamera = nullptr;
	TArray<UCameraComponent*> Cameras;
	MyPawn->GetComponents<UCameraComponent>(Cameras);
	for (UCameraComponent* Cam : Cameras)
	{
		if (Cam && Cam->IsActive()) { LensCamera = Cam; break; }
	}
	if (!LensCamera && Cameras.Num() > 0) LensCamera = Cameras[0];
	if (!LensCamera) return nullptr;

	const FTransform LensWorld = LensCamera->GetComponentTransform();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = this;

	ACameraActor* TrackerCam = GetWorld()->SpawnActor<ACameraActor>(
		ACameraActor::StaticClass(), LensWorld.GetLocation(), LensWorld.Rotator(), SpawnParams);

	if (!TrackerCam) return nullptr;

	// Copy lens settings for a seamless handoff — no FOV or post-process pop.
	if (UCameraComponent* DestCam = TrackerCam->GetCameraComponent())
	{
		DestCam->SetFieldOfView(LensCamera->FieldOfView);
		DestCam->SetAspectRatio(LensCamera->AspectRatio);
		DestCam->SetConstraintAspectRatio(LensCamera->bConstrainAspectRatio);
		DestCam->SetProjectionMode(LensCamera->ProjectionMode);
		DestCam->SetOrthoWidth(LensCamera->OrthoWidth);
		DestCam->PostProcessSettings = LensCamera->PostProcessSettings;
		DestCam->PostProcessBlendWeight = LensCamera->PostProcessBlendWeight;
	}

	SetViewTargetWithBlend(TrackerCam, BlendTime);
	return TrackerCam;
}
