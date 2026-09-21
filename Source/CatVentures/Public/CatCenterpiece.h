// CatCenterpiece.h — the finale set piece of the convergence loop ("the Shrine").
//
// One per map. A tapered stack of KINEMATIC Geometry-Collection sections that nothing
// breaks by accident: it is LOCKED until the chaos meter reaches UnlockChaosPercent, and
// once EXPOSED it comes down one section at a time — a stage completes only when EVERY
// cat in the match lands a hit inside a StageWindow. The last section ending the match is
// what puts every player at the same place for the finale (the League-nexus convergence
// read, minus the towers — see the 2026-09-20 core-loop conversation).
//
// Hits arrive three ways and all funnel into ReceiveHit (server only):
//   Swat   — ACatBase::HandleSwatHit's ApplyPointDamage → OnTakePointDamage here.
//   Charge — ACatBase::Multicast_BumperHitGC intercepts the actor before the Heavy gate.
//   Impact — a simulating prop striking a section (OnComponentHit); credited to the last
//            cat that touched that prop (ACatGameMode::ResolveAttacker), else ignored.
//
// Everything a player SEES runs on every machine from two reliable multicasts:
// Multicast_FinaleEvent (feedback beats) and Multicast_ShatterSection (the break itself,
// through the same ForceShatterGC every prop uses). The readout is two TextRender
// billboards above the stack; the HUD line comes from GetHudText via the GameState.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CatMatchTypes.h"
#include "CatCenterpiece.generated.h"

class UGeometryCollectionComponent;
class UGeometryCollection;
class UTextRenderComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UCameraShakeBase;
class APlayerState;

UCLASS()
class CATVENTURES_API ACatCenterpiece : public AActor
{
	GENERATED_BODY()

public:
	ACatCenterpiece();

	/** Upper bound on sections — they are default subobjects, so the count is fixed at
	 *  construction and SectionScales picks how many are live. */
	static constexpr int32 MaxSections = 6;

	// ── Body ────────────────────────────────────────────────────────

	/** The Geometry Collection each section is built from (GC_Cylinder by default). */
	UPROPERTY(EditAnywhere, Category = "Finale|Body")
	TObjectPtr<UGeometryCollection> SectionAsset;

	/** Uniform scale of each section, BOTTOM first. Count = number of stages. The taper is the
	 *  STAIR (round 6): each step leaves a 30 cm ring ledge (radii 140/110/80/50), inside the
	 *  balance-assist band — ground → base ledge is a held-jump mantle (280), then +220, +160. */
	UPROPERTY(EditAnywhere, Category = "Finale|Body")
	TArray<float> SectionScales = { 2.8f, 2.2f, 1.6f, 1.0f };

	/** Unscaled height of SectionAsset and its local Z minimum (0 = base at origin,
	 *  -50 = centred). Read from the asset's bounds; exposed so a different asset works. */
	UPROPERTY(EditAnywhere, Category = "Finale|Body")
	float SectionAssetHeight = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Body")
	float SectionAssetZMin = -50.0f;

	/** Material for the sections — needs a vector parameter named TintParameterName.
	 *  Defaults to the engine's BasicShapeMaterial ("Color"). */
	UPROPERTY(EditAnywhere, Category = "Finale|Body")
	TObjectPtr<UMaterialInterface> SectionMaterial;

	UPROPERTY(EditAnywhere, Category = "Finale|Body")
	FName TintParameterName = TEXT("Color");

	// ── Rules ───────────────────────────────────────────────────────

	/** Fraction of ACatGameMode::ChaosThreshold the meter must reach before the shrine
	 *  can be damaged. 0 = exposed from the start (testing). */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UnlockChaosPercent = 0.6f;

	/** The co-op window: cats whose last hit is inside it count toward the multiplier. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.2"))
	float StageWindow = 2.5f;

	// ── Tier health (Sean, round 6): any hit counts, the glowing top tier is the real dealer,
	// and cats hitting together multiply. Solo on the top: 4 swats a tier. Three cats on the
	// top inside one window: one round of swats. ──

	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "1.0"))
	float StageHP = 100.0f;

	/** Damage of a hit on the current top tier / on any lower tier, before the co-op bonus. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0"))
	float TopHitDamage = 25.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0"))
	float LowHitDamage = 8.0f;

	/** Each extra distinct cat inside the window adds this to the multiplier (1 cat x1, 2 x1.5, 3 x2). */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0"))
	float CoopBonusPerCat = 0.5f;

	// ── PRESENCE (round 7 of the 2-player rounds, 2026-09-21): the client brought the shrine
	// down alone while the host was across the map, and Sean's read since the first
	// conversation is "everybody is there when it falls". Damage scales with how many of the
	// MATCH's cats are near the shrine: (near / all)^2 with a floor, so one of three chips at a
	// tenth, two of three do about half, everyone does full. The count is the player list, so
	// solo is one of one = full damage, and a disconnect drops the requirement on its own.
	// Known gap (deliberately not built): a cat that is alive but AFK/lost still counts as
	// required — a grace timer is the fix if a playtest shows it matters. ──

	UPROPERTY(EditAnywhere, Category = "Finale|Rules")
	bool bRequirePresence = true;

	/** Horizontal distance from the shrine's centre within which a cat counts as "here". */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "100.0"))
	float ConvergenceRadius = 1200.0f;

	/** Damage scale when nobody but the hitter is near — the chip, never zero. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PresenceDamageFloor = 0.1f;

	/** A prop striking a section counts as a hit when its mass x speed (kg x cm/s) is at
	 *  least this. 50k = a Small placeholder (~245 kg) at ~200 cm/s. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0"))
	float ImpactMassSpeedThreshold = 50000.0f;

	/** Same cat, same shrine: hits closer together than this are one hit. A charge sits in
	 *  the bulldozer overlap for several ticks and a swat sweep can graze twice. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0"))
	float PerCatHitCooldown = 0.4f;

	/** LEGACY (round 5): reject every hit that is not on the top tier. Off since round 6 —
	 *  15 rejected base hits before the first counted one read as "not clear", so lower hits
	 *  now do LowHitDamage instead. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules")
	bool bRequireTopSectionHit = false;

	/** Chaos value recorded for the final break — large so the Aftermath hotspot lands here. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules", meta = (ClampMin = "0.0"))
	float FinaleChaosValue = 500.0f;

	/** Name the readout and the HUD use. */
	UPROPERTY(EditAnywhere, Category = "Finale|Rules")
	FString DisplayName = TEXT("THE SHRINE");

	// ── Feel ────────────────────────────────────────────────────────

	/** Global time dilation for the hit-stop on a completed stage, and how long it holds
	 *  in REAL seconds. Server-side; WorldSettings replicates it. Skipped on the final stage
	 *  (the match end owns dilation from there). */
	UPROPERTY(EditAnywhere, Category = "Finale|Feel", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float HitStopDilation = 0.15f;

	UPROPERTY(EditAnywhere, Category = "Finale|Feel", meta = (ClampMin = "0.0"))
	float HitStopDuration = 0.12f;

	/** Camera shakes fade to nothing at this distance from the shrine. */
	UPROPERTY(EditAnywhere, Category = "Finale|Feel", meta = (ClampMin = "100.0"))
	float ShakeRadius = 3500.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Feel")
	FLinearColor LockedTint = FLinearColor(0.10f, 0.10f, 0.14f);

	/** The GLOWING top tier — where the real damage happens. Pulses. */
	UPROPERTY(EditAnywhere, Category = "Finale|Feel")
	FLinearColor ArmedTint = FLinearColor(1.0f, 0.75f, 0.2f);

	/** Every exposed tier below the top: dim, so the glow reads as "hit this one". */
	UPROPERTY(EditAnywhere, Category = "Finale|Feel")
	FLinearColor LowerTierTint = FLinearColor(0.32f, 0.14f, 0.03f);

	UPROPERTY(EditAnywhere, Category = "Finale|Feel")
	FLinearColor DestroyedTint = FLinearColor(0.2f, 0.9f, 0.3f);

	/** Readout billboards sit at this height above the base (cat-eye-ish, Sean 2026-09-20:
	 *  "lower to the ground so players can see it"), on the VIEWER's side of the base section,
	 *  ReadoutStandoff beyond its face. Each machine places them for its own camera. */
	UPROPERTY(EditAnywhere, Category = "Finale|Feel", meta = (ClampMin = "0.0"))
	float ReadoutHeight = 260.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Feel", meta = (ClampMin = "0.0"))
	float ReadoutStandoff = 60.0f;

	// ── The unlock beat (Sean 2026-09-20: "could be a bit more intense") ──
	// Four layers, all from the Unlocked multicast: a slow-mo pulse (server dilation, so it
	// is the same beat on every machine), a map-wide rumble with no falloff, an ERUPTION that
	// shoves every prop and piece of debris near the shrine outward, and the readout's
	// EXPOSED headline landing oversized and settling.

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float UnlockSlowmoDilation = 0.25f;

	/** Real seconds the unlock slow-mo holds. 0 disables it. */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockSlowmoDuration = 0.8f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockShakeScale = 1.6f;

	/** Props and debris within this radius get shoved away from the shrine on the unlock. */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockEruptionRadius = 900.0f;

	/** Outward speed given to an INTACT prop (cm/s, mass-capped like the bump-push so a vase
	 *  hops and a fridge rocks) and to a plain physics body. */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockEruptionSpeed = 650.0f;

	/** Per-chunk proxy impulse for fractured debris (the bulldozer's momentum model). */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockEruptionDebrisImpulse = 4000.0f;

	/** Readout scale punch on the unlock (scale = 1 + punch x 0.5) and how slowly it settles. */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockReadoutPunch = 3.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.1"))
	float UnlockReadoutSettleRate = 2.5f;

	/** Seconds the white flash holds at full before fading to gold. */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockFlashHold = 0.5f;

	// ── The unlock REVEAL cut (Sean 2026-09-20: "didn't make the full connection") ──
	// Players are usually inside the house when the meter crosses the line and the shrine is
	// outside, so the beat had nothing to point at. Every local player's camera blends to a
	// framed shot of the shrine, holds through the flash and the eruption, and blends back.
	// Control is never taken; it is only the camera. Per machine, from the Unlocked multicast.

	/** OFF since round 4 (Sean: "gonna feel really weird if there's a player across the map").
	 *  Kept as an option; the BEACON below is the replacement cue. */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock")
	bool bUnlockRevealCut = false;

	// ── The BEACON — a column of light from the shrine, visible over every roof. Shoots up on
	// the unlock and stays lit (dimmer) while the shrine is exposed. Diegetic, so it works
	// wherever a player is standing, which the camera pull did not. ──

	/** Additive unlit material with vector "Color" + scalar "Intensity" (M_Beacon). */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock")
	TObjectPtr<UMaterialInterface> BeaconMaterial;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float BeaconHeight = 3000.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "1.0"))
	float BeaconRadius = 45.0f;

	/** Seconds the column takes to reach full height on the unlock. */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.05"))
	float BeaconRiseTime = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float BeaconPeakIntensity = 14.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float BeaconIdleIntensity = 3.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock")
	FLinearColor BeaconColor = FLinearColor(1.0f, 0.7f, 0.15f);

	/** Camera position relative to the shrine's base for the reveal shot. World-space
	 *  offset: -X is the hero yard's open side (the cinematic-offset convention). */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock")
	FVector UnlockRevealOffset = FVector(-1100.0f, 0.0f, 520.0f);

	/** Blend to the shot, hold on it, blend back — game seconds (the unlock slow-mo
	 *  stretches the first two in real time, which reads as part of the beat). */
	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockRevealBlendIn = 0.2f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockRevealHold = 1.5f;

	UPROPERTY(EditAnywhere, Category = "Finale|Unlock", meta = (ClampMin = "0.0"))
	float UnlockRevealBlendOut = 0.5f;

	// ── The section burst (round-3 read: "still didn't fully destruct") ──
	// ForceShatterGC's scatter is a fixed momentum tuned for ~100 kg prop chunks; the base
	// section's chunks are ~1.6 t, so it moved them centimetres per second and they slumped.
	// The shrine bursts its own sections to a target SPEED per chunk, scaled by each
	// section's estimated chunk mass (asset mass x scale^3 / leaves).

	/** 300, not the 550 first tried: the solver routes several impulses onto the same piece, so
	 *  the measured mean lands ~2x this and the max ~10x (2-player probe: 550 gave means of
	 *  ~1000 and maxes of ~6500 cm/s — pieces cleared the backdrop and fell out of the world). */
	UPROPERTY(EditAnywhere, Category = "Finale|Feel", meta = (ClampMin = "0.0"))
	float ShrineBurstSpeed = 300.0f;

	UPROPERTY(EditAnywhere, Category = "Finale|Feel", meta = (ClampMin = "0.0"))
	float ShrineBurstUpBias = 0.6f;

	// ── State ───────────────────────────────────────────────────────

	UPROPERTY(ReplicatedUsing = OnRep_State, BlueprintReadOnly, Category = "Finale")
	FCatFinaleState State;

	// ── Server API ──────────────────────────────────────────────────

	/** THE hit intake. Attacker may be null (an unattributed impact) — such hits are logged
	 *  and dropped, because a stage is "every cat hit it", and nobody did. SectionIndex is
	 *  the section struck when the caller knows it (swat / impact hit component); INDEX_NONE
	 *  resolves it from HitLocation's height (the charge path passes the cat's position). */
	void ReceiveHit(APlayerState* Attacker, ECatFinaleHitKind Kind, FVector HitLocation, int32 SectionIndex = INDEX_NONE);

	/** Called by the GameMode when the meter crosses UnlockChaosPercent. */
	void SetUnlocked(bool bNewUnlocked);

	bool IsUnlocked()  const { return State.bUnlocked; }
	bool IsDestroyed() const { return State.bDestroyed; }

	/** Where the cinematics should look: the centre of the lowest intact section, or the
	 *  base once everything is down. */
	FVector GetCinematicFocus() const;

	/** One HUD line. ChaosPercent is the replicated meter so clients render the same text. */
	FText GetHudText(float ChaosPercent) const;

	UFUNCTION(BlueprintPure, Category = "Finale")
	int32 GetNumStages() const { return NumActiveSections(); }

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_State();

	UFUNCTION()
	void OnSwatDamage(AActor* DamagedActor, float Damage, AController* InstigatedBy, FVector HitLocation,
	                  UPrimitiveComponent* FHitComponent, FName BoneName, FVector ShotFromDirection,
	                  const UDamageType* DamageType, AActor* DamageCauser);

	UFUNCTION()
	void OnSectionHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	                  FVector NormalImpulse, const FHitResult& Hit);

	/** Feedback beat on every machine — shakes, flashes, readout punches. Reliable: rare,
	 *  state-carrying one-shots (the impact-reaction doctrine). */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_FinaleEvent(ECatFinaleEvent Event, FVector Location, const FString& CatName);

	/** Brings one section down on every machine through ForceShatterGC (local solvers, like
	 *  every prop) and registers the shrine as debris with the local PlayerController so the
	 *  Aftermath director can frame it. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_ShatterSection(int32 SectionIndex, FVector HitLocation);

private:
	UPROPERTY(VisibleAnywhere, Category = "Finale")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Finale")
	TArray<TObjectPtr<UGeometryCollectionComponent>> Sections;

	UPROPERTY(VisibleAnywhere, Category = "Finale")
	TObjectPtr<UTextRenderComponent> Headline;

	UPROPERTY(VisibleAnywhere, Category = "Finale")
	TObjectPtr<UTextRenderComponent> Detail;

	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SectionMIDs;

	UPROPERTY(VisibleAnywhere, Category = "Finale")
	TObjectPtr<class UStaticMeshComponent> Beacon;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> BeaconMID;

	int32 NumActiveSections() const { return FMath::Clamp(SectionScales.Num(), 1, MaxSections); }
	int32 TopIntactSection() const { return NumActiveSections() - 1 - State.StagesDone; }
	int32 SectionIndexOf(const UPrimitiveComponent* Comp) const;
	int32 SectionIndexAtHeight(float WorldZ) const;
	void  UpdateBeacon(float DeltaTime);
	float SectionHeight(int32 Index) const;
	float StackTopZ(int32 IntactSections) const;
	void  LayoutSections();

	// server-side stage machine
	/** Counts the match's cats and how many are within ConvergenceRadius; writes
	 *  State.RequiredCats / CatsNear / PresenceScale. Returns true if anything changed. */
	bool RecountCats();
	float PresenceRecountTimer = 0.0f;
	void OpenWindow();
	void CloseWindow();
	void OnWindowExpired();
	void CompleteStage(FVector HitLocation);
	void PushState();
	void DoHitStop(float Dilation, float RealSeconds);
	void RestoreDilation();

	// per-machine cosmetics
	void ApplyLocalFeedback(ECatFinaleEvent Event, FVector Location, const FString& CatName);
	void ShakeLocalPlayers(TSubclassOf<UCameraShakeBase> ShakeClass, float Scale, FVector Epicenter, bool bIgnoreFalloff = false);
	void EruptNearbyProps();
	/** Bursts a shattered section once its pieces report as broken off on the game thread;
	 *  re-arms itself for the next tick (Attempt counts) until they do. */
	void BurstSection(int32 SectionIndex, FVector Origin, int32 Attempt = 0);
	void StartUnlockReveal();
	void EndUnlockReveal();
	void CleanupRevealCameras();
	void RefreshReadout();
	void RegisterAsDebrisLocally();
	float WindowRemaining() const;
	float BaseRadius() const;

	TArray<TWeakObjectPtr<class ACameraActor>> RevealCameras;
	FTimerHandle RevealTimer;
	FTimerHandle RevealCleanupTimer;

	TArray<TWeakObjectPtr<APlayerState>>       HitSetThisWindow;
	TMap<TWeakObjectPtr<APlayerState>, double> LastHitTimeByCat;
	FTimerHandle WindowTimer;
	FTimerHandle HitStopTimer;

	bool  bRegisteredDebrisLocally = false;
	/** A timed headline/detail override on the readout ("AGAIN - ALL TOGETHER", "HIGHER"). */
	FString OverrideHead;
	FString OverrideDetail;
	FColor  OverrideColor = FColor::White;
	float   OverrideUntil = 0.0f;
	float BeaconRise = 0.0f;        // 0..1 of BeaconHeight
	float BeaconPulse = 0.0f;       // 1 on the unlock, decays to 0
	float FlashAmount = 0.0f;
	float FlashHoldUntil = 0.0f;
	float ReadoutPunch = 0.0f;
	float PunchDecay = 8.0f;
	TArray<FLinearColor> SectionTints;   // per-section smoothed tint
	FString LastHeadline;
	FString LastDetail;
};
