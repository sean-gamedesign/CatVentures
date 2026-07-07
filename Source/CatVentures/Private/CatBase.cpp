// CatBase.cpp

#include "CatBase.h"
#include "CatVenturesLog.h"
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
		CMC->FallingLateralFriction      = 3.0f;
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

	// Landing cushion: remember the mesh's authored relative Z — the spring offsets from it.
	if (const USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshCushionBaseZ = MeshComp->GetRelativeLocation().Z;
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
	}
	GravityScaleInterp = GravityScaleRising;
	JumpMaxHoldTime = JumpMaxHoldTimeTuning;
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

	// ── Pitch Clamping (local player only) ─────────────────────────
	if (IsLocallyControlled())
	{
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

		AddMovementInput(ForwardDirection, MoveInput.Y);
		AddMovementInput(RightDirection,   MoveInput.X);
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
		&& (Speed < TurnEngageMaxSpeed);

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

	// Ease the blend param so the footwork fades in/out instead of popping.
	constexpr float TurnRateInterpSpeed = 10.0f;
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
	const float BodyYaw = GetActorRotation().Yaw;
	if (bGoTurn != bIsTurningInPlace)
	{
		bGoTurn = bIsTurningInPlace;
		LastSentBodyYaw = BodyYaw;
		if (!HasAuthority()) Server_SetTurnActive(bIsTurningInPlace, BodyYaw);
	}
	// Send on rate change OR yaw drift: a saturated full-speed turn holds TurnRateAnim
	// constant at ±1, so a rate-only throttle would starve the server of yaw mid-turn.
	if (FMath::Abs(TurnRateAnim - LastSentTurnRateAnim) > 0.05f
		|| (bIsTurningInPlace && FMath::Abs(FRotator::NormalizeAxis(BodyYaw - LastSentBodyYaw)) > 5.0f))
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
		CMC->MaxWalkSpeed              = MovementMaxWalkSpeed;
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
	DOREPLIFETIME_CONDITION(ACatBase, TurnRateAnim, COND_SkipOwner);
	DOREPLIFETIME(ACatBase, bIsGrabbing);
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

	// (f) SpeedType — threshold chain on normalized speed
	const float MaxSpeed = CMC->MaxWalkSpeed;
	const float NormalizedSpeed = (MaxSpeed > KINDA_SMALL_NUMBER) ? (Speed / MaxSpeed) : 0.0f;

	if (NormalizedSpeed >= 0.8f)
	{
		SpeedType = ECatMoveType::Run;
	}
	else if (NormalizedSpeed >= 0.6f)
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

	UE_LOG(LogCatVentures, Verbose, TEXT("[%s] Tick — Speed: %.1f | NormSpeed: %.2f | SpeedType: %d | HasInput: %d | OnGround: %d"),
		*GetName(), Speed, NormalizedSpeed, (int32)SpeedType, bHasMovementInput, bIsOnGround);
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

	// ── (C) Whole-body slope pitch — APPLY only ──────────────────────
	// MeshSlopePitch is COMPUTED in UpdateFootIK from the paw-floor traces (single source
	// of truth with the foot conform; runs after this function — 1-frame lag, invisible).
	// It originally had its own ±30 uu fore/aft probes here: standing at the ramp's BASE,
	// one probe caught the lip while the paws stood on flat ground — a persistent false
	// ~19° reading under the 25° rejection threshold, bowing the cat on level ground
	// (Sean's "stuck downward pose", snapshot-confirmed 2026-07-06: SpineInclineF 0.978
	// vs applied pitch 19.55). Paw-floor-derived pitch cannot disagree with the stance.

	// Single transform write; skip once fully at rest (avoids per-tick transform writes).
	const float TotalOffset = MeshCushionOffset + MeshGroundConformZ;
	FVector Rel = MeshComp->GetRelativeLocation();
	if (FMath::Abs(TotalOffset) < 0.01f && FMath::Abs(MeshSlopePitch) < 0.05f
		&& FMath::IsNearlyEqual(Rel.Z, MeshCushionBaseZ, 0.01f))
	{
		return;
	}
	Rel.Z = MeshCushionBaseZ + TotalOffset;
	// Slope pitch is about the ACTOR's lateral axis — compose it in parent space ahead of
	// the rig's authored relative rotation (the −90° yaw), then write both in one call.
	const FQuat RelQuat = FQuat(FRotator(MeshSlopePitch, 0.0f, 0.0f)) * FQuat(MeshBaseRelRot);
	MeshComp->SetRelativeLocationAndRotation(Rel, RelQuat.Rotator());
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
	// NO speed taper (2026-07-06): the kit's 1.0→0.5-over-0..400 was tuned for its
	// MaxWalkSpeed 250 — at our 400 a normal walk sat at the 0.5 floor, halving every
	// terrain offset (downhill paws floated by half the reach, uphill paws penetrated by
	// half the lift — exactly what Sean saw walking the ramp; standstill was perfect).
	// The taper's rationale (paw-relative IK lagging the stride at speed) doesn't apply
	// to the terrain-delta model: goals depend on ground geometry, not paw position, so
	// they can't lag or fight the stride. Full weight whenever grounded otherwise.
	const bool bTurningFootwork = (SpeedType == ECatMoveType::Turn);
	const float TargetAlpha = (bEnableFootIK && bIsOnGround && !bTurningFootwork) ? 1.0f : 0.0f;

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
		: FMath::FInterpTo(FootIKAlpha, TargetAlpha, DeltaTime, FootIKInterpSpeed);

	// Fully blended out — settle every output to neutral and skip the traces.
	if (TargetAlpha == 0.0f && FootIKAlpha < KINDA_SMALL_NUMBER)
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
		const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

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
		// Chest CP is RISE-ONLY (2026-07-06, Sean's second screenshot): uphill it lifts the
		// shoulders so the front legs can reach a rising slope (its reason to exist);
		// downhill it dropped the shoulders ~5 cm on top of the body pitch and bowed the
		// front end. Downhill reach belongs to the legs (signed paw offsets) + the pitch.
		const float ChestTarget  = FMath::Max(DropTarget(PawHandL, PawHandR), 0.0f);
		PelvisDropZ = FMath::FInterpTo(PelvisDropZ, PelvisTarget, DeltaTime,
			(PelvisTarget < PelvisDropZ) ? 15.0f : 10.0f);
		ChestDropZ = FMath::FInterpTo(ChestDropZ, ChestTarget, DeltaTime,
			(ChestTarget < ChestDropZ) ? 15.0f : 10.0f);

		// ── Whole-body slope pitch target (applied by UpdateLandCushion §C) ──
		// TWO estimators, take the SMALLER magnitude — each vetoes the other's failure mode
		// (both hit in the 2026-07-06 MP pass):
		//  • Capsule-anchored probes (±30 uu along facing, pose-independent): fooled at a
		//    ramp base — one probe on the lip, paws on flat → persistent false ~19° bow.
		//  • Paw-floor differential (stance-true): FEEDS BACK through pose — pitching moves
		//    the paw XY down-slope where floors are lower → runaway to ~2× the true slope.
		// min(|probe|, |paw|) can neither run away (probes bound it) nor bow off-stance
		// (paws bound it). Signs must agree, else 0 (genuinely ambiguous footing).
		{
			constexpr float MaxSlopePitch = 25.0f;
			constexpr float ProbeDist = 30.0f;
			float ProbePitch = 0.0f; bool bProbeValid = false;
			{
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
					ProbePitch = FMath::RadiansToDegrees(FMath::Atan2(FrontZ - BackZ, 2.0f * ProbeDist));
					bProbeValid = FMath::Abs(ProbePitch) <= MaxSlopePitch;
				}
			}
			float PawPitch = 0.0f; bool bPawValid = false;
			if (bFront && bBack)
			{
				const FVector2D FrontXY = (PawHandL.PawXY + PawHandR.PawXY) * 0.5f;
				const FVector2D BackXY  = (PawFootL.PawXY + PawFootR.PawXY) * 0.5f;
				const float Span = FVector2D::Distance(FrontXY, BackXY);
				if (Span > 20.0f)
				{
					const float FrontFloor = 0.5f * (PawHandL.FloorZ + PawHandR.FloorZ);
					const float BackFloor  = 0.5f * (PawFootL.FloorZ + PawFootR.FloorZ);
					PawPitch = FMath::RadiansToDegrees(FMath::Atan2(FrontFloor - BackFloor, Span));
					bPawValid = FMath::Abs(PawPitch) <= MaxSlopePitch;
				}
			}
			float PitchTarget = 0.0f;
			if (bProbeValid && bPawValid && (FMath::Sign(ProbePitch) == FMath::Sign(PawPitch)))
			{
				PitchTarget = (FMath::Abs(ProbePitch) < FMath::Abs(PawPitch)) ? ProbePitch : PawPitch;
			}
			// FRACTIONAL at IDLE only (2026-07-06, Sean's screenshots): full-slope body pitch
			// on a standing cat face-plants the low-hanging idle-pose head into a 20° ramp —
			// a standing quadruped keeps the body mostly level and lets the legs compensate
			// (the signed paw reach does exactly that). MOVING restores FULL alignment: at a
			// reduced moving fraction the front legs must absorb the difference and hit the
			// Leg IK fold limit — the uphill-walk leg IK Sean approved all day regressed.
			PitchTarget *= FMath::GetMappedRangeValueClamped(
				FVector2D(0.0f, 200.0f), FVector2D(0.35f, 1.0f), Speed);
			// Asymmetric ease: settle onto a slope slowly (weight), recover to level fast.
			const float EaseSpeed = (FMath::Abs(PitchTarget) < FMath::Abs(MeshSlopePitch)) ? 14.0f : 6.0f;
			MeshSlopePitch = FMath::FInterpTo(MeshSlopePitch, PitchTarget, DeltaTime, EaseSpeed);
		}
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

	// Kick the landing-cushion spring: aim for DipPerImpact × impact speed of body dip.
	// v = dip × ω × e makes a critically-damped spring peak at ≈ the target dip.
	// Moving landings get a reduced kick: foot IK is speed-gated off at a run, so a
	// full dip there would sink the body without planted paws (Land_run's own motion
	// carries the weight instead).
	if (bEnableLandCushion)
	{
		const float MoveScale = (Speed > 150.0f) ? 0.4f : 1.0f;
		const float DipTarget = FMath::Min(ImpactZ * LandCushionDipPerImpact, LandCushionMaxDip) * MoveScale;
		MeshCushionVelocity = -DipTarget * LandCushionFrequency * UE_EULERS_NUMBER;
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
	if (JumpBufferTimer > 0.0f && JumpAnticipationTimer <= 0.0f && IsLocallyControlled() && CanJump())
	{
		Jump();
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
