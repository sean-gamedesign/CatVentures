// CatBase.cpp

#include "CatBase.h"
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
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"

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
	PhysicsBumper->SetGenerateOverlapEvents(true);

	// ── Mouth Grab ────────────────────────────────────────────────
	GrabHandle = CreateDefaultSubobject<UPhysicsHandleComponent>(TEXT("GrabHandle"));
	GrabHandle->LinearStiffness    = 300.0f;  // Low stiffness = heavy drag feel for 100kg objects
	GrabHandle->LinearDamping      = 50.0f;   // High damping = sluggish, no oscillation
	GrabHandle->AngularStiffness   = 0.0f;    // Zero = object tumbles freely (realistic mouth grab)
	GrabHandle->AngularDamping     = 0.0f;
	GrabHandle->InterpolationSpeed = 50.0f;

	GrabTargetLocation = CreateDefaultSubobject<USceneComponent>(TEXT("GrabTargetLocation"));
	GrabTargetLocation->SetupAttachment(GetMesh(), TEXT("socket_mouth"));

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

	// Apply jump tuning UPROPERTYs to the CMC (so per-instance overrides in Details panel take effect)
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
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
	if (!HasAuthority()) return;
	if (!OtherComp || !OtherComp->IsSimulatingPhysics()) return;

	// Stage 1 — CMC floor check (primary, rotation-agnostic).
	// If the cat is grounded on this exact component, suppress the impulse.
	if (GetCharacterMovement()->CurrentFloor.HitResult.GetComponent() == OtherComp) return;

	// Stage 2 — Z-bounds fallback (airborne case).
	// When airborne, CurrentFloor is stale. Suppress if the object's AABB top
	// is at or below the cat's feet — it's directly underneath, not beside.
	const float FeetZ   = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float ObjTopZ = OtherComp->Bounds.GetBox().Max.Z;
	if (ObjTopZ <= FeetZ + UnderFootTolerance) return;

	FVector Vel = GetVelocity();
	Vel.Z = 0.0f;
	const FVector ImpulseDir = Vel.SizeSquared() > 1.0f ? Vel.GetSafeNormal() : GetActorForwardVector();
	OtherComp->AddImpulse(ImpulseDir * BumperPushForce, NAME_None, /*bVelChange=*/false);
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

			UE_LOG(LogTemp, Verbose, TEXT("[%s] CommitTurn -- Cur: %.1f | Tgt: %.1f | New: %.1f | Role: %s"),
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

			UE_LOG(LogTemp, Verbose, TEXT("[%s] CommitTurn -- Finished, restored bOrientRotationToMovement"),
				*GetName());
		}
	}

	// ── Cosmetic: skip on dedicated server (no visuals) ───────────
	if (GetNetMode() != NM_DedicatedServer)
	{
		UpdateCosmeticInterpolation(DeltaTime);
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

		// Jump — Started/Completed for variable-height
		EnhancedInput->BindAction(JumpAction,   ETriggerEvent::Started,   this, &ACharacter::Jump);
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

	if (GetWorld()->SweepSingleByChannel(
		HitResult,
		SwatPreviousPawLocation,
		CurrentPawLocation,
		FQuat::Identity,
		ECC_Pawn,
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

	// Apply impulse to physics objects
	if (UPrimitiveComponent* HitComp = HitResult.GetComponent())
	{
		if (HitComp->IsSimulatingPhysics())
		{
			const FVector ImpulseDir = (HitActor->GetActorLocation() - GetActorLocation()).GetSafeNormal();
			HitComp->AddImpulse(ImpulseDir * SwatImpulseForce, NAME_None, /*bVelChange=*/false);
		}
	}

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

	if (!GetWorld()->SweepSingleByChannel(
		HitResult,
		TraceStart,
		TraceEnd,
		FQuat::Identity,
		ECC_PhysicsBody,
		FCollisionShape::MakeSphere(GrabTraceRadius),
		Params))
	{
		return;
	}

	UPrimitiveComponent* HitComp = HitResult.GetComponent();
	if (!HitComp || !HitComp->IsSimulatingPhysics()) return;

	GrabHandle->GrabComponentAtLocationWithRotation(
		HitComp,
		NAME_None,
		HitResult.ImpactPoint,
		HitComp->GetComponentRotation());

	GrabbedComponent = HitComp;
	bIsGrabbing      = true;
	ApplyDragMovementSettings();
}

void ACatBase::Server_ReleaseGrab_Implementation()
{
	GrabHandle->ReleaseComponent();
	GrabbedComponent.Reset();
	bIsGrabbing = false;
	RestoreNormalMovementSettings();
}

void ACatBase::UpdateGrab(float DeltaTime)
{
	if (!bIsGrabbing) return;

	// Auto-release if the grabbed actor was destroyed mid-grab
	if (!GrabbedComponent.IsValid())
	{
		GrabHandle->ReleaseComponent();
		bIsGrabbing = false;
		RestoreNormalMovementSettings();
		return;
	}

	// Auto-release if the object has drifted too far (e.g. cat jumped over a wall)
	const float Dist = FVector::Dist(
		GrabTargetLocation->GetComponentLocation(),
		GrabbedComponent->GetComponentLocation());

	if (Dist > MaxGrabDistance)
	{
		Server_ReleaseGrab_Implementation();
		return;
	}

	GrabHandle->SetTargetLocationAndRotation(
		GrabTargetLocation->GetComponentLocation(),
		GrabTargetLocation->GetComponentRotation());
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
	DOREPLIFETIME(ACatBase, CurrentAction);
	DOREPLIFETIME(ACatBase, ControlMode);
	DOREPLIFETIME(ACatBase, MovementStage);
	DOREPLIFETIME(ACatBase, AimMode);
	DOREPLIFETIME(ACatBase, AnimBSMode);
	DOREPLIFETIME(ACatBase, BaseAction);
	DOREPLIFETIME(ACatBase, RestState);
	DOREPLIFETIME(ACatBase, bCrouchMode);
	DOREPLIFETIME(ACatBase, bDied);
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
void ACatBase::OnRep_CurrentAction()  {}
void ACatBase::OnRep_ControlMode()    {}
void ACatBase::OnRep_MovementStage()  {}
void ACatBase::OnRep_AimMode()        {}
void ACatBase::OnRep_AnimBSMode()     {}
void ACatBase::OnRep_BaseAction()     {}
void ACatBase::OnRep_RestState()      {}
void ACatBase::OnRep_bCrouchMode()    {}
void ACatBase::OnRep_bDied()          {}
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

	if (bCrouchMode)
	{
		SpeedType = ECatMoveType::Crouch;
	}
	else if (NormalizedSpeed >= 0.8f)
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
		// (f2) AimYaw — only valid with a local controller
		AimYaw = FRotator::NormalizeAxis(GetControlRotation().Yaw - GetActorRotation().Yaw);
		AimYawClamped = FMath::Clamp(AimYaw, -90.0f, 90.0f);

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

		UE_LOG(LogTemp, Verbose, TEXT("[%s] AimYaw: %.1f | bGoTurn: %d | TurnRateAnim: %.3f"),
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

	// (g) Backwards — dot product of velocity dir vs actor forward
	if (bHasMovementInput && Speed > KINDA_SMALL_NUMBER)
	{
		const float Dot = FVector::DotProduct(Velocity2D.GetSafeNormal(), GetActorForwardVector());
		bBackwards = Dot < -0.1f;
	}
	else
	{
		bBackwards = false;
	}

	// (h) SpeedMultiplierFinale
	SpeedMultiplierFinale = bBackwards ? 0.5f : 0.75f;

	UE_LOG(LogTemp, Verbose, TEXT("[%s] Tick — Speed: %.1f | NormSpeed: %.2f | SpeedType: %d | HasInput: %d | OnGround: %d"),
		*GetName(), Speed, NormalizedSpeed, (int32)SpeedType, bHasMovementInput, bIsOnGround);
}

// ══════════════════════════════════════════════════════════════════════════
// ── UpdateCosmeticInterpolation ───────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatBase::UpdateCosmeticInterpolation(float DeltaTime)
{
	// ── (A) Breath ────────────────────────────────────────────────────
	if (SpeedType == ECatMoveType::Run)
	{
		TimeInRun += DeltaTime;
	}
	else if (SpeedType == ECatMoveType::Trot)
	{
		TimeInRun += DeltaTime * 0.35f;
	}
	else
	{
		TimeInRun = 0.0f;
	}

	TimeInRunCache = TimeInRun;
	AlphaPlayBreath = (TimeInRunCache > 1.0f) ? 1.0f : 0.0f;
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

	// ── (D) Mesh Z-offset ─────────────────────────────────────────────
	FixedLocationMesh = FMath::FInterpTo(FixedLocationMesh, 0.0f, DeltaTime, 5.0f);
	FixedLocationSwim = FMath::FInterpTo(FixedLocationSwim, 0.0f, DeltaTime, 2.0f);
	FixedLocationCamera = FMath::FInterpTo(FixedLocationCamera, 0.0f, DeltaTime, 5.0f);

	// ── (E) Locomotion Lean ──────────────────────────────────────────
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

		UE_LOG(LogTemp, Verbose, TEXT("[%s] Lean -- Rate: %.1f d/s | Raw: %.3f | Final: %.3f | Gate: %d"),
			*GetName(), YawRate, RawLean, LeanAmount, bShouldLean);
	}
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

void ACatBase::OnJumped_Implementation()
{
	bFallPending = false;
	FallTransitionHoldTimer = 0.0f;
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
}

bool ACatBase::CanJumpInternal_Implementation() const
{
	if (JumpCooldownTimer > 0.0f) return false;
	return Super::CanJumpInternal_Implementation();
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
