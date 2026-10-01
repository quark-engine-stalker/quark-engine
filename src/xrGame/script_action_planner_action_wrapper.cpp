////////////////////////////////////////////////////////////////////////////
//	Module 		: script_action_planner_action_wrapper.cpp
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script action planner action wrapper
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_action_planner_action_wrapper.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "../xrServerEntities/script_engine.h"

namespace
{
LPCSTR script_action_name(CScriptActionPlannerActionWrapper* action)
{
	if (!action)
		return "<script_action>";
	CScriptActionBase* base_action = static_cast<CScriptActionBase*>(action);
	return base_action->m_action_name && base_action->m_action_name[0] ?
		base_action->m_action_name : "<script_action>";
}

CScriptGameObject* script_action_object(CScriptActionPlannerActionWrapper* action)
{
	if (!action)
		return nullptr;
	return static_cast<CScriptActionPlanner*>(action)->m_object;
}

void record_script_action_planner_stage(void* context, LPCSTR stage, LPCSTR nested_action_name,
	const u32 nested_action_id, const u64 elapsed_ticks)
{
	CScriptActionPlannerActionWrapper* action = static_cast<CScriptActionPlannerActionWrapper*>(context);
	CScriptGameObject* object = script_action_object(action);
	if (!action || !object || !stage)
		return;

	string1024 phase;
	if (nested_action_name && nested_action_name[0])
		_snprintf_s(phase, sizeof(phase), _TRUNCATE, "script_action.%s.%s:%s",
			script_action_name(action), stage, nested_action_name);
	else
		_snprintf_s(phase, sizeof(phase), _TRUNCATE, "script_action.%s.%s",
			script_action_name(action), stage);

	ai().script_engine().record_native_profile("ai_action", phase,
		*object->cName(), static_cast<u32>(object->ID()), elapsed_ticks, nested_action_id);
}

class CScriptActionNestedPlannerProfileScope : xray::noncopyable
{
public:
	CScriptActionNestedPlannerProfileScope(CScriptActionPlannerActionWrapper* action, const bool enabled) :
		m_action(enabled ? action : nullptr)
	{
		if (m_action)
			m_action->set_update_profile_callback(m_action, &record_script_action_planner_stage);
	}

	~CScriptActionNestedPlannerProfileScope()
	{
		if (m_action)
			m_action->set_update_profile_callback(nullptr, nullptr);
	}

private:
	CScriptActionPlannerActionWrapper* m_action;
};
}

void CScriptActionPlannerActionWrapper::setup(CScriptGameObject* object, CPropertyStorage* storage)
{
	::luabind::call_member<void>(this, "setup", object, storage);
}

void CScriptActionPlannerActionWrapper::setup_static(CScriptActionPlannerAction* planner, CScriptGameObject* object,
                                                     CPropertyStorage* storage)
{
	planner->CScriptActionPlannerAction::setup(object, storage);
}

void CScriptActionPlannerActionWrapper::initialize()
{
	call_void_noargs("initialize");
}

void CScriptActionPlannerActionWrapper::initialize_static(CScriptActionPlannerAction* action)
{
	action->CScriptActionPlannerAction::initialize();
}

void CScriptActionPlannerActionWrapper::execute()
{
	CScriptGameObject* object = script_action_object(this);
	const bool record_action = object && ai().script_engine().lua_recording() && CPU::qpc_freq;
	string512 phase;
	_snprintf_s(phase, sizeof(phase), _TRUNCATE, "script_action.execute.lua:%s", script_action_name(this));
	CScopedLuaRecordPhase lua_phase(ai().script_engine(), "ai_action", phase,
		object ? *object->cName() : "<script_action>", object ? static_cast<u32>(object->ID()) : 0u,
		0, record_action);
	CScriptActionNestedPlannerProfileScope nested_profile(this, record_action);
	call_void_noargs("execute");
}

void CScriptActionPlannerActionWrapper::execute_static(CScriptActionPlannerAction* action)
{
	action->CScriptActionPlannerAction::execute();
}

void CScriptActionPlannerActionWrapper::finalize()
{
	call_void_noargs("finalize");
}

void CScriptActionPlannerActionWrapper::finalize_static(CScriptActionPlannerAction* action)
{
	action->CScriptActionPlannerAction::finalize();
}

CScriptActionPlannerActionWrapper::_edge_value_type CScriptActionPlannerActionWrapper::weight(
	const CSConditionState& condition0, const CSConditionState& condition1) const
{
	return (::luabind::call_member<_edge_value_type>(const_cast<CScriptActionPlannerActionWrapper*>(this), "weight",
	                                               condition0, condition1));
}

CScriptActionPlannerActionWrapper::_edge_value_type CScriptActionPlannerActionWrapper::weight_static(
	CScriptActionPlannerAction* action, const CSConditionState& condition0, const CSConditionState& condition1)
{
	return (((const CScriptActionPlannerActionWrapper*)action)->CScriptActionPlannerAction::weight(
		condition0, condition1));
}
