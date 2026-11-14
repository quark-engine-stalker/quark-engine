#include "stdafx.h"
#include "imgui_base.h"
#include "device.h"
#include "mt_panel.h"
#include "frame_stats.h"
#include <tlhelp32.h>
#include <powrprof.h>
#include <intrin.h>
#include <algorithm>

#pragma comment(lib, "PowrProf.lib")

// THREADS of QUARK-PANEL. Once a second a sampler thread reads the CPU cycles of every thread of the process
// (QueryThreadCycleTime) and the load of every processor; the job pool counts, per thread, the jobs it ran, how long
// they waited to start and where they ran (mt_tasks).
namespace xr_imgui
{
namespace quark_mt
{
namespace
{
constexpr u32 max_threads = 256;
constexpr u32 max_cores = 64;
constexpr u32 slots = mt_tasks::thread_slots;

struct sampled_thread
{
	DWORD tid;
	int ideal; // ideal processor, -1 when unknown
	u64 cycles; // per second
	char name[64];
};

// A logical processor as Windows describes it; read once, the topology does not change while the game runs
struct processor
{
	u8 core, thread; // physical core and its hardware thread, both counted from 0 in the order of the processors
	u8 rank; // CPPC preferred core rank (SchedulingClass), higher is preferred
	u8 efficiency; // EfficiencyClass: on hybrid processors the P-cores have the higher one
	u32 mhz; // the maximum frequency Windows reports
	u32 l2_kb, l3_kb;
};

struct sample
{
	processor processors[max_cores];
	sampled_thread threads[max_threads];
	u32 thread_count;
	float core_busy[max_cores]; // 0..1, all processes
	u32 core_count;
	u64 cycles; // the whole process, per second
	double tsc_hz; // the rate of the cycle counter, measured over the same second
};

// The sampler allocates nothing: at exit it may outlive the engine's memory manager.
SRWLOCK sample_lock = SRWLOCK_INIT;
sample published = {};
sample shown = {};
volatile LONG sampler_started = FALSE;

struct cached_thread
{
	DWORD tid;
	HANDLE handle;
	ULONG64 cycles;
	int ideal;
	bool seen;
	char name[64];
};

struct processor_times // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
{
	LARGE_INTEGER idle, kernel, user, dpc, interrupt;
	ULONG interrupt_count;
};

using get_thread_description_fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using query_system_information_fn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
using query_thread_information_fn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
constexpr ULONG system_processor_performance_information = 8;
constexpr ULONG thread_query_set_win32_start_address = 9;

// A thread without a name is called after the module it started in: [nvwgf2umx.dll], [OpenAL32.dll]
void name_by_start_module(HANDLE thread, query_thread_information_fn query_thread, char (&name)[64])
{
	PVOID start = nullptr;
	if (!query_thread || query_thread(thread, thread_query_set_win32_start_address, &start, sizeof(start), nullptr) ||
		!start)
		return;
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		static_cast<LPCWSTR>(start), &module))
		return;
	char path[MAX_PATH];
	if (!GetModuleFileNameA(module, path, MAX_PATH))
		return;
	const char* base = strrchr(path, '\\');
	xr_sprintf(name, "[%s]", base ? base + 1 : path);
}

struct power_information // PROCESSOR_POWER_INFORMATION
{
	ULONG number, max_mhz, current_mhz, mhz_limit, max_idle_state, current_idle_state;
};

void read_topology(sample& out)
{
	static u8 buffer[32768];
	u32 core_id[max_cores]; // CoreIndex of the CPU Sets: the same for the hardware threads of one core
	for (u32 lp = 0; lp < max_cores; ++lp)
		core_id[lp] = lp; // without CPU Sets every processor is a core
	const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	using cpu_sets_fn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
	const auto cpu_sets = kernel ?
		reinterpret_cast<cpu_sets_fn>(GetProcAddress(kernel, "GetSystemCpuSetInformation")) : nullptr;
	ULONG length = 0;
	if (cpu_sets && cpu_sets(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer), sizeof(buffer), &length,
		GetCurrentProcess(), 0))
	{
		for (u8* it = buffer; it < buffer + length; it += reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(it)->Size)
		{
			const SYSTEM_CPU_SET_INFORMATION& info = *reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(it);
			if (info.Type != CpuSetInformation || info.CpuSet.Group || info.CpuSet.LogicalProcessorIndex >= max_cores)
				continue;
			core_id[info.CpuSet.LogicalProcessorIndex] = info.CpuSet.CoreIndex;
			out.processors[info.CpuSet.LogicalProcessorIndex].rank = info.CpuSet.SchedulingClass;
			out.processors[info.CpuSet.LogicalProcessorIndex].efficiency = info.CpuSet.EfficiencyClass;
		}
	}
	u32 physical_cores = 0;
	for (u32 lp = 0; lp < max_cores; ++lp)
	{
		processor& p = out.processors[lp];
		u32 earlier = 0;
		while (earlier < lp && core_id[earlier] != core_id[lp])
			++earlier;
		p.core = earlier == lp ? u8(physical_cores++) : out.processors[earlier].core;
		p.thread = 0;
		for (u32 other = 0; other < lp; ++other)
			p.thread += out.processors[other].core == p.core ? 1 : 0;
	}

	DWORD cache_length = sizeof(buffer);
	if (GetLogicalProcessorInformationEx(RelationCache, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer),
		&cache_length))
	{
		for (u8* it = buffer; it < buffer + cache_length;
			it += reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(it)->Size)
		{
			const CACHE_RELATIONSHIP& cache = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(it)->Cache;
			if (cache.GroupMask.Group || (cache.Level != 2 && cache.Level != 3))
				continue;
			for (u32 lp = 0; lp < max_cores; ++lp)
				if (cache.GroupMask.Mask & (KAFFINITY(1) << lp))
					(cache.Level == 2 ? out.processors[lp].l2_kb : out.processors[lp].l3_kb) = cache.CacheSize / 1024;
		}
	}

	static power_information power[max_cores];
	if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power, sizeof(power)) == 0)
		for (const power_information& info : power)
			if (info.number < max_cores && info.max_mhz)
				out.processors[info.number].mhz = info.max_mhz;
}

void sampler(void*)
{
	static cached_thread cache[max_threads];
	static sample next;
	static processor_times previous[max_cores], current[max_cores];
	u32 cached = 0;
	u64 previous_time = 0;
	const DWORD process = GetCurrentProcessId();
	const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	const auto get_description = kernel ?
		reinterpret_cast<get_thread_description_fn>(GetProcAddress(kernel, "GetThreadDescription")) : nullptr;
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const auto query_system = ntdll ?
		reinterpret_cast<query_system_information_fn>(GetProcAddress(ntdll, "NtQuerySystemInformation")) : nullptr;
	const auto query_thread = ntdll ?
		reinterpret_cast<query_thread_information_fn>(GetProcAddress(ntdll, "NtQueryInformationThread")) : nullptr;
	u64 previous_tsc = 0;
	read_topology(next);

	for (;;)
	{
		Sleep(1000);
		if (!mt_tasks::counting)
		{
			previous_time = 0; // the next sample only sets the baseline
			continue;
		}
		const u64 now = CPU::QPC();
		const u64 tsc = __rdtsc();
		const double seconds = previous_time ? double(now - previous_time) / double(CPU::qpc_freq) : 0.0;
		// QueryThreadCycleTime counts in the units of the time stamp counter
		next.tsc_hz = seconds > 0.0 ? double(tsc - previous_tsc) / seconds : 0.0;
		previous_time = now;
		previous_tsc = tsc;

		for (u32 index = 0; index < cached; ++index)
			cache[index].seen = false;
		next.thread_count = 0;
		next.cycles = 0;
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 entry = {};
			entry.dwSize = sizeof(entry);
			for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
			{
				if (entry.th32OwnerProcessID != process)
					continue;
				u32 index = 0;
				while (index < cached && cache[index].tid != entry.th32ThreadID)
					++index;
				if (index == cached)
				{
					const HANDLE handle = cached < max_threads ?
						OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID) : nullptr;
					if (!handle)
						continue;
					cached_thread& added = cache[cached++];
					added = {};
					added.tid = entry.th32ThreadID;
					added.handle = handle;
					added.seen = true;
					QueryThreadCycleTime(handle, &added.cycles);
					PROCESSOR_NUMBER ideal = {};
					added.ideal = GetThreadIdealProcessorEx(handle, &ideal) ? int(ideal.Number) : -1;
					PWSTR description = nullptr;
					if (get_description && SUCCEEDED(get_description(handle, &description)) && description)
					{
						WideCharToMultiByte(CP_UTF8, 0, description, -1, added.name, sizeof(added.name), nullptr,
							nullptr);
						LocalFree(description);
					}
					if (!added.name[0])
						name_by_start_module(handle, query_thread, added.name);
					continue; // the first reading is the baseline
				}

				cached_thread& thread = cache[index];
				thread.seen = true;
				ULONG64 cycles = 0;
				if (!QueryThreadCycleTime(thread.handle, &cycles))
					continue;
				const u64 delta = cycles - thread.cycles;
				thread.cycles = cycles;
				if (seconds <= 0.0 || next.thread_count == max_threads)
					continue;
				sampled_thread& out = next.threads[next.thread_count++];
				out.tid = thread.tid;
				out.ideal = thread.ideal;
				out.cycles = u64(double(delta) / seconds);
				memcpy(out.name, thread.name, sizeof(out.name));
				next.cycles += out.cycles;
			}
			CloseHandle(snapshot);
		}
		for (u32 index = 0; index < cached;)
		{
			if (cache[index].seen)
			{
				++index;
				continue;
			}
			CloseHandle(cache[index].handle);
			cache[index] = cache[--cached];
		}

		next.core_count = 0;
		ULONG returned = 0;
		if (query_system && query_system(system_processor_performance_information, current, sizeof(current),
			&returned) == 0)
		{
			const u32 cores = _min(u32(returned / sizeof(processor_times)), max_cores);
			for (u32 core = 0; core < cores; ++core)
			{
				const double idle = double(current[core].idle.QuadPart - previous[core].idle.QuadPart);
				const double total = double(current[core].kernel.QuadPart - previous[core].kernel.QuadPart +
					current[core].user.QuadPart - previous[core].user.QuadPart); // kernel time includes idle
				next.core_busy[core] = total > 0.0 ? float(_min(_max(1.0 - idle / total, 0.0), 1.0)) : 0.f;
			}
			if (seconds > 0.0)
				next.core_count = cores;
			memcpy(previous, current, sizeof(current));
		}

		if (seconds > 0.0)
		{
			AcquireSRWLockExclusive(&sample_lock);
			published = next;
			ReleaseSRWLockExclusive(&sample_lock);
		}
	}
}

// The job pool, per second
struct pool_thread
{
	u32 jobs;
	u64 delay; // QPC ticks
	u64 cores;
	LONG core;
};
pool_thread pool[slots] = {};
u32 core_jobs[64] = {}; // per second
u64 slot_tasks[slots][mt_tasks::count] = {}; // QPC ticks of each task on each thread, the last second
u64 second_start = 0;
bool was_open = false;

// The frame of the main thread from g_frame_stats, per frame of the last second
float stage_ms[fsStageCount] = {};
u64 stage_previous[fsStageCount] = {};
u64 frames_previous = 0;
SFrameClassStat updatecl_previous[SFrameStats::class_slots] = {};
struct class_time
{
	CLASS_ID clsid;
	float ms, calls;
};
class_time classes[8] = {};
u32 class_count = 0;

DWORD expanded = 0; // the thread opened in the THREADS tab
// The lines of the opened thread stay once they were shown, so a refresh does not move what is below them
DWORD lines_of = 0;
u64 shown_stages = 0;
static_assert(fsStageCount <= 64, "shown_stages is a 64 bit mask");
u32 shown_tasks = 0;

u32 used_slots() { return _min(xr_jobs::worker_count() + 1, slots - 1); } // main and the workers

void take_pool(bool publish)
{
	const u32 used = used_slots();
	auto take_slot = [&](u32 slot)
	{
		mt_tasks::thread_counter& counter = mt_tasks::thread_counters[slot];
		pool_thread& out = pool[slot];
		out.jobs = u32(InterlockedExchange(&counter.jobs, 0));
		out.delay = u64(InterlockedExchange64(&counter.start_delay, 0));
		out.cores = u64(InterlockedExchange64(&counter.cores, 0));
		out.core = counter.core;
	};
	for (u32 slot = 0; slot < used; ++slot)
		take_slot(slot);
	take_slot(mt_tasks::other_slot);
	for (u32 core = 0; core < 64; ++core)
		core_jobs[core] = u32(InterlockedExchange(&mt_tasks::core_jobs[core], 0));
	auto take_tasks = [&](u32 slot)
	{
		for (u32 task = 0; task < mt_tasks::count; ++task)
			slot_tasks[slot][task] = u64(InterlockedExchange64(&mt_tasks::slot_ticks[slot][task], 0));
	};
	for (u32 slot = 0; slot < used; ++slot)
		take_tasks(slot);
	take_tasks(mt_tasks::other_slot);
	if (!publish)
	{
		ZeroMemory(pool, sizeof(pool));
		ZeroMemory(core_jobs, sizeof(core_jobs));
		ZeroMemory(slot_tasks, sizeof(slot_tasks));
	}
}

// The stages of the main thread's frame and the costliest UpdateCL classes of the last second
void take_frame_stats(bool publish)
{
	const SFrameStats& stats = g_frame_stats;
	const u64 frames = stats.frames - frames_previous;
	const bool valid = publish && stats.enabled && frames && stats.frames > frames_previous;
	const double tick_ms = 1000.0 / double(CPU::qpc_freq);
	for (u32 stage = 0; stage < fsStageCount; ++stage)
	{
		stage_ms[stage] = valid && stats.sum[stage] >= stage_previous[stage] ?
			float(double(stats.sum[stage] - stage_previous[stage]) * tick_ms / double(frames)) : 0.f;
		stage_previous[stage] = stats.sum[stage];
	}
	class_count = 0;
	for (u32 slot = 0; slot < SFrameStats::class_slots; ++slot)
	{
		const SFrameClassStat& now = stats.updatecl[slot];
		const SFrameClassStat& before = updatecl_previous[slot];
		const bool same = before.calls && before.clsid == now.clsid && now.ticks >= before.ticks;
		const u64 ticks = now.ticks - (same ? before.ticks : 0);
		const u32 calls = now.calls - (same ? before.calls : 0);
		if (!valid || !calls || !ticks)
			continue;
		const class_time entry = {now.clsid, float(double(ticks) * tick_ms / double(frames)),
			float(double(calls) / double(frames))};
		// the 8 costliest, sorted
		if (class_count < 8)
			classes[class_count++] = entry;
		else if (entry.ms > classes[7].ms)
			classes[7] = entry;
		else
			continue;
		for (u32 at = class_count - 1; at > 0 && classes[at].ms > classes[at - 1].ms; --at)
			std::swap(classes[at], classes[at - 1]);
	}
	memcpy(updatecl_previous, stats.updatecl, sizeof(updatecl_previous));
	frames_previous = stats.frames;
}

int slot_of(DWORD tid)
{
	for (u32 slot = 0; slot < used_slots(); ++slot)
		if (DWORD(mt_tasks::thread_counters[slot].tid) == tid)
			return int(slot);
	return -1;
}

const char* thread_label(const sampled_thread& thread, int slot, string64& buffer)
{
	if (slot == 0)
		return L("MAIN THREAD", u8"ГЛАВНЫЙ ПОТОК");
	if (slot > 0)
	{
		xr_sprintf(buffer, L("JOB WORKER #%d", u8"ВОРКЕР #%d"), slot - 1);
		return buffer;
	}
	if (thread.name[0])
		return thread.name;
	xr_sprintf(buffer, "thread %u", thread.tid);
	return buffer;
}

// A thread the engine started and named (thread_spawn); a thread without a name is called after its module, [name.dll]
bool engine_thread(const sampled_thread& thread) { return thread.name[0] && thread.name[0] != '['; }

double tsc_hz() { return shown.tsc_hz > 0.0 ? shown.tsc_hz : double(_max(CPU::clk_per_second, u64(1))); }

// Threads sorted by cycles, the busiest first
u32 sorted[max_threads];
u32 sort_threads()
{
	for (u32 index = 0; index < shown.thread_count; ++index)
		sorted[index] = index;
	std::sort(sorted, sorted + shown.thread_count,
		[](u32 a, u32 b) { return shown.threads[a].cycles > shown.threads[b].cycles; });
	return shown.thread_count;
}

void wrapped_tooltip(const char* text)
{
	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

// "1 MB", "512 KB"
void cache_size(string16& out, u32 kb)
{
	if (!kb)
		xr_strcpy(out, "-");
	else if (kb >= 1024)
		xr_sprintf(out, "%u %s", kb / 1024, L("MB", u8"МБ"));
	else
		xr_sprintf(out, "%u %s", kb, L("KB", u8"КБ"));
}

// One block per physical core: its rank, frequency and caches, and its threads (SMT) as a pair of cells side by side
void cores_grid()
{
	if (!shown.core_count)
	{
		ImGui::TextDisabled("%s", L("measuring...", u8"измерение..."));
		return;
	}
	const float gap = 10.f, line = ImGui::GetTextLineHeight() + 2.f, pad = 6.f, text_pad = 7.f;
	u8 best_rank = 0, worst_rank = 255, top_efficiency = 0, low_efficiency = 255;
	u32 cores = 0, smt = 1;
	for (u32 lp = 0; lp < shown.core_count; ++lp)
	{
		best_rank = _max(best_rank, shown.processors[lp].rank);
		worst_rank = _min(worst_rank, shown.processors[lp].rank);
		top_efficiency = _max(top_efficiency, shown.processors[lp].efficiency);
		low_efficiency = _min(low_efficiency, shown.processors[lp].efficiency);
		cores = _max(cores, u32(shown.processors[lp].core) + 1);
		smt = _max(smt, u32(shown.processors[lp].thread) + 1);
	}
	const bool ranked = best_rank != worst_rank; // all equal: CPPC is off
	const bool hybrid = top_efficiency != low_efficiency; // P-cores and E-cores
	// a block is as wide as its widest texts in the language of the window, at least 270 px
	string64 text;
	xr_sprintf(text, L("thread %u", u8"поток %u"), shown.core_count - 1);
	const float cell_need = 2.f * text_pad + _max(_max(ImGui::CalcTextSize(text).x + 8.f +
		ImGui::CalcTextSize("100%").x, ImGui::CalcTextSize("MAIN-THREAD").x), ImGui::CalcTextSize(L("9999 jobs/s", u8"9999 задач/с")).x);
	const float header_need = 2.f * (_max(ImGui::CalcTextSize("CORE-00 P").x, ImGui::CalcTextSize("CPPC 00").x + 6.f) + 12.f) +
		ImGui::CalcTextSize(L("0.00 GHz", u8"0.00 ГГц")).x;
	const float cache_need = ImGui::CalcTextSize(L("cache L2 0000 KB  L3 000 MB", u8"кэш L2 0000 КБ  L3 000 МБ")).x;
	const float box_need = _max(_max(cell_need * float(smt), header_need), _max(cache_need, 270.f - 2.f * pad)) + 2.f * pad;
	const float width = ImGui::GetContentRegionAvail().x;
	const u32 per_row = _max(u32((width + gap) / (box_need + gap)), 1u);
	const float box_w = (width - gap * float(per_row - 1)) / float(per_row);
	const float cell_h = line * 2.f + 18.f;
	const float box_h = pad * 3.f + line * 2.f + cell_h;
	const u32 rows = (cores + per_row - 1) / per_row;
	const ImVec2 size(width, float(rows) * (box_h + gap) - gap);
	if (!ImGui::IsRectVisible(size)) // scrolled away
	{
		ImGui::Dummy(size);
		return;
	}
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 ink_u = ImGui::GetColorU32(ink), muted_u = ImGui::GetColorU32(muted), stroke_u = ImGui::GetColorU32(stroke),
		accent_u = ImGui::GetColorU32(accent), on_accent_u = ImGui::GetColorU32(on_accent);
	const u32 main_lp = u32(pool[0].core); // where the main thread of the game is now
	const char* tooltip_text = nullptr;
	string512 tooltip;

	for (u32 core = 0; core < cores; ++core)
	{
		const ImVec2 at(origin.x + float(core % per_row) * (box_w + gap), origin.y + float(core / per_row) * (box_h + gap));
		u32 first = 0, threads = 0;
		for (u32 lp = shown.core_count; lp-- > 0;)
			if (shown.processors[lp].core == core)
			{
				first = lp;
				++threads;
			}
		const processor& p = shown.processors[first];
		draw->AddRect(at, ImVec2(at.x + box_w, at.y + box_h), stroke_u);

		const float y = at.y + pad;
		if (hybrid)
			xr_sprintf(text, "CORE-%u %s", core, p.efficiency == top_efficiency ? "P" : "E");
		else
			xr_sprintf(text, "CORE-%u", core);
		draw->AddText(ImVec2(at.x + pad, y), ink_u, text);
		if (p.mhz)
		{
			xr_sprintf(text, "%.2f %s", double(p.mhz) / 1000.0, L("GHz", u8"ГГц"));
			draw->AddText(ImVec2(at.x + box_w * 0.5f - ImGui::CalcTextSize(text).x * 0.5f, y), ink_u, text);
		}
		if (ranked)
			xr_sprintf(text, "CPPC %u", u32(p.rank));
		else
			xr_strcpy(text, "CPPC -");
		const bool best = ranked && p.rank == best_rank;
		const float rank_w = ImGui::CalcTextSize(text).x;
		const ImVec2 rank_at(at.x + box_w - pad - rank_w, y);
		if (best) // the fastest cores: a light plate
			draw->AddRectFilled(ImVec2(rank_at.x - 3.f, y - 1.f), ImVec2(rank_at.x + rank_w + 3.f, y + line - 2.f), accent_u);
		draw->AddText(rank_at, best ? on_accent_u : muted_u, text);

		string16 l2, l3;
		cache_size(l2, p.l2_kb);
		cache_size(l3, p.l3_kb);
		xr_sprintf(text, L("cache L2 %s  L3 %s", u8"кэш L2 %s  L3 %s"), l2, l3);
		draw->AddText(ImVec2(at.x + pad, y + line), muted_u, text);
		if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + box_w, y + line * 2.f)))
			tooltip_text = L("CORE: a physical core of the processor, counted from 0. With SMT (Hyper-Threading) it runs "
				"two threads at once, the cells. The threads are numbered like in the Task Manager, from 0 to the last one "
				"of the processor. P and E on hybrid processors: performance and efficient cores. CPPC: how fast the processor itself rates the core, higher is faster, the fastest "
				"are on a light plate, Windows gives them the heaviest work. The frequency is the maximum Windows reports (on some "
				"systems the base one). L2: the cache of this core, L3: the cache it shares with its neighbours.",
				u8"CORE: физическое ядро процессора, счёт с 0. С SMT (Hyper-Threading) оно выполняет два потока сразу - "
				u8"это ячейки. Потоки пронумерованы как в Диспетчере задач, от 0 до последнего потока процессора. P и E на "
				u8"гибридных процессорах: производительные и энергоэффективные ядра. CPPC: насколько быстрым ядро считает сам процессор, выше - быстрее, самые быстрые на светлой плашке, Windows "
				u8"отдаёт им самую тяжёлую работу. Частота - максимальная по данным Windows (на части систем базовая). "
				u8"L2: кэш этого ядра, L3: кэш, общий с соседними ядрами.");

		// the threads of the core: cells side by side, one shared frame
		const float cells_x = at.x + pad, cells_y = y + line * 2.f + pad;
		const float cell_w = (box_w - 2.f * pad) / float(_max(threads, 1u));
		for (u32 lp = 0; lp < shown.core_count; ++lp)
		{
			const processor& hw = shown.processors[lp];
			if (hw.core != core)
				continue;
			const ImVec2 cell(cells_x + cell_w * float(hw.thread), cells_y);
			const ImVec2 cell_end(cell.x + cell_w, cell.y + cell_h);
			const float busy = shown.core_busy[lp];
			const bool main = lp == main_lp;
			const u32 jobs = lp < 64 ? core_jobs[lp] : 0;
			draw->AddRect(cell, cell_end, main ? accent_u : stroke_u, 0.f, 0, main ? 2.f : 1.f);
			xr_sprintf(text, L("thread %u", u8"поток %u"), lp);
			draw->AddText(ImVec2(cell.x + text_pad, cell.y + 5.f), muted_u, text);
			xr_sprintf(text, "%.0f%%", busy * 100.f);
			draw->AddText(ImVec2(cell_end.x - text_pad - ImGui::CalcTextSize(text).x, cell.y + 5.f), ink_u, text);
			// the load fills the cell from the left, under the text of the second line
			const float bar_top = cell.y + line + 6.f;
			draw->AddRectFilled(ImVec2(cell.x + text_pad, bar_top),
				ImVec2(cell.x + text_pad + (cell_w - 2.f * text_pad) * busy, bar_top + 4.f), accent_u);
			if (main)
				xr_strcpy(text, "MAIN-THREAD");
			else if (jobs >= 10000)
				xr_sprintf(text, L("%uk jobs/s", u8"%uk задач/с"), jobs / 1000);
			else
				xr_sprintf(text, L("%u jobs/s", u8"%u задач/с"), jobs);
			draw->AddText(ImVec2(cell.x + text_pad, bar_top + 8.f), main ? IM_COL32(0x14, 0x9A, 0xE7, 255) : muted_u, text);

			if (ImGui::IsMouseHoveringRect(cell, cell_end))
			{
				xr_sprintf(tooltip, L("CORE-%u, thread %u: %.0f%% busy with all programs. Engine jobs started here a "
					"second: %u.%s", u8"CORE-%u, поток %u: занят на %.0f%% всеми программами. Задач движка стартовало "
					u8"здесь за секунду: %u.%s"), core, lp, busy * 100.f, jobs, main ? L(" The main thread of the game runs here now (thick light frame).",
						u8" Сейчас здесь работает главный поток игры (толстая светлая рамка).") : "");
				tooltip_text = tooltip;
			}
		}
	}
	ImGui::Dummy(size);
	if (tooltip_text && ImGui::IsWindowHovered())
		wrapped_tooltip(tooltip_text);
}

// "CORE-3", or "CORE-3, thread 2" for the tooltips
void processor_name(string64& out, u32 lp, bool thread = false)
{
	if (lp >= shown.core_count)
		xr_sprintf(out, "CPU %u", lp);
	else if (thread)
		xr_sprintf(out, L("CORE-%u, thread %u", u8"CORE-%u, поток %u"), u32(shown.processors[lp].core), lp);
	else
		xr_sprintf(out, "CORE-%u", u32(shown.processors[lp].core));
}
} // namespace

void threads_frame(bool open)
{
	if (!open)
	{
		was_open = false;
		return;
	}
	if (InterlockedExchange(&sampler_started, TRUE) == FALSE)
		thread_spawn(sampler, "Quark-Panel sampler", 0, nullptr);

	// The main thread runs its jobs rarely: its processor is taken here, once a frame.
	const DWORD core = GetCurrentProcessorNumber();
	InterlockedExchange(&mt_tasks::thread_counters[0].core, LONG(core));
	if (core < 64)
		InterlockedOr64(&mt_tasks::thread_counters[0].cores, LONG64(1ull << core));

	const u64 now = CPU::QPC();
	if (!was_open)
	{
		was_open = true;
		second_start = now;
		take_pool(false); // what was left from the last time the window was open
		take_frame_stats(false);
		return;
	}
	if (now - second_start < CPU::qpc_freq)
		return;
	const double seconds = double(now - second_start) / double(CPU::qpc_freq);
	second_start = now;
	take_pool(true);
	take_frame_stats(true);
	for (pool_thread& thread : pool)
	{
		thread.jobs = u32(thread.jobs / seconds);
	}
	for (u32& jobs : core_jobs)
		jobs = u32(jobs / seconds);

	AcquireSRWLockShared(&sample_lock);
	shown = published;
	ReleaseSRWLockShared(&sample_lock);
}

float process_cores() { return float(double(shown.cycles) / tsc_hz()); }

namespace
{
const char* stage_name(u32 stage)
{
	static const char* const russian[fsStageCount] = {u8"тело кадра", u8"обновление (seqFrame)",
		u8"события, сеть, спавн", u8"объекты (UpdateCL)", u8"HUD", u8"планировщик (главный поток)", u8"скрипты уровня",
		u8"командир физики", u8"подготовка рендера (скелеты)", u8"сборка Lua (до рендера)", u8"запуск задач",
		u8"начало рендера", u8"рендер сцены", u8"расчёт (отсечение)", u8"основной расчёт (HOM)", u8"проход геометрии",
		u8"видимость ламп", u8"каскады солнца", u8"сбор и подготовка", u8"отправка теней", u8"рисование теней",
		u8"накопление", u8"лампы и карты теней", u8"настройка, сортировка, упаковка", u8"обход порталов",
		u8"рисование теней", u8"накопление", u8"сведение и постобработка", u8"показ кадра (vsync/GPU)",
		u8"ожидание логики игры", u8"сборка Lua (после рендера)", u8"сон: пауза, меню, лимит"};
	return L(frame_stage_info(stage).name, russian[stage]);
}

void heading(const char* text)
{
	ImGui::Spacing();
	ImGui::TextUnformatted(text);
}

// A line of the opened thread: indented name, ms a frame, share of the frame
void detail_line(const char* name, u32 depth, float ms, float frame_ms, const char* extra = nullptr)
{
	ImGui::TableNextRow();
	if (row_hidden(ImGui::GetTextLineHeight()))
		return;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.f * float(depth));
	if (depth > 1)
		ImGui::TextDisabled("%s", name);
	else
		ImGui::TextUnformatted(name);
	ImGui::TableSetColumnIndex(1);
	ImGui::Text("%6.2f", ms);
	ImGui::TableSetColumnIndex(2);
	bar(frame_ms > 0.f ? ms / frame_ms : 0.f, 120.f, ImGui::GetTextLineHeight() - 6.f);
	ImGui::SameLine();
	ImGui::Text("%3.0f%%", frame_ms > 0.f ? ms * 100.f / frame_ms : 0.f);
	if (extra)
	{
		ImGui::TableSetColumnIndex(3);
		ImGui::TextDisabled("%s", extra);
	}
}

bool begin_detail_table(const char* id)
{
	if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingFixedFit))
		return false;
	ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 340.f);
	ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.f);
	ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthFixed, 170.f);
	ImGui::TableSetupColumn("extra", ImGuiTableColumnFlags_WidthFixed, 200.f);
	return true;
}

// What an opened thread does
void thread_details(const sampled_thread& thread, int slot, float fps, float frame_ms)
{
	const ImVec2 top = ImGui::GetCursorScreenPos();
	if (lines_of != thread.tid)
	{
		lines_of = thread.tid;
		shown_stages = 0;
		shown_tasks = 0;
	}
	ImGui::Indent(16.f);
	if (slot < 0)
	{
		ImGui::Spacing();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width - 24.f);
		ImGui::TextDisabled("%s", L("Not a thread of the engine's job pool: Windows gives only its CPU cycles. The "
			"name in brackets is the module it started in (a driver, the sound library, Windows).",
			u8"Это не поток пула задач движка: Windows отдаёт только его такты CPU. Имя в скобках - модуль, в котором он "
			u8"стартовал (драйвер, звуковая библиотека, Windows)."));
		ImGui::PopTextWrapPos();
	}
	if (slot == 0)
	{
		heading(L("WHAT THE MAIN THREAD DOES, MS A FRAME", u8"ЧТО ДЕЛАЕТ ГЛАВНЫЙ ПОТОК, МС ЗА КАДР"));
		if (!g_frame_stats.enabled)
			ImGui::TextDisabled("%s", L("frame_stats is off", u8"frame_stats выключен"));
		else if (begin_detail_table("stages"))
		{
			for (u32 stage = fsUpdate; stage < fsStageCount; ++stage)
			{
				if (stage_ms[stage] >= 0.01f)
					shown_stages |= 1ull << stage;
				if (!(shown_stages & (1ull << stage)))
					continue;
				detail_line(stage_name(stage), frame_stage_info(stage).depth, stage_ms[stage], frame_ms);
			}
			ImGui::EndTable();
		}
		if (class_count)
		{
			heading(L("OBJECTS: UpdateCL BY CLASS", u8"ОБЪЕКТЫ: UpdateCL ПО КЛАССАМ"));
			if (begin_detail_table("classes"))
			{
				for (u32 index = 0; index < class_count; ++index)
				{
					string16 name;
					CLSID2TEXT(classes[index].clsid, name);
					string64 calls;
					xr_sprintf(calls, L("%.0f objects a frame", u8"%.0f объектов за кадр"), classes[index].calls);
					detail_line(name, 1, classes[index].ms, frame_ms, calls);
				}
				ImGui::EndTable();
			}
		}
	}
	if (slot >= 0)
	{
		// the pool tasks this thread ran, in a fixed order: the frame job with its parts first
		static_assert(mt_tasks::count <= 32, "shown_tasks is a 32 bit mask");
		const u32 job_parts[] = {mt_tasks::frame_job, mt_tasks::job_lua_prelude, mt_tasks::job_scheduler,
			mt_tasks::job_frame_mt};
		u32 order[mt_tasks::count];
		u32 count = 0;
		for (u32 task = 0; task < mt_tasks::count; ++task)
			if (slot_tasks[slot][task])
				shown_tasks |= 1u << task;
		for (u32 task : job_parts)
			if (shown_tasks & (1u << task))
				order[count++] = task;
		for (u32 task = 0; task < mt_tasks::count; ++task)
			if ((shown_tasks & (1u << task)) && std::find(job_parts, job_parts + 4, task) == job_parts + 4)
				order[count++] = task;
		heading(slot == 0 ? L("POOL TASKS IT RAN ITSELF", u8"ЗАДАЧИ ПУЛА, КОТОРЫЕ ОН ВЫПОЛНИЛ САМ") :
			L("WHAT THIS WORKER RAN, MS A FRAME", u8"ЧТО ВЫПОЛНЯЛ ЭТОТ ВОРКЕР, МС ЗА КАДР"));
		if (!count)
			ImGui::TextDisabled("%s", L("nothing in the last second", u8"ничего за последнюю секунду"));
		else if (begin_detail_table("tasks"))
		{
			const double frames = _max(double(fps), 1.0);
			for (u32 index = 0; index < count; ++index)
			{
				const mt_tasks::task task = mt_tasks::task(order[index]);
				const float ms = float(double(slot_tasks[slot][task]) * 1000.0 / double(CPU::qpc_freq) / frames);
				const bool job_part = task == mt_tasks::job_scheduler || task == mt_tasks::job_lua_prelude ||
					task == mt_tasks::job_frame_mt;
				detail_line(task_name(task), job_part ? 2 : 1, ms, frame_ms,
					task == mt_tasks::frame_job ? L("it hosted the game logic", u8"на нём шла логика игры") : nullptr);
			}
			ImGui::EndTable();
		}
	}
	ImGui::Unindent(16.f);
	ImGui::Spacing();
	const ImVec2 bottom = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(top.x + 4.f, top.y), ImVec2(top.x + 4.f, bottom.y),
		ImGui::GetColorU32(accent), 2.f);
}

bool begin_threads_table(const char* id, bool header)
{
	if (!ImGui::BeginTable(id, 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH))
		return false;
	ImGui::TableSetupColumn(L("THREAD", u8"ПОТОК"), ImGuiTableColumnFlags_WidthFixed, 220.f);
	ImGui::TableSetupColumn(L("CORE", u8"ЯДРО"), ImGuiTableColumnFlags_WidthFixed, 92.f);
	ImGui::TableSetupColumn(L("CYCLES/FRAME", u8"ТАКТЫ/КАДР"), ImGuiTableColumnFlags_WidthFixed, 116.f);
	ImGui::TableSetupColumn(L("CYCLES/S", u8"ТАКТЫ/С"), ImGuiTableColumnFlags_WidthFixed, 92.f);
	ImGui::TableSetupColumn(L("LOAD", u8"НАГРУЗКА"), ImGuiTableColumnFlags_WidthFixed, 84.f);
	ImGui::TableSetupColumn(L("JOBS/S", u8"ЗАДАЧ/С"), ImGuiTableColumnFlags_WidthFixed, 70.f);
	ImGui::TableSetupColumn(L("WAIT", u8"ОЖИДАНИЕ"), ImGuiTableColumnFlags_WidthFixed, 80.f);
	if (!header)
		return true;
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int column = 0; column < 7; ++column)
	{
		ImGui::TableSetColumnIndex(column);
		ImGui::TextDisabled("%s", ImGui::TableGetColumnName(column));
		if (ImGui::IsItemHovered())
		{
			const char* const help[] = {
				L("Name of the thread. Click it to see what it does", u8"Имя потока. Нажмите, чтобы увидеть, что он делает"),
				L("Engine threads: the core where the last job ran (for the main thread: where it is now). Others, "
					"grey: the core Windows prefers for the thread, it may run on any. Hover a cell to see every core of "
					"the last second", u8"Потоки движка: ядро, где выполнялась последняя задача (у главного потока - где "
					u8"он сейчас). Остальные, серым: ядро, которое Windows предпочитает для потока, работать он может на "
					u8"любом. Наведите на ячейку, чтобы увидеть все ядра за секунду"),
				L("CPU cycles a frame, millions", u8"Тактов CPU за кадр, миллионов"),
				L("CPU cycles a second, billions", u8"Тактов CPU в секунду, миллиардов"),
				L("Share of one processor", u8"Доля одного процессора"),
				L("Jobs it ran a second", u8"Задач выполнено за секунду"),
				L("Average time from submit to the start of its jobs, microseconds",
					u8"Среднее время от постановки до старта его задач, микросекунды")};
			wrapped_tooltip(help[column]);
		}
	}
	return true;
}
} // namespace

void threads_tab(float fps, float frame_ms)
{
	ImGui::TextUnformatted(L("CORES", u8"ЯДРА"));
	ImGui::SameLine();
	ImGui::TextDisabled("%s", L("a block is a core, its cells are its threads (SMT)",
		u8"блок - ядро процессора, ячейки в нём - его потоки (SMT)"));
	cores_grid();
	ImGui::Spacing();

	ImGui::TextUnformatted(L("THREADS", u8"ПОТОКИ"));
	ImGui::SameLine();
	ImGui::TextDisabled(L("%u in the process, %.2f cores busy", u8"%u в процессе, занято %.2f ядра"), shown.thread_count,
		process_cores());

	// The main thread is opened the first time: what loads the frame is the first question
	static bool opened_once = false;
	if (!opened_once && mt_tasks::thread_counters[0].tid)
	{
		opened_once = true;
		expanded = DWORD(mt_tasks::thread_counters[0].tid);
	}

	const double hz = tsc_hz();
	// Engine threads first: the main thread and the pool in their order, then the engine's named threads; then the
	// threads of drivers, sound and Windows. In each part the opened thread comes first and the rest the busiest
	// first: a refresh does not move the opened thread.
	const u32 count = sort_threads();
	int key[max_threads];
	for (u32 index = 0; index < count; ++index)
	{
		const sampled_thread& thread = shown.threads[index];
		const int slot = slot_of(thread.tid);
		const int opened = thread.tid == expanded ? 0 : 1;
		key[index] = slot >= 0 ? slot : int(slots) + (engine_thread(thread) ? 0 : 2) + opened;
	}
	std::stable_sort(sorted, sorted + count, [&key](u32 a, u32 b) { return key[a] < key[b]; });
	u32 idle = 0;
	bool in_engine_part = true;
	bool open_table = begin_threads_table("threads", true);
	for (u32 order = 0; order < count && open_table; ++order)
	{
		const sampled_thread& thread = shown.threads[sorted[order]];
		const int slot = slot_of(thread.tid);
		const float load = float(double(thread.cycles) / hz);
		const bool opened = expanded == thread.tid;
		if (slot < 0 && load < 0.001f && !opened)
		{
			++idle;
			continue;
		}
		const bool engine = slot >= 0 || engine_thread(thread);
		if (engine != in_engine_part || order == 0)
		{
			in_engine_part = engine;
			ImGui::TableNextRow();
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(surface));
			ImGui::TableSetColumnIndex(0);
			ImGui::PushStyleColor(ImGuiCol_Text, accent);
			ImGui::TextUnformatted(engine ? L("ENGINE THREADS", u8"ПОТОКИ ДВИЖКА") :
				L("OTHER THREADS", u8"ДРУГИЕ ПОТОКИ"));
			ImGui::PopStyleColor();
		}
		ImGui::TableNextRow();
		if (!opened && row_hidden(ImGui::GetTextLineHeight()))
			continue;
		ImGui::PushStyleColor(ImGuiCol_Text, slot >= 0 ? ink : muted);
		string64 buffer;
		string128 label;
		xr_sprintf(label, "%s %s##%u", opened ? "-" : "+", thread_label(thread, slot, buffer), thread.tid);
		ImGui::TableSetColumnIndex(0);
		if (ImGui::Selectable(label, opened, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			expanded = opened ? 0 : thread.tid;
		if (ImGui::IsItemHovered() && ImGui::TableGetHoveredColumn() == 0)
			ImGui::SetTooltip("TID %u%s%s", thread.tid, thread.name[0] ? "  " : "", thread.name);

		ImGui::TableSetColumnIndex(1);
		string64 where;
		if (slot >= 0 && pool[slot].cores)
		{
			processor_name(where, u32(pool[slot].core));
			ImGui::TextUnformatted(where);
			if (ImGui::IsItemHovered())
			{
				string1024 list;
				xr_strcpy(list, L("Ran on these processors in the last second:", u8"За последнюю секунду работал на:"));
				for (u32 lp = 0; lp < 64; ++lp)
					if ((pool[slot].cores >> lp) & 1)
					{
						processor_name(where, lp, true);
						xr_strcat(list, "\n");
						xr_strcat(list, where);
					}
				wrapped_tooltip(list);
			}
		}
		else if (slot < 0 && thread.ideal >= 0)
		{
			processor_name(where, u32(thread.ideal));
			ImGui::TextDisabled("%s", where);
			if (ImGui::IsItemHovered())
				wrapped_tooltip(L("The ideal processor Windows gave the thread, it may run on any",
					u8"Идеальный процессор, который Windows дала потоку, работать он может на любом"));
		}
		else
			ImGui::TextDisabled("-");
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%7.2f %s", fps > 0.f ? double(thread.cycles) / fps / 1e6 : 0.0, L("M", u8"млн"));
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%.2f %s", double(thread.cycles) / 1e9, L("G", u8"млрд"));
		ImGui::TableSetColumnIndex(4);
		bar(load, 40.f, ImGui::GetTextLineHeight() - 4.f);
		ImGui::SameLine();
		ImGui::Text("%3.0f%%", load * 100.f);
		if (slot >= 0)
		{
			const pool_thread& counted = pool[slot];
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%u", counted.jobs);
			ImGui::TableSetColumnIndex(6);
			if (counted.jobs)
				ImGui::Text("%.1f %s", double(counted.delay) * 1e6 / double(CPU::qpc_freq) / double(counted.jobs),
					L("us", u8"мкс"));
			else
				ImGui::TextDisabled("-");
		}
		ImGui::PopStyleColor();

		// The opened thread: the table stops, its work is listed, the table goes on without a header
		if (opened)
		{
			ImGui::EndTable();
			thread_details(thread, slot, fps, frame_ms);
			open_table = begin_threads_table("threads_after", false);
		}
	}
	if (open_table)
		ImGui::EndTable();
	if (idle)
		ImGui::TextDisabled(L("and %u threads that took less than 0.1%% of a processor",
			u8"и ещё %u потоков, занявших меньше 0.1%% процессора"), idle);
}
} // namespace quark_mt
} // namespace xr_imgui
