// InteractableLoot.cpp

#include "InteractableLoot.h"
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
	const FString InstigatorName = Interactor ? Interactor->GetName() : TEXT("Unknown");

	UE_LOG(LogTemp, Log, TEXT("AInteractableLoot::Interact — Loot collected by %s!"), *InstigatorName);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green,
			FString::Printf(TEXT("Loot collected by %s!"), *InstigatorName));
	}

	Destroy();
}
