// CatGameState.cpp

#include "CatGameState.h"
#include "CatPlayerState.h"
#include "Net/UnrealNetwork.h"

void ACatGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ACatGameState, MatchPhase);
	DOREPLIFETIME(ACatGameState, ChaosScore);
	DOREPLIFETIME(ACatGameState, ChaosThreshold);
	DOREPLIFETIME(ACatGameState, FinalBreakLocation);
	DOREPLIFETIME(ACatGameState, TopDestroyedLocations);
	DOREPLIFETIME(ACatGameState, PlayerScores);
	DOREPLIFETIME(ACatGameState, AftermathHotspot);
	DOREPLIFETIME(ACatGameState, ObjectiveStates);
}

bool ACatGameState::AllPlayersReadyForRematch() const
{
	if (PlayerArray.Num() == 0) return false;

	for (const TObjectPtr<APlayerState>& PS : PlayerArray)
	{
		const ACatPlayerState* CatPS = Cast<ACatPlayerState>(PS);
		if (!CatPS || !CatPS->bWantsRematch) return false;
	}
	return true;
}

float ACatGameState::GetChaosPercent() const
{
	if (ChaosThreshold <= 0.0f) return 1.0f;
	return FMath::Clamp(ChaosScore / ChaosThreshold, 0.0f, 1.0f);
}

void ACatGameState::OnRep_MatchPhase()
{
	OnMatchPhaseChanged.Broadcast(MatchPhase);
}

void ACatGameState::OnRep_TopDestroyedLocations()
{
	// Phase 3 camera setup can bind to OnMatchPhaseChanged or poll this directly.
}

void ACatGameState::OnRep_ObjectiveStates()
{
	OnObjectiveStatesChanged.Broadcast();
}

bool ACatGameState::AreNonFinaleObjectivesComplete() const
{
	bool bAnyNonFinale = false;

	for (const FObjectiveState& S : ObjectiveStates)
	{
		if (S.bIsFinale) continue;
		bAnyNonFinale = true;
		if (!S.bComplete) return false;
	}

	// No non-finale objectives at all would unlock the finale at spawn. Treat that as
	// LOCKED rather than open — the same vacuous-completion trap the registry guard
	// exists for, one level up.
	return bAnyNonFinale;
}

FText ACatGameState::GetObjectiveChecklistText() const
{
	if (ObjectiveStates.Num() == 0)
	{
		return FText::FromString(TEXT("OBJECTIVES\n  (none — no MapObjectiveTable assigned)"));
	}

	const bool bFinaleUnlocked = AreNonFinaleObjectivesComplete();

	TArray<FString> Lines;
	Lines.Add(TEXT("OBJECTIVES"));

	for (const FObjectiveState& S : ObjectiveStates)
	{
		const FString Name = S.DisplayName.IsEmpty() ? S.RowName.ToString() : S.DisplayName.ToString();

		// The finale reads LOCKED until its gate opens. §5: the checklist teaches the
		// Heavy rather than hoping the Heavy teaches itself — an unbreakable prop
		// reads as "bug" far more naturally than "later".
		if (S.bIsFinale && !S.bComplete && !bFinaleUnlocked)
		{
			Lines.Add(FString::Printf(TEXT("  [LOCKED]  %s"), *Name));
			continue;
		}

		if (S.bComplete)
		{
			// Forgiveness is VISIBLE, never silent — a self-checking objective reads
			// as a bug, an acknowledged one reads as the game having a sense of humour.
			const bool bAnyForgiven = (S.ForgivenMask != 0);
			Lines.Add(FString::Printf(TEXT("  [X]  %s%s"), *Name,
				bAnyForgiven ? TEXT("  (close enough)") : TEXT("")));
			continue;
		}

		// Incomplete: show the fraction. It is playtest instrumentation, not polish —
		// without it players cannot tell a multi-part objective from a broken one.
		int32 Done = 0, Need = 0;
		if (S.ConditionProgress.Num() == 1 && S.ConditionRequired.Num() == 1)
		{
			// Single condition (incl. count-of): the meaningful fraction lives INSIDE it.
			Done = S.ConditionProgress[0];
			Need = S.ConditionRequired[0];
		}
		else
		{
			// Multi-condition AND: fraction is conditions latched / conditions total.
			Need = S.ConditionProgress.Num();
			for (int32 i = 0; i < Need; ++i)
			{
				if ((S.LatchedMask & (1 << i)) != 0) ++Done;
			}
		}

		Lines.Add(FString::Printf(TEXT("  [ ]  %s  (%d/%d)"), *Name, Done, FMath::Max(Need, 1)));
	}

	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}
