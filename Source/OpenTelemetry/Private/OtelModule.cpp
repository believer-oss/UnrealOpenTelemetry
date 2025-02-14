// Copyright The Believer Company. All Rights Reserved.

#include "Otel.h"
#include "OtelStats.h"

#include "AnalyticsEventAttribute.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/common/kv_properties.h"
#include "opentelemetry/context/context_value.h"
#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_factory.h"
#include "opentelemetry/logs/provider.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/metrics/sync_instruments.h"
#include "opentelemetry/sdk/common/env_variables.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_factory.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/meter_context_factory.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/view/instrument_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/meter_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/view_factory.h"
#include "opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include "opentelemetry/sdk/resource/resource_detector.h"
#include "opentelemetry/sdk/trace/batch_span_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/trace/semantic_conventions.h"

#ifdef GetEnvironmentVariable
#undef GetEnvironmentVariable // Seems like windows.h is getting dragged in by some otel headers :(
#endif

#include <iostream>

DEFINE_LOG_CATEGORY_STATIC(LogOtel, Log, All);

///////////////////////////////////////////////////////////////////////////////////////////////////
// utilties to handle UE <-> otel communication

class FOtelLogHandler : public otel::sdk::common::internal_log::LogHandler
{
	virtual void Handle(otel::sdk::common::internal_log::LogLevel level,
		const char* file,
		int line,
		const char* msg,
		const otel::sdk::common::AttributeMap& attributes) noexcept override
	{
#if !PLATFORM_APPLE
		TAnsiStringBuilder<1024> Msg;
		Msg.Appendf("%s:%d: %s", file, line, msg);

		switch (level)
		{
			case otel::sdk::common::internal_log::LogLevel::Error:
				UE_LOG(LogOtel, Error, TEXT("%s"), ANSI_TO_TCHAR(Msg.GetData()));
				break;
			case otel::sdk::common::internal_log::LogLevel::Warning:
				UE_LOG(LogOtel, Warning, TEXT("%s"), ANSI_TO_TCHAR(Msg.GetData()));
				break;
			case otel::sdk::common::internal_log::LogLevel::Info:
				UE_LOG(LogOtel, Log, TEXT("%s"), ANSI_TO_TCHAR(Msg.GetData()));
				break;
			case otel::sdk::common::internal_log::LogLevel::Debug:
				UE_LOG(LogOtel, Verbose, TEXT("%s"), ANSI_TO_TCHAR(Msg.GetData()));
				break;
		}
#endif
	}
};

class EventAttributesOtelConverter : public otel::common::KeyValueIterable
{
public:
	EventAttributesOtelConverter(TArrayView<const FAnalyticsEventAttribute> InAttributes)
		: Attributes(InAttributes)
		, File(nullptr)
		, LineNumber(-1)
	{
	}

	EventAttributesOtelConverter(TArrayView<const FAnalyticsEventAttribute> InAttributes, const TCHAR* InFile, int32 InLineNumber)
		: Attributes(InAttributes)
		, File(InFile)
		, LineNumber(InLineNumber)
	{
	}

	bool ForEachKeyValue(otel::nostd::function_ref<bool(otel::nostd::string_view, otel::common::AttributeValue)> Callback) const noexcept
	{
		for (const FAnalyticsEventAttribute& Attribute : Attributes)
		{
			auto Name = StringCast<ANSICHAR>(*Attribute.GetName());
			auto Value = StringCast<ANSICHAR>(*Attribute.GetValue());
			if (Callback(Name.Get(), otel::common::AttributeValue(Value.Get())) == false)
			{
				return false;
			}
		}

		if (File != nullptr)
		{
			check(LineNumber > 0);
			auto FileAnsi = StringCast<ANSICHAR>(File);
			Callback(otel::trace::SemanticConventions::kCodeFilepath, FileAnsi.Get());
			Callback(otel::trace::SemanticConventions::kCodeLineno, LineNumber);
		}

		return true;
	}

	size_t size() const noexcept
	{
		return static_cast<size_t>(Attributes.Num());
	}

private:
	TArrayView<const FAnalyticsEventAttribute> Attributes;
	const TCHAR* File;
	int32 LineNumber;
};

template <typename T>
void ParseKeyValuePairs(const FString& String, T* Container)
{
	auto StringAnsi = StringCast<ANSICHAR>(*String);

	otel::common::KeyValueStringTokenizer Tokenizer{ StringAnsi.Get() };
	otel::nostd::string_view Key;
	otel::nostd::string_view Value;
	bool IsValid = true;

	while (Tokenizer.next(IsValid, Key, Value))
	{
		if (IsValid)
		{
			Container->emplace(std::string(Key), std::string(Value));
		}
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelTimestampBridge

struct FOtelTimestampBridge
{
	otel::common::SystemTimestamp System;
	otel::common::SteadyTimestamp Steady;
};

FOtelTimestamp FOtelTimestamp::Now()
{
	FOtelTimestamp Timestamp;
	Timestamp.System = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	Timestamp.Steady = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	return Timestamp;
}

FOtelTimestampBridge ToBridgeTimestamp(const FOtelTimestamp& Timestamp)
{
	auto System = std::chrono::system_clock::time_point{
		std::chrono::duration_cast<std::chrono::system_clock::duration>(
			std::chrono::nanoseconds{ Timestamp.System })
	};
	auto Steady = std::chrono::steady_clock::time_point{
		std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::nanoseconds{ Timestamp.Steady })
	};

	FOtelTimestampBridge OtelTimestamp;
	OtelTimestamp.System = otel::common::SystemTimestamp(System);
	OtelTimestamp.Steady = otel::common::SteadyTimestamp(Steady);
	return OtelTimestamp;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelSpan

FOtelSpan::FOtelSpan()
	: TracerName(NAME_None)
	, OtelSpan(nullptr)
{
}

FOtelSpan::FOtelSpan(FName InTracerName, std::shared_ptr<otel::trace::Span> InOtelSpan)
	: TracerName(InTracerName)
	, OtelSpan(InOtelSpan)
{
}

void FOtelSpan::SetStatus(EOtelStatus Status)
{
	if (OtelSpan)
	{
		const otel::trace::StatusCode ToOtelStatus[] = {
			otel::trace::StatusCode::kOk,
			otel::trace::StatusCode::kError,
		};
		check(static_cast<int32>(Status) < GetNum(ToOtelStatus));
		const otel::trace::StatusCode OtelStatus = ToOtelStatus[static_cast<int32>(Status)];

		OtelSpan->SetStatus(OtelStatus);
	}
}

void FOtelSpan::AddAttribute(const FAnalyticsEventAttribute& Attribute)
{
	if (OtelSpan)
	{
		auto NameAnsi = StringCast<ANSICHAR>(*Attribute.GetName());
		auto ValueAnsi = StringCast<ANSICHAR>(*Attribute.GetValue());
		OtelSpan->SetAttribute(NameAnsi.Get(), ValueAnsi.Get());
	}
}

void FOtelSpan::AddAttributes(TArrayView<const FAnalyticsEventAttribute> Attributes)
{
	if (OtelSpan)
	{
		EventAttributesOtelConverter AttributeConverter(Attributes);
		AttributeConverter.ForEachKeyValue([this](std::string_view Name, otel::common::AttributeValue Value)
			{
				OtelSpan->SetAttribute(Name, Value);
				return true;
			});
	}
}

void FOtelSpan::AddEvent(const TCHAR* Name, TArrayView<const FAnalyticsEventAttribute> Attributes)
{
	check(Name);

	if (OtelSpan)
	{
		EventAttributesOtelConverter AttributeConverter(Attributes);

		auto NameAnsi = StringCast<ANSICHAR>(Name);
		OtelSpan->AddEvent(NameAnsi.Get(), AttributeConverter);
	}
}

FString FOtelSpan::TraceId() const
{
	if (OtelSpan)
	{
		auto IdOpaque = OtelSpan->GetContext().trace_id().Id();
		return FBase64::Encode(IdOpaque.data(), IdOpaque.size(), EBase64Mode::UrlSafe);
	}

	return FString();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelScopedSpan and FOtelScopedSpanImpl

struct FOtelScopedSpanImpl : public FNoncopyable
{
	FOtelScopedSpanImpl(const FOtelSpan& InSpan);

	FOtelSpan Span;
	int32 RefCount;
};

FOtelScopedSpanImpl::FOtelScopedSpanImpl(const FOtelSpan& InSpan)
	: Span(InSpan)
	, RefCount(0)
{
}

FOtelScopedSpan::FOtelScopedSpan(const FOtelSpan& Span)
{
	if (Span.OtelSpan)
	{
		if (FOtelModule* Module = FOtelModule::TryGet())
		{
			FOtelLockedData<FOtelModule::FTracerToScopeStack> TracerToScopeStack = Module->LockedTracerToScopeStack.Lock();
			TArray<TSharedPtr<FOtelScopedSpanImpl>>* ScopesPtr = TracerToScopeStack->Find(Span.TracerName);
			if (ScopesPtr)
			{
				TArray<TSharedPtr<FOtelScopedSpanImpl>>& Scopes = *ScopesPtr;
				for (int32 i = Scopes.Num() - 1; i >= 0; --i)
				{
					check(Scopes[i]);
					if (Span.OtelSpan == Scopes[i]->Span.OtelSpan)
					{
						Scope = Scopes[i];
						++Scope->RefCount;
						return;
					}
				}
			}
		}
	}
}

FOtelScopedSpan::FOtelScopedSpan(const FOtelScopedSpan& ScopedSpan)
{
	Scope = ScopedSpan.Scope;
	if (Scope && Scope->Span.OtelSpan)
	{
		++Scope->RefCount;
	}
}

FOtelScopedSpan& FOtelScopedSpan::operator=(const FOtelScopedSpan& ScopedSpan)
{
	Scope = ScopedSpan.Scope;
	if (Scope && Scope->Span.OtelSpan)
	{
		++Scope->RefCount;
	}
	return *this;
}

FOtelScopedSpan::FOtelScopedSpan(FOtelScopedSpan&& ScopedSpan)
{
	Scope = MoveTemp(ScopedSpan.Scope);
}

FOtelScopedSpan::~FOtelScopedSpan()
{
	if (Scope)
	{
		--Scope->RefCount;
		if (Scope->RefCount <= 0)
		{
			// Destroy scoped span
			if (FOtelModule* Module = FOtelModule::TryGet())
			{
				FOtelLockedData<FOtelModule::FTracerToScopeStack> TracerToScopeStack = Module->LockedTracerToScopeStack.Lock();
				TArray<TSharedPtr<FOtelScopedSpanImpl>>* ScopesPtr = TracerToScopeStack->Find(Scope->Span.TracerName);
				if (ensure(ScopesPtr))
				{
					TArray<TSharedPtr<FOtelScopedSpanImpl>>& Scopes = *ScopesPtr;
					const int32 Index = Scopes.IndexOfByPredicate([this](const TSharedPtr<FOtelScopedSpanImpl>& Impl)
						{
							return Impl == Scope;
						});

					// End this and all child Scopes
					if (Index != INDEX_NONE)
					{
						for (int i = Scopes.Num() - 1; i >= Index; --i)
						{
							TSharedPtr<FOtelScopedSpanImpl>& Impl = Scopes[i];
							check(Impl);
							if (ensure(Impl->Span.OtelSpan))
							{
								Impl->Span.OtelSpan->End();
							}
						}
						Scopes.SetNum(Index);
					}
				}
			}
		}
	}
}

FOtelSpan FOtelScopedSpan::Inner() const
{
	if (Scope)
	{
		return Scope->Span;
	}
	return FOtelSpan();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelTracer

FOtelTracer::FOtelTracer(FName Name, std::shared_ptr<otel::trace::Tracer> InOtelTracer)
	: TracerName(Name)
	, OtelTracer(InOtelTracer)
{
}

FOtelSpan FOtelTracer::StartSpan(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber)
{
	return StartSpanOpts(SpanName, File, LineNumber, nullptr, {}, nullptr);
}

FOtelSpan FOtelTracer::StartSpanOpts(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber, const FOtelSpan* OptionalParentSpan, TArrayView<const FAnalyticsEventAttribute> Attributes, FOtelTimestamp* OptionalTimestamp)
{
	check(SpanName);

	if (OtelTracer)
	{
		EventAttributesOtelConverter AttributeConverter(Attributes, File, LineNumber);

		otel::trace::StartSpanOptions StartOptions;
		if (OptionalParentSpan && OptionalParentSpan->OtelSpan)
		{
			StartOptions.parent = OptionalParentSpan->OtelSpan->GetContext();
		}

		if (OptionalTimestamp)
		{
			const FOtelTimestampBridge BridgeTimestamp = ToBridgeTimestamp(*OptionalTimestamp);
			StartOptions.start_system_time = BridgeTimestamp.System;
			StartOptions.start_steady_time = BridgeTimestamp.Steady;
		}

		auto SpanNameAnsi = StringCast<ANSICHAR>(SpanName);
		auto OtelSpan = OtelTracer->StartSpan(SpanNameAnsi.Get(), AttributeConverter, StartOptions);
		auto Span = FOtelSpan(TracerName, OtelSpan);
#if WITH_EDITOR
		Span.SpanName = SpanName;
#endif
		return Span;
	}

	return FOtelSpan();
}

FOtelScopedSpan FOtelTracer::StartSpanScoped(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber)
{
	return StartSpanScopedOpts(SpanName, File, LineNumber, {}, nullptr);
}

FOtelScopedSpan FOtelTracer::StartSpanScopedOpts(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber, TArrayView<const FAnalyticsEventAttribute> Attributes, FOtelTimestamp* OptionalTimestamp)
{
	check(SpanName);

	FOtelModule& Module = FOtelModule::Get();
	FOtelLockedData<FOtelModule::FTracerToScopeStack> TracerToScopeStack = Module.LockedTracerToScopeStack.Lock();
	TArray<TSharedPtr<FOtelScopedSpanImpl>>& Scopes = TracerToScopeStack->FindOrAdd(TracerName);

	FOtelSpan ParentSpan;
	if (Scopes.Num() > 0)
	{
		if (ensure(Scopes.Last()))
		{
			ParentSpan = Scopes.Last()->Span;
		}
	}

	FOtelSpan Span = StartSpanOpts(SpanName, File, LineNumber, &ParentSpan, Attributes, OptionalTimestamp);

	TSharedPtr<FOtelScopedSpanImpl> Scope = MakeShared<FOtelScopedSpanImpl>(Span);
	Scopes.Add(Scope);

	FOtelScopedSpan ScopedSpan = FOtelScopedSpan(Span);
	return ScopedSpan;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelMeter and counter/gauge/histogram implementations

struct FOtelCounterUInt64 : public FOtelCounter
{
	virtual void Add(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		EventAttributesOtelConverter AttributeIterable = EventAttributesOtelConverter(Attributes);
		OtelCounter->Add(static_cast<uint64_t>(Value), AttributeIterable, otel::context::Context());
	}

	virtual void Add(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		UE_LOG(LogOtel, Warning, TEXT("Adding double value on Counter that is configured for uint64 - value will be dropped."));
	}

	std::unique_ptr<otel::metrics::Counter<uint64_t>> OtelCounter;
};

struct FOtelCounterDouble : public FOtelCounter
{
	virtual void Add(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		UE_LOG(LogOtel, Warning, TEXT("Adding uint64 value on Counter that is configured for doubles - value will be dropped."));
	}

	virtual void Add(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		if (ensure(Value >= 0.0))
		{
			EventAttributesOtelConverter AttributeIterable = EventAttributesOtelConverter(Attributes);
			OtelCounter->Add(Value, AttributeIterable, otel::context::Context());
		}
	}

	std::unique_ptr<otel::metrics::Counter<double>> OtelCounter;
};

struct FOtelCounterNoop : public FOtelCounter
{
	FOtelCounterNoop(EOtelInstrumentType InType)
		: Type(InType)
	{
	}

	virtual void Add(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		ensureMsgf(Type == EOtelInstrumentType::Int64, TEXT("Adding double value on Counter that is configured for uint64 - value will be dropped."));
	}

	virtual void Add(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		ensure(Value >= 0.0);
		ensureMsgf(Type == EOtelInstrumentType::Double, TEXT("Adding uint64 value on Counter that is configured for doubles - value will be dropped."));
	}

	EOtelInstrumentType Type;
};

template <typename T>
struct TOtelGauge : public FOtelGauge
{
	inline void ObserveInternal(T Value, TArrayView<FAnalyticsEventAttribute> Attributes)
	{
		if (LastObserved.IsValid() == false)
		{
			LastObserved = MakeUnique<std::atomic<T>>();
		}
		*LastObserved = Value;
		LastAttributes = Attributes;
	}

	virtual void Observe(int64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		if constexpr (std::is_same_v<int64_t, T>)
		{
			ObserveInternal(Value, Attributes);
		}
		else
		{
			UE_LOG(LogOtel, Warning, TEXT("Adding double value on Gauge that is configured for uint64 - value will be dropped."));
		}
	}

	virtual void Observe(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		if constexpr (std::is_same_v<double, T>)
		{
			ObserveInternal(Value, Attributes);
		}
		else
		{
			UE_LOG(LogOtel, Warning, TEXT("Adding double value on Gauge that is configured for uint64 - value will be dropped."));
		}
	}

	void SetGauge(std::shared_ptr<otel::metrics::ObservableInstrument> InOtelGauge)
	{
		check(OtelGauge == nullptr);
		OtelGauge = InOtelGauge;
		OtelGauge->AddCallback(&OtelCallback, this);
	}

	static void OtelCallback(otel::metrics::ObserverResult Result, void* ThisGauge)
	{
		TOtelGauge<T>* This = static_cast<TOtelGauge<T>*>(ThisGauge);
		if (const std::atomic<T>* Value = This->LastObserved.Get())
		{
			EventAttributesOtelConverter AttributeIterable = EventAttributesOtelConverter(This->LastAttributes);

			auto TypedResult = std::get<otel::nostd::shared_ptr<otel::metrics::ObserverResultT<T>>>(Result);
			TypedResult->Observe(Value->load(), AttributeIterable);
		}
	}

	std::shared_ptr<otel::metrics::ObservableInstrument> OtelGauge;
	TUniquePtr<std::atomic<T>> LastObserved;
	TArray<FAnalyticsEventAttribute> LastAttributes;
};

struct FOtelGaugeNoop : public FOtelGauge
{
	FOtelGaugeNoop(EOtelInstrumentType InType)
		: Type(InType)
	{
	}

	virtual void Observe(int64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		ensureMsgf(Type == EOtelInstrumentType::Int64, TEXT("Adding int64 value on Gauge that is configured for double - value will be dropped."));
	}

	virtual void Observe(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		ensureMsgf(Type == EOtelInstrumentType::Double, TEXT("Adding double value on Gauge that is configured for int64 - value will be dropped."));
	}

	EOtelInstrumentType Type;
};

struct FOtelHistogramUInt64 : public FOtelHistogram
{
	virtual void Record(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		EventAttributesOtelConverter AttributeIterable = EventAttributesOtelConverter(Attributes);
		OtelHistogram->Record(static_cast<uint64_t>(Value), AttributeIterable, otel::context::Context());
	}

	virtual void Record(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		UE_LOG(LogOtel, Warning, TEXT("Recording double value on histogram that is configured for uint64 - value will be dropped."));
	}

	std::unique_ptr<otel::metrics::Histogram<uint64_t>> OtelHistogram;
};

struct FOtelHistogramDouble : public FOtelHistogram
{
	virtual void Record(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		UE_LOG(LogOtel, Warning, TEXT("Recording uint64 value on histogram that is configured for doubles - value will be dropped."));
	}

	virtual void Record(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		if (ensure(Value >= 0.0))
		{
			EventAttributesOtelConverter AttributeIterable = EventAttributesOtelConverter(Attributes);
			OtelHistogram->Record(Value, AttributeIterable, otel::context::Context());
		}
	}

	std::unique_ptr<otel::metrics::Histogram<double>> OtelHistogram;
};

struct FOtelHistogramNoop : public FOtelHistogram
{
	FOtelHistogramNoop(EOtelInstrumentType InType)
		: Type(InType)
	{
	}

	virtual void Record(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		ensureMsgf(Type == EOtelInstrumentType::Int64, TEXT("Recording double value on histogram that is configured for uint64 - value will be dropped."));
	}

	virtual void Record(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) override
	{
		ensure(Value >= 0.0);
		ensureMsgf(Type == EOtelInstrumentType::Double, TEXT("Recording uint64 value on histogram that is configured for doubles - value will be dropped."));
	}

	EOtelInstrumentType Type;
};

FOtelHistogramBuckets FOtelHistogramBuckets::From(TArrayView<const uint64> InBuckets)
{
	// clang treats uint64 and uint64_t as different types, so to ensure type compatibility we have to do some casting
	FOtelHistogramBuckets Buckets;
	Buckets.UInt64Buckets = TArrayView<const uint64_t>(reinterpret_cast<const uint64_t*>(InBuckets.GetData()), InBuckets.Num());
	return Buckets;
}

FOtelHistogramBuckets FOtelHistogramBuckets::From(TArrayView<const double> InBuckets)
{
	FOtelHistogramBuckets Buckets;
	Buckets.DoubleBuckets = InBuckets;
	return Buckets;
}

FOtelMeter::FOtelMeter(const TCHAR* InName, FOtelModule& InModule, std::shared_ptr<otel::metrics::Meter> InOtelMeter)
	: Module(InModule)
	, Name(InName)
	, OtelMeter(InOtelMeter)
{
}

TSharedPtr<FOtelCounter> FOtelMeter::CreateCounter(EOtelInstrumentType MeterType, const TCHAR* CounterName, EUnit UnitType)
{
	auto CounterNameAnsi = StringCast<ANSICHAR>(CounterName);

	const TCHAR* UnitTypeStr = (UnitType != EUnit::Unspecified) ? FUnitConversion::GetUnitDisplayString(UnitType) : TEXT("");
	auto UnitTypeStrAnsi = StringCast<ANSICHAR>(UnitTypeStr);

	TSharedPtr<FOtelCounter> Counter;
	if (OtelMeter)
	{
		if (MeterType == EOtelInstrumentType::Int64)
		{
			TSharedPtr<FOtelCounterUInt64> UInt64Counter = MakeShared<FOtelCounterUInt64>();
			UInt64Counter->OtelCounter = OtelMeter->CreateUInt64Counter(CounterNameAnsi.Get(), "", UnitTypeStrAnsi.Get());
			Counter = UInt64Counter;
		}
		else
		{
			TSharedPtr<FOtelCounterDouble> DoubleCounter = MakeShared<FOtelCounterDouble>();
			DoubleCounter->OtelCounter = OtelMeter->CreateDoubleCounter(CounterNameAnsi.Get(), "", UnitTypeStrAnsi.Get());
			Counter = DoubleCounter;
		}
	}
	else
	{
		Counter = MakeShared<FOtelCounterNoop>(MeterType);
	}

	return Counter;
}

TSharedPtr<FOtelGauge> FOtelMeter::CreateGauge(EOtelInstrumentType MeterType, const TCHAR* GaugeName, EUnit UnitType)
{
	auto GaugeNameAnsi = StringCast<ANSICHAR>(GaugeName);

	const TCHAR* UnitTypeStr = (UnitType != EUnit::Unspecified) ? FUnitConversion::GetUnitDisplayString(UnitType) : TEXT("");
	auto UnitTypeStrAnsi = StringCast<ANSICHAR>(UnitTypeStr);

	TSharedPtr<FOtelGauge> Gauge;
	if (OtelMeter)
	{
		if (MeterType == EOtelInstrumentType::Int64)
		{
			TSharedPtr<TOtelGauge<int64_t>> UInt64Gauge = MakeShared<TOtelGauge<int64_t>>();
			UInt64Gauge->SetGauge(OtelMeter->CreateInt64ObservableGauge(GaugeNameAnsi.Get(), "", UnitTypeStrAnsi.Get()));
			Gauge = UInt64Gauge;
		}
		else
		{
			TSharedPtr<TOtelGauge<double>> DoubleGauge = MakeShared<TOtelGauge<double>>();
			DoubleGauge->SetGauge(OtelMeter->CreateDoubleObservableGauge(GaugeNameAnsi.Get(), "", UnitTypeStrAnsi.Get()));
			Gauge = DoubleGauge;
		}
	}
	else
	{
		Gauge = MakeShared<FOtelGaugeNoop>(MeterType);
	}

	return Gauge;
}

TSharedPtr<FOtelHistogram> FOtelMeter::CreateHistogram(EOtelInstrumentType MeterType, const TCHAR* HistogramName, FOtelHistogramBuckets Buckets, EUnit UnitType)
{
#if !PLATFORM_APPLE
	check(HistogramName);
	auto HistogramNameAnsi = StringCast<ANSICHAR>(HistogramName);

	const TCHAR* UnitTypeStr = (UnitType != EUnit::Unspecified) ? FUnitConversion::GetUnitDisplayString(UnitType) : TEXT("");
	auto UnitTypeStrAnsi = StringCast<ANSICHAR>(UnitTypeStr);

	TSharedPtr<FOtelHistogram> Histogram;
	if (OtelMeter)
	{
		// OTEL aggregation buckets must be registered _before_ histogram creation
		std::shared_ptr<otel::sdk::metrics::HistogramAggregationConfig> OtelHistogramAggregation;
		if (MeterType == EOtelInstrumentType::Int64)
		{
			check(Buckets.DoubleBuckets.IsEmpty());
			if (Buckets.UInt64Buckets.Num() > 0)
			{
				OtelHistogramAggregation = std::make_unique<otel::sdk::metrics::HistogramAggregationConfig>();
				OtelHistogramAggregation->boundaries_.insert(OtelHistogramAggregation->boundaries_.begin(), Buckets.UInt64Buckets.begin(), Buckets.UInt64Buckets.end());
			}
		}
		else
		{
			check(Buckets.UInt64Buckets.IsEmpty());
			if (Buckets.DoubleBuckets.Num() > 0)
			{
				OtelHistogramAggregation = std::make_unique<otel::sdk::metrics::HistogramAggregationConfig>();
				OtelHistogramAggregation->boundaries_.insert(OtelHistogramAggregation->boundaries_.begin(), Buckets.DoubleBuckets.begin(), Buckets.DoubleBuckets.end());
			}
		}

		if (OtelHistogramAggregation)
		{
			auto InstrumentSelector = otel::sdk::metrics::InstrumentSelectorFactory::Create(
				otel::sdk::metrics::InstrumentType::kHistogram,
				HistogramNameAnsi.Get(),
				UnitTypeStrAnsi.Get());

			const auto MeterNameAnsi = StringCast<ANSICHAR>(*Name);
			const auto VersionAnsi = StringCast<ANSICHAR>(*Module.Config.Metric.Version);
			const auto SchemaAnsi = StringCast<ANSICHAR>(*Module.Config.Metric.SchemaUrl);
			auto MeterSelector = otel::sdk::metrics::MeterSelectorFactory::Create(MeterNameAnsi.Get(), VersionAnsi.Get(), SchemaAnsi.Get());

			auto View = otel::sdk::metrics::ViewFactory::Create(
				HistogramNameAnsi.Get(),
				"",
				UnitTypeStrAnsi.Get(),
				otel::sdk::metrics::AggregationType::kHistogram,
				OtelHistogramAggregation);

			Module.MeterProvider->AddView(MoveTemp(InstrumentSelector), MoveTemp(MeterSelector), MoveTemp(View));
		}

		if (MeterType == EOtelInstrumentType::Int64)
		{
			TSharedPtr<FOtelHistogramUInt64> UInt64Histogram = MakeShared<FOtelHistogramUInt64>();
			UInt64Histogram->OtelHistogram = OtelMeter->CreateUInt64Histogram(HistogramNameAnsi.Get(), "", UnitTypeStrAnsi.Get());
			Histogram = UInt64Histogram;
		}
		else
		{
			TSharedPtr<FOtelHistogramDouble> DoubleHistogram = MakeShared<FOtelHistogramDouble>();
			DoubleHistogram->OtelHistogram = OtelMeter->CreateDoubleHistogram(HistogramNameAnsi.Get(), "", UnitTypeStrAnsi.Get());
			Histogram = DoubleHistogram;
		}
	}
	else
	{
		Histogram = MakeShared<FOtelHistogramNoop>(MeterType);
	}

	return Histogram;
#else
	return nullptr;
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelConfig

FOtelConfig FOtelConfig::LoadFromIni()
{
	FOtelConfig Config;

	FString EngineConfigDir = FPaths::EngineConfigDir();
	FString ProjectConfigDir = FPaths::ProjectConfigDir();
	bool bForceReloadFromDisk = false;
	bool bWriteDestIni = false;
	bool bIsBaseIniName = false;

	const FString IniFilePath = FPaths::ProjectConfigDir() / TEXT("DefaultOtel.ini");

	FConfigFile ConfigFile;
	if (GConfig->LoadLocalIniFile(ConfigFile, *IniFilePath, false) == false)
	{
		UE_LOG(LogOtel, Warning, TEXT("DefaultOtel.ini not found in Config folder. All events will be dropped."));
		return Config;
	}

	const TCHAR* TargetName = nullptr;
	if (IsRunningDedicatedServer())
	{
		TargetName = TEXT("Server");
	}
	else if (IsRunningGame() || IsRunningClientOnly())
	{
		TargetName = TEXT("Client");
	}
	else
	{
		TargetName = TEXT("Editor");
	}

	// traces

	const FString TraceSectionName = FString::Printf(TEXT("%s.Trace"), TargetName);
	ConfigFile.GetString(*TraceSectionName, TEXT("EndpointUrl"), Config.Trace.EndpointUrl);
	ConfigFile.GetString(*TraceSectionName, TEXT("Headers"), Config.Trace.Headers);
	ConfigFile.GetString(*TraceSectionName, TEXT("ResourceAttributes"), Config.Trace.ResourceAttributes);
	ConfigFile.GetString(*TraceSectionName, TEXT("DefaultTracerName"), Config.Trace.DefaultTracerName);
	ConfigFile.GetBool(*TraceSectionName, TEXT("bUseSsl"), Config.Trace.bUseSsl);

	if (Config.Trace.EndpointUrl.IsEmpty())
	{
		UE_LOG(LogOtel, Display, TEXT("No EndpointUrl found for DefaultOtel.ini section %s. All traces will be dropped."), *TraceSectionName);
	}

	if (Config.Trace.DefaultTracerName.IsEmpty())
	{
		Config.Trace.DefaultTracerName = TEXT("Otel");
	}

	// metrics

	const FString MetricSectionName = FString::Printf(TEXT("%s.Metric"), TargetName);
	ConfigFile.GetString(*MetricSectionName, TEXT("EndpointUrl"), Config.Metric.EndpointUrl);
	ConfigFile.GetString(*MetricSectionName, TEXT("Headers"), Config.Metric.Headers);
	ConfigFile.GetString(*MetricSectionName, TEXT("ResourceAttributes"), Config.Metric.ResourceAttributes);
	ConfigFile.GetString(*MetricSectionName, TEXT("DefaultMeterName"), Config.Metric.DefaultMeterName);
	ConfigFile.GetString(*MetricSectionName, TEXT("Version"), Config.Metric.Version);
	ConfigFile.GetString(*MetricSectionName, TEXT("SchemaUrl"), Config.Metric.SchemaUrl);
	ConfigFile.GetInt(*MetricSectionName, TEXT("ExportIntervalMs"), Config.Metric.ExportIntervalMs);
	ConfigFile.GetInt(*MetricSectionName, TEXT("ExportTimeoutMs"), Config.Metric.ExportTimeoutMs);
	ConfigFile.GetBool(*MetricSectionName, TEXT("bUseSsl"), Config.Metric.bUseSsl);

	if (Config.Metric.EndpointUrl.IsEmpty())
	{
		UE_LOG(LogOtel, Display, TEXT("No EndpointUrl found for DefaultOtel.ini section %s. All metrics will be dropped."), *MetricSectionName);
	}

	if (Config.Metric.ExportIntervalMs < 0)
	{
		Config.Metric.ExportIntervalMs = 60000;
		UE_LOG(LogOtel, Error, TEXT("ExportIntervalMs in DefaultOtel.ini section %s is not allowed to be negative. Falling back to %dms."),
			*MetricSectionName,
			Config.Metric.ExportIntervalMs);
	}

	if (Config.Metric.ExportTimeoutMs < 0)
	{
		Config.Metric.ExportTimeoutMs = 30000;
		UE_LOG(LogOtel, Error, TEXT("ExportTimeoutMs in DefaultOtel.ini section %s is not allowed to be negative. Falling back to %dms."),
			*MetricSectionName,
			Config.Metric.ExportTimeoutMs);
	}

	// logs

	const FString LogSectionName = FString::Printf(TEXT("%s.Log"), TargetName);
	ConfigFile.GetString(*LogSectionName, TEXT("EndpointUrl"), Config.Log.EndpointUrl);
	ConfigFile.GetString(*LogSectionName, TEXT("Headers"), Config.Log.Headers);
	ConfigFile.GetString(*LogSectionName, TEXT("ResourceAttributes"), Config.Log.ResourceAttributes);
	ConfigFile.GetString(*LogSectionName, TEXT("AppName"), Config.Log.AppName);
	ConfigFile.GetBool(*LogSectionName, TEXT("bUseSsl"), Config.Log.bUseSsl);

	if (Config.Log.EndpointUrl.IsEmpty())
	{
		UE_LOG(LogOtel, Display, TEXT("No EndpointUrl found for DefaultOtel.ini section %s. All logs will be dropped."), *LogSectionName);
	}

	return Config;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelScopedLogHook

FOtelScopedLogHook::FOtelScopedLogHook(FLogCategoryBase* InCategory, FName InTracerName, ELogVerbosity::Type LogVerbosity)
	: Category(InCategory)
	, TracerName(InTracerName)
{
#if !NO_LOGGING
	FOtelModule::Get().SetEnableEventsForLogChannel(Category, TracerName, LogVerbosity);
#endif
}

FOtelScopedLogHook::~FOtelScopedLogHook()
{
#if !NO_LOGGING
	FOtelModule::Get().SetEnableEventsForLogChannel(Category, TracerName, ELogVerbosity::NoLogging);
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelOutputDevice

void FOtelOutputDevice::SetCategoryEnabled(const FName& LogCategory, const FName TracerName, ELogVerbosity::Type LogVerbosity)
{
	const bool bIsEnabled = LogVerbosity != ELogVerbosity::NoLogging;
	const bool bAllCategories = LogCategory == NAME_None;

	FOtelLockedData<FLogRoutingData> LockedRouting = TracerLogging.Lock();

	FTracerRouting& Routing = LockedRouting->FindOrAdd(TracerName);

	if (bAllCategories)
	{
		Routing.AllCategoryVerbosity = LogVerbosity;
	}
	else
	{
		if (LogVerbosity != ELogVerbosity::NoLogging)
		{
			Routing.CategoryVerbosity.FindOrAdd(LogCategory) = LogVerbosity;
		}
		else
		{
			Routing.CategoryVerbosity.Remove(LogCategory);
		}
	}

	if (Routing.CategoryVerbosity.IsEmpty() && Routing.AllCategoryVerbosity == ELogVerbosity::NoLogging)
	{
		LockedRouting->Remove(TracerName);
	}
}

void FOtelOutputDevice::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (V == nullptr || *V == 0 || Verbosity == ELogVerbosity::NoLogging)
	{
		return;
	}

	FOtelLockedData<FLogRoutingData> LockedRouting = TracerLogging.Lock();
	for (const TPair<FName, FTracerRouting>& Pair : *LockedRouting)
	{
		const FName& TracerName = Pair.Key;
		const FTracerRouting& Routing = Pair.Value;

		const ELogVerbosity::Type* CategoryVerbosityPtr = Routing.CategoryVerbosity.Find(Category);
		const ELogVerbosity::Type CategoryVerbosity = CategoryVerbosityPtr ? *CategoryVerbosityPtr : ELogVerbosity::NoLogging;

		bool bIsAllowed = false;
		bIsAllowed |= (Routing.AllCategoryVerbosity != ELogVerbosity::NoLogging) && (Verbosity <= Routing.AllCategoryVerbosity);
		bIsAllowed |= (CategoryVerbosity != ELogVerbosity::NoLogging) && (Verbosity <= CategoryVerbosity);

		if (bIsAllowed)
		{
			TStringBuilder<1024> StringBuilder;
			StringBuilder.Append(*Category.ToString());
			StringBuilder.Append(TEXT(": "));
			StringBuilder.Append(ToString(Verbosity));
			StringBuilder.Append(TEXT(": "));
			StringBuilder.Append(V);

			if (Verbosity > ELogVerbosity::Warning)
			{
				FOtelModule::Get().EmitLog(StringBuilder.ToString(), {}, TEXT(__FILE__), __LINE__, TracerName);
			}
			else
			{
				FOtelModule::Get().EmitLog(StringBuilder.ToString(), {}, TEXT(__FILE__), __LINE__, TracerName, EOtelStatus::Error);
			}
		}
	}
}

bool FOtelOutputDevice::IsMemoryOnly() const
{
	return true;
}

bool FOtelOutputDevice::CanBeUsedOnAnyThread() const
{
	return true;
}

bool FOtelOutputDevice::CanBeUsedOnMultipleThreads() const
{
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FOtelModule

IMPLEMENT_MODULE(FOtelModule, OpenTelemetry);

void FOtelModule::StartupModule()
{
#if !PLATFORM_APPLE
	Config = FOtelConfig::LoadFromIni();

	otel::sdk::common::internal_log::GlobalLogHandler::SetLogHandler(std::make_shared<FOtelLogHandler>());

	// If someone is debugging this process, all the timings will probably be off, so don't send any events
	// to avoid polluting the data.
	bool bUseRealBackend = true;
	if (FPlatformMisc::IsDebuggerPresent())
	{
		const FString ForceOn = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_OTEL_FORCE_ON_WITH_DEBUGGER"));
		if (ForceOn.IsEmpty() || ForceOn == TEXT("0"))
		{
			UE_LOG(LogOtel, Display, TEXT("Debugger attached - disabling telemetry to avoid polluting timing data. Set UE_OTEL_FORCE_ON_WITH_DEBUGGER=1 if you want to force it to on."));
			bUseRealBackend = false;
		}
	}

	TOptional<bool> bUseSslOverride;
	{
		bool bEnvSetting = false;
		if (otel::sdk::common::GetBoolEnvironmentVariable("OTEL_EXPORTER_OTLP_TRACES_INSECURE", bEnvSetting))
		{
			bUseSslOverride = !bEnvSetting;
		}
		else if (otel::sdk::common::GetBoolEnvironmentVariable("OTEL_EXPORTER_OTLP_INSECURE", bEnvSetting))
		{
			bUseSslOverride = !bEnvSetting;
		}
		else if (otel::sdk::common::GetBoolEnvironmentVariable("OTEL_EXPORTER_OTLP_TRACES_SSL_ENABLE", bEnvSetting))
		{
			bUseSslOverride = bEnvSetting;
		}
		else if (otel::sdk::common::GetBoolEnvironmentVariable("OTEL_EXPORTER_OTLP_SSL_ENABLE", bEnvSetting))
		{
			bUseSslOverride = bEnvSetting;
		}
	}

	const FString CertPath = FPaths::EngineContentDir() / TEXT("Certificates/ThirdParty/cacert.pem");
	const auto CertPathAnsi = StringCast<ANSICHAR>(*CertPath);

	otel::sdk::resource::ResourceAttributes SharedResAttributes;
	{
		opentelemetry::sdk::resource::OTELResourceDetector Detector;
		opentelemetry::sdk::resource::Resource OTELResource = Detector.Detect();
		SharedResAttributes = OTELResource.GetAttributes();

		this->SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		FString EngineVersion = FEngineVersion::Current().ToString();

		SharedResAttributes.emplace(otel::trace::SemanticConventions::kSessionId, StringCast<ANSICHAR>(*this->SessionId).Get());
		SharedResAttributes.emplace("service.engine.version", StringCast<ANSICHAR>(*EngineVersion).Get());

		// avoid Personally Identifying Information in builds that go to external users
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		FString PlatformVersion;
		FString PlatformSubVersion;
		FPlatformMisc::GetOSVersions(PlatformVersion, PlatformSubVersion);

		FString CPUVendor = FPlatformMisc::GetCPUVendor();
		FString CPUBrand = FPlatformMisc::GetCPUBrand();
		FString PrimaryGPUBrand = FPlatformMisc::GetPrimaryGPUBrand();
		const TCHAR* CmdLine = FCommandLine::Get();

		SharedResAttributes.emplace("user.name", StringCast<ANSICHAR>(FPlatformProcess::UserName()).Get());
		SharedResAttributes.emplace("user.machine", StringCast<ANSICHAR>(FPlatformProcess::UserName()).Get());
		SharedResAttributes.emplace("user.computer_name", StringCast<ANSICHAR>(FPlatformProcess::ComputerName()).Get());
		SharedResAttributes.emplace("user.platform", StringCast<ANSICHAR>(*PlatformVersion).Get());
		SharedResAttributes.emplace("user.platform_version", StringCast<ANSICHAR>(*PlatformSubVersion).Get());
		SharedResAttributes.emplace("user.cpu_vendor", StringCast<ANSICHAR>(*CPUVendor).Get());
		SharedResAttributes.emplace("user.cpu_brand", StringCast<ANSICHAR>(*CPUBrand).Get());
		SharedResAttributes.emplace("user.gpu", StringCast<ANSICHAR>(*PrimaryGPUBrand).Get());

		SharedResAttributes.emplace("process.command_line", StringCast<ANSICHAR>(CmdLine).Get());
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	}

	if (Config.Trace.EndpointUrl.IsEmpty() == false && bUseRealBackend)
	{
		otel::sdk::resource::ResourceAttributes ResourceAttributes = SharedResAttributes;
		ParseKeyValuePairs(Config.Trace.ResourceAttributes, &ResourceAttributes);
		const auto Resource = otel::sdk::resource::Resource::Create(ResourceAttributes);

		const bool bUseSsl = bUseSslOverride.IsSet() ? bUseSslOverride.GetValue() : Config.Trace.bUseSsl;

		otel::exporter::otlp::OtlpGrpcExporterOptions ExporterOpts;
		ExporterOpts.use_ssl_credentials = bUseSsl;
		ExporterOpts.ssl_credentials_cacert_path = CertPathAnsi.Get();
		ExporterOpts.endpoint = StringCast<ANSICHAR>(*Config.Trace.EndpointUrl).Get();
		ParseKeyValuePairs(Config.Trace.Headers, &ExporterOpts.metadata);

		auto Exporter = otel::exporter::otlp::OtlpGrpcExporterFactory::Create(ExporterOpts);

		otel::sdk::trace::BatchSpanProcessorOptions ProcessorOpts;
		auto Processor = otel::sdk::trace::BatchSpanProcessorFactory::Create(MoveTemp(Exporter), ProcessorOpts);

		std::shared_ptr<otel::trace::TracerProvider> Provider = otel::sdk::trace::TracerProviderFactory::Create(MoveTemp(Processor), Resource);
		otel::trace::Provider::SetTracerProvider(Provider);
	}

	if (Config.Metric.EndpointUrl.IsEmpty() == false && bUseRealBackend)
	{
		otel::sdk::resource::ResourceAttributes ResourceAttributes = SharedResAttributes;
		ParseKeyValuePairs(Config.Metric.ResourceAttributes, &ResourceAttributes);
		const auto Resource = otel::sdk::resource::Resource::Create(ResourceAttributes);

		const bool bUseSsl = bUseSslOverride.IsSet() ? bUseSslOverride.GetValue() : Config.Metric.bUseSsl;

		// TODO make PreferredAggregationTemporality configurable via config
		otel::exporter::otlp::OtlpGrpcMetricExporterOptions ExporterOpts;
		ExporterOpts.use_ssl_credentials = bUseSsl;
		ExporterOpts.ssl_credentials_cacert_path = CertPathAnsi.Get();
		ExporterOpts.endpoint = StringCast<ANSICHAR>(*Config.Metric.EndpointUrl).Get();
		ExporterOpts.aggregation_temporality = otel::exporter::otlp::PreferredAggregationTemporality::kDelta;
		ParseKeyValuePairs(Config.Metric.Headers, &ExporterOpts.metadata);

		auto Exporter = otel::exporter::otlp::OtlpGrpcMetricExporterFactory::Create(ExporterOpts);

		auto Views = otel::sdk::metrics::ViewRegistryFactory::Create();
		auto Context = otel::sdk::metrics::MeterContextFactory::Create(MoveTemp(Views), Resource);
		MeterProvider = opentelemetry::sdk::metrics::MeterProviderFactory::Create(MoveTemp(Context));

		otel::sdk::metrics::PeriodicExportingMetricReaderOptions ReaderOptions;
		ReaderOptions.export_interval_millis = std::chrono::milliseconds(Config.Metric.ExportIntervalMs);
		ReaderOptions.export_timeout_millis = std::chrono::milliseconds(Config.Metric.ExportTimeoutMs);

		auto Reader = otel::sdk::metrics::PeriodicExportingMetricReaderFactory::Create(MoveTemp(Exporter), ReaderOptions);
		MeterProvider->AddMetricReader(MoveTemp(Reader));

		otel::metrics::Provider::SetMeterProvider(MeterProvider);
	}

	if (Config.Log.EndpointUrl.IsEmpty() == false && bUseRealBackend)
	{
		otel::sdk::resource::ResourceAttributes ResourceAttributes = SharedResAttributes;
		ParseKeyValuePairs(Config.Log.ResourceAttributes, &ResourceAttributes);
		const auto Resource = otel::sdk::resource::Resource::Create(ResourceAttributes);

		const bool bUseSsl = bUseSslOverride.IsSet() ? bUseSslOverride.GetValue() : Config.Log.bUseSsl;

		opentelemetry::exporter::otlp::OtlpGrpcLogRecordExporterOptions ExporterOpts;
		ExporterOpts.use_ssl_credentials = bUseSsl;
		ExporterOpts.ssl_credentials_cacert_path = CertPathAnsi.Get();
		ExporterOpts.endpoint = StringCast<ANSICHAR>(*Config.Log.EndpointUrl).Get();
		ParseKeyValuePairs(Config.Log.Headers, &ExporterOpts.metadata);

		std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> Exporter = otel::exporter::otlp::OtlpGrpcLogRecordExporterFactory::Create(ExporterOpts);

		// TODO make processor opts configurable via .ini
		otel::sdk::logs::BatchLogRecordProcessorOptions ProcessorOpts;
		std::unique_ptr<otel::sdk::logs::LogRecordProcessor> Processor = otel::sdk::logs::BatchLogRecordProcessorFactory::Create(MoveTemp(Exporter), ProcessorOpts);

		LoggerProvider = otel::sdk::logs::LoggerProviderFactory::Create(MoveTemp(Processor), Resource);
		otel::logs::Provider::SetLoggerProvider(LoggerProvider);
	}
#endif // !PLATFORM_APPLE

	FrameStats = new FOtelStats(*this);
}

void FOtelModule::ShutdownModule()
{
#if !PLATFORM_APPLE
	delete FrameStats;
	FrameStats = nullptr;
	MeterProvider = nullptr;
	LoggerProvider = nullptr;

	const double FlushTimeoutSeconds = 1.5;
	ForceFlush(FlushTimeoutSeconds);

	FOtelLockedData<FOtelModule::FTracerToScopeStack> TracerToScopeStack = LockedTracerToScopeStack.Lock();
	for (TPair<FName, TArray<TSharedPtr<FOtelScopedSpanImpl>>>& Pair : *TracerToScopeStack)
	{
		for (int32 i = Pair.Value.Num() - 1; i >= 0; --i)
		{
			if (Pair.Value[i])
			{
				FOtelSpan& Span = Pair.Value[i]->Span;
				if (Span.OtelSpan)
				{
					Span.OtelSpan->End();
				}
			}
		}
	}
	TracerToScopeStack->Reset();

	std::shared_ptr<otel::trace::TracerProvider> TracerProviderNone;
	otel::trace::Provider::SetTracerProvider(TracerProviderNone);

	std::shared_ptr<otel::sdk::metrics::MeterProvider> MeterProviderNone;
	otel::metrics::Provider::SetMeterProvider(MeterProviderNone);

	std::shared_ptr<otel::sdk::logs::LoggerProvider> LoggerProviderNone;
	otel::logs::Provider::SetLoggerProvider(LoggerProviderNone);

	otel::sdk::common::internal_log::GlobalLogHandler::SetLogHandler(std::shared_ptr<FOtelLogHandler>());

	if (OutputDevice.IsValid() && GLog)
	{
		GLog->RemoveOutputDevice(OutputDevice.Get());
		OutputDevice.Reset();
	}
#endif
}

FOtelModule& FOtelModule::Get()
{
	return FModuleManager::Get().LoadModuleChecked<FOtelModule>("OpenTelemetry");
}

FOtelModule* FOtelModule::TryGet()
{
	return FModuleManager::Get().GetModulePtr<FOtelModule>("OpenTelemetry");
}

void FOtelModule::SetEnableEventsForLogChannel(const FLogCategoryBase* LogCategory, FName TracerName, ELogVerbosity::Type LogVerbosity)
{
	if (OutputDevice.IsValid() == false && GLog)
	{
		OutputDevice = MakeUnique<FOtelOutputDevice>();
		GLog->AddOutputDevice(OutputDevice.Get());
	}

	if (OutputDevice)
	{
		FName CategoryName = LogCategory ? LogCategory->GetCategoryName() : NAME_None;
		OutputDevice->SetCategoryEnabled(CategoryName, TracerName, LogVerbosity);
	}
}

FOtelTracer FOtelModule::GetTracer(FName TracerName)
{
	auto FinalTracerNameAnsi = StringCast<ANSICHAR>((TracerName == NAME_None) ? *Config.Trace.DefaultTracerName : *TracerName.ToString());
	std::shared_ptr<otel::trace::Tracer> OtelTracer = otel::trace::Provider::GetTracerProvider()->GetTracer(FinalTracerNameAnsi.Get());
	return FOtelTracer(TracerName, OtelTracer);
}

uint64 FOtelModule::Pin(FOtelScopedSpan ScopedSpan)
{
	uint64 SpanId = 0; // 0 is the invalid span ID according to trace_id.h

	auto Span = ScopedSpan.Inner();
	if (Span.OtelSpan)
	{
		auto SpanIdRaw = Span.OtelSpan->GetContext().span_id().Id(); // std::span
		static_assert(sizeof(uint64) == otel::trace::SpanId::kSize);
		memcpy(&SpanId, SpanIdRaw.data(), sizeof(SpanId));
		PinnedSpans.Add(SpanId, ScopedSpan);
	}

	return SpanId;
}

FOtelScopedSpan* FOtelModule::GetPinnedSpan(uint64 SpanId)
{
	return PinnedSpans.Find(SpanId);
}

FOtelScopedSpan FOtelModule::Unpin(uint64 SpanId)
{
	FOtelScopedSpan ScopedSpan;
	PinnedSpans.RemoveAndCopyValue(SpanId, ScopedSpan);
	return ScopedSpan;
}

void FOtelModule::EmitLog(const TCHAR* Message, TArrayView<const FAnalyticsEventAttribute> Attributes, const TCHAR* File, int32 LineNumber, FName TracerName, TOptional<EOtelStatus> Status)
{
#if !PLATFORM_APPLE
	check(Message);

	otel::trace::SpanId SpanId;
	otel::trace::TraceId TraceId;
	otel::trace::TraceFlags TraceFlags;

	FOtelModule& Module = FOtelModule::Get();
	FOtelLockedData<FOtelModule::FTracerToScopeStack> TracerToScopeStack = Module.LockedTracerToScopeStack.Lock();
	TArray<TSharedPtr<FOtelScopedSpanImpl>>& Scopes = TracerToScopeStack->FindOrAdd(TracerName);
	if (Scopes.Num() > 0 && Scopes.Last())
	{
		FOtelSpan& Span = Scopes.Last()->Span;
		Span.AddEvent(Message, Attributes);
		if (Status.IsSet() && *Status != EOtelStatus::Ok)
		{
			Span.SetStatus(Status.GetValue());
		}

		if (Span.OtelSpan)
		{
			const otel::trace::SpanContext Context = Span.OtelSpan->GetContext();
			SpanId = Context.span_id();
			TraceId = Context.trace_id();
			TraceFlags = Context.trace_flags();
		}
	}

	if (LoggerProvider)
	{
		const auto Severity = (Status.IsSet() && *Status == EOtelStatus::Error) ? otel::logs::Severity::kError : otel::logs::Severity::kInfo;

		auto TracerNameAnsi = StringCast<ANSICHAR>(*TracerName.ToString());

		std::shared_ptr<otel::logs::Logger> Logger = LoggerProvider->GetLogger(StringCast<ANSICHAR>(*Config.Log.AppName).Get(), TracerNameAnsi.Get());
		std::unique_ptr<otel::logs::LogRecord> Record = Logger->CreateLogRecord();
		Record->SetSeverity(Severity);
		Record->SetBody(StringCast<ANSICHAR>(Message).Get());

		EventAttributesOtelConverter AttributeConverter(Attributes);
		AttributeConverter.ForEachKeyValue([&Record](std::string_view Name, otel::common::AttributeValue Value)
			{
				Record->SetAttribute(Name, Value);
				return true;
			});

		if (SpanId.IsValid())
		{
			Record->SetSpanId(SpanId);
			Record->SetTraceId(TraceId);
			Record->SetTraceFlags(TraceFlags);
		}

		Logger->EmitLogRecord(MoveTemp(Record));
	}
#endif
}

FOtelMeter FOtelModule::GetMeter(const TCHAR* MeterName)
{
	otel::nostd::shared_ptr<otel::metrics::Meter> OtelMeter;
#if !PLATFORM_APPLE
	if (MeterProvider)
	{
		auto FinalMeterNameAnsi = StringCast<ANSICHAR>((MeterName == nullptr) ? *Config.Metric.DefaultMeterName : MeterName);
		OtelMeter = MeterProvider->GetMeter(FinalMeterNameAnsi.Get());
	}
#endif

	return FOtelMeter(MeterName, *this, OtelMeter);
}

void FOtelModule::ForceFlush(double TimeoutSeconds, const FName TracerName)
{
	FOtelTracer Tracer = GetTracer(TracerName);
	auto Timeout = std::chrono::milliseconds(static_cast<uint32>(1000 * TimeoutSeconds));
	Tracer.OtelTracer->ForceFlush(Timeout);
}
