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


UCLASS()
class CATVENTURES_API UCatGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:

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

protected:
	virtual void Init() override;

private:
	// ── OSS interface cache ────────────────────────────────────────────────
	IOnlineSessionPtr SessionInterface;

	// ── Search state ──────────────────────────────────────────────────────
	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	// ── Native OSS delegates ──────────────────────────────────────────────
	// Constructed once in Init(). Registered immediately before each async OSS
	// call and cleared inside the completion callback to prevent duplicate fires.
	FOnCreateSessionCompleteDelegate CreateSessionCompleteDelegate;
	FOnFindSessionsCompleteDelegate  FindSessionsCompleteDelegate;
	FOnJoinSessionCompleteDelegate   JoinSessionCompleteDelegate;

	FDelegateHandle CreateSessionCompleteDelegateHandle;
	FDelegateHandle FindSessionsCompleteDelegateHandle;
	FDelegateHandle JoinSessionCompleteDelegateHandle;

	// ── Native callbacks (bound to delegates above) ───────────────────────
	void HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	void HandleFindSessionsComplete(bool bWasSuccessful);
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
};
