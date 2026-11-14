#include "stdafx.h"
#pragma hdrstop

#include "mt_tasks.h"

namespace mt_tasks
{
Flags32 switches = {~0u}; // experimental tasks start off: none now
counter counters[count] = {};
volatile bool counting = false;
thread_counter thread_counters[thread_slots] = {};
volatile LONG core_jobs[64] = {};
volatile LONG64 slot_ticks[thread_slots][count] = {};
} // namespace mt_tasks
