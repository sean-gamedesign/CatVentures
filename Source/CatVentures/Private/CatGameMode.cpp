// CatGameMode.cpp

#include "CatGameMode.h"
#include "CatGameState.h"
#include "CatPlayerController.h"
#include "CatPlayerState.h"
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

	UE_LOG(LogTemp, Warning,
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

	// Accumulate score and push to GameState for HUD replication.
	TotalChaosScore += Value;

	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->ChaosScore = TotalChaosScore;
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Orange,
			FString::Printf(TEXT("Chaos +%.0f  (%.0f / %.0f)"), Value, TotalChaosScore, ChaosThreshold));
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

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] Warning  RealT=%.2f GameT=%.2f Dilation=%.2f  WarningDuration=%.2f  -> TransitionToFinalCut after %.2f game-secs (=%.2f real)"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		Dilation, WarningDuration, ScheduledRate, WarningDuration);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
			TEXT("MATCH END — Phase 1: The Warning"));
	}
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

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] FinalCut RealT=%.2f GameT=%.2f Dilation=%.2f  CinematicHoldDuration=%.2f  -> TransitionToFade after %.2f game-secs (=%.2f real)"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		Dilation, CinematicHoldDuration, ScheduledRate, CinematicHoldDuration);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
			TEXT("MATCH END — Phase 2: The Final Cut"));
	}
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

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] Fade-Hold RealT=%.2f GameT=%.2f Dilation=1.0  PostCinematicHoldDuration=%.2f  -> BeginActualFade in %.2fs"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		PostCinematicHoldDuration, PostCinematicHoldDuration);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
			TEXT("MATCH END — Phase 2b: Real-Time Hold (no fade yet)"));
	}
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

	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] Fade-RPC  RealT=%.2f GameT=%.2f Dilation=%.2f  FadeDuration=%.2f  -> TransitionToAftermath after %.2f game-secs (=%.2f real)"),
		GetWorld()->GetRealTimeSeconds(), GetWorld()->GetTimeSeconds(),
		Dilation, FadeDuration, ScheduledRate, FadeDuration);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
			TEXT("MATCH END — Phase 2b: Fade RPC firing"));
	}
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

	UE_LOG(LogTemp, Warning,
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
	UE_LOG(LogTemp, Warning,
		TEXT("[CatMatch] Aftermath RPC firing  Hotspot=(%.1f, %.1f, %.1f)"),
		Hotspot.X, Hotspot.Y, Hotspot.Z);
	NotifyAllControllersPhaseChanged(ECatMatchPhase::Aftermath, Hotspot, nullptr);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
			FString::Printf(TEXT("MATCH END — Phase 3: Aftermath at %s"), *Hotspot.ToString()));
	}
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

	UE_LOG(LogTemp, Warning,
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
