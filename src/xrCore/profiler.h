#pragma once

// Profiling backend removed from the production-only engine branch.
#define START_PROFILE(Name) {
#define STOP_PROFILE }
#define PROF_THREAD(Name)
#define PROF_START_CAPTURE()
#define PROF_STOP_CAPTURE()
#define PROF_SAVE_CAPTURE(Name)
#define PROF_FRAME(Name)
#define PROF_EVENT(Name)
#define PROF_EVENT_DYNAMIC(...)
