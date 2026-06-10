// CatAnimationTypes.h — Native enums consumed by ACatBase and ABP_Cat_V2.
// (Unused enums ported from the AnimX CharBP_Base were removed in the 2026-06 cleanup;
//  recover them from git history if a future feature needs them.)

#pragma once

#include "CoreMinimal.h"

// ── Movement Speed Type ───────────────────────────────────────────────────

/** Discrete speed tier — drives locomotion blendspace selection. */
UENUM(BlueprintType)
enum class ECatMoveType : uint8
{
	Idle,
	Walk,
	Trot,
	Run,
	Crouch,
	Turn
};

// ── Movement Stage ────────────────────────────────────────────────────────

/** High-level locomotion surface/state. */
UENUM(BlueprintType)
enum class ECatMovementStage : uint8
{
	OnGround	UMETA(DisplayName = "onGround"),
	InAir		UMETA(DisplayName = "inAir"),
	Swimming,
	Ragdoll
};

// ── Jump Phase ────────────────────────────────────────────────────────────

/** Discrete jump phase — drives the AnimBP jump state machine transitions. */
UENUM(BlueprintType)
enum class ECatJumpPhase : uint8
{
	None,		// On ground, not jumping
	Launch,		// Just left ground, ascending          (AnimX: JumpStart)
	Apex,		// Near peak, |Vz| < threshold          (AnimX: JumpApex)
	Fall,		// Descending                            (AnimX: JumpFall)
	Land		// Just touched ground, in recovery      (AnimX: JumpLand)
};
