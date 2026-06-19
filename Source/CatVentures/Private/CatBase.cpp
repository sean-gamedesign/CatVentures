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

	// ── Turn-In-Place Rotation Commitment ─────────────────────────────
	// Runs BEFORE cosmetic interp so next frame's AimYaw sees the
	// already-committed actor rotation — eliminates one-frame snap.
	if (UCharacterMovementComponent* CMC_Mut = GetCharacterMovement())
	{
		// Commit rotation when bGoTurn is active.
		// Two roles need this:
		//   1. Local client (IsLocallyControlled) — for instant prediction
		//   2. Server copy of client pawn (HasAuthority && !IsLocallyControlled) — so the
		//      authoritative actor rotation matches the turn animation, preventing pop.
		// Simulated proxies receive the replicated rotation automatically.
		const bool bIsLocalTurn  = bGoTurn && IsLocallyControlled();
		const bool bIsServerTurn = bGoTurn && HasAuthority() && !IsLocallyControlled();

		if (bIsLocalTurn || bIsServerTurn)
		{
			CMC_Mut->bOrientRotationToMovement = false;
			bIsCommittingTurn = true;

			// Fresh target every frame — tracks the live camera yaw.
			// GetControlRotation() is valid on both:
			//   - Client: local PlayerController
			//   - Server: the owning PlayerController exists server-side,
			//     ControlRotation is updated via CMC packed movement RPCs.
			TargetTurnRotation = FRotator(0.0f, GetControlRotation().Yaw, 0.0f);
			const FRotator CurrentRotation = GetActorRotation();
			// RInterpTo takes the shortest path across ±180° — prevents 360° death spins
			const FRotator NewRotation = FMath::RInterpTo(CurrentRotation, TargetTurnRotation, DeltaTime, 5.0f);
			SetActorRotation(NewRotation);

			UE_LOG(LogCatVentures, Verbose, TEXT("[%s] CommitTurn -- Cur: %.1f | Tgt: %.1f | New: %.1f | Role: %s"),
				*GetName(), CurrentRotation.Yaw, TargetTurnRotation.Yaw, NewRotation.Yaw,
				IsLocallyControlled() ? TEXT("Local") : TEXT("Server"));
		}
		else if (bIsCommittingTurn)
		{
			// Only restore auto-rotation if a grab is not active — grab holds its own
			// lock on bOrientRotationToMovement and restores it on release.
			if (!bIsGrabbing)
			{
				CMC_Mut->bOrientRotationToMovement = true;
			}
			bIsCommittingTurn = false;

			UE_LOG(LogCatVentures, Verbose, TEXT("[%s] CommitTurn -- Finished, restored bOrientRotationToMovement"),
				*GetName());
		}
	}

	// ── Cosmetic: skip on dedicated server (no visuals) ───────────
	if (GetNetMode() != NM_DedicatedServer)
	{
		UpdateCosmeticInterpolation(DeltaTime);
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

		// (f3) Turn-In-Place detection (hysteresis unchanged)
		const bool bWasTurning = bGoTurn;
		if (SpeedType == ECatMoveType::Idle
			&& MovementStage == ECatMovementStage::OnGround
			&& FMath::Abs(AimYaw) > 40.0f)
		{
			bGoTurn = true;
			SpeedType = ECatMoveType::Turn;
		}
		else if (FMath::Abs(AimYaw) < 10.0f)
		{
			bGoTurn = false;
		}

		// (f4) TurnRateAnim
		TurnRateAnim = FMath::GetMappedRangeValueClamped(
			FVector2D(-90.0f, 90.0f), FVector2D(-1.0f, 1.0f), AimYaw);

		// ── Client → Server RPC: send turn state so server can replicate it out ──
		// Reliable edge-trigger for state; unreliable delta-trigger for blendspace.
		if (!HasAuthority())
		{
			// Reliable edge-trigger: guaranteed ordered delivery for state flips
			if (bGoTurn != bWasTurning)
			{
				Server_SetTurnActive(bGoTurn);
			}

			// Unreliable delta-trigger: smooth blendspace updates during active turn
			if (bGoTurn && FMath::Abs(TurnRateAnim - LastSentTurnRateAnim) > 0.05f)
			{
				Server_SetTurnRate(TurnRateAnim);
				LastSentTurnRateAnim = TurnRateAnim;
			}
		}

		UE_LOG(LogCatVentures, Verbose, TEXT("[%s] AimYaw: %.1f | bGoTurn: %d | TurnRateAnim: %.3f"),
			*GetName(), AimYaw, bGoTurn, TurnRateAnim);
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

	// ── (C) PlayRate Interp ───────────────────────────────────────────
	const float OutputYAbs = (GetCharacterMovement() && GetCharacterMovement()->MaxWalkSpeed > KINDA_SMALL_NUMBER)
		? FMath::Clamp(Speed / GetCharacterMovement()->MaxWalkSpeed, 0.0f, 1.0f)
		: 0.0f;

	const float PlayRateInterpSpeed = FMath::GetMappedRangeValueClamped(
		FVector2D(0.0f, 1.0f), FVector2D(5.0f, 0.5f), OutputYAbs);
	PlayRateInterp = FMath::FInterpTo(PlayRateInterp, PlayRate, DeltaTime, PlayRateInterpSpeed);

	// ── (D) Locomotion Lean ──────────────────────────────────────────
	// Signed yaw RATE (deg/sec) mapped to [-1, 1]. Positive = turning right.
	// Drive a Modify Bone Roll in the ABP — NOT the incline additive.
	// Zero during Turn/Idle to avoid fighting the turn-in-place animation.
	{
		const float CurrentYaw = GetActorRotation().Yaw;
		const float YawDelta = FRotator::NormalizeAxis(CurrentYaw - PreviousYaw);
		const float SafeDT = FMath::Max(DeltaTime, 0.001f);
		// Yaw rate in deg/sec — 90°/s maps to full lean (±1)
		const float YawRate = YawDelta / SafeDT;
		const float RawLean = FMath::GetMappedRangeValueClamped(
			FVector2D(-90.0f, 90.0f), FVector2D(-1.0f, 1.0f), YawRate);

		// Gate: only lean while actually moving, never during Turn or Idle
		const bool bShouldLean = (Speed > 10.0f)
			&& (SpeedType != ECatMoveType::Turn)
			&& (SpeedType != ECatMoveType::Idle);
		const float TargetLean = bShouldLean ? RawLean : 0.0f;
		// Fast attack (6.0) when leaning, slow decay (2.0) to bleed out — eliminates pop on Turn entry
		const float LeanInterpSpeed = bShouldLean ? 6.0f : 2.0f;
		LeanAmount = FMath::FInterpTo(LeanAmount, TargetLean, DeltaTime, LeanInterpSpeed);
		PreviousYaw = CurrentYaw;

		UE_LOG(LogCatVentures, Verbose, TEXT("[%s] Lean -- Rate: %.1f d/s | Raw: %.3f | Final: %.3f | Gate: %d"),
			*GetName(), YawRate, RawLean, LeanAmount, bShouldLean);
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
// ── UpdateFootIK ──────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Quadruped foot planting. For each paw we trace straight down from the animated
// (FK) paw and publish only the VERTICAL offset needed to meet the ground. The
// AnimBP adds that offset to the matching VB Hand/Foot goal (which tracks the live
// FK paw) and lets Leg IK solve the chain — so the stride is fully preserved and
// only ground conform is layered on. FootIKAlpha scales the whole effect.
//
// Cosmetic and local: runs on every non-dedicated machine for every pawn (no
// replication), so each client plants feet against its own world.
void ACatBase::UpdateFootIK(float DeltaTime)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	UWorld* World = GetWorld();
	if (!MeshComp || !World) return;

	// Blend in when grounded, out while airborne. bIsOnGround is refreshed earlier
	// this frame in UpdateAnimationStates().
	const float TargetAlpha = (bEnableFootIK && bIsOnGround) ? 1.0f : 0.0f;
	FootIKAlpha = FMath::FInterpTo(FootIKAlpha, TargetAlpha, DeltaTime, FootIKInterpSpeed);

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
			// extension (the "snap straight" pop). Walk up the 3-bone limb to find
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
					for (int32 i = 0; i < 2; ++i)   // 3 bones in limb -> walk 2 parents up
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
		DesiredOffsetZ = FMath::Clamp(DesiredOffsetZ, -FootIKTraceDownDistance, FootIKTraceUpDistance);
		OutOffsetZ = FMath::FInterpTo(OutOffsetZ, DesiredOffsetZ, DeltaTime, FootIKInterpSpeed);

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
		// Fall -> Land is handled by Landed() override, not tick.
		// NormalizedFallSpeed cosmetic update:
		const float TerminalReference = FMath::Max(LaunchVelocityZ * 1.5f, HardLandSpeedThreshold);
		NormalizedFallSpeed = FMath::Clamp(FMath::Abs(Vz) / TerminalReference, 0.0f, 1.0f);
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
}
