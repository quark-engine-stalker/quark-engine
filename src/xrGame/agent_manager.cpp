////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_manager.cpp
//	Created 	: 24.05.2004
//  Modified 	: 24.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Agent manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "agent_manager.h"
#include "agent_corpse_manager.h"
#include "agent_enemy_manager.h"
#include "agent_explosive_manager.h"
#include "agent_location_manager.h"
#include "agent_member_manager.h"
#include "agent_memory_manager.h"
#include "agent_manager_planner.h"
#include "ai/stalker/ai_stalker.h"
#include "ai_space.h"
#include "../xrServerEntities/script_engine.h"
#include "profiler.h"

CAgentManager::CAgentManager()
{
	m_relcase_object_ids.reserve(32);
	init_scheduler();
	init_components();
}

CAgentManager::~CAgentManager()
{
	VERIFY(member().members().empty());
#ifdef USE_SCHEDULER_IN_AGENT_MANAGER
	remove_scheduler			();
#endif // USE_SCHEDULER_IN_AGENT_MANAGER
	remove_components();
}

void CAgentManager::init_scheduler()
{
#ifdef USE_SCHEDULER_IN_AGENT_MANAGER
	shedule.t_min				= 1000;
	shedule.t_max				= 1000;
	shedule_register			();
#else // USE_SCHEDULER_IN_AGENT_MANAGER
	m_last_update_time = 0;
	m_update_rate = 1000;
#endif // USE_SCHEDULER_IN_AGENT_MANAGER
}

void CAgentManager::init_components()
{
	m_corpse = xr_new<CAgentCorpseManager>(this);
	m_enemy = xr_new<CAgentEnemyManager>(this);
	m_explosive = xr_new<CAgentExplosiveManager>(this);
	m_location = xr_new<CAgentLocationManager>(this);
	m_member = xr_new<CAgentMemberManager>(this);
	m_memory = xr_new<CAgentMemoryManager>(this);
	m_brain = xr_new<CAgentManagerPlanner>();
	brain().setup(this);
}

#ifdef USE_SCHEDULER_IN_AGENT_MANAGER
void CAgentManager::remove_scheduler	()
{
	shedule_unregister			();
}
#endif // USE_SCHEDULER_IN_AGENT_MANAGER

void CAgentManager::remove_components()
{
	xr_delete(m_corpse);
	xr_delete(m_enemy);
	xr_delete(m_explosive);
	xr_delete(m_location);
	xr_delete(m_member);
	xr_delete(m_memory);
	xr_delete(m_brain);
}

void CAgentManager::remove_links(CObject* object)
{
	if (object)
	{
		if (m_relcase_frame != Device.dwFrame)
		{
			m_relcase_frame = Device.dwFrame;
			m_relcase_object_ids.clear_not_free();
		}

		const u16 object_id = object->ID();
		auto processed = std::lower_bound(m_relcase_object_ids.begin(), m_relcase_object_ids.end(), object_id);
		if (processed != m_relcase_object_ids.end() && *processed == object_id)
			return;

		m_relcase_object_ids.insert(processed, object_id);
	}

	corpse().remove_links(object);
	enemy().remove_links(object);
	explosive().remove_links(object);
	location().remove_links(object);
	member().remove_links(object);
	memory().remove_links(object);
	brain().remove_links(object);

}

void CAgentManager::update_impl()
{
	VERIFY(!member().members().empty());

	const CMemberOrder* representative = member().members().front();
	LPCSTR profile_name = representative ? *representative->object().cName() : "agent_group";
	const u32 profile_id = representative ? static_cast<u32>(representative->object().ID()) : u32(-1);
	const u32 member_count = static_cast<u32>(member().members().size());

	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_manager", "memory", profile_name, profile_id, member_count);

		memory().update();
	}
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_manager", "corpse", profile_name, profile_id, member_count);

		corpse().update();
	}
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_manager", "enemy", profile_name, profile_id, member_count);

		enemy().update();
	}
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_manager", "explosive", profile_name, profile_id, member_count);

		explosive().update();
	}
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_manager", "location", profile_name, profile_id, member_count);

		location().update();
	}
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_manager", "member", profile_name, profile_id, member_count);

		member().update();
	}
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_manager", "brain", profile_name, profile_id, member_count);

		brain().update();
	}
}

#ifdef USE_SCHEDULER_IN_AGENT_MANAGER
void CAgentManager::shedule_Update		(u32 time_delta)
{
	START_PROFILE("Agent_Manager")

	ISheduled::shedule_Update	(time_delta);

	update_impl					();

	STOP_PROFILE
}

float CAgentManager::shedule_Scale		()
{
	return						(.5f);
}

#else // USE_SCHEDULER_IN_AGENT_MANAGER

void CAgentManager::update()
{
	if (Device.dwTimeGlobal <= m_last_update_time)
		return;

	if (Device.dwTimeGlobal - m_last_update_time < m_update_rate)
		return;

	m_last_update_time = Device.dwTimeGlobal;
	update_impl();
}

#endif // USE_SCHEDULER_IN_AGENT_MANAGER
