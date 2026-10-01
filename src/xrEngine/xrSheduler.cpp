#include "stdafx.h"
#include "xrSheduler.h"
#include "xr_object.h"
#include "Feel_Vision.h"

//#define DEBUG_SCHEDULER

float psShedulerCurrent = 0.f;
int psShedulerMaxBudgetMs = 12;
ENGINE_API BOOL mt_Scheduler = TRUE;
ENGINE_API int psShedulerBatchSize = 512;
ENGINE_API int psShedulerMainBudgetMs = 6;
ENGINE_API int psShedulerDeferredBudgetMs = 6;
ENGINE_API int psShedulerJitterMs = 12;
ENGINE_API BOOL psShedulerLog = FALSE;
ENGINE_API BOOL psShedulerAdaptive = TRUE;
ENGINE_API int psShedulerHeavyThresholdUs = 1750;
ENGINE_API int psShedulerMaxHeavyPerFrame = 1;
ENGINE_API int psShedulerMaxDeferrals = 8;
ENGINE_API BOOL psShedulerClassPrediction = TRUE;
ENGINE_API BOOL psShedulerHeavyBackoff = TRUE;
// Dense-combat traces contain dozens of 3-7 ms NPC Lua callbacks. Waiting for
// an individual callback to exceed 8 ms misses most of that sustained load.
// Backoff never leaves the object's declared [t_min, t_max] interval, so the
// lower threshold reduces redundant update frequency without changing the
// scheduler ABI or requiring any GAMMA/Anomaly script changes.
ENGINE_API int psShedulerBackoffThresholdUs = 3000;
BOOL g_bSheduleInProgress = FALSE;

namespace
{}

//-------------------------------------------------------------------------------------
void CSheduler::Initialize()
{
	m_current_step_obj = nullptr;
	m_current_step_registered = FALSE;
	m_processing_thread_id = 0;
	m_frame_open = FALSE;
	m_heavy_callbacks_started = 0;
	m_items_rt_dirty = false;
	m_items_main_dirty = false;
	m_items_deferred_dirty = false;
	m_frame_scheduler_cycles = 0;
	cycles_start = 0;
	cycles_limit = 0;

	// Keep scheduler bookkeeping allocations out of active gameplay. Microsoft
	// recommends preallocation/pooling for time-sensitive paths; registration is
	// the natural non-critical point for growing these containers.
	ItemsRT.reserve(512);
	ItemsMainThread.reserve(4096);
	Items.reserve(4096);
	Registration.reserve(1024);
	m_registration_previous.reserve(1024);
	m_registration_pending_index.reserve(1024);
	m_class_costs.reserve(2048);
	m_class_cost_index.reserve(2048);
}

void CSheduler::Destroy()
{
	// A deferred scheduler frame is normally joined by the device before engine
	// shutdown. Keep this assertion local as a guard against future lifecycle edits.
	R_ASSERT(InterlockedCompareExchange(&m_frame_open, FALSE, FALSE) == FALSE);
	internal_Registration();

	{
		xrSRWLockGuard guard(m_items_lock);
		Items.erase(std::remove_if(Items.begin(), Items.end(), [](const Item& item)
			{
				return item.Object == nullptr;
			}), Items.end());
#ifdef DEBUG
		if (!Items.empty())
		{
			Msg("! Sheduler work-list is not empty");
			for (u32 it = 0; it < Items.size(); ++it)
				if (Items[it].Object)
					Msg("%s", Items[it].Object->shedule_Name().c_str());
		}
#endif // DEBUG
		ItemsRT.clear();
		ItemsMainThread.clear();
		Items.clear();
	}

	{
		xrSRWLockGuard guard(m_registration_lock);
		Registration.clear();
	}
	m_registration_previous.clear_and_free();
	m_registration_pending_index.clear();

	{
		xrSRWLockGuard guard(m_class_cost_lock);
		m_class_costs.clear();
		m_class_cost_index.clear();
	}
}

u32 CSheduler::ResolveCostClass(ISheduled* object, const shared_str& scheduled_name)
{
	shared_str class_name = scheduled_name;
	if (CObject* game_object = fast_dynamic_cast<CObject*>(object))
	{
		const shared_str section = game_object->cNameSect();
		if (section.c_str() && section.c_str()[0])
			class_name = section;
	}
	if (!class_name.c_str() || !class_name.c_str()[0])
		class_name = "<unknown>";

	xrSRWLockGuard guard(m_class_cost_lock);
	const auto found = m_class_cost_index.find(class_name);
	if (found != m_class_cost_index.end())
		return found->second;

	ClassCostStats stats = {};
	stats.name = class_name;
	const u32 index = static_cast<u32>(m_class_costs.size());
	m_class_costs.push_back(stats);
	m_class_cost_index.try_emplace(class_name, index);
	return index;
}

u32 CSheduler::PredictClassCost(u32 class_index, u16* slow_hits) const
{
	if (!psShedulerClassPrediction)
	{
		if (slow_hits)
			*slow_hits = 0;
		return 0;
	}

	xrSRWLockGuard guard(const_cast<xrSRWLock&>(m_class_cost_lock), true);
	if (class_index >= m_class_costs.size())
	{
		if (slow_hits)
			*slow_hits = 0;
		return 0;
	}

	const ClassCostStats& stats = m_class_costs[class_index];
	if (slow_hits)
		*slow_hits = stats.slow_hits;
	// A single transient callback must not poison cold-start prediction for every
	// object in the same section. The class EWMA already tracks sustained cost and
	// is intentionally slower-moving than the last sample.
	u32 predicted = stats.ewma_cost_us ? stats.ewma_cost_us : stats.last_cost_us;
	const u32 age = Device.dwFrame - stats.last_seen_frame;
	if (age > 3600)
		predicted /= 4;
	else if (age > 900)
		predicted /= 2;
	return predicted;
}

u16 CSheduler::UpdateClassCost(u32 class_index, u32 actual_cost_us, u32 slow_threshold_us)
{
	if (!psShedulerClassPrediction)
		return 0;

	xrSRWLockGuard guard(m_class_cost_lock);
	if (class_index >= m_class_costs.size())
		return 0;

	ClassCostStats& stats = m_class_costs[class_index];
	stats.last_cost_us = actual_cost_us;
	stats.ewma_cost_us = stats.ewma_cost_us
		? static_cast<u32>((static_cast<u64>(stats.ewma_cost_us) * 7ull + actual_cost_us) / 8ull)
		: actual_cost_us;
	stats.last_seen_frame = Device.dwFrame;
	if (actual_cost_us >= slow_threshold_us)
	{
		if (stats.slow_hits < u16(-1))
			++stats.slow_hits;
	}
	else if (stats.slow_hits)
	{
		--stats.slow_hits;
	}
	return stats.slow_hits;
}

void CSheduler::BeginCurrentStep(ISheduled* object)
{
	R_ASSERT(object);
	R_ASSERT(CurrentStepObject() == nullptr);
	InterlockedExchange(&m_current_step_registered, TRUE);
	InterlockedExchangePointer(
		reinterpret_cast<PVOID volatile*>(&m_current_step_obj), object);
}

bool CSheduler::EndCurrentStep(ISheduled* object)
{
	R_ASSERT(CurrentStepObject() == object);
	const bool registered = InterlockedCompareExchange(
		&m_current_step_registered, FALSE, FALSE) != FALSE;
	InterlockedExchangePointer(
		reinterpret_cast<PVOID volatile*>(&m_current_step_obj), nullptr);
	return registered;
}

void CSheduler::WaitCurrentStep(ISheduled* object) const
{
	while (CurrentStepObject() == object)
	{
		SwitchToThread();
	}
}

void CSheduler::internal_Registration()
{
	// Unregister must observe either the pending request or its materialized
	// queue item. Keep the swap and materialization in one registration critical
	// section so an object cannot be destroyed between those two states.
	xrSRWLockGuard registration_guard(m_registration_lock);
	xr_vector<ItemReg> registration;
	registration.swap(Registration);

	// Pair transient register/unregister requests in linear time. The legacy
	// forward scan erased from the middle of the vector and became O(n^2) during
	// large ALife onlining bursts.
	m_registration_previous.assign(registration.size(), u32(-1));
	m_registration_pending_index.clear();
	for (u32 it = 0; it < registration.size(); ++it)
	{
		ItemReg& R = registration[it];
		if (R.OP)
		{
			const auto found = m_registration_pending_index.find(R.Object);
			if (found != m_registration_pending_index.end())
				m_registration_previous[it] = found->second;
			m_registration_pending_index[R.Object] = it;
		}
		else
		{
			const auto found = m_registration_pending_index.find(R.Object);
			if (found == m_registration_pending_index.end())
				continue;

			const u32 register_index = found->second;
			registration[register_index].Object = nullptr;
			R.Object = nullptr;
			const u32 previous = m_registration_previous[register_index];
			if (previous == u32(-1))
				m_registration_pending_index.erase(found);
			else
				found->second = previous;
		}
	}

	for (u32 it = 0; it < registration.size(); ++it)
	{
		ItemReg& R = registration[it];
		if (!R.Object)
			continue;
		if (R.OP)
			internal_Register(R.Object, R.RT);
		else
			internal_Unregister(R.Object, R.RT);
	}

	registration.clear_not_free();
	m_registration_previous.clear_not_free();
	m_registration_pending_index.clear();
	if (Registration.empty())
		registration.swap(Registration);
}

void CSheduler::internal_Register(ISheduled* O, BOOL RT)
{
	VERIFY(O);
	VERIFY(!O->shedule.b_locked);

	Item next;
	next.dwTimeForExecute = Device.dwTimeGlobal;
	next.dwTimeOfLastExecute = Device.dwTimeGlobal;
	next.Object = O;
	next.scheduled_name = O->shedule_Name();
	next.last_cost_us = 0;
	next.ewma_cost_us = 0;
	next.cost_class_index = ResolveCostClass(O, next.scheduled_name);
	next.budget_deferrals = 0;
	next.slow_strikes = 0;
	O->shedule.b_RT = RT;

	xrSRWLockGuard guard(m_items_lock);
	if (RT)
		ItemsRT.push_back(next);
	else if (O->shedule_AllowDeferred())
		Push(Items, next);
	else
		Push(ItemsMainThread, next);
}

bool CSheduler::internal_Unregister(ISheduled* O, BOOL RT, bool warn_on_not_found)
{
	if (!O)
		return false;

	bool found = false;
	{
		xrSRWLockGuard guard(m_items_lock);
		auto tombstone = [O](xr_vector<Item>& items)
			{
				for (u32 i = 0; i < items.size(); ++i)
				{
					if (items[i].Object == O)
					{
						// A null tombstone keeps heap/vector iterators stable while a scheduler
						// callback is active. It is removed on the next scheduler pass.
						items[i].Object = nullptr;
						return true;
					}
				}
				return false;
			};

		if (RT)
		{
			found = tombstone(ItemsRT);
			if (found)
				m_items_rt_dirty = true;
		}
		else
		{
			found = tombstone(ItemsMainThread);
			if (found)
				m_items_main_dirty = true;
			else
			{
				found = tombstone(Items);
				if (found)
					m_items_deferred_dirty = true;
			}
		}
	}

	if (CurrentStepObject() == O)
	{
		InterlockedExchange(&m_current_step_registered, FALSE);
		found = true;
	}

#ifdef DEBUG
	if (!found && warn_on_not_found)
		Msg("! scheduled object %s tries to unregister but is not registered", *O->shedule_Name());
#endif // DEBUG

	return found;
}

void CSheduler::CompactRealtimeItems()
{
	xrSRWLockGuard guard(m_items_lock);
	if (!m_items_rt_dirty)
		return;
	ItemsRT.erase(std::remove_if(ItemsRT.begin(), ItemsRT.end(), [](const Item& item)
		{
			return item.Object == nullptr;
		}), ItemsRT.end());
	m_items_rt_dirty = false;
}

#ifdef DEBUG
bool CSheduler::Registered(ISheduled* object) const
{
	u32 count = 0;
	{
		xrSRWLockGuard guard(const_cast<xrSRWLock&>(m_items_lock), true);
		for (u32 i = 0; i < ItemsRT.size(); ++i)
			if (ItemsRT[i].Object == object)
				++count;
		for (u32 i = 0; i < ItemsMainThread.size(); ++i)
			if (ItemsMainThread[i].Object == object)
				++count;
		for (u32 i = 0; i < Items.size(); ++i)
			if (Items[i].Object == object)
				++count;
	}

	{
		xrSRWLockGuard guard(const_cast<xrSRWLock&>(m_registration_lock), true);
		for (u32 i = 0; i < Registration.size(); ++i)
		{
			if (Registration[i].Object != object)
				continue;
			if (Registration[i].OP)
				++count;
			else if (count)
				--count;
		}
	}

	if (!count && CurrentStepObject() == object &&
		InterlockedCompareExchange(const_cast<volatile LONG*>(&m_current_step_registered), FALSE, FALSE) != FALSE)
		++count;

	VERIFY(count <= 1);
	return count == 1;
}
#endif // DEBUG

void CSheduler::Register(ISheduled* A, BOOL RT)
{
#ifdef DEBUG
	VERIFY(!Registered(A));
#endif
	ItemReg R;
	R.OP = TRUE;
	R.RT = RT;
	R.Object = A;

	xrSRWLockGuard guard(m_registration_lock);
	R.Object->shedule.b_RT = RT;
	Registration.push_back(R);
}

void CSheduler::Unregister(ISheduled* A)
{
#ifdef DEBUG
	VERIFY(Registered(A));
#endif
	{
		// Serialize the queue lookup with internal_Registration's swap and
		// materialization. A miss can then only be a still-pending registration.
		xrSRWLockGuard guard(m_registration_lock);
		const BOOL realtime = A->shedule.b_RT;
		if (!internal_Unregister(A, realtime, false))
		{
			ItemReg R;
			R.OP = FALSE;
			R.RT = realtime;
			R.Object = A;
			Registration.push_back(R);
		}
	}

	// The callback may itself register or unregister other objects. Never wait
	// for it while holding the registration lock.
	const DWORD scheduler_thread = static_cast<DWORD>(InterlockedCompareExchange(
		&m_processing_thread_id, 0, 0));
	if (scheduler_thread && scheduler_thread != GetCurrentThreadId())
		WaitCurrentStep(A);
}

void CSheduler::EnsureOrder(ISheduled* Before, ISheduled* After)
{
	VERIFY(Before->shedule.b_RT && After->shedule.b_RT);
	xrSRWLockGuard guard(m_items_lock);

	for (u32 i = 0; i < ItemsRT.size(); ++i)
	{
		if (ItemsRT[i].Object == After)
		{
			Item item = ItemsRT[i];
			ItemsRT.erase(ItemsRT.begin() + i);
			ItemsRT.push_back(item);
			return;
		}
	}
}

void CSheduler::Push(xr_vector<Item>& queue, Item& I)
{
	queue.push_back(I);
	std::push_heap(queue.begin(), queue.end());
}

void CSheduler::Pop(xr_vector<Item>& queue)
{
	std::pop_heap(queue.begin(), queue.end());
	queue.pop_back();
}

void CSheduler::ProcessQueue(xr_vector<Item>& queue, LPCSTR queue_name)
{
	const u32 frame_time = Device.dwTimeGlobal;
	const u32 batch_limit = static_cast<u32>(_max(psShedulerBatchSize, 1));
	const u32 heavy_threshold_us = static_cast<u32>(_max(psShedulerHeavyThresholdUs, 500));
	const u32 max_deferrals = static_cast<u32>(_max(psShedulerMaxDeferrals, 0));
	const u32 backoff_threshold_us = static_cast<u32>(_max(psShedulerBackoffThresholdUs, 1000));
	u32 processed = 0;
	u32 deferred_for_budget = 0;

	const auto reserve_heavy_slot = [this]()
		{
			const LONG limit = static_cast<LONG>(_max(psShedulerMaxHeavyPerFrame, 0));
			if (limit <= 0)
				return true;

			for (;;)
			{
				const LONG current = InterlockedCompareExchange(&m_heavy_callbacks_started, 0, 0);
				if (current >= limit)
					return false;
				if (InterlockedCompareExchange(&m_heavy_callbacks_started, current + 1, current) == current)
					return true;
			}
		};

	for (;;)
	{
		if (Device.dwPrecacheFrame == 0)
		{
			if (processed >= batch_limit)
			{
				break;
			}
			if (CPU::QPC() > cycles_limit)
			{
				break;
			}
		}

		Item current;
		bool have_item = false;
		{
			xrSRWLockGuard guard(m_items_lock);
			while (!queue.empty())
			{
				if (Top(queue).dwTimeForExecute >= frame_time)
					break;

				current = Top(queue);
				Pop(queue);
				if (!current.Object)
					continue;

				BeginCurrentStep(current.Object);
				have_item = true;
				break;
			}
		}

		if (!have_item)
			break;

		ISheduled* const object = current.Object;
		const u32 elapsed = frame_time - current.dwTimeOfLastExecute;
		bool needed = object->shedule_Needed();
		bool still_registered = InterlockedCompareExchange(
			&m_current_step_registered, FALSE, FALSE) != FALSE;

		if (needed && still_registered)
		{
			const u32 object_cost_us = _max(current.last_cost_us, current.ewma_cost_us);
			const bool has_object_cost = object_cost_us != 0;
			u16 class_slow_hits = 0;
			const u32 class_cost_us = has_object_cost ? 0u :
				PredictClassCost(current.cost_class_index, &class_slow_hits);
			// Class history is useful only as a cold-start estimate. Once this object has
			// its own timing, using max(object, class) lets one expensive scripted NPC
			// poison every object with the same section name and creates false heavy
			// deferrals. The per-object EWMA is the more specific predictor.
			const u32 predicted_cost_us = has_object_cost ? object_cost_us : class_cost_us;
			const bool predicted_heavy = predicted_cost_us >= heavy_threshold_us;
			bool heavy_slot_reserved = false;
			bool defer_callback = false;

			if (psShedulerAdaptive && Device.dwPrecacheFrame == 0 && predicted_cost_us)
			{
				const u64 now = CPU::QPC();
				const u64 remaining_ticks = cycles_limit > now ? cycles_limit - now : 0;
				const u64 remaining_us = CPU::qpc_freq
					? remaining_ticks * 1000000ull / CPU::qpc_freq : 0;

				// Do not start a callback already known to exceed the remaining slice
				// after other work has run. It remains due and is retried next frame.
				const bool over_remaining_budget = processed && predicted_cost_us > remaining_us;
				if (over_remaining_budget && current.budget_deferrals < max_deferrals)
				{
					defer_callback = true;
				}
				else if (predicted_heavy)
				{
					heavy_slot_reserved = reserve_heavy_slot();
					if (!heavy_slot_reserved && current.budget_deferrals < max_deferrals)
					{
						defer_callback = true;
					}
				}
			}

			if (defer_callback)
			{
				current.dwTimeForExecute = frame_time + 1;
				if (current.budget_deferrals < u16(-1))
					++current.budget_deferrals;

				{
					xrSRWLockGuard guard(m_items_lock);
					still_registered = InterlockedCompareExchange(
						&m_current_step_registered, FALSE, FALSE) != FALSE;
					if (still_registered)
						Push(queue, current);
					R_ASSERT(CurrentStepObject() == object);
					InterlockedExchangePointer(
						reinterpret_cast<PVOID volatile*>(&m_current_step_obj), nullptr);
				}

				++processed;
				++deferred_for_budget;
				continue;
			}

			// Calculate the next interval while the active-callback lifetime gate still
			// protects the object. After shedule_Update returns, an external thread may
			// request unregister and wait for this gate before destroying the object.
			const u32 min_update = _max(u32(30), object->shedule.t_min);
			const u32 max_update = (1000 + object->shedule.t_max) / 2;
			const float scale = object->shedule_Scale();
			u32 next_update = min_update + iFloor(float(max_update - min_update) * scale);
			clamp(next_update, u32(_max(min_update, u32(20))), max_update);

#ifdef DEBUG
			object->dbg_startframe = Device.dwFrame;
#endif // DEBUG

			const bool measure_cost = psShedulerAdaptive || psShedulerClassPrediction ||
				psShedulerHeavyBackoff || psShedulerJitterMs > 0;
			const u64 callback_started = measure_cost ? CPU::QPC() : 0;
			object->shedule_Update(clampr(elapsed, u32(1), u32(_max(u32(object->shedule.t_max), u32(1000)))));
			const u64 callback_finished = callback_started ? CPU::QPC() : 0;

			if (callback_started && CPU::qpc_freq)
			{
				const u64 elapsed_ticks = callback_finished - callback_started;
				const u32 actual_cost_us = static_cast<u32>(_min<u64>(
					(elapsed_ticks * 1000000ull) / CPU::qpc_freq, u32(-1)));
				current.last_cost_us = actual_cost_us;
				current.ewma_cost_us = current.ewma_cost_us
					? static_cast<u32>((static_cast<u64>(current.ewma_cost_us) * 7ull + actual_cost_us) / 8ull)
					: actual_cost_us;
				current.budget_deferrals = 0;
				if (actual_cost_us >= backoff_threshold_us)
				{
					if (current.slow_strikes < u16(-1))
						++current.slow_strikes;
				}
				else if (current.slow_strikes)
				{
					--current.slow_strikes;
				}
				class_slow_hits = UpdateClassCost(current.cost_class_index, actual_cost_us, backoff_threshold_us);

				// Unknown callbacks are allowed once. If one proves heavy, account for it
				// immediately so no other known-heavy callback is stacked behind it.
				if (psShedulerAdaptive && actual_cost_us >= heavy_threshold_us && !heavy_slot_reserved)
					InterlockedIncrement(&m_heavy_callbacks_started);
			}

			if (psShedulerJitterMs > 0 && Device.dwPrecacheFrame == 0 && max_update > min_update)
			{
				const u32 measured_object_cost_us = _max(current.last_cost_us, current.ewma_cost_us);
				const u32 learned_cost_us = measured_object_cost_us ? measured_object_cost_us : class_cost_us;
				if (learned_cost_us >= 250u)
				{
					const u32 spread = max_update - min_update;
					const u32 amplitude = _min(static_cast<u32>(psShedulerJitterMs), spread / 2u);
					if (amplitude)
					{
						const uintptr_t identity = reinterpret_cast<uintptr_t>(object);
						u32 seed = static_cast<u32>((identity >> 4u) ^
							(static_cast<uintptr_t>(current.cost_class_index) * 2654435761u) ^
							(static_cast<uintptr_t>(frame_time) * 2246822519u));
						seed ^= seed >> 16u;
						const u32 window = amplitude * 2u + 1u;
						const int signed_offset = static_cast<int>(seed % window) - static_cast<int>(amplitude);
						const int jittered = static_cast<int>(next_update) + signed_offset;
						next_update = static_cast<u32>(clampr(jittered,
							static_cast<int>(min_update), static_cast<int>(max_update)));
					}
				}
			}

			if (psShedulerHeavyBackoff && Device.dwPrecacheFrame == 0)
			{
				const u32 measured_object_cost_us = _max(current.last_cost_us, current.ewma_cost_us);
				const bool has_measured_object_cost = measured_object_cost_us != 0;
				const u32 learned_cost_us = has_measured_object_cost ? measured_object_cost_us : class_cost_us;
				const bool repeatedly_slow = has_measured_object_cost ?
					current.slow_strikes >= 2 : class_slow_hits >= 2;
				if (learned_cost_us >= backoff_threshold_us && repeatedly_slow)
				{
					// Stay inside the object's declared [t_min, t_max] scheduling envelope,
					// but use its slow end and stable per-object jitter to avoid synchronized bursts.
					const u32 spread = max_update > min_update ? max_update - min_update : 0;
					const u32 slow_floor = min_update + (spread * 3u) / 4u;
					const u32 slow_span = max_update >= slow_floor ? max_update - slow_floor + 1u : 1u;
					const uintptr_t identity = reinterpret_cast<uintptr_t>(object);
					const u32 jitter = static_cast<u32>((identity >> 4u) ^
						(static_cast<uintptr_t>(current.cost_class_index) * 2654435761u));
					next_update = slow_floor + jitter % slow_span;
				}
			}

			// Requeue and release the active object under the same item lock. This closes
			// the window where Unregister could return and destroy the object between
			// EndCurrentStep() and Push().
			{
				xrSRWLockGuard guard(m_items_lock);
				still_registered = InterlockedCompareExchange(
					&m_current_step_registered, FALSE, FALSE) != FALSE;
				if (still_registered)
				{
					current.dwTimeForExecute = frame_time + next_update;
					current.dwTimeOfLastExecute = frame_time;
					Push(queue, current);
				}
				R_ASSERT(CurrentStepObject() == object);
				InterlockedExchangePointer(
					reinterpret_cast<PVOID volatile*>(&m_current_step_obj), nullptr);
			}

		}
		else
		{
			EndCurrentStep(object);
		}

		++processed;
	}

	if (psShedulerLog && (processed || deferred_for_budget))
	{
		u32 pending = 0;
		{
			xrSRWLockGuard guard(m_items_lock, true);
			pending = static_cast<u32>(queue.size());
		}
		const int queue_budget_ms = xr_strcmp(queue_name, "main") == 0 ?
			_min(psShedulerMaxBudgetMs, psShedulerMainBudgetMs) :
			_min(psShedulerMaxBudgetMs, psShedulerDeferredBudgetMs);
		Msg("* Scheduler[%s]: processed=%u, adaptive_defers=%u, pending=%u, budget=%d ms, batch=%d",
			queue_name, processed, deferred_for_budget, pending, queue_budget_ms, psShedulerBatchSize);
	}
}

void CSheduler::ProcessStep()
{
	ProcessQueue(Items, "deferred");
}

void CSheduler::ProcessMainThreadStep()
{
	ProcessQueue(ItemsMainThread, "main");
}

void CSheduler::Process()
{
	ProcessMainThreadStep();
	ProcessStep();
}

void CSheduler::UpdateInit()
{

	R_ASSERT(Device.Statistic);
	R_ASSERT(InterlockedCompareExchange(&m_frame_open, TRUE, FALSE) == FALSE);

	InterlockedExchange64(&m_frame_scheduler_cycles, 0);
	const u64 phase_started = CPU::QPC();
	InterlockedExchange(&m_heavy_callbacks_started, 0);
	Feel::vision_budget_begin_frame();
	internal_Registration();
	g_bSheduleInProgress = TRUE;
	InterlockedExchangeAdd64(&m_frame_scheduler_cycles,
		static_cast<LONG64>(CPU::QPC() - phase_started));
}

void CSheduler::UpdateRT()
{

	const u64 phase_started = CPU::QPC();
	InterlockedExchange(&m_processing_thread_id, static_cast<LONG>(GetCurrentThreadId()));
	const u32 frame_time = Device.dwTimeGlobal;

	for (u32 index = 0;; ++index)
	{
		Item current;
		{
			xrSRWLockGuard guard(m_items_lock, true);
			if (index >= ItemsRT.size())
				break;
			current = ItemsRT[index];
		}

		if (!current.Object)
			continue;

		ISheduled* const object = current.Object;
		BeginCurrentStep(object);
		if (object->shedule_Needed())
		{
			const u32 elapsed = frame_time - current.dwTimeOfLastExecute;
#ifdef DEBUG
			VERIFY(object->dbg_startframe != Device.dwFrame);
			object->dbg_startframe = Device.dwFrame;
#endif // DEBUG

			const bool measure_cost = psShedulerAdaptive || psShedulerClassPrediction ||
				psShedulerHeavyBackoff || psShedulerJitterMs > 0;
			const u64 callback_started = measure_cost ? CPU::QPC() : 0;
			object->shedule_Update(elapsed);
			const u64 callback_finished = callback_started ? CPU::QPC() : 0;
			u32 actual_cost_us = 0;
			if (callback_started && CPU::qpc_freq)
			{
				actual_cost_us = static_cast<u32>(_min<u64>(
					((callback_finished - callback_started) * 1000000ull) / CPU::qpc_freq, u32(-1)));
				current.last_cost_us = actual_cost_us;
				current.ewma_cost_us = current.ewma_cost_us
					? static_cast<u32>((static_cast<u64>(current.ewma_cost_us) * 7ull + actual_cost_us) / 8ull)
					: actual_cost_us;
				const u32 backoff_threshold_us = static_cast<u32>(_max(psShedulerBackoffThresholdUs, 1000));
				if (actual_cost_us >= backoff_threshold_us)
				{
					if (current.slow_strikes < u16(-1))
						++current.slow_strikes;
				}
				else if (current.slow_strikes)
				{
					--current.slow_strikes;
				}
				UpdateClassCost(current.cost_class_index, actual_cost_us, backoff_threshold_us);
				if (psShedulerAdaptive && actual_cost_us >= static_cast<u32>(_max(psShedulerHeavyThresholdUs, 500)))
					InterlockedIncrement(&m_heavy_callbacks_started);
			}
			const bool still_registered = EndCurrentStep(object);

			if (still_registered)
			{
				xrSRWLockGuard guard(m_items_lock);
				if (index < ItemsRT.size() && ItemsRT[index].Object == object)
				{
					ItemsRT[index].dwTimeOfLastExecute = frame_time;
					if (actual_cost_us)
					{
						ItemsRT[index].last_cost_us = current.last_cost_us;
						ItemsRT[index].ewma_cost_us = current.ewma_cost_us;
						ItemsRT[index].slow_strikes = current.slow_strikes;
					}
				}
			}


		}
		else
		{
			EndCurrentStep(object);
			xrSRWLockGuard guard(m_items_lock);
			if (index < ItemsRT.size() && ItemsRT[index].Object == object)
				ItemsRT[index].dwTimeOfLastExecute = frame_time;
		}
	}

	CompactRealtimeItems();
	InterlockedExchange(&m_processing_thread_id, 0);
	InterlockedExchangeAdd64(&m_frame_scheduler_cycles,
		static_cast<LONG64>(CPU::QPC() - phase_started));
}

void CSheduler::UpdateMainThread()
{

	const u64 phase_started = CPU::QPC();
	InterlockedExchange(&m_processing_thread_id, static_cast<LONG>(GetCurrentThreadId()));
	cycles_start = CPU::QPC();
	const u64 budget_ms = static_cast<u64>(_max(_min(psShedulerMaxBudgetMs, psShedulerMainBudgetMs), 1));
	cycles_limit = cycles_start + CPU::qpc_freq * budget_ms / 1000u;
	ProcessMainThreadStep();
	InterlockedExchange(&m_processing_thread_id, 0);
	InterlockedExchangeAdd64(&m_frame_scheduler_cycles,
		static_cast<LONG64>(CPU::QPC() - phase_started));
}

void CSheduler::UpdateDeferred()
{

	const u64 phase_started = CPU::QPC();
	InterlockedExchange(&m_processing_thread_id, static_cast<LONG>(GetCurrentThreadId()));
	cycles_start = CPU::QPC();
	const u64 budget_ms = static_cast<u64>(_max(_min(psShedulerMaxBudgetMs, psShedulerDeferredBudgetMs), 1));
	cycles_limit = cycles_start + CPU::qpc_freq * budget_ms / 1000u;
	ProcessStep();
	InterlockedExchange(&m_processing_thread_id, 0);
	InterlockedExchangeAdd64(&m_frame_scheduler_cycles,
		static_cast<LONG64>(CPU::QPC() - phase_started));
}

void CSheduler::UpdateFinalize()
{

	const u64 phase_started = CPU::QPC();
	internal_Registration();
	{
		xrSRWLockGuard guard(m_items_lock);
		auto compact_heap = [](xr_vector<Item>& items)
			{
				items.erase(std::remove_if(items.begin(), items.end(), [](const Item& item)
					{
						return item.Object == nullptr;
					}), items.end());
				std::make_heap(items.begin(), items.end());
			};
		if (m_items_main_dirty)
		{
			compact_heap(ItemsMainThread);
			m_items_main_dirty = false;
		}
		if (m_items_deferred_dirty)
		{
			compact_heap(Items);
			m_items_deferred_dirty = false;
		}
	}
	Feel::vision_budget_end_frame();
	g_bSheduleInProgress = FALSE;

	InterlockedExchangeAdd64(&m_frame_scheduler_cycles,
		static_cast<LONG64>(CPU::QPC() - phase_started));
	const u64 scheduler_cycles = static_cast<u64>(InterlockedCompareExchange64(
		&m_frame_scheduler_cycles, 0, 0));
	Device.Statistic->Sheduler.Add(scheduler_cycles);
	psShedulerCurrent = static_cast<float>(CPU::qpc_freq ?
		1000.0 * static_cast<double>(scheduler_cycles) / static_cast<double>(CPU::qpc_freq) : 0.0);
	Device.Statistic->fShedulerLoad = psShedulerCurrent;
	R_ASSERT(InterlockedExchange(&m_frame_open, FALSE) == TRUE);
}

void CSheduler::Update()
{
	UpdateInit();
	UpdateRT();

	// Legacy single-thread fallback keeps the same per-queue caps as the split
	// path so a main-thread burst cannot consume the deferred-safe slice too.
	const u64 phase_started = CPU::QPC();
	InterlockedExchange(&m_processing_thread_id, static_cast<LONG>(GetCurrentThreadId()));
	cycles_start = CPU::QPC();
	const u64 main_budget_ms = static_cast<u64>(_max(_min(psShedulerMaxBudgetMs, psShedulerMainBudgetMs), 1));
	cycles_limit = cycles_start + CPU::qpc_freq * main_budget_ms / 1000u;
	{

		ProcessMainThreadStep();
	}
	cycles_start = CPU::QPC();
	const u64 deferred_budget_ms = static_cast<u64>(_max(_min(psShedulerMaxBudgetMs, psShedulerDeferredBudgetMs), 1));
	cycles_limit = cycles_start + CPU::qpc_freq * deferred_budget_ms / 1000u;
	{

		ProcessStep();
	}
	InterlockedExchange(&m_processing_thread_id, 0);
	InterlockedExchangeAdd64(&m_frame_scheduler_cycles,
		static_cast<LONG64>(CPU::QPC() - phase_started));

	UpdateFinalize();
}
