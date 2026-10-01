#ifndef xr_device
#define xr_device
#pragma once

// Note:
// ZNear - always 0.0f
// ZFar - always 1.0f

//class ENGINE_API CResourceManager;
//class ENGINE_API CGammaControl;

#include "pure.h"
//#include "hw.h"
#include "../xrcore/ftimer.h"
#include "../xrCore/job_system.h"
#include "stats.h"
//#include "shader.h"
//#include "R_Backend.h"

#include "../build_config_defines.h"

#define VIEWPORT_NEAR  Device.ViewportNear //0.2f
#define R_VIEWPORT_NEAR 0.005f

#define DEVICE_RESET_PRECACHE_FRAME_COUNT 10

// demonized: toggle bone optimization
#define OPTIMIZE_CALCULATE_BONES

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/RenderDeviceRender.h"
#include "imgui_base.h"

#ifdef INGAME_EDITOR
# include "../Include/editor/interfaces.hpp"
#endif // #ifdef INGAME_EDITOR

class engine_impl;

enum class EFrameParallelLane : u8
{
	Gameplay = 0,
	Audio,
	Simulation
};

#pragma pack(push,4)

class IRenderDevice
{
public:
	virtual CStatsPhysics* _BCL StatPhysics() = 0;
	virtual void _BCL AddSeqFrame(pureFrame* f, bool mt) = 0;
	virtual void _BCL RemoveSeqFrame(pureFrame* f) = 0;
};

struct SLuaGCStatus
{
	u32 state = 0;
	u32 total_kb = 0;
	u32 debt_kb = 0;
	u32 threshold_kb = 0;
	u32 pool_reserved_kb = 0;
	u32 pool_committed_kb = 0;
	u32 pool_fallback_kb = 0;
	u32 pool_allocation_failures = 0;
	u32 atomic_mark_permille = 0;
	u32 atomic_finalize_permille = 0;
	u32 atomic_weak_permille = 0;
	u32 atomic_udata_visited = 0;
	u32 atomic_udata_finalizable = 0;
	u32 atomic_udata_pages = 0;
	u32 atomic_weak_tables = 0;
	u32 atomic_weak_slots = 0;
	u64 udata_alloc_serial = 0;
	bool cycle_active = false;
};

class ENGINE_API CRenderDeviceData
{
public:
	u32 dwWidth;
	u32 dwHeight;
	u32 clientWidth;
	u32 clientHeight;

	u32 dwPrecacheFrame;
	BOOL b_is_Ready;
	BOOL b_is_Active;
	BOOL b_hide_cursor;
public:

	// Engine flow-control
	u32 dwFrame;

	float fTimeDelta;
	float fTimeGlobal;
	u32 dwTimeDelta;
	u32 dwTimeGlobal;
	u32 dwTimeContinual;

	Fvector vCameraPosition;
	Fvector vCameraDirection;
	Fvector vCameraTop;
	Fvector vCameraRight;

	Fmatrix mView;
	Fmatrix mProject;
	Fmatrix mProjectHud;
	Fmatrix mFullTransform;
	Fmatrix mFullTransformHud;

	Fmatrix mView_prev;
	Fmatrix mProject_prev;

	Fvector4 wind_anim_prev;
	Fvector4 wind_anim_saved;

	// Copies of corresponding members. Used for synchronization.
	Fvector vCameraPosition_saved;

	Fmatrix mView_saved;
	Fmatrix mProject_saved;
	Fmatrix mFullTransform_saved;

	float fFOV;
	float fASPECT;
	float ViewportNear = 0.2f;
protected:

	u32 Timer_MM_Delta;
	CTimer_paused Timer;
	CTimer_paused TimerGlobal;

	//AVO: 
	CTimer frame_timer; //TODO: ïðîâåðèòü, íå äóáëèðóåòñÿ-ëè ñõîæèé òàéìåð (alpet)
	//-AVO

public:

	// Registrators
	CRegistrator<pureRender> seqRender;
	CRegistrator<pureAppActivate> seqAppActivate;
	CRegistrator<pureAppDeactivate> seqAppDeactivate;
	CRegistrator<pureAppStart> seqAppStart;
	CRegistrator<pureAppEnd> seqAppEnd;
	CRegistrator<pureFrame> seqFrame;
	CRegistrator<pureScreenResolutionChanged> seqResolutionChanged;

	HWND m_hWnd;
	// CStats* Statistic;
};

class ENGINE_API CRenderDeviceBase :
	public IRenderDevice,
	public CRenderDeviceData
{
public:
};

#pragma pack(pop)
// refs
class ENGINE_API CRenderDevice : public CRenderDeviceBase
{
public:
	class ENGINE_API CSecondVPParams //--#SM+#-- +SecondVP+
	{
		bool isActive; // Oeaa aeoeaaoee ?aiaa?a ai aoi?ie au?ii?o
		u8 frameDelay;  // Ia eaeii eaa?a n iiiaioa i?ioeiai ?aiaa?a ai aoi?ie au?ii?o iu ia?i?i iiaue
						  //(ia ii?ao auou iaiuoa 2 - ea?aue aoi?ie eaa?, ?ai aieuoa oai aieaa ieceee FPS ai aoi?ii au?ii?oa)

	public:
		bool isCamReady; // Oeaa aioiaiinoe eaia?u (FOV, iiceoey, e o.i) e ?aiaa?o aoi?iai au?ii?oa

		IC bool IsSVPActive() { return isActive; }
		void SetSVPActive(bool bState);
		bool    IsSVPFrame();

		IC u8 GetSVPFrameDelay() { return frameDelay; }
		void  SetSVPFrameDelay(u8 iDelay)
		{
			frameDelay = iDelay;
			clamp<u8>(frameDelay, 2, u8(-1));
		}
	};	

private:
	// Main objects used for creating and rendering the 3D scene
	u32 m_dwWindowStyle;
	RECT m_rcWindowBounds;
	RECT m_rcWindowClient;

	//u32 Timer_MM_Delta;
	//CTimer_paused Timer;
	//CTimer_paused TimerGlobal;
	CTimer TimerMM;

	void _Create(LPCSTR shName);
	void _Destroy(BOOL bKeepTextures);
	void _SetupStates();
public:
	// HWND m_hWnd;
	LRESULT MsgProc(HWND, UINT, WPARAM, LPARAM);

	// u32 dwFrame;
	// u32 dwPrecacheFrame;
	u32 dwPrecacheTotal;

	// u32 dwWidth, dwHeight;
	float fWidth_2, fHeight_2;
	// BOOL b_is_Ready;
	// BOOL b_is_Active;
	void OnWM_Activate(WPARAM wParam, LPARAM lParam);
public:
	//ref_shader m_WireShader;
	//ref_shader m_SelectionShader;

	IRenderDeviceRender* m_pRender;

	BOOL m_bNearer;

	void SetNearer(BOOL enabled)
	{
		if (enabled && !m_bNearer)
		{
			m_bNearer = TRUE;
			mProject._43 -= EPS_L;
		}
		else if (!enabled && m_bNearer)
		{
			m_bNearer = FALSE;
			mProject._43 += EPS_L;
		}
		m_pRender->SetCacheXform(mView, mProject);
		//R_ASSERT(0);
		// TODO: re-implement set projection
		//RCache.set_xform_project (mProject);
	}

	void DumpResourcesMemoryUsage() { m_pRender->ResourcesDumpMemoryUsage(); }
public:
	// Registrators
	//CRegistrator <pureRender > seqRender;
	// CRegistrator <pureAppActivate > seqAppActivate;
	// CRegistrator <pureAppDeactivate > seqAppDeactivate;
	// CRegistrator <pureAppStart > seqAppStart;
	// CRegistrator <pureAppEnd > seqAppEnd;
	//CRegistrator <pureFrame > seqFrame;
	CRegistrator<pureFrame> seqFrameMT;
	CRegistrator<pureDeviceReset> seqDeviceReset;
	// Independent frame-compute callbacks are allowed to run concurrently with
	// each other, but complete before the legacy sequential secondary lane.
	// Queued and in-flight storage are separated so registrations/removals from
	// other threads cannot reallocate a vector that workers are traversing.
	xr_vector<fastdelegate::FastDelegate0<>> seqParallelIndependent;
	xr_vector<fastdelegate::FastDelegate0<>> seqParallelIndependentWork;
	xrCriticalSection seqParallelIndependentLock;
	// Speculative render-preparation callbacks run as a parallel batch with a
	// barrier before rendering starts. A small batch may be discarded, so every
	// producer must retain a correct lazy fallback (for example skeletal render).
	xr_vector<fastdelegate::FastDelegate0<>> seqRenderPrepParallel;
	xr_vector<fastdelegate::FastDelegate0<>> seqRenderPrepParallelWork;
	xr_vector<u32> seqRenderPrepParallelUnits;
	xr_vector<u32> seqRenderPrepParallelWorkUnits;
	xrCriticalSection seqRenderPrepParallelLock;
	xr_vector<fastdelegate::FastDelegate0<>> seqParallel;
	xr_vector<LPCSTR> seqParallelNames;
	xr_vector<EFrameParallelLane> seqParallelLanes;
	// Stable in-flight storage. Registrations during worker execution remain in
	// seqParallel for the next frame and cannot invalidate active lane contexts.
	xr_vector<fastdelegate::FastDelegate0<>> seqParallelWork;
	xr_vector<LPCSTR> seqParallelWorkNames;
	xr_vector<EFrameParallelLane> seqParallelWorkLanes;
	xrCriticalSection seqParallelLock;

	volatile LONG isRendering;

	// LuaGC
	int LuaGCCount;
	bool LuaGCDone;
	bool LuaGCCycleActive;
	bool LuaGCBudgetHit;
	bool LuaGCCallCapHit;
	bool LuaGCEmergencyStep;
	bool LuaGCAtomicPrepared;
	u32 LuaGCPreAtomicSlices;
	u32 LuaGCAtomicCommits;
	u32 LuaGCAtomicMarkPermille;
	u32 LuaGCAtomicFinalizePermille;
	u32 LuaGCAtomicWeakPermille;
	u32 LuaGCAtomicUdataVisited;
	u32 LuaGCAtomicUdataFinalizable;
	u32 LuaGCAtomicUdataPages;
	u32 LuaGCAtomicWeakTables;
	u32 LuaGCAtomicWeakSlots;
	u32 LuaGCUdataAllocatedSinceCycle;
	bool LuaGCNativeCycleActive;
	bool LuaGCAssistingNativeCycle;
	u32 LuaGCNativeState;
	u32 LuaGCDebtKB;
	u32 LuaGCThresholdKB;
	u32 LuaGCBudgetUS;
	u32 LuaGCBudgetUsedUS;
	u32 LuaGCBudgetUtilPercent;
	u32 LuaGCCycleAgeFrames;
	u32 LuaGCProgressKB;
	u32 LuaGCStepKB;
	u32 LuaGCStepMaxUS;
	u32 LuaGCMemoryKB;
	u32 LuaGCMemoryGrowthKB;
	u32 LuaGCTriggerGrowthKB;
	u32 LuaGCPoolReservedKB;
	u32 LuaGCPoolCommittedKB;
	u32 LuaGCPoolFallbackKB;
	u32 LuaGCPoolAllocationFailures;
	u32 LuaGCCycleReclaimedKB;
		u32 LuaGCReclaimPercent;
		u32 LuaGCGrowthScale;
		u32 LuaGCWaitUS;
		u32 LuaGCDeferredWaitUS;
		u32 LuaGCLastWaitUS;
	u32 LuaGCBackoffFrames;
	u32 LuaGCWaitStreak;
	u32 FrameParallelWaitUS;
	u64 LuaFrameAllocCalls;
	u64 LuaFrameReallocCalls;
	u64 LuaFrameFreeCalls;
	u64 LuaFrameAllocatedBytes;
	u64 LuaFrameFreedBytes;
	fastdelegate::FastDelegate0<int> LuaGC;
	fastdelegate::FastDelegate0<u32> LuaGCMemory;
	fastdelegate::FastDelegate1<SLuaGCStatus*> LuaGCStatus;
	fastdelegate::FastDelegate0<void> LuaGCDebug;
	fastdelegate::FastDelegate0<void> LuaProfileFrameBegin;
	fastdelegate::FastDelegate0<void> LuaProfileFrameEnd;

	// Dependent classes
	//CResourceManager* Resources;

	CStats* Statistic;

	// Engine flow-control
	//float fTimeDelta;
	//float fTimeGlobal;
	//u32 dwTimeDelta;
	//u32 dwTimeGlobal;
	//u32 dwTimeContinual;

	// Cameras & projection
	//Fvector vCameraPosition;
	//Fvector vCameraDirection;
	//Fvector vCameraTop;
	//Fvector vCameraRight;

	//Fmatrix mView;
	//Fmatrix mProject;
	//Fmatrix mFullTransform;

	Fmatrix mInvView;
	Fmatrix mInvProject;
	Fmatrix mInvProjectHud;
	Fmatrix mInvFullTransform;

	CSecondVPParams m_SecondViewport;	//--#SM+#-- +SecondVP+

	//float fFOV;
	//float fASPECT;

	CRenderDevice()
		:
		m_pRender(0)
#ifdef INGAME_EDITOR
        , m_editor_module(0),
        m_editor_initialize(0),
        m_editor_finalize(0),
        m_editor(0),
        m_engine(0)
#endif // #ifdef INGAME_EDITOR
	{
		m_hWnd = NULL;
		b_is_Active = FALSE;
		b_is_Ready = FALSE;
		b_hide_cursor = FALSE;
		Timer.Start();
		m_bNearer = FALSE;
		isRendering = FALSE;
		mt_bMustExit = FALSE;
		mt_frame_submitted = FALSE;
		mt_scheduler_deferred_pending = FALSE;
		LuaGCCount = 0;
		LuaGCDone = false;
		LuaGCCycleActive = false;
		LuaGCBudgetHit = false;
		LuaGCCallCapHit = false;
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
		LuaGCAtomicWeakTables = 0;
		LuaGCAtomicWeakSlots = 0;
		LuaGCUdataAllocatedSinceCycle = 0;
		LuaGCNativeCycleActive = false;
		LuaGCAssistingNativeCycle = false;
		LuaGCNativeState = 0;
		LuaGCDebtKB = 0;
		LuaGCThresholdKB = 0;
		LuaGCBudgetUS = 0;
		LuaGCBudgetUsedUS = 0;
		LuaGCBudgetUtilPercent = 0;
		LuaGCCycleAgeFrames = 0;
		LuaGCProgressKB = 0;
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

		m_SecondViewport.SetSVPActive(false);
		m_SecondViewport.SetSVPFrameDelay(2);
		m_SecondViewport.isCamReady = false;			
	};

	void Pause(BOOL bOn, BOOL bTimer, BOOL bSound, LPCSTR reason);
	bool Paused();

	// Scene control
	void PreCache(u32 amount, bool b_draw_loadscreen, bool b_wait_user_input);
	u32 LevelPrecacheFrames() const;
	BOOL Begin();
	void Clear();
	void End();
	void FrameMove();
	void QueueSchedulerDeferredWork();
	void SubmitFrameParallelWork();
	void WaitFrameParallelWork();
	void ProcessFrameParallelWork();
	void ProcessRenderPrepParallelWork();
	void ProcessLuaGCWork(bool allow_deferred_phase);
	void UpdateLuaGCCycleFeedback(u32 memory_after_kb);
	void ResetLuaGCAdaptiveState();

	void overdrawBegin();
	void overdrawEnd();

	//Console Screenshot
	void Screenshot();

	// Mode control
	void DumpFlags();
	IC CTimer_paused* GetTimerGlobal() { return &TimerGlobal; }
	u32 TimerAsync() { return TimerGlobal.GetElapsed_ms(); }
	u32 TimerAsync_MMT() { return TimerMM.GetElapsed_ms() + Timer_MM_Delta; }

	// Creation & Destroying
	void ConnectToRender();
	void Create(void);
	void Run(void);
	void Destroy(void);
	void Reset(bool precache = true);

	bool ChangeOutputMonitor(HMONITOR hTargetMon);

	void Initialize(void);
	void ShutDown(void);

public:
	void time_factor(const float& time_factor)
	{
		Timer.time_factor(time_factor);
		TimerGlobal.time_factor(time_factor);
	}

	IC const float& time_factor() const
	{
		VERIFY(Timer.time_factor() == TimerGlobal.time_factor());
		return (Timer.time_factor());
	}

	Fvector& hud_to_world(Fvector& v, const Fmatrix& p)
	{
		mView.transform_tiny(v);
		p.transform_tiny(v);

		v.z -= ViewportNear;

		mInvProject.transform_tiny(v);
		mInvView.transform_tiny(v);

		return v;
	}

	Fvector& hud_to_world(Fvector& v)
	{
		return hud_to_world(v, mProjectHud);
	}

	Fvector& hud_to_world_dir(Fvector& v, const Fmatrix& p)
	{
		mView.transform_dir(v);
		p.transform_dir(v);

		mInvProject.transform_dir(v);
		mInvView.transform_dir(v);

		return v;
	}

	Fvector& hud_to_world_dir(Fvector& v)
	{
		return hud_to_world_dir(v, mProjectHud);
	}

	Fmatrix& hud_to_world(Fmatrix& m, const Fmatrix& p)
	{
		hud_to_world(m.c, p);
		hud_to_world_dir(m.i, p).normalize();
		hud_to_world_dir(m.j, p).normalize();
		hud_to_world_dir(m.k, p).normalize();
		return m;
	}

	Fmatrix& hud_to_world(Fmatrix& m)
	{
		return hud_to_world(m, mProjectHud);
	}

	Fvector& world_to_hud(Fvector& v, const Fmatrix& p)
	{
		mInvView.transform_tiny(v);
		mInvProject.transform_tiny(v);

		v.z += ViewportNear;

		p.transform_tiny(v);
		mView.transform_tiny(v);
		return v;
	}

	Fvector& world_to_hud(Fvector& v)
	{
		return world_to_hud(v, mProjectHud);
	}

	Fvector& world_to_hud_dir(Fvector& v, const Fmatrix& p)
	{
		mInvView.transform_dir(v);
		mInvProject.transform_dir(v);

		p.transform_dir(v);
		mView.transform_dir(v);

		return v;
	}

	Fvector& world_to_hud_dir(Fvector& v)
	{
		return world_to_hud_dir(v, mProjectHud);
	}

	Fmatrix& world_to_hud(Fmatrix& m, const Fmatrix& p)
	{
		world_to_hud(m.c, p);
		world_to_hud_dir(m.i, p).normalize();
		world_to_hud_dir(m.j, p).normalize();
		world_to_hud_dir(m.k, p).normalize();
		return m;
	}

	Fmatrix& world_to_hud(Fmatrix& m)
	{
		return world_to_hud(m, mProjectHud);
	}

	// Multi-threading
	xr_jobs::task_group mt_frame_group;
	volatile LONG mt_frame_submitted;
	volatile LONG mt_scheduler_deferred_pending;
	volatile LONG mt_bMustExit;
	u32 lua_gc_baseline_kb;
	u32 lua_gc_last_memory_kb;
	u32 lua_gc_budget_hit_streak;
	u32 lua_gc_adaptive_step_kb;
	u32 lua_gc_last_step_us;
	u32 lua_gc_cycle_start_memory_kb;
	u32 lua_gc_cycle_growth_kb;
	u32 lua_gc_growth_scale;
	u32 lua_gc_cycle_start_frame;
	u64 lua_gc_udata_baseline_serial;
	bool lua_gc_cycle_started;

	ICF void remove_from_seq_parallel(const fastdelegate::FastDelegate0<>& delegate)
	{
		xrCriticalSectionGuard guard(seqParallelLock);
		xr_vector<fastdelegate::FastDelegate0<>>::iterator queued = std::find(
			seqParallel.begin(), seqParallel.end(), delegate);
		if (queued != seqParallel.end())
		{
			const size_t index = static_cast<size_t>(queued - seqParallel.begin());
			seqParallel.erase(queued);
			if (index < seqParallelNames.size())
				seqParallelNames.erase(seqParallelNames.begin() + index);
			if (index < seqParallelLanes.size())
				seqParallelLanes.erase(seqParallelLanes.begin() + index);
		}

		xr_vector<fastdelegate::FastDelegate0<>>::iterator active = std::find(
			seqParallelWork.begin(), seqParallelWork.end(), delegate);
		if (active != seqParallelWork.end())
		{
			const size_t index = static_cast<size_t>(active - seqParallelWork.begin());
			*active = fastdelegate::FastDelegate0<>();
			if (index < seqParallelWorkNames.size())
				seqParallelWorkNames[index] = "removed";
		}
	}

	ICF void add_to_seq_parallel(const fastdelegate::FastDelegate0<>& delegate, LPCSTR name,
		EFrameParallelLane lane = EFrameParallelLane::Gameplay)
	{
		xrCriticalSectionGuard guard(seqParallelLock);
		seqParallel.push_back(delegate);
		seqParallelNames.push_back(name ? name : "unnamed");
		seqParallelLanes.push_back(lane);
	}

	ICF void add_to_seq_parallel_independent(const fastdelegate::FastDelegate0<>& delegate)
	{
		xrCriticalSectionGuard guard(seqParallelIndependentLock);
		seqParallelIndependent.push_back(delegate);
	}

	ICF void remove_from_seq_parallel_independent(const fastdelegate::FastDelegate0<>& delegate)
	{
		xrCriticalSectionGuard guard(seqParallelIndependentLock);
		xr_vector<fastdelegate::FastDelegate0<>>::iterator queued = std::find(
			seqParallelIndependent.begin(), seqParallelIndependent.end(), delegate);
		if (queued != seqParallelIndependent.end())
			seqParallelIndependent.erase(queued);

		xr_vector<fastdelegate::FastDelegate0<>>::iterator active = std::find(
			seqParallelIndependentWork.begin(), seqParallelIndependentWork.end(), delegate);
		if (active != seqParallelIndependentWork.end())
			*active = fastdelegate::FastDelegate0<>();
	}

	ICF void add_to_seq_render_prep_parallel(const fastdelegate::FastDelegate0<>& delegate, u32 work_units)
	{
		xrCriticalSectionGuard guard(seqRenderPrepParallelLock);
		seqRenderPrepParallel.push_back(delegate);
		seqRenderPrepParallelUnits.push_back(work_units);
	}

	ICF void remove_from_seq_render_prep_parallel(const fastdelegate::FastDelegate0<>& delegate)
	{
		xrCriticalSectionGuard guard(seqRenderPrepParallelLock);
		xr_vector<fastdelegate::FastDelegate0<>>::iterator queued = std::find(
			seqRenderPrepParallel.begin(), seqRenderPrepParallel.end(), delegate);
		if (queued != seqRenderPrepParallel.end())
		{
			const size_t index = static_cast<size_t>(queued - seqRenderPrepParallel.begin());
			seqRenderPrepParallel.erase(queued);
			if (index < seqRenderPrepParallelUnits.size())
				seqRenderPrepParallelUnits.erase(seqRenderPrepParallelUnits.begin() + index);
		}

		xr_vector<fastdelegate::FastDelegate0<>>::iterator active = std::find(
			seqRenderPrepParallelWork.begin(), seqRenderPrepParallelWork.end(), delegate);
		if (active != seqRenderPrepParallelWork.end())
		{
			const size_t index = static_cast<size_t>(active - seqRenderPrepParallelWork.begin());
			*active = fastdelegate::FastDelegate0<>();
			if (index < seqRenderPrepParallelWorkUnits.size())
				seqRenderPrepParallelWorkUnits[index] = 0;
		}
	}

	//AVO: elapsed famed counter (by alpet)
	IC u32 frame_elapsed()
	{
		return frame_timer.GetElapsed_ms();
	}

	// demonized: Perceivable distance depending on FOV, so that objects will behave normal in binoculars
	IC float GetPerceivedDist(const Fvector& p, float* real_dist = nullptr)
	{
		float dist = vCameraPosition.distance_to(p);
		float fov_rad = deg2rad(fFOV);
		float perceived_dist = dist * tanf(fov_rad * 0.5f);
		if (real_dist) *real_dist = dist;
		return perceived_dist;
	}

	IC float CalcSSADynamic(const Fvector& C, float R)
	{
		Fvector4 v_res1, v_res2;
		mFullTransform.transform(v_res1, C);
		mFullTransform.transform(v_res2, Fvector(C).mad(vCameraRight, R));
		return v_res1.sub(v_res2).magnitude();
	}

public:
	void xr_stdcall on_idle();
	bool xr_stdcall on_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
	void message_loop();
	virtual void _BCL AddSeqFrame(pureFrame* f, bool mt);
	virtual void _BCL RemoveSeqFrame(pureFrame* f);
	virtual CStatsPhysics* _BCL StatPhysics() { return Statistic; }

private:
	xr_imgui::ide m_imgui;

public:
	xr_imgui::ide& imgui() { return m_imgui; }
	bool imgui_shown() const { return m_imgui.is_shown(); }
#ifdef INGAME_EDITOR
public:
    IC editor::ide* editor() const { return m_editor; }

private:
    void initialize_editor();
    void message_loop_editor();

private:
    typedef editor::initialize_function_ptr initialize_function_ptr;
    typedef editor::finalize_function_ptr finalize_function_ptr;

private:
    HMODULE m_editor_module;
    initialize_function_ptr m_editor_initialize;
    finalize_function_ptr m_editor_finalize;
    editor::ide* m_editor;
    engine_impl* m_engine;
#endif // #ifdef INGAME_EDITOR
};

extern ENGINE_API CRenderDevice Device;

#ifndef _EDITOR
#define RDEVICE Device
#else
#define RDEVICE EDevice
#endif

#ifdef ECO_RENDER
extern ENGINE_API float refresh_rate;
#endif // ECO_RENDER

extern ENGINE_API bool g_bBenchmark;

typedef fastdelegate::FastDelegate0<bool> LOADING_EVENT;
extern ENGINE_API xr_list<LOADING_EVENT> g_loading_events;

class ENGINE_API CLoadScreenRenderer : public pureRender
{
public:
	CLoadScreenRenderer();
	void start(bool b_user_input);
	void stop();
	virtual void OnRender();

	bool b_registered;
	bool b_need_user_input;
};

extern ENGINE_API CLoadScreenRenderer load_screen_renderer;
#endif
