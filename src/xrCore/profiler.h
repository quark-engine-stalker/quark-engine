#pragma once

// Tracy profiler. Build with MSBuild /p:QuarkTracy=true (see Common.props) and connect
// tracy-profiler 0.14.1. Ordinary builds compile every macro below to nothing.
#ifdef TRACY_ENABLE
#	include "../3rd party/tracy/public/tracy/Tracy.hpp"
#	include <string.h>

namespace xr_profiler
{
constexpr const char* zone_name() { return nullptr; } // Tracy shows the function name
constexpr const char* zone_name(const char* name) { return name; }
inline void zone_text(tracy::ScopedZone& zone, const char* text) { if (text) zone.Text(text, strlen(text)); }
}

#	define PROF_EVENT(...) ZoneNamedN(___tracy_scoped_zone, ::xr_profiler::zone_name(__VA_ARGS__), true)
// Text of the enclosing PROF_EVENT zone; the argument is evaluated only while Tracy is connected.
#	define PROF_TEXT(Text) do { if (___tracy_scoped_zone.IsActive()) ::xr_profiler::zone_text(___tracy_scoped_zone, Text); } while (0)
#	define PROF_THREAD(Name) ::tracy::SetThreadName(Name)
#	define PROF_FRAME(Name) FrameMark
#	define START_PROFILE(Name) { PROF_EVENT(Name);
#	define STOP_PROFILE }
#else
#	define PROF_EVENT(...)
#	define PROF_TEXT(Text)
#	define PROF_THREAD(Name)
#	define PROF_FRAME(Name)
#	define START_PROFILE(Name) {
#	define STOP_PROFILE }
#endif
