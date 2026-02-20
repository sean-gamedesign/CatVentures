// InteractableLoot.h — Simple test actor for the Interaction System

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InteractableInterface.h"
#include "InteractableLoot.generated.h"

/**
 * Test actor: implements IInteractableInterface.
 * Prints a debug message and destroys itself when interacted with.
 * bReplicates = true so Destroy() propagates to all clients.
 */
UCLASS()
class CATVENTURES_API AInteractableLoot : public AActor, public IInteractableInterface
{
	GENERATED_BODY()

public:
	AInteractableLoot();

protected:
	/** IInteractableInterface — C++ implementation. */
	virtual void Interact_Implementation(AActor* Interactor) override;

private:
	/** Visual representation (default cube — swap in Blueprint if desired). */
	UPROPERTY(VisibleAnywhere, Category = "Loot")
	TObjectPtr<UStaticMeshComponent> LootMesh;
};
