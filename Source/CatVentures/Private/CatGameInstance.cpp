// CatGameInstance.cpp

#include "CatGameInstance.h"
#include "OnlineSubsystem.h"
#include "OnlineSessionSettings.h"
#include "GameFramework/PlayerController.h"

// ── Session name shared across all methods ────────────────────────────────
static const FName SESSION_NAME = FName("CatVenturesSession");

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
	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("HostSession — SessionInterface invalid."));
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

	UE_LOG(LogTemp, Log, TEXT("HandleCreateSessionComplete — Session: %s | Success: %d"),
		*SessionName.ToString(), bWasSuccessful);

	OnHostSessionResult.Broadcast(bWasSuccessful);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Find ─────────────────────────────════════════════════════════════════
// ══════════════════════════════════════════════════════════════════════════

void UCatGameInstance::FindSessions(int32 MaxResults, bool bIsLAN)
{
	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FindSessions — SessionInterface invalid."));
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
	UE_LOG(LogTemp, Log, TEXT("HandleFindSessionsComplete — Success: %d | Results: %d"),
		bWasSuccessful, NumResults);

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
	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("JoinFoundSession — SessionInterface invalid."));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	JoinSessionCompleteDelegateHandle =
		SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegate);

	const ULocalPlayer* LocalPlayer = GetFirstGamePlayer();
	if (!SessionInterface->JoinSession(*LocalPlayer->GetPreferredUniqueNetId(), SESSION_NAME, SessionResult.OnlineResult))
	{
		SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);
		UE_LOG(LogTemp, Warning, TEXT("JoinFoundSession — JoinSession call failed immediately."));
		OnJoinSessionResult.Broadcast(false, FString());
	}
}

void UCatGameInstance::HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result)
{
	SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);

	if (Result != EOnJoinSessionCompleteResult::Success)
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleJoinSessionComplete — Failed. Result: %d"),
			static_cast<int32>(Result));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	// Resolve the Steam P2P connect string — Blueprint passes this to ClientTravel.
	FString ConnectString;
	if (!SessionInterface->GetResolvedConnectString(SessionName, ConnectString))
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleJoinSessionComplete — GetResolvedConnectString failed."));
		OnJoinSessionResult.Broadcast(false, FString());
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("HandleJoinSessionComplete — Success. URL: %s"), *ConnectString);

	if (APlayerController* PC = GetFirstLocalPlayerController())
	{
		PC->ClientTravel(ConnectString, ETravelType::TRAVEL_Absolute);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleJoinSessionComplete — No local PlayerController for ClientTravel."));
	}

	OnJoinSessionResult.Broadcast(true, ConnectString);
}

void UCatGameInstance::HandleSessionUserInviteAccepted(
	const bool bWasSuccessful,
	const int32 ControllerId,
	FUniqueNetIdPtr UserId,
	const FOnlineSessionSearchResult& InviteResult)
{
	UE_LOG(LogTemp, Log, TEXT("HandleSessionUserInviteAccepted — Success: %d"), bWasSuccessful);
	if (!bWasSuccessful) return;

	FBlueprintSessionResult BPResult;
	BPResult.OnlineResult = InviteResult;
	JoinFoundSession(BPResult);
}
