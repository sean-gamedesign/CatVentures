#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CatTraversalComponent.generated.h"

class ACatBase;

/** Traversal verb currently owning the character (the JumpPhase pattern — one enum,
 *  one owner). Mantle first; wall bounce / scramble / fence trot / curtain climb
 *  join here per the traversal-verbs plan. */
UENUM(BlueprintType)
enum class ECatTraversalState : uint8
{
	None,
	Mantle,
};

/**
 * Traversal component — owns the wall-detection trace library and every traversal
 * verb's movement takeover (growth-watch decision: new large systems live in
 * components, and there is ONE CMC apply/restore point here so five verbs don't
 * each fight the grab/stop/start/pivot restore precedence).
 *
 * V1 ships the MANTLE (cat clamber): airborne, pushing toward a wall whose lip is
 * within paw reach → the cat catches the ledge and pulls itself up-and-over on a
 * vertical-first curve. Mechanics-first doctrine: runs on placeholder anim, logs
 * its own clip-authoring spec (height/duration per mantle via LogCatVentures).
 *
 * MP model: the local owner detects and drives; Server_StartMantle (on the pawn,
 * the grab pattern) mirrors the takeover server-side, which drives its own copy
 * deterministically from the same start/target — no progress streaming needed.
 * Proxies read the pawn's replicated bGoMantle/MantleProgress for the anim.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class CATVENTURES_API UCatTraversalComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCatTraversalComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Begin the mantle takeover (local owner on detection; server via the pawn RPC). */
	void StartMantle(const FVector& InStart, const FVector& InTarget);

	bool IsMantling() const { return TraversalState == ECatTraversalState::Mantle; }

	// ── Tuning (header-only defaults, live-tunable) ─────────────────────

	/** Master switch for the mantle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle")
	bool bEnableMantle = true;

	/** Forward chest-probe reach (uu) — how far ahead a wall can be caught. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "20.0", ClampMax = "100.0"))
	float MantleReachDistance = 45.0f;

	/** Ledge-lip height band ABOVE the capsule bottom (uu). Below min a step-up handles
	 *  it; above max the wall is a scramble/climb candidate, not a mantle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "10.0", ClampMax = "80.0"))
	float MantleMinLedgeHeight = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "40.0", ClampMax = "200.0"))
	float MantleMaxLedgeHeight = 70.0f;   // 105 shipped v1 — Sean live-tuned: 103+ catches read superhero, 64–74 read cat

	/** How far past the wall face the lip probe starts (uu) — the paw-hook depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "5.0", ClampMax = "50.0"))
	float MantleForwardClearance = 18.0f;

	/** Takeover duration = base + height × per-cm (taller ledges pull longer). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.1", ClampMax = "1.5"))
	float MantleBaseDuration = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "0.01"))
	float MantleDurationPerCm = 0.0015f;

	/** Forward speed handed to the CMC at mantle exit — a soft step-out, the gait
	 *  earns the rest (the start-burst boundary lesson: bake no lunge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "400.0"))
	float MantleExitSpeed = 150.0f;

	/** Re-mantle cooldown (s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float MantleCooldown = 0.25f;

private:
	/** Airborne ledge detection for the locally controlled cat; starts the mantle on a hit. */
	void TryDetectMantle();

	/** Advance the takeover curve on owner and server alike. */
	void DriveMantle(float DeltaTime);

	/** Restore the CMC and clear state. bCompleted = reached the target (vs external abort). */
	void EndMantle(bool bCompleted);

	ACatBase* GetCat() const;

	ECatTraversalState TraversalState = ECatTraversalState::None;

	FVector MantleStart = FVector::ZeroVector;
	FVector MantleTarget = FVector::ZeroVector;
	float MantleDuration = 0.4f;
	float MantleElapsed = 0.0f;
	float CooldownTimer = 0.0f;
};
