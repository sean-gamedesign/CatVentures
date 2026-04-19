// CatPlayerController.cpp

#include "CatPlayerController.h"
#include "CatBase.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Blueprint/UserWidget.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollectionProxyData.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"

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
		HandlePhase_Aftermath();
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
	// Hand off to Blueprint — it plays a fade-to-black widget animation.
	OnMatchTransitionFade();

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Magenta,
			TEXT("Phase 2b: FADE — transition to scoreboard"));
	}
}

void ACatPlayerController::HandlePhase_Aftermath()
{
	// Restore normal time is already handled server-side (GameMode restores dilation to 1.0).
	// Hand off to Blueprint — it shows the scoreboard and cuts to the panning camera.
	OnShowScoreboard();

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green,
			TEXT("Phase 3: THE AFTERMATH — scoreboard active"));
	}
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
