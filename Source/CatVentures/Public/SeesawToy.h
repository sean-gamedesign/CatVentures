// SeesawToy.h — Physics-constrained seesaw toy for the CatVentures sandbox

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SeesawToy.generated.h"

class UStaticMeshComponent;
class UPhysicsConstraintComponent;

/**
 * A seesaw toy consisting of a static base pedestal and a physics-simulated plank
 * hinged at the centre via a UPhysicsConstraintComponent.
 *
 * Multiplayer: bReplicates = true. PlankMesh uses SetIsReplicated(true) so its
 * simulated movement is synced to all clients independently of the static root.
 *
 * Usage: Create a Blueprint child (e.g. BP_SeesawToy), assign meshes to BaseMesh
 * and PlankMesh in the Component panel, and position PlankConstraint at the fulcrum.
 */
UCLASS()
class CATVENTURES_API ASeesawToy : public AActor
{
	GENERATED_BODY()

public:
	ASeesawToy();

	// ── Components ──────────────────────────────────────────────────────

	/** Static world anchor. Neither mesh is the root so both can be repositioned freely. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Seesaw")
	TObjectPtr<USceneComponent> SeesawRoot;

	/** Non-simulating pedestal mesh. Swap in the Blueprint Component panel. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Seesaw")
	TObjectPtr<UStaticMeshComponent> BaseMesh;

	/** Physics-simulated plank. Swap in the Blueprint Component panel. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Seesaw")
	TObjectPtr<UStaticMeshComponent> PlankMesh;

	/** Hinge constraint — position at the fulcrum in the Blueprint viewport. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Seesaw")
	TObjectPtr<UPhysicsConstraintComponent> PlankConstraint;

	// ── Tuning ──────────────────────────────────────────────────────────

	/** Max tilt angle (degrees) in either direction. Applied to Angular Swing2 (pitch).
	 *  85° hard cap prevents gimbal/constraint instability near 90°. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seesaw", meta = (ClampMin = "1.0", ClampMax = "85.0"))
	float PlankTiltLimitDegrees = 30.0f;

	/** Physics mass of the plank (kg). 100 kg matches the project's Realistic Mass standard
	 *  and ensures a cat (~4 kg equivalent) tilts it without launching violently. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seesaw", meta = (ClampMin = "1.0"))
	float PlankMassKg = 100.0f;
};
