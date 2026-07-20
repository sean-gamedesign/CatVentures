// Copyright Epic Games, Inc. All Rights Reserved.

#include "CatVentures.h"
#include "CatVenturesLog.h"
#include "Modules/ModuleManager.h"

#if WITH_EDITOR
#include "PawPrintWindow.h"
#endif

DEFINE_LOG_CATEGORY(LogCatVentures);

void FCatVenturesModule::StartupModule()
{
#if WITH_EDITOR
	if (GIsEditor)
	{
		PawPrintWindow::Register();
	}
#endif
}

void FCatVenturesModule::ShutdownModule()
{
#if WITH_EDITOR
	if (GIsEditor)
	{
		PawPrintWindow::Unregister();
	}
#endif
}

IMPLEMENT_PRIMARY_GAME_MODULE( FCatVenturesModule, CatVentures, "CatVentures" );
