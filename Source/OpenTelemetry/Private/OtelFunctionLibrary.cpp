// Copyright The Believer Company. All Rights Reserved.

#include "OtelFunctionLibrary.h"

#include "AnalyticsEventAttribute.h"
#include "Otel.h"
#include "GameplayTagContainer.h"

void UOtelFunctionLibrary::EmitTelemetryEvent(FGameplayTag Tag)
{
	const FString TagString = Tag.ToString();
	OTEL_LOG(*TagString, {});
}

void UOtelFunctionLibrary::EmitTelemetryEventWithAttributes(FGameplayTag Tag, const TMap<FString, FString>& Attributes)
{
	const FString TagString = Tag.ToString();

	TArray<FAnalyticsEventAttribute> EventAttributes;
	EventAttributes.Reserve(Attributes.Num());
	for (const TPair<FString, FString>& Pair : Attributes)
	{
		EventAttributes.Add(FAnalyticsEventAttribute(Pair.Key, Pair.Value));
	}

	// TODO: Get stackframe from blueprint and include as file and line number
	OTEL_LOG(*TagString, EventAttributes);
}
