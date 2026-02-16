// CatBase.h — Multiplayer-ready Character base for CatVentures

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "CatBase.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class USpringArmComponent;
class UCameraComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMeowDelegate);

/**
 * Base C++ Character for all Cat pawns.
 *
 * Multiplayer features:
 *  - Hard Tick Gate: Tick() early-returns on non-locally-controlled instances
 *    so Blueprint tick logic only runs where it matters.
 *  - PossessedBy / OnRep_PlayerState force Walking movement mode immediately,
 *    preventing the "frozen client" problem.
 *  - Server_Meow RPC → NetMulticast_Meow → OnMeow broadcast for networked meowing.
 */
UCLASS()
class CATVENTURES_API ACatBase : public ACharacter
{
	GENERATED_BODY()

public:
	ACatBase();

	//~ Begin AActor Interface
	virtual void Tick(float DeltaTime) override;
	//~ End AActor Interface

	/** Broadcast on all machines when this cat meows. */
	UPROPERTY(BlueprintAssignable, Category = "Cat")
	FOnMeowDelegate OnMeow;

	// ── Camera ─────────────────────────────────────────────────────────

	/** Spring arm that holds the follow camera behind the cat. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	/** Third-person follow camera. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	// ── Camera Tuning ──────────────────────────────────────────────

	/** Sensitivity multiplier applied to mouse/stick look input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float LookSensitivity = 1.0f;

	/** Pitch clamp (degrees) — how far the camera can look up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float PitchClampUp = 60.0f;

	/** Pitch clamp (degrees) — how far the camera can look down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float PitchClampDown = 70.0f;

	/** Enable positional camera lag on the spring arm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bEnableCameraLag = true;

	/** Speed of positional camera lag (higher = snappier). Only used when bEnableCameraLag is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", EditCondition = "bEnableCameraLag"))
	float CameraLagSpeed = 10.0f;

	/** Enable rotational camera lag on the spring arm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bEnableCameraRotationLag = true;

	/** Speed of rotational camera lag (higher = snappier). Only used when bEnableCameraRotationLag is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", EditCondition = "bEnableCameraRotationLag"))
	float CameraRotationLagSpeed = 8.0f;

	// ── Tank Controls ──────────────────────────────────────────────

	/** Yaw turn speed in degrees/second when A/D are held. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "30.0", ClampMax = "720.0"))
	float TurnRate = 180.0f;

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	//~ End AActor Interface

	//~ Begin APawn Interface
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	//~ End APawn Interface

	// ── Enhanced Input Assets ────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> MeowAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> JumpAction;

	// ── Input Handlers ──────────────────────────────────────────────

	/** Tank-style input: Y axis (W/S) moves along ActorForward, X axis (A/D) yaw-rotates the character. */
	void Move(const FInputActionValue& Value);

	/** Processes IA_Look (Axis2D) — applies yaw/pitch to the controller rotation. */
	void Look(const FInputActionValue& Value);

	// ── Networked Meow ──────────────────────────────────────────────

	/** Client → Server: request a meow. */
	UFUNCTION(Server, Reliable)
	void Server_Meow();

	/** Server → All: replicate the meow to every machine. */
	UFUNCTION(NetMulticast, Reliable)
	void NetMulticast_Meow();

private:
	/** Forces the CharacterMovementComponent into Walking mode if it is currently None. */
	void ForceWalkingMovementMode();
};
