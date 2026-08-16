// CatGameInstance.h — Steam session manager. C++ backend only; no UI or ServerTravel here.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "FindSessionsCallbackProxy.h"   // FBlueprintSessionResult (OnlineSubsystemUtils)
#include "CatGameInstance.generated.h"

// ── Blueprint-assignable delegate types ──────────────────────────────────

/** Fired when HostSession completes. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHostSessionResult,
	bool, bWasSuccessful);

/** Fired when FindSessions completes. bWasSuccessful is false if the search
 *  timed out or returned 0 results. Call GetFoundSessions() to read results. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFindSessionsResult,
	bool, bWasSuccessful);

/** Fired when JoinSession completes. ConnectionString is the resolved Steam P2P URL —
 *  pass it to PlayerController::ClientTravel in Blueprint. Only valid when bWasSuccessful. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnJoinSessionResult,
	bool, bWasSuccessful,
	FString, ConnectionString);


/** Which caller issued the DestroySession that is currently in flight.
 *
 *  DestroySession is async and there is exactly ONE completion delegate, so the
 *  callback has to be told what to do next. Four different entry points now tear
 *  a session down before doing their real work (re-host, join, leave-to-menu,
 *  quit) — this was a single bool while only two existed, and adding a third
 *  boolean would have been the point where a missed check silently re-created a
 *  session someone was trying to leave. */
enum class ECatPendingSessionAction : uint8
{
	None,
	Host,         // → CreateSessionInternal
	Join,         // → JoinSessionInternal(PendingJoinResult)
	LeaveToMenu,  // → TravelToMainMenu
	Quit          // → ExecuteQuit
};


UCLASS()
class CATVENTURES_API UCatGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:

	// ── Developer Toggles ─────────────────────────────────────────────────

	/** When true, HostSession and FindSessions force bIsLAN=true regardless of
	 *  the value passed from Blueprint. Lets us A/B test the LAN code path
	 *  without touching the UI graph. UCatGameInstance is assigned directly in
	 *  DefaultEngine.ini (no Blueprint subclass), so there is no CDO to tick in
	 *  the editor — Init() also sets this from the `-ForceLAN` command line
	 *  parameter, which is how it gets enabled on a packaged build. Never ship
	 *  enabled. Pair with `-nosteam` for a pure UE LAN networking A/B. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network|Debug")
	bool bForceLANMatch = false;

	// ── Host ──────────────────────────────────────────────────────────────

	/** Creates a named Steam session and advertises it for discovery.
	 *  Broadcasts OnHostSessionResult when the async call completes. */
	UFUNCTION(BlueprintCallable, Category = "Session")
	void HostSession(int32 MaxPlayers, bool bIsLAN);

	UPROPERTY(BlueprintAssignable, Category = "Session")
	FOnHostSessionResult OnHostSessionResult;

	// ── Find ──────────────────────────────────────────────────────────────

	/** Searches for active sessions. Broadcasts OnFindSessionsComplete when done.
	 *  Call GetFoundSessions() inside that delegate to retrieve the result list. */
	UFUNCTION(BlueprintCallable, Category = "Session")
	void FindSessions(int32 MaxResults, bool bIsLAN);

	UPROPERTY(BlueprintAssignable, Category = "Session")
	FOnFindSessionsResult OnFindSessionsComplete;

	/** Returns the session list populated by the last successful FindSessions call.
	 *  Safe to call any time; returns empty array if no search has completed yet. */
	UFUNCTION(BlueprintCallable, Category = "Session")
	TArray<FBlueprintSessionResult> GetFoundSessions() const;

	// ── Join ──────────────────────────────────────────────────────────────

	/** Joins a session from the find results. Broadcasts OnJoinSessionResult with
	 *  the resolved travel URL — Blueprint calls ClientTravel(ConnectionString). */
	UFUNCTION(BlueprintCallable, Category = "Session")
	void JoinFoundSession(const FBlueprintSessionResult& SessionResult);

	UPROPERTY(BlueprintAssignable, Category = "Session")
	FOnJoinSessionResult OnJoinSessionResult;

	// ── Leave ─────────────────────────────────────────────────────────────

	/** Tears down any live session, then returns this machine to the main menu.
	 *  Every "return to menu" button — host and client alike — must call THIS.
	 *
	 *  What it replaces, and why both were broken:
	 *   - The client button used a bare Open Level. That travels fine but leaves
	 *     the joined session registered under SESSION_NAME, so the next JoinSession
	 *     fails synchronously with UnknownError.
	 *   - The host button used Host_ServerTravel, which carries `?listen` through
	 *     the travel. The old SteamSockets listen socket is not released before the
	 *     new net driver binds, so it dies on "Already have a listen socket on P2P
	 *     vport 7777", falls through to Map_Title?closed, and leaves the machine on
	 *     the title screen still advertising a session it can no longer serve.
	 *
	 *  Travel is absolute and carries NO listen option, so no socket collision is
	 *  possible. Travels anyway if the teardown fails: a menu button that does
	 *  nothing is worse than a stale lobby. */
	UFUNCTION(BlueprintCallable, Category = "Session")
	void LeaveToMainMenu();

	// ── Quit ──────────────────────────────────────────────────────────────

	/** Tears down any live session, then closes the game. Menus must call THIS and
	 *  not a bare Quit Game node: our session is named SESSION_NAME
	 *  ("CatVenturesSession"), while the engine's Destroy Session Blueprint proxy
	 *  targets NAME_GameSession — so that node destroys nothing and leaves a stale
	 *  lobby advertised on Steam until it times out. Quits immediately when no
	 *  session exists, and quits anyway if the teardown fails: a quit button that
	 *  does nothing is worse than a stale lobby. */
	UFUNCTION(BlueprintCallable, Category = "Session")
	void QuitToDesktop();

protected:
	virtual void Init() override;
	virtual void Shutdown() override;

private:
	// ── OSS interface cache ────────────────────────────────────────────────
	IOnlineSessionPtr SessionInterface;

	// ── Net driver identity logging ───────────────────────────────────────
	// CreateNamedNetDriver loads DriverClassNameFallback with LOAD_Quiet when the
	// configured driver class is missing or reports IsAvailable()==false — silently,
	// with no warning anywhere. That silence hid a dead SteamNetDriver config for
	// three months (every build ran raw IpNetDriver while lobbies advertised Steam
	// P2P URLs). Naming the driver that was actually created makes the fallback
	// impossible to miss again, on host and client alike.
	FDelegateHandle NetDriverCreatedDelegateHandle;
	void HandleNetDriverCreated(UWorld* World, UNetDriver* NetDriver);

	/** Warns when a non-LAN session op runs on something other than the Steam OSS —
	 *  i.e. Steam init failed and we silently fell through to the NULL subsystem. */
	void WarnIfNotSteam(const TCHAR* Context, bool bIsLAN) const;

	// ── Search state ──────────────────────────────────────────────────────
	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	// ── Pending re-host state ─────────────────────────────────────────────
	// DestroySession is async: when HostSession is called while a session still
	// exists (re-hosting after a match), the create is deferred until the destroy
	// completes. These hold the requested parameters across that gap.
	int32 PendingHostMaxPlayers = 0;
	bool  bPendingHostIsLAN = false;

	// ── Pending destroy-then-X state ──────────────────────────────────────
	// DestroySessionCompleteDelegate is shared by every path that must tear a
	// session down first. This says which one issued the in-flight destroy, so a
	// quit or a menu return never falls through into re-creating a session.
	ECatPendingSessionAction PendingSessionAction = ECatPendingSessionAction::None;

	// The join being deferred across a destroy. Held by value: the search result
	// must outlive the async gap, and the caller's FBlueprintSessionResult does not.
	FOnlineSessionSearchResult PendingJoinResult;

	/** Issues the actual JoinSession call. Shared by the direct join path and the
	 *  deferred destroy-then-join path. */
	void JoinSessionInternal(const FOnlineSessionSearchResult& SearchResult);

	/** Absolute, non-listen travel to the menu map. Shared by the immediate and
	 *  post-teardown leave paths. */
	void TravelToMainMenu();

	/** Closes the game. Shared by the immediate and post-teardown quit paths. */
	void ExecuteQuit();

	// ── Native OSS delegates ──────────────────────────────────────────────
	// Constructed once in Init(). Registered immediately before each async OSS
	// call and cleared inside the completion callback to prevent duplicate fires.
	FOnCreateSessionCompleteDelegate      CreateSessionCompleteDelegate;
	FOnDestroySessionCompleteDelegate     DestroySessionCompleteDelegate;
	FOnFindSessionsCompleteDelegate       FindSessionsCompleteDelegate;
	FOnJoinSessionCompleteDelegate        JoinSessionCompleteDelegate;
	FOnSessionUserInviteAcceptedDelegate  SessionUserInviteAcceptedDelegate;

	FDelegateHandle CreateSessionCompleteDelegateHandle;
	FDelegateHandle DestroySessionCompleteDelegateHandle;
	FDelegateHandle FindSessionsCompleteDelegateHandle;
	FDelegateHandle JoinSessionCompleteDelegateHandle;

	/** Issues the actual CreateSession call. Shared by the direct host path and
	 *  the deferred destroy-then-recreate path. */
	void CreateSessionInternal(int32 MaxPlayers, bool bIsLAN);

	// ── Native callbacks (bound to delegates above) ───────────────────────
	void HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	void HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful);
	void HandleFindSessionsComplete(bool bWasSuccessful);
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	void HandleSessionUserInviteAccepted(const bool bWasSuccessful, const int32 ControllerId,
		FUniqueNetIdPtr UserId, const FOnlineSessionSearchResult& InviteResult);
};
