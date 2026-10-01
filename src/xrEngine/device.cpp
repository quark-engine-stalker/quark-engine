#include "stdafx.h"
#include "../xrCDB/frustum.h"
#include "xr_ioconsole.h"
#include "xr_input.h"
#include "../xrCore/profiler.h"
#include "../xrCore/engine_error_logger.h"

#pragma warning(disable:4995)
// mmsystem.h
#define MMNOSOUND
#define MMNOMIDI
#define MMNOAUX
#define MMNOMIXER
#define MMNOJOY
#include <mmsystem.h>
// d3dx9.h
#include <d3dx9.h>
#pragma warning(default:4995)

#include "x_ray.h"
#include "Engine.h"
#include "igame_level.h"
#include "discord\discord.h"
#include "render.h"
#include <chrono>

// must be defined before include of FS_impl.h
#define INCLUDE_FROM_ENGINE
#include "../xrCore/FS_impl.h"

#ifdef INGAME_EDITOR
# include "../include/editor/ide.hpp"
# include "engine_impl.hpp"
#endif // #ifdef INGAME_EDITOR

#include "xrSash.h"
#include "igame_persistent.h"

#pragma comment( lib, "d3dx9.lib" )

ENGINE_API CRenderDevice Device;
ENGINE_API CLoadScreenRenderer load_screen_renderer;

// xrCore math types must remain independent of CRenderDevice. Intel LLVM performs
// strict two-phase lookup, so these engine-dependent Fvector/Fmatrix helpers are
// implemented only after CRenderDevice is fully declared.
template <>
_vector3<float>& _vector3<float>::hud_to_world()
{
	Device.hud_to_world(*this);
	return *this;
}

template <>
_vector3<float>& _vector3<float>::world_to_hud()
{
	Device.world_to_hud(*this);
	return *this;
}

template <>
_vector3<float>& _vector3<float>::hud_to_world_dir()
{
	Device.hud_to_world_dir(*this);
	return *this;
}

template <>
_vector3<float>& _vector3<float>::world_to_hud_dir()
{
	Device.world_to_hud_dir(*this);
	return *this;
}

template <>
_vector3<float>& _vector3<float>::ema(_vector3<float>& target, unsigned int steps)
{
	const float smoothing_alpha = 2.0f / (static_cast<float>(steps) + 1.0f);
	const float delta = Device.dwTimeDelta;

	if (steps <= 1 || (fis_zero(x) && fis_zero(y) && fis_zero(z)))
	{
		set(target);
		return *this;
	}

	const float factor = std::min(1.0f, smoothing_alpha * (delta / static_cast<float>(steps)));
	x += factor * (target.x - x);
	y += factor * (target.y - y);
	z += factor * (target.z - z);
	return *this;
}

template <>
_matrix<float>& _matrix<float>::hud_to_world()
{
	Device.hud_to_world(*this);
	return *this;
}

template <>
_matrix<float>& _matrix<float>::world_to_hud()
{
	Device.world_to_hud(*this);
	return *this;
}

ENGINE_API BOOL g_bRendering = FALSE;

BOOL g_bLoaded = FALSE;
ref_light precache_light = 0;

BOOL psLua_ParallelGC = TRUE;
BOOL psLua_ParallelGC_debug = FALSE;
int psLua_ParallelGC_CallAmount = 8;
int psLua_ParallelGC_BudgetUS = 400;
int psLua_ParallelGC_MaxBudgetUS = 1200;
int psLua_ParallelGC_GrowthKB = 16384;

extern discord::Core* discord_core;
extern bool use_discord;

extern Fvector4 ps_ssfx_grass_interactive;

#ifdef ECO_RENDER
namespace
{
using frame_limiter_clock = std::chrono::steady_clock;

class CFrameLimiterWaiter
{
public:
	CFrameLimiterWaiter() : timer_(CreateWaitableTimerW(nullptr, FALSE, nullptr)) {}

	~CFrameLimiterWaiter()
	{
		if (timer_)
			CloseHandle(timer_);
	}

	void WaitUntil(frame_limiter_clock::time_point deadline) const
	{
		constexpr auto spin_guard = std::chrono::microseconds(500);
		auto now = frame_limiter_clock::now();
		const auto blocking_deadline = deadline - spin_guard;

		if (now < blocking_deadline)
		{
			const auto blocking_time = blocking_deadline - now;
			bool waited = false;

			if (timer_)
			{
				const auto ticks_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(blocking_time).count() / 100;
				LARGE_INTEGER due_time{};
				due_time.QuadPart = -static_cast<LONGLONG>(ticks_100ns > 0 ? ticks_100ns : 1);
				if (SetWaitableTimer(timer_, &due_time, 0, nullptr, nullptr, FALSE))
					waited = WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0;
			}

			if (!waited)
			{
				const auto sleep_ms = std::chrono::duration_cast<std::chrono::milliseconds>(blocking_time).count();
				if (sleep_ms > 0)
					Sleep(static_cast<DWORD>(sleep_ms));
			}
		}

		while (frame_limiter_clock::now() < deadline)
			_mm_pause();
	}

private:
	HANDLE timer_ = nullptr;
};

CFrameLimiterWaiter frame_limiter_waiter;
frame_limiter_clock::time_point frame_limiter_last = frame_limiter_clock::now();

void WaitForFrameInterval(float interval_seconds)
{
	if (interval_seconds <= 0.f)
		return;

	const auto interval = std::chrono::duration_cast<frame_limiter_clock::duration>(
		std::chrono::duration<float>(interval_seconds));
	const auto deadline = frame_limiter_last + interval;

	if (deadline > frame_limiter_clock::now())
		frame_limiter_waiter.WaitUntil(deadline);

	frame_limiter_last = frame_limiter_clock::now();
}
} // namespace

ENGINE_API float refresh_rate = 0;
#endif // ECO_RENDER

BOOL CRenderDevice::Begin()
{
	PROF_EVENT();

#ifndef DEDICATED_SERVER
	switch (m_pRender->GetDeviceState())
	{
	case IRenderDeviceRender::dsOK:
		break;

	case IRenderDeviceRender::dsLost:
		// If the device was lost, do not render until we get it back
		Sleep(33);
		return FALSE;
		break;

	case IRenderDeviceRender::dsNeedReset:
		// Check if the device is ready to be reset
		Reset();
		break;

	default:
		R_ASSERT(0);
	}

	m_pRender->Begin();

	FPU::m24r();
	g_bRendering = TRUE;
#endif
	return TRUE;
}

void CRenderDevice::Clear()
{
	m_pRender->Clear();
}

extern void CheckPrivilegySlowdown();

void CRenderDevice::End(void)
{
	PROF_EVENT();

#ifndef DEDICATED_SERVER

#ifdef INGAME_EDITOR
    bool load_finished = false;
#endif // #ifdef INGAME_EDITOR
	if (dwPrecacheFrame)
	{
		::Sound->set_master_volume(0.f);
		dwPrecacheFrame--;

		if (!dwPrecacheFrame)
		{
#ifdef INGAME_EDITOR
            load_finished = true;
#endif // #ifdef INGAME_EDITOR

			m_pRender->updateGamma();

			if (precache_light)
			{
				precache_light->set_active(false);
				precache_light.destroy();
			}
			::Sound->set_master_volume(1.f);

			m_pRender->ResourcesDestroyNecessaryTextures();

			Msg("* [x-ray]: Handled Necessary Textures Destruction");
			if (IsLoadDiagnosticsEnabled())
			{
				Memory.mem_compact();
				Msg("* MEMORY USAGE: %lld K", Memory.mem_usage() / 1024);
			}
			Msg("* End of synchronization A[%d] R[%d]", b_is_Active, b_is_Ready);

#ifdef FIND_CHUNK_BENCHMARK_ENABLE
            g_find_chunk_counter.flush();
#endif // FIND_CHUNK_BENCHMARK_ENABLE

			CheckPrivilegySlowdown();

			if (g_pGamePersistent->GameType() == 1) //haCk
			{
				WINDOWINFO wi;
				GetWindowInfo(m_hWnd, &wi);
				if (wi.dwWindowStatus != WS_ACTIVECAPTION)
					Pause(TRUE, TRUE, TRUE, "application start");
			}
		}
	}

	g_bRendering = FALSE;
	// end scene
	// Present goes here, so call OA Frame end.
	if (g_SASH.IsBenchmarkRunning())
		g_SASH.DisplayFrame(Device.fTimeGlobal);
	m_pRender->End();

# ifdef INGAME_EDITOR
    if (load_finished && m_editor)
        m_editor->on_load_finished();
# endif // #ifdef INGAME_EDITOR
#endif
}

namespace
{
u32 qpc_ticks_to_us(const u64 ticks)
{
	if (!CPU::qpc_freq)
		return 0;

	return static_cast<u32>((ticks * 1000000ull) / CPU::qpc_freq);
}

void process_frame_parallel_job(void* context)
{
	static_cast<CRenderDevice*>(context)->ProcessFrameParallelWork();
}

struct SParallelDelegateBatch
{
	xr_vector<fastdelegate::FastDelegate0<>>* delegates;
	xrCriticalSection* lock;
	// Workers update the cursor for every claimed callback. Keep it away from
	// read-only batch metadata to avoid invalidating that cache line on every claim.
	__declspec(align(64)) volatile LONG next_index;
};

void process_parallel_delegate_batch(void* raw_context)
{
	SParallelDelegateBatch& context = *static_cast<SParallelDelegateBatch*>(raw_context);
	for (;;)
	{
		const LONG index = InterlockedIncrement(&context.next_index) - 1;
		fastdelegate::FastDelegate0<> callback;
		{
			xrCriticalSectionGuard guard(*context.lock);
			if (index < 0 || static_cast<u32>(index) >= context.delegates->size())
				return;
			callback = (*context.delegates)[index];
		}

		// A lifecycle removal may cancel an item that has not started yet.
		if (callback)
		{
			callback();
		}
	}
}

struct __declspec(align(64)) SNamedParallelLane
{
    CRenderDevice* device;
    EFrameParallelLane lane;
};

void process_named_parallel_lane(void* raw_context)
{
	SNamedParallelLane& context = *static_cast<SNamedParallelLane*>(raw_context);

	for (u32 index = 0;; ++index)
	{
		fastdelegate::FastDelegate0<> callback;

		{
			xrCriticalSectionGuard guard(context.device->seqParallelLock);
			if (index >= context.device->seqParallelWork.size())
				break;
			if (index >= context.device->seqParallelWorkLanes.size() ||
				context.device->seqParallelWorkLanes[index] != context.lane)
				continue;

			callback = context.device->seqParallelWork[index];
		}

		if (!callback)
			continue;

		callback();

	}

}
}

void CRenderDevice::ProcessFrameParallelWork()
{

	// RT callbacks were completed on the main thread during FrameMove. The normal
	// scheduler retains its deterministic serial order, but overlaps rendering.
	if (InterlockedExchange(&mt_scheduler_deferred_pending, FALSE) != FALSE)
	{

		Engine.Sheduler.UpdateDeferred();
		Engine.Sheduler.UpdateFinalize();

	}

	{
		xrCriticalSectionGuard guard(seqParallelIndependentLock);
		R_ASSERT(seqParallelIndependentWork.empty());
		if (!seqParallelIndependent.empty())
			seqParallelIndependentWork.swap(seqParallelIndependent);
	}

	if (!seqParallelIndependentWork.empty())
	{
		SParallelDelegateBatch context = {
			&seqParallelIndependentWork, &seqParallelIndependentLock,
			0};
		xr_jobs::task_group independent_group;
		const u32 lane_count = _max(1u, _min(
			static_cast<u32>(seqParallelIndependentWork.size()), xr_jobs::available_thread_count()));
		xr_jobs::submit_many(
			&process_parallel_delegate_batch,
			&context,
			lane_count - 1,
			&independent_group,
			xr_jobs::priority::high);

		process_parallel_delegate_batch(&context);
		xr_jobs::wait(independent_group);

		xrCriticalSectionGuard guard(seqParallelIndependentLock);
		seqParallelIndependentWork.clear_not_free();
		// Keep the largest allocation as the next-frame queued container unless
		// callbacks were registered while this batch was executing.
		if (seqParallelIndependent.empty())
			seqParallelIndependentWork.swap(seqParallelIndependent);
	}

	START_PROFILE("Process seqParallel lanes");

	{
		xrCriticalSectionGuard guard(seqParallelLock);
		R_ASSERT(seqParallelWork.empty());
		R_ASSERT(seqParallelWorkNames.empty());
		R_ASSERT(seqParallelWorkLanes.empty());
		R_ASSERT(seqParallel.size() == seqParallelNames.size());
		R_ASSERT(seqParallel.size() == seqParallelLanes.size());
		seqParallelWork.swap(seqParallel);
		seqParallelWorkNames.swap(seqParallelNames);
		seqParallelWorkLanes.swap(seqParallelLanes);

	}

	SNamedParallelLane gameplay = {this, EFrameParallelLane::Gameplay};
	SNamedParallelLane audio = {this, EFrameParallelLane::Audio};
	SNamedParallelLane simulation = {this, EFrameParallelLane::Simulation};
	xr_jobs::task_group lane_group;

	bool has_audio = false;
	bool has_simulation = false;
	{
		xrCriticalSectionGuard guard(seqParallelLock);
		for (u32 index = 0; index < seqParallelWorkLanes.size(); ++index)
		{
			has_audio = has_audio || seqParallelWorkLanes[index] == EFrameParallelLane::Audio;
			has_simulation = has_simulation || seqParallelWorkLanes[index] == EFrameParallelLane::Simulation;
		}
	}

	if (!has_audio && !has_simulation)
	{
		// Compatibility fast path: all legacy callbacks keep their exact order and
		// incur no per-callback lock or nested job overhead.

		for (u32 index = 0; index < seqParallelWork.size(); ++index)
		{
			fastdelegate::FastDelegate0<> callback = seqParallelWork[index];
			if (!callback)
				continue;

			callback();

		}
	}
	else
	{
		if (xr_jobs::worker_count() <= 1)
		{
			process_named_parallel_lane(&gameplay);
			if (has_audio)
				process_named_parallel_lane(&audio);
			if (has_simulation)
				process_named_parallel_lane(&simulation);
		}
		else
		{
			if (has_audio && !xr_jobs::submit(&process_named_parallel_lane, &audio, &lane_group, xr_jobs::priority::high))
				process_named_parallel_lane(&audio);
			if (has_simulation && !xr_jobs::submit(&process_named_parallel_lane, &simulation, &lane_group, xr_jobs::priority::high))
				process_named_parallel_lane(&simulation);

			process_named_parallel_lane(&gameplay);
			xr_jobs::wait(lane_group);
		}
	}

	{
		xrCriticalSectionGuard guard(seqParallelLock);
		seqParallelWork.clear_not_free();
		seqParallelWorkNames.clear_not_free();
		seqParallelWorkLanes.clear_not_free();
		if (seqParallel.empty())
		{
			seqParallelWork.swap(seqParallel);
			seqParallelWorkNames.swap(seqParallelNames);
			seqParallelWorkLanes.swap(seqParallelLanes);
		}
	}
	STOP_PROFILE;

	START_PROFILE("Process seqFrameMT");

	seqFrameMT.Process(rp_Frame);

	STOP_PROFILE;

}

void CRenderDevice::ProcessRenderPrepParallelWork()
{
	{
		xrCriticalSectionGuard guard(seqRenderPrepParallelLock);
		R_ASSERT(seqRenderPrepParallelWork.empty());
		R_ASSERT(seqRenderPrepParallelWorkUnits.empty());
		R_ASSERT(seqRenderPrepParallel.size() == seqRenderPrepParallelUnits.size());
		if (!seqRenderPrepParallel.empty())
		{
			seqRenderPrepParallelWork.swap(seqRenderPrepParallel);
			seqRenderPrepParallelWorkUnits.swap(seqRenderPrepParallelUnits);
		}
	}

	if (seqRenderPrepParallelWork.empty())
		return;

	u32 active_count = 0;
	u32 work_units = 0;
	{
		xrCriticalSectionGuard guard(seqRenderPrepParallelLock);
		R_ASSERT(seqRenderPrepParallelWork.size() == seqRenderPrepParallelWorkUnits.size());
		for (u32 index = 0; index < seqRenderPrepParallelWork.size(); ++index)
		{
			if (!seqRenderPrepParallelWork[index])
				continue;
			++active_count;
			work_units += seqRenderPrepParallelWorkUnits[index];
		}
	}

	// Four medium skeletons are enough to amortize one batched wake-up and
	// barrier. Below either threshold, leave calculation to the renderer's lazy
	// path: this avoids turning a light frame into synchronization overhead.
	constexpr u32 min_parallel_callbacks = 4;
	constexpr u32 min_parallel_work_units = 192;
	if (xr_jobs::worker_count() == 0 || active_count < min_parallel_callbacks ||
		work_units < min_parallel_work_units)
	{
		xrCriticalSectionGuard guard(seqRenderPrepParallelLock);
		seqRenderPrepParallelWork.clear_not_free();
		seqRenderPrepParallelWorkUnits.clear_not_free();
		if (seqRenderPrepParallel.empty())
		{
			seqRenderPrepParallelWork.swap(seqRenderPrepParallel);
			seqRenderPrepParallelWorkUnits.swap(seqRenderPrepParallelUnits);
		}
		return;
	}

	SParallelDelegateBatch context = {
		&seqRenderPrepParallelWork, &seqRenderPrepParallelLock,
		0};
	xr_jobs::task_group group;
	const u32 lane_count = _max(1u, _min(
		active_count, xr_jobs::available_thread_count()));
	xr_jobs::submit_many(
		&process_parallel_delegate_batch,
		&context,
		lane_count - 1,
		&group,
		xr_jobs::priority::high);
	process_parallel_delegate_batch(&context);
	xr_jobs::wait(group);

	xrCriticalSectionGuard guard(seqRenderPrepParallelLock);
	seqRenderPrepParallelWork.clear_not_free();
	seqRenderPrepParallelWorkUnits.clear_not_free();
	// Preserve the allocation for the next frame. Registrations made during the
	// batch stay queued and are intentionally not pulled into the active batch.
	if (seqRenderPrepParallel.empty())
	{
		seqRenderPrepParallelWork.swap(seqRenderPrepParallel);
		seqRenderPrepParallelWorkUnits.swap(seqRenderPrepParallelUnits);
	}
}

void CRenderDevice::UpdateLuaGCCycleFeedback(u32 memory_after_kb)
{
	LuaGCCycleReclaimedKB = lua_gc_cycle_start_memory_kb > memory_after_kb ?
		lua_gc_cycle_start_memory_kb - memory_after_kb : 0u;
	LuaGCReclaimPercent = lua_gc_cycle_growth_kb ? static_cast<u32>(_min<u64>(
		static_cast<u64>(LuaGCCycleReclaimedKB) * 100ull / lua_gc_cycle_growth_kb, 100ull)) : 0u;

	// A cycle which collected most of the bytes allocated since the previous one
	// is useful and may run somewhat sooner. A mostly-live cycle is expensive
	// bookkeeping, so widen its next growth window. Two separated thresholds keep
	// the feedback stable instead of oscillating around a single percentage.
	if (lua_gc_cycle_growth_kb >= 1024u)
	{
		if (LuaGCReclaimPercent >= 75u && lua_gc_growth_scale > 2u)
			--lua_gc_growth_scale;
		else if (LuaGCReclaimPercent <= 25u && lua_gc_growth_scale < 8u)
			++lua_gc_growth_scale;
	}

	LuaGCGrowthScale = lua_gc_growth_scale;
	lua_gc_cycle_start_memory_kb = 0;
	lua_gc_cycle_growth_kb = 0;
}

void CRenderDevice::ProcessLuaGCWork(bool allow_deferred_phase)
{
	if (!psLua_ParallelGC || !LuaGC)
		return;

	SLuaGCStatus native_gc_status = {};
	if (LuaGCStatus)
		LuaGCStatus(&native_gc_status);
	const u32 current_memory_kb = LuaGCStatus ? native_gc_status.total_kb :
		(LuaGCMemory ? LuaGCMemory() : 0);
	const u64 current_udata_serial = native_gc_status.udata_alloc_serial;
	LuaGCMemoryKB = current_memory_kb;

	LuaGCNativeState = native_gc_status.state;
	if (LuaGCNativeState != 2u)
		LuaGCAtomicPrepared = false;
	LuaGCDebtKB = native_gc_status.debt_kb;
	LuaGCThresholdKB = native_gc_status.threshold_kb;
	LuaGCPoolReservedKB = native_gc_status.pool_reserved_kb;
	LuaGCPoolCommittedKB = native_gc_status.pool_committed_kb;
	LuaGCPoolFallbackKB = native_gc_status.pool_fallback_kb;
	LuaGCPoolAllocationFailures = native_gc_status.pool_allocation_failures;
	LuaGCNativeCycleActive = native_gc_status.cycle_active;

	if (!lua_gc_baseline_kb)
	{
		// The first pass normally follows level loading and, in GAMMA, an explicit
		// full collect performed by a loading-screen callback. Treat the observed
		// live heap as a baseline. Starting a forced cycle here immediately scans
		// the same object graph again and turns the first seconds of gameplay into
		// a chain of waits on the GC worker.
		lua_gc_baseline_kb = current_memory_kb;
		lua_gc_last_memory_kb = current_memory_kb;
		lua_gc_udata_baseline_serial = current_udata_serial;
		LuaGCUdataAllocatedSinceCycle = 0;
		LuaGCCycleActive = false;
		LuaGCAssistingNativeCycle = false;
		lua_gc_cycle_started = false;
		lua_gc_cycle_start_frame = 0;
		LuaGCCycleAgeFrames = 0;
		LuaGCBudgetUS = 0;
		LuaGCBudgetUsedUS = 0;
		LuaGCStepKB = 0;
		LuaGCStepMaxUS = 0;
		return;
	}
	else if (current_memory_kb < lua_gc_baseline_kb)
	{
		// A script-requested collection may finish between worker passes. If no
		// native/host cycle is active, it has also consumed the userdata population
		// accumulated before this point, so begin pressure accounting from here.
		lua_gc_baseline_kb = current_memory_kb;
		if (!LuaGCCycleActive && !LuaGCNativeCycleActive)
		{
			lua_gc_udata_baseline_serial = current_udata_serial;
			LuaGCUdataAllocatedSinceCycle = 0;
		}
	}

	// Userdata-heavy GAMMA workloads can create hundreds of thousands of small
	// finalizable wrappers while heap growth is still only a few dozen MiB. Waiting
	// for the byte threshold therefore allows an indivisible atomic commit to grow
	// into a 30-70 ms spike. Start the *normal* LuaJIT cycle earlier based on a
	// monotonic allocation counter; atomic itself remains stock-order and indivisible.
	// The current GAMMA hitch trace reaches a 3.8-4.8 ms indivisible atomic phase
	// already around 46K-61K visited userdata. Start a host cycle at 24K so the mark
	// phase has more headroom, while keeping the existing 32K active-cycle pressure
	// threshold to avoid spending the 1.8 ms boosted budget earlier on every cycle.
	constexpr u64 udata_cycle_trigger = 24ull * 1024ull;
	constexpr u64 udata_active_pressure_trigger = 32ull * 1024ull;
	constexpr u64 udata_hard_pressure = 96ull * 1024ull;
	if (current_udata_serial < lua_gc_udata_baseline_serial)
		lua_gc_udata_baseline_serial = current_udata_serial;
	const u64 udata_allocated_since_cycle = current_udata_serial - lua_gc_udata_baseline_serial;
	LuaGCUdataAllocatedSinceCycle = static_cast<u32>(_min<u64>(udata_allocated_since_cycle, u32(-1)));
	const bool start_cycle_udata_pressure = udata_allocated_since_cycle >= udata_cycle_trigger;
	const bool active_cycle_udata_pressure = udata_allocated_since_cycle >= udata_active_pressure_trigger;
	const bool critical_udata_pressure = udata_allocated_since_cycle >= udata_hard_pressure;

	const u32 configured_growth_kb = static_cast<u32>(_max(psLua_ParallelGC_GrowthKB, 1));
	// An engine-requested exact step bypasses LuaJIT's normal post-cycle pause. The
	// old 16 MB / 25% rule consequently launched a complete major cycle every few
	// seconds in GAMMA, repeatedly scanning a mostly-live object graph and reducing
	// FPS on small levels. Completed cycles move between 32..128 MB and 25..100%
	// according to measured reclaim yield. The upper bound matches LuaJIT's stock
	// 200% post-cycle pause, so the host scheduler no longer pre-empts it on a
	// mostly-live heap. LuaJIT's own allocation-driven cycle is still assisted
	// immediately, and the pool guard below can always tighten the threshold.
	const u32 growth_scale = _min(_max(lua_gc_growth_scale, 2u), 8u);
	LuaGCGrowthScale = growth_scale;
	const u32 amortized_growth_kb = configured_growth_kb <= u32(-1) / growth_scale ?
		configured_growth_kb * growth_scale : u32(-1);
	const u32 proportional_growth_kb = static_cast<u32>(_min<u64>(
		static_cast<u64>(lua_gc_baseline_kb) * growth_scale / 8ull, u32(-1)));
	u32 soft_growth_kb = _max(amortized_growth_kb, proportional_growth_kb);

	// A proportional 100% growth window is appropriate for a small, mostly-live
	// heap, but becomes unsafe inside LuaJIT 2.0's fixed low-address pool: a burst
	// can add close to a gigabyte before the next cycle even starts. Never leave
	// more than one eighth of the actually reserved pool between major cycles.
	// Small heaps retain the adaptive 32..128 MiB behaviour, so this cap has no
	// cost on the normal Anomaly/GAMMA gameplay footprint.
	if (LuaGCPoolReservedKB)
	{
		const u32 pool_growth_cap_kb = _max(configured_growth_kb, LuaGCPoolReservedKB / 8u);
		soft_growth_kb = _min(soft_growth_kb, pool_growth_cap_kb);

		// Do not trade fewer collections for an out-of-memory failure on
		// installations where the pool had to reserve less than requested.
		const u32 pool_guard_kb = static_cast<u32>(
			static_cast<u64>(LuaGCPoolReservedKB) * 7ull / 10ull);
		const u32 capacity_growth_kb = pool_guard_kb > lua_gc_baseline_kb ?
			pool_guard_kb - lua_gc_baseline_kb : 0u;
		const u32 capacity_limited_growth_kb = _max(configured_growth_kb, capacity_growth_kb);
		soft_growth_kb = _min(soft_growth_kb, capacity_limited_growth_kb);
	}
	LuaGCTriggerGrowthKB = soft_growth_kb;
	const u32 growth_kb = current_memory_kb > lua_gc_baseline_kb ?
		current_memory_kb - lua_gc_baseline_kb : 0;
	LuaGCMemoryGrowthKB = growth_kb;

	LuaGCCount = 0;
	LuaGCDone = false;
	LuaGCBudgetHit = false;
	LuaGCCallCapHit = false;
	LuaGCBudgetUtilPercent = 0;
	LuaGCProgressKB = 0;
	LuaGCCycleAgeFrames = LuaGCCycleActive && lua_gc_cycle_start_frame && Device.dwFrame >= lua_gc_cycle_start_frame ?
		Device.dwFrame - lua_gc_cycle_start_frame + 1u : 0u;
	LuaGCEmergencyStep = false;

	// Normal Lua allocations can complete an incremental cycle between two
	// maintenance passes. Once a cycle has actually started, a return to the
	// native pause state means there is nothing left to assist. Do not issue a
	// fresh host step here, since that would immediately start another cycle.
	if (LuaGCCycleActive && lua_gc_cycle_started && !LuaGCNativeCycleActive)
	{
		UpdateLuaGCCycleFeedback(current_memory_kb);
		LuaGCCycleActive = false;
		LuaGCAssistingNativeCycle = false;
		lua_gc_cycle_started = false;
		lua_gc_cycle_start_frame = 0;
		LuaGCCycleAgeFrames = 0;
		lua_gc_baseline_kb = current_memory_kb;
		lua_gc_last_memory_kb = current_memory_kb;
		// Keep the baseline from the start of the completed cycle. Allocations made
		// while that cycle was running are candidates for the *next* cycle and must
		// not be forgotten here.
		LuaGCUdataAllocatedSinceCycle = static_cast<u32>(_min<u64>(
			current_udata_serial >= lua_gc_udata_baseline_serial ?
			current_udata_serial - lua_gc_udata_baseline_serial : 0ull, u32(-1)));
		lua_gc_budget_hit_streak = 0;
		LuaGCBudgetUS = 0;
		LuaGCBudgetUsedUS = 0;
		LuaGCStepKB = 0;
		LuaGCStepMaxUS = 0;
		return;
	}

	const bool logical_critical_pool_pressure = LuaGCPoolReservedKB &&
		static_cast<u64>(current_memory_kb) * 10ull >=
		static_cast<u64>(LuaGCPoolReservedKB) * 7ull;
	const bool logical_severe_pool_pressure = LuaGCPoolReservedKB &&
		static_cast<u64>(current_memory_kb) * 5ull >=
		static_cast<u64>(LuaGCPoolReservedKB) * 4ull;
	const bool allocator_critical_pool_pressure = LuaGCPoolReservedKB &&
		static_cast<u64>(LuaGCPoolCommittedKB) * 10ull >=
		static_cast<u64>(LuaGCPoolReservedKB) * 7ull;
	const bool allocator_severe_pool_pressure = LuaGCPoolReservedKB &&
		static_cast<u64>(LuaGCPoolCommittedKB) * 5ull >=
		static_cast<u64>(LuaGCPoolReservedKB) * 4ull;
	const bool allocator_fallback_pressure = LuaGCPoolFallbackKB >=
		_max(configured_growth_kb, 64u * 1024u);
	const bool allocator_failure_pressure = LuaGCPoolAllocationFailures != 0;
	const bool critical_pool_pressure = logical_critical_pool_pressure ||
		allocator_critical_pool_pressure || allocator_fallback_pressure || allocator_failure_pressure;
	const bool severe_pool_pressure = logical_severe_pool_pressure ||
		allocator_severe_pool_pressure || allocator_fallback_pressure || allocator_failure_pressure;
	// Committed allocator pressure raises urgency for an active cycle, but does not
	// by itself start a new major cycle every frame if dlmalloc cannot return whole
	// segments. Fallback use/failures are stronger fragmentation signals and may
	// force a cycle even when the logical Lua heap itself is below 80% of the pool.
	const bool force_cycle_pool_pressure = logical_severe_pool_pressure ||
		allocator_fallback_pressure || allocator_failure_pressure;
	const bool critical_memory_pressure = critical_pool_pressure || critical_udata_pressure ||
		static_cast<u64>(growth_kb) >= static_cast<u64>(soft_growth_kb) * 4ull;
	// Userdata churn is a reason to spend more bounded marking time, not a reason
	// to skip the pre-atomic smoother. The latter makes the most difference on the
	// wrapper-heavy GAMMA heap. Reserve the emergency completion path for actual
	// low-address/allocator pressure or extreme byte growth, where OOM safety must
	// take precedence over frame pacing.
	const bool emergency_completion_pressure = critical_pool_pressure ||
		static_cast<u64>(growth_kb) >= static_cast<u64>(soft_growth_kb) * 4ull;

	// If a previous post-frame GC slice took too long, skip a small number of
	// maintenance frames. This prevents repeated long waits on an indivisible
	// LuaJIT atomic phase. Critical heap growth bypasses backoff.
	const bool lua_gc_deferred_pending = LuaGCNativeState == 2u || LuaGCNativeState == 5u;
	if (LuaGCBackoffFrames && !critical_memory_pressure && !lua_gc_deferred_pending)
	{
		--LuaGCBackoffFrames;
		LuaGCBudgetUS = 0;
		LuaGCBudgetUsedUS = 0;
		LuaGCStepKB = 0;
		LuaGCStepMaxUS = 0;
		lua_gc_last_memory_kb = current_memory_kb;
		return;
	}

	// An explicit exact step starts a cycle even while the collector is still in
	// GCSpause, so never create maintenance cycles on a stable heap. If LuaJIT
	// has already entered an allocation-driven cycle, however, advance that same
	// cycle during the serialized idle window instead of leaving its atomic/sweep
	// tail for an arbitrary gameplay allocation. Heap growth still controls when
	// QUARK is allowed to start a cycle of its own.
	if (!LuaGCCycleActive)
	{
		if (LuaGCNativeCycleActive)
		{
			LuaGCCycleActive = true;
			LuaGCAssistingNativeCycle = true;
			lua_gc_cycle_started = true;
			lua_gc_cycle_start_frame = Device.dwFrame;
			LuaGCCycleAgeFrames = 1u;
			lua_gc_cycle_start_memory_kb = current_memory_kb;
			lua_gc_cycle_growth_kb = growth_kb;
			// Count userdata allocated while this native cycle is running. Those objects
			// are pressure for the following cycle, not the one already in progress.
			lua_gc_udata_baseline_serial = current_udata_serial;
			LuaGCUdataAllocatedSinceCycle = 0;
		}
		else if (growth_kb >= soft_growth_kb || force_cycle_pool_pressure || start_cycle_udata_pressure)
		{
			LuaGCCycleActive = true;
			LuaGCAssistingNativeCycle = false;
			lua_gc_cycle_started = false;
			lua_gc_cycle_start_frame = Device.dwFrame;
			LuaGCCycleAgeFrames = 1u;
			lua_gc_cycle_start_memory_kb = current_memory_kb;
			lua_gc_cycle_growth_kb = growth_kb;
			// The current accumulated wrappers triggered (or accompany) this cycle.
			// From now on track only allocations made during it, so they can trigger a
			// prompt follow-up cycle instead of being discarded at completion.
			lua_gc_udata_baseline_serial = current_udata_serial;
			LuaGCUdataAllocatedSinceCycle = 0;
		}
		else
		{
			LuaGCAssistingNativeCycle = false;
			LuaGCBudgetUS = 0;
			LuaGCBudgetUsedUS = 0;
			LuaGCStepKB = 0;
			LuaGCStepMaxUS = 0;
			lua_gc_last_memory_kb = current_memory_kb;
			return;
		}
	}

	// State 2 is LuaJIT's GCSatomic boundary and state 5 runs arbitrary userdata
	// finalizers. Engine-only and allocation-driven steps yield before both. Keep
	// the pre-render pass bounded; the caller resumes after Present and after all
	// frame jobs, where callbacks are on the main thread and cannot race Lua.
	constexpr u32 lua_gc_atomic_state = 2u;
	constexpr u32 lua_gc_finalize_state = 5u;
	const bool resuming_deferred_phase = allow_deferred_phase &&
		(LuaGCNativeState == lua_gc_atomic_state || LuaGCNativeState == lua_gc_finalize_state);
	if (!allow_deferred_phase &&
		(LuaGCNativeState == lua_gc_atomic_state || LuaGCNativeState == lua_gc_finalize_state))
	{
		LuaGCBudgetUS = 0;
		LuaGCBudgetUsedUS = 0;
		LuaGCStepKB = 0;
		LuaGCStepMaxUS = 0;
		lua_gc_last_memory_kb = current_memory_kb;
		return;
	}

	// Both scheduling points are serialized: the bounded pass runs before worker
	// submission; atomic and finalizers run only after those workers finish. This
	// preserves exclusive ownership of the single LuaJIT state. Critical pressure
	// still bypasses backoff.
	R_ASSERT(InterlockedCompareExchange(&isRendering, FALSE, FALSE) == FALSE);
	LuaGCEmergencyStep = emergency_completion_pressure;

	// Long atomic phases are indivisible and do not describe the cost of a bounded
	// propagate/sweep quantum. Never let an old atomic hitch throttle collector
	// throughput while the fixed low-address pool is under pressure.
	const u32 wait_budget_divisor = critical_memory_pressure ? 1u :
		1u + _min(LuaGCWaitStreak, 3u);
	const u32 configured_base_budget_us = static_cast<u32>(_max(psLua_ParallelGC_BudgetUS, 1));
	const u32 configured_max_budget_us = static_cast<u32>(
		_max(psLua_ParallelGC_MaxBudgetUS, psLua_ParallelGC_BudgetUS));
	const u32 base_budget_us = _max(configured_base_budget_us / wait_budget_divisor, 50u);
	u32 pressure_max_budget_us = severe_pool_pressure ?
		_max(configured_max_budget_us, 2400u) : configured_max_budget_us;
	// When wrapper churn is outrunning an active mark phase, spend a little more
	// incremental time now rather than paying tens of milliseconds in atomic later.
	// This work is still budgeted and never makes an individual GC step unbounded.
	if (active_cycle_udata_pressure)
		pressure_max_budget_us = _max(pressure_max_budget_us, critical_udata_pressure ? 2400u : 1800u);
	const u32 max_budget_us = _max(pressure_max_budget_us / wait_budget_divisor, base_budget_us);

	u32 pressure_level = soft_growth_kb ? _min(growth_kb / soft_growth_kb, 3u) : 0u;
	if (udata_allocated_since_cycle >= udata_hard_pressure * 2ull)
		pressure_level = _max(pressure_level, 4u);
	else if (critical_udata_pressure)
		pressure_level = _max(pressure_level, 3u);
	else if (active_cycle_udata_pressure)
		pressure_level = _max(pressure_level, 2u);
	if (LuaGCPoolReservedKB)
	{
		const u64 pool_scaled = static_cast<u64>(LuaGCPoolReservedKB) * 10ull;
		const u64 logical_scaled = static_cast<u64>(current_memory_kb) * 10ull;
		const u64 committed_scaled = static_cast<u64>(LuaGCPoolCommittedKB) * 10ull;
		const u64 pressure_scaled = _max(logical_scaled, committed_scaled);
		if (pressure_scaled >= pool_scaled * 8ull / 10ull)
			pressure_level = _max(pressure_level, 3u);
		else if (pressure_scaled >= pool_scaled * 7ull / 10ull)
			pressure_level = _max(pressure_level, 2u);
		else if (pressure_scaled >= pool_scaled * 6ull / 10ull)
			pressure_level = _max(pressure_level, 1u);
	}
	if (allocator_fallback_pressure)
		pressure_level = _max(pressure_level, 2u);
	if (allocator_failure_pressure)
		pressure_level = _max(pressure_level, 4u);
	if (current_memory_kb > lua_gc_last_memory_kb &&
		current_memory_kb - lua_gc_last_memory_kb > _max(soft_growth_kb / 4u, 1u))
	{
		pressure_level = _min(pressure_level + 1u, 3u);
	}

	// gc.debt is LuaJIT's own estimate of collector work that fell behind
	// allocation pressure. Use it only to scale the budget of an already active
	// cycle; debt never starts a new maintenance cycle by itself.
	if (LuaGCNativeCycleActive && LuaGCDebtKB)
	{
		// psLua_ParallelGCStep is owned by xrGame and must not be referenced from
		// xrEngine. Use the device-owned requested/adaptive step as the debt scale;
		// CLevel::LuaGC() still applies the configured xrGame headroom cap before the
		// actual exact-quantum call.
		const u32 debt_reference_step_kb = _max(
			LuaGCStepKB ? LuaGCStepKB : (lua_gc_adaptive_step_kb ? lua_gc_adaptive_step_kb : 100u), 1u);
		const u32 debt_quantum_kb = _max(debt_reference_step_kb * 4u, 1u);
		const u32 debt_pressure = _min(LuaGCDebtKB / debt_quantum_kb + 1u, 2u);
		pressure_level = _min(pressure_level + debt_pressure, 4u);
	}

	// A healthy incremental cycle should finish in hundreds of frames, not tens of
	// thousands. Cycle age is therefore an independent catch-up signal. It only
	// increases work for a cycle which is already active and never starts a new
	// collection on its own. This is especially important for LuaJIT finalize and
	// tiny sweep states where one exact host step may cost only a few hundred ns.
	if (LuaGCCycleAgeFrames >= 1800u)
		pressure_level = _max(pressure_level, 4u);
	else if (LuaGCCycleAgeFrames >= 600u)
		pressure_level = _max(pressure_level, 3u);
	else if (LuaGCCycleAgeFrames >= 300u)
		pressure_level = _max(pressure_level, 2u);
	else if (LuaGCCycleAgeFrames >= 120u)
		pressure_level = _max(pressure_level, 1u);

	pressure_level = _min(pressure_level + _min(lua_gc_budget_hit_streak / 4u, 2u), 4u);
	const u64 scaled_budget_us = static_cast<u64>(base_budget_us) * static_cast<u64>(pressure_level + 1u);
	LuaGCBudgetUS = static_cast<u32>(_min<u64>(scaled_budget_us, max_budget_us));
	const u64 budget_ticks = CPU::qpc_freq ?
		(static_cast<u64>(LuaGCBudgetUS) * CPU::qpc_freq + 999999ull) / 1000000ull : 0;

	// The exact-quantum API makes time the primary throttle. Keep a meaningful
	// minimum quantum so a cheap 1-2 KiB sweep cannot consume the call cap while
	// leaving virtually the whole frame budget unused. The controller may grow up
	// to 1 MiB (the LuaJIT-side API hard cap), but every quantum is still timed.
	constexpr u32 min_adaptive_step_kb = 8u;
	constexpr u32 max_adaptive_step_kb = 1024u;
	if (!lua_gc_adaptive_step_kb)
		lua_gc_adaptive_step_kb = _max(LuaGCStepKB ? LuaGCStepKB : 64u, min_adaptive_step_kb);

	const u32 target_step_us = _max(LuaGCBudgetUS / 8u, 25u);
	if (lua_gc_last_step_us > target_step_us + target_step_us / 2u &&
		lua_gc_adaptive_step_kb > min_adaptive_step_kb)
	{
		const u64 scaled_step = static_cast<u64>(lua_gc_adaptive_step_kb) * target_step_us /
			_max(lua_gc_last_step_us, 1u);
		lua_gc_adaptive_step_kb = clampr(static_cast<u32>(scaled_step),
			min_adaptive_step_kb, max_adaptive_step_kb);
	}
	else if (lua_gc_last_step_us && lua_gc_last_step_us < _max(target_step_us / 4u, 1u))
	{
		lua_gc_adaptive_step_kb = _min(lua_gc_adaptive_step_kb * 2u, max_adaptive_step_kb);
	}
	else if (lua_gc_last_step_us && lua_gc_last_step_us < target_step_us / 2u)
	{
		lua_gc_adaptive_step_kb = _min(
			lua_gc_adaptive_step_kb + _max(lua_gc_adaptive_step_kb / 2u, 1u),
			max_adaptive_step_kb);
	}

	if (pressure_level >= 2u)
		lua_gc_adaptive_step_kb = _min(_max(lua_gc_adaptive_step_kb * 2u, 32u), max_adaptive_step_kb);

	if (severe_pool_pressure)
		lua_gc_adaptive_step_kb = _max(lua_gc_adaptive_step_kb, 256u);
	else if (critical_pool_pressure)
		lua_gc_adaptive_step_kb = _max(lua_gc_adaptive_step_kb, 128u);

	LuaGCStepKB = lua_gc_adaptive_step_kb;

	const u64 start_ticks = CPU::QPC();
	u64 elapsed_ticks = 0;
	u64 max_step_ticks = 0;
	// With a working QPC the elapsed-time budget is the real limiter. Do not turn
	// the number of API calls into a second, much tighter budget: LuaJIT's sweep
	// and finalize states may intentionally advance only one very cheap item per
	// host call. The previous 64-call default could therefore spend ~10 us out of
	// a 1.2 ms slice and keep one GC cycle alive for tens of thousands of frames.
	// Keep only a very high corruption/runaway guard. Finalizers are still safe:
	// CLevel::LuaGC() returns after at most one arbitrary __gc callback, and the
	// host checks elapsed time before starting the next one.
	const int configured_calls = _min(_max(psLua_ParallelGC_CallAmount, 1), 50);
	const int max_calls = budget_ticks ?
		(critical_memory_pressure || LuaGCCycleAgeFrames >= 600u ? 65536 : 32768) : configured_calls;

	bool reached_deferred_phase = false;
	while (LuaGCCount < max_calls)
	{
		if (LuaGCCount && budget_ticks && elapsed_ticks >= budget_ticks)
		{
			LuaGCBudgetHit = true;
			break;
		}

		LuaGCStepKB = lua_gc_adaptive_step_kb;
		const u64 step_start_ticks = CPU::QPC();
		++LuaGCCount;
		const int cycle_completed = LuaGC();
		lua_gc_cycle_started = true;
		const u64 step_end_ticks = CPU::QPC();
		const u64 step_ticks = step_end_ticks - step_start_ticks;
		max_step_ticks = _max(max_step_ticks, step_ticks);
		elapsed_ticks = step_end_ticks - start_ticks;

		// React inside the same frame when the current quantum is far cheaper than
		// the target. CLevel::LuaGC() applies the game-configured maximum before the
		// next exact-quantum call, so this cannot bypass its safety clamp.
		const u32 step_us = qpc_ticks_to_us(step_ticks);
		const u32 actual_step_kb = _max(LuaGCStepKB, min_adaptive_step_kb);
		if (step_us && !resuming_deferred_phase && !reached_deferred_phase)
		{
			if (step_us < _max(target_step_us / 4u, 1u))
				lua_gc_adaptive_step_kb = _min(actual_step_kb * 2u, max_adaptive_step_kb);
			else if (step_us > target_step_us + target_step_us / 2u && actual_step_kb > min_adaptive_step_kb)
				lua_gc_adaptive_step_kb = _max(actual_step_kb / 2u, min_adaptive_step_kb);
			else
				lua_gc_adaptive_step_kb = actual_step_kb;
		}

		if (cycle_completed == 1)
		{
			LuaGCDone = true;
			break;
		}
		if (cycle_completed == 2)
		{
			// Propagation finished without entering atomic. Never loop straight
			// back into the indivisible phase at the pre-render scheduling point.
			break;
		}
		if (cycle_completed == 4)
		{
			++LuaGCPreAtomicSlices;
			// A bounded pre-atomic marking slice completed. Stay inside the current
			// time budget; the next loop iteration may continue the pre-pass.
			continue;
		}
		if (cycle_completed == 5)
		{
			// First quiescent pre-atomic probe. Deliberately leave the commit for a
			// later post-frame pass so one budget slice never contains both the
			// pre-pass and the indivisible stock atomic commit.
			break;
		}
		if (cycle_completed == 3)
		{
			reached_deferred_phase = true;
			// Before rendering, never enter arbitrary __gc callbacks. At the post-
			// frame point one callback has run (or the boundary was just reached),
			// so loop only after the regular time-budget check above.
			if (!allow_deferred_phase)
				break;
		}
	}

	if (!LuaGCCount)
		elapsed_ticks = CPU::QPC() - start_ticks;

	LuaGCBudgetUsedUS = qpc_ticks_to_us(elapsed_ticks);
	LuaGCBudgetUtilPercent = LuaGCBudgetUS ?
		_min(static_cast<u32>((static_cast<u64>(LuaGCBudgetUsedUS) * 100ull) / LuaGCBudgetUS), 999u) : 0u;
	LuaGCStepMaxUS = qpc_ticks_to_us(max_step_ticks);
	if (LuaGCCount)
	{
		// Atomic can take tens or hundreds of milliseconds regardless of the
		// requested quantum. Feeding that time into the quantum controller collapsed
		// later sweep work to 1 KiB and caused the observed 800 MiB GC debt.
		if (LuaGCStepMaxUS && !resuming_deferred_phase && !reached_deferred_phase)
			lua_gc_last_step_us = LuaGCStepMaxUS;
		lua_gc_adaptive_step_kb = _max(lua_gc_adaptive_step_kb, min_adaptive_step_kb);
	}
	else
	{
		LuaGCStepKB = 0;
	}
	if (!LuaGCDone && LuaGCCount >= max_calls)
	{
		LuaGCCallCapHit = true;
		// This is now only a runaway guard. A low-utilization hit is diagnostic, not
		// a reason to keep growing the byte quantum: some native GC states advance
		// one list item per call and are insensitive to step_kb.
	}

	SLuaGCStatus native_gc_status_after = {};
	if (LuaGCStatus)
		LuaGCStatus(&native_gc_status_after);
	const u32 memory_after_kb = LuaGCStatus ? native_gc_status_after.total_kb :
		(LuaGCMemory ? LuaGCMemory() : current_memory_kb);
	LuaGCMemoryKB = memory_after_kb;
	LuaGCProgressKB = current_memory_kb > memory_after_kb ? current_memory_kb - memory_after_kb : 0u;
	if (lua_gc_cycle_started)
		LuaGCCycleAgeFrames = Device.dwFrame >= lua_gc_cycle_start_frame ?
			Device.dwFrame - lua_gc_cycle_start_frame + 1u : 1u;
	else
		LuaGCCycleAgeFrames = 0;
	if (LuaGCStatus)
	{
		LuaGCNativeState = native_gc_status_after.state;
		LuaGCDebtKB = native_gc_status_after.debt_kb;
		LuaGCThresholdKB = native_gc_status_after.threshold_kb;
		LuaGCPoolReservedKB = native_gc_status_after.pool_reserved_kb;
		LuaGCPoolCommittedKB = native_gc_status_after.pool_committed_kb;
		LuaGCPoolFallbackKB = native_gc_status_after.pool_fallback_kb;
		LuaGCPoolAllocationFailures = native_gc_status_after.pool_allocation_failures;
		LuaGCNativeCycleActive = native_gc_status_after.cycle_active;
		const u64 udata_after_serial = native_gc_status_after.udata_alloc_serial;
		LuaGCUdataAllocatedSinceCycle = static_cast<u32>(_min<u64>(
			udata_after_serial >= lua_gc_udata_baseline_serial ?
			udata_after_serial - lua_gc_udata_baseline_serial : 0ull, u32(-1)));
		if (LuaGCAtomicCommits)
		{
			LuaGCAtomicMarkPermille = native_gc_status_after.atomic_mark_permille;
			LuaGCAtomicFinalizePermille = native_gc_status_after.atomic_finalize_permille;
			LuaGCAtomicWeakPermille = native_gc_status_after.atomic_weak_permille;
			LuaGCAtomicUdataVisited = native_gc_status_after.atomic_udata_visited;
			LuaGCAtomicUdataFinalizable = native_gc_status_after.atomic_udata_finalizable;
			LuaGCAtomicUdataPages = native_gc_status_after.atomic_udata_pages;
			LuaGCAtomicWeakTables = native_gc_status_after.atomic_weak_tables;
			LuaGCAtomicWeakSlots = native_gc_status_after.atomic_weak_slots;
		}
	}

	if (LuaGCDone)
	{
		UpdateLuaGCCycleFeedback(memory_after_kb);
		LuaGCCycleActive = false;
		LuaGCAssistingNativeCycle = false;
		lua_gc_cycle_started = false;
		lua_gc_cycle_start_frame = 0;
		lua_gc_baseline_kb = memory_after_kb;
		// Do not reset userdata pressure here. The baseline was captured when the
		// cycle started, so this retains wrappers allocated during the cycle and can
		// immediately schedule a follow-up collection if churn stayed high.
		lua_gc_budget_hit_streak = 0;
	}
	else if (LuaGCBudgetHit)
	{
		lua_gc_budget_hit_streak = _min(lua_gc_budget_hit_streak + 1u, 32u);
	}
	else if (lua_gc_budget_hit_streak)
	{
		--lua_gc_budget_hit_streak;
	}

	if (memory_after_kb < lua_gc_baseline_kb)
		lua_gc_baseline_kb = memory_after_kb;

	lua_gc_last_memory_kb = memory_after_kb;
}

void CRenderDevice::ResetLuaGCAdaptiveState()
{
	lua_gc_baseline_kb = 0;
	lua_gc_last_memory_kb = 0;
	lua_gc_budget_hit_streak = 0;
	lua_gc_adaptive_step_kb = 0;
	lua_gc_cycle_start_frame = 0;
	lua_gc_last_step_us = 0;
	lua_gc_cycle_start_memory_kb = 0;
	lua_gc_cycle_growth_kb = 0;
	lua_gc_growth_scale = 8;
	lua_gc_udata_baseline_serial = 0;
	lua_gc_cycle_started = false;
	LuaGCCount = 0;
	LuaGCDone = false;
	LuaGCCycleActive = false;
	LuaGCBudgetHit = false;
	LuaGCCallCapHit = false;
	LuaGCBudgetUtilPercent = 0;
	LuaGCCycleAgeFrames = 0;
	LuaGCProgressKB = 0;
	LuaGCEmergencyStep = false;
	LuaGCAtomicPrepared = false;
	LuaGCPreAtomicSlices = 0;
	LuaGCAtomicCommits = 0;
	LuaGCAtomicMarkPermille = 0;
	LuaGCAtomicFinalizePermille = 0;
	LuaGCAtomicWeakPermille = 0;
	LuaGCAtomicUdataVisited = 0;
	LuaGCAtomicUdataFinalizable = 0;
	LuaGCAtomicUdataPages = 0;
	LuaGCUdataAllocatedSinceCycle = 0;
	LuaGCAtomicWeakTables = 0;
	LuaGCAtomicWeakSlots = 0;
	LuaGCNativeCycleActive = false;
	LuaGCAssistingNativeCycle = false;
	LuaGCNativeState = 0;
	LuaGCDebtKB = 0;
	LuaGCThresholdKB = 0;
	LuaGCBudgetUS = 0;
	LuaGCBudgetUsedUS = 0;
	LuaGCStepKB = 0;
	LuaGCStepMaxUS = 0;
	LuaGCMemoryKB = 0;
	LuaGCMemoryGrowthKB = 0;
	LuaGCTriggerGrowthKB = 0;
	LuaGCPoolReservedKB = 0;
	LuaGCPoolCommittedKB = 0;
	LuaGCPoolFallbackKB = 0;
	LuaGCPoolAllocationFailures = 0;
	LuaGCCycleReclaimedKB = 0;
	LuaGCReclaimPercent = 0;
	LuaGCGrowthScale = 8;
	LuaGCWaitUS = 0;
	LuaGCDeferredWaitUS = 0;
	LuaGCLastWaitUS = 0;
	LuaGCBackoffFrames = 0;
	LuaGCWaitStreak = 0;
	FrameParallelWaitUS = 0;

	LuaFrameAllocCalls = 0;
	LuaFrameReallocCalls = 0;
	LuaFrameFreeCalls = 0;
	LuaFrameAllocatedBytes = 0;
	LuaFrameFreedBytes = 0;
}

void CRenderDevice::QueueSchedulerDeferredWork()
{
	R_ASSERT(InterlockedCompareExchange(&mt_scheduler_deferred_pending, TRUE, FALSE) == FALSE);
}

void CRenderDevice::SubmitFrameParallelWork()
{
	R_ASSERT(mt_frame_group.empty());
	R_ASSERT(InterlockedCompareExchange(&mt_frame_submitted, FALSE, FALSE) == FALSE);

	InterlockedExchange(&mt_frame_submitted, TRUE);

	if (!xr_jobs::submit(&process_frame_parallel_job, this, &mt_frame_group, xr_jobs::priority::high))
	{
		// The pool may reject work only while shutting down. Preserve frame
		// correctness by executing the lane on the submitting thread.
		ProcessFrameParallelWork();
	}
}

void CRenderDevice::WaitFrameParallelWork()
{
	if (InterlockedExchange(&mt_frame_submitted, FALSE) == FALSE)
	{
		// Shutdown or a non-rendering frame may skip SubmitFrameParallelWork after
		// FrameMove. Never leave the scheduler frame open across lifecycle changes.
		if (InterlockedExchange(&mt_scheduler_deferred_pending, FALSE) != FALSE)
		{

			Engine.Sheduler.UpdateDeferred();
			Engine.Sheduler.UpdateFinalize();

		}
		return;
	}

	const u64 wait_start_ticks = CPU::QPC();
	xr_jobs::wait(mt_frame_group);
	const u64 wait_end_ticks = CPU::QPC();
	FrameParallelWaitUS = qpc_ticks_to_us(wait_end_ticks - wait_start_ticks);
}

#include "igame_level.h"

u32 CRenderDevice::LevelPrecacheFrames() const
{
	// Deferred texture upload already materializes the complete texture set before
	// this warm-up. Twenty-four full-circle samples retain shader/visibility warm-up
	// while avoiding the fixed 60-frame tail on every level or save load.
	static u32 frames = []()
	{
		u32 value = 24;
		if (Core.Params)
		{
			LPCSTR option = strstr(Core.Params, "-precache_frames");
			if (option)
			{
				u32 requested = value;
				if (sscanf_s(option + xr_strlen("-precache_frames"), "%u", &requested) == 1)
					value = _min(requested, 120u);
			}
		}
		return value;
	}();

	return frames;
}

void CRenderDevice::PreCache(u32 amount, bool b_draw_loadscreen, bool b_wait_user_input)
{
#ifdef DEDICATED_SERVER
    amount = 0;
#else
	if (m_pRender->GetForceGPU_REF())
		amount = 0;
#endif

	dwPrecacheFrame = dwPrecacheTotal = amount;
	if (amount && !precache_light && g_pGameLevel && g_loading_events.empty())
	{
		precache_light = ::Render->light_create();
		precache_light->set_shadow(false);
		precache_light->set_position(vCameraPosition);
		precache_light->set_color(255, 255, 255);
		precache_light->set_range(5.0f);
		precache_light->set_active(true);
	}

	if (amount && b_draw_loadscreen && !load_screen_renderer.b_registered)
	{
		load_screen_renderer.start(b_wait_user_input);
	}
}

int g_svDedicateServerUpdateReate = 100;

ENGINE_API xr_list<LOADING_EVENT> g_loading_events;

extern bool IsMainMenuActive(); //ECO_RENDER add

static HMONITOR g_StartupMonitor = NULL;

#include "MonitorList.h"

static void InitMonitor()
{
	if (g_StartupMonitor)
		return;

	HMONITOR chosen = ResolveSelectedMonitor();
	if (chosen)
	{
		MONITORINFO mi;
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoA(chosen, &mi))
		{
			g_StartupMonitor = chosen;
			return;
		}
		Msg("! vid_monitor: resolved handle is invalid, using Auto");
	}

	POINT cursorPos;
	GetCursorPos(&cursorPos);
	g_StartupMonitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTOPRIMARY);
}

ENGINE_API void ResetStartupMonitor()
{
	g_StartupMonitor = NULL;
}

ENGINE_API void SetStartupMonitor(HMONITOR h)
{
	g_StartupMonitor = h;
}

ENGINE_API HMONITOR GetStartupMonitor()
{
	InitMonitor();
	return g_StartupMonitor;
}

void GetMonitorResolution(u32& horizontal, u32& vertical)
{
	InitMonitor();

	MONITORINFO mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(g_StartupMonitor, &mi))
	{
		horizontal = mi.rcMonitor.right - mi.rcMonitor.left;
		vertical = mi.rcMonitor.bottom - mi.rcMonitor.top;
	}
	else
	{
		RECT desktop;
		const HWND hDesktop = GetDesktopWindow();
		GetWindowRect(hDesktop, &desktop);
		horizontal = desktop.right - desktop.left;
		vertical = desktop.bottom - desktop.top;
	}
}

void GetMonitorPosition(int& x, int& y)
{
	InitMonitor();

	MONITORINFO mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(g_StartupMonitor, &mi))
	{
		x = mi.rcMonitor.left;
		y = mi.rcMonitor.top;
	}
	else
	{
		x = 0;
		y = 0;
	}
}

float GetMonitorRefresh()
{
	DEVMODE lpDevMode;
	memset(&lpDevMode, 0, sizeof(DEVMODE));
	lpDevMode.dmSize = sizeof(DEVMODE);
	lpDevMode.dmDriverExtra = 0;

	if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &lpDevMode) == 0)
	{
		return 1.f / 60.f;
	}
	else
		return 1.f / lpDevMode.dmDisplayFrequency;
}

extern int ps_framelimiter;
extern u32 g_screenmode;

CTimer FreezeTimer;
void mt_FreezeThread(void *ptr) {
	float freezetime = 0.f;
	float repeatcheck = 500.f;

	while (true)
	{
		PROF_EVENT();

		if (g_loading_events.size())
			freezetime = 25000.0f;
		else
			freezetime = 5000.0f;

		repeatcheck = 500.f;

		START_PROFILE("Check timer");
		if (FreezeTimer.GetElapsed_sec()*1000.f > freezetime)
		{
			FlushLog();
			repeatcheck = 5000.f;
		}
		STOP_PROFILE;

		Sleep(repeatcheck);
	}
}

void CRenderDevice::on_idle()
{
	QUARK_DIAGNOSTIC_STAGE("main/idle/start");
	FreezeTimer.Start();

	if (!b_is_Ready)
	{
		QUARK_DIAGNOSTIC_STAGE("main/idle/device-not-ready");
		Sleep(100);
		return;
	}

	PROF_FRAME("X-RAY Primary thread");
	PROF_EVENT();

#ifdef DEDICATED_SERVER
    u32 FrameStartTime = TimerGlobal.GetElapsed_ms();
#endif

	START_PROFILE("Set stat gathering");
	if (psDeviceFlags.test(rsStatistic))
		g_bEnableStatGather = TRUE;
	else g_bEnableStatGather = FALSE;
	Memory.mem_calls_enable(g_bEnableStatGather != FALSE);
	STOP_PROFILE;

	if (g_loading_events.size())
	{
		QUARK_DIAGNOSTIC_STAGE("main/loading/callback");
		PROF_EVENT("Pop loading event");

		// Loading callbacks are state-machine steps. Some callbacks replace or erase
		// entries in g_loading_events themselves, so only one step may run per idle
		// pass. This preserves the original transition and loading-screen semantics.

		if (g_loading_events.front()())
			g_loading_events.pop_front();

		QUARK_DIAGNOSTIC_STAGE("main/loading/draw");
		pApp->LoadDraw();

		return;
	}

	if (!Device.dwPrecacheFrame && !g_SASH.IsBenchmarkRunning() && g_bLoaded)
	{
		PROF_EVENT("Start xrSASH Benchmark");
		g_SASH.StartBenchmark();
	}

	if (LuaProfileFrameBegin)
		LuaProfileFrameBegin();

	QUARK_DIAGNOSTIC_STAGE("main/frame/update");
	FrameMove();

	// Precache
	if (dwPrecacheFrame)
	{
		PROF_EVENT("Precache frame");
		float factor = float(dwPrecacheFrame) / float(dwPrecacheTotal);
		float angle = PI_MUL_2 * factor;
		vCameraDirection.set(_sin(angle), 0, _cos(angle));
		vCameraDirection.normalize();
		vCameraTop.set(0, 1, 0);
		vCameraRight.crossproduct(vCameraTop, vCameraDirection);

		mView.build_camera_dir(vCameraPosition, vCameraDirection, vCameraTop);
	}

	// Matrices
	START_PROFILE("Matrices");
	mFullTransform.mul(mProject, mView);
	mFullTransformHud.mul(mProjectHud, mView);
	m_pRender->SetCacheXform(mView, mProject);

	// Previous frame data -- 
	mView_prev = mView_saved;
	mProject_prev = mProject_saved;
	//mFullTransform_prev = mFullTransform_saved; // Unused?

	m_pRender->SetCacheXform_prev(mView_prev, mProject_prev);

	// Save previous frame grass benders data
	IGame_Persistent::grass_data& GData = g_pGamePersistent->grass_shader_data;

	GData.prev_pos[0].set(Device.vCameraPosition.x, Device.vCameraPosition.y, Device.vCameraPosition.z, -1);
	GData.prev_dir[0].set(0.0f, -99.0f, 0.0f, 1.0f);

	for (int pBend = 1; pBend < _min(16, ps_ssfx_grass_interactive.y + 1); pBend++)
	{
		GData.prev_pos[pBend].set(GData.pos[pBend].x, GData.pos[pBend].y, GData.pos[pBend].z, GData.radius_curr[pBend]);
		GData.prev_dir[pBend].set(GData.dir[pBend].x, GData.dir[pBend].y, GData.dir[pBend].z, GData.str[pBend]);
	}

	// Save wind animation position
	wind_anim_prev = wind_anim_saved;
	wind_anim_saved = g_pGamePersistent->Environment().wind_anim;

	//RCache.set_xform_view ( mView );
	//RCache.set_xform_project ( mProject );
	D3DXMatrixInverse((D3DXMATRIX*)&mInvFullTransform, 0, (D3DXMATRIX*)&mFullTransform);

	vCameraPosition_saved = vCameraPosition;
	mFullTransform_saved = mFullTransform;
	mView_saved = mView;
	mProject_saved = mProject;
	STOP_PROFILE;

	QUARK_DIAGNOSTIC_STAGE("main/render/preparation");
	START_PROFILE("Process render preparation");
	ProcessRenderPrepParallelWork();
	STOP_PROFILE;

	Device.LuaGCCount = 0;
	Device.LuaGCDone = false;
	Device.LuaGCBudgetHit = false;
	Device.LuaGCEmergencyStep = false;
	Device.LuaGCBudgetUS = 0;
	Device.LuaGCBudgetUsedUS = 0;
	Device.LuaGCStepKB = 0;
	Device.LuaGCStepMaxUS = 0;
	Device.LuaGCPreAtomicSlices = 0;
	Device.LuaGCAtomicCommits = 0;
	Device.LuaGCAtomicMarkPermille = 0;
	Device.LuaGCAtomicFinalizePermille = 0;
	Device.LuaGCAtomicWeakPermille = 0;
	Device.LuaGCAtomicUdataVisited = 0;
	Device.LuaGCAtomicUdataFinalizable = 0;
	Device.LuaGCAtomicUdataPages = 0;
	Device.LuaGCAtomicWeakTables = 0;
	Device.LuaGCAtomicWeakSlots = 0;
	Device.LuaGCWaitUS = 0;
	Device.LuaGCDeferredWaitUS = 0;
	Device.FrameParallelWaitUS = 0;

	bool lua_gc_phase_deferred = false;
	auto is_lua_gc_deferred_state = [](u32 state)
	{
		return state == 2u || state == 5u;
	};

	if (psLua_ParallelGC && Device.LuaGC)
	{
		// Advance only bounded phases here. LuaJIT yields before atomic/finalizers,
		// so an indivisible phase or arbitrary callback cannot delay frame submission.
		QUARK_DIAGNOSTIC_STAGE("main/lua/gc");
		const u64 gc_started_ticks = CPU::QPC();
		ProcessLuaGCWork(false);
		Device.LuaGCWaitUS = qpc_ticks_to_us(CPU::QPC() - gc_started_ticks);
		lua_gc_phase_deferred = Device.LuaGCNativeCycleActive &&
			is_lua_gc_deferred_state(Device.LuaGCNativeState);
	}

	InterlockedExchange(&Device.isRendering, TRUE);

	QUARK_DIAGNOSTIC_STAGE("main/jobs/submit");
	START_PROFILE("Submit frame parallel work");
	SubmitFrameParallelWork();
	STOP_PROFILE;

#ifdef ECO_RENDER // ECO_RENDER START
	if (Device.Paused() || IsMainMenuActive() || ps_framelimiter)
	{
		PROF_EVENT("Eco Render");

		if (refresh_rate == 0)
			refresh_rate = GetMonitorRefresh();

		float rr;

		if (ps_framelimiter)
			rr = 1.f / ps_framelimiter;
		else
			rr = refresh_rate;

		WaitForFrameInterval(rr);
	}
#endif // ECO_RENDER END

#ifndef DEDICATED_SERVER
	Statistic->RenderTOTAL_Real.FrameStart();
	Statistic->RenderTOTAL_Real.Begin();

	QUARK_DIAGNOSTIC_STAGE("main/render/device-begin");
	const bool frame_began = b_is_Active && Begin();
	if (frame_began)
	{
		QUARK_DIAGNOSTIC_STAGE("main/render/seqRender");

		START_PROFILE("Process seqRender");
		seqRender.Process(rp_Render);
		STOP_PROFILE;

		if (psDeviceFlags.test(rsCameraPos) || psDeviceFlags.test(rsStatistic) || Statistic->errors.size())
		{
			PROF_EVENT("Draw statistics");
			Statistic->Show();
		}

		QUARK_DIAGNOSTIC_STAGE("main/render/present");

		End();

	}
	Statistic->RenderTOTAL_Real.End();
	Statistic->RenderTOTAL_Real.FrameEnd();
	Statistic->RenderTOTAL.accum = Statistic->RenderTOTAL_Real.accum;
#endif // #ifndef DEDICATED_SERVER
	InterlockedExchange(&Device.isRendering, FALSE);

	QUARK_DIAGNOSTIC_STAGE("main/jobs/wait");
	START_PROFILE("Wait frame parallel work");
	WaitFrameParallelWork();
	STOP_PROFILE;

	// An allocation on a gameplay worker may reach atomic/finalize after the
	// pre-render pass. Re-check only after every frame job has joined, then service
	// that boundary in this same frame instead of stranding it until the next one.
	if (!lua_gc_phase_deferred && psLua_ParallelGC && Device.LuaGC && Device.LuaGCStatus)
	{
		SLuaGCStatus post_job_gc_status = {};
		Device.LuaGCStatus(&post_job_gc_status);
		lua_gc_phase_deferred = post_job_gc_status.cycle_active &&
			is_lua_gc_deferred_state(post_job_gc_status.state);
	}

	if (lua_gc_phase_deferred && psLua_ParallelGC && Device.LuaGC)
	{
		// All frame jobs are complete and the just-submitted GPU work is already in
		// flight. Resume on the main thread, so Lua cannot race the collector and GPU
		// execution can hide part of atomic/finalizer work.
		QUARK_DIAGNOSTIC_STAGE("main/lua/gc-deferred");
		const u64 gc_deferred_started_ticks = CPU::QPC();
		ProcessLuaGCWork(true);
		Device.LuaGCDeferredWaitUS = qpc_ticks_to_us(CPU::QPC() - gc_deferred_started_ticks);
		Device.LuaGCWaitUS += Device.LuaGCDeferredWaitUS;
	}

	if (psLua_ParallelGC && Device.LuaGC)
	{
		// Only bounded incremental work drives the adaptive step/backoff controller.
		// Atomic is a once-per-cycle phase and shrinking the following sweep because
		// atomic was long makes both the next hitch and memory pressure worse.
		const u32 bounded_gc_wait_us = Device.LuaGCWaitUS >= Device.LuaGCDeferredWaitUS ?
			Device.LuaGCWaitUS - Device.LuaGCDeferredWaitUS : 0u;
		Device.LuaGCLastWaitUS = bounded_gc_wait_us;
		if (bounded_gc_wait_us > 500u)
		{
			Device.LuaGCWaitStreak = _min(Device.LuaGCWaitStreak + 2u, 16u);
			Device.LuaGCBackoffFrames = _max(Device.LuaGCBackoffFrames, 4u);
		}
		else if (bounded_gc_wait_us > 150u)
		{
			Device.LuaGCWaitStreak = _min(Device.LuaGCWaitStreak + 1u, 16u);
			Device.LuaGCBackoffFrames = _max(Device.LuaGCBackoffFrames, 1u);
		}
		else if (Device.LuaGCWaitStreak)
		{
			--Device.LuaGCWaitStreak;
		}
	}

	QUARK_DIAGNOSTIC_STAGE("main/frame/post");
	if (LuaProfileFrameEnd)
		LuaProfileFrameEnd();

	if (psLua_ParallelGC_debug && psLua_ParallelGC && Device.LuaGCDebug)
	{
		Device.LuaGCDebug();
	}
	// Finish the full recorder first so its OS CPU/cycle sample ends next to the
	// measured frame boundary. The legacy warning below may format and lock the
	// console, which must not be misdiagnosed as gameplay CPU work.

#ifdef DEDICATED_SERVER
    u32 FrameEndTime = TimerGlobal.GetElapsed_ms();
    u32 FrameTime = (FrameEndTime - FrameStartTime);
    u32 DSUpdateDelta = 1000 / g_svDedicateServerUpdateReate;
    if (FrameTime < DSUpdateDelta)
        Sleep(DSUpdateDelta - FrameTime);
#endif
	QUARK_DIAGNOSTIC_STAGE("main/idle/completed");
	if (!b_is_Active)
		Sleep(1);
}

#ifdef INGAME_EDITOR
void CRenderDevice::message_loop_editor()
{
    m_editor->run();
    m_editor_finalize(m_editor);
    m_editor = nullptr;
    xr_delete(m_engine);

    if (m_editor_module)
    {
        FreeLibrary(m_editor_module);
        m_editor_module = nullptr;
        m_editor_initialize = nullptr;
        m_editor_finalize = nullptr;
    }
}
#endif // #ifdef INGAME_EDITOR

void CRenderDevice::Screenshot()
{
	PROF_EVENT();
	Render->Screenshot();
}

void CRenderDevice::message_loop()
{
#ifdef INGAME_EDITOR
    if (editor())
    {
        message_loop_editor();
        return;
    }
#endif
	MSG msg;
	PeekMessage(&msg, NULL, 0U, 0U, PM_NOREMOVE);
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}
		// A deactivated single-player game is already paused by
		// CGamePersistent::OnAppDeactivate. Do not spin complete engine frames while
		// object updates are suspended: frame-side producers would otherwise build
		// queues that all consumers drain on the first frame after Alt+Tab. Loading
		// state machines and non-paused multiplayer/always-active sessions still run.
		if (!b_is_Active && Paused() && g_loading_events.empty())
		{
			FreezeTimer.Start();
			Sleep(10);
			continue;
		}
		on_idle();
	}
}

void mt_DiscordThread(void*)
{
	bool discord_initialized = false;

	while (true)
	{
		if (!pApp)
		{
			Msg("[Discord] pApp destroyed, killing thread");
			return;
		}

		if (use_discord && psDeviceFlags2.test(rsDiscord))
		{
			// Prioritize the first rendered frame and main-menu construction.
			if (!g_bLoaded)
			{
				Sleep(50);
				continue;
			}

			if (!discord_initialized)
			{
				Init_Discord();
				discord_initialized = use_discord && discord_core != nullptr;
				if (!discord_initialized)
				{
					Sleep(1000);
					continue;
				}
			}

			START_PROFILE("Discord");
			discord_core->RunCallbacks();
			updateDiscordPresence();
			STOP_PROFILE;
			Sleep(int(discord_update_rate * 1000));
		}
		else
		{
			Sleep(1000);
		}
	}
}

void CRenderDevice::Run()
{
	// DUMP_PHASE;
	g_bLoaded = FALSE;
	Log("Starting engine...");
	thread_name("X-RAY Primary thread");
	QUARK_DIAGNOSTIC_BREADCRUMB("lifecycle", "render device run started");
	QUARK_DIAGNOSTIC_STAGE("main/run/startup");
	// Startup timers and calculate timer delta
	dwTimeGlobal = 0;
	Timer_MM_Delta = 0;
	{
		u32 time_mm = timeGetTime();
		while (timeGetTime() == time_mm); // wait for next tick
		u32 time_system = timeGetTime();
		u32 time_local = TimerAsync();
		Timer_MM_Delta = time_system - time_local;
	}
	// Start auxiliary threads. Per-frame parallel work is dispatched through
	// the shared worker pool instead of a dedicated secondary thread.
	InterlockedExchange(&mt_bMustExit, FALSE);
	thread_spawn(mt_FreezeThread, "Freeze detecting thread", 0, 0);
	thread_spawn(mt_DiscordThread, "X-RAY Discord thread", 0, 0);
	// Message cycle
	QUARK_DIAGNOSTIC_STAGE("main/run/app-start-callbacks");
	seqAppStart.Process(rp_AppStart);
	m_pRender->ClearTarget();
	SetForegroundWindow(m_hWnd);
	QUARK_DIAGNOSTIC_STAGE("main/run/message-loop");
	message_loop();
	QUARK_DIAGNOSTIC_STAGE("main/run/app-end-callbacks");
	seqAppEnd.Process(rp_AppEnd);
	WaitFrameParallelWork();
	InterlockedExchange(&mt_bMustExit, TRUE);
	QUARK_DIAGNOSTIC_BREADCRUMB("lifecycle", "render device run stopped");
	QUARK_DIAGNOSTIC_CLEAR_STAGE();
}

u32 app_inactive_time = 0;
u32 app_inactive_time_start = 0;

void CRenderDevice::FrameMove()
{
	PROF_EVENT();

	if (InterlockedExchange(&g_monitor_list_dirty, 0))
		refresh_vid_monitor_list();

	dwFrame++;
	Core.dwFrame = dwFrame;
	EngineErrorLogger::SetFrameNumber(dwFrame);
	dwTimeContinual = TimerMM.GetElapsed_ms() - app_inactive_time;
	if (psDeviceFlags.test(rsConstantFPS))
	{
		PROF_EVENT("Constant FPS");

		// 20ms = 50fps
		//fTimeDelta = 0.020f;
		//fTimeGlobal += 0.020f;
		//dwTimeDelta = 20;
		//dwTimeGlobal += 20;
		// 33ms = 30fps
		fTimeDelta = 0.033f;
		fTimeGlobal += 0.033f;
		dwTimeDelta = 33;
		dwTimeGlobal += 33;
	}
	else
	{
		PROF_EVENT("Timer FPS");

		// Timer
		float fPreviousFrameTime = Timer.GetElapsed_sec();
		Timer.Start(); // previous frame
		fTimeDelta = 0.1f * fTimeDelta + 0.9f * fPreviousFrameTime;
		// smooth random system activity - worst case ~7% error
		//fTimeDelta = 0.7f * fTimeDelta + 0.3f*fPreviousFrameTime; // smooth random system activity
		if (fTimeDelta > .1f)
			fTimeDelta = .1f; // limit to 15fps minimum
		if (fTimeDelta <= 0.f)
			fTimeDelta = EPS_S + EPS_S; // limit to 15fps minimum
		if (Paused())
			fTimeDelta = 0.0f;
		// u64 qTime = TimerGlobal.GetElapsed_clk();
		fTimeGlobal = TimerGlobal.GetElapsed_sec(); //float(qTime)*CPU::cycles2seconds;
		u32 _old_global = dwTimeGlobal;
		dwTimeGlobal = TimerGlobal.GetElapsed_ms();
		dwTimeDelta = dwTimeGlobal - _old_global;
	}
	// Frame move
	Statistic->EngineTOTAL.Begin();
	// TODO: HACK to test loading screen.
	//if(!g_bLoaded)
	START_PROFILE("Process seqFrame");
	Device.seqFrame.Process(rp_Frame);
	STOP_PROFILE;
	g_bLoaded = TRUE;
	//else
	// seqFrame.Process(rp_Frame);
	Statistic->EngineTOTAL.End();
}

ENGINE_API BOOL bShowPauseString = TRUE;
#include "IGame_Persistent.h"

void CRenderDevice::Pause(BOOL bOn, BOOL bTimer, BOOL bSound, LPCSTR reason)
{
	PROF_EVENT();

	static int snd_emitters_ = -1;

	if (g_bBenchmark)
		return;
#ifndef DEDICATED_SERVER
	if (bOn)
	{
		if (!Paused())
			bShowPauseString =
#ifdef INGAME_EDITOR
                editor() ? FALSE :
#endif // #ifdef INGAME_EDITOR
#ifdef DEBUG
                !xr_strcmp(reason, "li_pause_key_no_clip") ? FALSE :
#endif // DEBUG
				TRUE;

		if (bTimer && (!g_pGamePersistent || g_pGamePersistent->CanBePaused()))
		{
			g_pauseMngr().Pause(true);
#ifdef DEBUG
            if (!xr_strcmp(reason, "li_pause_key_no_clip"))
                TimerGlobal.Pause(FALSE);
#endif // DEBUG
		}

		if (bSound && ::Sound)
		{
			snd_emitters_ = ::Sound->pause_emitters(true);
#ifdef DEBUG
			// Log("snd_emitters_[true]",snd_emitters_);
#endif // DEBUG
		}
	}
	else
	{
		if (bTimer && g_pauseMngr().Paused())
		{
			fTimeDelta = EPS_S + EPS_S;
			g_pauseMngr().Pause(false);
		}

		if (bSound)
		{
			if (snd_emitters_ > 0) //avoid crash
			{
				snd_emitters_ = ::Sound->pause_emitters(false);
#ifdef DEBUG
				// Log("snd_emitters_[false]",snd_emitters_);
#endif
			}
			else
			{
#ifdef DEBUG
                Log("Sound->pause_emitters underflow");
#endif
			}
		}
	}

#endif
}

bool CRenderDevice::Paused()
{
	return g_pauseMngr().Paused();
}

void CRenderDevice::OnWM_Activate(WPARAM wParam, LPARAM lParam)
{
	u16 fActive = LOWORD(wParam);
	BOOL fMinimized = (BOOL)HIWORD(wParam);
	BOOL bActive = ((fActive != WA_INACTIVE) && (!fMinimized)) ? TRUE : FALSE;

	if (psDeviceFlags2.test(rsAlwaysActive) && g_screenmode != 2)
	{
		Device.b_is_Active = TRUE;

		if (Device.b_hide_cursor != bActive)
		{
			Device.b_hide_cursor = bActive;

			if (Device.b_hide_cursor)
			{
				ShowCursor(FALSE);
				if (m_hWnd)
				{
					RECT winRect;
					GetClientRect(m_hWnd, &winRect);
					MapWindowPoints(m_hWnd, nullptr, reinterpret_cast<LPPOINT>(&winRect), 2);
					ClipCursor(&winRect);
				}
				pInput->OnAppActivate();
			}
			else
			{
				ShowCursor(TRUE);
				ClipCursor(NULL);
				pInput->OnAppDeactivate();
			}
		}

		return;
	}

	if (bActive != Device.b_is_Active)
	{
		Device.b_is_Active = bActive;

		if (Device.b_is_Active)
		{
			Device.seqAppActivate.Process(rp_AppActivate);
			app_inactive_time += TimerMM.GetElapsed_ms() - app_inactive_time_start;

#ifndef DEDICATED_SERVER
# ifdef INGAME_EDITOR
            if (!editor())
# endif // #ifdef INGAME_EDITOR
			ShowCursor(FALSE);
			if (m_hWnd)
			{
				RECT winRect;
				GetClientRect(m_hWnd, &winRect);
				MapWindowPoints(m_hWnd, nullptr, reinterpret_cast<LPPOINT>(&winRect), 2);
				ClipCursor(&winRect);
			}
#endif // #ifndef DEDICATED_SERVER
		}
		else
		{
			app_inactive_time_start = TimerMM.GetElapsed_ms();
			Device.seqAppDeactivate.Process(rp_AppDeactivate);
			ShowCursor(TRUE);
			ClipCursor(NULL);
		}
	}
}

void CRenderDevice::AddSeqFrame(pureFrame* f, bool mt)
{
	PROF_EVENT();

	if (mt)
		seqFrameMT.Add(f, REG_PRIORITY_HIGH);
	else
		seqFrame.Add(f, REG_PRIORITY_LOW);
}

void CRenderDevice::RemoveSeqFrame(pureFrame* f)
{
	PROF_EVENT();

	seqFrameMT.Remove(f);
	seqFrame.Remove(f);
}

CLoadScreenRenderer::CLoadScreenRenderer()
	: b_registered(false)
{}

void CLoadScreenRenderer::start(bool b_user_input)
{
	PROF_EVENT();

	Device.seqRender.Add(this, 0);
	b_registered = true;
	b_need_user_input = b_user_input;
}

void CLoadScreenRenderer::stop()
{
	PROF_EVENT();

	if (!b_registered)
		return;
	Device.seqRender.Remove(this);
	pApp->destroy_loading_shaders();
	b_registered = false;
	b_need_user_input = false;
}

void CLoadScreenRenderer::OnRender()
{
	PROF_EVENT();

	pApp->load_draw_internal();
}

void CRenderDevice::CSecondVPParams::SetSVPActive(bool bState) //--#SM+#-- +SecondVP+
{
	isActive = bState;
	if (g_pGamePersistent != NULL)
		g_pGamePersistent->m_pGShaderConstants->m_blender_mode.z = (isActive ? 1.0f : 0.0f);
}

bool CRenderDevice::CSecondVPParams::IsSVPFrame() //--#SM+#-- +SecondVP+
{
	return IsSVPActive() && Device.dwFrame % frameDelay == 0;
}
