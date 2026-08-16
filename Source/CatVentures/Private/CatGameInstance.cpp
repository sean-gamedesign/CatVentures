// CatGameInstance.cpp

#include "CatGameInstance.h"
#include "CatVenturesLog.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"        // STEAM_SUBSYSTEM
#include "OnlineSessionSettings.h"
#include "Online/OnlineSessionNames.h"   // SEARCH_LOBBIES
#include "Engine/NetDriver.h"
#include "Engine/World.h"                // FWorldDelegates::OnNetDriverCreated
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"  // QuitGame
#include "Kismet/GameplayStatics.h"      // OpenLevel
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// ── Session name shared across all methods ────────────────────────────────
static const FName SESSION_NAME = FName("CatVenturesSession");

// ── Menu map, travelled to by LeaveToMainMenu ─────────────────────────────
static const FName MAIN_MENU_LEVEL = FName("Map_Title");

// ── Diagnostic helper: stringify the deferred-destroy action ──────────────
static FString PendingActionToString(ECatPendingSessionAction Action)
{
	switch (Action)
	{
	case ECatPendingSessionAction::None:        return TEXT("None");
	case ECatPendingSessionAction::Host:        return TEXT("Host");
	case ECatPendingSessionAction::Join:        return TEXT("Join");
	case ECatPendingSessionAction::LeaveToMenu: return TEXT("LeaveToMenu");
	case ECatPendingSessionAction::Quit:        return TEXT("Quit");
	default:                                    return TEXT("Unknown");
	}
}

// ── Diagnostic helper: stringify EOnJoinSessionCompleteResult ─────────────
static FString JoinResultToString(EOnJoinSessionCompleteResult::Type Result)
{
	switch (Result)
	{
	case EOnJoinSessionCompleteResult::Success:                       return TEXT("Success");
	case EOnJoinSessionCompleteResult::SessionIsFull:                 return TEXT("SessionIsFull");
	case EOnJoinSessionCompleteResult::SessionDoesNotExist:           return TEXT("SessionDoesNotExist");
	case EOnJoinSessionCompleteResult::CouldNotRetrieveAddress:       return TEXT("CouldNotRetrieveAddress");
	case EOnJoinSessionCompleteResult::AlreadyInSession:              return TEXT("AlreadyInSession");
	case EOnJoinSessionCompleteResult::UnknownError:                  return TEXT("UnknownError");
	default:                                                          return FString::Printf(TEXT("UnknownEnum(%d)"), static_cast<int32>(Result));
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Init ─────────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::Init()
{
	Super::Init();

	// Bind BEFORE the OSS checks below — those early-return, and the driver identity
	// matters most in exactly the cases where the OSS is missing (-nosteam, Steam
	// client down), since that is when the silent IpNetDriver fallback kicks in.
	NetDriverCreatedDelegateHandle = FWorldDelegates::OnNetDriverCreated.AddUObject(
		this, &UCatGameInstance::HandleNetDriverCreated);

	// Packaged builds have no CDO to tick — `-ForceLAN` is how the LAN A/B path gets
	// enabled on the rig. Never clears the flag, so an editor-set true still wins.
	if (FParse::Param(FCommandLine::Get(), TEXT("ForceLAN")))
	{
		bForceLANMatch = true;
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] -ForceLAN on the command line: bForceLANMatch enabled."));
	}

	IOnlineSubsystem* OSS = IOnlineSubsystem::Get();
	if (!OSS)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("UCatGameInstance::Init — No OnlineSubsystem found. "
			"Session features disabled. Is Steam running?"));
		return;
	}

	SessionInterface = OSS->GetSessionInterface();
	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("UCatGameInstance::Init — SessionInterface is invalid."));
		return;
	}

	// Pre-bind native delegates — registered/unregistered around each async call.
	CreateSessionCompleteDelegate = FOnCreateSessionCompleteDelegate::CreateUObject(
		this, &UCatGameInstance::HandleCreateSessionComplete);
	DestroySessionCompleteDelegate = FOnDestroySessionCompleteDelegate::CreateUObject(
		this, &UCatGameInstance::HandleDestroySessionComplete);
	FindSessionsCompleteDelegate = FOnFindSessionsCompleteDelegate::CreateUObject(
		this, &UCatGameInstance::HandleFindSessionsComplete);
	JoinSessionCompleteDelegate = FOnJoinSessionCompleteDelegate::CreateUObject(
		this, &UCatGameInstance::HandleJoinSessionComplete);

	// Persistent delegate — fired by OSS Steam when the user clicks "Join Game" in the overlay.
	// Registered once; never removed (matches UE5 convention for this callback).
	SessionUserInviteAcceptedDelegate = FOnSessionUserInviteAcceptedDelegate::CreateUObject(
		this, &UCatGameInstance::HandleSessionUserInviteAccepted);
	SessionInterface->AddOnSessionUserInviteAcceptedDelegate_Handle(SessionUserInviteAcceptedDelegate);

	UE_LOG(LogCatVentures, Log, TEXT("UCatGameInstance::Init — OSS: %s. SessionInterface ready."),
		*OSS->GetSubsystemName().ToString());
}

void UCatGameInstance::Shutdown()
{
	FWorldDelegates::OnNetDriverCreated.Remove(NetDriverCreatedDelegateHandle);
	NetDriverCreatedDelegateHandle.Reset();

	Super::Shutdown();
}

// ══════════════════════════════════════════════════════════════════════════
// ── Diagnostics ──────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::HandleNetDriverCreated(UWorld* World, UNetDriver* NetDriver)
{
	if (!NetDriver)
	{
		return;
	}

	// Def=GameNetDriver Class=SteamSocketsNetDriver is the healthy Steam-transport
	// line. Class=IpNetDriver on a Steam build means the configured driver failed to
	// load or reported IsAvailable()==false and the engine quietly fell back — the
	// exact state that made every lobby join fail in ~3 ms with no explanation.
	UE_LOG(LogCatVentures, Log, TEXT("[Net] NetDriver created  Def=%s  Class=%s  Obj=%s"),
		*NetDriver->GetNetDriverDefinition().ToString(),
		*NetDriver->GetClass()->GetName(),
		*NetDriver->GetName());
}

void UCatGameInstance::WarnIfNotSteam(const TCHAR* Context, bool bIsLAN) const
{
	if (bIsLAN)
	{
		return;   // LAN never needed Steam.
	}

	IOnlineSubsystem* OSS = IOnlineSubsystem::Get();
	const FName ActiveName = OSS ? OSS->GetSubsystemName() : FName(TEXT("<none>"));

	if (ActiveName != STEAM_SUBSYSTEM)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] %s requested a non-LAN session but the active OSS is '%s', not STEAM. ")
			TEXT("Steam init failed (client not running, or -nosteam) and we fell through to the NULL subsystem — ")
			TEXT("discovery across machines will not work."), Context, *ActiveName.ToString());
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Host ─────────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::HostSession(int32 MaxPlayers, bool bIsLAN)
{
	if (bForceLANMatch && !bIsLAN)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] DEBUG: bForceLANMatch is TRUE. Overriding Host request to LAN."));
		bIsLAN = true;
	}

	UE_LOG(LogCatVentures, Log, TEXT("[Session] HostSession start  MaxPlayers=%d  LAN=%d"), MaxPlayers, bIsLAN ? 1 : 0);
	WarnIfNotSteam(TEXT("HostSession"), bIsLAN);

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] HostSession — SessionInterface invalid."));
		OnHostSessionResult.Broadcast(false);
		return;
	}

	// DestroySession is ASYNC: creating a new session under the same name before
	// the destroy completes fails with "session already exists" (the re-host-after-
	// match bug). Defer the create into HandleDestroySessionComplete.
	if (SessionInterface->GetNamedSession(SESSION_NAME))
	{
		PendingHostMaxPlayers = MaxPlayers;
		bPendingHostIsLAN     = bIsLAN;
		PendingSessionAction  = ECatPendingSessionAction::Host;

		DestroySessionCompleteDelegateHandle =
			SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegate);

		UE_LOG(LogCatVentures, Log, TEXT("[Session] HostSession — existing session found, destroying first."));
		if (!SessionInterface->DestroySession(SESSION_NAME))
		{
			SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegateHandle);
			PendingSessionAction = ECatPendingSessionAction::None;
			UE_LOG(LogCatVentures, Warning, TEXT("[Session] HostSession — DestroySession call failed immediately."));
			OnHostSessionResult.Broadcast(false);
		}
		return;
	}

	CreateSessionInternal(MaxPlayers, bIsLAN);
}

void UCatGameInstance::HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful)
{
	SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegateHandle);

	// Consume the action BEFORE dispatching. Every branch below can re-enter the
	// session system (a join issues JoinSession, a host issues CreateSession), and
	// a stale action left set here would misroute the NEXT destroy that completes.
	const ECatPendingSessionAction Action = PendingSessionAction;
	PendingSessionAction = ECatPendingSessionAction::None;

	UE_LOG(LogCatVentures, Log, TEXT("[Session] HandleDestroySessionComplete  Session=%s  Success=%d  Action=%s"),
		*SessionName.ToString(), bWasSuccessful ? 1 : 0, *PendingActionToString(Action));

	switch (Action)
	{
	case ECatPendingSessionAction::Quit:
		// Deliberately ignores bWasSuccessful: the player asked to leave, so a failed
		// teardown must not strand them in the menu. Worst case is a stale lobby that
		// Steam times out on its own.
		if (!bWasSuccessful)
		{
			UE_LOG(LogCatVentures, Warning, TEXT("[Session] Session teardown failed on quit — quitting anyway; the lobby may linger until Steam expires it."));
		}
		ExecuteQuit();
		return;

	case ECatPendingSessionAction::LeaveToMenu:
		// Same reasoning as Quit — travel regardless. A player stuck on a scoreboard
		// is a worse outcome than a lobby Steam will expire on its own.
		if (!bWasSuccessful)
		{
			UE_LOG(LogCatVentures, Warning, TEXT("[Session] Session teardown failed on leave — travelling to the menu anyway; the next Host/Join will retry the destroy."));
		}
		TravelToMainMenu();
		return;

	case ECatPendingSessionAction::Join:
		// Unlike Quit/Leave this one MUST honour the result: joining with the old
		// session still registered is exactly the failure this destroy exists to
		// prevent, so proceeding anyway would just reproduce it.
		if (!bWasSuccessful)
		{
			UE_LOG(LogCatVentures, Warning, TEXT("[Session] Session teardown failed before join — aborting; JoinSession would fail with UnknownError."));
			OnJoinSessionResult.Broadcast(false, FString());
			return;
		}
		JoinSessionInternal(PendingJoinResult);
		return;

	case ECatPendingSessionAction::Host:
		if (!bWasSuccessful)
		{
			OnHostSessionResult.Broadcast(false);
			return;
		}
		CreateSessionInternal(PendingHostMaxPlayers, bPendingHostIsLAN);
		return;

	default:
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] HandleDestroySessionComplete fired with no pending action — ignoring."));
		return;
	}
}

void UCatGameInstance::CreateSessionInternal(int32 MaxPlayers, bool bIsLAN)
{
	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections  = MaxPlayers;
	Settings.bIsLANMatch           = bIsLAN;
	Settings.bUsesPresence         = true;   // Required for Steam lobby P2P discovery
	Settings.bShouldAdvertise      = true;
	Settings.bUseLobbiesIfAvailable = true;  // Steam lobbies, not dedicated game servers
	Settings.bAllowJoinInProgress  = true;
	Settings.bAllowInvites          = true;
	Settings.bAllowJoinViaPresence  = true;   // enables overlay "Join Game" button

	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!LocalPlayer || !LocalPlayer->GetPreferredUniqueNetId().IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] CreateSessionInternal — no local player / net id."));
		OnHostSessionResult.Broadcast(false);
		return;
	}

	CreateSessionCompleteDelegateHandle =
		SessionInterface->AddOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteDelegate);

	if (!SessionInterface->CreateSession(*LocalPlayer->GetPreferredUniqueNetId(), SESSION_NAME, Settings))
	{
		SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteDelegateHandle);
		UE_LOG(LogCatVentures, Warning, TEXT("HostSession — CreateSession call failed immediately."));
		OnHostSessionResult.Broadcast(false);
	}
}

void UCatGameInstance::HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful)
{
	SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteDelegateHandle);

	UE_LOG(LogCatVentures, Log, TEXT("[Session] HandleCreateSessionComplete  Session=%s  Success=%d"),
		*SessionName.ToString(), bWasSuccessful ? 1 : 0);

	OnHostSessionResult.Broadcast(bWasSuccessful);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Find ─────────────────────────────════════════════════════════════════
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::FindSessions(int32 MaxResults, bool bIsLAN)
{
	if (bForceLANMatch && !bIsLAN)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] DEBUG: bForceLANMatch is TRUE. Overriding Find request to LAN."));
		bIsLAN = true;
	}

	UE_LOG(LogCatVentures, Log, TEXT("[Session] FindSessions start  MaxResults=%d  LAN=%d"), MaxResults, bIsLAN ? 1 : 0);
	WarnIfNotSteam(TEXT("FindSessions"), bIsLAN);

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] FindSessions — SessionInterface invalid."));
		OnFindSessionsComplete.Broadcast(false);
		return;
	}

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->MaxSearchResults = MaxResults;
	SessionSearch->bIsLanQuery      = bIsLAN;
	// SEARCH_LOBBIES ("LOBBYSEARCH") is the key OSS Steam checks in UE 5.7 to route
	// FindSessions to RequestLobbyList (the lobby path our sessions live on).
	// The old SEARCH_PRESENCE key was removed from the engine — without SEARCH_LOBBIES
	// the search silently falls through to the internet *server* query and finds nothing.
	SessionSearch->QuerySettings.Set(SEARCH_LOBBIES, true, EOnlineComparisonOp::Equals);

	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!LocalPlayer || !LocalPlayer->GetPreferredUniqueNetId().IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] FindSessions — no local player / net id."));
		OnFindSessionsComplete.Broadcast(false);
		return;
	}

	FindSessionsCompleteDelegateHandle =
		SessionInterface->AddOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteDelegate);

	if (!SessionInterface->FindSessions(*LocalPlayer->GetPreferredUniqueNetId(), SessionSearch.ToSharedRef()))
	{
		SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteDelegateHandle);
		UE_LOG(LogCatVentures, Warning, TEXT("FindSessions — FindSessions call failed immediately."));
		OnFindSessionsComplete.Broadcast(false);
	}
}

void UCatGameInstance::HandleFindSessionsComplete(bool bWasSuccessful)
{
	SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteDelegateHandle);

	const int32 NumResults = SessionSearch.IsValid() ? SessionSearch->SearchResults.Num() : 0;
	UE_LOG(LogCatVentures, Log, TEXT("[Session] HandleFindSessionsComplete  Success=%d  Results=%d"),
		bWasSuccessful ? 1 : 0, NumResults);

	// Treat "succeeded but found 0 sessions" as a failed search so Blueprint
	// can show a "no sessions found" message without extra null checks.
	const bool bFoundAny = bWasSuccessful && NumResults > 0;
	OnFindSessionsComplete.Broadcast(bFoundAny);
}

TArray<FBlueprintSessionResult> UCatGameInstance::GetFoundSessions() const
{
	TArray<FBlueprintSessionResult> Results;
	if (!SessionSearch.IsValid()) return Results;

	for (const FOnlineSessionSearchResult& SearchResult : SessionSearch->SearchResults)
	{
		FBlueprintSessionResult BPResult;
		BPResult.OnlineResult = SearchResult;
		Results.Add(BPResult);
	}
	return Results;
}

// ══════════════════════════════════════════════════════════════════════════
// ── Join ─────────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::JoinFoundSession(const FBlueprintSessionResult& SessionResult)
{
	const FString OwningName = SessionResult.OnlineResult.Session.OwningUserName;
	UE_LOG(LogCatVentures, Log, TEXT("[Session] JoinFoundSession start  OwningUser=%s  Ping=%dms"),
		*OwningName, SessionResult.OnlineResult.PingInMs);

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] JoinFoundSession — SessionInterface invalid."));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!LocalPlayer || !LocalPlayer->GetPreferredUniqueNetId().IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] JoinFoundSession — no local player / net id."));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	// Exactly the async trap HostSession has guarded since the re-host-after-match
	// bug: JoinSession under a name that still holds a live session fails
	// SYNCHRONOUSLY with UnknownError. Hosting handled it, joining did not — so a
	// player who had hosted (or joined) once could never join again without
	// restarting the game, because nothing on the way back to the menu tore the
	// session down. Defer the join into HandleDestroySessionComplete.
	if (SessionInterface->GetNamedSession(SESSION_NAME))
	{
		PendingJoinResult    = SessionResult.OnlineResult;
		PendingSessionAction = ECatPendingSessionAction::Join;

		DestroySessionCompleteDelegateHandle =
			SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegate);

		UE_LOG(LogCatVentures, Log, TEXT("[Session] JoinFoundSession — existing session found, destroying first."));
		if (!SessionInterface->DestroySession(SESSION_NAME))
		{
			SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegateHandle);
			PendingSessionAction = ECatPendingSessionAction::None;
			UE_LOG(LogCatVentures, Warning, TEXT("[Session] JoinFoundSession — DestroySession call failed immediately."));
			OnJoinSessionResult.Broadcast(false, FString());
		}
		return;
	}

	JoinSessionInternal(SessionResult.OnlineResult);
}

void UCatGameInstance::JoinSessionInternal(const FOnlineSessionSearchResult& SearchResult)
{
	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!LocalPlayer || !LocalPlayer->GetPreferredUniqueNetId().IsValid())
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] JoinSessionInternal — no local player / net id."));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	JoinSessionCompleteDelegateHandle =
		SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegate);

	if (!SessionInterface->JoinSession(*LocalPlayer->GetPreferredUniqueNetId(), SESSION_NAME, SearchResult))
	{
		SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] JoinSessionInternal — JoinSession call failed immediately."));
		OnJoinSessionResult.Broadcast(false, FString());
	}
}

void UCatGameInstance::HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result)
{
	SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);

	UE_LOG(LogCatVentures, Log, TEXT("[Session] HandleJoinSessionComplete entry  Session=%s  Result=%s"),
		*SessionName.ToString(), *JoinResultToString(Result));

	if (Result != EOnJoinSessionCompleteResult::Success)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] Join FAILED at OSS layer  Result=%s"),
			*JoinResultToString(Result));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	// Resolve the Steam P2P connect string — passed to ClientTravel below.
	FString ConnectString;
	if (!SessionInterface->GetResolvedConnectString(SessionName, ConnectString))
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] GetResolvedConnectString FAILED  Session=%s"),
			*SessionName.ToString());
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	UE_LOG(LogCatVentures, Log, TEXT("[Session] ResolvedConnectString=%s"), *ConnectString);

	// Track whether ClientTravel actually fired. Broadcasting "true" without a real
	// travel is the bug that produced the ghost "JOIN TRUE" symptom — the BP printed
	// success even though nothing happened on the wire.
	APlayerController* PC = GetFirstLocalPlayerController();
	const bool bDidTravel = (PC != nullptr);

	if (bDidTravel)
	{
		UE_LOG(LogCatVentures, Log, TEXT("[Session] About to ClientTravel  PC=%s  URL=%s"),
			*PC->GetName(), *ConnectString);
		PC->ClientTravel(ConnectString, ETravelType::TRAVEL_Absolute);
		UE_LOG(LogCatVentures, Log, TEXT("[Session] ClientTravel issued — engine now in PendingNetGame state"));
	}
	else
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] No local PlayerController; ClientTravel SKIPPED. ")
			TEXT("Reporting failure to BP so the UI does not falsely show success."));
	}

	OnJoinSessionResult.Broadcast(bDidTravel, ConnectString);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Leave ────────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::LeaveToMainMenu()
{
	if (SessionInterface.IsValid() && SessionInterface->GetNamedSession(SESSION_NAME))
	{
		PendingSessionAction = ECatPendingSessionAction::LeaveToMenu;

		DestroySessionCompleteDelegateHandle =
			SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegate);

		UE_LOG(LogCatVentures, Log, TEXT("[Session] LeaveToMainMenu — live session found, destroying before travel."));
		if (SessionInterface->DestroySession(SESSION_NAME))
		{
			return;   // TravelToMainMenu runs from HandleDestroySessionComplete.
		}

		SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegateHandle);
		PendingSessionAction = ECatPendingSessionAction::None;
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] LeaveToMainMenu — DestroySession call failed immediately; travelling anyway."));
	}

	TravelToMainMenu();
}

void UCatGameInstance::TravelToMainMenu()
{
	// Absolute travel with NO options. The `?listen` the host's old ServerTravel
	// carried is what made the menu return fail: the new net driver tried to bind
	// P2P vport 7777 while the outgoing driver still held it, so the listen failed
	// and the machine fell through to Map_Title?closed. A menu is never a server.
	UE_LOG(LogCatVentures, Log, TEXT("[Session] Travelling to the main menu (%s)."), *MAIN_MENU_LEVEL.ToString());
	UGameplayStatics::OpenLevel(this, MAIN_MENU_LEVEL, /*bAbsolute=*/true);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Quit ─────────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::QuitToDesktop()
{
	// Teardown lives here because SESSION_NAME lives here. The engine's Destroy
	// Session Blueprint proxy operates on NAME_GameSession, so wiring the menu
	// button to it would report success while destroying nothing.
	if (SessionInterface.IsValid() && SessionInterface->GetNamedSession(SESSION_NAME))
	{
		PendingSessionAction = ECatPendingSessionAction::Quit;

		DestroySessionCompleteDelegateHandle =
			SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegate);

		UE_LOG(LogCatVentures, Log, TEXT("[Session] QuitToDesktop — live session found, destroying before quit."));
		if (SessionInterface->DestroySession(SESSION_NAME))
		{
			return;   // ExecuteQuit runs from HandleDestroySessionComplete.
		}

		SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegateHandle);
		PendingSessionAction = ECatPendingSessionAction::None;
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] QuitToDesktop — DestroySession call failed immediately; quitting anyway."));
	}

	ExecuteQuit();
}

void UCatGameInstance::ExecuteQuit()
{
	UE_LOG(LogCatVentures, Log, TEXT("[Session] Quitting to desktop."));
	UKismetSystemLibrary::QuitGame(this, GetFirstLocalPlayerController(GetWorld()), EQuitPreference::Quit, false);
}

void UCatGameInstance::HandleSessionUserInviteAccepted(
	const bool bWasSuccessful,
	const int32 ControllerId,
	FUniqueNetIdPtr UserId,
	const FOnlineSessionSearchResult& InviteResult)
{
	UE_LOG(LogCatVentures, Log, TEXT("[Session] OVERLAY invite accepted  Success=%d  ControllerId=%d"),
		bWasSuccessful ? 1 : 0, ControllerId);
	if (!bWasSuccessful) return;

	FBlueprintSessionResult BPResult;
	BPResult.OnlineResult = InviteResult;
	UE_LOG(LogCatVentures, Log, TEXT("[Session] OVERLAY invite -> JoinFoundSession"));
	JoinFoundSession(BPResult);
}
