#ifndef XRSHEDULER_H_INCLUDED
#define XRSHEDULER_H_INCLUDED

#include "ISheduled.h"

class ENGINE_API CSheduler
{
private:
	struct Item
	{
		u32 dwTimeForExecute;
		u32 dwTimeOfLastExecute;
		shared_str scheduled_name;
		ISheduled* Object;
		u32 last_cost_us;
		u32 ewma_cost_us;
		u32 cost_class_index;
		u16 budget_deferrals;
		u16 slow_strikes;

		IC bool operator <(Item& I)
		{
			return dwTimeForExecute > I.dwTimeForExecute;
		}
	};

	struct ItemReg
	{
		BOOL OP;
		BOOL RT;
		ISheduled* Object;
	};

	struct ClassCostStats
	{
		shared_str name;
		u32 last_cost_us;
		u32 ewma_cost_us;
		u32 last_seen_frame;
		u16 slow_hits;
		u16 reserved;
	};

private:
	xr_vector<Item> ItemsRT;
	xr_vector<Item> ItemsMainThread;
	xr_vector<Item> Items;
	xr_vector<ItemReg> Registration;
	xr_vector<u32> m_registration_previous;
	xr_unordered_map<ISheduled*, u32> m_registration_pending_index;
	xr_vector<ClassCostStats> m_class_costs;
	xr_unordered_map<shared_str, u32> m_class_cost_index;
	xrSRWLock m_items_lock;
	xrSRWLock m_registration_lock;
	xrSRWLock m_class_cost_lock;
	ISheduled* volatile m_current_step_obj;
	volatile LONG m_current_step_registered;
	volatile LONG m_processing_thread_id;
	volatile LONG m_frame_open;
	volatile LONG m_heavy_callbacks_started;
	bool m_items_rt_dirty;
	bool m_items_main_dirty;
	bool m_items_deferred_dirty;
	// The split MT path spans unrelated level callbacks between UpdateInit() and
	// UpdateFinalize(). Accumulate only scheduler work instead of timing that
	// whole wall-clock window and misreporting level/Lua work as scheduler load.
	__declspec(align(8)) volatile LONG64 m_frame_scheduler_cycles;

	IC void Push(xr_vector<Item>& queue, Item& I);
	IC void Pop(xr_vector<Item>& queue);
	IC Item& Top(xr_vector<Item>& queue)
	{
		return queue.front();
	}

	IC ISheduled* CurrentStepObject() const
	{
		ISheduled* volatile* current = const_cast<ISheduled* volatile*>(&m_current_step_obj);
		return static_cast<ISheduled*>(InterlockedCompareExchangePointer(
			reinterpret_cast<PVOID volatile*>(current), nullptr, nullptr));
	}

	void BeginCurrentStep(ISheduled* object);
	bool EndCurrentStep(ISheduled* object);
	void WaitCurrentStep(ISheduled* object) const;
	void internal_Register(ISheduled* A, BOOL RT = FALSE);
	bool internal_Unregister(ISheduled* A, BOOL RT, bool warn_on_not_found = true);
	void internal_Registration();
	void CompactRealtimeItems();
	u32 ResolveCostClass(ISheduled* object, const shared_str& scheduled_name);
	u32 PredictClassCost(u32 class_index, u16* slow_hits = nullptr) const;
	u16 UpdateClassCost(u32 class_index, u32 actual_cost_us, u32 slow_threshold_us);
	void ProcessQueue(xr_vector<Item>& queue, LPCSTR queue_name);

public:
	u64 cycles_start;
	u64 cycles_limit;

public:
	void ProcessStep();
	void ProcessMainThreadStep();
	void Process();
	void Update();
	void UpdateInit();
	void UpdateRT();
	void UpdateMainThread();
	void UpdateDeferred();
	void UpdateFinalize();

#ifdef DEBUG
	bool Registered(ISheduled* object) const;
#endif // DEBUG
	void Register(ISheduled* A, BOOL RT = FALSE);
	void Unregister(ISheduled* A);
	void EnsureOrder(ISheduled* Before, ISheduled* After);

	void Initialize();
	void Destroy();
};

extern ENGINE_API BOOL mt_Scheduler;
extern ENGINE_API int psShedulerBatchSize;
extern ENGINE_API int psShedulerMainBudgetMs;
extern ENGINE_API int psShedulerDeferredBudgetMs;
extern ENGINE_API int psShedulerJitterMs;
extern ENGINE_API BOOL psShedulerLog;
extern ENGINE_API BOOL psShedulerAdaptive;
extern ENGINE_API int psShedulerHeavyThresholdUs;
extern ENGINE_API int psShedulerMaxHeavyPerFrame;
extern ENGINE_API int psShedulerMaxDeferrals;
extern ENGINE_API BOOL psShedulerClassPrediction;
extern ENGINE_API BOOL psShedulerHeavyBackoff;
extern ENGINE_API int psShedulerBackoffThresholdUs;

#endif // XRSHEDULER_H_INCLUDED
