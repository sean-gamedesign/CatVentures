// CatAnimationTypes.h — Native enums migrated from CharBP_Base (AnimX asset pack)

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

// ── Action ────────────────────────────────────────────────────────────────

/** Special contextual actions the cat can perform. */
UENUM(BlueprintType)
enum class ECatAction : uint8
{
	None,
	Eat,
	Drink,
	Dig,
	JumpIntoSnow	UMETA(DisplayName = "Jump Into Snow"),
	Howl,
	Lick,
	SharpenClaws	UMETA(DisplayName = "Sharpens Claws"),
	Pet,
	Roar,
	Rub,
	WavesHello		UMETA(DisplayName = "Waves Hello"),
	DropItem		UMETA(DisplayName = "Drop Item")
};

// ── Control Mode ──────────────────────────────────────────────────────────

/** How the character interprets camera/input for rotation. */
UENUM(BlueprintType)
enum class ECatControlMode : uint8
{
	Simple,
	Looking,
	Behind
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

// ── Aim Mode ──────────────────────────────────────────────────────────────

/** Head/body aim mode for look-at and aiming blendspaces. */
UENUM(BlueprintType)
enum class ECatAim : uint8
{
	None,
	Aim,
	LookAt,
	AtCamera	UMETA(DisplayName = "AtCamara")
};

// ── Anim Blendspace Mode ──────────────────────────────────────────────────

/** Which blendspace set the AnimBP should use. */
UENUM(BlueprintType)
enum class ECatAnimBSMode : uint8
{
	Simple,
	Looking,
	AI
};

// ── Base Action ───────────────────────────────────────────────────────────

/** Priority action state that overrides normal locomotion (combat, damage, death). */
UENUM(BlueprintType)
enum class ECatBaseAction : uint8
{
	None,
	Attack,
	Shaking,
	Dead,
	Damage,
	EnterToWater	UMETA(DisplayName = "Enter to Water")
};

// ── Rest State ────────────────────────────────────────────────────────────

/** Idle rest progression: Standing → Sitting → Lying → Sleeping. */
UENUM(BlueprintType)
enum class ECatRest : uint8
{
	None,
	Stand,
	Sit,
	Lie,
	Sleep,
	NearEdge	UMETA(DisplayName = "NearEdge")
};

// ── Turn In Place ─────────────────────────────────────────────────────────

/** When the character should play a procedural turn-in-place animation. */
UENUM(BlueprintType)
enum class ECatTurnInPlace : uint8
{
	None,
	Wait,
	Always
};

// ── Damage Direction ──────────────────────────────────────────────────────

/** Direction the damage came from — selects the correct hit-react montage. */
UENUM(BlueprintType)
enum class ECatDamageDirection : uint8
{
	BL	UMETA(DisplayName = "Back Left"),
	BR	UMETA(DisplayName = "Back Right"),
	FL	UMETA(DisplayName = "Front Left"),
	FR	UMETA(DisplayName = "Front Right"),
	SL	UMETA(DisplayName = "Side Left"),
	SR	UMETA(DisplayName = "Side Right")
};
