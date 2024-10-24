// Copyright The Believer Company. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "OtelFunctionLibrary.generated.h"

/**
 *
 */
UCLASS()
class OPENTELEMETRY_API UOtelFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

	// make a blueprint callable function that takes in a map and calls EmitLog
	UFUNCTION(BlueprintCallable, Category = "Telemetry")
	static void EmitTelemetryEvent(UPARAM(meta = (Categories = "Telemetry")) FGameplayTag Tag);

	UFUNCTION(BlueprintCallable, Category = "Telemetry")
	static void EmitTelemetryEventWithAttributes(UPARAM(meta = (Categories = "Telemetry")) FGameplayTag Tag, const TMap<FString, FString>& Attributes);
};
