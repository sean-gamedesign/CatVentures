// CatObjectiveTargetComponent.cpp

#include "CatObjectiveTargetComponent.h"
#include "CatGameMode.h"
#include "CatVenturesLog.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

UCatObjectiveTargetComponent::UCatObjectiveTargetComponent()
{
	// Purely reactive — no per-frame work. Objective state changes arrive as events.
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);   // server-side bookkeeping; the STATE replicates via GameState.
}

ACatGameMode* UCatObjectiveTargetComponent::GetCatGameMode() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetAuthGameMode<ACatGameMode>() : nullptr;
}

void UCatObjectiveTargetComponent::BeginPlay()
{
	Super::BeginPlay();

	// Clients have no GameMode, so this is server-only by construction.
	if (ACatGameMode* GM = GetCatGameMode())
	{
		if (!bRegistered)
		{
			GM->RegisterObjectiveTarget(this);
			bRegistered = true;
		}
	}
}

void UCatObjectiveTargetComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bRegistered)
	{
		if (ACatGameMode* GM = GetCatGameMode())
		{
			GM->UnregisterObjectiveTarget(this);
		}
		bRegistered = false;
	}

	Super::EndPlay(EndPlayReason);
}

void UCatObjectiveTargetComponent::ReportDestroyed()
{
	// Latch-on-true at the target level too: a Geometry Collection survives its own
	// fracture (BB-17), so the break path can fire more than once for one prop.
	// Without this guard a count-of condition would double-count a single target.
	if (bDestroyed)
	{
		return;
	}

	ACatGameMode* GM = GetCatGameMode();
	if (!GM)
	{
		return;   // client copy — the server's report is the authoritative one.
	}

	bDestroyed = true;

	UE_LOG(LogCatVentures, Log, TEXT("[Objective] Target DESTROYED  Tag=%s  Actor=%s"),
		*ObjectiveTag.ToString(), *GetNameSafe(GetOwner()));

	GM->NotifyObjectiveTargetChanged(this);
}
