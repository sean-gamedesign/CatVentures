// CatGameState.cpp

#include "CatGameState.h"
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
