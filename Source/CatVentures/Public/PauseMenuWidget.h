// PauseMenuWidget.h — Base class for the pause menu. Handles Escape to close.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PauseMenuWidget.generated.h"

/**
 * C++ base for WBP_PauseMenu.
 * Intercepts the Escape key while UIOnly input mode is active and closes the menu
 * via ACatPlayerController::ToggleMenu(), keeping all close logic in one place.
 */
UCLASS()
class CATVENTURES_API UPauseMenuWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
};
