// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "EnginePrivate.h"
#include "Engine/Breakpoint.h"
#include "EdGraph/EdGraph.h"
#include "BlueprintUtilities.h"
#include "LatentActions.h"

#if WITH_EDITOR
#include "Editor/UnrealEd/Public/Kismet2/BlueprintEditorUtils.h"
#include "Editor/UnrealEd/Public/Kismet2/KismetEditorUtilities.h"
#include "Editor/UnrealEd/Public/Kismet2/CompilerResultsLog.h"
#include "Editor/UnrealEd/Public/Kismet2/StructureEditorUtils.h"
#include "Editor/Kismet/Public/FindInBlueprintManager.h"
#include "Editor/UnrealEd/Public/Editor.h"
#include "Crc.h"
#include "MessageLog.h"
#include "Editor/UnrealEd/Classes/Settings/EditorLoadingSavingSettings.h"
#include "BlueprintEditorUtils.h"
#include "Engine/TimelineTemplate.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#endif
#include "Components/TimelineComponent.h"
#include "Engine/InheritableComponentHandler.h"

DEFINE_LOG_CATEGORY(LogBlueprint);

//////////////////////////////////////////////////////////////////////////
// Static Helpers

/**
 * Updates the blueprint's OwnedComponents, such that they reflect changes made 
 * natively since the blueprint was last saved (a change in AttachParents, etc.)
 * 
 * @param  Blueprint	The blueprint whose components you wish to vet.
 */
static void ConformNativeComponents(UBlueprint* Blueprint)
{
#if WITH_EDITOR
	if (UClass* const BlueprintClass = Blueprint->GeneratedClass)
	{
		if (AActor* BlueprintCDO = Cast<AActor>(BlueprintClass->ClassDefaultObject))
		{
			TInlineComponentArray<UActorComponent*> OldNativeComponents;
			// collect the native components that this blueprint was serialized out 
			// with (the native components it had last time it was saved)
			BlueprintCDO->GetComponents(OldNativeComponents);

			UClass* const NativeSuperClass = FBlueprintEditorUtils::FindFirstNativeClass(BlueprintClass);
			AActor* NativeCDO = CastChecked<AActor>(NativeSuperClass->ClassDefaultObject);
			// collect the more up to date native components (directly from the 
			// native super-class)
			TInlineComponentArray<UActorComponent*> NewNativeComponents;
			NativeCDO->GetComponents(NewNativeComponents);

			// utility lambda for finding named components in a supplied list
			auto FindNamedComponentLambda = [](FName const ComponentName, TInlineComponentArray<UActorComponent*> const& ComponentList)->UActorComponent*
			{
				UActorComponent* FoundComponent = nullptr;
				for (UActorComponent* Component : ComponentList)
				{
					if (Component->GetFName() == ComponentName)
					{
						FoundComponent = Component;
						break;
					}
				}
				return FoundComponent;
			};

			// utility lambda for finding matching components in the NewNativeComponents list
			auto FindNativeComponentLambda = [&NewNativeComponents, &FindNamedComponentLambda](UActorComponent* BlueprintComponent)->UActorComponent*
			{
				UActorComponent* MatchingComponent = nullptr;
				if (BlueprintComponent != nullptr)
				{
					FName const ComponentName = BlueprintComponent->GetFName();
					MatchingComponent = FindNamedComponentLambda(ComponentName, NewNativeComponents);
				}
				return MatchingComponent;
			};

			// loop through all components that this blueprint thinks come from its
			// native super-class (last time it was saved)
			for (UActorComponent* Component : OldNativeComponents)
			{
				// if we found this component also listed for the native class
				if (UActorComponent* NativeComponent = FindNativeComponentLambda(Component))
				{
					USceneComponent* BlueprintSceneComponent = Cast<USceneComponent>(Component);
					if (BlueprintSceneComponent == nullptr)
					{
						// if this isn't a scene-component, then we don't care
						// (we're looking to fixup scene-component parents)
						continue;
					}
					UActorComponent* OldNativeParent = FindNativeComponentLambda(BlueprintSceneComponent->AttachParent);

					USceneComponent* NativeSceneComponent = CastChecked<USceneComponent>(NativeComponent);
					// if this native component has since been reparented, we need
					// to make sure that this blueprint reflects that change
					if (OldNativeParent != NativeSceneComponent->AttachParent)
					{
						USceneComponent* NewParent = nullptr;
						if (NativeSceneComponent->AttachParent != nullptr)
						{
							NewParent = CastChecked<USceneComponent>(FindNamedComponentLambda(NativeSceneComponent->AttachParent->GetFName(), OldNativeComponents));
						}
						BlueprintSceneComponent->AttachParent = NewParent;
					}
				}
				else // the component has been removed from the native class
				{
					// @TODO: I think we already handle removed native components elsewhere, so maybe we should error here?
// 				BlueprintCDO->RemoveOwnedComponent(Component);
// 
// 				USimpleConstructionScript* BlueprintSCS = Blueprint->SimpleConstructionScript;
// 				USCS_Node* ComponentNode = BlueprintSCS->CreateNode(Component, Component->GetFName());
// 
// 				BlueprintSCS->AddNode(ComponentNode);
				}
			}
		}
	}
#endif // #if WITH_EDITOR
}


//////////////////////////////////////////////////////////////////////////
// FBPVariableDescription

int32 FBPVariableDescription::FindMetaDataEntryIndexForKey(const FName& Key) const
{
	for(int32 i=0; i<MetaDataArray.Num(); i++)
	{
		if(MetaDataArray[i].DataKey == Key)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

bool FBPVariableDescription::HasMetaData(const FName& Key) const
{
	return FindMetaDataEntryIndexForKey(Key) != INDEX_NONE;
}

/** Gets a metadata value on the variable; asserts if the value isn't present.  Check for validiy using FindMetaDataEntryIndexForKey. */
FString FBPVariableDescription::GetMetaData(const FName& Key) const
{
	int32 EntryIndex = FindMetaDataEntryIndexForKey(Key);
	check(EntryIndex != INDEX_NONE);
	return MetaDataArray[EntryIndex].DataValue;
}

void FBPVariableDescription::SetMetaData(const FName& Key, const FString& Value)
{
	int32 EntryIndex = FindMetaDataEntryIndexForKey(Key);
	if(EntryIndex != INDEX_NONE)
	{
		MetaDataArray[EntryIndex].DataValue = Value;
	}
	else
	{
		MetaDataArray.Add( FBPVariableMetaDataEntry(Key, Value) );
	}
}

void FBPVariableDescription::RemoveMetaData(const FName& Key)
{
	int32 EntryIndex = FindMetaDataEntryIndexForKey(Key);
	if(EntryIndex != INDEX_NONE)
	{
		MetaDataArray.RemoveAt(EntryIndex);
	}
}

//////////////////////////////////////////////////////////////////////////
// UBlueprintCore

UBlueprintCore::UBlueprintCore(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bLegacyGeneratedClassIsAuthoritative = false;
	bLegacyNeedToPurgeSkelRefs = true;
}

void UBlueprintCore::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar << bLegacyGeneratedClassIsAuthoritative;	

	if ((Ar.UE4Ver() < VER_UE4_BLUEPRINT_SKEL_CLASS_TRANSIENT_AGAIN)
		&& (Ar.UE4Ver() != VER_UE4_BLUEPRINT_SKEL_TEMPORARY_TRANSIENT))
	{
		Ar << SkeletonGeneratedClass;
		if( SkeletonGeneratedClass )
		{
			// If we serialized in a skeleton class, make sure it and all its children are updated to be transient
			SkeletonGeneratedClass->SetFlags(RF_Transient);
			TArray<UObject*> SubObjs;
			GetObjectsWithOuter(SkeletonGeneratedClass, SubObjs, true);
			for(auto SubObjIt = SubObjs.CreateIterator(); SubObjIt; ++SubObjIt)
			{
				(*SubObjIt)->SetFlags(RF_Transient);
			}
		}

		// We only want to serialize in the GeneratedClass if the SkeletonClass didn't trigger a recompile
		bool bSerializeGeneratedClass = true;
		if (UBlueprint* BP = Cast<UBlueprint>(this))
		{
			bSerializeGeneratedClass = !Ar.IsLoading() || !BP->bHasBeenRegenerated;
		}

		if (bSerializeGeneratedClass)
		{
			Ar << GeneratedClass;
		}
		else if (Ar.IsLoading())
		{
			UClass* DummyClass = NULL;
			Ar << DummyClass;
		}
	}

	if( Ar.ArIsLoading && !BlueprintGuid.IsValid() )
	{
		GenerateDeterministicGuid();
	}
}

#if WITH_EDITORONLY_DATA
void UBlueprintCore::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	Super::GetAssetRegistryTags(OutTags);

	FString GeneratedClassVal;
	if ( GeneratedClass != NULL )
	{
		GeneratedClassVal = FString::Printf(TEXT("%s'%s'"), *GeneratedClass->GetClass()->GetName(), *GeneratedClass->GetPathName());
	}
	else
	{
		GeneratedClassVal = TEXT("None");
	}

	OutTags.Add( FAssetRegistryTag("GeneratedClass", GeneratedClassVal, FAssetRegistryTag::TT_Hidden) );
}
#endif

void UBlueprintCore::GenerateDeterministicGuid()
{
	FString HashString = GetPathName();
	HashString.Shrink();
	ensure( HashString.Len() );

	uint32 HashBuffer[ 5 ];
	uint32 BufferLength = HashString.Len() * sizeof( HashString[0] );
	FSHA1::HashBuffer(*HashString, BufferLength, reinterpret_cast< uint8* >( HashBuffer ));
	BlueprintGuid.A = HashBuffer[ 1 ];
	BlueprintGuid.B = HashBuffer[ 2 ];
	BlueprintGuid.C = HashBuffer[ 3 ];
	BlueprintGuid.D = HashBuffer[ 4 ];
}

UBlueprint::UBlueprint(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
#if WITH_EDITOR
	, bRunConstructionScriptOnDrag(true)
	, bGenerateConstClass(false)
#endif
#if WITH_EDITORONLY_DATA
	, bDuplicatingReadOnly(false)
	, bCachedDependenciesUpToDate(false)
#endif
{
}

void UBlueprint::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

#if WITH_EDITORONLY_DATA
	if(Ar.IsLoading() && Ar.UE4Ver() < VER_UE4_BLUEPRINT_VARS_NOT_READ_ONLY)
	{
		// Allow all blueprint defined vars to be read/write.  undoes previous convention of making exposed variables read-only
		for (int32 i = 0; i < NewVariables.Num(); ++i)
		{
			FBPVariableDescription& Variable = NewVariables[i];
			Variable.PropertyFlags &= ~CPF_BlueprintReadOnly;
		}
	}

	if (Ar.UE4Ver() < VER_UE4_K2NODE_REFERENCEGUIDS)
	{
		for (int32 Index = 0; Index < NewVariables.Num(); ++Index)
		{
			NewVariables[Index].VarGuid = FGuid::NewGuid();
		}
	}

	// Preload our parent blueprints
	if (Ar.IsLoading())
	{
		for (UClass* ClassIt = ParentClass; (ClassIt != NULL) && !(ClassIt->HasAnyClassFlags(CLASS_Native)); ClassIt = ClassIt->GetSuperClass())
		{
			if (!ensure(ClassIt->ClassGeneratedBy != nullptr))
			{
				UE_LOG(LogBlueprint, Error, TEXT("Cannot preload parent blueprint from null ClassGeneratedBy field (for '%s')"), *ClassIt->GetName());
			}
			else if (ClassIt->ClassGeneratedBy->HasAnyFlags(RF_NeedLoad))
			{
				ClassIt->ClassGeneratedBy->GetLinker()->Preload(ClassIt->ClassGeneratedBy);
			}
		}
	}

	if (Ar.UE4Ver() < VER_UE4_FIX_BLUEPRINT_VARIABLE_FLAGS)
	{
		// Actor variables can't have default values (because Blueprint templates are library elements that can 
		// bridge multiple levels and different levels might not have the actor that the default is referencing).
		for (int32 i = 0; i < NewVariables.Num(); ++i)
		{
			FBPVariableDescription& Variable = NewVariables[i];

			const FEdGraphPinType& VarType = Variable.VarType;

			bool bDisableEditOnTemplate = false;
			if (VarType.PinSubCategoryObject.IsValid()) // ignore variables that don't have associated objects
			{
				const UClass* ClassObject = Cast<UClass>(VarType.PinSubCategoryObject.Get());
				// if the object type is an actor...
				if ((ClassObject != NULL) && ClassObject->IsChildOf(AActor::StaticClass()))
				{
					// hide the default value field
					bDisableEditOnTemplate = true;
				}
			}

			if(bDisableEditOnTemplate)
			{
				Variable.PropertyFlags |= CPF_DisableEditOnTemplate;
			}
			else
			{
				Variable.PropertyFlags &= ~CPF_DisableEditOnTemplate;
			}
		}
	}

	if(Ar.IsSaving() && !Ar.IsTransacting())
	{
		// Cache the BP for use
		FFindInBlueprintSearchManager::Get().AddOrUpdateBlueprintSearchMetadata(this);
	}
#endif // WITH_EDITORONLY_DATA
}

#if WITH_EDITOR

bool UBlueprint::RenameGeneratedClasses( const TCHAR* InName, UObject* NewOuter, ERenameFlags Flags )
{
	const bool bRenameGeneratedClasses = !(Flags & REN_SkipGeneratedClasses );

	if(bRenameGeneratedClasses)
	{
		FName SkelClassName, GenClassName;
		GetBlueprintClassNames(GenClassName, SkelClassName, FName(InName));

		UPackage* NewTopLevelObjectOuter = NewOuter ? NewOuter->GetOutermost() : NULL;
		if (GeneratedClass != NULL)
		{
			bool bMovedOK = GeneratedClass->Rename(*GenClassName.ToString(), NewTopLevelObjectOuter, Flags);
			if (!bMovedOK)
			{
				return false;
			}
		}

		// Also move skeleton class, if different from generated class, to new package (again, to create redirector)
		if (SkeletonGeneratedClass != NULL && SkeletonGeneratedClass != GeneratedClass)
		{
			bool bMovedOK = SkeletonGeneratedClass->Rename(*SkelClassName.ToString(), NewTopLevelObjectOuter, Flags);
			if (!bMovedOK)
			{
				return false;
			}
		}
	}
	return true;
}

bool UBlueprint::Rename( const TCHAR* InName, UObject* NewOuter, ERenameFlags Flags )
{
	// Move generated class to the new package, to create redirector
	if ( !RenameGeneratedClasses(InName, NewOuter, Flags) )
	{
		return false;
	}

	bool bSuccess = Super::Rename( InName, NewOuter, Flags );

	// Finally, do a compile 
	if(bSuccess && !(Flags & REN_Test) && !(Flags & REN_DoNotDirty))
	{
		FKismetEditorUtilities::CompileBlueprint(this, false);
	}

	return bSuccess;
}

void UBlueprint::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
	if( !bDuplicatingReadOnly )
	{
		FBlueprintEditorUtils::PostDuplicateBlueprint(this);
	}
}

UClass* UBlueprint::RegenerateClass(UClass* ClassToRegenerate, UObject* PreviousCDO, TArray<UObject*>& ObjLoaded)
{
	return FBlueprintEditorUtils::RegenerateBlueprintClass(this, ClassToRegenerate, PreviousCDO, ObjLoaded);
}

void UBlueprint::RemoveGeneratedClasses()
{
	FBlueprintEditorUtils::RemoveGeneratedClasses(this);
}

void UBlueprint::PostLoad()
{
	Super::PostLoad();

	// Mark the blueprint as in error if there has been a major version bump
	if (BlueprintSystemVersion < UBlueprint::GetCurrentBlueprintSystemVersion())
	{
		Status = BS_Error;
	}

	// Purge any NULL graphs
	FBlueprintEditorUtils::PurgeNullGraphs(this);

	// Remove old AutoConstructionScript graph
	for (int32 i = 0; i < FunctionGraphs.Num(); ++i)
	{
		UEdGraph* FuncGraph = FunctionGraphs[i];
		if ((FuncGraph != NULL) && (FuncGraph->GetName() == TEXT("AutoConstructionScript")))
		{
			UE_LOG(LogBlueprint, Log, TEXT("!!! Removing AutoConstructionScript from %s"), *GetPathName());
			FunctionGraphs.RemoveAt(i);
			break;
		}
	}

	// Remove stale breakpoints
	for (int32 i = 0; i < Breakpoints.Num(); ++i)
	{
		UBreakpoint* Breakpoint = Breakpoints[i];
		if (!Breakpoint || !Breakpoint->GetLocation())
		{
			Breakpoints.RemoveAt(i);
			--i;
		}
	}

	// Make sure we have an SCS and ensure it's transactional
	if( FBlueprintEditorUtils::SupportsConstructionScript(this) )
	{
		if(SimpleConstructionScript == NULL)
		{
			check(NULL != GeneratedClass);
			SimpleConstructionScript = NewObject<USimpleConstructionScript>(GeneratedClass);
			SimpleConstructionScript->SetFlags(RF_Transactional);

			UBlueprintGeneratedClass* BPGClass = Cast<UBlueprintGeneratedClass>(*GeneratedClass);
			if(BPGClass)
			{
				BPGClass->SimpleConstructionScript = SimpleConstructionScript;
			}
		}
		else
		{
			if (!SimpleConstructionScript->HasAnyFlags(RF_Transactional))
			{
				SimpleConstructionScript->SetFlags(RF_Transactional);
			}
		}
	}

	// Make sure the CDO's scene root component is valid
	FBlueprintEditorUtils::UpdateRootComponentReference(this);

	// Make sure all the components are used by this blueprint
	FBlueprintEditorUtils::UpdateComponentTemplates(this);

	// Make sure that all of the parent function calls are valid
	FBlueprintEditorUtils::ConformCallsToParentFunctions(this);

	// Make sure that all of the events this BP implements are valid
	FBlueprintEditorUtils::ConformImplementedEvents(this);

	// Make sure that all of the interfaces this BP implements have all required graphs
	FBlueprintEditorUtils::ConformImplementedInterfaces(this);

	// Update old Anim Blueprints
	FBlueprintEditorUtils::UpdateOutOfDateAnimBlueprints(this);

	// Update old macro blueprints
	if(BPTYPE_MacroLibrary == BlueprintType)
	{
		//macros were moved into a separate array
		MacroGraphs.Append(FunctionGraphs);
		FunctionGraphs.Empty();
	}

	// Update old pure functions to be pure using new system
	FBlueprintEditorUtils::UpdateOldPureFunctions(this);

#if WITH_EDITORONLY_DATA
	// Ensure all the pin watches we have point to something useful
	FBlueprintEditorUtils::UpdateStalePinWatches(this);
#endif // WITH_EDITORONLY_DATA

	{
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(Graphs);
		for (auto It = Graphs.CreateIterator(); It; ++It)
		{
			UEdGraph* const Graph = *It;
			const UEdGraphSchema* Schema = Graph->GetSchema();
			Schema->BackwardCompatibilityNodeConversion(Graph, true);
		}
	}

	FStructureEditorUtils::RemoveInvalidStructureMemberVariableFromBlueprint(this);


#if WITH_EDITOR
	// Do not want to run this code without the editor present nor when running commandlets.
	if(GEditor && GIsEditor && !IsRunningCommandlet())
	{
		// Gathers Find-in-Blueprint data, makes sure that it is fresh and ready, especially if the asset did not have any available.
		FFindInBlueprintSearchManager::Get().AddOrUpdateBlueprintSearchMetadata(this);
	}
#endif
}

void UBlueprint::DebuggingWorldRegistrationHelper(UObject* ObjectProvidingWorld, UObject* ValueToRegister)
{
	if (ObjectProvidingWorld != NULL)
	{
		// Fix up the registration with the world
		UWorld* ObjWorld = NULL;
		UObject* ObjOuter = ObjectProvidingWorld->GetOuter();
		while (ObjOuter != NULL)
		{
			ObjWorld = Cast<UWorld>(ObjOuter);
			if (ObjWorld != NULL)
			{
				break;
			}

			ObjOuter = ObjOuter->GetOuter();
		}

		if (ObjWorld != NULL)
		{
			ObjWorld->NotifyOfBlueprintDebuggingAssociation(this, ValueToRegister);
		}
	}
}

UClass* UBlueprint::GetBlueprintClass() const
{
	return UBlueprintGeneratedClass::StaticClass();
}

void UBlueprint::SetObjectBeingDebugged(UObject* NewObject)
{
	// Unregister the old object
	if (UObject* OldObject = CurrentObjectBeingDebugged.Get())
	{
		if (OldObject == NewObject)
		{
			// Nothing changed
			return;
		}

		DebuggingWorldRegistrationHelper(OldObject, NULL);
	}

	// Note that we allow macro Blueprints to bypass this check
	if ((NewObject != NULL) && !GCompilingBlueprint && BlueprintType != BPTYPE_MacroLibrary)
	{
		// You can only debug instances of this!
		if (!ensure(NewObject->IsA(this->GeneratedClass)))
		{
			NewObject = NULL;
		}
	}

	// Update the current object being debugged
	CurrentObjectBeingDebugged = NewObject;

	// Register the new object
	if (NewObject != NULL)
	{
		DebuggingWorldRegistrationHelper(NewObject, NewObject);
	}
}

void UBlueprint::SetWorldBeingDebugged(UWorld *NewWorld)
{
	CurrentWorldBeingDebugged = NewWorld;
}

void UBlueprint::GetReparentingRules(TSet< const UClass* >& AllowedChildrenOfClasses, TSet< const UClass* >& DisallowedChildrenOfClasses) const
{

}

UObject* UBlueprint::GetObjectBeingDebugged()
{
	UObject* DebugObj = CurrentObjectBeingDebugged.Get();
	if(DebugObj)
	{
		//Check whether the object has been deleted.
		if(DebugObj->HasAnyFlags(RF_PendingKill))
		{
			SetObjectBeingDebugged(NULL);
			DebugObj = NULL;
		}
	}
	return DebugObj;
}

UWorld* UBlueprint::GetWorldBeingDebugged()
{
	UWorld* DebugWorld = CurrentWorldBeingDebugged.Get();
	if (DebugWorld)
	{
		if(DebugWorld->HasAnyFlags(RF_PendingKill))
		{
			SetWorldBeingDebugged(NULL);
			DebugWorld = NULL;
		}
	}

	return DebugWorld;
}

void UBlueprint::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	UClass* GenClass = Cast<UClass>(GeneratedClass);
	if ( GenClass && GenClass->GetDefaultObject() )
	{
		GenClass->GetDefaultObject()->GetAssetRegistryTags(OutTags);
	}

	Super::GetAssetRegistryTags(OutTags);

	FString ParentClassPackageName;
	FString NativeParentClassName;
	if ( ParentClass )
	{
		ParentClassPackageName = ParentClass->GetOutermost()->GetName();

		// Walk up until we find a native class (ie 'while they are BP classes')
		UClass* NativeParentClass = ParentClass;
		while (Cast<UBlueprintGeneratedClass>(NativeParentClass) != nullptr) // can't use IsA on UClass
		{
			NativeParentClass = NativeParentClass->GetSuperClass();
		}
		NativeParentClassName = FString::Printf(TEXT("%s'%s'"), *UClass::StaticClass()->GetName(), *NativeParentClass->GetPathName());
	}
	else
	{
		ParentClassPackageName = TEXT("None");
		NativeParentClassName = TEXT("None");
	}

	//NumReplicatedProperties
	int32 NumReplicatedProperties = 0;
	UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(SkeletonGeneratedClass);
	if (BlueprintClass)
	{
		NumReplicatedProperties = BlueprintClass->NumReplicatedProperties;
	}

	OutTags.Add(FAssetRegistryTag("NumReplicatedProperties", FString::FromInt(NumReplicatedProperties), FAssetRegistryTag::TT_Numerical));
	OutTags.Add(FAssetRegistryTag("ParentClassPackage", ParentClassPackageName, FAssetRegistryTag::TT_Hidden));
	OutTags.Add(FAssetRegistryTag("NativeParentClass", NativeParentClassName, FAssetRegistryTag::TT_Alphabetical));
	OutTags.Add(FAssetRegistryTag(GET_MEMBER_NAME_CHECKED(UBlueprint, BlueprintDescription), BlueprintDescription, FAssetRegistryTag::TT_Hidden));

	uint32 ClassFlagsTagged = 0;
	if (BlueprintClass)
	{
		ClassFlagsTagged = BlueprintClass->GetClassFlags();
	}
	else
	{
		ClassFlagsTagged = GetClass()->GetClassFlags();
	}
	OutTags.Add( FAssetRegistryTag("ClassFlags", FString::FromInt(ClassFlagsTagged), FAssetRegistryTag::TT_Hidden) );

	OutTags.Add( FAssetRegistryTag( "IsDataOnly",
		FBlueprintEditorUtils::IsDataOnlyBlueprint(this) ? TEXT("True") : TEXT("False"),
		FAssetRegistryTag::TT_Alphabetical ) );

	OutTags.Add( FAssetRegistryTag("FiB", FFindInBlueprintSearchManager::Get().QuerySingleBlueprint((UBlueprint*)this, false), FAssetRegistryTag::TT_Hidden) );
}

FString UBlueprint::GetFriendlyName() const
{
	return GetName();
}

bool UBlueprint::AllowsDynamicBinding() const
{
	return FBlueprintEditorUtils::IsActorBased(this);
}

bool UBlueprint::SupportsInputEvents() const
{
	return FBlueprintEditorUtils::IsActorBased(this);
}

struct FBlueprintInnerHelper
{
	template<typename TOBJ, typename TARR>
	static TOBJ* FindObjectByName(TARR& Array, const FName& TimelineName)
	{
		for(int32 i=0; i<Array.Num(); i++)
		{
			TOBJ* Obj = Array[i];
			if((NULL != Obj) && (Obj->GetFName() == TimelineName))
			{
				return Obj;
			}
		}
		return NULL;
	}
};

UActorComponent* UBlueprint::FindTemplateByName(const FName& TemplateName) const
{
	return FBlueprintInnerHelper::FindObjectByName<UActorComponent>(ComponentTemplates, TemplateName);
}

const UTimelineTemplate* UBlueprint::FindTimelineTemplateByVariableName(const FName& TimelineName) const
{
	const FName TimelineTemplateName = *UTimelineTemplate::TimelineVariableNameToTemplateName(TimelineName);
	const UTimelineTemplate* Timeline =  FBlueprintInnerHelper::FindObjectByName<const UTimelineTemplate>(Timelines, TimelineTemplateName);

	// >>> Backwards Compatibility:  VER_UE4_EDITORONLY_BLUEPRINTS
	if(Timeline)
	{
		ensure(Timeline->GetOuter() && Timeline->GetOuter()->IsA(UClass::StaticClass()));
	}
	else
	{
		Timeline = FBlueprintInnerHelper::FindObjectByName<const UTimelineTemplate>(Timelines, TimelineName);
		if(Timeline)
		{
			ensure(Timeline->GetOuter() && Timeline->GetOuter() == this);
		}
	}
	// <<< End Backwards Compatibility

	return Timeline;
}

UTimelineTemplate* UBlueprint::FindTimelineTemplateByVariableName(const FName& TimelineName)
{
	const FName TimelineTemplateName = *UTimelineTemplate::TimelineVariableNameToTemplateName(TimelineName);
	UTimelineTemplate* Timeline = FBlueprintInnerHelper::FindObjectByName<UTimelineTemplate>(Timelines, TimelineTemplateName);
	
	// >>> Backwards Compatibility:  VER_UE4_EDITORONLY_BLUEPRINTS
	if(Timeline)
	{
		ensure(Timeline->GetOuter() && Timeline->GetOuter()->IsA(UClass::StaticClass()));
	}
	else
	{
		Timeline = FBlueprintInnerHelper::FindObjectByName<UTimelineTemplate>(Timelines, TimelineName);
		if(Timeline)
		{
			ensure(Timeline->GetOuter() && Timeline->GetOuter() == this);
		}
	}
	// <<< End Backwards Compatibility

	return Timeline;
}

UBlueprint* UBlueprint::GetBlueprintFromClass(const UClass* InClass)
{
	UBlueprint* BP = NULL;
	if(InClass != NULL)
	{
		BP = Cast<UBlueprint>(InClass->ClassGeneratedBy);
	}
	return BP;
}

bool UBlueprint::ValidateGeneratedClass(const UClass* InClass)
{
	const UBlueprintGeneratedClass* GeneratedClass = Cast<const UBlueprintGeneratedClass>(InClass);
	if (!ensure(GeneratedClass))
	{
		return false;
	}
	const UBlueprint* Blueprint = GetBlueprintFromClass(GeneratedClass);
	if (!ensure(Blueprint))
	{
		return false;
	}

	for (auto CompIt = Blueprint->ComponentTemplates.CreateConstIterator(); CompIt; ++CompIt)
	{
		const UActorComponent* Component = (*CompIt);
		if (!ensure(Component && (Component->GetOuter() == GeneratedClass)))
		{
			return false;
		}
	}
	for (auto CompIt = GeneratedClass->ComponentTemplates.CreateConstIterator(); CompIt; ++CompIt)
	{
		const UActorComponent* Component = (*CompIt);
		if (!ensure(Component && (Component->GetOuter() == GeneratedClass)))
		{
			return false;
		}
	}

	for (auto CompIt = Blueprint->Timelines.CreateConstIterator(); CompIt; ++CompIt)
	{
		const UTimelineTemplate* Template = (*CompIt);
		if (!ensure(Template && (Template->GetOuter() == GeneratedClass)))
		{
			return false;
		}
	}
	for (auto CompIt = GeneratedClass->Timelines.CreateConstIterator(); CompIt; ++CompIt)
	{
		const UTimelineTemplate* Template = (*CompIt);
		if (!ensure(Template && (Template->GetOuter() == GeneratedClass)))
		{
			return false;
		}
	}

	if (const USimpleConstructionScript* SimpleConstructionScript = Blueprint->SimpleConstructionScript)
	{
		if (!ensure(SimpleConstructionScript->GetOuter() == GeneratedClass))
		{
			return false;
		}
	}
	if (const USimpleConstructionScript* SimpleConstructionScript = GeneratedClass->SimpleConstructionScript)
	{
		if (!ensure(SimpleConstructionScript->GetOuter() == GeneratedClass))
		{
			return false;
		}
	}

	if (const UInheritableComponentHandler* InheritableComponentHandler = Blueprint->InheritableComponentHandler)
	{
		if (!ensure(InheritableComponentHandler->GetOuter() == GeneratedClass))
		{
			return false;
		}
	}

	if (const UInheritableComponentHandler* InheritableComponentHandler = GeneratedClass->InheritableComponentHandler)
	{
		if (!ensure(InheritableComponentHandler->GetOuter() == GeneratedClass))
		{
			return false;
		}
	}

	return true;
}

bool UBlueprint::GetBlueprintHierarchyFromClass(const UClass* InClass, TArray<UBlueprint*>& OutBlueprintParents)
{
	OutBlueprintParents.Empty();

	bool bNoErrors = true;
	const UClass* CurrentClass = InClass;
	while( UBlueprint* BP = UBlueprint::GetBlueprintFromClass(CurrentClass) )
	{
		OutBlueprintParents.Add(BP);
		bNoErrors &= (BP->Status != BS_Error);
		CurrentClass = CurrentClass->GetSuperClass();
	}

	return bNoErrors;
}

#endif // WITH_EDITOR

ETimelineSigType UBlueprint::GetTimelineSignatureForFunctionByName(const FName& FunctionName, const FName& ObjectPropertyName)
{
	check(SkeletonGeneratedClass != NULL);
	
	UClass* UseClass = SkeletonGeneratedClass;

	// If an object property was specified, find the class of that property instead
	if(ObjectPropertyName != NAME_None)
	{
		UObjectPropertyBase* ObjProperty = FindField<UObjectPropertyBase>(SkeletonGeneratedClass, ObjectPropertyName);
		if(ObjProperty == NULL)
		{
			UE_LOG(LogBlueprint, Log, TEXT("GetTimelineSignatureForFunction: Object Property '%s' not found."), *ObjectPropertyName.ToString());
			return ETS_InvalidSignature;
		}

		UseClass = ObjProperty->PropertyClass;
	}

	UFunction* Function = FindField<UFunction>(UseClass, FunctionName);
	if(Function == NULL)
	{
		UE_LOG(LogBlueprint, Log, TEXT("GetTimelineSignatureForFunction: Function '%s' not found in class '%s'."), *FunctionName.ToString(), *UseClass->GetName());
		return ETS_InvalidSignature;
	}

	return UTimelineComponent::GetTimelineSignatureForFunction(Function);


	UE_LOG(LogBlueprint, Log, TEXT("GetTimelineSignatureForFunction: No SkeletonGeneratedClass in Blueprint '%s'."), *GetName());
	return ETS_InvalidSignature;
}

FString UBlueprint::GetDesc(void)
{
	FString BPType;
	switch (BlueprintType)
	{
		case BPTYPE_MacroLibrary:
			BPType = TEXT("macros for");
			break;
		case BPTYPE_Const:
			BPType = TEXT("const extends");
			break;
		case BPTYPE_Interface:
			// Always extends interface, so no extraneous information needed
			BPType = TEXT("");
			break;
		default:
			BPType = TEXT("extends");
			break;
	}
	const FString ResultString = FString::Printf(TEXT("%s %s"), *BPType, *ParentClass->GetName());

	return ResultString;
}

struct FDontLoadBlueprintOutsideEditorHelper
{
	bool bDontLoad;

	FDontLoadBlueprintOutsideEditorHelper() : bDontLoad(false)
	{
		GConfig->GetBool(TEXT("EditoronlyBP"), TEXT("bDontLoadBlueprintOutsideEditor"), bDontLoad, GEditorIni);
	}
};

bool UBlueprint::NeedsLoadForClient() const
{
	static const FDontLoadBlueprintOutsideEditorHelper Helper;
	return !Helper.bDontLoad;
}

bool UBlueprint::NeedsLoadForServer() const
{
	static const FDontLoadBlueprintOutsideEditorHelper Helper;
	return !Helper.bDontLoad;
}

bool UBlueprint::NeedsLoadForEditorGame() const
{
	return true;
}

void UBlueprint::TagSubobjects(EObjectFlags NewFlags)
{
	Super::TagSubobjects(NewFlags);

	if (GeneratedClass && !GeneratedClass->HasAnyFlags(GARBAGE_COLLECTION_KEEPFLAGS | RF_RootSet))
	{
		GeneratedClass->SetFlags(NewFlags);
		GeneratedClass->TagSubobjects(NewFlags);
	}

	if (SkeletonGeneratedClass && SkeletonGeneratedClass != GeneratedClass && !SkeletonGeneratedClass->HasAnyFlags(GARBAGE_COLLECTION_KEEPFLAGS | RF_RootSet))
	{
		SkeletonGeneratedClass->SetFlags(NewFlags);
		SkeletonGeneratedClass->TagSubobjects(NewFlags);
	}
}

void UBlueprint::GetAllGraphs(TArray<UEdGraph*>& Graphs) const
{
#if WITH_EDITORONLY_DATA
	for (int32 i = 0; i < FunctionGraphs.Num(); ++i)
	{
		UEdGraph* Graph = FunctionGraphs[i];
		Graphs.Add(Graph);
		Graph->GetAllChildrenGraphs(Graphs);
	}
	for (int32 i = 0; i < MacroGraphs.Num(); ++i)
	{
		UEdGraph* Graph = MacroGraphs[i];
		Graphs.Add(Graph);
		Graph->GetAllChildrenGraphs(Graphs);
	}

	for (int32 i = 0; i < UbergraphPages.Num(); ++i)
	{
		UEdGraph* Graph = UbergraphPages[i];
		Graphs.Add(Graph);
		Graph->GetAllChildrenGraphs(Graphs);
	}

	for (int32 BPIdx=0; BPIdx<ImplementedInterfaces.Num(); BPIdx++)
	{
		const FBPInterfaceDescription& InterfaceDesc = ImplementedInterfaces[BPIdx];
		for (int32 GraphIdx = 0; GraphIdx < InterfaceDesc.Graphs.Num(); GraphIdx++)
		{
			UEdGraph* Graph = InterfaceDesc.Graphs[GraphIdx];
			Graphs.Add(Graph);
			Graph->GetAllChildrenGraphs(Graphs);
		}
	}
#endif // WITH_EDITORONLY_DATA
}

#if WITH_EDITOR
void UBlueprint::Message_Note(const FString& MessageToLog)
{
	if( CurrentMessageLog )
	{
		CurrentMessageLog->Note(*MessageToLog);
	}
	else
	{
		UE_LOG(LogBlueprint, Log, TEXT("[%s] %s"), *GetName(), *MessageToLog);
	}
}

void UBlueprint::Message_Warn(const FString& MessageToLog)
{
	if( CurrentMessageLog )
	{
		CurrentMessageLog->Warning(*MessageToLog);
	}
	else
	{
		UE_LOG(LogBlueprint, Warning, TEXT("[%s] %s"), *GetName(), *MessageToLog);
	}
}

void UBlueprint::Message_Error(const FString& MessageToLog)
{
	if( CurrentMessageLog )
	{
		CurrentMessageLog->Error(*MessageToLog);
	}
	else
	{
		UE_LOG(LogBlueprint, Error, TEXT("[%s] %s"), *GetName(), *MessageToLog);
	}
}

bool UBlueprint::ChangeOwnerOfTemplates()
{
	struct FUniqueNewNameHelper
	{
	private:
		FString NewName;
		bool isValid;
	public:
		FUniqueNewNameHelper(const FString& Name, UObject* Outer) : isValid(false)
		{
			const uint32 Hash = FCrc::StrCrc32<TCHAR>(*Name);
			NewName = FString::Printf(TEXT("%s__%08X"), *Name, Hash);
			isValid = IsUniqueObjectName(FName(*NewName), Outer);
			if (!isValid)
			{
				check(Outer);
				UE_LOG(LogBlueprint, Warning, TEXT("ChangeOwnerOfTemplates: Cannot generate a deterministic new name. Old name: %s New outer: %s"), *Name, *Outer->GetName());
			}
		}

		const TCHAR* Get() const
		{
			return isValid ? *NewName : NULL;
		}
	};

	UBlueprintGeneratedClass* BPGClass = Cast<UBlueprintGeneratedClass>(*GeneratedClass);
	bool bIsStillStale = false;
	if (BPGClass)
	{
		check(!bIsRegeneratingOnLoad);

		// >>> Backwards Compatibility:  VER_UE4_EDITORONLY_BLUEPRINTS
		bool bMigratedOwner = false;
		TSet<class UCurveBase*> Curves;
		for( auto CompIt = ComponentTemplates.CreateIterator(); CompIt; ++CompIt )
		{
			UActorComponent* Component = (*CompIt);
			check(Component);
			if(Component->GetOuter() == this)
			{
				const bool bRenamed = Component->Rename(*Component->GetName(), BPGClass, REN_ForceNoResetLoaders|REN_DoNotDirty);
				ensure(bRenamed);
				bIsStillStale |= !bRenamed;
				bMigratedOwner = true;
			}
			if (auto TimelineComponent = Cast<UTimelineComponent>(Component))
			{
				TimelineComponent->GetAllCurves(Curves);
			}
		}

		for( auto CompIt = Timelines.CreateIterator(); CompIt; ++CompIt )
		{
			UTimelineTemplate* Template = (*CompIt);
			check(Template);
			if(Template->GetOuter() == this)
			{
				const FString OldTemplateName = Template->GetName();
				ensure(!OldTemplateName.EndsWith(TEXT("_Template")));
				const bool bRenamed = Template->Rename(*UTimelineTemplate::TimelineVariableNameToTemplateName(Template->GetFName()), BPGClass, REN_ForceNoResetLoaders|REN_DoNotDirty);
				ensure(bRenamed);
				bIsStillStale |= !bRenamed;
				ensure(OldTemplateName == UTimelineTemplate::TimelineTemplateNameToVariableName(Template->GetFName()));
				bMigratedOwner = true;
			}
			Template->GetAllCurves(Curves);
		}
		for (auto Curve : Curves)
		{
			if (Curve && (Curve->GetOuter() == this))
			{
				const bool bRenamed = Curve->Rename(FUniqueNewNameHelper(Curve->GetName(), BPGClass).Get(), BPGClass, REN_ForceNoResetLoaders | REN_DoNotDirty);
				ensure(bRenamed);
				bIsStillStale |= !bRenamed;
			}
		}

		if(USimpleConstructionScript* SCS = SimpleConstructionScript)
		{
			if(SCS->GetOuter() == this)
			{
				const bool bRenamed = SCS->Rename(FUniqueNewNameHelper(SCS->GetName(), BPGClass).Get(), BPGClass, REN_ForceNoResetLoaders | REN_DoNotDirty);
				ensure(bRenamed);
				bIsStillStale |= !bRenamed;
				bMigratedOwner = true;
			}

			TArray<USCS_Node*> SCSNodes = SCS->GetAllNodes();
			for (auto SCSNode : SCSNodes)
			{
				UActorComponent* Component = SCSNode ? SCSNode->ComponentTemplate : NULL;
				if (Component && Component->GetOuter() == this)
				{
					const bool bRenamed = Component->Rename(FUniqueNewNameHelper(Component->GetName(), BPGClass).Get(), BPGClass, REN_ForceNoResetLoaders | REN_DoNotDirty);
					ensure(bRenamed);
					bIsStillStale |= !bRenamed;
					bMigratedOwner = true;
				}
			}
		}

		if (bMigratedOwner)
		{
			if( !HasAnyFlags( RF_Transient ))
			{
				// alert the user that blueprints gave been migrated and require re-saving to enable them to locate and fix them without nagging them.
				FMessageLog("BlueprintLog").Warning( FText::Format( NSLOCTEXT( "Blueprint", "MigrationWarning", "Blueprint {0} has been migrated and requires re-saving to avoid import errors" ), FText::FromString( *GetName() )));

				if( GetDefault<UEditorLoadingSavingSettings>()->bDirtyMigratedBlueprints )
				{
					UPackage* BPPackage = GetOutermost();

					if( BPPackage )
					{
						BPPackage->SetDirtyFlag( true );
					}
				}
			}

			BPGClass->ComponentTemplates = ComponentTemplates;
			BPGClass->Timelines = Timelines;
			BPGClass->SimpleConstructionScript = SimpleConstructionScript;
		}
		// <<< End Backwards Compatibility
	}
	else
	{
		UE_LOG(LogBlueprint, Log, TEXT("ChangeOwnerOfTemplates: No BlueprintGeneratedClass in %s"), *GetName());
	}
	return !bIsStillStale;
}

void UBlueprint::PostLoadSubobjects(FObjectInstancingGraph* OuterInstanceGraph)
{
	Super::PostLoadSubobjects(OuterInstanceGraph);
	ChangeOwnerOfTemplates();

	ConformNativeComponents(this);
}

bool UBlueprint::Modify(bool bAlwaysMarkDirty)
{
	bCachedDependenciesUpToDate = false;
	return Super::Modify(bAlwaysMarkDirty);
}

UInheritableComponentHandler* UBlueprint::GetInheritableComponentHandler(bool bCreateIfNecessary)
{
	static const FBoolConfigValueHelper EnableInheritableComponents(TEXT("Kismet"), TEXT("bEnableInheritableComponents"), GEngineIni);
	if (!EnableInheritableComponents)
	{
		return nullptr;
	}

	if (!InheritableComponentHandler && bCreateIfNecessary)
	{
		UBlueprintGeneratedClass* BPGC = CastChecked<UBlueprintGeneratedClass>(GeneratedClass);
		ensure(!BPGC->InheritableComponentHandler);
		InheritableComponentHandler = BPGC->GetInheritableComponentHandler(true);
	}
	return InheritableComponentHandler;
}

#endif

#if WITH_EDITOR

FName UBlueprint::GetFunctionNameFromClassByGuid(const UClass* InClass, const FGuid FunctionGuid)
{
	return FBlueprintEditorUtils::GetFunctionNameFromClassByGuid(InClass, FunctionGuid);
}

bool UBlueprint::GetFunctionGuidFromClassByFieldName(const UClass* InClass, const FName FunctionName, FGuid& FunctionGuid)
{
	return FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(InClass, FunctionName, FunctionGuid);
}

#endif //WITH_EDITOR