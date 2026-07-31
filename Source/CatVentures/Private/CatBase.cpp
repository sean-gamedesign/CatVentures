// CatBase.cpp

#include "CatBase.h"
#include "CatVenturesLog.h"
#include "CatTraversalComponent.h"
#include "PawPrintSubsystem.h"
#include "CatAnimationTypes.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "InteractableInterface.h"
#include "Kismet/KismetSystemLibrary.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkinnedAsset.h"
#include "ReferenceSkeleton.h"

ACatBase::ACatBase()
{
	PrimaryActorTick.bCanEverTick = true;

	// ── Camera rig ─────────────────────────────────────────────
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// ── Physics Bumper ────────────────────────────────────────────
	PhysicsBumper = CreateDefaultSubobject<UBoxComponent>(TEXT("PhysicsBumper"));
	PhysicsBumper->SetupAttachment(RootComponent);
	PhysicsBumper->SetRelativeLocation(FVector(60.0f, 0.0f, 10.0f));
	PhysicsBumper->SetBoxExtent(FVector(15.0f, 28.0f, 22.0f));
	PhysicsBumper->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PhysicsBumper->SetCollisionResponseToAllChannels(ECR_Ignore);
	PhysicsBumper->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Overlap);
	PhysicsBumper->SetCollisionResponseToChannel(ECC_Destructible, ECR_Overlap);
	PhysicsBumper->SetGenerateOverlapEvents(true);

	// ── Mouth Grab ────────────────────────────────────────────────
	// GrabConstraint is created dynamically in Server_Grab_Implementation — not a CDO subobject.

	GrabTargetLocation = CreateDefaultSubobject<USceneComponent>(TEXT("GrabTargetLocation"));

	Traversal = CreateDefaultSubobject<UCatTraversalComponent>(TEXT("Traversal"));
	GrabTargetLocation->SetupAttachment(GetMesh(), TEXT("socket_mouth"));
	// Push the hold point 80 cm forward in mouth-socket space so the held object sits
	// in front of the cat's capsule rather than pressing against it.
	GrabTargetLocation->SetRelativeLocation(FVector(80.0f, 0.0f, 0.0f));

	// ── Rotation settings ────────────────────────────────────────
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw   = false;
	bUseControllerRotationRoll  = false;

	// ── Spring arm collision ──────────────────────────────────────
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->ProbeSize = 12.0f;
	CameraBoom->ProbeChannel = ECC_Camera;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 10.0f;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraRotationLagSpeed = 8.0f;

	// Free-roaming 3rd-person: orient to movement, platforming air control
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->bOrientRotationToMovement = true;

		// Movement Tuning — designer-exposed UPROPERTYs for momentum feel
		CMC->MaxWalkSpeed                = MovementMaxWalkSpeed;
		CMC->MaxAcceleration             = MovementAcceleration;
		CMC->BrakingDecelerationWalking  = MovementBrakingDeceleration;
		CMC->GroundFriction              = MovementGroundFriction;
		CMC->BrakingFriction             = MovementBrakingFriction;
		CMC->RotationRate                = FRotator(0.0f, MovementRotationRateYaw, 0.0f);

		CMC->GravityScale                = GravityScaleRising;
		CMC->JumpZVelocity               = JumpLaunchVelocity;
		CMC->AirControl                  = JumpAirControl;
		CMC->FallingLateralFriction      = MovementFallingLateralFriction;
	}

	JumpMaxHoldTime = JumpMaxHoldTimeTuning;
}

void ACatBase::BeginPlay()
{
	Super::BeginPlay();

	PhysicsBumper->OnComponentBeginOverlap.AddDynamic(this, &ACatBase::OnBumperOverlapBegin);

	// Register the default mapping context for the local player only.
	if (const APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}

	// Apply camera tuning UPROPERTYs to the SpringArm
	CameraBoom->bEnableCameraLag         = bEnableCameraLag;
	CameraBoom->CameraLagSpeed           = CameraLagSpeed;
	CameraBoom->bEnableCameraRotationLag = bEnableCameraRotationLag;
	CameraBoom->CameraRotationLagSpeed   = CameraRotationLagSpeed;

	// Camera weight (M3): remember the authored socket offset + FOV — the dip spring
	// and the sprint FOV push offset from these.
	CamBaseSocketOffsetZ = CameraBoom->SocketOffset.Z;
	if (FollowCamera)
	{
		CameraBaseFOV = FollowCamera->FieldOfView;
	}

	// Landing cushion: remember the mesh's authored relative Z — the spring offsets from it.
	if (const USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshCushionBaseZ = MeshComp->GetRelativeLocation().Z;
		MeshBaseRelLocX  = MeshComp->GetRelativeLocation().X;  // wall-hug slides forward from this
		MeshBaseRelRot   = MeshComp->GetRelativeRotation();   // the −90° rig yaw; slope pitch composes onto it
	}

	// Apply movement + jump tuning UPROPERTYs to the CMC. The constructor bakes the
	// C++ defaults into the CMC subobject BEFORE Blueprint property serialization, so
	// per-instance overrides on PrimeCatBase only take effect if re-applied here.
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxWalkSpeed               = MovementMaxWalkSpeed;
		CMC->MaxAcceleration            = MovementAcceleration;
		CMC->BrakingDecelerationWalking = MovementBrakingDeceleration;
		CMC->GroundFriction             = MovementGroundFriction;
		CMC->BrakingFriction            = MovementBrakingFriction;
		CMC->RotationRate               = FRotator(0.0f, MovementRotationRateYaw, 0.0f);

		CMC->JumpZVelocity  = JumpLaunchVelocity;
		CMC->AirControl     = JumpAirControl;
		CMC->GravityScale   = GravityScaleRising;
		CMC->FallingLateralFriction = MovementFallingLateralFriction;
	}
	GravityScaleInterp = GravityScaleRising;
	JumpMaxHoldTime = JumpMaxHoldTimeTuning;

	// PawPrint telemetry sink — null outside game/PIE worlds (subsystem opts out there).
	PawPrint = GetWorld() ? GetWorld()->GetSubsystem<UPawPrintSubsystem>() : nullptr;
}

void ACatBase::OnBumperOverlapBegin(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep,
    const FHitResult& SweepResult)
{
	if (!OtherActor || !OtherComp) return;

	// Stage 1 — CMC floor check (primary, rotation-agnostic).
	// If the cat is grounded on this exact component, suppress the interaction.
	if (GetCharacterMovement()->CurrentFloor.HitResult.GetComponent() == OtherComp) return;

	// Stage 2 — Z-bounds fallback (airborne case).
	// When airborne, CurrentFloor is stale. Suppress if the object's AABB top
	// is at or below the cat's feet — it's directly underneath, not beside.
	const float FeetZ   = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float ObjTopZ = OtherComp->Bounds.GetBox().Max.Z;
	if (ObjTopZ <= FeetZ + UnderFootTolerance) return;

	// Use the bumper's actual world position as the damage/impulse origin,
	// not the actor root — the root sits 60 cm behind the bumper face.
	const FVector BumperOrigin = PhysicsBumper->GetComponentLocation();

	// Path A — rigid body push impulse. Server-authoritative; physics replicates normally.
	if (HasAuthority() && OtherComp->IsSimulatingPhysics())
	{
		FVector Vel = GetVelocity();
		Vel.Z = 0.0f;
		const FVector ImpulseDir = Vel.SizeSquared() > 1.0f ? Vel.GetSafeNormal() : GetActorForwardVector();
		OtherComp->AddImpulse(ImpulseDir * BumperPushForce, NAME_None, /*bVelChange=*/false);
	}

	// Path B — GC fracture via RPC pattern.
	//
	// ApplyExternalStrain modifies the local Chaos physics solver — every peer must call
	// it independently for deterministic simultaneous fracture. Only the cat's owning
	// machine reports the hit (IsLocallyControlled gate), preventing the server copy of a
	// remote pawn from racing the client's Server RPC and double-triggering the multicast.
	if (Cast<UGeometryCollectionComponent>(OtherComp))
	{
		if (!IsLocallyControlled()) return;

		if (HasAuthority())
		{
			// Listen server host's own cat — authority, call multicast directly.
			Multicast_BumperHitGC(OtherActor, BumperOrigin);
		}
		else
		{
			// Client's own cat — send to server for validation, server then multicasts.
			Server_BumperHitGC(OtherActor, BumperOrigin);
		}
	}
}

void ACatBase::Server_BumperHitGC_Implementation(AActor* GCActor, FVector Origin)
{
	if (!GCActor) return;

	// Range check using the server's authoritative pawn position.
	// 300 cm = bumper reach (60) + shatter radius slack + prediction jitter buffer.
	constexpr float MaxReachCm = 300.0f;
	if (FVector::Dist(GetActorLocation(), GCActor->GetActorLocation()) > MaxReachCm) return;

	Multicast_BumperHitGC(GCActor, Origin);
}

void ACatBase::Multicast_BumperHitGC_Implementation(AActor* GCActor, FVector Origin)
{
	if (!GCActor) return;
	UGeometryCollectionComponent* GCC = GCActor->FindComponentByClass<UGeometryCollectionComponent>();
	if (!GCC) return;

	ForceShatterGC(GCC, Origin);
}

void ACatBase::ForceShatterGC(UGeometryCollectionComponent* GCC, FVector HitLocation)
{
	if (!GCC) return;

	// Massive radius + strain guarantees every cluster bond in the GC shatters
	// regardless of the asset's Damage Threshold setting.
	constexpr float ShatterRadius = 500.0f;
	constexpr float ShatterStrain = 500000000.0f;

	GCC->ApplyKinematicField(ShatterRadius, HitLocation);
	GCC->ApplyExternalStrain(
		/*ItemIndex=*/         0,
		/*Location=*/          HitLocation,
		/*Radius=*/            ShatterRadius,
		/*PropagationDepth=*/  5,
		/*PropagationFactor=*/ 1.0f,
		/*Strain=*/            ShatterStrain
	);
}

void ACatBase::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	DeltaTimeCached = DeltaTime;

	// ── State: runs on ALL roles (server, autonomous, simulated) ──
	UpdateAnimationStates();

	// ── Jump gravity: authority + autonomous proxy only ────────────────
	UpdateJumpGravity();

	// ── Mouth Grab: authority only — moves the physics handle target ──
	if (HasAuthority())
	{
		UpdateGrab(DeltaTime);
	}

	// Turn-in-place is now driven by root-motion turn montages (see TryTurnInPlace),
	// not a procedural rotation commitment — the montage's root motion rotates the actor
	// with real footwork. (The old CommitTurn RInterpTo block was removed.)

	// ── Cosmetic: skip on dedicated server (no visuals) ───────────
	if (GetNetMode() != NM_DedicatedServer)
	{
		UpdateCosmeticInterpolation(DeltaTime);
		UpdateLandCushion(DeltaTime);   // before foot IK: the IK traces must see the dipped paws
		UpdateFootIK(DeltaTime);
	}

	// ── Post-mantle anim-hold failsafe ─────────────────────────────────
	// Landed() is the normal clear; this covers a completed mantle whose landing never
	// comes (slid off the lip) — after the window the SM falls back through Mantle →
	// Jump_Fall, which is the correct pose for that outcome anyway. Only ever armed on
	// the machines that drive the mantle (owner + server); proxies read the replicas.
	if (MantleAnimHoldTimer > 0.0f)
	{
		MantleAnimHoldTimer -= DeltaTime;
		if (MantleAnimHoldTimer <= 0.0f && bGoMantle)
		{
			// Freeze the scrub where it is (1.0) — the Mantle state's blend-out still
			// evaluates the clip, and a reset-to-0 snaps the pose back to the hang.
			SetMantleAnimState(false, MantleProgress);
		}
	}

	// ── PawPrint telemetry — the locally controlled cat's channel set ──
	// Every pose-driving scalar the diagnostic sessions kept hand-sampling.
	if (PawPrint && IsLocallyControlled() && GetNetMode() != NM_DedicatedServer)
	{
		static const FName ChSpeed(TEXT("Speed")),           ChSpeedType(TEXT("SpeedType")),
		                   ChJumpPhase(TEXT("JumpPhase")),   ChLean(TEXT("LeanAmount")),
		                   ChAccelLean(TEXT("AccelLean")),   ChCushionZ(TEXT("CushionZ")),
		                   ChCamDipZ(TEXT("CamDipZ")),       ChTurnRate(TEXT("TurnRateAnim")),
		                   ChStopping(TEXT("bStopping")),    ChCoiling(TEXT("bCoiling")),
		                   ChBursting(TEXT("bBursting")),    ChPivoting(TEXT("bPivoting")),
		                   ChSprinting(TEXT("bSprinting")), ChTurnInPlace(TEXT("bTurnInPlace")),
		                   ChTurnDropZ(TEXT("TurnDropZ")),  ChSkidProgress(TEXT("SkidProgress")),
		                   ChStartProgress(TEXT("StartProgress"));
		PawPrint->SampleChannel(ChSpeed,     Speed);
		PawPrint->SampleChannel(ChSpeedType, static_cast<float>(SpeedType));
		PawPrint->SampleChannel(ChJumpPhase, static_cast<float>(JumpPhase));
		PawPrint->SampleChannel(ChLean,      LeanAmount);
		PawPrint->SampleChannel(ChAccelLean, AccelLeanAmount);
		PawPrint->SampleChannel(ChCushionZ,  MeshCushionOffset);
		PawPrint->SampleChannel(ChCamDipZ,   CamDipOffset);
		PawPrint->SampleChannel(ChTurnRate,  TurnRateAnim);
		PawPrint->SampleChannel(ChStopping,  bIsStopping ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChCoiling,   bIsStartCoiling ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChBursting,  bIsStartBursting ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChPivoting,  bIsPivoting ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChSprinting, bIsSprinting ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChTurnInPlace, bIsTurningInPlace ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChTurnDropZ, TurnStanceDropZ);
		PawPrint->SampleChannel(ChSkidProgress, bGoSkid ? SkidProgress : 0.0f);
		PawPrint->SampleChannel(ChStartProgress, bGoStartStep ? StartProgress : 0.0f);
		static const FName ChMantleProgress(TEXT("MantleProgress"));
		PawPrint->SampleChannel(ChMantleProgress, bGoMantle ? MantleProgress : 0.0f);
		static const FName ChWallHug(TEXT("WallHugSlide")), ChAnimJumpPhase(TEXT("AnimJumpPhase"));
		PawPrint->SampleChannel(ChWallHug, WallHugForward);
		PawPrint->SampleChannel(ChAnimJumpPhase, static_cast<float>(AnimJumpPhase));

		// Live paw-contact gap while mantling: min distance from any paw to the nearest
		// surface (short down + forward traces per paw). This is the number Sean's eye
		// measures when he says "paws aren't connected" — the offline contact sweep can
		// only check the canonical composition, not a specific in-game mantle (2026-07-31).
		if (bGoMantle)
		{
			static const FName ChMantlePawGap(TEXT("MantlePawGapMin"));
			static const FName PawBones[4] = { FName(TEXT("Hand_L")), FName(TEXT("Hand_R")),
			                                   FName(TEXT("Foot_L")), FName(TEXT("Foot_R")) };
			float MinGap = 50.0f;
			if (const USkeletalMeshComponent* MeshComp = GetMesh())
			{
				FCollisionQueryParams GapParams(FName(TEXT("CatMantlePawGap")), false, this);
				const FVector Fwd = GetActorForwardVector();
				for (const FName& Bone : PawBones)
				{
					const FVector P = MeshComp->GetBoneLocation(Bone);
					FHitResult GapHit;
					if (GetWorld()->LineTraceSingleByChannel(GapHit, P, P - FVector(0, 0, 25.0f), ECC_Visibility, GapParams))
					{
						MinGap = FMath::Min(MinGap, GapHit.Distance);
					}
					if (GetWorld()->LineTraceSingleByChannel(GapHit, P, P + Fwd * 25.0f, ECC_Visibility, GapParams))
					{
						MinGap = FMath::Min(MinGap, GapHit.Distance);
					}
				}
			}
			PawPrint->SampleChannel(ChMantlePawGap, MinGap);
		}
	}

	// ── Camera weight (M3) + pitch clamping (local player only) ────────
	if (IsLocallyControlled())
	{
		UpdateCameraWeight(DeltaTime);

		if (APlayerController* PC = Cast<APlayerController>(Controller))
		{
			FRotator ControlRot = PC->GetControlRotation();
			ControlRot.Pitch = FMath::ClampAngle(ControlRot.Pitch, -PitchClampDown, PitchClampUp);
			PC->SetControlRotation(ControlRot);
		}
	}
}

void ACatBase::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Movement (tank-style) — fires every frame while the key is held
		EnhancedInput->BindAction(MoveAction,   ETriggerEvent::Triggered, this, &ACatBase::Move);

		// Look (mouse/stick) — fires every frame while input is non-zero
		EnhancedInput->BindAction(LookAction,   ETriggerEvent::Triggered, this, &ACatBase::Look);

		// Jump — Started records the buffer + jumps; Completed for variable-height release
		EnhancedInput->BindAction(JumpAction,   ETriggerEvent::Started,   this, &ACatBase::OnJumpInputPressed);
		EnhancedInput->BindAction(JumpAction,   ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Meow
		EnhancedInput->BindAction(MeowAction,   ETriggerEvent::Started,   this, &ACatBase::Server_Meow);

		// Swat
		EnhancedInput->BindAction(SwatAction,   ETriggerEvent::Started,   this, &ACatBase::TriggerSwat);

		// Interact
		EnhancedInput->BindAction(InteractAction, ETriggerEvent::Started, this, &ACatBase::TriggerInteract);

		// Mouth Grab — Started = bite, Completed = release
		EnhancedInput->BindAction(GrabAction, ETriggerEvent::Started,   this, &ACatBase::TriggerGrab);
		EnhancedInput->BindAction(GrabAction, ETriggerEvent::Completed, this, &ACatBase::TriggerRelease);

		EnhancedInput->BindAction(SprintAction, ETriggerEvent::Started,   this, &ACatBase::OnSprintPressed);
		EnhancedInput->BindAction(SprintAction, ETriggerEvent::Completed, this, &ACatBase::OnSprintReleased);
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Input Handlers ──────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::Move(const FInputActionValue& Value)
{
	const FVector2D MoveInput = Value.Get<FVector2D>();

	if (Controller)
	{
		// Camera-relative directions (yaw only — no pitch influence)
		const FRotator YawRotation(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection   = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// Cache the world-space steering direction for the moving pivot. Input events
		// fire before Tick, so UpdateMovingPivot always sees this frame's steer.
		const FVector WorldInput = ForwardDirection * MoveInput.Y + RightDirection * MoveInput.X;
		if (WorldInput.SizeSquared() > KINDA_SMALL_NUMBER)
		{
			PivotLiveInputDir   = WorldInput.GetSafeNormal();
			PivotInputStaleTime = 0.0f;
		}

		// During a pivot the plant owns locomotion: input keeps steering the pivot
		// target (cache above) but must not feed the CMC — zero acceleration is what
		// lets friction brake the cat and keeps orient-to-movement inert under the
		// pivot's SetActorRotation.
		if (bIsPivoting)
		{
			return;
		}

		// During the start coil the body loads before it launches — same suppression
		// contract as the pivot: the direction cache above keeps steering live.
		if (bIsStartCoiling)
		{
			return;
		}

		// During a traversal takeover the component owns the capsule; CMC input
		// would fight the curve (Flying mode + AddMovementInput = drift). The wall
		// attach zeroes horizontal velocity every frame anyway, but suppressing here
		// stops air control fighting it — and the direction cache above stays live,
		// which is what TryDetectMantle reads as its heading while the cat clings
		// (horizontal velocity is zero there, so input is the only heading source —
		// this is what lets a cling turn into a mantle when a lip enters band).
		if (Traversal && (Traversal->IsMantling() || Traversal->IsWallAttached()))
		{
			return;
		}

		// Input envelopes — both ease CMC acceleration in by scaling input magnitude.
		// Post-plant re-acceleration ramp: right after a weighty stop's plant, a fresh
		// press scales 0→1 over the remaining window instead of instantly cancelling
		// the halt. M4 fresh-input ramp: a plain start from idle eases in over
		// StartInputRampTime so a keyboard tap doesn't twitch. Overlap takes the
		// stricter (smaller) scale.
		float InputScale = 1.0f;
		if (StopReaccelTimer > 0.0f && StopReaccelDuration > KINDA_SMALL_NUMBER)
		{
			InputScale = 1.0f - (StopReaccelTimer / StopReaccelDuration);
		}
		if (InputRampTimer > 0.0f && StartInputRampTime > KINDA_SMALL_NUMBER)
		{
			InputScale = FMath::Min(InputScale, 1.0f - (InputRampTimer / StartInputRampTime));
		}

		// Wall-bounce rebound window: reaching a wall REQUIRES holding input into it, and
		// that same held input then accelerates the cat straight back at the wall it just
		// kicked off — measured at ~-70 cm/s over the first 0.2 s, against a rebound that
		// is itself decaying. Cancel only the INTO-THE-WALL component for the window, so
		// steering along the wall or away from it still works (a blanket input lock would
		// fix the rebound by removing the air control Sean is asking for more of).
		FVector AppliedInput = WorldInput * InputScale;
		if (Traversal && Traversal->IsRebounding())
		{
			const FVector Away = Traversal->GetReboundDirection();
			const float IntoWall = FVector::DotProduct(AppliedInput, -Away);
			if (IntoWall > 0.0f)
			{
				AppliedInput += Away * IntoWall;
			}
		}
		// Single accumulate — APawn::AddMovementInput sums Direction*Scale into
		// ControlInputVector, so this is identical to the two per-axis calls it replaces.
		AddMovementInput(AppliedInput, 1.0f);
	}
}

void ACatBase::Look(const FInputActionValue& Value)
{
	const FVector2D LookInput = Value.Get<FVector2D>();

	if (Controller)
	{
		AddControllerYawInput(LookInput.X * LookSensitivity);
		AddControllerPitchInput(LookInput.Y * LookSensitivity);
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Networked Meow ──────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::Server_Meow_Implementation()
{
	NetMulticast_Meow();
}

void ACatBase::NetMulticast_Meow_Implementation()
{
	OnMeow.Broadcast();
}

// ══════════════════════════════════════════════════════════════════════════
// ── The Swat ────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::TriggerSwat()
{
	if (bIsSwatting) return;

	// Local prediction: play montage immediately
	PlaySwatMontageAndBindEnd();

	// Tell the server
	Server_Swat();
}

void ACatBase::Server_Swat_Implementation()
{
	// Deliberately NO bIsSwatting guard here: the server's montage ends slightly
	// later than the client's (RPC latency), so a guard would eat legitimate
	// rapid re-swats at the montage boundary. Worst case without it is a montage
	// restart — cosmetic, and the trace window is bounded by the montage anyway.

	// Multicast to all *other* machines (the instigator already predicted)
	Multicast_Swat();
}

void ACatBase::Multicast_Swat_Implementation()
{
	// Skip on the instigator — they already started the montage locally
	if (IsLocallyControlled()) return;

	PlaySwatMontageAndBindEnd();
}

void ACatBase::PlaySwatMontageAndBindEnd()
{
	if (!SwatMontage) return;

	bIsSwatting = true;

	if (UAnimInstance* AnimInst = GetMesh()->GetAnimInstance())
	{
		AnimInst->Montage_Play(SwatMontage);

		FOnMontageEnded EndDelegate;
		EndDelegate.BindUObject(this, &ACatBase::OnSwatMontageEnded);
		AnimInst->Montage_SetEndDelegate(EndDelegate, SwatMontage);
	}
}

void ACatBase::OnSwatMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
	bIsSwatting = false;
}

// ══════════════════════════════════════════════════════════════════════════
// ── Moving Pivot (M2 part 2, weighty-movement pass) ───────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// A hard sustained steer at speed (input direction far off the velocity direction —
// the saturated-lean edge case from the M2 part 1 pass) plants the cat and turns it
// with the BS1_Cat_Turn footwork instead of letting the orient-to-movement arc swing
// the body through. While planted: Move() input is suppressed (friction + boosted
// braking kill the speed), the body rotates toward the live input direction at a
// capped rate (orient-to-movement is inert at zero acceleration, so SetActorRotation
// is uncontested — same mechanism as turn-in-place), and the landing-cushion spring
// takes a plant kick. Footwork + yaw ride the existing turn RPC infrastructure via
// the shared block in UpdateTurnInPlace; the camera stop-settle fires by itself as
// the speed collapses through the stopped threshold.

void ACatBase::UpdateMovingPivot()
{
	const float DeltaTime = DeltaTimeCached;
	PivotInputStaleTime += DeltaTime;
	PivotCooldownTimer   = FMath::Max(PivotCooldownTimer - DeltaTime, 0.0f);
	PivotTurnRateTarget  = 0.0f;

	const bool bInputFresh = PivotInputStaleTime < 0.15f;

	if (!bIsPivoting)
	{
		// ── Arm/detect ────────────────────────────────────────────────
		FVector Vel = GetVelocity();
		Vel.Z = 0.0f;
		FVector InputAccel = GetCharacterMovement()
			? GetCharacterMovement()->GetCurrentAcceleration() : FVector::ZeroVector;
		InputAccel.Z = 0.0f;

		float SteerAngle = 0.0f;
		if (Vel.SizeSquared() >= 100.0f && InputAccel.SizeSquared() >= 1.0f)
		{
			const float Dot = FVector::DotProduct(Vel.GetSafeNormal(), InputAccel.GetSafeNormal());
			SteerAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.0f, 1.0f)));
		}

		// Sweep accumulator: how far the input DIRECTION has rotated while the sustain
		// window runs. A camera sweep keeps rotating the input (that's what holds the
		// steer angle up against the velocity chase); a direction tap flips once and
		// then holds constant. This is the sweep-tier discriminator.
		const float InputYaw = (InputAccel.SizeSquared() >= 1.0f)
			? InputAccel.Rotation().Yaw : PivotPrevInputYaw;
		const float InputYawDelta = FMath::Abs(FRotator::NormalizeAxis(InputYaw - PivotPrevInputYaw));
		PivotPrevInputYaw = InputYaw;
		if (PivotSustainTimer > 0.0f)
		{
			PivotSweepAccumDeg += InputYawDelta;
		}

		// Speed gates: full PivotMinSpeed only to START the sustain window — the hard
		// steer itself brakes the cat, so re-checking it every frame voided the trigger
		// right as the angle peaked (2026-07-18 diag round). Once armed, only a
		// near-stop cancels.
		constexpr float PivotKeepArmedMinSpeed = 100.0f;   // cm/s
		const float RequiredSpeed = (PivotSustainTimer > 0.0f) ? PivotKeepArmedMinSpeed : PivotMinSpeed;

		const bool bEligible = bEnableMovingPivot
			&& PivotCooldownTimer <= 0.0f
			&& !bIsGrabbing
			&& MovementStage == ECatMovementStage::OnGround
			&& JumpPhase == ECatJumpPhase::None
			&& Speed >= RequiredSpeed
			&& bInputFresh;
		if (!bEligible || SteerAngle < PivotAngleThreshold)
		{
			PivotSustainTimer   = 0.0f;
			PivotSweepAccumDeg  = 0.0f;
			return;
		}

		// Sustain filter: a corrective flick crosses the threshold for a frame or two;
		// a deliberate hard steer holds it.
		PivotSustainTimer += DeltaTime;
		if (PivotSustainTimer < PivotSustainTime)
		{
			return;
		}

		// Two-tier fire: a hard reversal (flip tier) is unambiguous; the lower sweep
		// tier also demands the input DIRECTION rotated during the window, so ordinary
		// direction taps — which cross the same angle band with a constant input —
		// keep waiting here until their angle decays and the window resets.
		if (SteerAngle < PivotFlipAngle && PivotSweepAccumDeg < PivotSweepMinDeg)
		{
			return;
		}

		EnterPivot();
	}

	// ── Run the plant-and-turn ────────────────────────────────────────
	if (bIsPivoting)
	{
		// Abort: anything that invalidates a grounded plant, or input released.
		if (!bEnableMovingPivot || bIsGrabbing || !bInputFresh
			|| MovementStage != ECatMovementStage::OnGround
			|| JumpPhase != ECatJumpPhase::None)
		{
			ExitPivot();
			return;
		}

		const float DesiredYaw = PivotLiveInputDir.Rotation().Yaw;
		const float CurrentYaw = GetActorRotation().Yaw;
		if (FMath::Abs(FRotator::NormalizeAxis(DesiredYaw - CurrentYaw)) <= PivotExitAngle)
		{
			ExitPivot();
			return;
		}

		// Same capped-rate shortest-path rotation as turn-in-place; the input direction
		// is live, so a still-sweeping camera keeps the pivot chasing it.
		const float NewYaw       = FMath::FixedTurn(CurrentYaw, DesiredYaw, PivotTurnSpeedDegPerSec * DeltaTime);
		const float AppliedDelta = FRotator::NormalizeAxis(NewYaw - CurrentYaw);
		SetActorRotation(FRotator(0.0f, NewYaw, 0.0f));

		// Scaled by PivotFootworkCap: full-rate pivots sit in the 45-step blend zone
		// instead of pinning the stride-elevated Move_Turn-90 rim clips (see header).
		const float AppliedRate = (DeltaTime > KINDA_SMALL_NUMBER) ? AppliedDelta / DeltaTime : 0.0f;
		PivotTurnRateTarget = FMath::Clamp(AppliedRate / PivotTurnSpeedDegPerSec, -1.0f, 1.0f) * PivotFootworkCap;

		// Clip scrub: accumulated APPLIED rotation over a drift-growing denominator.
		// The remaining-vs-live-target form stalled during camera sweeps (2026-07-22
		// PawPrint round: the target drifts as fast as the body chases it → progress
		// froze at ~0.2 holding a mid-swing paw in the air, then played the back 80%
		// at ~3× when the camera stopped). The numerator grows at the real turn rate,
		// so the scrub cannot stall; a drifting target grows the denominator instead,
		// pacing the footwork over the whole sweep. Exit lands at exactly 1: when
		// RemainingDeg == PivotExitAngle the denominator equals the numerator.
		// Monotonic clamp kept — a target drifting faster than the turn rate briefly
		// grows the denominator faster than the numerator.
		PivotAccumDeg += FMath::Abs(AppliedDelta);
		const float RemainingDeg = FMath::Abs(FRotator::NormalizeAxis(DesiredYaw - NewYaw));
		const float ReachableDeg = FMath::Max3(PivotInitialAngleDeg - PivotExitAngle,
		                                       PivotAccumDeg + RemainingDeg - PivotExitAngle, 1.0f);
		PivotProgress = FMath::Max(PivotProgress,
			FMath::Clamp(PivotAccumDeg / ReachableDeg, 0.0f, 1.0f));
		if (!HasAuthority() && FMath::Abs(PivotProgress - LastSentPivotProgress) >= 0.05f)
		{
			LastSentPivotProgress = PivotProgress;
			Server_SetPivotProgress(PivotProgress);
		}

		SpeedType = ECatMoveType::Turn;   // footwork state; also gates turn-in-place + lean off
	}
}

void ACatBase::EnterPivot()
{
	bIsPivoting        = true;
	bGoPivot           = true;
	PivotSustainTimer  = 0.0f;
	PivotSweepAccumDeg = 0.0f;

	// Latch the scrub denominator; floor at 1° so a threshold-grazing entry can't divide by ~0.
	PivotInitialAngleDeg = FMath::Max(FMath::Abs(FRotator::NormalizeAxis(
		PivotLiveInputDir.Rotation().Yaw - GetActorRotation().Yaw)), 1.0f);
	PivotAngleDeg         = PivotInitialAngleDeg;
	PivotAccumDeg         = 0.0f;
	PivotProgress         = 0.0f;
	LastSentPivotProgress = 0.0f;
	if (!HasAuthority())
	{
		Server_SetPivotProgress(0.0f);
	}

	// Predicted braking boost; the server mirrors it so move replay agrees.
	ApplyPivotBraking(true);
	if (!HasAuthority())
	{
		Server_SetPivotBraking(true, PivotAngleDeg);
	}

	// Plant dip: additive kick into the landing-cushion spring (composes with a
	// same-moment landing kick instead of clobbering it), scaled by entry speed
	// over the trot→sprint band so a sprint plant hits harder than a trot plant.
	if (bEnableLandCushion)
	{
		const float PlantScale = FMath::GetMappedRangeValueClamped(
			FVector2D(PivotMinSpeed, SprintMaxWalkSpeed), FVector2D(0.5f, 1.0f), Speed);
		MeshCushionVelocity -= PivotPlantDip * PlantScale * LandCushionFrequency * UE_EULERS_NUMBER;
	}

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Pivot ENTER — Speed %.0f"), *GetName(), Speed);
}

void ACatBase::ExitPivot()
{
	if (!bIsPivoting)
	{
		return;
	}

	bIsPivoting        = false;
	bGoPivot           = false;
	PivotCooldownTimer = PivotCooldown;

	// Snap the scrub to the end pose for the blend-out.
	PivotProgress = 1.0f;
	if (!HasAuthority())
	{
		Server_SetPivotProgress(1.0f);
	}

	ApplyPivotBraking(false);
	if (!HasAuthority())
	{
		Server_SetPivotBraking(false, 0.0f);
	}

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Pivot EXIT — Speed %.0f"), *GetName(), Speed);
}

void ACatBase::ApplyPivotBraking(bool bApply)
{
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->BrakingDecelerationWalking = bApply ? PivotBrakingDeceleration : MovementBrakingDeceleration;
	}
}

void ACatBase::Server_SetPivotBraking_Implementation(bool bApply, float EntryAngleDeg)
{
	ApplyPivotBraking(bApply);
	bGoPivot = bApply;   // server copy → replicates on to simulated proxies (COND_SkipOwner)
	if (bApply)
	{
		PivotAngleDeg = EntryAngleDeg;   // proxies select the same 90°/180° clip variant
	}
}

void ACatBase::Server_SetPivotProgress_Implementation(float NewProgress)
{
	PivotProgress = NewProgress;
}

// ══════════════════════════════════════════════════════════════════════════
// ── Weighty Stop (M2 part 2b, weighty-movement pass) ──────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Releasing input at speed swaps the CMC's braking model for the duration of the
// stop. Default braking is friction-dominated (GroundFriction 5 × factor 2 → a
// ~60 ms exponential decay constant): a sprint halts in ~52 cm and a trot in
// ~29 cm — quick, light, and barely gait-differentiated, because exponential
// decay compresses entry-speed differences. A CONSTANT deceleration instead makes
// stop distance v²/2a, so the sprint run-out naturally carries ~3× the trot's
// from one knob. The cat keeps striding down through the gait blendspace as
// speed bleeds (a natural run-out stop — no skid clip exists yet; that's the M5
// batch), the accel-lean spring braces the body, and at the plant the
// landing-cushion spring takes a kick while the camera stop-settle fires on its
// own. Local owner drives everything; only the braking swap touches the server.

void ACatBase::UpdateWeightyStop()
{
	const float DeltaTime = DeltaTimeCached;
	StopReaccelTimer = FMath::Max(StopReaccelTimer - DeltaTime, 0.0f);

	const bool bGrounded = MovementStage == ECatMovementStage::OnGround
		&& JumpPhase == ECatJumpPhase::None;

	if (!bIsStopping)
	{
		// Entry: the had-input → released edge while grounded at gait speed. The pivot
		// gate matters because its input suppression zeroes acceleration, which would
		// otherwise read as a release right as the pivot plants.
		const bool bReleasedAtSpeed = bEnableWeightyStops
			&& !bIsPivoting
			&& !bIsGrabbing
			&& bGrounded
			&& bHadMovementInputLastFrame
			&& !bHasMovementInput
			&& Speed >= StopMinSpeed;
		if (bReleasedAtSpeed)
		{
			EnterStop();
		}
	}
	else
	{
		StopElapsed += DeltaTime;

		// Skid-clip scrub: fraction of the reachable speed collapse (linear in time under
		// the constant-decel run-out). Monotonic — downhill wiggle must not run it backward.
		if (bGoSkid && StopEntrySpeed > KINDA_SMALL_NUMBER)
		{
			const float Raw = (1.0f - Speed / StopEntrySpeed) / SkidReachableFrac;
			SkidProgress = FMath::Max(SkidProgress, FMath::Clamp(Raw, 0.0f, 1.0f));
			if (!HasAuthority() && FMath::Abs(SkidProgress - LastSentSkidProgress) >= 0.05f)
			{
				LastSentSkidProgress = SkidProgress;
				Server_SetSkidProgress(SkidProgress);
			}
		}

		// Failsafe: a steep-downhill run-out can outlast any sensible stop.
		constexpr float StopMaxDuration = 1.2f;

		if (!bEnableWeightyStops || bIsPivoting || bIsGrabbing || !bGrounded
			|| bHasMovementInput || StopElapsed > StopMaxDuration)
		{
			ExitStop(false);   // interrupted — no plant, no ramp
		}
		else if (Speed <= StopPlantSpeed)
		{
			ExitStop(true);    // run-out complete — plant
		}
	}

	bHadMovementInputLastFrame = bHasMovementInput;
}

void ACatBase::EnterStop()
{
	bIsStopping       = true;
	StopEntrySpeed    = Speed;
	StopStartLocation = GetActorLocation();
	StopElapsed       = 0.0f;

	// Sprint-band stops drive the Skid state; trot-band stops keep the plain gait run-out.
	bGoSkid = StopEntrySpeed >= SkidMinEntrySpeed;
	SkidProgress         = 0.0f;
	LastSentSkidProgress = 0.0f;
	SkidReachableFrac    = FMath::Max(1.0f - StopPlantSpeed / FMath::Max(StopEntrySpeed, 1.0f), 0.1f);
	if (bGoSkid && !HasAuthority())
	{
		Server_SetSkidProgress(0.0f);
	}

	// Predicted braking swap; the server mirrors it so move replay coasts the same.
	ApplyStopBraking(true);
	if (!HasAuthority())
	{
		Server_SetStopBraking(true, bGoSkid);
	}

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Stop ENTER — Speed %.0f%s"), *GetName(), StopEntrySpeed,
		bGoSkid ? TEXT(" (skid)") : TEXT(""));
}

void ACatBase::ExitStop(bool bPlanted)
{
	if (!bIsStopping)
	{
		return;
	}
	bIsStopping = false;

	// Plant = clip completed, snap the scrub for the settle; abort = freeze where it was
	// so the blend-out leaves from the pose the skid actually reached.
	if (bGoSkid && bPlanted)
	{
		SkidProgress = 1.0f;
		if (!HasAuthority())
		{
			Server_SetSkidProgress(1.0f);
		}
	}
	bGoSkid = false;

	ApplyStopBraking(false);
	if (!HasAuthority())
	{
		Server_SetStopBraking(false, false);
	}

	if (bPlanted)
	{
		// Entry-speed scale over the stop→sprint band — a sprint plant hits harder
		// and guards re-acceleration longer than a bare-minimum trot plant.
		const float PlantScale = FMath::GetMappedRangeValueClamped(
			FVector2D(StopMinSpeed, SprintMaxWalkSpeed), FVector2D(0.4f, 1.0f), StopEntrySpeed);

		// Plant dip: additive kick into the landing-cushion spring (pivot pattern —
		// composes with a same-moment landing kick instead of clobbering it).
		if (bEnableLandCushion && StopPlantDip > 0.0f)
		{
			MeshCushionVelocity -= StopPlantDip * PlantScale * LandCushionFrequency * UE_EULERS_NUMBER;
		}

		StopReaccelDuration = StopReaccelDelay * PlantScale;
		StopReaccelTimer    = StopReaccelDuration;
	}

	// Per-stop spec line: entry speed + run-out distance/duration. These numbers are
	// the M5 skid-clip authoring spec — read them out of the log after a PIE session.
	const float RunOutDist = FVector::Dist2D(GetActorLocation(), StopStartLocation);
	UE_LOG(LogCatVentures, Log, TEXT("[%s] Stop %s — entry %.0f cm/s, run-out %.0f cm in %.2f s"),
		*GetName(), bPlanted ? TEXT("PLANT") : TEXT("ABORT"), StopEntrySpeed, RunOutDist, StopElapsed);
}

void ACatBase::ApplyStopBraking(bool bApply)
{
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->bUseSeparateBrakingFriction = bApply;
		CMC->BrakingFriction             = bApply ? StopBrakingFriction : MovementBrakingFriction;
		// The restore re-asserts whichever braking should be live: the pivot boost if a
		// pivot owns the CMC (defensive — the stop exits on the input frame, a pivot
		// arms ≥ PivotSustainTime later, so overlap shouldn't occur), else gait braking.
		CMC->BrakingDecelerationWalking  = bApply ? StopBrakingDeceleration
		                                 : (bIsPivoting ? PivotBrakingDeceleration : MovementBrakingDeceleration);
	}
}

void ACatBase::Server_SetStopBraking_Implementation(bool bApply, bool bSkid)
{
	ApplyStopBraking(bApply);
	bGoSkid = bApply && bSkid;   // server copy → replicates on to simulated proxies (COND_SkipOwner)
}

void ACatBase::Server_SetSkidProgress_Implementation(float NewProgress)
{
	SkidProgress = NewProgress;
}

// ══════════════════════════════════════════════════════════════════════════
// ── Weighty Start (M2 part 2c, weighty-movement pass) ─────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// The launch-step into a sprint. A fresh sprint start from near-standstill
// coils for StartCoilTime (Move() suppresses input — the standstill-jump
// anticipation doctrine; trot starts stay instant for platforming), then
// bursts: MaxAcceleration × StartBurstAccelMultiplier until the speed punches
// through StartBurstEndSpeed, after which normal acceleration earns the sprint
// top end. Pose comes free: the coil kicks a modest cushion load dip, and the
// burst rails the accel-lean spring forward (the lunge) exactly like the stop
// rides its release overshoot. Triggers are EDGE-based — a fresh-input edge
// while sprinting, or a sprint-engage edge while input is fresh — never level:
// sprinting into a wall collapses Speed under the coil gate with input held,
// and a level trigger would coil against the wall.

void ACatBase::UpdateWeightyStart()
{
	const float DeltaTime = DeltaTimeCached;
	StartCooldownTimer = FMath::Max(StartCooldownTimer - DeltaTime, 0.0f);
	InputRampTimer     = FMath::Max(InputRampTimer - DeltaTime, 0.0f);

	const bool bInputFresh = PivotInputStaleTime < 0.15f;
	const bool bGrounded   = MovementStage == ECatMovementStage::OnGround
		&& JumpPhase == ECatJumpPhase::None;

	// Trigger edges, evaluated every frame so the last-frame trackers stay current.
	const bool bFreshInputEdge  = bInputFresh && !bInputWasFreshLastFrame;
	const bool bSprintOnEdge    = bIsSprinting && !bWasSprintingLastFrame;
	bInputWasFreshLastFrame = bInputFresh;
	bWasSprintingLastFrame  = bIsSprinting;

	if (!bIsStartCoiling && !bIsStartBursting)
	{
		const bool bEligible = bEnableWeightyStarts
			&& StartCooldownTimer <= 0.0f
			&& !bIsPivoting && !bIsStopping && !bIsGrabbing
			&& bGrounded
			&& Speed <= StartCoilMaxSpeed
			&& JumpAnticipationTimer <= 0.0f;   // a jump coil owns the moment

		if (bEligible
			&& ((bFreshInputEdge && bIsSprinting) || (bSprintOnEdge && bInputFresh)))
		{
			// M4 ramp would fight the burst envelope — clear it on the coil path.
			InputRampTimer = 0.0f;
			EnterStartCoil();
		}
		else if (bFreshInputEdge && StartInputRampTime > 0.0f)
		{
			// M4 input shaping: plain fresh start from idle — ease the input in.
			InputRampTimer = StartInputRampTime;
		}
		return;
	}

	// ── Coil hold ─────────────────────────────────────────────────────
	if (bIsStartCoiling)
	{
		// Abort: released, airborne, grabbed, or a pivot somehow took over. A jump
		// press also lands here (JumpAnticipationTimer arms) — the jump coil wins.
		if (!bInputFresh || !bGrounded || bIsGrabbing || bIsPivoting
			|| JumpAnticipationTimer > 0.0f)
		{
			bIsStartCoiling    = false;
			StartCooldownTimer = 0.4f;
			SetStartStepActive(false, /*bSnapProgress=*/false);   // freeze — blend out from the actual pose
			UE_LOG(LogCatVentures, Log, TEXT("[%s] Start coil ABORT"), *GetName());
			return;
		}

		// Clip scrub, coil phase: timer fraction over the clip's authored coil portion.
		StartProgress = FMath::Max(StartProgress,
			StartClipCoilFrac * (1.0f - StartCoilTimer / FMath::Max(StartCoilTime, KINDA_SMALL_NUMBER)));
		StreamStartProgress();

		StartCoilTimer -= DeltaTime;
		if (StartCoilTimer <= 0.0f)
		{
			bIsStartCoiling = false;
			EnterStartBurst();
		}
		return;
	}

	// ── Burst ─────────────────────────────────────────────────────────
	if (bIsStartBursting)
	{
		StartBurstElapsed += DeltaTime;
		constexpr float BurstMaxDuration = 0.6f;   // failsafe (uphill, bumped, etc.)

		// Clip scrub, burst phase: the launch pose tracks the physical acceleration.
		StartProgress = FMath::Max(StartProgress, StartClipCoilFrac + (1.0f - StartClipCoilFrac)
			* FMath::Clamp(Speed / FMath::Max(StartBurstEndSpeed, 1.0f), 0.0f, 1.0f));
		StreamStartProgress();

		if (!bInputFresh || !bGrounded || bIsGrabbing || bIsPivoting
			|| Speed >= StartBurstEndSpeed || StartBurstElapsed > BurstMaxDuration)
		{
			EndStartBurst();
		}
	}
}

void ACatBase::SetStartStepActive(bool bActive, bool bSnapProgress)
{
	bGoStartStep = bActive;
	if (bSnapProgress)
	{
		StartProgress = 1.0f;
		if (!HasAuthority())
		{
			Server_SetStartProgress(1.0f);
		}
	}
	if (!HasAuthority())
	{
		Server_SetStartStep(bActive);
	}
}

void ACatBase::StreamStartProgress()
{
	if (!HasAuthority() && FMath::Abs(StartProgress - LastSentStartProgress) >= 0.05f)
	{
		LastSentStartProgress = StartProgress;
		Server_SetStartProgress(StartProgress);
	}
}

void ACatBase::EnterStartCoil()
{
	StartCoilLocation = GetActorLocation();

	// Clip scrub arm (the skid pattern) — before the burst-only early-out so the clip
	// plays either way (burst-only starts scrub from the clip's burst portion).
	StartProgress          = 0.0f;
	LastSentStartProgress  = 0.0f;
	SetStartStepActive(true, /*bSnapProgress=*/false);

	// StartCoilTime 0 = burst-only mode: skip the hold entirely.
	if (StartCoilTime <= 0.0f)
	{
		EnterStartBurst();
		return;
	}

	bIsStartCoiling = true;
	StartCoilTimer  = StartCoilTime;

	// The authored §B2 crouch replaces the springy coil dip — both at once would
	// double-dip the load.
	if (bEnableLandCushion && StartCoilDip > 0.0f && StartCrouchScale <= 0.0f)
	{
		MeshCushionVelocity -= StartCoilDip * LandCushionFrequency * UE_EULERS_NUMBER;
	}

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Start coil ENTER — Speed %.0f"), *GetName(), Speed);
}

void ACatBase::EnterStartBurst()
{
	bIsStartBursting  = true;
	StartBurstElapsed = 0.0f;

	// Predicted acceleration boost; the server mirrors it so move replay agrees.
	ApplyStartBurstAccel(true);
	if (!HasAuthority())
	{
		Server_SetStartBurstAccel(true);
	}

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Start burst ENTER"), *GetName());
}

void ACatBase::EndStartBurst()
{
	if (!bIsStartBursting)
	{
		return;
	}
	bIsStartBursting   = false;
	StartCooldownTimer = 0.4f;

	// Reached the burst end speed = launch complete → snap the scrub for the blend-out;
	// any abort freezes it instead (blend out from the pose the launch actually reached).
	SetStartStepActive(false, /*bSnapProgress=*/Speed >= StartBurstEndSpeed);

	ApplyStartBurstAccel(false);
	if (!HasAuthority())
	{
		Server_SetStartBurstAccel(false);
	}

	// Per-start spec line: coil→burst-end distance/duration — the M5 start-clip
	// authoring spec, read out of the log after a PIE session.
	const float LaunchDist = FVector::Dist2D(GetActorLocation(), StartCoilLocation);
	UE_LOG(LogCatVentures, Log, TEXT("[%s] Start burst END — Speed %.0f, launch %.0f cm in %.2f s"),
		*GetName(), Speed, LaunchDist, StartBurstElapsed);
}

void ACatBase::Server_SetStartStep_Implementation(bool bActive)
{
	bGoStartStep = bActive;   // server copy → replicates on to simulated proxies (COND_SkipOwner)
}

void ACatBase::Server_StartMantle_Implementation(FVector_NetQuantize InStart, FVector_NetQuantize InTarget)
{
	if (Traversal)
	{
		Traversal->StartMantle(InStart, InTarget);
	}
}

void ACatBase::Server_WallBounce_Implementation(FVector_NetQuantizeNormal WallNormal)
{
	if (Traversal)
	{
		Traversal->DoWallBounce(WallNormal);
	}
}

void ACatBase::Server_SetWallAttach_Implementation(bool bActive, FVector_NetQuantizeNormal WallNormal,
                                                   float RiseSpeed, float RiseTime)
{
	if (!Traversal)
	{
		return;
	}
	if (bActive)
	{
		Traversal->StartWallAttach(WallNormal, RiseSpeed, RiseTime);
	}
	else
	{
		Traversal->EndWallAttach(EWallAttachEnd::Remote);
	}
}

void ACatBase::SetWallAttachAnimState(bool bActive)
{
	bGoWallAttach = bActive;
}

void ACatBase::SetMantleAnimState(bool bActive, float Progress)
{
	bGoMantle      = bActive;
	MantleProgress = Progress;
}

void ACatBase::BeginMantleAnimHold()
{
	bGoMantle           = true;
	MantleProgress      = 1.0f;
	MantleAnimHoldTimer = 0.5f;
}

void ACatBase::Server_SetStartProgress_Implementation(float NewProgress)
{
	StartProgress = NewProgress;
}

void ACatBase::ApplyStartBurstAccel(bool bApply)
{
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxAcceleration = bApply
			? MovementAcceleration * StartBurstAccelMultiplier
			: MovementAcceleration;
	}
}

void ACatBase::Server_SetStartBurstAccel_Implementation(bool bApply)
{
	ApplyStartBurstAccel(bApply);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Turn-In-Place (root-motion montages) ──────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Procedural turn-in-place (Fab-kit model): when the camera turns away from a
// stationary cat, rotate the BODY toward the camera at a capped rate and drive the
// in-place BS1_Cat_Turn footwork via TurnRateAnim. Rotation is by shortest signed
// angle — so there is no ±180 wrap and no multi-second catch-up; the cat tracks the
// camera continuously. Owner-only; proxies get bGoTurn + TurnRateAnim (replicated)
// plus the replicated body rotation, and enter the Turn state via the else-branch
// of UpdateAnimationStates.

void ACatBase::UpdateTurnInPlace()
{
	if (!IsLocallyControlled()) return;

	const float DeltaTime  = DeltaTimeCached;
	const float DesiredYaw = GetControlRotation().Yaw;
	const float CurrentYaw = GetActorRotation().Yaw;
	const float DeltaToCam = FRotator::NormalizeAxis(DesiredYaw - CurrentYaw);

	// Require the cat to have nearly stopped before engaging. Coming straight out of
	// movement there is a window where SpeedType==Idle but the body is still sliding;
	// if we forced SpeedType=Turn during it, the Locomotion SM (which has no direct
	// Move->Turn edge) would stay stuck in Move and rotate with no footwork. Gating on
	// near-zero speed lets Move->Idle finish first, so the following Idle->Turn shows
	// the BS1_Cat_Turn footwork — and it reads naturally as "settle, then turn".
	constexpr float TurnEngageMaxSpeed = 10.0f;   // cm/s
	const bool bCanTurn = (SpeedType == ECatMoveType::Idle)
		&& (MovementStage == ECatMovementStage::OnGround)
		&& (Speed < TurnEngageMaxSpeed)
		&& !bIsPivoting;   // pivot owns the body + footwork (its SpeedType=Turn also gates this)

	// Hysteresis: a deliberate offset (TurnInPlaceThreshold) engages the turn; it stays
	// engaged until the body is nearly aligned, so it neither quits short nor chatters at
	// the threshold. bIsTurningInPlace is the engaged flag.
	constexpr float DisengageAngle = 4.0f;
	if (!bCanTurn)
	{
		bIsTurningInPlace = false;
	}
	else if (bIsTurningInPlace)
	{
		if (FMath::Abs(DeltaToCam) <= DisengageAngle) bIsTurningInPlace = false;
	}
	else if (FMath::Abs(DeltaToCam) >= TurnInPlaceThreshold)
	{
		bIsTurningInPlace = true;
	}

	float TargetTurnRate = 0.0f;
	if (bIsTurningInPlace)
	{
		// Rotate the capsule toward the camera by the shortest signed path, capped per
		// frame. FixedTurn clamps the final step to the remaining angle, so the rotation
		// eases out naturally as it lands on target — and it can never wrap the wrong way.
		constexpr float TurnSpeedDegPerSec = 150.0f;
		const float NewYaw       = FMath::FixedTurn(CurrentYaw, DesiredYaw, TurnSpeedDegPerSec * DeltaTime);
		const float AppliedDelta = FRotator::NormalizeAxis(NewYaw - CurrentYaw);
		SetActorRotation(FRotator(0.0f, NewYaw, 0.0f));

		// Footwork blend param for BS1_Cat_Turn: signed, normalized so a full-speed turn
		// reads as ±1 (the 90° step) and the ease-out near target settles toward 0 (idle).
		const float AppliedRate = (DeltaTime > KINDA_SMALL_NUMBER) ? AppliedDelta / DeltaTime : 0.0f;
		TargetTurnRate = FMath::Clamp(AppliedRate / TurnSpeedDegPerSec, -1.0f, 1.0f);

		SpeedType = ECatMoveType::Turn;   // drive the AnimBP Locomotion Turn state
	}
	else if (bIsPivoting)
	{
		// Moving pivot (runs earlier this frame): its footwork rides the same
		// TurnRateAnim interp and turn RPCs as turn-in-place.
		TargetTurnRate = PivotTurnRateTarget;
	}

	// Ease the blend param so the footwork fades in/out instead of popping. The pivot
	// uses a much faster attack: a plant is sudden and rotates hardest on its first
	// frames — the soft turn-in-place ramp left the footwork near the idle pose for
	// ~0.25 s while the body whipped (the "skating" in the 2026-07-18 sampler round:
	// TurnRateAnim 0.08 at pivot entry, ~0.9 only a quarter-second later).
	const float TurnRateInterpSpeed = bIsPivoting ? 30.0f : 10.0f;
	TurnRateAnim = FMath::FInterpTo(TurnRateAnim, TargetTurnRate, DeltaTime, TurnRateInterpSpeed);

	// Replicate the turn to other machines via the existing RPC infra: proxies enter the
	// Turn state from bGoTurn (else-branch) and read the replicated TurnRateAnim. On the
	// listen-server host (authority) these writes are already authoritative.
	// Turn-in-place body rotation does NOT replicate via the CMC: with orient-to-movement
	// the server DERIVES rotation from acceleration and never receives the client's
	// SetActorRotation — at zero acceleration (in-place) the server copy freezes (found in
	// the 2026-07-06 MP pass: the host saw the client cat's body never rotate). The yaw
	// therefore rides the turn RPCs explicitly; the server applies it and standard movement
	// replication carries it on to simulated proxies.
	const bool bTurnActive = bIsTurningInPlace || bIsPivoting;
	const float BodyYaw = GetActorRotation().Yaw;
	if (bGoTurn != bTurnActive)
	{
		bGoTurn = bTurnActive;
		LastSentBodyYaw = BodyYaw;
		if (!HasAuthority()) Server_SetTurnActive(bTurnActive, BodyYaw);
	}
	// Send on rate change OR yaw drift: a saturated full-speed turn holds TurnRateAnim
	// constant at ±1, so a rate-only throttle would starve the server of yaw mid-turn.
	if (FMath::Abs(TurnRateAnim - LastSentTurnRateAnim) > 0.05f
		|| (bTurnActive && FMath::Abs(FRotator::NormalizeAxis(BodyYaw - LastSentBodyYaw)) > 5.0f))
	{
		LastSentTurnRateAnim = TurnRateAnim;
		LastSentBodyYaw = BodyYaw;
		if (!HasAuthority()) Server_SetTurnRate(TurnRateAnim, BodyYaw);
	}
}

void ACatBase::BeginSwatTrace(USkeletalMeshComponent* MeshComp, FName SocketName)
{
	if (!HasAuthority()) return;

	SwatPreviousPawLocation = MeshComp->GetSocketLocation(SocketName);
	SwatAlreadyHitActors.Empty();
}

void ACatBase::ProcessSwatTraceTick(USkeletalMeshComponent* MeshComp, FName SocketName, float SweepRadius, float DeltaSeconds)
{
	if (!HasAuthority()) return;

	const FVector CurrentPawLocation = MeshComp->GetSocketLocation(SocketName);

	FHitResult HitResult;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	FCollisionObjectQueryParams ObjParams;
	ObjParams.AddObjectTypesToQuery(ECC_Pawn);
	ObjParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	ObjParams.AddObjectTypesToQuery(ECC_Destructible);

	if (GetWorld()->SweepSingleByObjectType(
		HitResult,
		SwatPreviousPawLocation,
		CurrentPawLocation,
		FQuat::Identity,
		ObjParams,
		FCollisionShape::MakeSphere(SweepRadius),
		Params))
	{
		if (HitResult.GetActor() && !SwatAlreadyHitActors.Contains(HitResult.GetActor()))
		{
			SwatAlreadyHitActors.Add(HitResult.GetActor());
			HandleSwatHit(HitResult);
		}
	}

	SwatPreviousPawLocation = CurrentPawLocation;
}

void ACatBase::EndSwatTrace()
{
	if (!HasAuthority()) return;

	SwatAlreadyHitActors.Empty();
}

void ACatBase::HandleSwatHit(const FHitResult& HitResult)
{
	AActor* HitActor = HitResult.GetActor();
	if (!HitActor) return;

	const FVector ImpulseDir = (HitActor->GetActorLocation() - GetActorLocation()).GetSafeNormal();

	// Apply impulse to physics objects
	if (UPrimitiveComponent* HitComp = HitResult.GetComponent())
	{
		if (HitComp->IsSimulatingPhysics())
		{
			HitComp->AddImpulse(ImpulseDir * SwatImpulseForce, NAME_None, /*bVelChange=*/false);
		}
	}

	// Send 1 point of damage with the swat direction. The receiver (e.g. BPC_ChaosItem)
	// decides how to respond — the Cat has no knowledge of GC or destruction logic.
	UGameplayStatics::ApplyPointDamage(
		HitActor,
		1.0f,
		ImpulseDir,
		HitResult,
		GetController(),
		this,
		UDamageType::StaticClass()
	);

	OnSwatHit.Broadcast(HitActor, HitResult.ImpactPoint);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Interaction ─────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::TriggerInteract()
{
	Server_Interact();
}

void ACatBase::Server_Interact_Implementation()
{
	PerformInteractTrace();
}

void ACatBase::PerformInteractTrace()
{
	const FVector Start = GetActorLocation();
	const FVector End   = Start + GetActorForwardVector() * InteractTraceLength;

	FHitResult HitResult;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	if (GetWorld()->SweepSingleByChannel(
		HitResult,
		Start,
		End,
		FQuat::Identity,
		ECC_Visibility,
		FCollisionShape::MakeSphere(30.0f),
		Params))
	{
		if (AActor* HitActor = HitResult.GetActor())
		{
			if (HitActor->GetClass()->ImplementsInterface(UInteractableInterface::StaticClass()))
			{
				IInteractableInterface::Execute_Interact(HitActor, this);
			}
		}
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Mouth Grab ──────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::TriggerGrab()
{
	// Client-side prediction: apply drag settings immediately so there is no
	// rubber-band stutter waiting for the server round-trip.
	ApplyDragMovementSettings();

	if (HasAuthority())
	{
		Server_Grab_Implementation();
	}
	else
	{
		Server_Grab();
	}
}

void ACatBase::TriggerRelease()
{
	// Client-side prediction: restore settings before the RPC so input feels instant.
	RestoreNormalMovementSettings();

	if (HasAuthority())
	{
		Server_ReleaseGrab_Implementation();
	}
	else
	{
		Server_ReleaseGrab();
	}
}

void ACatBase::Server_Grab_Implementation()
{
	if (bIsGrabbing) return;

	const FTransform MouthTransform = GetMesh()->GetSocketTransform(TEXT("socket_mouth"));
	const FVector    TraceStart     = MouthTransform.GetLocation();
	const FVector    TraceEnd       = TraceStart + MouthTransform.GetUnitAxis(EAxis::X) * GrabTraceLength;

	FHitResult HitResult;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	// Sweep against physics bodies (SM), destructibles (GC), and world dynamic actors.
	FCollisionObjectQueryParams ObjParams;
	ObjParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	ObjParams.AddObjectTypesToQuery(ECC_Destructible);
	ObjParams.AddObjectTypesToQuery(ECC_WorldDynamic);

	const bool bHit = GetWorld()->SweepSingleByObjectType(
		HitResult,
		TraceStart,
		TraceEnd,
		FQuat::Identity,
		ObjParams,
		FCollisionShape::MakeSphere(GrabTraceRadius),
		Params);

	if (!bHit)
	{
		Client_GrabFailed();
		return;
	}

	UPrimitiveComponent* HitComp = HitResult.GetComponent();
	if (!HitComp)
	{
		Client_GrabFailed();
		return;
	}

	// For Geometry Collections: wake the Chaos solver on the SERVER before the
	// IsSimulatingPhysics() check. The multicast will wake on all other machines.
	if (UGeometryCollectionComponent* GCC = Cast<UGeometryCollectionComponent>(HitComp))
	{
		GCC->ApplyKinematicField(GrabTraceRadius * 2.0f, HitResult.ImpactPoint);
	}

	if (!HitComp->IsSimulatingPhysics())
	{
		UE_LOG(LogCatVentures, Verbose, TEXT("[%s] Grab failed — '%s' not simulating physics"),
			*GetName(), *GetNameSafe(HitComp->GetOwner()));
		Client_GrabFailed();
		return;
	}

	// Determine the bone to constrain. GC → root cluster (NAME_None) to avoid
	// crashing on individual cluster particles that lack rigid body handles.
	FName ConstraintBone = HitResult.BoneName;
	if (HitComp->IsA<UGeometryCollectionComponent>())
	{
		ConstraintBone = NAME_None;
	}

	// Server validated the trace — now multicast so ALL machines create their own
	// local constraint and modify their own Chaos solver state.
	Multicast_Grab(HitComp, ConstraintBone);
}

void ACatBase::Client_GrabFailed_Implementation()
{
	// Roll back the drag-settings prediction from TriggerGrab. Without this, a
	// missed grab left the player at DragWalkSpeed with rotation locked until
	// they released the button.
	if (!bIsGrabbing)
	{
		RestoreNormalMovementSettings();
	}
}

void ACatBase::Multicast_Grab_Implementation(UPrimitiveComponent* GrabbedComp, FName BoneName)
{
	if (!GrabbedComp) return;

	// Create the constraint dynamically on this machine's physics solver.
	// No explicit name: a fixed name can collide with a previous, still-pending-kill
	// constraint during a fast release→re-grab.
	GrabConstraint = NewObject<UPhysicsConstraintComponent>(this);
	GrabConstraint->SetupAttachment(GrabTargetLocation);
	GrabConstraint->RegisterComponent();

	// Linear: limited slack + position/velocity drive toward anchor.
	GrabConstraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Limited, GrabLinearLimit);
	GrabConstraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Limited, GrabLinearLimit);
	GrabConstraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Limited, GrabLinearLimit);
	GrabConstraint->SetLinearPositionDrive(true, true, true);
	GrabConstraint->SetLinearVelocityDrive(true, true, true);
	GrabConstraint->SetLinearDriveParams(GrabConstraintStiffness, GrabConstraintDamping, GrabConstraintMaxForce);

	// Angular: free — let the object tumble naturally while being dragged.
	GrabConstraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Free, 0.0f);
	GrabConstraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Free, 0.0f);
	GrabConstraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Free, 0.0f);

	// Disable collision between constrained bodies to prevent jitter.
	GrabConstraint->SetDisableCollision(true);

	// Snap the constraint to the grab target offset (80 cm ahead of socket_mouth).
	GrabConstraint->SetWorldLocation(GrabTargetLocation->GetComponentLocation());

	// Wake the target body on THIS machine's solver before binding the constraint.
	GrabbedComp->WakeRigidBody(BoneName);

	// Anchor to this machine's local capsule physics body → grabbed component.
	GrabConstraint->SetConstrainedComponents(GetCapsuleComponent(), NAME_None, GrabbedComp, BoneName);

	// Suppress collision-based strain on THIS machine's Chaos solver while dragging.
	if (UGeometryCollectionComponent* GCC = Cast<UGeometryCollectionComponent>(GrabbedComp))
	{
		GCC->SetEnableDamageFromCollision(false);
	}

	GrabbedComponent = GrabbedComp;
	bIsGrabbing      = true;
	ApplyDragMovementSettings();
}

void ACatBase::Server_ReleaseGrab_Implementation()
{
	if (!bIsGrabbing) return;
	Multicast_ReleaseGrab();
}

void ACatBase::Multicast_ReleaseGrab_Implementation()
{
	// Re-enable collision strain on THIS machine's Chaos solver.
	if (UGeometryCollectionComponent* GCC = Cast<UGeometryCollectionComponent>(GrabbedComponent.Get()))
	{
		GCC->SetEnableDamageFromCollision(true);
	}
	if (GrabConstraint)
	{
		GrabConstraint->DestroyComponent();
		GrabConstraint = nullptr;
	}
	GrabbedComponent.Reset();
	bIsGrabbing = false;
	RestoreNormalMovementSettings();
}

void ACatBase::UpdateGrab(float DeltaTime)
{
	if (!bIsGrabbing) return;

	// Local cleanup: if the grabbed actor was destroyed on this machine, tear down
	// the local constraint immediately. No multicast needed — each machine detects
	// destruction independently.
	if (!GrabbedComponent.IsValid())
	{
		if (GrabConstraint)
		{
			GrabConstraint->DestroyComponent();
			GrabConstraint = nullptr;
		}
		bIsGrabbing = false;
		RestoreNormalMovementSettings();
		return;
	}

	// Server-authoritative auto-release: if the object drifted too far, the server
	// multicasts the release to all machines.
	if (HasAuthority())
	{
		const float Dist = FVector::Dist(
			GrabTargetLocation->GetComponentLocation(),
			GrabbedComponent->GetComponentLocation());

		if (Dist > MaxGrabDistance)
		{
			Multicast_ReleaseGrab();
			return;
		}
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Sprint Gait (M1, weighty-movement pass) ───────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::OnSprintPressed()  { SetSprinting(true); }
void ACatBase::OnSprintReleased() { SetSprinting(false); }

void ACatBase::SetSprinting(bool bNewSprinting)
{
	if (bIsSprinting == bNewSprinting)
	{
		return;
	}

	// Owner prediction: apply the gait locally right away (same pattern as the
	// grab drag-settings prediction); the server copy follows via RPC/authority.
	bIsSprinting = bNewSprinting;
	ApplyGaitMovementSettings();

	if (!HasAuthority())
	{
		Server_SetSprinting(bNewSprinting);
	}

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Sprint %s"), *GetName(), bNewSprinting ? TEXT("ON") : TEXT("OFF"));
}

void ACatBase::Server_SetSprinting_Implementation(bool bNewSprinting)
{
	bIsSprinting = bNewSprinting;
	ApplyGaitMovementSettings();
}

void ACatBase::OnRep_bIsSprinting()
{
	ApplyGaitMovementSettings();
}

void ACatBase::ApplyGaitMovementSettings()
{
	// Drag settings own the CMC during a grab; RestoreNormalMovementSettings
	// re-applies the current gait on release.
	if (bIsGrabbing)
	{
		return;
	}

	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxWalkSpeed = bIsSprinting ? SprintMaxWalkSpeed : MovementMaxWalkSpeed;
		CMC->RotationRate = FRotator(0.0f, bIsSprinting ? SprintRotationRateYaw : MovementRotationRateYaw, 0.0f);
	}
}

void ACatBase::ApplyDragMovementSettings()
{
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxWalkSpeed              = DragWalkSpeed;
		CMC->bOrientRotationToMovement = false;
	}
}

void ACatBase::RestoreNormalMovementSettings()
{
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxWalkSpeed              = bIsSprinting ? SprintMaxWalkSpeed : MovementMaxWalkSpeed;
		CMC->bOrientRotationToMovement = true;
	}
}

void ACatBase::OnRep_bIsGrabbing()
{
	if (bIsGrabbing)
		ApplyDragMovementSettings();
	else
		RestoreNormalMovementSettings();
}

// ══════════════════════════════════════════════════════════════════════════
// ── Replication Boilerplate ─────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ACatBase, SpeedType);
	DOREPLIFETIME(ACatBase, MovementStage);
	DOREPLIFETIME(ACatBase, JumpPhase);
	DOREPLIFETIME_CONDITION(ACatBase, bGoTurn, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, bGoPivot, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, PivotProgress, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, PivotAngleDeg, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, bGoSkid, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, SkidProgress, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, bGoStartStep, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, StartProgress, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, bGoMantle, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, MantleProgress, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, bMantleVault, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, MantleLedgeHeight, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, bGoWallAttach, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(ACatBase, TurnRateAnim, COND_SkipOwner);
	DOREPLIFETIME(ACatBase, bIsGrabbing);
	DOREPLIFETIME(ACatBase, bIsSprinting);
}

void ACatBase::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	ForceWalkingMovementMode();
}

void ACatBase::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	ForceWalkingMovementMode();

	// Re-add mapping context — BeginPlay may have run before PlayerState replicated.
	if (const APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}
}

void ACatBase::ForceWalkingMovementMode()
{
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		if (CMC->MovementMode == MOVE_None)
		{
			CMC->SetMovementMode(MOVE_Walking);
		}
	}
}

// ── OnRep Stubs ─────────────────────────────────────────────────────────
void ACatBase::OnRep_SpeedType()      {}
void ACatBase::OnRep_MovementStage()  {}
void ACatBase::OnRep_JumpPhase()
{
	OnJumpPhaseChanged.Broadcast(JumpPhase);
}

// ══════════════════════════════════════════════════════════════════════════
// ── UpdateAnimationStates ───────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::UpdateAnimationStates()
{
	const UCharacterMovementComponent* CMC = GetCharacterMovement();
	if (!CMC)
	{
		return;
	}

	// (a) Speed — 2D velocity magnitude (XY only, matching CharBP_Base)
	FVector Velocity2D = GetVelocity();
	Velocity2D.Z = 0.0f;
	Speed = Velocity2D.Size();

	// (b) HasMovementInput — derived from acceleration
	bHasMovementInput = CMC->GetCurrentAcceleration().SizeSquared() > KINDA_SMALL_NUMBER;

	// (c) IsOnGround
	bIsOnGround = CMC->IsMovingOnGround();

	// (d) IsFalling
	bIsFalling = CMC->IsFalling();

	// (e) MovementStage
	if (CMC->MovementMode == MOVE_Swimming)
	{
		MovementStage = ECatMovementStage::Swimming;
	}
	else if (bIsOnGround)
	{
		MovementStage = ECatMovementStage::OnGround;
	}
	else
	{
		MovementStage = ECatMovementStage::InAir;
	}

	// (e2) Jump phase — tick-driven phase transitions
	UpdateJumpPhase(DeltaTimeCached);

	// (f) SpeedType — threshold chain on ABSOLUTE speed (M1 gait pass). With sprint,
	// MaxWalkSpeed is gait-dependent, so normalizing by it would reclassify a full
	// trot (400/400 = 1.0) as Run. Absolute thresholds derived from the two gait
	// speeds agree on every machine regardless of the local CMC state:
	// Run is only reachable while sprinting; a full plain-W trot stays Trot.
	const float RunSpeedThreshold  = SprintMaxWalkSpeed * 0.8f;     // 520 at defaults
	const float TrotSpeedThreshold = MovementMaxWalkSpeed * 0.6f;   // 240 at defaults

	if (Speed >= RunSpeedThreshold)
	{
		SpeedType = ECatMoveType::Run;
	}
	else if (Speed >= TrotSpeedThreshold)
	{
		SpeedType = ECatMoveType::Trot;
	}
	else if (bHasMovementInput)
	{
		SpeedType = ECatMoveType::Walk;
	}
	else
	{
		SpeedType = ECatMoveType::Idle;
	}

	// (f2)–(f4): Aim yaw & turn detection — local only.
	// Simulated proxies and server copies of client pawns have no valid
	// ControlRotation; these values would be garbage and trigger false
	// Turn states / ghost rotation.
	if (IsLocallyControlled())
	{
		// (f2) AimYaw / AimPitch — only valid with a local controller
		AimYaw = FRotator::NormalizeAxis(GetControlRotation().Yaw - GetActorRotation().Yaw);
		AimYawClamped = FMath::Clamp(AimYaw, -90.0f, 90.0f);

		AimPitch = FRotator::NormalizeAxis(GetControlRotation().Pitch);
		AimPitchClamped = FMath::Clamp(AimPitch, -90.0f, 90.0f);

		// (f2b) Moving pivot — at speed, a hard sustained steer plants the cat and turns
		// it with footwork (sets SpeedType=Turn + PivotTurnRateTarget). Runs BEFORE
		// UpdateTurnInPlace: its SpeedType write gates turn-in-place off, and the shared
		// interp/RPC block there carries its footwork + yaw to the other machines.
		UpdateMovingPivot();

		// (f2c) Weighty stop — input release at speed swaps to constant-deceleration
		// braking for a real run-out + plant. Runs AFTER the pivot: a pivot supersedes,
		// and its input suppression must not read as a release here.
		UpdateWeightyStop();

		// (f2d) Weighty start — a fresh sprint start from near-standstill coils then
		// bursts (launch-step); also arms the M4 fresh-input ramp for plain starts.
		UpdateWeightyStart();

		// (f3) Turn-In-Place — when idle, procedurally rotate the body toward the camera
		// and drive the BS1_Cat_Turn footwork (sets SpeedType=Turn, bGoTurn, TurnRateAnim).
		UpdateTurnInPlace();

		UE_LOG(LogCatVentures, Verbose, TEXT("[%s] AimYaw: %.1f | TurningInPlace: %d"),
			*GetName(), AimYaw, bIsTurningInPlace);
	}
	else
	{
		// ── Non-local (server copy of client pawn + all simulated proxies) ──
		// bGoTurn is the authoritative replicated signal. Force SpeedType = Turn
		// regardless of local velocity noise — proxy Speed can flicker above the
		// Walk threshold (40 cm/s) due to network micro-corrections, which would
		// otherwise override the turn state every other frame causing animation pops.
		if (bGoTurn && MovementStage == ECatMovementStage::OnGround)
		{
			SpeedType = ECatMoveType::Turn;
		}

		// Smooth pursuit of the owning client's RPC'd turn yaw (server copies only —
		// bHasClientTurnTarget is set exclusively by the Server RPCs, so this is inert on
		// simulated proxies, whose rotation arrives via movement replication instead).
		// EXPONENTIAL smoothing, deliberately NOT a fixed-rate chase: a pursuit faster
		// than the real 150°/s catches each ~5° RPC step and stalls until the next one
		// (move-stop-move = the jitter in the 2026-07-06 MP retest). Velocity proportional
		// to remaining error stays continuous; the few degrees of steady-state lag are
		// invisible on a remote cat, and the reliable final yaw closes it at turn end.
		if (bHasClientTurnTarget)
		{
			constexpr float PursuitInterpSpeed = 12.0f;
			const float CurYaw = GetActorRotation().Yaw;
			const float ErrDeg = FRotator::NormalizeAxis(ClientTurnTargetYaw - CurYaw);
			const float NewYaw = CurYaw + ErrDeg * FMath::Clamp(PursuitInterpSpeed * DeltaTimeCached, 0.0f, 1.0f);
			SetActorRotation(FRotator(0.0f, NewYaw, 0.0f));
			if (!bGoTurn && FMath::Abs(FRotator::NormalizeAxis(NewYaw - ClientTurnTargetYaw)) < 0.1f)
			{
				bHasClientTurnTarget = false;
			}
		}
	}

	UE_LOG(LogCatVentures, Verbose, TEXT("[%s] Tick — Speed: %.1f | Sprint: %d | SpeedType: %d | HasInput: %d | OnGround: %d"),
		*GetName(), Speed, (int32)bIsSprinting, (int32)SpeedType, bHasMovementInput, bIsOnGround);
}

// ══════════════════════════════════════════════════════════════════════════
// ── UpdateCosmeticInterpolation ───────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::UpdateCosmeticInterpolation(float DeltaTime)
{
	// ── (A) Breath ────────────────────────────────────────────────────
	// Idle breathing: strongest when standing still, eased out as the cat
	// gains speed (the locomotion anims already carry their own body motion).
	// (Was previously panting-after-exertion gated on TimeInRun, which left
	//  the idle pose frozen — TimeInRun is now unused; drop it on the next
	//  full rebuild rather than churn reflection data during live iteration.)
	AlphaPlayBreath = FMath::GetMappedRangeValueClamped(
		FVector2D(10.0f, 200.0f), FVector2D(1.0f, 0.0f), Speed);
	AlphaPlayBreathInterp = FMath::FInterpTo(AlphaPlayBreathInterp, AlphaPlayBreath, DeltaTime, 4.0f);

	// ── (B) Aim Interp ────────────────────────────────────────────────
	AlphaAim = FMath::GetMappedRangeValueClamped(FVector2D(0.0f, 800.0f), FVector2D(1.0f, 0.0f), Speed);
	AlphaAimInterp = FMath::FInterpTo(AlphaAimInterp, AlphaAim, DeltaTime, 2.0f);
	AimYawInterp = FMath::FInterpTo(AimYawInterp, AimYawClamped, DeltaTime, 5.0f);
	AimPitchInterp = FMath::FInterpTo(AimPitchInterp, AimPitchClamped, DeltaTime, 5.0f);

	// BS1_Cat_Aim inputs (kit AnimBP wiring: the aim clips bake the yaw sweep into their
	// timeline, so yaw scrubs NormalizedTime while pitch is the blend axis). Kit ranges:
	// yaw-max 180° → 0..1 (0.5 centered), pitch-max 60° → −1..+1. Non-local pawns keep
	// AimYaw/PitchInterp at 0 (no valid ControlRotation), so proxies hold a neutral head.
	AimBSYawTime = FMath::GetMappedRangeValueClamped(
		FVector2D(-180.0f, 180.0f), FVector2D(0.0f, 1.0f), AimYawInterp);
	AimBSPitch = FMath::GetMappedRangeValueClamped(
		FVector2D(-60.0f, 60.0f), FVector2D(-1.0f, 1.0f), AimPitchInterp);

	// ── (B1b) Jump rise scrub ─────────────────────────────────────────
	// 0→1 over the EXPECTED RISE TIME while in Launch; the ABP's Jump_Launch evaluators
	// scrub each clip's rise portion with it. Runs on every non-dedicated machine.
	// TIME-based, deliberately NOT height-based (2026-07-06): a height scrub is physically
	// exact but unreadable — the variable-height hold front-loads ~half the height into
	// 0.18 s, so the push-off frames flashed by as a "little leg jut" and the clip parked
	// in its airborne tuck (read as instant falling — Sean). Motion needs uniform screen
	// time; taps simply exit the state before the clip finishes and blend to Apex.
	{
		// 0..0.425 = anticipation coil (clip share of the crouch: 0.51/1.2), 0.425..1 = rise.
		constexpr float CoilShare = 0.425f;
		if (JumpPhase == ECatJumpPhase::Launch)
		{
			if (!bJumpRiseTracking)
			{
				JumpRiseStartZ = GetActorLocation().Z;   // kept for the per-jump rise log
				bJumpRiseTracking = true;
			}
			constexpr float ExpectedRiseTime = 0.53f;    // measured full-hold launch duration
			JumpRiseProgress = CoilShare
				+ (1.0f - CoilShare) * FMath::Clamp(JumpAirTime / ExpectedRiseTime, 0.0f, 1.0f);
		}
		else if (JumpPhase == ECatJumpPhase::None && JumpAnticipationTimer > 0.0f)
		{
			bJumpRiseTracking = false;
			JumpRiseProgress = CoilShare
				* (1.0f - JumpAnticipationTimer / FMath::Max(JumpAnticipationDuration, 0.01f));
		}
		else
		{
			if (bJumpRiseTracking && JumpPhase == ECatJumpPhase::Apex)
			{
				// One line per jump at the Launch→Apex hand-off — Sean reads feel, this
				// reads the numbers behind it (rise height/time and how much of the clip
				// scrub was consumed) from the log after a PIE session.
				UE_LOG(LogCatVentures, Log, TEXT("[%s] JumpRise: %.1f cm in %.2f s | scrub progress at apex %.2f"),
					*GetName(), GetActorLocation().Z - JumpRiseStartZ, JumpAirTime, JumpRiseProgress);
			}
			bJumpRiseTracking = false;
			// Hold 1 through Apex/Fall (the Launch state may still be blending out); reset
			// once grounded so the next hop starts from the clip's rise start.
			if (JumpPhase == ECatJumpPhase::None || JumpPhase == ECatJumpPhase::Land)
			{
				JumpRiseProgress = 0.0f;
			}
			else
			{
				JumpRiseProgress = 1.0f;
			}
		}
	}

	// ── (B2) Additive idle life — blink & ear-twitch pulses ──────────
	// Kit cadence (CharBP_Base "Idles & Add Anim" config, verified 2026-07-06): blink every
	// 3–7 s, ear twitch every 6–12 s, three equal-chance ear clips never repeated back-to-back.
	// Each machine rolls its own randomness — cosmetic divergence between clients is fine
	// (same policy as Geometry Collection debris).
	{
		constexpr float BlinkIntervalMin = 3.0f, BlinkIntervalMax = 7.0f;
		constexpr float EarsIntervalMin  = 6.0f, EarsIntervalMax  = 12.0f;
		constexpr int32 NumEarsClips = 3;
		// Must outlive one anim update but end before the SM's auto-return transition
		// finishes (shortest ears clip is 0.6 s), or the state would immediately retrigger.
		constexpr float PulseDuration = 0.2f;

		if (!bAdditiveTimersSeeded)
		{
			BlinkCountdown = FMath::FRandRange(0.0f, BlinkIntervalMax);
			EarsCountdown  = FMath::FRandRange(0.0f, EarsIntervalMax);
			bAdditiveTimersSeeded = true;
		}

		if (BlinkPulseRemaining > 0.0f)
		{
			BlinkPulseRemaining -= DeltaTime;
			if (BlinkPulseRemaining <= 0.0f)
			{
				bPlayBlink = false;
			}
		}
		else if ((BlinkCountdown -= DeltaTime) <= 0.0f)
		{
			bPlayBlink = true;
			BlinkPulseRemaining = PulseDuration;
			BlinkCountdown = FMath::FRandRange(BlinkIntervalMin, BlinkIntervalMax);
		}

		if (EarsPulseRemaining > 0.0f)
		{
			EarsPulseRemaining -= DeltaTime;
			if (EarsPulseRemaining <= 0.0f)
			{
				bPlayEarsTwitch = false;
			}
		}
		else if ((EarsCountdown -= DeltaTime) <= 0.0f)
		{
			// Pick a different clip than last time (kit "Get Random Index without repetitions").
			IndexEars = (IndexEars + FMath::RandRange(1, NumEarsClips - 1)) % NumEarsClips;
			bPlayEarsTwitch = true;
			EarsPulseRemaining = PulseDuration;
			EarsCountdown = FMath::FRandRange(EarsIntervalMin, EarsIntervalMax);
		}
	}

	// ── (C) PlayRate Interp ───────────────────────────────────────────
	const float OutputYAbs = (GetCharacterMovement() && GetCharacterMovement()->MaxWalkSpeed > KINDA_SMALL_NUMBER)
		? FMath::Clamp(Speed / GetCharacterMovement()->MaxWalkSpeed, 0.0f, 1.0f)
		: 0.0f;

	const float PlayRateInterpSpeed = FMath::GetMappedRangeValueClamped(
		FVector2D(0.0f, 1.0f), FVector2D(5.0f, 0.5f), OutputYAbs);
	PlayRateInterp = FMath::FInterpTo(PlayRateInterp, PlayRate, DeltaTime, PlayRateInterpSpeed);

	// ── (D) Locomotion Lean ──────────────────────────────────────────
	// Banked turn lean from the SIGNED ANGLE between where the cat is moving
	// (velocity) and where the player is steering (input acceleration). Unlike a
	// yaw-RATE signal, this persists through the whole turn — when you tap A/D the
	// input snaps to the new direction while the body catches up, so a sustained
	// angle (and thus a sustained, blended lean) holds until velocity realigns.
	// Positive = steering right of current motion. Zeroed during Turn/Idle.
	{
		FVector Vel = GetVelocity();
		FVector InputAccel = GetCharacterMovement()
			? GetCharacterMovement()->GetCurrentAcceleration() : FVector::ZeroVector;
		Vel.Z = 0.0f; InputAccel.Z = 0.0f;

		float RawLean = 0.0f;
		if (Vel.SizeSquared() > 100.0f && InputAccel.SizeSquared() > 1.0f)
		{
			const FVector VelDir   = Vel.GetSafeNormal();
			const FVector InputDir = InputAccel.GetSafeNormal();
			const float Dot   = FVector::DotProduct(VelDir, InputDir);
			const float SignZ = FVector::CrossProduct(VelDir, InputDir).Z;   // + = input right of motion
			const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.0f, 1.0f)))
				* FMath::Sign(SignZ);
			// 60° of steering offset maps to full lean (±1)
			RawLean = FMath::GetMappedRangeValueClamped(FVector2D(-60.0f, 60.0f), FVector2D(-1.0f, 1.0f), AngleDeg);
		}

		// Gate: only lean while actually moving, never during Turn or Idle
		const bool bShouldLean = (Speed > 10.0f)
			&& (SpeedType != ECatMoveType::Turn)
			&& (SpeedType != ECatMoveType::Idle);
		const float TargetLean = bShouldLean ? RawLean : 0.0f;
		// Fast attack while steering, slower decay to bleed the bank out smoothly.
		const float LeanInterpSpeed = bShouldLean ? 8.0f : 3.0f;
		LeanAmount = FMath::FInterpTo(LeanAmount, TargetLean, DeltaTime, LeanInterpSpeed);

		UE_LOG(LogCatVentures, Verbose, TEXT("[%s] Lean -- Raw: %.3f | Final: %.3f | Gate: %d"),
			*GetName(), RawLean, LeanAmount, bShouldLean);
	}

	// ── (E) Longitudinal weight lean (accel / decel) ──────────────────
	// The body pitches into acceleration and braces back on deceleration. Driven by a
	// spring-damper (not a plain interp) so a hard stop overshoots past neutral and settles —
	// that overshoot is what reads as mass. Target is the signed change in speed, normalized
	// by the reference acceleration. Steady cruising -> accel ~0 -> lean settles to 0.
	{
		const float SafeDT = FMath::Max(DeltaTime, 0.001f);
		const float Accel = (Speed - PreviousLeanSpeed) / SafeDT;   // cm/s^2, signed
		PreviousLeanSpeed = Speed;

		const float Ref = (AccelLeanReference > KINDA_SMALL_NUMBER)
			? AccelLeanReference
			: (GetCharacterMovement() ? FMath::Max(GetCharacterMovement()->MaxAcceleration, 1.0f) : 1.0f);
		const float Target = FMath::Clamp(Accel / Ref, -1.0f, 1.0f);

		// Semi-implicit spring-damper toward Target. The integration step is CLAMPED
		// (2026-07-06 MP pass): a frame hitch making Damping×dt exceed 1 flips the
		// velocity's sign and grows it — the spring explodes, slams into the ±1.5 rail,
		// and the old clamp kept the position but not the velocity, parking the lean at
		// full bow forever (the "stuck landing pose" that survived every slope fix;
		// AccelLeanAmount read ±1.5 standing still on both machines). Rails now also
		// zero the velocity (anti-windup).
		const float SpringDT = FMath::Min(SafeDT, 1.0f / 30.0f);
		AccelLeanVelocity += (Target - AccelLeanAmount) * AccelLeanStiffness * SpringDT;
		AccelLeanVelocity -= AccelLeanVelocity * FMath::Min(AccelLeanDamping * SpringDT, 0.9f);
		AccelLeanAmount   += AccelLeanVelocity * SpringDT;
		if (FMath::Abs(AccelLeanAmount) >= 1.5f)
		{
			AccelLeanAmount   = FMath::Clamp(AccelLeanAmount, -1.5f, 1.5f);
			AccelLeanVelocity = 0.0f;
		}

		UE_LOG(LogCatVentures, Verbose, TEXT("[%s] AccelLean -- Accel: %.0f | Target: %.3f | Lean: %.3f"),
			*GetName(), Accel, Target, AccelLeanAmount);
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── UpdateLandCushion ─────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Damped vertical spring on the mesh's relative Z (AnimX pass Step 4 — the code
// replacement for the kit's HeightFixer physics rig). Landed() injects a downward
// velocity scaled by impact speed; this integrates the spring back to rest. The dip
// pushes the paws into the ground, and the upward-only foot IK (which runs right
// after this, same frame) snap-lifts them onto the surface — so the BODY dips while
// the PAWS stay planted and the legs visibly compress like springs taking the weight.
// Cosmetic and local: runs on every non-dedicated machine, never replicated.
void ACatBase::UpdateLandCushion(float DeltaTime)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp) return;

	// ── (A) Landing impulse spring ────────────────────────────────────
	if (!bEnableLandCushion
		|| (FMath::Abs(MeshCushionOffset) < 0.01f && FMath::Abs(MeshCushionVelocity) < 0.1f))
	{
		MeshCushionOffset = MeshCushionVelocity = 0.0f;
	}
	else
	{
		// Semi-implicit damped spring toward offset 0. Integration step clamped like the
		// accel-lean spring (2026-07-06): at a frame-hitch dt, ω²·dt explodes the update.
		const float CushionDT = FMath::Min(DeltaTime, 1.0f / 30.0f);
		const float W = LandCushionFrequency;
		const float Accel = (-W * W * MeshCushionOffset) - (2.0f * LandCushionDampingRatio * W * MeshCushionVelocity);
		MeshCushionVelocity += Accel * CushionDT;
		MeshCushionOffset = FMath::Clamp(MeshCushionOffset + MeshCushionVelocity * CushionDT,
		                                 -LandCushionMaxDip, LandCushionMaxDip * 0.25f);
	}

	// ── (B) Continuous slope ground-conform (kit HeightFixer role, 2026-07-06) ──
	// On an incline the capsule touches the slope uphill of its center, leaving the mesh
	// hanging above the surface directly below — the "cat floats on ramps" gap. Trace
	// under the capsule bottom and ease the mesh down onto the surface (drop only; on
	// flat ground the gap is ~0 and this stays inert). Runs before UpdateFootIK so the
	// paw solve and the spine control points see the conformed body height this frame.
	{
		constexpr float ConformMaxDrop = 10.0f;
		constexpr float ConformInterpSpeed = 8.0f;
		float ConformTarget = 0.0f;
		const UCapsuleComponent* Capsule = GetCapsuleComponent();
		if (bIsOnGround && Capsule && GetWorld())
		{
			const float CapsuleBottomZ = GetActorLocation().Z - Capsule->GetScaledCapsuleHalfHeight();
			const FVector Center = GetActorLocation();
			FHitResult Hit;
			FCollisionQueryParams Params(FName(TEXT("CatGroundConform")), false, this);
			if (GetWorld()->LineTraceSingleByChannel(Hit,
				FVector(Center.X, Center.Y, CapsuleBottomZ + 20.0f),
				FVector(Center.X, Center.Y, CapsuleBottomZ - ConformMaxDrop - 20.0f),
				ECC_Visibility, Params))
			{
				// Discontinuity rejection (2026-07-06): a gap LARGER than the conformable
				// range means a ledge/edge under the capsule center, not a slope — clamping
				// it sank the cat 10 cm into every platform lip. Conform only within range.
				const float Delta = Hit.ImpactPoint.Z - CapsuleBottomZ;
				ConformTarget = (Delta >= -ConformMaxDrop) ? FMath::Min(Delta, 0.0f) : 0.0f;
			}
		}
		MeshGroundConformZ = FMath::FInterpTo(MeshGroundConformZ, ConformTarget, DeltaTime, ConformInterpSpeed);
	}

	// ── (B3) Wall-hug conform (2026-07-30) — the §B role rotated 90°. While attached to
	// a wall (cling/scramble) or mantling, the capsule sits wherever the attach caught it
	// (the gap varies per catch) and the skeleton discards body translation, so the wall
	// clips render a fixed paw plane that floats off the surface. Trace the wall along
	// actor forward and slide the MESH so the paw plane meets it. Trace-miss decays to 0,
	// which self-resolves the mantle top-out (the capsule center rises past the lip).
	{
		float HugTarget = 0.0f;
		// During a mantle the hug only serves the wall-face half; releasing it there
		// (masked by the fast pull) instead of during the calm walk-out killed the
		// post-mantle backward mesh glide (Sean's "jitter after topping out").
		const bool bMantleHugPhase = bGoMantle && MantleProgress < 0.5f;
		// The climb clip's chest/head already ride AT the wall plane, so every uu of
		// mantle-phase hug is a uu of face interpenetration ("whole front of the cat
		// clips through the ledge") — cap it hard; the drop tables own paw contact.
		constexpr float MantleHugCap = 5.0f;
		if (bEnableWallHug && (bGoWallAttach || bMantleHugPhase) && GetWorld())
		{
			const FVector Start = GetActorLocation();
			const FVector Fwd   = GetActorForwardVector();
			FHitResult Hit;
			FCollisionQueryParams Params(FName(TEXT("CatWallHug")), false, this);
			if (GetWorld()->LineTraceSingleByChannel(Hit, Start,
					Start + Fwd * (WallHugMaxSlide + 30.0f), ECC_Visibility, Params)
				&& !Hit.bStartPenetrating          // the 07-21 start-penetrating trap
				&& Hit.ImpactNormal.Z < 0.5f)      // steeper than ~60° = a wall, not floor
			{
				HugTarget = FMath::Clamp(Hit.Distance - WallHugSurfaceBias - WallHugPawPlane,
				                         0.0f, bMantleHugPhase ? MantleHugCap : WallHugMaxSlide);
			}
		}
		// Corner-flap guard: during a mantle the wall distance is fixed by the takeover,
		// so LATCH the first computed hug and hold it — at a face's lateral edge the live
		// trace alternately hits and misses across the corner as the body moves, and the
		// resulting 0↔target flapping WAS the "jittery" corner mantle (2026-07-31).
		if (bMantleHugPhase)
		{
			if (MantleHugLatch < 0.0f)
			{
				MantleHugLatch = HugTarget;
			}
			HugTarget = MantleHugLatch;
		}
		else if (!bGoMantle)
		{
			MantleHugLatch = -1.0f;
		}
		// Downward hug moves during a mantle ease at HALF speed: the cling→mantle handoff
		// sheds the cling's full hug (~14) down to the mantle cap (5), and at full interp
		// that 9 uu pull-back reads as a hitch right at the takeover start (2026-07-31).
		const float HugInterp = (bGoMantle && HugTarget < WallHugForward)
			? WallHugInterpSpeed * 0.5f : WallHugInterpSpeed;
		WallHugForward = FMath::FInterpTo(WallHugForward, HugTarget, DeltaTime, HugInterp);
		if (HugTarget == 0.0f && FMath::Abs(WallHugForward) < 0.01f)
		{
			WallHugForward = 0.0f;
		}
	}

	// ── (B2) Turn stance drop — ease the body down during turn footwork so the kit
	// turn clips' non-planting outside forepaw reaches contact (see TurnStanceDrop).
	// Asymmetric: engage must beat the Turn-state blend-in; release stays gentle.
	{
		// Pivot crouch drop (cm) by PivotProgress — the authored crouch envelope of the
		// Pivot90/180 clips (drop = 0.26 + 8.84 × envelope(progress); the clips' spine-
		// translation base is flattened in Blender so this analytic curve is exact for
		// BOTH clip lengths, verified 2026-07-22 within 0.15 cm). The clips' Spine
		// translation is discarded by the skeleton's all-SKELETON translation
		// retargeting, so the crouch is restored here procedurally; the clip legs were
		// IK-solved against the crouched hip, so drop + rotation-only pose reproduces
		// the authored pose exactly. Runs on every machine (bGoPivot and PivotProgress
		// both replicate COND_SkipOwner).
		static constexpr float PivotCrouchDrop[11] =
			{ 0.26f, 1.59f, 2.91f, 3.58f, 3.91f, 4.24f, 4.57f, 5.24f, 6.09f, 6.93f, 7.78f };

		// Skid crouch drop (cm) by SkidProgress — the authored brace envelope of the skid-stop
		// clip (rise into the plant → hold through the slide → release into the settle);
		// same retargeting doctrine as the pivot table above. Peak 7.8 matches the Land_run
		// source brace — shallower left the pitched pelvis's hind sockets beyond leg reach.
		static constexpr float SkidCrouchDrop[11] =
			{ 0.00f, 4.59f, 7.42f, 7.80f, 7.80f, 7.80f, 7.80f, 7.80f, 5.46f, 3.12f, 0.78f };

		constexpr float TurnDropReleaseSpeed = 8.0f;
		// While a scrub owns the drop, track the curve near-directly (it is already smooth and
		// the clip legs are solved for exactly this value each frame — the engage-speed interp
		// lagged it by ~1–1.5 cm on fast scrubs, floating all four paws, 2026-07-22 PawPrint
		// round). The high rate still smooths the enter edge; release keeps the gentle ease.
		constexpr float CurveDropTrackSpeed = 60.0f;
		auto SampleDropTable = [](const float (&Table)[11], float Progress)
		{
			const float P = FMath::Clamp(Progress, 0.0f, 1.0f) * 10.0f;
			const int32 Seg = FMath::Min(FMath::FloorToInt32(P), 9);
			return FMath::Lerp(Table[Seg], Table[Seg + 1], P - Seg);
		};
		bool bCurveDrop = false;
		float DropTarget = 0.0f;
		// The mantle drop releases through the GENTLE interp for its last 10% (and the
		// post-exit freeze): tracking the tables' steep settle tail exactly popped the
		// body up ~7.5 uu in 2–3 frames at the exit (TurnDropZ −7.66 → −0.13 in 0.1 s,
		// PawPrint 2026-07-31 — the residual end-jitter on running-jump mantles).
		if (bGoMantle && MantleCrouchScale > 0.0f && MantleProgress < 0.9f)
		{
			// Mantle deep-hang (2026-07-30) — the ONLY airborne curve drop, so it sits above
			// the grounded chain. The source Ledge_Climb_Up pelvis translation dives from
			// stance 25.8 to 3.9 uu mid-pull (the body hangs low against the wall face while
			// the root track rises past it) and is discarded by the skeleton's all-SKELETON
			// translation retargeting — without this valley the tucked hind rotations render
			// as legs sticking forward at body height (the kangaroo, frame-verified vs the
			// pack showcase). Sampled from the source IP pelvis at 11 points, scaled to our
			// pelvis height (30.2/25.8); the capsule curve carries the root track, this
			// carries the pelvis-local remainder — together they reproduce the authored
			// world trajectory exactly.
			// CONTACT-CALIBRATED 2026-07-30 (round 11): the raw source-pelvis sampling left
			// every paw 5–8.5 uu off both surfaces through progress 0.46–0.69 (Sean's
			// frame-37 all-airborne catch) — our legs are proportionally shorter than the
			// pelvis-height ratio assumes, so the body must ride DEEPER than the authored
			// dive for the paws to reach contact. Each entry = source-pelvis sample + the
			// measured shortfall of the lowest reachable paw vs a 1.5 uu contact target
			// (the TurnStanceDrop philosophy). Verified: min paw-surface distance ≤3.4 uu
			// across the whole mantle, matching the source's own contact profile.
			// RE-DERIVED 2026-07-31 (round 18): raw source-pelvis samples + ONE smooth
			// contact hump (amp 3.5, smoothstep 0.25–0.45 in / 0.85–1.0 out) instead of
			// per-knot exact corrections — the jagged correction envelope rode on top of
			// the smooth authored pelvis path and pumped the paws ("legs flapping",
			// measured 2–3× the authored paw wiggle; smoothing recovers ~15–20%).
			static constexpr float MantleCrouchDrop[11] =
				{ 0.00f, 3.18f, 13.61f, 23.57f, 28.39f, 23.42f, 16.95f, 10.52f, 6.13f, 3.33f, 0.00f };
			// Vault bucket (Ledge_050M): crouch-load coil, hop, landing gather — same
			// contact calibration (its gather window floated up to 8.7 uu uncorrected).
			// RE-DERIVED 2026-07-31 (round 21, the round-18 recipe): the source's
			// authored pelvis dive is load-peak 19.6 → hop release to 0 → a GENTLE
			// landing plateau of ~6.6 — but the per-knot contact calibration had
			// turned that plateau into a 16.4 spike arriving in 170 ms (2.5× the
			// authored motion), pumping the body through every vault landing (the
			// "jitter on all mantles" once vault-for-all shipped); its sharp 0-valley
			// also made Catmull overshoot NEGATIVE (a mesh pop-UP mid-hop). Now the
			// authored shape + smooth contact humps (load +2.5, gather +7 peaked
			// p0.65). Contact cost vs the calibrated knots ≤ ~4 uu, gather only.
			static constexpr float VaultCrouchDrop[11] =
				{ 0.00f, 13.50f, 22.00f, 4.00f, 0.50f, 5.50f, 14.00f, 13.00f, 10.00f, 5.00f, 0.00f };
			// Height-aware: each table is contact-calibrated at ONE height (climb 68,
			// vault 40); scale toward the actual ledge so bucket-edge heights stay
			// planted (ledge 47 played the 68-calibrated climb → all paws >25 uu off
			// every surface for half the mantle — MantlePawGapMin's first catch).
			const float CalibHeight = bMantleVault ? 40.0f : 68.0f;
			const float HeightScale = (MantleLedgeHeight > 1.0f)
				? FMath::Clamp(MantleLedgeHeight / CalibHeight, 0.8f, 1.1f) : 1.0f;
			// Catmull-Rom through the knots (mantle only): linear segments put a velocity
			// kink every 85 ms and the smooth clip pose pumps over each one.
			auto SampleSmooth = [](const float (&Table)[11], float Progress)
			{
				const float s = FMath::Clamp(Progress, 0.0f, 1.0f) * 10.0f;
				const int32 i = FMath::Min(FMath::FloorToInt32(s), 9);
				const float u = s - i;
				const float P0 = Table[FMath::Max(i - 1, 0)], P1 = Table[i];
				const float P2 = Table[i + 1], P3 = Table[FMath::Min(i + 2, 10)];
				return 0.5f * ((2.f*P1) + (-P0+P2)*u + (2.f*P0-5.f*P1+4.f*P2-P3)*u*u + (-P0+3.f*P1-3.f*P2+P3)*u*u*u);
			};
			DropTarget = -SampleSmooth(bMantleVault ? VaultCrouchDrop : MantleCrouchDrop,
			                           MantleProgress) * MantleCrouchScale * HeightScale;
			bCurveDrop = true;
		}
		else if (bIsOnGround)
		{
			if (bGoPivot && PivotCrouchScale > 0.0f)
			{
				DropTarget = -SampleDropTable(PivotCrouchDrop, PivotProgress) * PivotCrouchScale;
				bCurveDrop = true;
			}
			else if (bGoSkid && SkidCrouchScale > 0.0f)
			{
				DropTarget = -SampleDropTable(SkidCrouchDrop, SkidProgress) * SkidCrouchScale;
				bCurveDrop = true;
			}
			else if (bGoStartStep && StartCrouchScale > 0.0f)
			{
				// Start-step coil load: rise into the loaded coil by the clip's coil
				// portion, HELD through the push (the extension pose needs a low pelvis
				// with planted hinds), released at the stride handoff. Matches the
				// authored envelope exactly.
				static constexpr float StartCrouchDrop[11] =
					{ 0.00f, 1.88f, 3.75f, 4.50f, 4.20f, 3.80f, 3.20f, 2.00f, 0.80f, 0.15f, 0.00f };
				DropTarget = -SampleDropTable(StartCrouchDrop, StartProgress) * StartCrouchScale;
				bCurveDrop = true;
			}
			else if (SpeedType == ECatMoveType::Turn)
			{
				DropTarget = -TurnStanceDrop;
			}
		}
		// Sustained curve mode tracks EXACTLY — the clip legs are solved for the table
		// value at this frame's progress, so any lag is a paw float/penetration (the
		// speed-60 interp still lagged 0.3–0.95 cm on the skid's fast rise, PawPrint
		// 2026-07-22 evening round). Both tables start ≈0 at progress 0, so the first
		// curve frame has no edge to smooth; the interp covers enter/exit only.
		if (bCurveDrop && bCurveDropWasActive)
		{
			TurnStanceDropZ = DropTarget;
		}
		else
		{
			const float InterpSpeed = bCurveDrop ? CurveDropTrackSpeed
				: (DropTarget < TurnStanceDropZ) ? TurnStanceDropEngageSpeed : TurnDropReleaseSpeed;
			TurnStanceDropZ = FMath::FInterpTo(TurnStanceDropZ, DropTarget, DeltaTime, InterpSpeed);
		}
		bCurveDropWasActive = bCurveDrop;
	}

	// ── (C) Whole-body slope pitch — APPLY only ──────────────────────
	// MeshSlopePitch is COMPUTED in UpdateFootIK from the paw-floor traces (single source
	// of truth with the foot conform; runs after this function — 1-frame lag, invisible).
	// It originally had its own ±30 uu fore/aft probes here: standing at the ramp's BASE,
	// one probe caught the lip while the paws stood on flat ground — a persistent false
	// ~19° reading under the 25° rejection threshold, bowing the cat on level ground
	// (Sean's "stuck downward pose", snapshot-confirmed 2026-07-06: SpineInclineF 0.978
	// vs applied pitch 19.55). Paw-floor-derived pitch cannot disagree with the stance.

	// Single transform write; skip once fully at rest (avoids per-tick transform writes).
	const float TotalOffset = MeshCushionOffset + MeshGroundConformZ + TurnStanceDropZ;
	FVector Rel = MeshComp->GetRelativeLocation();
	if (FMath::Abs(TotalOffset) < 0.01f && FMath::Abs(MeshSlopePitch) < 0.05f
		&& FMath::Abs(WallHugForward) < 0.01f
		&& FMath::IsNearlyEqual(Rel.Z, MeshCushionBaseZ, 0.01f)
		&& FMath::IsNearlyEqual(Rel.X, MeshBaseRelLocX, 0.01f))
	{
		return;
	}
	Rel.Z = MeshCushionBaseZ + TotalOffset;
	Rel.X = MeshBaseRelLocX + WallHugForward;   // wall-hug: actor-forward slide toward the wall
	// Slope pitch is about the ACTOR's lateral axis — compose it in parent space ahead of
	// the rig's authored relative rotation (the −90° yaw), then write both in one call.
	const FQuat RelQuat = FQuat(FRotator(MeshSlopePitch, 0.0f, 0.0f)) * FQuat(MeshBaseRelRot);
	MeshComp->SetRelativeLocationAndRotation(Rel, RelQuat.Rotator());
}

// ══════════════════════════════════════════════════════════════════════════
// ── UpdateCameraWeight ────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// M3 of the weighty-movement pass: per-gait spring-arm lag, landing dip, sprint
// FOV push, stop-settle. Runs only for the locally controlled cat (it owns the
// rendered camera) — purely cosmetic, nothing here replicates.
void ACatBase::UpdateCameraWeight(float DeltaTime)
{
	if (!CameraBoom || !FollowCamera) return;

	if (!bEnableCameraWeight)
	{
		// Kill any live dip so toggling the switch off can't strand the camera low.
		if (CamDipOffset != 0.0f || CamDipVelocity != 0.0f)
		{
			CamDipOffset = CamDipVelocity = 0.0f;
			FVector Socket = CameraBoom->SocketOffset;
			Socket.Z = CamBaseSocketOffsetZ;
			CameraBoom->SocketOffset = Socket;
		}
		return;
	}

	// ── (A) Per-gait lag — blended by ACTUAL speed, not sprint intent ──
	// The blend RISES with raw speed (the CMC accel already ramps that over
	// ~0.5 s) but RELAXES back through an interp: braking collapses Speed in a
	// few frames, and an instantaneous blend snaps the lag speeds back to base
	// mid-catch-up — the camera lurches onto the stopped cat (2026-07-14 PIE).
	const FVector2D GaitSpeedRange(MovementMaxWalkSpeed, SprintMaxWalkSpeed);
	const float SprintAlphaRaw = FMath::GetMappedRangeValueClamped(GaitSpeedRange, FVector2D(0.0f, 1.0f), Speed);
	CamGaitAlpha = (SprintAlphaRaw >= CamGaitAlpha)
		? SprintAlphaRaw
		: FMath::FInterpTo(CamGaitAlpha, SprintAlphaRaw, DeltaTime, CameraGaitRelaxSpeed);
	CameraBoom->CameraLagSpeed         = FMath::Lerp(CameraLagSpeed,         SprintCameraLagSpeed,         CamGaitAlpha);
	CameraBoom->CameraRotationLagSpeed = FMath::Lerp(CameraRotationLagSpeed, SprintCameraRotationLagSpeed, CamGaitAlpha);

	// ── (B) Sprint FOV push (shares the relaxed blend so the zoom-out eases with the lag) ──
	const float TargetFOV = CameraBaseFOV + SprintFOVPush * CamGaitAlpha;
	FollowCamera->SetFieldOfView(
		FMath::FInterpTo(FollowCamera->FieldOfView, TargetFOV, DeltaTime, CameraFOVInterpSpeed));

	// ── (C) Stop-settle — a sprint halting kicks a small dip ──────────
	// Peak-tracking: the kick scales by how fast the run WAS (mapped over the
	// trot→sprint band, so a plain trot stop stays silent).
	constexpr float StoppedSpeed = 60.0f;
	if (Speed >= StoppedSpeed)
	{
		CamPeakSpeedSinceStop = FMath::Max(CamPeakSpeedSinceStop, Speed);
	}
	else if (CamPeakSpeedSinceStop >= StoppedSpeed)
	{
		if (bIsOnGround && StopSettleDip > 0.0f)
		{
			const float SettleScale = FMath::GetMappedRangeValueClamped(
				GaitSpeedRange, FVector2D(0.0f, 1.0f), CamPeakSpeedSinceStop);
			CamDipVelocity -= StopSettleDip * SettleScale * CameraDipFrequency * UE_EULERS_NUMBER;
		}
		CamPeakSpeedSinceStop = 0.0f;
	}

	// ── (D) Dip spring (landing + stop-settle kicks) ──────────────────
	// Same clamped-step semi-implicit spring as the landing cushion, applied to
	// the spring arm's SocketOffset.Z (post-lag, so it reads as a camera dip).
	if (FMath::Abs(CamDipOffset) < 0.01f && FMath::Abs(CamDipVelocity) < 0.1f)
	{
		CamDipOffset = CamDipVelocity = 0.0f;
	}
	else
	{
		const float DipDT = FMath::Min(DeltaTime, 1.0f / 30.0f);
		const float W = CameraDipFrequency;
		const float Accel = (-W * W * CamDipOffset) - (2.0f * CameraDipDampingRatio * W * CamDipVelocity);
		CamDipVelocity += Accel * DipDT;
		CamDipOffset = FMath::Clamp(CamDipOffset + CamDipVelocity * DipDT,
		                            -CameraLandDipMax, CameraLandDipMax * 0.25f);
	}
	FVector Socket = CameraBoom->SocketOffset;
	Socket.Z = CamBaseSocketOffsetZ + CamDipOffset;
	CameraBoom->SocketOffset = Socket;
}

// ══════════════════════════════════════════════════════════════════════════
// ── UpdateFootIK ──────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Quadruped foot planting. For each paw we trace straight down from the animated
// (FK) paw and publish only the VERTICAL offset needed to meet the ground. The
// AnimBP adds that offset to the IK goal VB (front: VB Pastern — the hand paw stays
// FK and rides the solved pastern; back: VB Foot) and lets Leg IK solve the chain —
// so the stride is fully preserved and only ground conform is layered on.
// FootIKAlpha scales the whole effect. Chain spec is the AnimX kit's (n=3, never
// through the front paw) — see Saved/.Aura/plans/quadruped-ik-port.md.
//
// Cosmetic and local: runs on every non-dedicated machine for every pawn (no
// replication), so each client plants feet against its own world.
void ACatBase::UpdateFootIK(float DeltaTime)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	UWorld* World = GetWorld();
	if (!MeshComp || !World) return;

	// Blend in when grounded, out while airborne — and out during TURN-IN-PLACE
	// (2026-07-06 MP pass): the turn clips are pure authored footwork (force_root_lock)
	// and the capsule rotates procedurally under them; per-frame paw traces lag that
	// rotation by a frame and the conform fights the stepping feet (leg jitter, most
	// visible on a remote copy driven by the turn-yaw pursuit).
	// Speed taper, reinstated 2026-07-08 (M1 gait pass) with a DIFFERENT band than the
	// kit's. The kit's 1.0→0.5-over-0..400 halved slope offsets at a normal walk (the
	// 2026-07-06 ramp bug) and was removed. But the M1 trot exposed the other failure
	// mode: at gait speeds the Leg IK node re-solves the near-straight front leg at full
	// stride reach and snaps between chain configurations — a visible shoulder pop —
	// while its measured useful contribution at steady trot is ±0.15 cm (sampler,
	// 2026-07-08). So: FULL alpha through every walk/blend speed (≤300, slope conform
	// untouched), fading to ZERO by 380 — below the 400 trot cruise, so the solver is
	// bypassed at trot/sprint. Ground conform at gait speeds is carried by the
	// whole-body slope pitch + mesh conform (coarse layers), as the kit intended.
	// Speed-driven, not phase-driven — per-paw swing fades stay banned (slope trap).
	constexpr float FootIKFadeStartSpeed = 300.0f;
	constexpr float FootIKFadeEndSpeed   = 380.0f;
	const bool bTurningFootwork = (SpeedType == ECatMoveType::Turn);
	const float TargetAlpha = (bEnableFootIK && bIsOnGround && !bTurningFootwork)
		? FMath::GetMappedRangeValueClamped(
			FVector2D(FootIKFadeStartSpeed, FootIKFadeEndSpeed), FVector2D(1.0f, 0.0f), Speed)
		: 0.0f;

	// Spine-block weight: the kit fades its Spline-IK spine 1→0 over speed 0..800 (steeper
	// than the leg taper) and disables it in the air. Interp 10 (kit shared Interp helper).
	{
		const float SpineTarget = (bEnableFootIK && bIsOnGround)
			? FMath::GetMappedRangeValueClamped(FVector2D(0.0f, 800.0f), FVector2D(1.0f, 0.0f), Speed)
			: 0.0f;
		SpineIKAlpha = FMath::FInterpTo(SpineIKAlpha, SpineTarget, DeltaTime, 10.0f);
	}

	// Foot IK runs ONLY while grounded — never mid-air. (Pre-arming it during the fall made
	// the Leg IK reprocess the airborne pose and stretch the legs on the way down.) To still
	// avoid the first-grounded-frame pop, SNAP the alpha fully on at the moment of landing so
	// the offset-snap below plants the paws on the contact frame; ease normally otherwise
	// (e.g. decelerating into the slow-speed conform range).
	const bool bJustLanded = (TargetAlpha > 0.0f) && (JumpPhase == ECatJumpPhase::Land) && (FootIKAlpha < 1.0f);
	FootIKAlpha = bJustLanded
		? 1.0f
		: bGoMantle
			? 0.0f   // takeover owns the paws — a walk-up mantle's decaying alpha tail was
			         // dragging stale ground offsets through the pull-up (PawPrint 2026-07-30)
			: FMath::FInterpTo(FootIKAlpha, TargetAlpha, DeltaTime, FootIKInterpSpeed);

	// Fully blended out — settle every output to neutral and skip the traces.
	// EXCEPT when the zero came purely from the speed taper while grounded: the paw
	// layer is bypassed in the ABP by FootIKAlpha ≈ 0 anyway, but the rest of this
	// function must keep running — MeshSlopePitch (the coarse slope layer that carries
	// ramps at trot/sprint) and the spine/incline probes are computed below and would
	// level out / freeze if we returned here.
	const bool bZeroFromSpeedTaper = bEnableFootIK && bIsOnGround && !bTurningFootwork;
	if (TargetAlpha == 0.0f && FootIKAlpha < KINDA_SMALL_NUMBER && !bZeroFromSpeedTaper)
	{
		FootIKOffsetZ_HandL = FootIKOffsetZ_HandR = FootIKOffsetZ_FootL = FootIKOffsetZ_FootR = 0.0f;
		FootIKRot_HandL = FootIKRot_HandR = FootIKRot_FootL = FootIKRot_FootR = FRotator::ZeroRotator;
		MeshSlopePitch = FMath::FInterpTo(MeshSlopePitch, 0.0f, DeltaTime, 14.0f);   // airborne/off: level out fast
		SpineInclineF = FMath::FInterpTo(SpineInclineF, 1.0f, DeltaTime, 10.0f);
		SpineInclineS = FMath::FInterpTo(SpineInclineS, 0.0f, DeltaTime, 10.0f);
		UpTailAlpha   = FMath::GetMappedRangeValueClamped(FVector2D(1.0f, 1.2f), FVector2D(0.0f, 0.3f), SpineInclineF);
		PelvisDropZ = FMath::FInterpTo(PelvisDropZ, 0.0f, DeltaTime, 10.0f);
		ChestDropZ  = FMath::FInterpTo(ChestDropZ,  0.0f, DeltaTime, 10.0f);
		return;
	}

	// Per-paw solve results the spine/incline block below aggregates.
	struct FPawSolve
	{
		bool  bValidFloor = false;
		float FloorZ = 0.0f;        // trace impact Z (world)
		float GroundDeltaZ = 0.0f;  // ground under the paw relative to the capsule contact plane (<0 = downhill)
		FVector2D PawXY = FVector2D::ZeroVector;   // paw world XY — for the stance-span slope pitch
	};
	FPawSolve PawHandL, PawHandR, PawFootL, PawFootR;

	// Capsule contact plane — terrain deltas and the stance fade are measured against this.
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const float ActorGroundZ = GetActorLocation().Z
		- (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);

	auto SolveFoot = [&](const FName Bone, float& OutOffsetZ, FRotator& OutRot, FPawSolve& OutSolve)
	{
		const FVector PawWorld = MeshComp->GetSocketLocation(Bone);
		const FVector Start    = PawWorld + FVector(0.0f, 0.0f, FootIKTraceUpDistance);
		const FVector End      = PawWorld - FVector(0.0f, 0.0f, FootIKTraceDownDistance);

		FHitResult Hit;
		FCollisionQueryParams Params(FName(TEXT("CatFootIK")), /*bTraceComplex*/ false, this);
		bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

		// Wall-hit rejection (wall-pin jerk fix): pinned against a wall, the front paws
		// poke through the face (the body is longer than the capsule) and this trace
		// STARTS inside the wall's collision — UE reports a start-penetrating hit at the
		// trace start (~TraceUp above the paw), which railed the offset to
		// +FootIKTraceUpDistance and the penetration snap applied it in one frame (the
		// head/shoulder wrench; PawPrint-measured −0.1→+25.0 cm in one sample at wall
		// contact). A start-penetrating hit carries no floor information, and a
		// near-vertical surface isn't floor either — treat both as a miss so the paw
		// rides FK and the chest/incline aggregation sees no valid floor.
		if (bHit && (Hit.bStartPenetrating || Hit.ImpactNormal.Z < 0.5f))
		{
			bHit = false;
		}

		// Vertical-only correction, KIT MODEL (2026-07-06): the primary term is the pure
		// TERRAIN DELTA (ground under the paw vs the capsule contact plane) — it depends
		// only on geometry, so there is no feedback through the solved pose. (Both a
		// direct paw-error target and an integrator were tried on the 20° ramp first:
		// the paw error is measured on the POST-IK pose, so direct targeting converges
		// to half the lift and integrating winds up when Leg IK hits the fold limit.)
		// A direct anti-penetration lift is kept as a second term for the landing/cushion
		// dips the terrain term can't see; max() of the two applies whichever matters.
		float DesiredOffsetZ = 0.0f;
		FRotator RotTarget = FRotator::ZeroRotator;
		if (bHit)
		{
			// Direct paw residual (2026-07-06 final form): with the whole-body slope pitch +
			// ground conform doing the COARSE alignment (UpdateLandCushion §B/§C), the paw
			// offsets are small residuals again (≤ a few cm), so the direct measurement is
			// valid — its post-IK feedback halves the correction, which is invisible at this
			// magnitude (it only mattered when offsets carried the full 12 cm slope delta;
			// that era's terrain-delta model is superseded by the body pitch). No swing fade:
			// residuals this small can't yank a swinging paw visibly, and both fade bases
			// break one slope direction (local-ground kills downhill, capsule-plane uphill).
			DesiredOffsetZ = (Hit.ImpactPoint.Z + FootIKPawHeight) - PawWorld.Z;

			// Capture for the spine/incline aggregation. The pelvis/chest drop uses the GROUND
			// height under the paw relative to the capsule's contact plane (kit CompBP model:
			// "floor loc = paw XY at Root Z") — NOT the paw-to-ground distance. A back paw
			// floating over the downhill side mid-stride must still report the full ground
			// drop, and the swing fade below must not shrink it (that fade zeroed the drop
			// signal and left the back legs floating on ramps — found by Sean 2026-07-06).
			OutSolve.bValidFloor = true;
			OutSolve.FloorZ = Hit.ImpactPoint.Z;
			OutSolve.GroundDeltaZ = Hit.ImpactPoint.Z - ActorGroundZ;
			OutSolve.PawXY = FVector2D(PawWorld.X, PawWorld.Y);

			// ── Paw rotation from the surface normal (kit SetToeRot) ──────
			// Normal into MESH COMPONENT space — the ABP applies these as component-space
			// additive rotations, and the mesh component is yawed −90° from the actor, so
			// actor/world axes would mix roll into pitch. Kit formula on component axes:
			// Roll = atan2(N.Y, N.Z), Pitch = −atan2(N.X, N.Z). Faded by proximity to the
			// LOCAL surface — a paw only conforms its rotation when it's near the ground
			// it would be standing on (a swinging or gathered paw stays neutral).
			const float PawAboveLocal = FMath::Max(PawWorld.Z - Hit.ImpactPoint.Z, 0.0f);
			const float RotFade = 1.0f - FMath::Clamp((PawAboveLocal - 2.0f) / 8.0f, 0.0f, 1.0f);
			const FVector N = MeshComp->GetComponentTransform().InverseTransformVectorNoScale(Hit.ImpactNormal);
			const float RollDeg  = FMath::RadiansToDegrees(FMath::Atan2(N.Y, N.Z));
			const float PitchDeg = -FMath::RadiansToDegrees(FMath::Atan2(N.X, N.Z));
			RotTarget = FRotator(
				FMath::ClampAngle(PitchDeg, -45.0f, 45.0f) * RotFade,
				0.0f,
				FMath::ClampAngle(RollDeg, -45.0f, 45.0f) * RotFade);

			// ── Over-extension guard ──────────────────────────────────────
			// A downward pull on an already near-straight leg slams it to full
			// extension (the "snap straight" pop). Walk up the limb to find
			// its root + total reach, then fade the *downward* pull out as the leg
			// approaches full extension. Upward pulls (which bend the leg) are safe.
			if (DesiredOffsetZ < 0.0f)
			{
				if (const USkinnedAsset* Skinned = MeshComp->GetSkinnedAsset())
				{
					const FReferenceSkeleton& Ref = Skinned->GetRefSkeleton();
					FName Cur = Bone;
					FVector ChildPos = PawWorld;
					float Reach = 0.0f;
					// The cat's legs are 4-BONE limbs (digitigrade), so paw->root is 3
					// parents — NOT the 2 a biped 3-bone leg uses:
					//   front: Hand -> Pastern -> Forearm -> UpperArm
					//   back : Foot -> Hook    -> Shin    -> Thigh
					// Walking only 2 measured the lower sub-chain and never reached the
					// limb root, so Ext was meaningless and the guard never engaged.
					constexpr int32 LimbSegmentsToRoot = 3;   // 4-bone limb -> 3 parents
					for (int32 i = 0; i < LimbSegmentsToRoot; ++i)
					{
						const int32 Ci = Ref.FindBoneIndex(Cur);
						const int32 Pi = (Ci != INDEX_NONE) ? Ref.GetParentIndex(Ci) : INDEX_NONE;
						if (Pi == INDEX_NONE) break;
						const FName Parent = Ref.GetBoneName(Pi);
						const FVector ParentPos = MeshComp->GetSocketLocation(Parent);
						Reach += FVector::Dist(ChildPos, ParentPos);
						ChildPos = ParentPos;
						Cur = Parent;
					}
					if (Reach > 1.0f)
					{
						const float Ext = FVector::Dist(PawWorld, ChildPos) / Reach;  // ~1 = straight
						const float Slack = FMath::Clamp((0.95f - Ext) / 0.10f, 0.0f, 1.0f);
						DesiredOffsetZ *= Slack;   // -> 0 as the leg nears full extension
					}
				}
			}

		}
		// Bound by the trace window; the downhill reach can never exceed the real terrain
		// drop below the capsule plane (on flat ground the lower bound is 0 — a paw lifted
		// by the stride or the land-gather pose is never yanked to the floor; the June bug
		// the old upward-only rule fixed stays fixed).
		const float DownhillReach = FMath::Min(OutSolve.GroundDeltaZ + FootIKPawHeight, 0.0f);
		DesiredOffsetZ = FMath::Clamp(DesiredOffsetZ,
			FMath::Max(DownhillReach, -FootIKTraceDownDistance), FootIKTraceUpDistance);

		// Snap (don't smooth) when the paw is BELOW the ground: a landing paw is lifted to the
		// surface on the contact frame instead of easing up over ~0.2s. That ease-up was the
		// "slow paw rise with no animation" on landings. Smooth interp otherwise, so idle/slope
		// micro-adjustments stay soft.
		const bool bPenetrating = bHit && (PawWorld.Z < Hit.ImpactPoint.Z);
		OutOffsetZ = bPenetrating
			? DesiredOffsetZ
			: FMath::FInterpTo(OutOffsetZ, DesiredOffsetZ, DeltaTime, FootIKInterpSpeed);

		// Surface-conform rotation eases at the kit's RInterp speed 30 (both in and out —
		// a missed trace interps back to zero rather than snapping).
		OutRot = FMath::RInterpTo(OutRot, RotTarget, DeltaTime, 30.0f);

		if (bFootIKDebugDraw)
		{
			DrawDebugLine(World, Start, End, FColor::Yellow, false, -1.0f, 0, 0.5f);
			if (bHit)
			{
				DrawDebugPoint(World, Hit.ImpactPoint, 8.0f, FColor::Green, false, -1.0f);
			}
		}
	};

	SolveFoot(TEXT("Hand_L"), FootIKOffsetZ_HandL, FootIKRot_HandL, PawHandL);
	SolveFoot(TEXT("Hand_R"), FootIKOffsetZ_HandR, FootIKRot_HandR, PawHandR);
	SolveFoot(TEXT("Foot_L"), FootIKOffsetZ_FootL, FootIKRot_FootL, PawFootL);
	SolveFoot(TEXT("Foot_R"), FootIKOffsetZ_FootR, FootIKRot_FootR, PawFootR);

	// ── Spine incline + pelvis/chest drop (kit CompBP_IK_ANX math, 2026-07-06) ──
	// Fore/aft: front-vs-back floor height over the ~80 uu hand↔foot span, mapped to the
	// A_Cat_Add_Incline scrub (1.0 neutral ± 0.2). Side: left-vs-right over ±20 uu → |0..0.2|
	// squat weight. All derived from the four paw traces above — no extra casts.
	{
		constexpr float InclineFSpan = 80.0f;    // kit "Incline F - Dis Difference" 40 × 2
		constexpr float InclineFMult = 0.2f;
		constexpr float InclineSSpan = 20.0f;    // kit side map ±20 uu
		constexpr float InclineSMult = 0.2f;
		constexpr float InclineInterp = 10.0f;   // kit shared Interp helper default
		constexpr float MaxBodyDrop = 15.0f;     // sanity clamp on the pelvis/chest reach (cm)

		const bool bFront = PawHandL.bValidFloor && PawHandR.bValidFloor;
		const bool bBack  = PawFootL.bValidFloor && PawFootR.bValidFloor;

		float TargetF = 1.0f, TargetS = 0.0f;
		if (bFront && bBack)
		{
			const float FrontZ = 0.5f * (PawHandL.FloorZ + PawHandR.FloorZ);
			const float BackZ  = 0.5f * (PawFootL.FloorZ + PawFootR.FloorZ);
			TargetF = 1.0f + InclineFMult * FMath::Clamp((FrontZ - BackZ) / InclineFSpan, -1.0f, 1.0f);

			const float LeftZ  = 0.5f * (PawHandL.FloorZ + PawFootL.FloorZ);
			const float RightZ = 0.5f * (PawHandR.FloorZ + PawFootR.FloorZ);
			TargetS = FMath::Abs(InclineSMult * FMath::Clamp((LeftZ - RightZ) / InclineSSpan, -1.0f, 1.0f));
		}
		SpineInclineF = FMath::FInterpTo(SpineInclineF, TargetF, DeltaTime, InclineInterp);
		SpineInclineS = FMath::FInterpTo(SpineInclineS, TargetS, DeltaTime, InclineInterp);
		UpTailAlpha = FMath::GetMappedRangeValueClamped(FVector2D(1.0f, 1.2f), FVector2D(0.0f, 0.3f), SpineInclineF);

		// Pelvis/chest follow the LOWEST ground delta of their pair — SIGNED, like the kit
		// (drop toward downhill ground AND rise over uphill ground; the front legs hang off
		// Spine_3, so a rising chest CP is what gives them room to reach a 20° slope —
		// clamping to drop-only left the front paws buried to the wrist, 2026-07-06).
		// Measured RELATIVE to the whole-body ground conform (UpdateLandCushion §B) so the
		// Spline IK only bends for the fore/aft remainder (raw deltas double-counted the
		// drop and arched the spine). Kit SetPelvisOffset interps direction-dependently.
		auto DropTarget = [&](const FPawSolve& A, const FPawSolve& B) -> float
		{
			if (!A.bValidFloor || !B.bValidFloor) return 0.0f;
			const float Remainder = FMath::Min(A.GroundDeltaZ, B.GroundDeltaZ) - MeshGroundConformZ;
			return FMath::Clamp(Remainder, -MaxBodyDrop, MaxBodyDrop);
		};
		// Pelvis CP held at ZERO (2026-07-06): the back legs hang off the Pelvis bone, which
		// the Spline IK (Spine→Spine_3) never moves — dropping the spine's pelvis end toward
		// the downhill rear couldn't plant anything and read as a sagging lower back ("back
		// arch looks funny", Sean). Rear planting comes from the signed paw goals; downhill
		// body settle from the mesh ground-conform. The chest CP stays: Spine_3 carries the
		// shoulders, so its rise/drop genuinely repositions the front legs on slopes.
		const float PelvisTarget = 0.0f;
		// Chest CP is SIGNED again (2026-07-06 late revert to the Sean-approved 0f1c28b
		// slope stack): the rise-only clamp was treating a downhill "bow" whose real cause
		// was the railed accel-lean spring (now disabled) — with the villain gone, the
		// downhill chest drop is legitimate reach assistance, not a bow.
		const float ChestTarget  = DropTarget(PawHandL, PawHandR);
		PelvisDropZ = FMath::FInterpTo(PelvisDropZ, PelvisTarget, DeltaTime,
			(PelvisTarget < PelvisDropZ) ? 15.0f : 10.0f);
		ChestDropZ = FMath::FInterpTo(ChestDropZ, ChestTarget, DeltaTime,
			(ChestTarget < ChestDropZ) ? 15.0f : 10.0f);

		// ── Whole-body slope pitch target (applied by UpdateLandCushion §C) ──
		// REVERTED to the Sean-approved 0f1c28b stack (2026-07-06 late): capsule-anchored
		// probes (±30 uu along facing, pose-independent), FULL pitch, discontinuity
		// rejection. The MP-session layers (paw-floor veto, idle fraction) each traded leg
		// reach away to treat a "bow" whose real cause was the railed accel-lean spring —
		// with the spring disabled they cost more (floating paws on the ramp) than they
		// protect against. If the ramp-base false-positive resurfaces, prefer tightening
		// the rejection over re-stacking estimators (see the handoff plan §6).
		{
			constexpr float MaxSlopePitch = 25.0f;
			constexpr float ProbeDist = 30.0f;
			float PitchTarget = 0.0f;
			const FVector Fwd = GetActorForwardVector() * ProbeDist;
			const FVector Center = GetActorLocation();
			auto ProbeZ = [&](const FVector& XY, float& OutZ) -> bool
			{
				FHitResult PHit;
				FCollisionQueryParams PParams(FName(TEXT("CatSlopePitch")), false, this);
				if (World->LineTraceSingleByChannel(PHit,
					FVector(XY.X, XY.Y, ActorGroundZ + 30.0f), FVector(XY.X, XY.Y, ActorGroundZ - 40.0f),
					ECC_Visibility, PParams))
				{
					OutZ = PHit.ImpactPoint.Z;
					return true;
				}
				return false;
			};
			float FrontZ = 0.0f, BackZ = 0.0f;
			if (ProbeZ(Center + Fwd, FrontZ) && ProbeZ(Center - Fwd, BackZ))
			{
				const float RawPitch = FMath::RadiansToDegrees(FMath::Atan2(FrontZ - BackZ, 2.0f * ProbeDist));
				PitchTarget = (FMath::Abs(RawPitch) <= MaxSlopePitch) ? RawPitch : 0.0f;
			}
			// Asymmetric ease: settle onto a slope slowly (weight), recover to level fast.
			const float EaseSpeed = (FMath::Abs(PitchTarget) < FMath::Abs(MeshSlopePitch)) ? 14.0f : 6.0f;
			MeshSlopePitch = FMath::FInterpTo(MeshSlopePitch, PitchTarget, DeltaTime, EaseSpeed);
		}
	}

	// ── PawPrint: foot-IK stack channels (wall-pin jerk investigation) ──
	// Every head/shoulder-capable output of this function plus the front-paw trace
	// validity — the wall-pin oscillator must show in one of these, or it's the Leg IK
	// node's chain-config snap inside the ABP (by elimination).
	if (PawPrint && IsLocallyControlled())
	{
		static const FName ChIKAlpha(TEXT("IK_Alpha")),
		                   ChIKHandL(TEXT("IK_HandL")),           ChIKHandR(TEXT("IK_HandR")),
		                   ChIKFootL(TEXT("IK_FootL")),           ChIKFootR(TEXT("IK_FootR")),
		                   ChIKHandLFloor(TEXT("IK_HandL_Floor")), ChIKHandRFloor(TEXT("IK_HandR_Floor")),
		                   ChIKChest(TEXT("IK_ChestDropZ")),      ChIKPitch(TEXT("IK_SlopePitch")),
		                   ChIKInclineF(TEXT("IK_InclineF")),     ChIKHandLRotP(TEXT("IK_HandL_RotPitch"));
		PawPrint->SampleChannel(ChIKAlpha,      FootIKAlpha);
		PawPrint->SampleChannel(ChIKHandL,      FootIKOffsetZ_HandL);
		PawPrint->SampleChannel(ChIKHandR,      FootIKOffsetZ_HandR);
		PawPrint->SampleChannel(ChIKFootL,      FootIKOffsetZ_FootL);
		PawPrint->SampleChannel(ChIKFootR,      FootIKOffsetZ_FootR);
		PawPrint->SampleChannel(ChIKHandLFloor, PawHandL.bValidFloor ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChIKHandRFloor, PawHandR.bValidFloor ? 1.0f : 0.0f);
		PawPrint->SampleChannel(ChIKChest,      ChestDropZ);
		PawPrint->SampleChannel(ChIKPitch,      MeshSlopePitch);
		PawPrint->SampleChannel(ChIKInclineF,   SpineInclineF);
		PawPrint->SampleChannel(ChIKHandLRotP,  FootIKRot_HandL.Pitch);
	}
}

// ── Server RPC: Turn Active (Reliable) ────────────────────────────
void ACatBase::Server_SetTurnActive_Implementation(bool bNewGoTurn, float BodyYaw)
{
	bGoTurn = bNewGoTurn;
	// SpeedType derivation happens next frame in UpdateAnimationStates() else-branch.
	// bGoTurn replicates to all proxies via DOREPLIFETIME_CONDITION (COND_SkipOwner).
	ApplyClientTurnYaw(BodyYaw);
}

// ── Server RPC: Turn Rate (Unreliable) ────────────────────────────
void ACatBase::Server_SetTurnRate_Implementation(float NewTurnRateAnim, float BodyYaw)
{
	TurnRateAnim = NewTurnRateAnim;
	// Replicates to all proxies via DOREPLIFETIME_CONDITION (COND_SkipOwner).
	ApplyClientTurnYaw(BodyYaw);
}

void ACatBase::ApplyClientTurnYaw(float BodyYaw)
{
	// The owning client is authoritative for its cosmetic in-place body yaw. RPC yaws
	// arrive as discrete ~5° steps — store as a TARGET and let the per-frame interp in
	// UpdateAnimationStates rotate the server copy smoothly toward it (snapping here read
	// as jitter on the host, 2026-07-06 MP retest). Never applied to a locally controlled
	// pawn — the host's own turn logic owns that.
	if (!IsLocallyControlled())
	{
		ClientTurnTargetYaw = BodyYaw;
		bHasClientTurnTarget = true;
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Jump System ─────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::SetJumpPhase(ECatJumpPhase NewPhase)
{
	if (JumpPhase == NewPhase) return;

	// A landing prediction only means something while falling.
	if (NewPhase != ECatJumpPhase::Fall)
	{
		bLandPredicted = false;
	}

	JumpPhase = NewPhase;
	OnJumpPhaseChanged.Broadcast(NewPhase);
}

void ACatBase::OnJumpInputPressed()
{
	// Arm the jump buffer, then forward to the standard jump. If the jump can't fire
	// right now (airborne beyond the coyote window), the buffer lets Landed() re-fire it.
	JumpBufferTimer = JumpBufferTime;

	// A jump press while airborne next to a wall is a WALL BOUNCE, not a dead input.
	// Checked before the coyote/anticipation paths: airborne-and-at-a-wall is
	// unambiguous, and the bounce consumes the buffer so it can't also queue a jump.
	if (Traversal && Traversal->TryWallBounce())
	{
		JumpBufferTimer = 0.0f;
		return;
	}

	// Standstill anticipation (2026-07-06, Sean: "all 4 legs/body coil downward and then
	// the up happens"): a grounded, near-stationary jump plays the authored crouch for
	// JumpAnticipationDuration before the physics launch. Running jumps stay instant —
	// an input delay at speed would hurt platforming. The buffered retry below is gated
	// on the timer, so nothing else fires the jump early.
	if (JumpAnticipationDuration > 0.0f && bIsOnGround && Speed <= 200.0f && CanJump())
	{
		JumpAnticipationTimer = JumpAnticipationDuration;
		return;
	}
	Jump();
}

void ACatBase::OnJumped_Implementation()
{
	bFallPending = false;
	FallTransitionHoldTimer = 0.0f;
	// A jump fired: consume the buffer and suppress coyote-time for this airborne span.
	JumpBufferTimer = 0.0f;
	CoyoteTimer = 0.0f;
	bLeftGroundByJumping = true;
	SetJumpPhase(ECatJumpPhase::Launch);
	LaunchVelocityZ = FMath::Abs(GetVelocity().Z);
	JumpAirTime = 0.0f;
}

void ACatBase::Landed(const FHitResult& Hit)
{
	bFallPending = false;
	FallTransitionHoldTimer = 0.0f;
	Super::Landed(Hit);

	const float ImpactZ = FMath::Abs(GetCharacterMovement()->Velocity.Z);
	LandImpactIntensity = FMath::Clamp(ImpactZ / HardLandSpeedThreshold, 0.0f, 1.0f);
	LandRecoveryTimer = LandRecoveryDuration;

	// Post-mantle anim hold: DON'T clear here. Landing puts Anim_JumpPhase in its
	// brief Land window, and releasing bGoMantle inside it let the SM race out
	// through Mantle → Jump_Land — an ~0.08 s crouch-pose flash (the residual end
	// jitter, PawPrint 2026-07-31). Shortening the timer instead releases the hold
	// just AFTER the 0.05 recovery expires, so the SM always leaves at phase None
	// via the clean 0.25 Mantle → Idle/Move blends (Rule_MantleToLand is now
	// phase == Land only, serving abort-landings).
	const bool bPostMantleLanding = MantleAnimHoldTimer > 0.0f;
	if (bPostMantleLanding)
	{
		MantleAnimHoldTimer = 0.08f;
		// The 2 uu settle is not a real fall: the clip's walk-out already IS the
		// landing. A full recovery window + cushion kick here read as the cat
		// "bouncing" right after topping out (Sean's frame-19 catch) — collapse both
		// so the SM can leave for Idle/Move almost immediately and the mesh stays put.
		LandRecoveryTimer = 0.05f;
		LandImpactIntensity = 0.0f;
	}

	// Kick the landing-cushion spring: aim for DipPerImpact × impact speed of body dip.
	// v = dip × ω × e makes a critically-damped spring peak at ≈ the target dip.
	// Moving landings get a reduced kick: foot IK is speed-gated off at a run, so a
	// full dip there would sink the body without planted paws (Land_run's own motion
	// carries the weight instead). Skipped after a mantle (see above).
	if (bEnableLandCushion && !bPostMantleLanding)
	{
		const float MoveScale = (Speed > 150.0f) ? 0.4f : 1.0f;
		const float DipTarget = FMath::Min(ImpactZ * LandCushionDipPerImpact, LandCushionMaxDip) * MoveScale;
		MeshCushionVelocity = -DipTarget * LandCushionFrequency * UE_EULERS_NUMBER;
	}

	// Camera weight (M3): landing dip on the local player's camera. Additive into the
	// dip spring so it composes with an in-flight stop-settle instead of clobbering it.
	if (bEnableCameraWeight && IsLocallyControlled())
	{
		CamDipVelocity -= LandImpactIntensity * CameraLandDipMax * CameraDipFrequency * UE_EULERS_NUMBER;
	}

	SetJumpPhase(ECatJumpPhase::Land);
	OnCatLanded.Broadcast(LandImpactIntensity, JumpAirTime);

	// Kill any stale held-jump so merely *holding* the button doesn't auto-bounce on landing.
	// A *fresh* press near touchdown survives via JumpBufferTimer and is re-fired by the
	// per-frame buffer retry in UpdateJumpPhase — giving deterministic one-press-one-jump.
	StopJumping();
}

bool ACatBase::CanJumpInternal_Implementation() const
{
	if (JumpCooldownTimer > 0.0f) return false;

	// Normal grounded jump.
	if (Super::CanJumpInternal_Implementation()) return true;

	// Coyote time — allow a jump shortly after walking off a ledge (but not after jumping),
	// even though the CMC already reports falling. DoJump still applies JumpZVelocity.
	if (CoyoteTimer > 0.0f && !bLeftGroundByJumping)
	{
		return true;
	}

	return false;
}

void ACatBase::UpdateJumpGravity()
{
	UCharacterMovementComponent* CMC = GetCharacterMovement();
	if (!CMC) return;

	// Only authority and autonomous proxy need to drive physics.
	// Simulated proxies receive replicated position/velocity.
	if (!HasAuthority() && !IsLocallyControlled()) return;

	// On ground phases — snap the interpolator back to Rising so the next
	// airborne jump starts from the correct baseline, not a stale fall value.
	if (JumpPhase == ECatJumpPhase::None || JumpPhase == ECatJumpPhase::Land)
	{
		GravityScaleInterp = GravityScaleRising;
		CMC->GravityScale  = GravityScaleInterp;
		return;
	}

	const float Vz = GetVelocity().Z;
	float TargetGravityScale;

	if (Vz > ApexVelocityThreshold)
	{
		TargetGravityScale = GravityScaleRising;
	}
	else if (FMath::Abs(Vz) <= ApexVelocityThreshold)
	{
		TargetGravityScale = GravityScaleApex;
	}
	else
	{
		TargetGravityScale = GravityScaleFalling;
	}

	// Interpolate toward target — eliminates the single-frame Apex→Fall velocity spike.
	// DeltaTimeCached is set at the top of Tick() before this function is called.
	GravityScaleInterp = FMath::FInterpTo(GravityScaleInterp, TargetGravityScale, DeltaTimeCached, GravityScaleInterpSpeed);
	CMC->GravityScale  = GravityScaleInterp;
}

void ACatBase::UpdateJumpPhase(float DeltaTime)
{
	// ── Cooldown countdown ───────────────────────────────────────────
	if (JumpCooldownTimer > 0.0f)
	{
		JumpCooldownTimer -= DeltaTime;
	}

	// ── Coyote-time + jump-buffer timers ─────────────────────────────
	// bIsOnGround / bIsFalling are refreshed earlier in UpdateAnimationStates().
	if (bIsOnGround)
	{
		// Re-arm coyote every grounded frame; a fresh ground contact is not "left by jumping".
		CoyoteTimer = CoyoteTime;
		bLeftGroundByJumping = false;
	}
	else if (CoyoteTimer > 0.0f)
	{
		CoyoteTimer -= DeltaTime;
	}

	if (JumpBufferTimer > 0.0f)
	{
		JumpBufferTimer -= DeltaTime;
	}

	// Standstill-anticipation countdown: the coil plays while this runs; the launch fires
	// the moment it expires. Runs BEFORE the buffer retry so the retry can't preempt it.
	if (JumpAnticipationTimer > 0.0f)
	{
		JumpAnticipationTimer -= DeltaTime;
		if (JumpAnticipationTimer <= 0.0f)
		{
			JumpAnticipationTimer = 0.0f;
			Jump();
		}
	}

	// Per-frame buffer retry: a buffered press keeps trying until the jump is legal
	// (covers taps eaten by a same-frame release, land-recovery, and cooldown). OnJumped
	// clears the buffer on success, so exactly one jump fires per press. Held off while
	// the anticipation coil is playing.
	if (JumpBufferTimer > 0.0f && JumpAnticipationTimer <= 0.0f && IsLocallyControlled())
	{
		// Wall bounce gets the same buffer grace as a landing: a press made just before
		// the cat reaches the wall still kicks. Tried first — CanJump() is false while
		// airborne, so the bounce would otherwise never see a buffered press.
		if (Traversal && Traversal->TryWallBounce())
		{
			JumpBufferTimer = 0.0f;
		}
		else if (CanJump())
		{
			Jump();
		}
	}

	// ── Air time accumulation ────────────────────────────────────────
	if (JumpPhase != ECatJumpPhase::None && JumpPhase != ECatJumpPhase::Land)
	{
		JumpAirTime += DeltaTime;
	}

	// ── Fall transition hold timer — counts down when Fall condition is detected ──
	// Gives the AnimBP time to finish the uncoil before SetJumpPhase(Fall) fires.
	if (bFallPending && FallTransitionHoldTimer > 0.0f)
	{
		FallTransitionHoldTimer -= DeltaTime;
	}

	const float Vz = GetVelocity().Z;

	// Fall-pose axis for the fall blendspaces — HEIGHT-ABOVE-GROUND based (AnimX pass,
	// Step 2 follow-up). The old |Vz|/impact-speed ratio saturated ~0.2 s into ANY fall
	// (falling gravity 5.5), so even a standstill hop swept the whole blendspace axis and
	// showed the Fall_low "skydive" leg-splay on the way down. Kit equivalent: "Lean Fall"
	// from the SetMaxHight ground probe — the splay belongs to how far the cat still has
	// to FALL, not how fast it is currently falling. The grid is symmetric (apex@0,
	// Fall_low@0.5, apex@1; ABP feeds 1-N), so:
	//   height <= GatherHeight -> N=1.0 (feed 0.0 -> gathered apex pose; ALL hops live here)
	//   height >= SpreadHeight -> N=0.5 (feed 0.5 -> full Fall_low skydive)
	// with a linear ramp between — a tall fall always gathers through the last
	// GatherHeight of descent, so the pose is collected by touchdown. Computed
	// continuously in every airborne phase so the axis is correct the instant Fall begins.
	if (JumpPhase != ECatJumpPhase::None && JumpPhase != ECatJumpPhase::Land)
	{
		// Raised from 150/350 (2026-07-06): those were tuned in the 125 cm tap-jump era —
		// a HELD jump apexes at ~240 cm, which sat mid-ramp and blended the fall splay in
		// right at the top of a deliberate jump (Sean's "back leg kickout at the apex").
		// Any jump apex now stays in the gathered pose; the splay is reserved for real
		// falls (walking off the taller JumpGym platforms).
		constexpr float GatherHeight = 260.0f;
		constexpr float SpreadHeight = 460.0f;
		float HeightAboveGround = SpreadHeight;   // no ground within reach -> treat as a high fall
		if (GetNetMode() != NM_DedicatedServer)   // cosmetic-only signal; skip the trace headless
		{
			FHitResult Hit;
			FCollisionQueryParams Params(FName(TEXT("CatFallHeight")), /*bTraceComplex*/ false, this);
			const FVector Feet = GetCharacterMovement()->GetActorFeetLocation();
			if (GetWorld()->LineTraceSingleByChannel(Hit, Feet, Feet - FVector(0.0f, 0.0f, SpreadHeight + 50.0f), ECC_Visibility, Params))
			{
				HeightAboveGround = Feet.Z - Hit.ImpactPoint.Z;
			}
		}
		const float SpreadAlpha = FMath::Clamp((HeightAboveGround - GatherHeight) / (SpreadHeight - GatherHeight), 0.0f, 1.0f);
		NormalizedFallSpeed = 1.0f - 0.5f * SpreadAlpha;
	}

	switch (JumpPhase)
	{
	case ECatJumpPhase::Launch:
	{
		// Apex window reached — abort any pending fall and advance normally
		if (FMath::Abs(Vz) <= ApexVelocityThreshold)
		{
			bFallPending = false;
			FallTransitionHoldTimer = 0.0f;
			SetJumpPhase(ECatJumpPhase::Apex);
		}
		// Short hop — already past apex, heading down
		else if (Vz < -ApexVelocityThreshold)
		{
			if (!bFallPending)
			{
				// First frame the fall condition is detected: start the hold timer
				bFallPending = true;
				FallTransitionHoldTimer = MinFallTransitionHoldTime;
			}
			else if (FallTransitionHoldTimer <= 0.0f)
			{
				// Timer expired — safe to commit to Fall
				bFallPending = false;
				SetJumpPhase(ECatJumpPhase::Fall);
			}
		}
		// Safety: landed on a ledge while still rising
		if (bIsOnGround && JumpPhase == ECatJumpPhase::Launch)
		{
			bFallPending = false;
			FallTransitionHoldTimer = 0.0f;
			SetJumpPhase(ECatJumpPhase::None);
		}
		break;
	}
	case ECatJumpPhase::Apex:
	{
		if (Vz < -ApexVelocityThreshold)
		{
			if (!bFallPending)
			{
				bFallPending = true;
				FallTransitionHoldTimer = MinFallTransitionHoldTime;
			}
			else if (FallTransitionHoldTimer <= 0.0f)
			{
				bFallPending = false;
				SetJumpPhase(ECatJumpPhase::Fall);
			}
		}
		// Safety: caught a ledge at apex
		if (bIsOnGround && JumpPhase == ECatJumpPhase::Apex)
		{
			bFallPending = false;
			FallTransitionHoldTimer = 0.0f;
			SetJumpPhase(ECatJumpPhase::None);
		}
		break;
	}
	case ECatJumpPhase::Fall:
	{
		// Fall -> Land (gameplay) is handled by Landed() override, not tick.
		// NormalizedFallSpeed is updated continuously above (before the switch).
		// The ANIM Land phase can start earlier via the landing predictor below.
		UpdateLandPrediction();
		// Safety net (the Launch/Apex cases have the same): grounded WITHOUT a
		// Landed() impact — a movement-mode switch put us on the floor (traversal
		// takeover restore, future teleports). A normal landing never hits this
		// (Landed() fires during the CMC move and sets Land before this tick).
		if (bIsOnGround && JumpPhase == ECatJumpPhase::Fall)
		{
			bFallPending = false;
			FallTransitionHoldTimer = 0.0f;
			NormalizedFallSpeed = 0.0f;
			SetJumpPhase(ECatJumpPhase::None);
		}
		break;
	}
	case ECatJumpPhase::Land:
	{
		LandRecoveryTimer -= DeltaTime;
		// Decay LandImpactIntensity over the recovery window
		LandImpactIntensity = FMath::Max(LandImpactIntensity - (DeltaTime / FMath::Max(LandRecoveryDuration, 0.01f)), 0.0f);

		if (LandRecoveryTimer <= 0.0f)
		{
			LandRecoveryTimer = 0.0f;
			JumpCooldownTimer = JumpCooldown;
			NormalizedFallSpeed = 0.0f;
			SetJumpPhase(ECatJumpPhase::None);
		}
		break;
	}
	case ECatJumpPhase::None:
	default:
	{
		NormalizedFallSpeed = 0.0f;
		// Walked off a ledge without jumping — enter Fall directly
		if (bIsFalling && !bIsOnGround)
		{
			LaunchVelocityZ = FMath::Max(FMath::Abs(Vz), 100.0f);
			JumpAirTime = 0.0f;
			SetJumpPhase(ECatJumpPhase::Fall);
		}
		break;
	}
	}

	// ── Anim-facing phase (predictive landing, AnimX pass Step 2) ────
	// The ABP consumes AnimJumpPhase (not JumpPhase): identical except Land is
	// anticipated while falling with a positive prediction, so the land clip's
	// crouch-prep plays BEFORE impact (the kit's bGoLand behavior). Derived — never
	// latched: if the prediction drops (slid off a lip), this reverts to Fall and
	// the SM's Jump_Land -> Jump_Fall escape transition backs out of the land pose.
	AnimJumpPhase = (JumpPhase == ECatJumpPhase::Fall && bLandPredicted)
		? ECatJumpPhase::Land
		: JumpPhase;

	// Anticipation coil: the ABP enters Jump_Launch during the pre-launch crouch so the
	// authored anticipation frames play before the physics jump fires (gameplay JumpPhase
	// stays None until the real Jump()).
	if (JumpPhase == ECatJumpPhase::None && JumpAnticipationTimer > 0.0f)
	{
		AnimJumpPhase = ECatJumpPhase::Launch;
	}
}

void ACatBase::UpdateLandPrediction()
{
	// Cosmetic-only signal — no visuals on a dedicated server, so skip the trace.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bLandPredicted = false;
		return;
	}

	const FVector Vel = GetVelocity();
	UWorld* World = GetWorld();
	if (!World || Vel.Z >= 0.0f)
	{
		bLandPredicted = false;
		return;
	}

	// Moving lands (Land_run) need less anticipation than standstill lands (Land_stop).
	const float PredictTime = bHasMovementInput ? LandPredictTimeMoving : LandPredictTimeStopping;
	const float LookaheadDist = Vel.Size() * PredictTime;
	if (LookaheadDist <= KINDA_SMALL_NUMBER)
	{
		bLandPredicted = false;
		return;
	}

	// Trace from the capsule's feet along the velocity direction — the same path the
	// cat will actually travel over the lookahead window (kit: BottomLoc + Vel×30/50).
	const FVector Start = GetCharacterMovement()->GetActorFeetLocation();
	const FVector End   = Start + Vel.GetSafeNormal() * LookaheadDist;

	FHitResult Hit;
	FCollisionQueryParams Params(FName(TEXT("CatLandPredict")), /*bTraceComplex*/ false, this);
	bLandPredicted = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
}
