// Copyright The Believer Company. All Rights Reserved.

#pragma once

#include "Modules/ModuleInterface.h"
#include "AnalyticsEventAttribute.h"
#include "HAL/CriticalSection.h"
#include "Math/UnitConversion.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"

#include <memory> // shared_ptr

namespace opentelemetry
{
	inline namespace v1
	{
		namespace trace
		{
			class Tracer;
			class Span;
			class Scope;
		} // namespace trace

		namespace metrics
		{
			class Meter;
		} // namespace metrics

		namespace sdk::metrics
		{
			class MeterProvider;
		} // namespace sdk::metrics

		namespace logs
		{
			class Logger;
		}

		namespace sdk::logs
		{
			class LoggerProvider;
		}
	} // namespace v1
} // namespace opentelemetry

namespace otel = opentelemetry::v1;

struct FOtelScopedSpanImpl;
class FOtelStats;
class FOtelModule;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities

// Some editors have trouble parsing function prototypes prefixed with [[nodiscard]], so we'll use
// an alias for it
#define OTEL_NODISCARD [[nodiscard]]

template <typename T>
class FOtelLockedData
{
public:
	FOtelLockedData(T* InData, FCriticalSection& InMutex)
		: Data(InData), ScopeLock(InMutex) {}
	T& operator*() { return *Data; }
	T* operator->() { return Data; }

private:
	T* Data;
	UE::TScopeLock<FCriticalSection> ScopeLock;
};

template <typename T>
class FOtelUnlockedData
{
public:
	FOtelLockedData<T> Lock() { return FOtelLockedData(&Data, Mutex); }

private:
	T Data;
	FCriticalSection Mutex;
};

class FOtelOutputDevice : public FOutputDevice
{
public:
	void SetCategoryEnabled(const FName& LogCategory, const FName TracerName, ELogVerbosity::Type LogVerbosity);

	// FOutputDevice interface
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool IsMemoryOnly() const override;
	virtual bool CanBeUsedOnAnyThread() const override;
	virtual bool CanBeUsedOnMultipleThreads() const override;

	struct FTracerRouting
	{
		TMap<FName, ELogVerbosity::Type> CategoryVerbosity;
		ELogVerbosity::Type AllCategoryVerbosity = ELogVerbosity::NoLogging;
	};

	using FLogRoutingData = TMap<FName, FTracerRouting>;
	FOtelUnlockedData<FLogRoutingData> TracerLogging;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Otel API

struct OPENTELEMETRY_API FOtelTimestamp
{
	int64_t System = 0;
	int64_t Steady = 0;

	static FOtelTimestamp Now();
};

enum class EOtelStatus
{
	Ok,
	Error,
};

struct OPENTELEMETRY_API FOtelSpan
{
	FOtelSpan();
	FOtelSpan(FName InTracerName, std::shared_ptr<otel::trace::Span> InOtelSpan);

	void SetStatus(EOtelStatus Status);
	void AddAttribute(const FAnalyticsEventAttribute& Attribute);
	void AddAttributes(TArrayView<const FAnalyticsEventAttribute> Attributes);
	void AddEvent(const TCHAR* Name, TArrayView<const FAnalyticsEventAttribute> Attributes);
	FString TraceId() const;

	FName TracerName;
	std::shared_ptr<otel::trace::Span> OtelSpan;

#if WITH_EDITOR
	FString SpanName;
#endif
};

struct OPENTELEMETRY_API FOtelScopedSpan
{
	FOtelScopedSpan() = default;
	FOtelScopedSpan(const FOtelSpan& Span);
	FOtelScopedSpan(const FOtelScopedSpan& ScopedSpan);
	FOtelScopedSpan(FOtelScopedSpan&& ScopedSpan);
	~FOtelScopedSpan();

	FOtelScopedSpan& operator=(const FOtelScopedSpan& Span);

	FOtelSpan Inner() const;

	TSharedPtr<FOtelScopedSpanImpl> Scope;
};

struct OPENTELEMETRY_API FOtelTracer
{
	FOtelTracer(FName Name, std::shared_ptr<otel::trace::Tracer> InOtelTracer);

	// Use this API when you want to manually specify a parent span or you want to start a new root span
	OTEL_NODISCARD FOtelSpan StartSpan(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber);
	OTEL_NODISCARD FOtelSpan StartSpanOpts(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber, const FOtelSpan* OptionalParentSpan = nullptr, TArrayView<const FAnalyticsEventAttribute> Attributes = {}, FOtelTimestamp* OptionalTimestamp = nullptr);

	// Use this API when you want to automatically detect parent spans
	OTEL_NODISCARD FOtelScopedSpan StartSpanScoped(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber);
	OTEL_NODISCARD FOtelScopedSpan StartSpanScopedOpts(const TCHAR* SpanName, const TCHAR* File, int32 LineNumber, TArrayView<const FAnalyticsEventAttribute> Attributes, FOtelTimestamp* OptionalTimestamp);

	FName TracerName;
	std::shared_ptr<otel::trace::Tracer> OtelTracer;
};

// Monotonically-increasing counter. Negative values are not allowed.
struct FOtelCounter
{
	virtual ~FOtelCounter() = default;
	virtual void Add(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) = 0;
	virtual void Add(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) = 0;
};

// Records whatever the value was when the otel libs perform a collection for export to the backend.
struct FOtelGauge
{
	virtual ~FOtelGauge() = default;
	virtual void Observe(int64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) = 0;
	virtual void Observe(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) = 0;
};

// Record counts of values that get aggregated into buckets - good for large volumes of data where you don't care about exact values.
struct FOtelHistogram
{
	virtual ~FOtelHistogram() = default;
	virtual void Record(uint64 Value, TArrayView<FAnalyticsEventAttribute> Attributes) = 0;
	virtual void Record(double Value, TArrayView<FAnalyticsEventAttribute> Attributes) = 0;
};

enum EOtelInstrumentType
{
	Int64,
	Double,
};

struct FOtelHistogramBuckets
{
	static FOtelHistogramBuckets From(TArrayView<const uint64> InBuckets);
	static FOtelHistogramBuckets From(TArrayView<const double> InBuckets);

	TArrayView<const uint64_t> UInt64Buckets;
	TArrayView<const double> DoubleBuckets;
};

struct OPENTELEMETRY_API FOtelMeter
{
	FOtelMeter(const TCHAR* InName, FOtelModule& InModule, std::shared_ptr<otel::metrics::Meter> InOtelMeter);

	// Monotonically-increasing counter.
	TSharedPtr<FOtelCounter> CreateCounter(EOtelInstrumentType MeterType, const TCHAR* CounterName, EUnit UnitType = EUnit::Unspecified);

	// Reports the last-observed value at the time of export.
	TSharedPtr<FOtelGauge> CreateGauge(EOtelInstrumentType MeterType, const TCHAR* GaugeName, EUnit UnitType = EUnit::Unspecified);

	// Aggregates recorded values into buckets and reports the bucket count.
	TSharedPtr<FOtelHistogram> CreateHistogram(EOtelInstrumentType MeterType, const TCHAR* HistogramName, FOtelHistogramBuckets Buckets, EUnit UnitType = EUnit::Unspecified);

	FOtelModule& Module;
	FString Name;
	std::shared_ptr<otel::metrics::Meter> OtelMeter;
};

// Configuration values read out of DefaultOtel.ini. See the example provided in the plugin Config/.
struct FOtelSpanConfig
{
	FString EndpointUrl;
	FString Headers;
	FString ResourceAttributes;
	FString DefaultTracerName;
	bool bUseSsl = true;
};

struct FOtelMetricConfig
{
	FString EndpointUrl;
	FString Headers;
	FString ResourceAttributes;
	FString DefaultMeterName;
	FString Version;
	FString SchemaUrl;
	int32 ExportIntervalMs;
	int32 ExportTimeoutMs;
	bool bUseSsl = true;
};

struct FOtelLogConfig
{
	FString EndpointUrl;
	FString Headers;
	FString ResourceAttributes;
	FString AppName;
	bool bUseSsl = true;
};

struct FOtelConfig
{
	static FOtelConfig LoadFromIni();

	FOtelSpanConfig Trace;
	FOtelMetricConfig Metric;
	FOtelLogConfig Log;
};

// Emits all logs from the supplied category as span events, for the lifetime of the struct. If you want the hooks to
// persist longer than the current scope, use MakeUnique<> or MakeShared<> to create a heap-allocated object and store
// it until you want to unhook the logs.
struct OPENTELEMETRY_API FOtelScopedLogHook
{
	// InCategory - supply nullptr to capture logs from ALL categories
	// InTracerName - logs captured will be emitted as events under the currently-scoped span within the given tracer context.
	// LogVerbosity - logs will only be captured if they are a priority equal to or greater than this verbosity
	FOtelScopedLogHook(FLogCategoryBase* InCategory, FName InTracerName = NAME_None, ELogVerbosity::Type LogVerbosity = ELogVerbosity::Error);
	~FOtelScopedLogHook();

	const FLogCategoryBase* Category;
	FName TracerName;
};

// Note that this class explicitly does not conform to the IAnalyticsProviderModule interface. After evaluation,
// the stock Unreal interfaces were deemed to be a poor fit for wrapping the OpenTelemetry C++ library interfaces.
class OPENTELEMETRY_API FOtelModule : public IModuleInterface
{
public:
	/////////////////////////////////
	// IModuleInterface interface

	virtual void StartupModule() override;

	// The default tracer is flushed automatically on module shutdown - if you want to flush other tracers, you must call
	// ForceFlush() explicitly for them
	virtual void ShutdownModule() override;

	/////////////////////////////////
	// FOtelModule interface

	static FOtelModule& Get();
	static FOtelModule* TryGet();

	// Unreal log -> span event routing
	void SetEnableEventsForLogChannel(const FLogCategoryBase* LogCategory, FName TracerName, ELogVerbosity::Type LogVerbosity = ELogVerbosity::NoLogging);

	// Gets a tracer interface for creating spans.
	// passing NAME_None for TracerName falls back to FOtelConfig::DefaultTracerName
	FOtelTracer GetTracer(FName TracerName = NAME_None);

	// Pinning a scoped span allows the current object to go out of scope, but it will still be active until it is
	// explicitly unpinned. Useful for tracking the span of operations that are:
	// * async
	// * span multiple frames
	// * started/stopped from engine code
	uint64 Pin(FOtelScopedSpan ScopedSpan);
	FOtelScopedSpan* GetPinnedSpan(uint64 SpanId);
	FOtelScopedSpan Unpin(uint64 SpanId);

	// Emit log events to the remote. Will associated the log with the specified tracer's currently-active span, if any.
	void EmitLog(const TCHAR* Message, TArrayView<const FAnalyticsEventAttribute> Attributes, const TCHAR* File, int32 LineNumber, const FName TracerName = NAME_None, TOptional<EOtelStatus> Status = TOptional<EOtelStatus>());

	// Gets the meter interface for creating instruments such as counters, gauges, and histograms.
	// passing NAME_None for MeterName falls back to FOtelConfig::DefaultMeterName
	FOtelMeter GetMeter(const TCHAR* MeterName = nullptr);

	void ForceFlush(double TimeoutSeconds, const FName TracerName = NAME_None);

private:
	void LazyCreateLogHook();

	using FTracerToScopeStack = TMap<FName, TArray<TSharedPtr<FOtelScopedSpanImpl>>>;

	FOtelConfig Config;
	FString SessionId;
	FOtelUnlockedData<FTracerToScopeStack> LockedTracerToScopeStack;
	TMap<uint64, FOtelScopedSpan> PinnedSpans;
	TUniquePtr<FOtelOutputDevice> OutputDevice;
	std::shared_ptr<otel::sdk::metrics::MeterProvider> MeterProvider;
	std::shared_ptr<otel::sdk::logs::LoggerProvider> LoggerProvider;

	FOtelStats* FrameStats = nullptr;

	friend struct FOtelScopedSpan;
	friend struct FOtelScopedSpanImpl;
	friend struct FOtelTracer;
	friend struct FOtelMeter;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Convenience macros

#define OTEL_SPAN(SpanName) \
	FOtelModule::Get().GetTracer().StartSpanScoped(SpanName, TEXT(__FILE__), __LINE__)

#define OTEL_SPAN_FUNC() \
	OTEL_SPAN(StringCast<TCHAR>(__FUNCTION__).Get())

#define OTEL_TRACER_SPAN(TracerName, SpanName) \
	FOtelModule::Get().GetTracer(TracerName).StartSpanScoped(SpanName, TEXT(__FILE__), __LINE__)

#define OTEL_TRACER_SPAN_FUNC(TracerName) \
	OTEL_TRACER_SPAN(TracerName, StringCast<TCHAR>(__FUNCTION__).Get())

// Emits an event for the currently-scoped span within the given tracer context. Not using the TRACER variant uses the
// default tracer.
#define OTEL_LOG(Message, Attributes) \
	FOtelModule::Get().EmitLog(Message, Attributes, TEXT(__FILE__), __LINE__)

#define OTEL_LOG_ERROR(Message, Attributes) \
	FOtelModule::Get().EmitLog(Message, Attributes, TEXT(__FILE__), __LINE__, NAME_None, EOtelStatus::Error)

#define OTEL_TRACER_LOG(TracerName, Message, Attributes) \
	FOtelModule::Get().EmitLog(Message, Attributes, TEXT(__FILE__), __LINE__, TracerName)

#define OTEL_TRACER_LOG_ERROR(TracerName, Message, Attributes) \
	FOtelModule::Get().EmitLog(Message, Attributes, TEXT(__FILE__), __LINE__, TracerName, EOtelStatus::Error)

// Use this to capture all logs within the current scope. See FOtelScopedLogHook for more details.
#define OTEL_SCOPED_LOG_HOOK(LogCategory, LogVerbosity) \
	FOtelScopedLogHook PREPROCESSOR_JOIN(OtelLogHook, __LINE__) = FOtelScopedLogHook(&LogCategory, NAME_None, LogVerbosity)

#define OTEL_TRACER_SCOPED_LOG_HOOK(TracerName, LogCategory, LogVerbosity) \
	FOtelScopedLogHook PREPROCESSOR_JOIN(OtelLogHook, __LINE__) = FOtelScopedLogHook(&LogCategory, TracerName, LogVerbosity)

#define OTEL_SCOPED_LOG_HOOK_ALL(LogVerbosity) \
	FOtelScopedLogHook PREPROCESSOR_JOIN(OtelLogHook, __LINE__) = FOtelScopedLogHook(nullptr, NAME_None, LogVerbosity)

#define OTEL_TRACER_SCOPED_LOG_HOOK_ALL(TracerName, LogVerbosity) \
	FOtelScopedLogHook PREPROCESSOR_JOIN(OtelLogHook, __LINE__) = FOtelScopedLogHook(nullptr, TracerName, LogVerbosity)
