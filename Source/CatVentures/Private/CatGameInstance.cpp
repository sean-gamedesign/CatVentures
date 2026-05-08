// CatGameInstance.cpp

#include "CatGameInstance.h"
#include "OnlineSubsystem.h"
#include "OnlineSessionSettings.h"
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
		UE_LOG(LogTemp, Warning, TEXT("UCatGameInstance::Init — No OnlineSubsystem found. "
			"Session features disabled. Is Steam running?"));
		return;
	}

	SessionInterface = OSS->GetSessionInterface();
	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("UCatGameInstance::Init — SessionInterface is invalid."));
		return;
	}

	// Pre-bind native delegates — registered/unregistered around each async call.
	CreateSessionCompleteDelegate = FOnCreateSessionCompleteDelegate::CreateUObject(
		this, &UCatGameInstance::HandleCreateSessionComplete);
	FindSessionsCompleteDelegate = FOnFindSessionsCompleteDelegate::CreateUObject(
		this, &UCatGameInstance::HandleFindSessionsComplete);
	JoinSessionCompleteDelegate = FOnJoinSessionCompleteDelegate::CreateUObject(
		this, &UCatGameInstance::HandleJoinSessionComplete);

	// Persistent delegate — fired by OSS Steam when the user clicks "Join Game" in the overlay.
	// Registered once; never removed (matches UE5 convention for this callback).
	SessionUserInviteAcceptedDelegate = FOnSessionUserInviteAcceptedDelegate::CreateUObject(
		this, &UCatGameInstance::HandleSessionUserInviteAccepted);
	SessionInterface->AddOnSessionUserInviteAcceptedDelegate_Handle(SessionUserInviteAcceptedDelegate);

	UE_LOG(LogTemp, Log, TEXT("UCatGameInstance::Init — OSS: %s. SessionInterface ready."),
		*OSS->GetSubsystemName().ToString());
}

// ══════════════════════════════════════════════════════════════════════════
// ── Host ─────────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::HostSession(int32 MaxPlayers, bool bIsLAN)
{
	UE_LOG(LogTemp, Log, TEXT("[Session] HostSession start  MaxPlayers=%d  LAN=%d"), MaxPlayers, bIsLAN ? 1 : 0);

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Session] HostSession — SessionInterface invalid."));
		OnHostSessionResult.Broadcast(false);
		return;
	}

	// Destroy any existing session under SESSION_NAME before creating a new one.
	if (SessionInterface->GetNamedSession(SESSION_NAME))
	{
		SessionInterface->DestroySession(SESSION_NAME);
	}

	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections  = MaxPlayers;
	Settings.bIsLANMatch           = bIsLAN;
	Settings.bUsesPresence         = true;   // Required for Steam lobby P2P discovery
	Settings.bShouldAdvertise      = true;
	Settings.bUseLobbiesIfAvailable = true;  // Steam AppID 480: lobbies, not game servers
	Settings.bAllowJoinInProgress  = true;
	Settings.bAllowInvites          = true;
	Settings.bAllowJoinViaPresence  = true;   // enables overlay "Join Game" button

	CreateSessionCompleteDelegateHandle =
		SessionInterface->AddOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteDelegate);

	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!SessionInterface->CreateSession(*LocalPlayer->GetPreferredUniqueNetId(), SESSION_NAME, Settings))
	{
		SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteDelegateHandle);
		UE_LOG(LogTemp, Warning, TEXT("HostSession — CreateSession call failed immediately."));
		OnHostSessionResult.Broadcast(false);
	}
}

void UCatGameInstance::HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful)
{
	SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteDelegateHandle);

	UE_LOG(LogTemp, Log, TEXT("[Session] HandleCreateSessionComplete  Session=%s  Success=%d"),
		*SessionName.ToString(), bWasSuccessful ? 1 : 0);

	OnHostSessionResult.Broadcast(bWasSuccessful);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Find ─────────────────────────────════════════════════════════════════
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::FindSessions(int32 MaxResults, bool bIsLAN)
{
	UE_LOG(LogTemp, Log, TEXT("[Session] FindSessions start  MaxResults=%d  LAN=%d"), MaxResults, bIsLAN ? 1 : 0);

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Session] FindSessions — SessionInterface invalid."));
		OnFindSessionsComplete.Broadcast(false);
		return;
	}

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->MaxSearchResults = MaxResults;
	SessionSearch->bIsLanQuery      = bIsLAN;
	// SEARCH_PRESENCE is the OSS Steam key that routes to RequestLobbyList (Steam lobby path).
	// The literal "PRESENCE" does not match and bypasses lobby discovery entirely.
	SessionSearch->QuerySettings.Set(FName(TEXT("SEARCH_PRESENCE")), true, EOnlineComparisonOp::Equals);

	FindSessionsCompleteDelegateHandle =
		SessionInterface->AddOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteDelegate);

	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!SessionInterface->FindSessions(*LocalPlayer->GetPreferredUniqueNetId(), SessionSearch.ToSharedRef()))
	{
		SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteDelegateHandle);
		UE_LOG(LogTemp, Warning, TEXT("FindSessions — FindSessions call failed immediately."));
		OnFindSessionsComplete.Broadcast(false);
	}
}

void UCatGameInstance::HandleFindSessionsComplete(bool bWasSuccessful)
{
	SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteDelegateHandle);

	const int32 NumResults = SessionSearch.IsValid() ? SessionSearch->SearchResults.Num() : 0;
	UE_LOG(LogTemp, Log, TEXT("[Session] HandleFindSessionsComplete  Success=%d  Results=%d"),
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
	UE_LOG(LogTemp, Log, TEXT("[Session] JoinFoundSession start  OwningUser=%s  Ping=%dms"),
		*OwningName, SessionResult.OnlineResult.PingInMs);

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Session] JoinFoundSession — SessionInterface invalid."));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	JoinSessionCompleteDelegateHandle =
		SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegate);

	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!SessionInterface->JoinSession(*LocalPlayer->GetPreferredUniqueNetId(), SESSION_NAME, SessionResult.OnlineResult))
	{
		SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);
		UE_LOG(LogTemp, Warning, TEXT("[Session] JoinFoundSession — JoinSession call failed immediately."));
		OnJoinSessionResult.Broadcast(false, FString());
	}
}

void UCatGameInstance::HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result)
{
	SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);

	UE_LOG(LogTemp, Log, TEXT("[Session] HandleJoinSessionComplete entry  Session=%s  Result=%s"),
		*SessionName.ToString(), *JoinResultToString(Result));

	if (Result != EOnJoinSessionCompleteResult::Success)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Session] Join FAILED at OSS layer  Result=%s"),
			*JoinResultToString(Result));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	// Resolve the Steam P2P connect string — passed to ClientTravel below.
	FString ConnectString;
	if (!SessionInterface->GetResolvedConnectString(SessionName, ConnectString))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Session] GetResolvedConnectString FAILED  Session=%s"),
			*SessionName.ToString());
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[Session] ResolvedConnectString=%s"), *ConnectString);

	// Track whether ClientTravel actually fired. Broadcasting "true" without a real
	// travel is the bug that produced the ghost "JOIN TRUE" symptom — the BP printed
	// success even though nothing happened on the wire.
	APlayerController* PC = GetFirstLocalPlayerController();
	const bool bDidTravel = (PC != nullptr);

	if (bDidTravel)
	{
		UE_LOG(LogTemp, Log, TEXT("[Session] About to ClientTravel  PC=%s  URL=%s"),
			*PC->GetName(), *ConnectString);
		PC->ClientTravel(ConnectString, ETravelType::TRAVEL_Absolute);
		UE_LOG(LogTemp, Log, TEXT("[Session] ClientTravel issued — engine now in PendingNetGame state"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[Session] No local PlayerController; ClientTravel SKIPPED. ")
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
	UE_LOG(LogTemp, Log, TEXT("[Session] OVERLAY invite accepted  Success=%d  ControllerId=%d"),
		bWasSuccessful ? 1 : 0, ControllerId);
	if (!bWasSuccessful) return;

	FBlueprintSessionResult BPResult;
	BPResult.OnlineResult = InviteResult;
	UE_LOG(LogTemp, Log, TEXT("[Session] OVERLAY invite -> JoinFoundSession"));
	JoinFoundSession(BPResult);
}
