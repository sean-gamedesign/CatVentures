// CatPlayerController.cpp

#include "CatPlayerController.h"
#include "EnhancedInputComponent.h"
#include "Blueprint/UserWidget.h"

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
			SetInputMode(FInputModeUIOnly());
		}
	}
}
