// Copyright The Believer Company. All Rights Reserved.

#include "OtelStats.h"
#include "Otel.h"

#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "UObject/UObjectArray.h"

static FString ParseMapName(UWorld* World)
{
	check(World);

	FString MapName = World->GetOutermost()->GetName();
	const int32 PrefixIndex = MapName.Find(World->StreamingLevelsPrefix);
	if (PrefixIndex != INDEX_NONE)
	{
		MapName.RemoveAt(PrefixIndex, World->StreamingLevelsPrefix.Len());
	}
	return MapName;
}

FOtelStats::FOtelStats(FOtelModule& InModule)
	: Module(InModule)
	, NetUpdateTimestamp(0.0)
{
	{
		FOtelMeter Meter = Module.GetMeter(TEXT("frame_stats"));

		const double FrameTimingBucketsRaw[] = { 0, 4, 8, 16.6667, 33.3334, 50, 65, 80, 100 };
		const FOtelHistogramBuckets FrameTimingBuckets = FOtelHistogramBuckets::From(FrameTimingBucketsRaw);

		HistogramGameMs = Meter.CreateHistogram(EOtelInstrumentType::Double, TEXT("frame_stats_game_thread"), FrameTimingBuckets, EUnit::Milliseconds);
		HistogramRenderMs = Meter.CreateHistogram(EOtelInstrumentType::Double, TEXT("frame_stats_render_thread"), FrameTimingBuckets, EUnit::Milliseconds);
		HistogramRhiMs = Meter.CreateHistogram(EOtelInstrumentType::Double, TEXT("frame_stats_rhi_thread"), FrameTimingBuckets, EUnit::Milliseconds);
		HistogramGpuMs = Meter.CreateHistogram(EOtelInstrumentType::Double, TEXT("frame_stats_gpu"), FrameTimingBuckets, EUnit::Milliseconds);

		GaugeMemory = Meter.CreateGauge(EOtelInstrumentType::Int64, TEXT("frame_stats_memory"), EUnit::Megabytes);
		GaugeMemoryUsedPct = Meter.CreateGauge(EOtelInstrumentType::Double, TEXT("frame_stats_memory_pct_total"));
		GaugeUObjects = Meter.CreateGauge(EOtelInstrumentType::Int64, TEXT("frame_stats_uobjects"));
	}

	{
		FOtelMeter Meter = Module.GetMeter(TEXT("net_stats"));

		const double PingBucketsRaw[] = { 5, 10, 20, 30, 50, 75, 100, 150, 200 };
		const FOtelHistogramBuckets PingBuckets = FOtelHistogramBuckets::From(PingBucketsRaw);

		HistogramNetPingMs = Meter.CreateHistogram(EOtelInstrumentType::Double, TEXT("net_stats_ping"), PingBuckets, EUnit::Milliseconds);

		const uint64 KB = 1024;
		const uint64 InOutBytesBucketsRaw[] = { 0, KB / 2, KB, KB * 2, KB * 4, KB * 8, KB * 16, KB * 32, KB * 64 };
		const FOtelHistogramBuckets InOutBytesBuckets = FOtelHistogramBuckets::From(InOutBytesBucketsRaw);

		HistogramNetInBytes = Meter.CreateHistogram(EOtelInstrumentType::Int64, TEXT("net_stats_bytes_in"), InOutBytesBuckets, EUnit::Bytes);
		HistogramNetOutBytes = Meter.CreateHistogram(EOtelInstrumentType::Int64, TEXT("net_stats_bytes_out"), InOutBytesBuckets, EUnit::Bytes);

		const double PacketLossPctBucketsRaw[] = { 0, 0.05, 0.1, 0.2, 0.3, 0.5, 0.75, 1 };
		const FOtelHistogramBuckets PacketLossPctBuckets = FOtelHistogramBuckets::From(PacketLossPctBucketsRaw);

		HistogramNetInPacketLossPct = Meter.CreateHistogram(EOtelInstrumentType::Double, TEXT("net_stats_packet_loss_pct_in"), PacketLossPctBuckets);
		HistogramNetOutPacketLossPct = Meter.CreateHistogram(EOtelInstrumentType::Double, TEXT("net_stats_packet_loss_pct_out"), PacketLossPctBuckets);
	}
}

TStatId FOtelStats::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(FOtelStats, STATGROUP_Tickables);
}

void FOtelStats::Tick(float DeltaTime)
{
	TArray<FAnalyticsEventAttribute, TInlineAllocator<4>> Attributes;

	// Try to pick a client world, but fall back to a server world if no client world is available
	const TIndirectArray<FWorldContext>& WorldList = GEngine->GetWorldContexts();
	UWorld* PlayWorld = nullptr;
	APlayerController* LocalPC = nullptr;
	for (const FWorldContext& Context : WorldList)
	{
		Attributes.Reset();

		if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
		{
			if (UWorld* World = Context.World())
			{
				const bool bRemovePrefixString = true;
				FString MapName = ParseMapName(World);
				if (MapName.StartsWith(TEXT("/Game/")))
				{
					PlayWorld = World;
					Attributes.Add(FAnalyticsEventAttribute(TEXT("map"), MapName));

					if (APlayerController* PC = GEngine->GetFirstLocalPlayerController(World))
					{
						LocalPC = PC;
						break;
					}
				}
			}
		}
	}

	// determine and record frame stats
	const float GameThreadMs = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const float RenderThreadMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	const float RhiThreadMs = FPlatformTime::ToMilliseconds(GRHIThreadTime);
	const float GpuMs = FPlatformTime::ToMilliseconds(GGPUFrameTime);

	HistogramGameMs->Record(GameThreadMs, Attributes);
	HistogramRenderMs->Record(RenderThreadMs, Attributes);
	HistogramRhiMs->Record(RhiThreadMs, Attributes);
	HistogramGpuMs->Record(GpuMs, Attributes);

	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	const uint64 UsedMemoryMB = MemStats.UsedPhysical / (1024 * 1024);
	const uint64 TotalMemoryMB = MemStats.AvailablePhysical / (1024 * 1024);
	const double MemoryUsedPct = static_cast<double>(UsedMemoryMB) / static_cast<double>(TotalMemoryMB);

	GaugeMemory->Observe(static_cast<int64>(UsedMemoryMB), Attributes);
	GaugeMemoryUsedPct->Observe(MemoryUsedPct, Attributes);

	const int64 NumUObjects = GUObjectArray.GetObjectArrayNum();
	GaugeUObjects->Observe(NumUObjects, Attributes);

	// determine and record net stats
	if (PlayWorld && LocalPC)
	{
		if (APlayerState* PS = LocalPC->GetPlayerState<APlayerState>())
		{
			double PingMs = PS->GetPingInMilliseconds();
			HistogramNetPingMs->Record(PingMs, Attributes);
		}

		if (UNetConnection* NetConnection = LocalPC->GetNetConnection())
		{
			const double Now = FPlatformTime::Seconds();
			const double TimeSinceLastUpdate = Now - NetUpdateTimestamp;
			if (TimeSinceLastUpdate >= NetConnection->StatPeriod)
			{
				NetUpdateTimestamp = Now;

				const uint64 InBytes = static_cast<uint64>(FMath::Max(0, NetConnection->InBytes));
				const uint64 OutBytes = static_cast<uint64>(FMath::Max(0, NetConnection->OutBytes));
				const double InPacketLossPct = NetConnection->GetInLossPercentage().GetLossPercentage();
				const double OutPacketLossPct = NetConnection->GetOutLossPercentage().GetLossPercentage();

				HistogramNetInBytes->Record(InBytes, Attributes);
				HistogramNetOutBytes->Record(OutBytes, Attributes);
				HistogramNetInPacketLossPct->Record(InPacketLossPct, Attributes);
				HistogramNetOutPacketLossPct->Record(OutPacketLossPct, Attributes);
			}
		}
	}
}
