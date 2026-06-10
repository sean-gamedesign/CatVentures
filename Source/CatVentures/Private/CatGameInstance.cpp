// CatGameInstance.cpp

#include "CatGameInstance.h"
#include "CatVenturesLog.h"
#include "OnlineSubsystem.h"
#include "OnlineSessionSettings.h"
#include "Online/OnlineSessionNames.h"   // SEARCH_LOBBIES
#include "GameFramework/PlayerController.h"

// ── Session name shared across all methods ────────────────────────────────
static const FName SESSION_NAME = FName("CatVenturesSession");

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

		DestroySessionCompleteDelegateHandle =
			SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegate);

		UE_LOG(LogCatVentures, Log, TEXT("[Session] HostSession — existing session found, destroying first."));
		if (!SessionInterface->DestroySession(SESSION_NAME))
		{
			SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegateHandle);
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

	UE_LOG(LogCatVentures, Log, TEXT("[Session] HandleDestroySessionComplete  Session=%s  Success=%d"),
		*SessionName.ToString(), bWasSuccessful ? 1 : 0);

	if (!bWasSuccessful)
	{
		OnHostSessionResult.Broadcast(false);
		return;
	}

	CreateSessionInternal(PendingHostMaxPlayers, bPendingHostIsLAN);
}

void UCatGameInstance::CreateSessionInternal(int32 MaxPlayers, bool bIsLAN)
{
	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections  = MaxPlayers;
	Settings.bIsLANMatch           = bIsLAN;
	Settings.bUsesPresence         = true;   // Required for Steam lobby P2P discovery
	Settings.bShouldAdvertise      = true;
	Settings.bUseLobbiesIfAvailable = true;  // Steam AppID 480: lobbies, not game servers
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
	// FindSessions to RequestLobbyList (the lobby path AppID 480 sessions live on).
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

	JoinSessionCompleteDelegateHandle =
		SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegate);

	if (!SessionInterface->JoinSession(*LocalPlayer->GetPreferredUniqueNetId(), SESSION_NAME, SessionResult.OnlineResult))
	{
		SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);
		UE_LOG(LogCatVentures, Warning, TEXT("[Session] JoinFoundSession — JoinSession call failed immediately."));
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
