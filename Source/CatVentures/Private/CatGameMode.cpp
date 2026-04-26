// CatGameMode.cpp

#include "CatGameMode.h"
#include "CatGameState.h"
#include "CatPlayerController.h"
#include "Engine/DataTable.h"
#include "Kismet/GameplayStatics.h"

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
	NotifyAllControllersPhaseChanged(ECatMatchPhase::Warning, FinalBreakActor.Get());

	// Schedule Phase 2 transition using dilated time.
	// Timer ticks in game-seconds: RealDelay * Dilation = game-seconds that equal RealDelay wall-seconds.
	const float DilatedDelay = WarningDuration * SlowMoDilation;
	GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
		&ACatGameMode::TransitionToFinalCut, DilatedDelay, false);

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

	NotifyAllControllersPhaseChanged(ECatMatchPhase::FinalCut, FinalBreakActor.Get());

	// Hold on the cinematic camera, then transition to fade.
	const float DilatedDelay = CinematicHoldDuration * SlowMoDilation;
	GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
		&ACatGameMode::TransitionToFade, DilatedDelay, false);

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

	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->MatchPhase = ECatMatchPhase::Fade;
	}

	NotifyAllControllersPhaseChanged(ECatMatchPhase::Fade, FinalBreakActor.Get());

	// Wait for the fade animation to finish, then show the scoreboard.
	const float DilatedDelay = FadeDuration * SlowMoDilation;
	GetWorldTimerManager().SetTimer(PhaseTimerHandle, this,
		&ACatGameMode::TransitionToAftermath, DilatedDelay, false);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
			TEXT("MATCH END — Phase 2b: Fade"));
	}
}

// ── Phase 3: The Aftermath ──────────────────────────────────────────

void ACatGameMode::TransitionToAftermath()
{
	CurrentPhase = ECatMatchPhase::Aftermath;

	// Restore normal time.
	UGameplayStatics::SetGlobalTimeDilation(this, 1.0f);

	// Sort destroyed items by value descending — take top 3 locations.
	DestroyedItems.Sort([](const FDestroyedItemRecord& A, const FDestroyedItemRecord& B)
	{
		return A.Value > B.Value;
	});

	if (ACatGameState* GS = GetGameState<ACatGameState>())
	{
		GS->TopDestroyedLocations.Reset();
		const int32 Count = FMath::Min(DestroyedItems.Num(), 3);
		for (int32 i = 0; i < Count; ++i)
		{
			GS->TopDestroyedLocations.Add(DestroyedItems[i].Location);
		}

		GS->MatchPhase = ECatMatchPhase::Aftermath;
	}

	NotifyAllControllersPhaseChanged(ECatMatchPhase::Aftermath, nullptr);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
			TEXT("MATCH END — Phase 3: The Aftermath"));
	}
}

// ── Notify All Controllers ──────────────────────────────────────────

void ACatGameMode::NotifyAllControllersPhaseChanged(ECatMatchPhase NewPhase, AActor* TargetActor)
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (ACatPlayerController* PC = Cast<ACatPlayerController>(It->Get()))
		{
			PC->Client_OnMatchPhaseChanged(NewPhase, FinalBreakLocation, TargetActor);
		}
	}
}
