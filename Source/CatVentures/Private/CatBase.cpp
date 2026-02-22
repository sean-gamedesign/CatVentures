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
		CMC->RotationRate = FRotator(0.0f, 720.0f, 0.0f);

		// Platforming tuning: snappy accel/decel, heavy gravity, high air control
		CMC->GravityScale                = 2.5f;
		CMC->JumpZVelocity               = 600.0f;
		CMC->AirControl                  = 0.7f;
		CMC->FallingLateralFriction      = 3.0f;
		CMC->MaxWalkSpeed                = 400.0f;
		CMC->MaxAcceleration             = 2048.0f;
		CMC->BrakingDecelerationWalking  = 2048.0f;
	}

	// Variable jump height: hold jump up to 0.3s for full height, tap for a short hop.
	JumpMaxHoldTime = 0.3f;
}

void ACatBase::BeginPlay()
{
	Super::BeginPlay();

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
}

void ACatBase::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	DeltaTimeCached = DeltaTime;

	// ── State: runs on ALL roles (server, autonomous, simulated) ──
	UpdateAnimationStates();

	// ── Turn-In-Place Rotation Commitment ─────────────────────────────
	// Runs BEFORE cosmetic interp so next frame's AimYaw sees the
	// already-committed actor rotation — eliminates one-frame snap.
	if (UCharacterMovementComponent* CMC_Mut = GetCharacterMovement())
	{
		if (bGoTurn && IsLocallyControlled())
		{
			CMC_Mut->bOrientRotationToMovement = false;
			bIsCommittingTurn = true;

			// Fresh target every frame — tracks the live camera yaw (full rotator)
			TargetTurnRotation = FRotator(0.0f, GetControlRotation().Yaw, 0.0f);
			const FRotator CurrentRotation = GetActorRotation();
			// RInterpTo takes the shortest path across ±180° — prevents 360° death spins
			const FRotator NewRotation = FMath::RInterpTo(CurrentRotation, TargetTurnRotation, DeltaTime, 5.0f);
			SetActorRotation(NewRotation);

			UE_LOG(LogTemp, Verbose, TEXT("[%s] CommitTurn -- Cur: %.1f | Tgt: %.1f | New: %.1f"),
				*GetName(), CurrentRotation.Yaw, TargetTurnRotation.Yaw, NewRotation.Yaw);
		}
		else if (bIsCommittingTurn)
		{
			CMC_Mut->bOrientRotationToMovement = true;
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
			HitComp->AddImpulse(ImpulseDir * SwatImpulseStrength, NAME_None, true);
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
	else if (NormalizedSpeed >= 0.1f)
	{
		SpeedType = ECatMoveType::Walk;
	}
	else
	{
		SpeedType = ECatMoveType::Idle;
	}

	// (f2) AimYaw — signed yaw delta between control rotation and actor rotation
	AimYaw = FRotator::NormalizeAxis(GetControlRotation().Yaw - GetActorRotation().Yaw);
	AimYawClamped = FMath::Clamp(AimYaw, -90.0f, 90.0f);

	// (f3) Turn-In-Place detection
	//  Triggers when idle on the ground and the camera has orbited > 40° away.
	//  Clears when the yaw delta drops below 10° (hysteresis prevents flicker).
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

	// (f4) TurnRateAnim — drives BS1_Cat_Turn blendspace (-1 = 90°L, +1 = 90°R)
	TurnRateAnim = FMath::GetMappedRangeValueClamped(
		FVector2D(-90.0f, 90.0f), FVector2D(-1.0f, 1.0f), AimYaw);

	UE_LOG(LogTemp, Verbose, TEXT("[%s] AimYaw: %.1f | bGoTurn: %d | TurnRateAnim: %.3f"),
		*GetName(), AimYaw, bGoTurn, TurnRateAnim);

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
