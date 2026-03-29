// CatPlayerController.h — Handles pause menu toggling.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "CatPlayerController.generated.h"

class UInputAction;
class UUserWidget;

UCLASS()
class CATVENTURES_API ACatPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	/** Widget class for the pause menu. Assign WBP_PauseMenu in the Blueprint subclass. */
	UPROPERTY(EditAnywhere, Category = "UI")
	TSubclassOf<UUserWidget> PauseMenuClass;

protected:
	virtual void SetupInputComponent() override;

	/** Server-side: resets input mode when the pawn is possessed (covers listen-server host). */
	virtual void OnPossess(APawn* InPawn) override;

	/** Client-side: resets input mode once the server confirms possession via ClientRestart. */
	virtual void AcknowledgePossession(APawn* P) override;

private:
	/** Instantiated widget — UPROPERTY keeps it GC-safe while off-screen. */
	UPROPERTY()
	TObjectPtr<UUserWidget> PauseMenuInstance;

	/** IA_ToggleMenu input action asset. Assign in the Blueprint subclass. */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> ToggleMenuAction;

	/** Opens or closes the pause menu. */
	void ToggleMenu();
};
