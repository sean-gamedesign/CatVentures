// CatGameMode.cpp

#include "CatGameMode.h"
#include "CatVenturesLog.h"
#include "CatGameState.h"
#include "CatObjectiveTargetComponent.h"
#include "CatPlayerController.h"
#include "CatPlayerState.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

ACatGameMode::ACatGameMode()
{
	// PlayerState carries the per-player rematch-ready flag for the scoreboard.
	// BP_CatGameMode can override this if a designer needs a different class.
	PlayerStateClass = ACatPlayerState::StaticClass();
}

void ACatGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Push the threshold to GameState so clients can compute the HUD percentage.
	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->ChaosThreshold = ChaosThreshold;
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Heavy tier ───────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

/** Resolves the authoritative GameMode from any world context. Returns null on
 *  clients — the same shape ReportItemDestroyed relies on. */
static ACatGameMode* ResolveCatGameMode(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetAuthGameMode<ACatGameMode>() : nullptr;
}

bool ACatGameMode::IsHeavyProp(UObject* WorldContextObject, FName ChaosRewardKey)
{
	const ACatGameMode* GM = ResolveCatGameMode(WorldContextObject);
	if (!GM || !GM->ChaosRewardTable || ChaosRewardKey.IsNone()) return false;

	const FChaosRewardData* Row = GM->ChaosRewardTable->FindRow<FChaosRewardData>(ChaosRewardKey, TEXT("IsHeavyProp"));
	return Row && Row->bHeavyTier;
}

bool ACatGameMode::ShouldImpactShatter(UObject* WorldContextObject,
                                       FName ChaosRewardKey,
                                       UPrimitiveComponent* VictimComp,
                                       UPrimitiveComponent* ImpactorComp,
                                       FVector NormalImpulse)
{
	const ACatGameMode* GM = ResolveCatGameMode(WorldContextObject);
	if (!GM)
	{
		return false;   // no authority here; the server's decision is the real one.
	}

	bool  bHeavy       = false;
	float VelThreshold = GM->DefaultImpactVelocityThreshold;
	float ImpThreshold = GM->DefaultImpactImpulseThreshold;

	if (GM->ChaosRewardTable && !ChaosRewardKey.IsNone())
	{
		if (const FChaosRewardData* Row = GM->ChaosRewardTable->FindRow<FChaosRewardData>(ChaosRewardKey, TEXT("ShouldImpactShatter")))
		{
			bHeavy = Row->bHeavyTier;
			if (Row->ImpactVelocityThreshold > 0.0f) VelThreshold = Row->ImpactVelocityThreshold;
			if (Row->ImpactImpulseThreshold  > 0.0f) ImpThreshold = Row->ImpactImpulseThreshold;
		}
	}

	// The victim's own speed — the quantity the original graph measured.
	const float VictimSpeed = VictimComp ? VictimComp->GetComponentVelocity().Size() : 0.0f;

	if (!bHeavy)
	{
		return VictimSpeed > VelThreshold;
	}

	// ── Heavy: measure the IMPACTOR, two ways, and record both. ──
	const float ImpulseMag = NormalImpulse.Size();

	// Deterministic fallback: it is the impactor's own numbers, which is what
	// "how hard did the thing that hit it hit it" actually means.
	float ImpactorMass  = 0.0f;
	float ImpactorSpeed = 0.0f;
	if (ImpactorComp)
	{
		ImpactorMass  = ImpactorComp->GetMass();
		ImpactorSpeed = ImpactorComp->GetComponentVelocity().Size();
	}
	const float MassBySpeed = ImpactorMass * ImpactorSpeed;

	const bool bShatter = ImpulseMag > ImpThreshold;

	UE_LOG(LogCatVentures, Log,
		TEXT("[Heavy] impact on '%s' — NormalImpulse=%.1f (threshold %.1f) | fallback mass*speed=%.1f ")
		TEXT("(mass %.1f x speed %.1f) | victimSpeed=%.1f -> %s"),
		*ChaosRewardKey.ToString(), ImpulseMag, ImpThreshold, MassBySpeed,
		ImpactorMass, ImpactorSpeed, VictimSpeed, bShatter ? TEXT("SHATTER") : TEXT("no"));

	return bShatter;
}

// ══════════════════════════════════════════════════════════════════════════
// ── Objectives ───────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void ACatGameMode::StartPlay()
{
	// Super dispatches BeginPlay to every spawned actor (via WorldSettings->
	// NotifyBeginPlay), so every UCatObjectiveTargetComponent has registered by the
	// time this returns. That makes the line below the "world init settled" anchor.
	Super::StartPlay();

	bObjectiveRegistrationClosed = true;

	UE_LOG(LogCatVentures, Log, TEXT("[Objective] Registration closed — %d target(s) registered."),
		ObjectiveTargets.Num());

	ValidateObjectives();
	InitializeObjectiveStates();

	// First evaluation runs against the COMPLETE registry, never a partial one.
	if (MapObjectiveTable)
	{
		for (const FName& RowName : MapObjectiveTable->GetRowNames())
		{
			EvaluateObjective(RowName);
		}
	}
	PushObjectiveStatesToGameState();
}

void ACatGameMode::RegisterObjectiveTarget(UCatObjectiveTargetComponent* Target)
{
	if (!Target) return;

	ObjectiveTargets.AddUnique(Target);

	// A late registration means a streamed objective target — which silently changes
	// the HUD denominator mid-match and invalidates the one-shot validation. The fix
	// is an authoring one (put objective targets on an always-loaded data layer), so
	// say so loudly rather than papering over it.
	if (bObjectiveRegistrationClosed)
	{
		UE_LOG(LogCatVentures, Warning,
			TEXT("[Objective] Target '%s' (tag %s) registered AFTER registration closed — ")
			TEXT("objective targets must live on an always-loaded data layer."),
			*GetNameSafe(Target->GetOwner()), *Target->ObjectiveTag.ToString());
	}
}

void ACatGameMode::UnregisterObjectiveTarget(UCatObjectiveTargetComponent* Target)
{
	ObjectiveTargets.Remove(Target);
}

void ACatGameMode::NotifyObjectiveTargetChanged(UCatObjectiveTargetComponent* Target)
{
	if (!Target || !MapObjectiveTable) return;

	bool bAnyChanged = false;
	for (const FName& RowName : MapObjectiveTable->GetRowNames())
	{
		const FMapObjectiveRow* Row = MapObjectiveTable->FindRow<FMapObjectiveRow>(RowName, TEXT("NotifyObjectiveTargetChanged"));
		if (!Row) continue;

		// Only re-evaluate rows that actually reference this tag.
		const bool bReferencesTag = Row->Conditions.ContainsByPredicate(
			[Target](const FObjectiveCondition& C) { return C.TargetTag == Target->ObjectiveTag; });

		if (bReferencesTag)
		{
			bAnyChanged |= EvaluateObjective(RowName);
		}
	}

	if (bAnyChanged)
	{
		PushObjectiveStatesToGameState();
	}
}

void ACatGameMode::ValidateObjectives()
{
	if (!MapObjectiveTable)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Objective] No MapObjectiveTable assigned — objective system inert."));
		return;
	}

	int32 FinaleRows = 0;
	for (const FName& RowName : MapObjectiveTable->GetRowNames())
	{
		const FMapObjectiveRow* Row = MapObjectiveTable->FindRow<FMapObjectiveRow>(RowName, TEXT("ValidateObjectives"));
		if (!Row) continue;

		if (Row->bIsFinaleSetPiece) { ++FinaleRows; }

		if (Row->Conditions.Num() == 0)
		{
			UE_LOG(LogCatVentures, Warning, TEXT("[Objective] Row '%s' has NO conditions — it would complete vacuously."),
				*RowName.ToString());
			continue;
		}

		for (int32 i = 0; i < Row->Conditions.Num(); ++i)
		{
			const FObjectiveCondition& C = Row->Conditions[i];

			if (C.TargetTag.IsNone())
			{
				UE_LOG(LogCatVentures, Warning, TEXT("[Objective] Row '%s' condition %d has no TargetTag."),
					*RowName.ToString(), i);
				continue;
			}

			// THE vacuous-completion guard: "all matching tags" over an empty set is
			// trivially true, so a typo'd tag would complete at spawn.
			int32 Total = 0, Satisfied = 0;
			CountTargetsForCondition(C, Total, Satisfied);
			if (Total == 0)
			{
				UE_LOG(LogCatVentures, Warning,
					TEXT("[Objective] Row '%s' condition %d targets tag '%s' — NO registered targets carry it. ")
					TEXT("This condition would complete vacuously."),
					*RowName.ToString(), i, *C.TargetTag.ToString());
			}
			else if (C.CountRequired > Total)
			{
				UE_LOG(LogCatVentures, Warning,
					TEXT("[Objective] Row '%s' condition %d needs %d of tag '%s' but only %d exist — unreachable."),
					*RowName.ToString(), i, C.CountRequired, *C.TargetTag.ToString(), Total);
			}
		}
	}

	if (FinaleRows != 1)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Objective] Expected exactly 1 finale set-piece row, found %d."), FinaleRows);
	}
}

void ACatGameMode::InitializeObjectiveStates()
{
	ObjectiveStates.Reset();
	if (!MapObjectiveTable) return;

	for (const FName& RowName : MapObjectiveTable->GetRowNames())
	{
		const FMapObjectiveRow* Row = MapObjectiveTable->FindRow<FMapObjectiveRow>(RowName, TEXT("InitializeObjectiveStates"));
		if (!Row) continue;

		FObjectiveState State;
		State.RowName     = RowName;
		State.DisplayName = Row->DisplayName;
		State.bIsFinale   = Row->bIsFinaleSetPiece;
		State.ConditionProgress.SetNumZeroed(Row->Conditions.Num());
		State.ConditionRequired.SetNumZeroed(Row->Conditions.Num());

		// Resolve each condition's denominator ONCE, here — registration has closed,
		// so the registry count is final and cannot drift mid-match.
		for (int32 i = 0; i < Row->Conditions.Num(); ++i)
		{
			int32 Total = 0, Satisfied = 0;
			CountTargetsForCondition(Row->Conditions[i], Total, Satisfied);
			const int32 Required = (Row->Conditions[i].CountRequired > 0) ? Row->Conditions[i].CountRequired : Total;
			State.ConditionRequired[i] = static_cast<uint8>(FMath::Clamp(Required, 0, 255));
		}

		ObjectiveStates.Add(MoveTemp(State));
	}
}

void ACatGameMode::CountTargetsForCondition(const FObjectiveCondition& Condition,
                                            int32& OutTotal, int32& OutSatisfied) const
{
	OutTotal = 0;
	OutSatisfied = 0;

	for (const TWeakObjectPtr<UCatObjectiveTargetComponent>& Weak : ObjectiveTargets)
	{
		const UCatObjectiveTargetComponent* Target = Weak.Get();
		if (!Target || Target->ObjectiveTag != Condition.TargetTag) continue;

		++OutTotal;

		switch (Condition.Type)
		{
		case EObjectiveConditionType::Destroy:
			if (Target->IsDestroyed()) { ++OutSatisfied; }
			break;

		// Milestone 1a evaluates Destroy only. Relocate/KnockOff are in the schema
		// so the table and the HUD do not change shape when they land; their
		// machinery is the shared overlap path and arrives with milestone 1b.
		case EObjectiveConditionType::Relocate:
		case EObjectiveConditionType::KnockOff:
		default:
			break;
		}
	}
}

bool ACatGameMode::EvaluateObjective(FName RowName)
{
	if (!MapObjectiveTable) return false;

	const FMapObjectiveRow* Row = MapObjectiveTable->FindRow<FMapObjectiveRow>(RowName, TEXT("EvaluateObjective"));
	if (!Row) return false;

	FObjectiveState* State = ObjectiveStates.FindByPredicate(
		[RowName](const FObjectiveState& S) { return S.RowName == RowName; });
	if (!State) return false;

	bool bChanged = false;

	for (int32 i = 0; i < Row->Conditions.Num(); ++i)
	{
		const int32 Bit = 1 << i;

		// LATCH-ON-TRUE: once set, never re-examined. The naive level-triggered
		// re-check is WRONG — a prop settling back out of a volume must not
		// un-complete anything.
		if ((State->LatchedMask & Bit) != 0) continue;

		int32 Total = 0, Satisfied = 0;
		CountTargetsForCondition(Row->Conditions[i], Total, Satisfied);

		if (State->ConditionProgress.IsValidIndex(i))
		{
			const uint8 Clamped = static_cast<uint8>(FMath::Clamp(Satisfied, 0, 255));
			if (State->ConditionProgress[i] != Clamped)
			{
				State->ConditionProgress[i] = Clamped;
				bChanged = true;
			}
		}

		// CountRequired 0 = ALL matching tags; >0 = any N of them. Guard Total>0 so
		// an empty tag set can never satisfy "all of them" vacuously.
		const int32 Required = (Row->Conditions[i].CountRequired > 0) ? Row->Conditions[i].CountRequired : Total;
		const bool bSatisfied = (Total > 0) && (Satisfied >= Required);

		if (bSatisfied)
		{
			State->LatchedMask |= Bit;
			bChanged = true;

			UE_LOG(LogCatVentures, Log, TEXT("[Objective] '%s' condition %d LATCHED (%d/%d of tag '%s')."),
				*RowName.ToString(), i, Satisfied, Required, *Row->Conditions[i].TargetTag.ToString());
		}
	}

	// Objective complete iff every condition latched (AND).
	const int32 AllBits = (Row->Conditions.Num() >= 32) ? ~0 : ((1 << Row->Conditions.Num()) - 1);
	const bool bNowComplete = (Row->Conditions.Num() > 0) && ((State->LatchedMask & AllBits) == AllBits);

	if (bNowComplete && !State->bComplete)
	{
		State->bComplete = true;
		bChanged = true;

		UE_LOG(LogCatVentures, Log, TEXT("[Objective] === COMPLETE: '%s' (%s) ==="),
			*RowName.ToString(), *Row->DisplayName.ToString());
	}

	return bChanged;
}

void ACatGameMode::PushObjectiveStatesToGameState()
{
	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->ObjectiveStates = ObjectiveStates;

		// The host is also a client: replication does not fire OnRep locally, so the
		// listen server's own HUD would never update without this.
		GS->OnObjectiveStatesChanged.Broadcast();
	}
}

// ── Pawn Spawn ──────────────────────────────────────────────────────
//
// AGameModeBase::SpawnDefaultPawnAtTransform_Implementation leaves
// SpawnCollisionHandlingOverride at Undefined, which falls back to the pawn CDO's
// setting. With a single PlayerStart and two joining players, that path can produce
// a pawn that spawns encroached on the existing host's pawn, leaving the joiner
// without a usable view target — the "JOIN TRUE → black screen" symptom. Forcing
// AdjustIfPossibleButAlwaysSpawn lets the engine nudge the pawn out of overlap when
// possible, while still guaranteeing a pawn comes back from the spawn call.

APawn* ACatGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Instigator = GetInstigator();
	SpawnInfo.ObjectFlags |= RF_Transient;
	SpawnInfo.bDeferConstruction = false;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	APawn* ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, SpawnTransform, SpawnInfo);

	if (!ResultPawn)
	{
		UE_LOG(LogCatVentures, Warning,
			TEXT("ACatGameMode::SpawnDefaultPawnAtTransform — SpawnActor returned null. PawnClass=%s"),
			PawnClass ? *PawnClass->GetName() : TEXT("<null>"));
	}
	return ResultPawn;
}

// ── Score Reporting ─────────────────────────────────────────────────

void ACatGameMode::ReportItemDestroyed(AActor* Item, FVector Location, FName ChaosRewardKey)
{
	if (CurrentPhase != ECatMatchPhase::Playing) return;

	const FVector RawLocation = Location;
	const FString ItemPathBefore = Item ? Item->GetName() : FString(TEXT("<null>"));

	// Defensive: if BP forgets to wire the Location pin, fall back to the actor's transform.
	// Prevents the "Aftermath orbit camera at map origin" bug.
	if (Location.IsNearlyZero() && Item)
	{
		Location = Item->GetActorLocation();
	}

	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] ReportItemDestroyed  Item=%s  RawLoc=(%.1f, %.1f, %.1f)  FinalLoc=(%.1f, %.1f, %.1f)  Key=%s"),
		*ItemPathBefore, RawLocation.X, RawLocation.Y, RawLocation.Z,
		Location.X, Location.Y, Location.Z, *ChaosRewardKey.ToString());

	// Resolve the reward row — missing row falls back to DefaultChaosValue.
	float Value = DefaultChaosValue;
	FString ItemName = ChaosRewardKey.ToString();

	if (ChaosRewardTable && !ChaosRewardKey.IsNone())
	{
		if (const FChaosRewardData* Row = ChaosRewardTable->FindRow<FChaosRewardData>(
				ChaosRewardKey, TEXT("ReportItemDestroyed")))
		{
			Value = Row->ChaosValue;
			if (!Row->DisplayName.IsEmpty()) ItemName = Row->DisplayName.ToString();
		}
	}

	// Record the destruction.
	FDestroyedItemRecord Record;
	Record.Location = Location;
	Record.Value    = Value;
	Record.ItemName = ItemName;
	DestroyedItems.Add(Record);

	// Objective routing. Every objective target today is also a chaos prop, so the
	// existing break path is a free, zero-Blueprint-edit report. Targets that are
	// NOT chaos props call UCatObjectiveTargetComponent::ReportDestroyed directly —
	// the pipeline must not assume "objective target" implies "scores chaos".
	if (Item)
	{
		if (UCatObjectiveTargetComponent* ObjTarget = Item->FindComponentByClass<UCatObjectiveTargetComponent>())
		{
			ObjTarget->ReportDestroyed();
		}
	}

	// Accumulate score and push to GameState for HUD replication.
	TotalChaosScore += Value;

	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->ChaosScore = TotalChaosScore;
	}


	// Threshold check.
	if (TotalChaosScore >= ChaosThreshold)
	{
		FinalBreakLocation = Location;
		FinalBreakActor = Item;
		BeginMatchEnd();
	}
}

// ── Phase 1: The Warning ────────────────────────────────────────────

void ACatGameMode::BeginMatchEnd()
{
	CurrentPhase = ECatMatchPhase::Warning;

	// Slow-mo — WorldSettings.TimeDilation auto-replicates to clients.
	UGameplayStatics::SetGlobalTimeDilation(this, SlowMoDilation);

	// Push phase + break location to GameState.
	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->FinalBreakLocation = FinalBreakLocation;
		GS->MatchPhase = ECatMatchPhase::Warning;
	}

	// Notify every PlayerController via Client RPC.
	NotifyAllControllersPhaseChanged(ECatMatchPhase::Warning, FinalBreakLocation, FinalBreakActor.Get());

	// World timer manager is dilated. To wait X wall-clock seconds at dilation D, schedule X*D
	// game-seconds (those then take X*D / D = X real seconds to elapse).
	const float Dilation     = UGameplayStatics::GetGlobalTimeDilation(this);
	const float ScheduledRate = WarningDuration * Dilation;
	GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
		&ACatGameMode::TransitionToFinalCut, ScheduledRate, false);

	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] Warning  RealT=%.2f GameT=%.2f Dilation=%.2f  WarningDuration=%.2f  -> TransitionToFinalCut after %.2f game-secs (=%.2f real)"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		Dilation, WarningDuration, ScheduledRate, WarningDuration);

}

// ── Phase 2: The Final Cut ──────────────────────────────────────────

void ACatGameMode::TransitionToFinalCut()
{
	CurrentPhase = ECatMatchPhase::FinalCut;

	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->MatchPhase = ECatMatchPhase::FinalCut;
	}

	NotifyAllControllersPhaseChanged(ECatMatchPhase::FinalCut, FinalBreakLocation, FinalBreakActor.Get());

	// Same dilation-aware scheduling as Phase 1.
	const float Dilation     = UGameplayStatics::GetGlobalTimeDilation(this);
	const float ScheduledRate = CinematicHoldDuration * Dilation;
	GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
		&ACatGameMode::TransitionToFade, ScheduledRate, false);

	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] FinalCut RealT=%.2f GameT=%.2f Dilation=%.2f  CinematicHoldDuration=%.2f  -> TransitionToFade after %.2f game-secs (=%.2f real)"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		Dilation, CinematicHoldDuration, ScheduledRate, CinematicHoldDuration);

}

// ── Phase 2b: Fade ──────────────────────────────────────────────────

void ACatGameMode::TransitionToFade()
{
	CurrentPhase = ECatMatchPhase::Fade;

	// End slow-mo HERE so the destruction can finish settling at full speed.
	UGameplayStatics::SetGlobalTimeDilation(this, 1.0f);

	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->MatchPhase = ECatMatchPhase::Fade;
	}

	// IMPORTANT: do NOT fire the Fade RPC yet. We're inside a real-time settle window now —
	// dilation is back to 1.0 but the screen stays clear so the player can watch the chaos
	// finish unfolding. After PostCinematicHoldDuration we trigger the actual fade.
	GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
		&ACatGameMode::BeginActualFade, PostCinematicHoldDuration, false);

	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] Fade-Hold RealT=%.2f GameT=%.2f Dilation=1.0  PostCinematicHoldDuration=%.2f  -> BeginActualFade in %.2fs"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		PostCinematicHoldDuration, PostCinematicHoldDuration);

}

// ── Phase 2c: Actual Fade Trigger ───────────────────────────────────

void ACatGameMode::BeginActualFade()
{
	// Now (and only now) tell every client to start fading to black via PlayerCameraManager.
	NotifyAllControllersPhaseChanged(ECatMatchPhase::Fade, FinalBreakLocation, FinalBreakActor.Get());

	// Dilation is 1.0 here, so the multiplier is a no-op — kept for symmetry with the other phases.
	const float Dilation     = UGameplayStatics::GetGlobalTimeDilation(this);
	const float ScheduledRate = FadeDuration * Dilation;
	GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
		&ACatGameMode::TransitionToAftermath, ScheduledRate, false);

	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] Fade-RPC  RealT=%.2f GameT=%.2f Dilation=%.2f  FadeDuration=%.2f  -> TransitionToAftermath after %.2f game-secs (=%.2f real)"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		Dilation, FadeDuration, ScheduledRate, FadeDuration);

}

// ── Phase 3: The Aftermath ──────────────────────────────────────────

void ACatGameMode::TransitionToAftermath()
{
	CurrentPhase = ECatMatchPhase::Aftermath;

	// ── Step 1: sort destruction records (server-authoritative) ──
	DestroyedItems.Sort([](const FDestroyedItemRecord& A, const FDestroyedItemRecord& B)
	{
		return A.Value > B.Value;
	});

	// ── Step 2: COMPUTE the hotspot BEFORE any RPC fires ──
	// This must happen first; the value is captured into the local before being used below.
	const FVector Hotspot = ComputeChaosHotspot();

	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] Aftermath ENTER  RealT=%.2f  DestroyedItems=%d  ComputedHotspot=(%.1f, %.1f, %.1f)"),
		GetWorld()->GetRealTimeSeconds(), DestroyedItems.Num(),
		Hotspot.X, Hotspot.Y, Hotspot.Z);

	// ── Step 3: persist state on GameState BEFORE the RPC ──
	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->TopDestroyedLocations.Reset();
		const int32 Count = FMath::Min(DestroyedItems.Num(), 3);
		for (int32 i = 0; i < Count; ++i)
		{
			GS->TopDestroyedLocations.Add(DestroyedItems[i].Location);
		}

		// MVP even-split scoreboard — every connected player credited an equal slice of the total.
		GS->PlayerScores.Reset();
		const int32 NumPlayers = GS->PlayerArray.Num();
		if (NumPlayers > 0)
		{
			const int32 SharePerPlayer = FMath::RoundToInt(TotalChaosScore / static_cast<float>(NumPlayers));
			const int32 ItemsShare = FMath::RoundToInt(static_cast<float>(DestroyedItems.Num()) / static_cast<float>(NumPlayers));
			for (const TObjectPtr<APlayerState>& PS : GS->PlayerArray)
			{
				if (!PS) continue;
				FCatPlayerScore Entry;
				Entry.PlayerName     = PS->GetPlayerName();
				Entry.Score          = SharePerPlayer;
				Entry.ItemsDestroyed = ItemsShare;
				GS->PlayerScores.Add(Entry);
			}
		}

		GS->AftermathHotspot = Hotspot;
		GS->MatchPhase = ECatMatchPhase::Aftermath;
	}

	// ── Step 4: fire the RPC carrying the just-computed Hotspot ──
	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] Aftermath RPC firing  Hotspot=(%.1f, %.1f, %.1f)"),
		Hotspot.X, Hotspot.Y, Hotspot.Z);
	NotifyAllControllersPhaseChanged(ECatMatchPhase::Aftermath, Hotspot, nullptr);

}

// ── Chaos Hotspot ───────────────────────────────────────────────────

FVector ACatGameMode::ComputeChaosHotspot() const
{
	if (DestroyedItems.Num() == 0)
	{
		return FinalBreakLocation;
	}

	// Bucket-histogram approach: cells of AftermathCellSize accumulate weight
	// (sum of Value) and a weight-multiplied location sum. The heaviest cell's
	// weighted centroid is the hotspot — fast and good enough for ≤ a few dozen items.
	TMap<FIntVector, float> CellWeights;
	TMap<FIntVector, FVector> CellWeightedLocSums;

	for (const FDestroyedItemRecord& Record : DestroyedItems)
	{
		const FIntVector Cell(
			FMath::FloorToInt(Record.Location.X / AftermathCellSize),
			FMath::FloorToInt(Record.Location.Y / AftermathCellSize),
			FMath::FloorToInt(Record.Location.Z / AftermathCellSize));

		CellWeights.FindOrAdd(Cell)        += Record.Value;
		CellWeightedLocSums.FindOrAdd(Cell) += Record.Location * Record.Value;
	}

	FIntVector BestCell{};
	float BestWeight = 0.f;
	for (const TPair<FIntVector, float>& Pair : CellWeights)
	{
		if (Pair.Value > BestWeight)
		{
			BestCell = Pair.Key;
			BestWeight = Pair.Value;
		}
	}

	const FVector Hotspot = (BestWeight > 0.f)
		? CellWeightedLocSums[BestCell] / BestWeight
		: FinalBreakLocation;

	UE_LOG(LogCatVentures, Log,
		TEXT("[CatMatch] ComputeChaosHotspot  DestroyedItems=%d  Hotspot=(%.1f, %.1f, %.1f)  FinalBreakLoc=(%.1f, %.1f, %.1f)"),
		DestroyedItems.Num(), Hotspot.X, Hotspot.Y, Hotspot.Z,
		FinalBreakLocation.X, FinalBreakLocation.Y, FinalBreakLocation.Z);

	return Hotspot;
}

// ── Notify All Controllers ──────────────────────────────────────────

void ACatGameMode::NotifyAllControllersPhaseChanged(ECatMatchPhase NewPhase, FVector PhaseLocation, AActor* TargetActor)
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (ACatPlayerController* PC = Cast<ACatPlayerController>(It->Get()))
		{
			PC->Client_OnMatchPhaseChanged(NewPhase, PhaseLocation, TargetActor);
		}
	}
}
