// Copyright The Believer Company. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "OtelPlatformTime.h"

#include "Otel.h"

#include "AnalyticsEventAttribute.h"
#include "Editor.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "EngineUtils.h"

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

// Main reason to keep the class in the CPP is to avoid private implementation dependencies (e.g. Otel) leaking
// out into the header
struct FOtelEditorAnalytics
{
	TOptional<uint64> LaunchPieSpanId;
	FTimerHandle LaunchPieTimer;

	TOptional<uint64> MapLoadSpanId;
	FString MapLoadName;
	FTimerHandle MapLoadTimer;
	FOtelTimestamp MapLoadStartTime;

	TSharedPtr<FOtelCounter> NumPieLaunches;

	bool bShouldSendInitialEditorStartSpan = true;

	FOtelEditorAnalytics()
	{
		FOtelModule& Otel = FOtelModule::Get();
		FOtelMeter Meter = Otel.GetMeter(TEXT("editor_stats"));
		NumPieLaunches = Meter.CreateCounter(EOtelInstrumentType::Int64, TEXT("editor_stats_pie_launches"));
	}

	// PIE start time hooks

	void LaunchPieEndedTick()
	{
		TArray<UWorld*, TInlineAllocator<4>> ClientWorlds;
		bool bDidFindServerWorld = false;
		{
			const TIndirectArray<FWorldContext>& WorldContexts = GEngine->GetWorldContexts();
			for (const FWorldContext& Context : WorldContexts)
			{
				if (Context.WorldType == EWorldType::PIE)
				{
					// We measure the total time to start PIE by the client since it has more
					// asset loading and streaming to do, as well as player control state updates
					UWorld* World = Context.World();
					if (World->GetNetMode() == NM_DedicatedServer)
					{
						bDidFindServerWorld = true;
					}
					else
					{
						ClientWorlds.Add(World);
					}
				}
			}
		}

		int32 ControllersPlayingCount = 0;
		for (UWorld* World : ClientWorlds)
		{
			for (auto Iter = World->GetPlayerControllerIterator(); Iter; ++Iter)
			{
				if (APlayerController* PC = Iter->Get())
				{
					if (PC->IsInState(NAME_Playing))
					{
						++ControllersPlayingCount;
					}
				}
			}
		}

		if (ControllersPlayingCount == ClientWorlds.Num())
		{
			FOtelModule& Otel = FOtelModule::Get();
			FOtelScopedSpan ScopedSpan = Otel.Unpin(*LaunchPieSpanId);
			FOtelSpan Span = ScopedSpan.Inner();

			if (ClientWorlds.Num() > 0)
			{
				if (UWorld* World = ClientWorlds[0])
				{
					FString MapName = ParseMapName(World);
					Span.AddAttribute(FAnalyticsEventAttribute(TEXT("Map"), MoveTemp(MapName)));
				}
			}

			Span.AddAttribute(FAnalyticsEventAttribute(TEXT("IsHeavyPIE"), !bDidFindServerWorld));

			GEditor->GetTimerManager()->ClearTimer(LaunchPieTimer);
			LaunchPieSpanId.Reset();
		}
	}

	void OnPreBeginPIE(const bool bIsSimulating)
	{
		NumPieLaunches->Add(1ull, {});

		FOtelModule& Otel = FOtelModule::Get();
		FOtelScopedSpan ScopedSpan = OTEL_SPAN(TEXT("LaunchPie"));
		LaunchPieSpanId = Otel.Pin(ScopedSpan);
	}

	void OnCancelPIE()
	{
		if (LaunchPieSpanId.IsSet())
		{
			FOtelModule& Otel = FOtelModule::Get();
			FOtelScopedSpan ScopedSpan = Otel.Unpin(*LaunchPieSpanId);
			FOtelSpan Span = ScopedSpan.Inner();
			Span.AddAttribute(FAnalyticsEventAttribute(TEXT("Canceled"), true));

			LaunchPieSpanId.Reset();
			GEditor->GetTimerManager()->ClearTimer(LaunchPieTimer);
		}
	}

	void OnEndPIE(const bool bIsSimulating)
	{
		// If PIE ends before the span has been ended, just cancel
		OnCancelPIE();
	}

	void OnPostPIEStarted(const bool bIsSimulating)
	{
		ensure(LaunchPieTimer.IsValid() == false);

		auto TimerDelegate = FTimerDelegate::CreateRaw(this, &FOtelEditorAnalytics::LaunchPieEndedTick);
		const float PollRate = 1.0f / 30.0f;
		const bool bLoop = true;
		GEditor->GetTimerManager()->SetTimer(LaunchPieTimer, TimerDelegate, PollRate, bLoop);
	}

	// Map load time hooks

	void MapLoadedTick()
	{
		check(MapLoadSpanId.IsSet());

		UWorld* LoadedWorld = nullptr;

		const TIndirectArray<FWorldContext>& WorldContexts = GEngine->GetWorldContexts();
		for (const FWorldContext& WorldContext : WorldContexts)
		{
			if (WorldContext.WorldType == EWorldType::Editor)
			{
				UWorld* World = WorldContext.World();
				FString WorldName = World->GetOutermost()->GetName();
				if (WorldName == MapLoadName)
				{
					LoadedWorld = World;
					break;
				}
			}
		}

		bool bIsLoadComplete = false;
		if (LoadedWorld)
		{
			if (ULevelInstanceSubsystem* LISubsystem = LoadedWorld->GetSubsystem<ULevelInstanceSubsystem>())
			{
				bool bAllLoaded = true;
				for (TActorIterator<ALevelInstance> It(LoadedWorld); It; ++It)
				{
					if (ALevelInstance* LI = *It)
					{
						if (LISubsystem->IsLoaded(*It) == false)
						{
							bAllLoaded = false;
							break;
						}
					}
				}

				bIsLoadComplete = bAllLoaded;
			}
		}
		else
		{
			// somehow the map load was canceled, just finish it off
			bIsLoadComplete = true;
		}

		if (bIsLoadComplete)
		{
			GEditor->GetTimerManager()->ClearTimer(MapLoadTimer);

			FOtelModule& Otel = FOtelModule::Get();
			Otel.Unpin(*MapLoadSpanId);

			if (bShouldSendInitialEditorStartSpan)
			{
				bShouldSendInitialEditorStartSpan = false;

				const double UptimeSeconds = BVPlatformTime::ProcessUptimeSeconds();
				if (UptimeSeconds > 0.0)
				{
					const int64 UptimeNanoseconds = static_cast<int64>(UptimeSeconds * 1e9);

					FOtelTimestamp Timestamp = FOtelTimestamp::Now();
					Timestamp.System -= UptimeNanoseconds;
					Timestamp.Steady -= UptimeNanoseconds;

					FOtelSpan LaunchSpan = Otel.GetTracer().StartSpanOpts(TEXT("LaunchEditor"), TEXT(__FILE__), __LINE__, nullptr, {}, &Timestamp);
				}
			}
		}
	}

	void OnMapLoad(const FString& Filename, FCanLoadMap& OutCanLoadMap)
	{
		if (MapLoadTimer.IsValid())
		{
			GEditor->GetTimerManager()->ClearTimer(MapLoadTimer);
		}

		// FEditorFileUtils::LoadMap() executes this delegate, but can then return if there's any
		// problem with the map load, and doesn't call another delegate to notify this happened.
		// So starting the span in this function could lead to a dangling span in the error case.
		// Instead, we'll record only successful map opens since OnMapOpened will be executed on
		// success and record the actual start time here.
		MapLoadStartTime = FOtelTimestamp::Now();
		MapLoadName = FPackageName::FilenameToLongPackageName(Filename);
	}

	void OnMapOpened(const FString& Filename, bool bAsTemplate)
	{
		FAnalyticsEventAttribute Attributes[] = {
			FAnalyticsEventAttribute(TEXT("Map"), MapLoadName),
			FAnalyticsEventAttribute(TEXT("AsTemplate"), bAsTemplate)
		};

		FOtelModule& Otel = FOtelModule::Get();
		FOtelScopedSpan ScopedSpan = Otel.GetTracer().StartSpanScopedOpts(TEXT("LoadMap"), TEXT(__FILE__), __LINE__, Attributes, &MapLoadStartTime);
		MapLoadSpanId = Otel.Pin(ScopedSpan);

		ensure(MapLoadTimer.IsValid() == false);

		auto TimerDelegate = FTimerDelegate::CreateRaw(this, &FOtelEditorAnalytics::MapLoadedTick);
		const float PollRate = 1.0f / 30.0f;
		const bool bLoop = true;
		GEditor->GetTimerManager()->SetTimer(MapLoadTimer, TimerDelegate, PollRate, bLoop);
	}

	// Startup/shutdown

	void OnModuleStartup()
	{
		FEditorDelegates::PreBeginPIE.AddRaw(this, &FOtelEditorAnalytics::OnPreBeginPIE);
		FEditorDelegates::PostPIEStarted.AddRaw(this, &FOtelEditorAnalytics::OnPostPIEStarted);
		FEditorDelegates::CancelPIE.AddRaw(this, &FOtelEditorAnalytics::OnCancelPIE);
		FEditorDelegates::EndPIE.AddRaw(this, &FOtelEditorAnalytics::OnEndPIE);

		FEditorDelegates::OnMapLoad.AddRaw(this, &FOtelEditorAnalytics::OnMapLoad);
		FEditorDelegates::OnMapOpened.AddRaw(this, &FOtelEditorAnalytics::OnMapOpened);
	}

	void OnModuleShutdown()
	{
		FEditorDelegates::PreBeginPIE.RemoveAll(this);
		FEditorDelegates::CancelPIE.RemoveAll(this);
		FEditorDelegates::PostPIEStarted.RemoveAll(this);
		FEditorDelegates::EndPIE.RemoveAll(this);

		FEditorDelegates::OnMapLoad.RemoveAll(this);
		FEditorDelegates::OnMapOpened.RemoveAll(this);
	}
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// FSEditorAnalytics namespace

static TUniquePtr<FOtelEditorAnalytics> GEditorAnalyticsPtr;

class FOtelEditorModule final : public IModuleInterface, private FNoncopyable
{
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FOtelEditorAnalytics EditorAnalytics;
};

void FOtelEditorModule::StartupModule()
{
	EditorAnalytics.OnModuleStartup();
}

void FOtelEditorModule::ShutdownModule()
{
	EditorAnalytics.OnModuleShutdown();
}

IMPLEMENT_MODULE(FOtelEditorModule, OpenTelemetryEditor)
