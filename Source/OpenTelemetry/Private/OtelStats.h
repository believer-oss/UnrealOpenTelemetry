// Copyright The Believer Company. All Rights Reserved.

#pragma once

#include "Tickable.h"

class FOtelModule;
struct FOtelHistogram;
struct FOtelGauge;

class FOtelStats : public FTickableGameObject
{
public:
	FOtelStats(FOtelModule& InModule);

	// FTickableGameObject
	virtual TStatId GetStatId() const override;
	virtual void Tick(float DeltaTime) override;

private:
	FOtelModule& Module;

	TSharedPtr<FOtelHistogram> HistogramGameMs;
	TSharedPtr<FOtelHistogram> HistogramRenderMs;
	TSharedPtr<FOtelHistogram> HistogramRhiMs;
	TSharedPtr<FOtelHistogram> HistogramGpuMs;
	TSharedPtr<FOtelGauge> GaugeMemory;
	TSharedPtr<FOtelGauge> GaugeMemoryUsedPct;
	TSharedPtr<FOtelGauge> GaugeUObjects;

	TSharedPtr<FOtelHistogram> HistogramNetPingMs;
	TSharedPtr<FOtelHistogram> HistogramNetInBytes;
	TSharedPtr<FOtelHistogram> HistogramNetOutBytes;
	TSharedPtr<FOtelHistogram> HistogramNetInPacketLossPct;
	TSharedPtr<FOtelHistogram> HistogramNetOutPacketLossPct;

	double NetUpdateTimestamp;
};
