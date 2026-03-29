// PauseMenuWidget.cpp

#include "PauseMenuWidget.h"
#include "CatPlayerController.h"

FReply UPauseMenuWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (ACatPlayerController* PC = Cast<ACatPlayerController>(GetOwningPlayer()))
		{
			PC->ToggleMenu();
			return FReply::Handled();
		}
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
