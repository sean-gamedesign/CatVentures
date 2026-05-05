// CatPlayerState.h — Per-player replicated state. Owns the rematch-ready flag.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "CatPlayerState.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRematchReadyChanged, bool, bReady);

UCLASS()
class CATVENTURES_API ACatPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** True when this player has clicked "Play Again" on the scoreboard.
	 *  Host watches this to gate the lobby rematch button until everyone is ready. */
	UPROPERTY(ReplicatedUsing = OnRep_WantsRematch, BlueprintReadOnly, Category = "Match")
	bool bWantsRematch = false;

	/** Broadcast on every client (including the setter) when bWantsRematch flips.
	 *  Scoreboard widgets bind to this to refresh per-row ready indicators. */
	UPROPERTY(BlueprintAssignable, Category = "Match")
	FOnRematchReadyChanged OnRematchReadyChanged;

protected:
	UFUNCTION()
	void OnRep_WantsRematch();
};
