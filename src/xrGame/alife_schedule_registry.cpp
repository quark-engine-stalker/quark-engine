////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_schedule_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife schedule registry
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_schedule_registry.h"

int psALifeSmartTailQos = 1;
int psALifeSmartSlowThresholdMs = 8;
int psALifeSmartCooldownMs = 250;
int psALifeSmartMaxCooldownMs = 30000;
int psALifeSmartBudgetDeferral = 1;
int psALifeSmartHardDeferMs = 500;

CALifeScheduleRegistry::~CALifeScheduleRegistry()
{}

void CALifeScheduleRegistry::add(CSE_ALifeDynamicObject* object)
{
	CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(object);
	if (!schedulable)
		return;

	if (!schedulable->need_update(object))
		return;

	inherited::add(object->ID, schedulable);
	if (object->cast_smart_zone())
	{
		SSmartRuntime runtime = {};
		m_smart_runtime.insert(std::make_pair(object->ID, runtime));
	}
}

void CALifeScheduleRegistry::remove(CSE_ALifeDynamicObject* object, bool no_assert)
{
	CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(object);
	if (!schedulable)
		return;

	inherited::remove(object->ID, no_assert || !schedulable->need_update(object));
	m_smart_runtime.erase(object->ID);
}

void CALifeScheduleRegistry::update()
{
	if (empty())
		return;

	start_timer();
	++m_cycle_count;
	VERIFY(next() != m_objects.end());

	const u32 total_objects = static_cast<u32>(m_objects.size());
	const u32 update_limit = _max(m_objects_per_update, u32(1));
	const u32 threshold_us = static_cast<u32>(_max(psALifeSmartSlowThresholdMs, 1)) * 1000u;
	u32 scanned = 0;
	u32 updated = 0;

	while (!m_objects.empty() && scanned < total_objects && updated < update_limit && !time_over())
	{
		_iterator current = next();
		if (current == m_objects.end())
			break;
		update_next();
		++scanned;

		CSE_ALifeSchedulable* const scheduled_object = current->second;
		if (!scheduled_object || scheduled_object->m_schedule_counter == m_cycle_count)
			continue;

		scheduled_object->m_schedule_counter = m_cycle_count;
		CSE_Abstract* const base = scheduled_object->base();
		const ALife::_OBJECT_ID object_id = base ? base->ID : ALife::_OBJECT_ID(-1);
		const bool smart_object = base && base->cast_smart_zone();

		if (psALifeSmartTailQos && smart_object)
		{
			auto runtime = m_smart_runtime.find(object_id);
			if (runtime != m_smart_runtime.end())
			{
				SSmartRuntime& state = runtime->second;
				if (static_cast<s32>(Device.dwTimeGlobal - state.next_allowed_time) < 0)
					continue;

				// A learned 10-20 ms Lua smart-terrain callback cannot be interrupted by
				// the outer ALife deadline. If the current ALife slice has less time left
				// than the callback's observed cost, defer it briefly instead of stacking
				// the tail on top of switching/other scheduled work. A hard deadline keeps
				// gameplay semantics from starving an expensive smart terrain forever.
				if (psALifeSmartBudgetDeferral && state.ewma_cost_us >= threshold_us &&
					m_process_deadline && CPU::qpc_freq)
				{
					const u64 now_ticks = CPU::QPC();
					const u64 remaining_ticks = m_process_deadline > now_ticks ?
						m_process_deadline - now_ticks : 0ull;
					const u32 remaining_us = static_cast<u32>(_min<u64>(
						remaining_ticks * 1000000ull / CPU::qpc_freq, u32(-1)));
					const u32 reserve_us = _min(_max(state.ewma_cost_us, threshold_us), 20000u);
					const u32 hard_defer_ms = static_cast<u32>(_max(psALifeSmartHardDeferMs, 0));
					if (remaining_us < reserve_us)
					{
						if (!state.budget_defer_started)
							state.budget_defer_started = Device.dwTimeGlobal ? Device.dwTimeGlobal : 1u;
						const u32 deferred_ms = Device.dwTimeGlobal - state.budget_defer_started;
						if (hard_defer_ms && deferred_ms < hard_defer_ms)
						{
							if (state.budget_defers < u16(-1))
								++state.budget_defers;
							continue;
						}
					}
					state.budget_defer_started = 0;
					state.budget_defers = 0;
				}
			}
		}

		const bool measure = CPU::qpc_freq && (psALifeSmartTailQos && smart_object);
		const u64 started_ticks = measure ? CPU::QPC() : 0;

		START_PROFILE("ALife/scheduled/update")
			scheduled_object->update();
		STOP_PROFILE
		++updated;

		if (!started_ticks)
			continue;

		const u64 elapsed_ticks = CPU::QPC() - started_ticks;
		const u32 actual_cost_us = static_cast<u32>(_min<u64>(
			(elapsed_ticks * 1000000ull) / CPU::qpc_freq, u32(-1)));

		if (!psALifeSmartTailQos || !smart_object)
			continue;

		// The callback may remove itself. Update runtime state only if the same object
		// is still registered after returning from script code.
		const auto still_present = m_objects.find(object_id);
		if (still_present == m_objects.end() || still_present->second != scheduled_object)
			continue;

		SSmartRuntime& runtime = m_smart_runtime[object_id];
		runtime.budget_defer_started = 0;
		runtime.budget_defers = 0;
		runtime.last_cost_us = actual_cost_us;
		runtime.ewma_cost_us = runtime.ewma_cost_us
			? static_cast<u32>((static_cast<u64>(runtime.ewma_cost_us) * 7ull + actual_cost_us) / 8ull)
			: actual_cost_us;

		if (actual_cost_us < threshold_us)
		{
			if (runtime.slow_strikes)
				--runtime.slow_strikes;
			continue;
		}

		if (runtime.slow_strikes < u16(-1))
			++runtime.slow_strikes;

		u32 cooldown = static_cast<u32>(_max(psALifeSmartCooldownMs, 0));
		if (actual_cost_us >= 250000u)
			cooldown = _max(cooldown, 5000u);
		else if (actual_cost_us >= 128000u)
			cooldown = _max(cooldown, 3000u);
		else if (actual_cost_us >= 32000u)
			cooldown = _max(cooldown, 1000u);
		else if (actual_cost_us >= 16000u)
			cooldown = _max(cooldown, 750u);
		else if (actual_cost_us >= threshold_us)
			cooldown = _max(cooldown, 500u);

		const u32 shift = _min<u32>(runtime.slow_strikes > 0 ? runtime.slow_strikes - 1u : 0u, 3u);
		const u32 max_cooldown = static_cast<u32>(_max(psALifeSmartMaxCooldownMs, 0));
		if (shift && cooldown <= u32(-1) >> shift)
			cooldown <<= shift;
		if (max_cooldown)
			cooldown = _min(cooldown, max_cooldown);
		const u32 jitter = object_id % 33u;
		runtime.next_allowed_time = Device.dwTimeGlobal + cooldown + jitter;

		// A single pathological smart-terrain callback already exhausted the useful
		// slice. Stop here instead of stacking more A-Life work in the same pass.
		break;
	}

	m_first_update = false;
}
