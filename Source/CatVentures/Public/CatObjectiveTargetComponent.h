// CatObjectiveTargetComponent.h — marks an actor as an objective target.
//
// Pattern-sibling of BPC_ChaosItem: it may live on the SAME actor (a prop can be
// both a chaos scorer and an objective target). Deliberately C++ rather than a
// Blueprint component — the registry, the startup validation and the evaluator
// are all C++, and keeping the reporting side in the same language means no
// Blueprint graph surgery to add or change a condition type.
//
// CONTRACT: targets report condition-state CHANGES. They never know which
// objective they belong to — the GameMode owns evaluation. Keeping that boundary
// is what lets new operators land in EvaluateObjective and nowhere else.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CatObjectiveTargetComponent.generated.h"

UCLASS(ClassGroup = (CatVentures), meta = (BlueprintSpawnableComponent))
class CATVENTURES_API UCatObjectiveTargetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCatObjectiveTargetComponent();

	/** Matches FObjectiveCondition::TargetTag. The ChaosRewardKey pattern, reused.
	 *  A None tag is a level-authoring error and is reported by startup validation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Objective")
	FName ObjectiveTag;

	/** Reports this target destroyed to the GameMode.
	 *
	 *  Today every objective target is also a chaos prop, so the report arrives
	 *  automatically: ACatGameMode::ReportItemDestroyed finds this component on the
	 *  reported actor and routes it — zero Blueprint edits. This entry point exists
	 *  for targets that are NOT chaos props (Relocate/KnockOff props that never
	 *  shatter), so the pipeline does not silently assume every target scores chaos. */
	UFUNCTION(BlueprintCallable, Category = "Objective")
	void ReportDestroyed();

	/** True once this target has reported destruction. Read by the evaluator so a
	 *  Destroy condition can be re-evaluated from registry state rather than
	 *  depending on event ordering. */
	UFUNCTION(BlueprintPure, Category = "Objective")
	bool IsDestroyed() const { return bDestroyed; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Server-authoritative: set when destruction is reported. */
	bool bDestroyed = false;

	/** Guards against double-registration if BeginPlay ever runs twice. */
	bool bRegistered = false;

	/** Returns the authoritative GameMode, or null on clients (the cast naturally
	 *  fails there — the same shape ReportItemDestroyed relies on). */
	class ACatGameMode* GetCatGameMode() const;
};
