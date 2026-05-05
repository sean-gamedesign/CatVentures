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
