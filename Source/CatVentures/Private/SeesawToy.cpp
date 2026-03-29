// SeesawToy.cpp

#include "SeesawToy.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"

ASeesawToy::ASeesawToy()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	// bReplicateMovement intentionally false — root never moves.
	// Plank replication is per-component via SetIsReplicated(true) below.

	// ── Root ─────────────────────────────────────────────────────────
	SeesawRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SeesawRoot"));
	RootComponent = SeesawRoot;

	// ── Base (static pedestal) ────────────────────────────────────────
	BaseMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BaseMesh"));
	BaseMesh->SetupAttachment(RootComponent);
	BaseMesh->SetSimulatePhysics(false);
	BaseMesh->SetCollisionProfileName(TEXT("BlockAll"));

	// ── Plank (simulated) ─────────────────────────────────────────────
	PlankMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlankMesh"));
	PlankMesh->SetupAttachment(RootComponent);
	// SetSimulatePhysics and SetMassOverrideInKg are deferred to BeginPlay —
	// both access FBodyInstance::GetSimplePhysicalMaterial which requires GEngine,
	// and GEngine is not available during CDO construction in a Shipping build.
	PlankMesh->SetIsReplicated(true);
	PlankMesh->SetCollisionProfileName(TEXT("PhysicsActor"));

	// ── Constraint (hinge at fulcrum) ─────────────────────────────────
	PlankConstraint = CreateDefaultSubobject<UPhysicsConstraintComponent>(TEXT("PlankConstraint"));
	PlankConstraint->SetupAttachment(RootComponent);
	// Default position is at the actor origin (centre of plank).
	// Move it in the Blueprint viewport to align with the physical pivot point.

	// Lock all linear DOF — plank cannot translate away from the hinge.
	PlankConstraint->SetLinearXLimit(LCM_Locked, 0.0f);
	PlankConstraint->SetLinearYLimit(LCM_Locked, 0.0f);
	PlankConstraint->SetLinearZLimit(LCM_Locked, 0.0f);

	// Lock Twist (roll) and Swing1 (yaw) — plank can only pitch.
	PlankConstraint->SetAngularTwistLimit(ACM_Locked, 0.0f);
	PlankConstraint->SetAngularSwing1Limit(ACM_Locked, 0.0f);

	// Swing2 (pitch, around local Y) = the seesaw tilt axis. Limited to ±PlankTiltLimitDegrees.
	PlankConstraint->SetAngularSwing2Limit(ACM_Limited, PlankTiltLimitDegrees);

	// Wire the constraint between the static base and the simulated plank.
	// Component references are stored here; the Chaos constraint body is created
	// later during OnRegister() when physics bodies are available.
	PlankConstraint->SetConstrainedComponents(BaseMesh, NAME_None, PlankMesh, NAME_None);
}

void ASeesawToy::BeginPlay()
{
	Super::BeginPlay();

	// Deferred from constructor — requires GEngine (unavailable during CDO construction).
	PlankMesh->SetSimulatePhysics(true);
	PlankMesh->SetMassOverrideInKg(NAME_None, PlankMassKg, /*bOverrideMass=*/true);
}
