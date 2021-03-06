// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	FeedbackContextEditor.h: Feedback context tailored to UnrealEd

=============================================================================*/

#pragma once

/**
 * A FFeedbackContext implementation for use in UnrealEd.
 */
class FFeedbackContextEditor : public FFeedbackContext
{
	/** Slate slow task widget */
	TWeakPtr<class SWindow> SlowTaskWindow;

	/** Special Windows/Widget popup for building */
	TWeakPtr<class SWindow> BuildProgressWindow;
	TSharedPtr<class SBuildProgressWidget> BuildProgressWidget;

	bool HasTaskBeenCancelled;

public:

	UNREALED_API FFeedbackContextEditor();

	virtual void Serialize( const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category ) override;

	virtual void StartSlowTask( const FText& Task, bool bShowCancelButton=false ) override;
	virtual void FinalizeSlowTask( ) override;
	virtual void ProgressReported( const float TotalProgressInterp, FText DisplayMessage ) override;

	void SetContext( FContextSupplier* InSupplier ) {}

	/** Whether or not the user has canceled out of this dialog */
	virtual bool ReceivedUserCancel() override;

	void OnUserCancel();

	virtual bool YesNof( const FText& Question ) override
	{
		return EAppReturnType::Yes == FMessageDialog::Open( EAppMsgType::YesNo, Question );
	}

	/** 
	 * Show the Build Progress Window 
	 * @return Handle to the Build Progress Widget created
	 */
	TWeakPtr<class SBuildProgressWidget> ShowBuildProgressWindow();
	
	/** Close the Build Progress Window */
	void CloseBuildProgressWindow();
};
