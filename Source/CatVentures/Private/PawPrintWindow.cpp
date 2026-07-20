#include "PawPrintWindow.h"

#if WITH_EDITOR

#include "PawPrintSubsystem.h"
#include "PawPrintSettings.h"
#include "CatVenturesLog.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Styling/AppStyle.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "PawPrint"

// ══════════════════════════════════════════════════════════════════════════
// ── SPawPrintWindow ───────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Left: category checkbox list (auto-populated from the tap as categories appear;
// checked state persisted per user via UPawPrintSettings — first run = curated
// set). Right: watch pane of live channel values over a virtualized log view
// polling GetCapturedLines incrementally at 10 Hz. All data is pulled from
// UPawPrintSubsystem::GetActive(); the window survives across PIE sessions and
// re-binds to whatever session is currently live.

class SPawPrintWindow final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPawPrintWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		LoadCheckedState();

		ChildSlot
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)

			// ── Left: categories ──────────────────────────────────────
			+ SSplitter::Slot().Value(0.25f)
			[
				SNew(SBorder).Padding(4.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Categories", "Categories"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton).Text(LOCTEXT("All", "All"))
							.OnClicked(this, &SPawPrintWindow::OnCheckAll)
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton).Text(LOCTEXT("None", "None"))
							.OnClicked(this, &SPawPrintWindow::OnCheckNone)
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton).Text(LOCTEXT("Curated", "Curated"))
							.OnClicked(this, &SPawPrintWindow::OnCheckCurated)
						]
					]
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SAssignNew(CategoryList, SListView<TSharedPtr<FName>>)
						.ListItemsSource(&CategoryItems)
						.SelectionMode(ESelectionMode::None)
						.OnGenerateRow(this, &SPawPrintWindow::MakeCategoryRow)
					]
				]
			]

			// ── Right: watch pane over log view ───────────────────────
			+ SSplitter::Slot().Value(0.75f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Vertical)

				+ SSplitter::Slot().Value(0.22f)
				[
					SNew(SBorder).Padding(4.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Watch", "Watch"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						]
						+ SVerticalBox::Slot().FillHeight(1.0f)
						[
							SAssignNew(WatchList, SListView<TSharedPtr<FName>>)
							.ListItemsSource(&WatchItems)
							.SelectionMode(ESelectionMode::None)
							.OnGenerateRow(this, &SPawPrintWindow::MakeWatchRow)
						]
					]
				]

				+ SSplitter::Slot().Value(0.78f)
				[
					SNew(SBorder).Padding(4.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[
								SAssignNew(StatusBlock, STextBlock)
								.Text(LOCTEXT("NoSession", "No active session — start PIE"))
							]
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SNew(SCheckBox)
								.IsChecked(this, &SPawPrintWindow::GetAutoScrollState)
								.OnCheckStateChanged(this, &SPawPrintWindow::OnAutoScrollChanged)
								[
									SNew(STextBlock).Text(LOCTEXT("AutoScroll", "Auto-scroll"))
								]
							]
						]
						+ SVerticalBox::Slot().FillHeight(1.0f)
						[
							SAssignNew(LogList, SListView<TSharedPtr<FString>>)
							.ListItemsSource(&LineItems)
							.SelectionMode(ESelectionMode::Single)
							.OnGenerateRow(this, &SPawPrintWindow::MakeLogRow)
						]
					]
				]
			]
		];

		RegisterActiveTimer(0.1f,
			FWidgetActiveTimerDelegate::CreateSP(this, &SPawPrintWindow::Poll));
	}

private:
	// ── Data ──────────────────────────────────────────────────────────
	TArray<TSharedPtr<FName>>   CategoryItems;
	TSet<FName>                 KnownCategories;
	TSet<FName>                 CheckedCategories;
	TArray<TSharedPtr<FName>>   WatchItems;
	TArray<TSharedPtr<FString>> LineItems;
	TWeakObjectPtr<UPawPrintSubsystem> BoundSubsystem;
	int32 PollCursor   = 0;
	bool  bFilterDirty = true;
	bool  bAutoScroll  = true;

	TSharedPtr<SListView<TSharedPtr<FName>>>   CategoryList;
	TSharedPtr<SListView<TSharedPtr<FName>>>   WatchList;
	TSharedPtr<SListView<TSharedPtr<FString>>> LogList;
	TSharedPtr<STextBlock> StatusBlock;

	static constexpr int32 MaxVisibleLines = 4000;
	static constexpr int32 InitialFetch    = 2000;

	// ── Poll (10 Hz active timer) ─────────────────────────────────────
	EActiveTimerReturnType Poll(double InCurrentTime, float InDeltaTime)
	{
		UPawPrintSubsystem* PP = UPawPrintSubsystem::GetActive();
		if (!PP)
		{
			if (BoundSubsystem.IsValid() || LineItems.Num() > 0)
			{
				// Session ended: keep showing what we have, but note it's frozen.
				StatusBlock->SetText(LOCTEXT("SessionEnded", "Session ended — showing last capture"));
			}
			BoundSubsystem = nullptr;
			return EActiveTimerReturnType::Continue;
		}

		// New session → new capture with fresh indices; reset the poll state.
		if (PP != BoundSubsystem.Get())
		{
			BoundSubsystem = PP;
			PollCursor     = 0;
			bFilterDirty   = true;
			WatchItems.Reset();
		}
		StatusBlock->SetText(FText::Format(
			LOCTEXT("SessionLive", "Live — {0} categories, {1} channels"),
			FText::AsNumber(KnownCategories.Num()), FText::AsNumber(WatchItems.Num())));

		// Merge newly seen categories into the checkbox list (new ones arrive unchecked —
		// the firehose stays opt-in).
		bool bCatsChanged = false;
		for (const FName& Cat : PP->GetCapturedCategories())
		{
			if (!KnownCategories.Contains(Cat))
			{
				KnownCategories.Add(Cat);
				CategoryItems.Add(MakeShared<FName>(Cat));
				bCatsChanged = true;
			}
		}
		if (bCatsChanged)
		{
			CategoryItems.Sort([](const TSharedPtr<FName>& A, const TSharedPtr<FName>& B)
				{ return A->LexicalLess(*B); });
			CategoryList->RequestListRefresh();
		}

		// Watch pane tracks the live channel list.
		const TArray<FName> Channels = PP->GetChannelNames();
		if (Channels.Num() != WatchItems.Num())
		{
			WatchItems.Reset();
			for (const FName& Chan : Channels)
			{
				WatchItems.Add(MakeShared<FName>(Chan));
			}
			WatchList->RequestListRefresh();
		}

		// Log view: filter change = full refetch; otherwise incremental from the cursor.
		if (bFilterDirty)
		{
			PollCursor = 0;
			LineItems.Reset();
			bFilterDirty = false;
			LogList->RequestListRefresh();
		}
		if (CheckedCategories.Num() > 0)
		{
			TArray<FString> NewLines;
			int32 NextIndex = PollCursor;
			PP->GetCapturedLines(CheckedCategories.Array(),
				(PollCursor == 0) ? InitialFetch : 0, PollCursor, NewLines, NextIndex);
			PollCursor = NextIndex;
			if (NewLines.Num() > 0)
			{
				for (FString& Line : NewLines)
				{
					LineItems.Add(MakeShared<FString>(MoveTemp(Line)));
				}
				if (LineItems.Num() > MaxVisibleLines)
				{
					LineItems.RemoveAt(0, LineItems.Num() - MaxVisibleLines, EAllowShrinking::No);
				}
				LogList->RequestListRefresh();
				if (bAutoScroll)
				{
					LogList->ScrollToBottom();
				}
			}
		}
		return EActiveTimerReturnType::Continue;
	}

	// ── Rows ──────────────────────────────────────────────────────────
	TSharedRef<ITableRow> MakeCategoryRow(TSharedPtr<FName> Item, const TSharedRef<STableViewBase>& Owner)
	{
		return SNew(STableRow<TSharedPtr<FName>>, Owner)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, Item]()
				{ return CheckedCategories.Contains(*Item) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Item](ECheckBoxState State)
				{
					if (State == ECheckBoxState::Checked) { CheckedCategories.Add(*Item); }
					else                                  { CheckedCategories.Remove(*Item); }
					bFilterDirty = true;
					SaveCheckedState();
				})
			[
				SNew(STextBlock).Text(FText::FromName(*Item))
			]
		];
	}

	TSharedRef<ITableRow> MakeWatchRow(TSharedPtr<FName> Item, const TSharedRef<STableViewBase>& Owner)
	{
		return SNew(STableRow<TSharedPtr<FName>>, Owner)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.5f)
			[
				SNew(STextBlock)
				.Text(FText::FromName(*Item))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]
			+ SHorizontalBox::Slot().FillWidth(0.5f)
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.Text_Lambda([Item]()
					{
						UPawPrintSubsystem* PP = UPawPrintSubsystem::GetActive();
						return PP
							? FText::FromString(FString::Printf(TEXT("%.2f"), PP->GetChannelCurrentValue(*Item)))
							: LOCTEXT("Dash", "—");
					})
			]
		];
	}

	TSharedRef<ITableRow> MakeLogRow(TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& Owner)
	{
		return SNew(STableRow<TSharedPtr<FString>>, Owner)
		[
			SNew(STextBlock)
			.Text(FText::FromString(*Item))
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
		];
	}

	// ── Checkbox state persistence ────────────────────────────────────
	void LoadCheckedState()
	{
		const UPawPrintSettings* Settings = GetDefault<UPawPrintSettings>();
		const TArray<FName> Initial = Settings->bHasSavedCategories
			? Settings->CheckedCategories
			: UPawPrintSubsystem::GetCuratedCategories();
		for (const FName& Cat : Initial)
		{
			CheckedCategories.Add(Cat);
			// Seed the visible list so the boxes exist before a session starts.
			if (!KnownCategories.Contains(Cat))
			{
				KnownCategories.Add(Cat);
				CategoryItems.Add(MakeShared<FName>(Cat));
			}
		}
		CategoryItems.Sort([](const TSharedPtr<FName>& A, const TSharedPtr<FName>& B)
			{ return A->LexicalLess(*B); });
	}

	void SaveCheckedState()
	{
		UPawPrintSettings* Settings = GetMutableDefault<UPawPrintSettings>();
		Settings->CheckedCategories   = CheckedCategories.Array();
		Settings->bHasSavedCategories = true;
		Settings->SaveConfig();
	}

	// ── Buttons ───────────────────────────────────────────────────────
	FReply OnCheckAll()
	{
		CheckedCategories = KnownCategories;
		bFilterDirty = true;
		SaveCheckedState();
		return FReply::Handled();
	}

	FReply OnCheckNone()
	{
		CheckedCategories.Reset();
		bFilterDirty = true;
		SaveCheckedState();
		return FReply::Handled();
	}

	FReply OnCheckCurated()
	{
		CheckedCategories = TSet<FName>(UPawPrintSubsystem::GetCuratedCategories());
		bFilterDirty = true;
		SaveCheckedState();
		return FReply::Handled();
	}

	ECheckBoxState GetAutoScrollState() const
	{
		return bAutoScroll ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	}

	void OnAutoScrollChanged(ECheckBoxState State)
	{
		bAutoScroll = (State == ECheckBoxState::Checked);
	}
};

// ══════════════════════════════════════════════════════════════════════════
// ── Tab registration + auto-open ──────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

namespace PawPrintWindow
{
	static const FName TabName(TEXT("PawPrintWindow"));
	static FDelegateHandle WorldInitHandle;

	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SPawPrintWindow)
			];
	}

	static void OnWorldInit(UWorld* World, const UWorld::InitializationValues IVS)
	{
		// Auto-open (per-user setting): first PIE world up brings the tab up with it.
		if (GIsEditor && World && World->WorldType == EWorldType::PIE
			&& GetDefault<UPawPrintSettings>()->bAutoOpenOnPIE)
		{
			FGlobalTabmanager::Get()->TryInvokeTab(TabName);
		}
	}

	void Register()
	{
		// Idempotence: an ungrouped nomad spawner showed up TWICE in the Window menu
		// (2026-07-19 first round) — the group assignment below is the real fix, the
		// guard makes double-registration structurally impossible on top.
		static bool bRegistered = false;
		if (bRegistered)
		{
			return;
		}
		bRegistered = true;

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName,
			FOnSpawnTab::CreateStatic(&SpawnTab))
			.SetDisplayName(LOCTEXT("TabTitle", "PawPrint"))
			.SetTooltipText(LOCTEXT("TabTooltip",
				"PawPrint runtime telemetry — category-filtered live log + channel watch"))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());

		// Explicit Project Settings registration — UDeveloperSettings auto-discovery
		// never surfaced this class (2026-07-19: absent from the settings UI even with
		// CategoryName pinned), so register the section ourselves.
		if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->RegisterSettings("Project", "Game", "PawPrint",
				LOCTEXT("SettingsName", "PawPrint"),
				LOCTEXT("SettingsDesc", "PawPrint runtime telemetry — auto-open and window preferences"),
				GetMutableDefault<UPawPrintSettings>());
		}

		WorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddStatic(&OnWorldInit);
	}

	void Unregister()
	{
		FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitHandle);
		if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->UnregisterSettings("Project", "Game", "PawPrint");
		}
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		}
	}
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
