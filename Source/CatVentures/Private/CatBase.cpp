// CatBase.cpp

#include "CatBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"

ACatBase::ACatBase()
{
	PrimaryActorTick.bCanEverTick = true;

	// ── Camera rig ─────────────────────────────────────────────────
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// ── Rotation settings ──────────────────────────────────────────
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

	// Tank controls: character yaw is driven explicitly by A/D input, not by CMC.
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->bOrientRotationToMovement = false;

		// Cat-like jump tuning: snappy burst, heavy fall, good air steering
		CMC->GravityScale    = 2.0f;
		CMC->JumpZVelocity   = 700.0f;
		CMC->AirControl      = 0.5f;
	}
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
	// ── Hard Tick Gate ──────────────────────────────────────────────
	// Non-locally-controlled cats skip the entire Blueprint tick path.
	if (!IsLocallyControlled())
	{
		return;
	}

	Super::Tick(DeltaTime);

	// ── Pitch Clamping ─────────────────────────────────────────────
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		FRotator ControlRot = PC->GetControlRotation();
		ControlRot.Pitch = FMath::ClampAngle(ControlRot.Pitch, -PitchClampDown, PitchClampUp);
		PC->SetControlRotation(ControlRot);
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
	}
}

// ── Tank-Style Movement ─────────────────────────────────────────────────

void ACatBase::Move(const FInputActionValue& Value)
{
	const FVector2D MoveInput = Value.Get<FVector2D>();

	// X axis = Turn (A/D): yaw-rotate the character directly
	if (!FMath::IsNearlyZero(MoveInput.X))
	{
		const float DeltaYaw = MoveInput.X * TurnRate * GetWorld()->GetDeltaSeconds();
		AddActorWorldRotation(FRotator(0.0, DeltaYaw, 0.0));
	}

	// Y axis = Forward/Back (W/S): move along the character's own forward vector
	if (!FMath::IsNearlyZero(MoveInput.Y))
	{
		AddMovementInput(GetActorForwardVector(), MoveInput.Y);
	}
}

void ACatBase::Look(const FInputActionValue& Value)
{
	const FVector2D LookInput = Value.Get<FVector2D>();

	// Apply sensitivity and feed into the controller's control rotation.
	AddControllerYawInput(LookInput.X * LookSensitivity);
	AddControllerPitchInput(LookInput.Y * LookSensitivity);
}

// ── Networked Meow ─────────────────────────────────────────────────────

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
