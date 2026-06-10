// InteractableLoot.cpp

#include "InteractableLoot.h"
#include "CatVenturesLog.h"
#include "Engine/Engine.h"

AInteractableLoot::AInteractableLoot()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	LootMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("LootMesh"));
	RootComponent = LootMesh;

	// Assign a default cube mesh so the actor is visible out of the box.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		LootMesh->SetStaticMesh(CubeMesh.Object);
		LootMesh->SetWorldScale3D(FVector(0.5f));
	}
}

void AInteractableLoot::Interact_Implementation(AActor* Interactor)
{
	if (!HasAuthority()) return;

	const FString InstigatorName = Interactor ? Interactor->GetName() : TEXT("Unknown");

	UE_LOG(LogCatVentures, Log, TEXT("AInteractableLoot::Interact — Loot collected by %s!"), *InstigatorName);


	Destroy();
}
