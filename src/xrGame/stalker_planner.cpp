////////////////////////////////////////////////////////////////////////////
//	Module 		: motivation_action_manager_stalker.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker motivation action manager class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stalker_planner.h"
#include "stalker_property_evaluators.h"
#include "stalker_danger_property_evaluators.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_decision_space.h"
#include "script_game_object.h"
#include "stalker_alife_planner.h"
#include "stalker_anomaly_planner.h"
#include "stalker_death_planner.h"
#include "stalker_danger_planner.h"
#include "stalker_alife_actions.h"
#include "stalker_combat_planner.h"
#include "ai_space.h"
#include "../xrServerEntities/script_engine.h"
//#include "stalker_combat_planner_new.h"

//#define GOAP_DEBUG

using namespace StalkerDecisionSpace;

namespace
{
void record_stalker_brain_stage(void* context, LPCSTR stage, LPCSTR action_name,
	const u32 action_id, const u64 elapsed_ticks)
{
	CStalkerPlanner* planner = static_cast<CStalkerPlanner*>(context);
	if (!planner || !stage)
		return;

	CAI_Stalker& stalker = planner->object();
	string512 phase;
	if (action_name && action_name[0])
		_snprintf_s(phase, sizeof(phase), _TRUNCATE, "stalker.brain.%s:%s", stage, action_name);
	else
		_snprintf_s(phase, sizeof(phase), _TRUNCATE, "stalker.brain.%s", stage);

	ai().script_engine().record_native_profile("ai_brain", phase,
		*stalker.cName(), static_cast<u32>(stalker.ID()), elapsed_ticks, action_id);
}

class CStalkerBrainProfileScope : xray::noncopyable
{
public:
	CStalkerBrainProfileScope(CStalkerPlanner* planner, const bool enabled) :
		m_planner(enabled ? planner : nullptr)
	{
		if (m_planner)
			m_planner->set_update_profile_callback(m_planner, &record_stalker_brain_stage);
	}

	~CStalkerBrainProfileScope()
	{
		if (m_planner)
			m_planner->set_update_profile_callback(nullptr, nullptr);
	}

private:
	CStalkerPlanner* m_planner;
};
}

CStalkerPlanner::CStalkerPlanner()
{
	m_affect_cover = false;
}

CStalkerPlanner::~CStalkerPlanner()
{
}

#ifdef LOG_ACTION
LPCSTR CStalkerPlanner::action2string	(const _action_id_type &action_id)
{
	return					(inherited::action2string(action_id));
}

LPCSTR CStalkerPlanner::property2string	(const _condition_type &property_id)
{
	return					(inherited::property2string(property_id));
}

LPCSTR CStalkerPlanner::object_name		() const
{
	VERIFY					(m_object);
	return					(*m_object->cName());
}
#endif

void CStalkerPlanner::setup(CAI_Stalker* object)
{
#ifdef LOG_ACTION
	set_use_log					(!!psAI_Flags.test(aiGOAP));
#endif

	inherited::setup(object);

	clear();
	add_evaluators();
	add_actions();

	CWorldState target;
	target.add_condition(CWorldProperty(eWorldPropertyPuzzleSolved, true));
	set_target_state(target);

	m_affect_cover = false;
}

void CStalkerPlanner::update(u32 time_delta)
{
#ifdef LOG_ACTION
	if ((psAI_Flags.test(aiGOAP) && !m_use_log) || (!psAI_Flags.test(aiGOAP) && m_use_log))
		set_use_log			(!!psAI_Flags.test(aiGOAP));
#endif

	const bool record_brain = ai().script_engine().lua_recording() && CPU::qpc_freq;
	CStalkerBrainProfileScope profile_scope(this, record_brain);
	inherited::update();

#ifdef GOAP_DEBUG
	if (m_failed) {
		{
			Msg			("%d",evaluators().size());
			EVALUATORS::const_iterator	I = evaluators().begin();
			EVALUATORS::const_iterator	E = evaluators().end();
			for ( ; I != E; ++I)
				Msg		("%d,%d",(*I).first,(*I).second->evaluate() ? 1 : 0);
		}
		{
			Msg			("%d",target_state().conditions().size());
			xr_vector<COperatorCondition>::const_iterator	I = target_state().conditions().begin();
			xr_vector<COperatorCondition>::const_iterator	E = target_state().conditions().end();
			for ( ; I != E; ++I)
				Msg		("%d,%d",(*I).condition(),(*I).value() ? 1 : 0);
		}
		{
			Msg			("%d",operators().size());
			const_iterator	I = operators().begin();
			const_iterator	E = operators().end();
			for ( ; I != E; ++I) {
				Msg		("%d,%d",(*I).m_operator_id,(*I).m_operator->weight(target_state(),target_state()));
				{
					Msg		("%d",(*I).m_operator->conditions().conditions().size());
					xr_vector<COperatorCondition>::const_iterator	i = (*I).m_operator->conditions().conditions().begin();
					xr_vector<COperatorCondition>::const_iterator	e = (*I).m_operator->conditions().conditions().end();
					for ( ; i != e; ++i)
						Msg	("%d,%d",(*i).condition(),(*i).value() ? 1 : 0);
				}
				{
					Msg		("%d",(*I).m_operator->effects().conditions().size());
					xr_vector<COperatorCondition>::const_iterator	i = (*I).m_operator->effects().conditions().begin();
					xr_vector<COperatorCondition>::const_iterator	e = (*I).m_operator->effects().conditions().end();
					for ( ; i != e; ++i)
						Msg	("%d,%d",(*i).condition(),(*i).value() ? 1 : 0);
				}
			}
		}
	}
#endif
}

void CStalkerPlanner::add_evaluators()
{
	add_evaluator(eWorldPropertyAlreadyDead, xr_new<CStalkerPropertyEvaluatorConst>(false, "is_already_dead"));
	add_evaluator(eWorldPropertyPuzzleSolved, xr_new<CStalkerPropertyEvaluatorConst>(false, "is_zone_puzzle_solved"));
	add_evaluator(eWorldPropertyAlive, xr_new<CStalkerPropertyEvaluatorAlive>(m_object, "is_alive"));
	add_evaluator(eWorldPropertyEnemy,
	              xr_new<CStalkerPropertyEvaluatorEnemies>(m_object, "is_there_enemies",
	                                                       CStalkerCombatPlanner::POST_COMBAT_WAIT_INTERVAL));
	add_evaluator(eWorldPropertyDanger, xr_new<CStalkerPropertyEvaluatorDangers>(m_object, "is_there_danger"));
	add_evaluator(eWorldPropertyAnomaly, xr_new<CStalkerPropertyEvaluatorAnomaly>(m_object, "is_there_anomalies"));
	add_evaluator(eWorldPropertyItems, xr_new<CStalkerPropertyEvaluatorItems>(m_object, "is_there_items_to_pick_up"));
}

void CStalkerPlanner::add_actions()
{
	CActionPlannerAction* planner;

	planner = xr_new<CStalkerDeathPlanner>(m_object, "death_planner");
	add_condition(planner, eWorldPropertyAlive, false);
	add_condition(planner, eWorldPropertyPuzzleSolved, false);
	add_effect(planner, eWorldPropertyPuzzleSolved, true);
	add_operator(eWorldOperatorDeathPlanner, planner);

	planner = xr_new<CStalkerALifePlanner>(m_object, "alife_planner");
	add_condition(planner, eWorldPropertyAlive, true);
	add_condition(planner, eWorldPropertyEnemy, false);
	add_condition(planner, eWorldPropertyAnomaly, false);
	add_condition(planner, eWorldPropertyDanger, false);
	add_condition(planner, eWorldPropertyItems, false);
	add_condition(planner, eWorldPropertyPuzzleSolved, false);
	add_effect(planner, eWorldPropertyPuzzleSolved, true);
	add_operator(eWorldOperatorALifePlanner, planner);

	planner = xr_new<CStalkerCombatPlanner>(m_object, "combat_planner");
	//	planner					= xr_new<CStalkerCombatPlannerNew>(m_object,"combat_planner_new");
	add_condition(planner, eWorldPropertyAlive, true);
	add_condition(planner, eWorldPropertyAnomaly, false);
	add_condition(planner, eWorldPropertyEnemy, true);
	add_effect(planner, eWorldPropertyEnemy, false);
	add_operator(eWorldOperatorCombatPlanner, planner);

	planner = xr_new<CStalkerDangerPlanner>(m_object, "danger_planner");
	add_condition(planner, eWorldPropertyAlive, true);
	add_condition(planner, eWorldPropertyEnemy, false);
	add_condition(planner, eWorldPropertyAnomaly, false);
	add_condition(planner, eWorldPropertyDanger, true);
	add_effect(planner, eWorldPropertyDanger, false);
	add_operator(eWorldOperatorDangerPlanner, planner);

	planner = xr_new<CStalkerAnomalyPlanner>(m_object, "anomaly_planner");
	add_condition(planner, eWorldPropertyAlive, true);
	add_condition(planner, eWorldPropertyAnomaly, true);
	add_effect(planner, eWorldPropertyAnomaly, false);
	add_operator(eWorldOperatorAnomalyPlanner, planner);

	CStalkerActionBase* action;

	action = xr_new<CStalkerActionGatherItems>(m_object, "gather_items");
	add_condition(action, eWorldPropertyAlive, true);
	add_condition(action, eWorldPropertyEnemy, false);
	add_condition(action, eWorldPropertyAnomaly, false);
	add_condition(action, eWorldPropertyDanger, false);
	add_condition(action, eWorldPropertyItems, true);
	add_effect(action, eWorldPropertyItems, false);
	add_operator(eWorldOperatorGatherItems, action);
}
