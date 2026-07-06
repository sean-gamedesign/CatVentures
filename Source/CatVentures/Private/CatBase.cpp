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
	if (bGoTurn != bIsTurningInPlace)
	{
		bGoTurn = bIsTurningInPlace;
		if (!HasAuthority()) Server_SetTurnActive(bIsTurningInPlace);
	}
	if (FMath::Abs(TurnRateAnim - LastSentTurnRateAnim) > 0.05f)
	{
		LastSentTurnRateAnim = TurnRateAnim;
		if (!HasAuthority()) Server_SetTurnRate(TurnRateAnim);
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

		// Semi-implicit spring-damper toward Target.
		AccelLeanVelocity += (Target - AccelLeanAmount) * AccelLeanStiffness * SafeDT;
		AccelLeanVelocity -= AccelLeanVelocity * AccelLeanDamping * SafeDT;
		AccelLeanAmount   += AccelLeanVelocity * SafeDT;
		AccelLeanAmount    = FMath::Clamp(AccelLeanAmount, -1.5f, 1.5f);

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

	if (!bEnableLandCushion)
	{
		MeshCushionOffset = MeshCushionVelocity = 0.0f;
		return;
	}

	// Nothing to do once settled (avoids per-tick transform writes at rest).
	if (FMath::Abs(MeshCushionOffset) < 0.01f && FMath::Abs(MeshCushionVelocity) < 0.1f)
	{
		if (MeshCushionOffset != 0.0f)
		{
			MeshCushionOffset = MeshCushionVelocity = 0.0f;
			FVector Rel = MeshComp->GetRelativeLocation();
			Rel.Z = MeshCushionBaseZ;
			MeshComp->SetRelativeLocation(Rel);
		}
		return;
	}

	// Semi-implicit damped spring toward offset 0. Substep-free: at ω≤40 and game
	// framerates the integration is comfortably stable.
	const float W = LandCushionFrequency;
	const float Accel = (-W * W * MeshCushionOffset) - (2.0f * LandCushionDampingRatio * W * MeshCushionVelocity);
	MeshCushionVelocity += Accel * DeltaTime;
	MeshCushionOffset = FMath::Clamp(MeshCushionOffset + MeshCushionVelocity * DeltaTime,
	                                 -LandCushionMaxDip, LandCushionMaxDip * 0.25f);

	FVector Rel = MeshComp->GetRelativeLocation();
	Rel.Z = MeshCushionBaseZ + MeshCushionOffset;
	MeshComp->SetRelativeLocation(Rel);
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

	// Blend in when grounded AND moving slowly, out while airborne or running.
	// bIsOnGround is refreshed earlier this frame in UpdateAnimationStates().
	// The speed gate is the fix for the stretched-leg bug: foot IK is a landing/slope
	// conform aid, and at run speed the per-paw Z offsets lag the stride and over-extend
	// the 4-bone legs. Gating FootIKAlpha to 0 also kills the Leg IK nodes (they read it
	// on their Alpha pin), so the legs free up entirely during a run.
	// Ground speed (cm/s) at/above which foot IK fades fully out. 150 sits below the 400
	// walk speed so a brisk walk keeps conform; a run drops it. constexpr (not a UPROPERTY)
	// to keep this a Live-Coding-safe function-body change, matching UpdateTurnInPlace.
	constexpr float FootIKMaxSpeed = 150.0f;
	const bool bSlowEnough = Speed <= FootIKMaxSpeed;
	const float TargetAlpha = (bEnableFootIK && bIsOnGround && bSlowEnough) ? 1.0f : 0.0f;

	// Foot IK runs ONLY while grounded — never mid-air. (Pre-arming it during the fall made
	// the Leg IK reprocess the airborne pose and stretch the legs on the way down.) To still
	// avoid the first-grounded-frame pop, SNAP the alpha fully on at the moment of landing so
	// the offset-snap below plants the paws on the contact frame; ease normally otherwise
	// (e.g. decelerating into the slow-speed conform range).
	const bool bJustLanded = (TargetAlpha > 0.0f) && (JumpPhase == ECatJumpPhase::Land) && (FootIKAlpha < 1.0f);
	FootIKAlpha = bJustLanded
		? 1.0f
		: FMath::FInterpTo(FootIKAlpha, TargetAlpha, DeltaTime, FootIKInterpSpeed);

	// Fully blended out — let the offsets settle to zero and skip the traces.
	if (TargetAlpha == 0.0f && FootIKAlpha < KINDA_SMALL_NUMBER)
	{
		FootIKOffsetZ_HandL = FootIKOffsetZ_HandR = FootIKOffsetZ_FootL = FootIKOffsetZ_FootR = 0.0f;
		return;
	}

	auto SolveFoot = [&](const FName Bone, float& OutOffsetZ)
	{
		const FVector PawWorld = MeshComp->GetSocketLocation(Bone);
		const FVector Start    = PawWorld + FVector(0.0f, 0.0f, FootIKTraceUpDistance);
		const FVector End      = PawWorld - FVector(0.0f, 0.0f, FootIKTraceDownDistance);

		FHitResult Hit;
		FCollisionQueryParams Params(FName(TEXT("CatFootIK")), /*bTraceComplex*/ false, this);
		const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

		// Vertical-only correction toward the ground.
		float DesiredOffsetZ = 0.0f;
		if (bHit)
		{
			DesiredOffsetZ = (Hit.ImpactPoint.Z + FootIKPawHeight) - PawWorld.Z;

			// ── Swing-phase gate ──────────────────────────────────────────
			// Only conform a paw that's near the ground (stance). As the paw lifts
			// through its stride (swing), fade the offset to zero so the solver never
			// yanks a swinging foot back down — that yank was the per-stride pop.
			// Full IK below FadeLow cm above ground; zero IK above FadeHigh cm.
			const float FootAboveGround = FMath::Max(PawWorld.Z - Hit.ImpactPoint.Z, 0.0f);
			const float FadeLow  = 2.0f;
			const float FadeHigh = 10.0f;
			const float Fade = 1.0f - FMath::Clamp((FootAboveGround - FadeLow) / (FadeHigh - FadeLow), 0.0f, 1.0f);
			DesiredOffsetZ *= Fade;

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
		// Upward-only (anti-penetration) conform: lift a paw that is AT or BELOW the ground up
		// to the surface, but NEVER pull a lifted paw DOWN. Downward pulls were stretching the
		// hind legs on landing — the land-gather pose lifts the hind paws on purpose, and the IK
		// was dragging them onto the floor and over-extending the limb (same root cause as the
		// earlier run-stride stretch). Anti-penetration is all this game needs (landings + the
		// uphill side of slopes); a lifted paw simply keeps its animated position.
		DesiredOffsetZ = FMath::Clamp(DesiredOffsetZ, 0.0f, FootIKTraceUpDistance);

		// Snap (don't smooth) when the paw is BELOW the ground: a landing paw is lifted to the
		// surface on the contact frame instead of easing up over ~0.2s. That ease-up was the
		// "slow paw rise with no animation" on landings. Smooth interp otherwise, so idle/slope
		// micro-adjustments stay soft.
		const bool bPenetrating = bHit && (PawWorld.Z < Hit.ImpactPoint.Z);
		OutOffsetZ = bPenetrating
			? DesiredOffsetZ
			: FMath::FInterpTo(OutOffsetZ, DesiredOffsetZ, DeltaTime, FootIKInterpSpeed);

		if (bFootIKDebugDraw)
		{
			DrawDebugLine(World, Start, End, FColor::Yellow, false, -1.0f, 0, 0.5f);
			if (bHit)
			{
				DrawDebugPoint(World, Hit.ImpactPoint, 8.0f, FColor::Green, false, -1.0f);
			}
		}
	};

	SolveFoot(TEXT("Hand_L"), FootIKOffsetZ_HandL);
	SolveFoot(TEXT("Hand_R"), FootIKOffsetZ_HandR);
	SolveFoot(TEXT("Foot_L"), FootIKOffsetZ_FootL);
	SolveFoot(TEXT("Foot_R"), FootIKOffsetZ_FootR);
}

// ── Server RPC: Turn Active (Reliable) ────────────────────────────
void ACatBase::Server_SetTurnActive_Implementation(bool bNewGoTurn)
{
	bGoTurn = bNewGoTurn;
	// SpeedType derivation happens next frame in UpdateAnimationStates() else-branch.
	// bGoTurn replicates to all proxies via DOREPLIFETIME_CONDITION (COND_SkipOwner).
}

// ── Server RPC: Turn Rate (Unreliable) ────────────────────────────
void ACatBase::Server_SetTurnRate_Implementation(float NewTurnRateAnim)
{
	TurnRateAnim = NewTurnRateAnim;
	// Replicates to all proxies via DOREPLIFETIME_CONDITION (COND_SkipOwner).
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

	// Per-frame buffer retry: a buffered press keeps trying until the jump is legal
	// (covers taps eaten by a same-frame release, land-recovery, and cooldown). OnJumped
	// clears the buffer on success, so exactly one jump fires per press.
	if (JumpBufferTimer > 0.0f && IsLocallyControlled() && CanJump())
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
		constexpr float GatherHeight = 150.0f;
		constexpr float SpreadHeight = 350.0f;
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
