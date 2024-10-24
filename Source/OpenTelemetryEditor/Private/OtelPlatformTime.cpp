// Copyright The Believer Company. All Rights Reserved.

#include "OtelPlatformTime.h"

#if PLATFORM_WINDOWS

#include "Windows/WindowsSystemIncludes.h"
#include "Windows/WindowsHWrapper.h"

// helpers ripped from WindowsPlatformTime.cpp
namespace TimeHelpers
{
	/**
	 * @return number of ticks based on the specified Filetime.
	 */
	static FORCEINLINE uint64 TicksFromFileTime(const FILETIME& Filetime)
	{
		const uint64 NumTicks = (uint64(Filetime.dwHighDateTime) << 32) + Filetime.dwLowDateTime;
		return NumTicks;
	}

	/**
	 * @return number of seconds based on the specified Filetime.
	 */
	static FORCEINLINE double ToSeconds(const FILETIME& Filetime)
	{
		return double(TicksFromFileTime(Filetime)) / double(ETimespan::TicksPerSecond);
	}
}; // namespace TimeHelpers

double BVPlatformTime::ProcessUptimeSeconds()
{
	static double LastTotalProcessTime = 0.0f;

	FILETIME CreationTime = { 0 };
	FILETIME ExitTime = { 0 };
	FILETIME KernelTime = { 0 };
	FILETIME UserTime = { 0 };
	FILETIME CurrentTime = { 0 };

	::GetProcessTimes(::GetCurrentProcess(), &CreationTime, &ExitTime, &KernelTime, &UserTime);
	::GetSystemTimeAsFileTime(&CurrentTime);

	const double CurrentTotalProcessTime = TimeHelpers::ToSeconds(CurrentTime) - TimeHelpers::ToSeconds(CreationTime);
	return CurrentTotalProcessTime;
}

#elif PLATFORM_LINUX

#include <sys/resource.h>

static double TimeValToSecs(const timeval& tv)
{
	return static_cast<double>(tv.tv_sec) + (static_cast<double>(tv.tv_usec) / 1e6);
}

double BVPlatformTime::ProcessUptimeSeconds()
{
	rusage Usage;

	if (getrusage(RUSAGE_SELF, &Usage) == 0)
	{
		const double UserTime = TimeValToSecs(Usage.ru_utime);
		const double SystemTime = TimeValToSecs(Usage.ru_stime);
		const double Uptime = UserTime + SystemTime;

		return Uptime;
	}

	return 0.0;
}

#else

double BVPlatformTime::ProcessUptimeSeconds()
{
	return 0.0;
}

#endif
