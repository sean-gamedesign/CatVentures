#pragma once

#include "CoreMinimal.h"
#include "CatImpactTypes.generated.h"

/**
 * Impact-response severity tiers (plan: Saved/.Aura/plans/cat-impact-response-v1.2.md).
 * Severity is ORDINAL — rules speak in "tier >= X" terms so a future tier slots in
 * without touching call sites. v1 slice implements Flinch only; Stagger/Ragdoll are
 * in the schema so the classifier and RPC shapes do not change when they land.
 */
UENUM(BlueprintType)
enum class ECatImpactTier : uint8
{
	None    UMETA(DisplayName = "None"),
	Flinch  UMETA(DisplayName = "Flinch"),
	Stagger UMETA(DisplayName = "Stagger"),
	Ragdoll UMETA(DisplayName = "Ragdoll"),
};

/** Where an impact report came from. The classifier caps tier BY SOURCE:
 *  Swat is capped at Stagger (locked: a direct swat can never ragdoll — no stunlock
 *  tool; ragdolling a rival is always indirect via launched props/traffic).
 *  Authored bypasses measurement entirely (locked: vehicle hits are authored
 *  impulses, not solver collisions). */
UENUM(BlueprintType)
enum class ECatImpactSource : uint8
{
	Swat     UMETA(DisplayName = "Swat"),
	Measured UMETA(DisplayName = "Measured"),
	Authored UMETA(DisplayName = "Authored"),
};

/** Hit direction in the VICTIM's frame — which side the hit came FROM.
 *  Quantized once, server-side, so every machine picks the same react clip. */
UENUM(BlueprintType)
enum class ECatImpactDirection : uint8
{
	Front UMETA(DisplayName = "Front"),
	Back  UMETA(DisplayName = "Back"),
	Left  UMETA(DisplayName = "Left"),
	Right UMETA(DisplayName = "Right"),
};

/**
 * One impact report, whatever the source (plan §4.4). Only the fields relevant to
 * the source are filled: Swat carries instigator + direction; Measured carries the
 * classifier quantities (impactor mass x speed primary, NormalImpulse logged
 * alongside — the Heavy-tier verify-in-build pattern); Authored carries the tier
 * and launch values verbatim (the car IS the classifier).
 */
USTRUCT(BlueprintType)
struct FCatImpactParams
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	ECatImpactSource Source = ECatImpactSource::Swat;

	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	TObjectPtr<AActor> InstigatorActor = nullptr;

	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	FVector ImpactPoint = FVector::ZeroVector;

	/** Direction of the push, world space (points from the instigator INTO the victim). */
	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	FVector ImpactNormal = FVector::ZeroVector;

	/** Measured source: impactor mass x contact speed — the deterministic classifier
	 *  quantity (NormalImpulse against a kinematic capsule is solver/substep-dependent,
	 *  the same failure family the Heavy tier's verify round covers). */
	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	float ImpactorMassSpeed = 0.0f;

	/** Measured source: |NormalImpulse| — logged alongside for the shared verify round. */
	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	float NormalImpulseMag = 0.0f;

	/** Authored source: the tier, verbatim. */
	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	ECatImpactTier AuthoredTier = ECatImpactTier::None;

	/** Authored source: launch velocity, verbatim. */
	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	FVector AuthoredLaunch = FVector::ZeroVector;

	/** Authored source: angular impulse for the ragdoll pinwheel, verbatim. */
	UPROPERTY(BlueprintReadWrite, Category = "Impact")
	FVector AuthoredAngular = FVector::ZeroVector;
};
