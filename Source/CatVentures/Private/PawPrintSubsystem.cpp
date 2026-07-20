#include "PawPrintSubsystem.h"
#include "CatVenturesLog.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "Engine/World.h"

// ══════════════════════════════════════════════════════════════════════════
// ── Log tap — process-wide, ref-counted ───────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// One FOutputDevice shared by every PawPrint subsystem instance in the process:
// GLog lines carry no world context, so per-world taps would capture duplicate
// content in multi-world PIE. First subsystem in registers it, last one out
// unregisters and frees. Serialize() can run on any thread — the line store is
// lock-guarded, and the game-thread readers copy out under the same lock.

namespace PawPrint
{
	struct FLogLine
	{
		double  Time = 0.0;               // seconds since capture start (real time)
		FName   Category;
		uint8   Verbosity = 0;
		FString Message;
	};

	class FLogCapture final : public FOutputDevice
	{
	public:
		FLogCapture()
		{
			StartTime = FPlatformTime::Seconds();
			GLog->AddOutputDevice(this);
		}

		virtual ~FLogCapture() override
		{
			if (GLog)
			{
				GLog->RemoveOutputDevice(this);
			}
		}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			// VeryVerbose is per-frame firehose noise even by PawPrint standards.
			if (Verbosity > ELogVerbosity::Verbose)
			{
				return;
			}

			FScopeLock Lock(&Mutex);
			Categories.Add(Category);
			FLogLine& Line = Lines.AddDefaulted_GetRef();
			Line.Time      = FPlatformTime::Seconds() - StartTime;
			Line.Category  = Category;
			Line.Verbosity = static_cast<uint8>(Verbosity);
			Line.Message   = V;
			++TotalLinesAdded;

			// Trim in chunks so the shift cost amortizes instead of paying per line.
			if (Lines.Num() > MaxLines)
			{
				const int32 Removed = MaxLines / 4;
				Lines.RemoveAt(0, Removed, EAllowShrinking::No);
				BaseIndex += Removed;
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		void GetCategories(TArray<FName>& Out) const
		{
			FScopeLock Lock(&Mutex);
			Out = Categories.Array();
		}

		/** Copies lines with GLOBAL index >= SinceIndex (0-based since capture start),
		 *  filtered by category set (empty = all), capped to the newest LastN (0 = all).
		 *  Returns the next global index to poll from. */
		int32 GetLines(const TSet<FName>& CategoryFilter, int32 LastN, int32 SinceIndex,
		               TArray<FLogLine>& Out) const
		{
			FScopeLock Lock(&Mutex);
			const int32 Start = FMath::Max(SinceIndex - BaseIndex, 0);
			for (int32 i = Start; i < Lines.Num(); ++i)
			{
				if (CategoryFilter.Num() == 0 || CategoryFilter.Contains(Lines[i].Category))
				{
					Out.Add(Lines[i]);
				}
			}
			if (LastN > 0 && Out.Num() > LastN)
			{
				Out.RemoveAt(0, Out.Num() - LastN, EAllowShrinking::No);
			}
			return BaseIndex + Lines.Num();
		}

		double GetStartTime() const { return StartTime; }

	private:
		mutable FCriticalSection Mutex;
		TArray<FLogLine> Lines;
		TSet<FName>      Categories;
		double           StartTime = 0.0;
		int32            BaseIndex = 0;         // global index of Lines[0] (survives trims)
		int64            TotalLinesAdded = 0;
		static constexpr int32 MaxLines = 50000;
	};

	static FLogCapture* GCapture     = nullptr;
	static int32        GCaptureRefs = 0;

	static FString VerbosityToString(uint8 V)
	{
		return FString(ToString(static_cast<ELogVerbosity::Type>(V)));
	}

	static FString CsvEscape(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\""), TEXT("\"\""));
		return FString::Printf(TEXT("\"%s\""), *Out);
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Subsystem ─────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

bool UPawPrintSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Game/PIE worlds only — editor-preview worlds would register phantom taps.
	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game;
	}
	return false;
}

void UPawPrintSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (PawPrint::GCaptureRefs++ == 0)
	{
		PawPrint::GCapture = new PawPrint::FLogCapture();
	}
	// Align this world's sample clock with the shared tap's origin so channel
	// times and log-line times land on one axis.
	SampleTimeOrigin = PawPrint::GCapture->GetStartTime();

	UE_LOG(LogCatVentures, Log, TEXT("PawPrint capture started (world %s)"), *GetWorld()->GetName());
}

void UPawPrintSubsystem::Deinitialize()
{
	// No channels = a world that never hosted gameplay (PIE clients traverse an empty
	// transition world before ClientTravel) — its dump would be a header-only channels
	// file plus a duplicate log. The gameplay worlds' dumps carry everything.
	if (!bDumped && ChannelOrder.Num() > 0)
	{
		DumpToCSV();
	}

	if (--PawPrint::GCaptureRefs == 0)
	{
		delete PawPrint::GCapture;
		PawPrint::GCapture = nullptr;
	}

	Super::Deinitialize();
}

void UPawPrintSubsystem::Tick(float DeltaTime)
{
	// Nothing per-tick yet: capture is push-based and the store is pull-based.
	// The tick surface exists for the window's future needs (e.g. derived stats).
}

TStatId UPawPrintSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPawPrintSubsystem, STATGROUP_Tickables);
}

void UPawPrintSubsystem::SampleChannel(FName Channel, float Value)
{
	FChannel* Chan = Channels.Find(Channel);
	if (!Chan)
	{
		Chan = &Channels.Add(Channel);
		Chan->Points.Reserve(4096);
		ChannelOrder.Add(Channel);
	}

	FPawPrintSamplePoint& P = Chan->Points.AddDefaulted_GetRef();
	P.Time  = static_cast<float>(FPlatformTime::Seconds() - SampleTimeOrigin);
	P.Value = Value;

	if (Chan->Points.Num() > MaxPointsPerChannel)
	{
		Chan->Points.RemoveAt(0, MaxPointsPerChannel / 4, EAllowShrinking::No);
	}
}

TArray<FName> UPawPrintSubsystem::GetChannelNames() const
{
	return ChannelOrder;
}

void UPawPrintSubsystem::GetChannelSamples(FName Channel, int32 LastN,
                                           TArray<float>& OutTimes, TArray<float>& OutValues) const
{
	OutTimes.Reset();
	OutValues.Reset();
	if (const FChannel* Chan = Channels.Find(Channel))
	{
		const int32 Num   = Chan->Points.Num();
		const int32 Start = (LastN > 0) ? FMath::Max(Num - LastN, 0) : 0;
		OutTimes.Reserve(Num - Start);
		OutValues.Reserve(Num - Start);
		for (int32 i = Start; i < Num; ++i)
		{
			OutTimes.Add(Chan->Points[i].Time);
			OutValues.Add(Chan->Points[i].Value);
		}
	}
}

float UPawPrintSubsystem::GetChannelCurrentValue(FName Channel) const
{
	if (const FChannel* Chan = Channels.Find(Channel))
	{
		if (Chan->Points.Num() > 0)
		{
			return Chan->Points.Last().Value;
		}
	}
	return 0.0f;
}

TArray<FName> UPawPrintSubsystem::GetCapturedCategories() const
{
	TArray<FName> Out;
	if (PawPrint::GCapture)
	{
		PawPrint::GCapture->GetCategories(Out);
		Out.Sort(FNameLexicalLess());
	}
	return Out;
}

void UPawPrintSubsystem::GetCapturedLines(const TArray<FName>& Categories, int32 LastN, int32 SinceIndex,
                                          TArray<FString>& OutLines, int32& OutNextIndex) const
{
	OutLines.Reset();
	OutNextIndex = SinceIndex;
	if (!PawPrint::GCapture)
	{
		return;
	}

	TArray<PawPrint::FLogLine> Raw;
	OutNextIndex = PawPrint::GCapture->GetLines(TSet<FName>(Categories), LastN, SinceIndex, Raw);
	OutLines.Reserve(Raw.Num());
	for (const PawPrint::FLogLine& Line : Raw)
	{
		OutLines.Add(FString::Printf(TEXT("%8.3f [%s][%s] %s"),
			Line.Time, *Line.Category.ToString(),
			*PawPrint::VerbosityToString(Line.Verbosity), *Line.Message));
	}
}

FString UPawPrintSubsystem::DumpToCSV()
{
	const UWorld* World = GetWorld();
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const TCHAR* Role =
		  (World->GetNetMode() == NM_ListenServer) ? TEXT("host")
		: (World->GetNetMode() == NM_Client)       ? TEXT("client")
		: TEXT("standalone");
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("PawPrint");

	// ── Channels (long format: Time,Channel,Value — analysis-friendly) ──
	FString ChannelCsv = TEXT("Time,Channel,Value\n");
	for (const FName& Name : ChannelOrder)
	{
		const FChannel& Chan = Channels[Name];
		for (const FPawPrintSamplePoint& P : Chan.Points)
		{
			ChannelCsv += FString::Printf(TEXT("%.3f,%s,%.4f\n"), P.Time, *Name.ToString(), P.Value);
		}
	}

	// ── Log lines (this dump carries the whole shared tap — written per world;
	//    in 2-player PIE both dumps contain the same lines, tagged differently.
	//    Duplication is deliberate: each file is self-contained for analysis) ──
	FString LogCsv = TEXT("Time,Category,Verbosity,Message\n");
	if (PawPrint::GCapture)
	{
		TArray<PawPrint::FLogLine> Raw;
		PawPrint::GCapture->GetLines(TSet<FName>(), 0, 0, Raw);
		for (const PawPrint::FLogLine& Line : Raw)
		{
			LogCsv += FString::Printf(TEXT("%.3f,%s,%s,%s\n"),
				Line.Time, *Line.Category.ToString(),
				*PawPrint::VerbosityToString(Line.Verbosity), *PawPrint::CsvEscape(Line.Message));
		}
	}

	const FString ChannelPath = Dir / FString::Printf(TEXT("%s_%s_channels.csv"), *Stamp, Role);
	const FString LogFilePath = Dir / FString::Printf(TEXT("%s_%s_log.csv"), *Stamp, Role);
	FFileHelper::SaveStringToFile(ChannelCsv, *ChannelPath);
	FFileHelper::SaveStringToFile(LogCsv, *LogFilePath);
	bDumped = true;

	UE_LOG(LogCatVentures, Log, TEXT("PawPrint dumped %d channels / %s"),
		ChannelOrder.Num(), *ChannelPath);
	return ChannelPath;
}
