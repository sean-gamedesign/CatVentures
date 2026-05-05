// CatPlayerState.cpp

#include "CatPlayerState.h"
#include "Net/UnrealNetwork.h"

void ACatPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ACatPlayerState, bWantsRematch);
}

void ACatPlayerState::OnRep_WantsRematch()
{
	OnRematchReadyChanged.Broadcast(bWantsRematch);
}
