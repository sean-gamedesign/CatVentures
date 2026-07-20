#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

/**
 * PawPrint window — Slate nomad tab (editor builds only). Registered from the game
 * module's StartupModule; appears under the editor's Window menu as "PawPrint".
 * See PawPrintWindow.cpp for the widget; the data all comes from UPawPrintSubsystem.
 */
namespace PawPrintWindow
{
	void Register();
	void Unregister();
}

#endif
