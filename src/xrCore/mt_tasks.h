#pragma once

// The engine's parallel tasks, for the mt_panel window: what each one costs a frame, and the switches of those that
// had no console command before. A task switched off does the same work on the thread that would have waited for it.
namespace mt_tasks
{
enum task : u32
{
	frame_job,
	frame_independent,
	occlusion_grass,
	level_path,
	detail_path,
	bullets,
	map,
	level_sounds,
	weapon_sounds,
	frame_audio,
	frame_tail,
	lua_gc,
	skeletons,
	light_faces,
	opaque_sort,
	particle_render,
	wallmark_skin,
	physics,
	particle_turbulence,
	ai_vision,
	main_static, // main view statics recorded on the workers
	sun_static, // static casters of the sun cascades walked on a worker while the scene is drawn
	// parts of the frame job without a switch of their own, shown in the THREADS tab
	job_scheduler, // NPC and A-Life scheduler
	job_lua_prelude, // actor and hear Lua callbacks
	job_frame_mt, // seqFrameMT: physics world, network
	count
};

// Bit (1 << task). Read only for the tasks with an mt_* command over this word (xr_ioc_cmd.cpp), the others keep
// their older switch.
XRCORE_API extern Flags32 switches;
IC bool on(task t) { return switches.test(1u << t); }

struct counter
{
	volatile LONG64 ticks; // CPU time of all lanes since the window took the previous frame, QPC ticks
	volatile LONG lanes; // the most threads one run used
};
XRCORE_API extern counter counters[count];
XRCORE_API extern volatile bool counting; // the window is open

// Threads of the job pool, counted while the window is open: slot 0 is the thread that started the pool (the main
// thread), 1.. the workers, the last slot every other thread.
constexpr u32 thread_slots = 66;
constexpr u32 other_slot = thread_slots - 1;
XRCORE_API u32 current_thread_slot(); // job_system.cpp
XRCORE_API extern volatile LONG64 slot_ticks[thread_slots][count]; // time of each task on each thread

// Times the work of one lane, on any thread; task count times nothing.
class lane_time
{
	const task m_task;
	const u64 m_start;

public:
	explicit lane_time(task t) : m_task(t), m_start(counting && t < count ? CPU::QPC() : 0) {}
	~lane_time()
	{
		if (!m_start)
			return;
		const LONG64 ticks = LONG64(CPU::QPC() - m_start);
		InterlockedExchangeAdd64(&counters[m_task].ticks, ticks);
		InterlockedExchangeAdd64(&slot_ticks[current_thread_slot()][m_task], ticks);
	}
	lane_time(const lane_time&) = delete;
	lane_time& operator=(const lane_time&) = delete;
};
struct thread_counter
{
	volatile LONG tid; // set once by the thread itself
	volatile LONG jobs; // jobs it ran
	volatile LONG core; // processor of its last job
	volatile LONG64 start_delay; // QPC ticks from submit to start, summed over its jobs
	volatile LONG64 cores; // processors of its jobs, bit per processor below 64
};
XRCORE_API extern thread_counter thread_counters[thread_slots];
XRCORE_API extern volatile LONG core_jobs[64]; // jobs started on each processor below 64

// How many threads one run of the task took, the caller included. A task never reported runs on one thread.
IC void lanes(task t, u32 n)
{
	if (!counting)
		return;
	LONG seen = counters[t].lanes;
	while (LONG(n) > seen)
	{
		const LONG previous = InterlockedCompareExchange(&counters[t].lanes, LONG(n), seen);
		if (previous == seen)
			break;
		seen = previous;
	}
}
} // namespace mt_tasks
