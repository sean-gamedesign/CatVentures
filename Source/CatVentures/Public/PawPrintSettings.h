#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PawPrintSettings.generated.h"

/**
 * PawPrint per-user settings (Project Settings → Game → PawPrint; values live in
 * the per-user EditorPerProjectUserSettings ini, so they never enter the repo).
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "PawPrint"))
class CATVENTURES_API UPawPrintSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPawPrintSettings()
	{
		// Pin to Project Settings → Game → PawPrint (unset, the entry lands in a
		// default section — 2026-07-19 PIE round: Sean couldn't find it).
		CategoryName = TEXT("Game");
	}

	/** Open the PawPrint window automatically whenever a PIE session starts. */
	UPROPERTY(EditAnywhere, config, Category = "PawPrint")
	bool bAutoOpenOnPIE = false;

	/** Category checkboxes last left checked in the window (managed by the window;
	 *  first run seeds from the curated default set). */
	UPROPERTY(config)
	TArray<FName> CheckedCategories;

	/** True once the window has persisted a checkbox state at least once. */
	UPROPERTY(config)
	bool bHasSavedCategories = false;
};
