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
	bUseControllerRotationYaw = false;

	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->bOrientRotationToMovement = true;
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
}

void ACatBase::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Movement — fires every frame while the key is held
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ACatBase::Move);

		// Meow — fires once on press, routed through the Server RPC
		EnhancedInput->BindAction(MeowAction, ETriggerEvent::Started, this, &ACatBase::Server_Meow);
	}
}

void ACatBase::Move(const FInputActionValue& Value)
{
	const FVector2D MoveInput = Value.Get<FVector2D>();

	// Derive forward/right directions from the controller's yaw only.
	const FRotator YawRotation(0.0, Controller->GetControlRotation().Yaw, 0.0);
	const FVector ForwardDir = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector RightDir   = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

	AddMovementInput(ForwardDir, MoveInput.Y);
	AddMovementInput(RightDir,   MoveInput.X);
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
