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
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ACatBase::Move);

		// Meow — fires once on press, routed through the Server RPC
		EnhancedInput->BindAction(MeowAction, ETriggerEvent::Started, this, &ACatBase::Server_Meow);

		// Look — fires every frame while mouse/stick is active
		EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &ACatBase::Look);

		// Jump — Started triggers Jump(), Completed triggers StopJumping() for variable height
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started,   this, &ACharacter::Jump);
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Swat — fires once on press, local prediction + Server RPC
		EnhancedInput->BindAction(SwatAction, ETriggerEvent::Started, this, &ACatBase::TriggerSwat);

		// Interact — fires once on press, server-authoritative trace
		EnhancedInput->BindAction(InteractAction, ETriggerEvent::Started, this, &ACatBase::TriggerInteract);
	}
}

// ── Tank-Style Movement ─────────────────────────────────────────────────

void ACatBase::Move(const FInputActionValue& Value)
{
	const FVector2D MoveInput = Value.Get<FVector2D>();

	// Camera-relative movement: derive directions from controller yaw only
	// (zero pitch/roll so the cat stays grounded even when camera looks up/down).
	const FRotator YawRotation(0.0f, Controller ? Controller->GetControlRotation().Yaw : GetActorRotation().Yaw, 0.0f);
	const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector RightDirection   = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

	// Forward/back (W/S) along camera forward, left/right (A/D) along camera right.
	AddMovementInput(ForwardDirection, MoveInput.Y);
	AddMovementInput(RightDirection, MoveInput.X);
}

void ACatBase::Look(const FInputActionValue& Value)
{
	const FVector2D LookInput = Value.Get<FVector2D>();

	// Apply sensitivity and feed into the controller's control rotation.
	AddControllerYawInput(LookInput.X * LookSensitivity);
	AddControllerPitchInput(LookInput.Y * LookSensitivity);
}

// ── Networked Meow ───────────────────────────────────────────────────

void ACatBase::Server_Meow_Implementation()
{
	// Authority received the request — fan out to all clients.
	NetMulticast_Meow();
}

void ACatBase::NetMulticast_Meow_Implementation()
{
	// Runs on every machine (server + all clients).
	OnMeow.Broadcast();
}

// ── The Swat — Local Prediction + Server Authority ─────────────────────

void ACatBase::TriggerSwat()
{
	if (bIsSwatting || !SwatMontage)
	{
		return;
	}

	// Local prediction: play immediately on the autonomous proxy.
	bIsSwatting = true;
	PlaySwatMontageAndBindEnd();

	// Route based on authority to avoid the listen server sync-call bug:
	// On the host, Server_Swat() executes synchronously and sees bIsSwatting
	// already true, so it early-outs before calling Multicast_Swat().
	if (HasAuthority())
	{
		// Listen server host: we ARE the server, multicast directly.
		Multicast_Swat();
	}
	else
	{
		// Remote client: send RPC to server for validation.
		Server_Swat();
	}
}

void ACatBase::Server_Swat_Implementation()
{
	if (bIsSwatting)
	{
		return;
	}

	bIsSwatting = true;
	Multicast_Swat();
}

void ACatBase::Multicast_Swat_Implementation()
{
	// Skip the instigator — they already predicted locally in TriggerSwat().
	if (IsLocallyControlled())
	{
		return;
	}

	bIsSwatting = true;
	PlaySwatMontageAndBindEnd();
}

void ACatBase::PlaySwatMontageAndBindEnd()
{
	// Use UAnimInstance::Montage_Play directly instead of ACharacter::PlayAnimMontage
	// to avoid the CMC's RepRootMotion replication conflicting with our Multicast_Swat RPC.
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		bIsSwatting = false;
		return;
	}

	const float Duration = AnimInstance->Montage_Play(SwatMontage);
	if (Duration > 0.0f)
	{
		FOnMontageEnded EndDelegate;
		EndDelegate.BindUObject(this, &ACatBase::OnSwatMontageEnded);
		AnimInstance->Montage_SetEndDelegate(EndDelegate, SwatMontage);
	}
	else
	{
		// Montage failed to play — reset immediately.
		bIsSwatting = false;
		UE_LOG(LogTemp, Verbose, TEXT("ACatBase::PlaySwatMontageAndBindEnd — Montage failed to play."));
	}
}

void ACatBase::OnSwatMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
	bIsSwatting = false;
	UE_LOG(LogTemp, Log, TEXT("ACatBase::OnSwatMontageEnded — bInterrupted=%s"),
		bInterrupted ? TEXT("true") : TEXT("false"));
}

// ── Swat Trace (called by UAnimNotifyState_SwatTrace) ──────────────────

void ACatBase::BeginSwatTrace(USkeletalMeshComponent* MeshComp, FName SocketName)
{
	if (!HasAuthority())
	{
		return;
	}

	SwatPreviousPawLocation = MeshComp->GetSocketLocation(SocketName);
	SwatAlreadyHitActors.Empty();
}

void ACatBase::ProcessSwatTraceTick(USkeletalMeshComponent* MeshComp, FName SocketName, float SweepRadius, float DeltaTime)
{
	if (!HasAuthority())
	{
		return;
	}

	const FVector CurrentPawLocation = MeshComp->GetSocketLocation(SocketName);

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(this);
	QueryParams.bTraceComplex = false;

	const bool bHit = GetWorld()->SweepSingleByChannel(
		HitResult,
		SwatPreviousPawLocation,
		CurrentPawLocation,
		FQuat::Identity,
		ECC_PhysicsBody,
		FCollisionShape::MakeSphere(SweepRadius),
		QueryParams
	);

	if (bHit && HitResult.GetActor())
	{
		TWeakObjectPtr<AActor> HitActorWeak = HitResult.GetActor();
		if (!SwatAlreadyHitActors.Contains(HitActorWeak))
		{
			SwatAlreadyHitActors.Add(HitActorWeak);
			HandleSwatHit(HitResult);
		}
	}

	SwatPreviousPawLocation = CurrentPawLocation;
}

void ACatBase::EndSwatTrace()
{
	// Only clear the hit set. Do NOT reset bIsSwatting —
	// that’s handled by OnSwatMontageEnded (interruption-safe).
	SwatAlreadyHitActors.Empty();
}

void ACatBase::HandleSwatHit(const FHitResult& HitResult)
{
	if (!HasAuthority())
	{
		return;
	}

	UPrimitiveComponent* HitComp = HitResult.GetComponent();
	AActor* HitActor = HitResult.GetActor();

	if (HitComp && HitComp->IsSimulatingPhysics())
	{
		// Impulse direction: forward + slight upward arc for satisfying knockback.
		const FVector ImpulseDir = (GetActorForwardVector() + FVector(0.0, 0.0, 0.4)).GetSafeNormal();
		HitComp->AddImpulseAtLocation(ImpulseDir * SwatImpulseStrength, HitResult.ImpactPoint);

		UE_LOG(LogTemp, Log, TEXT("ACatBase::HandleSwatHit — Hit %s at %s"),
			*HitActor->GetName(), *HitResult.ImpactPoint.ToString());
	}

	OnSwatHit.Broadcast(HitActor, HitResult.ImpactPoint);
}

// ── Movement Mode Fix ─────────────────────────────────────────────────

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
	UCharacterMovementComponent* CMC = GetCharacterMovement();
	if (CMC && CMC->MovementMode == MOVE_None)
	{
		CMC->SetMovementMode(MOVE_Walking);
	}
}

// ── Interaction System ───────────────────────────────────────────────────

void ACatBase::TriggerInteract()
{
	if (HasAuthority())
	{
		// Listen server host: we ARE the server, trace directly.
		PerformInteractTrace();
	}
	else
	{
		// Remote client: send RPC to server.
		Server_Interact();
	}
}

void ACatBase::Server_Interact_Implementation()
{
	PerformInteractTrace();
}

void ACatBase::PerformInteractTrace()
{
	const FVector TraceStart = GetActorLocation();
	const FVector TraceEnd   = TraceStart + GetActorForwardVector() * InteractTraceLength;
	const float SphereRadius = 20.0f;

	FHitResult HitResult;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	const bool bHit = GetWorld()->SweepSingleByChannel(
		HitResult,
		TraceStart,
		TraceEnd,
		FQuat::Identity,
		ECC_Visibility,
		FCollisionShape::MakeSphere(SphereRadius),
		Params
	);

	if (!bHit || !HitResult.GetActor())
	{
		return;
	}

	AActor* HitActor = HitResult.GetActor();

	if (HitActor->GetClass()->ImplementsInterface(UInteractableInterface::StaticClass()))
	{
		IInteractableInterface::Execute_Interact(HitActor, this);

		UE_LOG(LogTemp, Log, TEXT("ACatBase::PerformInteractTrace — Interacted with %s"), *HitActor->GetName());
	}
}

// ══════════════════════════════════════════════════════════════════════
// ── Replication ──────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════

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

// ══════════════════════════════════════════════════════════════════════
// ── OnRep Callbacks ─────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════

void ACatBase::OnRep_SpeedType()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_SpeedType -> %d"), *GetName(), static_cast<uint8>(SpeedType));
}

void ACatBase::OnRep_CurrentAction()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_CurrentAction -> %d"), *GetName(), static_cast<uint8>(CurrentAction));
}

void ACatBase::OnRep_ControlMode()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_ControlMode -> %d"), *GetName(), static_cast<uint8>(ControlMode));
}

void ACatBase::OnRep_MovementStage()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_MovementStage -> %d"), *GetName(), static_cast<uint8>(MovementStage));
}

void ACatBase::OnRep_AimMode()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_AimMode -> %d"), *GetName(), static_cast<uint8>(AimMode));
}

void ACatBase::OnRep_AnimBSMode()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_AnimBSMode -> %d"), *GetName(), static_cast<uint8>(AnimBSMode));
}

void ACatBase::OnRep_BaseAction()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_BaseAction -> %d"), *GetName(), static_cast<uint8>(BaseAction));
}

void ACatBase::OnRep_RestState()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_RestState -> %d"), *GetName(), static_cast<uint8>(RestState));
}

void ACatBase::OnRep_bCrouchMode()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_bCrouchMode -> %s"), *GetName(), bCrouchMode ? TEXT("true") : TEXT("false"));
}

void ACatBase::OnRep_bDied()
{
	UE_LOG(LogTemp, Log, TEXT("[%s] OnRep_bDied -> %s"), *GetName(), bDied ? TEXT("true") : TEXT("false"));
}

// ══════════════════════════════════════════════════════════════════════════
// ── UpdateAnimationStates ─────────────────────────────────────────────────
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
}
