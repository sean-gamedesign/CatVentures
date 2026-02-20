// InteractableInterface.h — Universal interaction interface for CatVentures

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "InteractableInterface.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UInteractableInterface : public UInterface
{
	GENERATED_BODY()
};

class IInteractableInterface
{
	GENERATED_BODY()

public:
	/**
	 * Called when an actor interacts with this object.
	 * BlueprintNativeEvent — override in C++ via Interact_Implementation() or in Blueprint.
	 *
	 * @param Instigator  The actor performing the interaction (typically ACatBase).
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Interaction")
	void Interact(AActor* Interactor);
};
