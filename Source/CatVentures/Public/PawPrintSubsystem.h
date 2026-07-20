#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PawPrintSubsystem.generated.h"

/**
 * PawPrint — lightweight in-memory runtime telemetry (2026-07-19).
 *
 * Eliminates the need to live-monitor a PIE session: everything interesting is
 * captured in memory while the session runs and is (a) queryable through the
 * reflected API below (VibeUE python reaches it live or post-hoc) and (b) dumped
 * to Saved/PawPrint/*.csv when the world tears down, so every session leaves an
 * artifact even if nobody watched it.
 *
 * Two capture surfaces:
 *  - LOG TAP: a global FOutputDevice mirrors every engine log line (with its
 *    category) into a shared in-memory ring — the full-firehose category set the
 *    PawPrint window filters with checkboxes. Shared process-wide (log lines
 *    carry no world context); ref-counted by subsystem instances, so 2-player
 *    PIE registers it exactly once.
 *  - SAMPLED CHANNELS: named float ring buffers fed by explicit SampleChannel()
 *    calls (ACatBase pushes its locally-controlled channel set per tick).
 *    Per-world, so each PIE instance dumps its own channel CSV tagged by role.
 *
 * Timebase is real seconds since capture start (log lines can arrive off the
 * game thread, where world time is unreadable). Zero allocation steady-state
 * beyond ring growth to cap; no file I/O until dump.
 */

/** One sampled point on a named channel. */
struct FPawPrintSamplePoint
{
	float Time  = 0.0f;
	float Value = 0.0f;
};

UCLASS()
class CATVENTURES_API UPawPrintSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// ── Subsystem lifecycle ────────────────────────────────────────────
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Tickable ──────────────────────────────────────────────────────
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// ── Capture API (game code) ───────────────────────────────────────

	/** Push one sample onto a named channel. Cheap (map find + array append); call per tick. */
	void SampleChannel(FName Channel, float Value);

	// ── Query API (window + VibeUE python; parallel arrays for reflection) ──

	/** All channel names that have received at least one sample. */
	UFUNCTION(BlueprintCallable, Category = "PawPrint")
	TArray<FName> GetChannelNames() const;

	/** Last N samples of a channel (0 = all). Parallel time/value arrays. */
	UFUNCTION(BlueprintCallable, Category = "PawPrint")
	void GetChannelSamples(FName Channel, int32 LastN, TArray<float>& OutTimes, TArray<float>& OutValues) const;

	/** Most recent value of a channel (0 if never sampled) — the window's watch pane. */
	UFUNCTION(BlueprintCallable, Category = "PawPrint")
	float GetChannelCurrentValue(FName Channel) const;

	/** Every log category seen by the tap so far (the window's filter checkbox list). */
	UFUNCTION(BlueprintCallable, Category = "PawPrint")
	TArray<FName> GetCapturedCategories() const;

	/** Captured log lines, newest last. LastN caps the result (0 = all); Categories empty = no
	 *  filter. Lines are pre-formatted "12.345 [Category][Verbosity] Message". SinceIndex skips
	 *  lines already fetched (pass the last OutNextIndex) so the window polls incrementally. */
	UFUNCTION(BlueprintCallable, Category = "PawPrint")
	void GetCapturedLines(const TArray<FName>& Categories, int32 LastN, int32 SinceIndex,
	                      TArray<FString>& OutLines, int32& OutNextIndex) const;

	/** Write the channel + log CSVs now (also happens automatically on world teardown). */
	UFUNCTION(BlueprintCallable, Category = "PawPrint")
	FString DumpToCSV();

private:
	struct FChannel
	{
		TArray<FPawPrintSamplePoint> Points;
	};

	/** Named sample channels (per-world). */
	TMap<FName, FChannel> Channels;

	/** Insertion order for stable CSV column order. */
	TArray<FName> ChannelOrder;

	/** Real-time origin for this world's samples (aligned with the log tap's origin). */
	double SampleTimeOrigin = 0.0;

	/** True once this instance has written its teardown dump (guards double-dump). */
	bool bDumped = false;

	static constexpr int32 MaxPointsPerChannel = 40000;   // ~11 min at 60 Hz
};
