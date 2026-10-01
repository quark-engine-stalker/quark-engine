////////////////////////////////////////////////////////////////////////////
//	Module 		: hit_memory_manager.cpp
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Hit memory manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "hit_memory_manager.h"
#include "memory_space_impl.h"
#include "custommonster.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "script_callback_ex.h"
#include "script_game_object.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "game_object_space.h"
#include "profiler.h"
#include "client_spawn_manager.h"
#include "memory_manager.h"
#include "../xrEngine/IGame_Persistent.h"

#ifndef MASTER_GOLD
#	include "actor.h"
#	include "ai_debug.h"
#endif // MASTER_GOLD

CHitMemoryManager::~CHitMemoryManager()
{
	clear_delayed_objects();

#ifdef USE_SELECTED_HIT
	xr_delete				(m_selected_hit);
#endif
}

u32 CHitMemoryManager::object_index(u16 id) const
{
	if (!m_hits)
		return u32(-1);
	auto cached = m_object_indices.find(id);
	if (cached != m_object_indices.end())
	{
		const u32 index = cached->second;
		if (index < m_hits->size() && object_id((*m_hits)[index].m_object) == id)
			return index;
		m_object_indices.erase(id);
	}
	for (u32 index = 0; index < m_hits->size(); ++index)
	{
		if (object_id((*m_hits)[index].m_object) != id)
			continue;
		m_object_indices[id] = index;
		return index;
	}
	return u32(-1);
}

const CHitObject* CHitMemoryManager::object_by_id(u16 id) const
{
	const u32 index = object_index(id);
	return index != u32(-1) ? &(*m_hits)[index] : nullptr;
}

const CHitObject* CHitMemoryManager::hit(const CEntityAlive* object) const
{
	VERIFY(m_hits);
	return object_by_id(object_id(object));
}

void CHitMemoryManager::add(const CEntityAlive* entity_alive)
{
	add(0, Fvector().set(0, 0, 1), entity_alive, 0);
}

void CHitMemoryManager::Load(LPCSTR section)
{
}

void CHitMemoryManager::reinit()
{
	m_hits = 0;
	m_object_indices.clear();
	m_last_hit_object_id = ALife::_OBJECT_ID(-1);
	m_last_hit_time = 0;
}

void CHitMemoryManager::reload(LPCSTR section)
{
#ifdef USE_SELECTED_HIT
	xr_delete				(m_selected_hit);
#endif
	m_max_hit_count = READ_IF_EXISTS(pSettings, r_s32, section, "DynamicHitCount", 1);
}

void CHitMemoryManager::add(float amount, const Fvector& vLocalDir, const CObject* who, s16 element)
{
#ifndef MASTER_GOLD
	if (who && smart_cast<CActor const*>(who) && psAI_Flags.test(aiIgnoreActor))
		return;
#endif // MASTER_GOLD

	VERIFY(m_hits);
	if (!object().g_Alive())
		return;

	if (who && (m_object->ID() == who->ID()))
		return;

	if (who && !fis_zero(amount))
	{
		m_last_hit_object_id = who->ID();
		m_last_hit_time = Device.dwTimeGlobal;
	}

	object().callback(GameObject::eHit)(
		m_object->lua_game_object(),
		amount,
		vLocalDir,
		smart_cast<const CGameObject*>(who)->lua_game_object(),
		element
	);

	Fvector direction;
	m_object->XFORM().transform_dir(direction, vLocalDir);

	const CEntityAlive* entity_alive = smart_cast<const CEntityAlive*>(who);
	if (!entity_alive || (m_object->tfGetRelationType(entity_alive) == ALife::eRelationTypeFriend))
		return;

	const u16 hit_id = object_id(who);
	const u32 hit_index = object_index(hit_id);
	if (hit_index == u32(-1))
	{
		CHitObject hit_object;

		hit_object.fill(entity_alive, m_object,
		                !m_stalker ? squad_mask_type(-1) : m_stalker->agent_manager().member().mask(m_stalker));

#ifdef USE_FIRST_GAME_TIME
		hit_object.m_first_game_time	= Level().GetGameTime();
#endif
#ifdef USE_FIRST_LEVEL_TIME
		hit_object.m_first_level_time	= Device.dwTimeGlobal;
#endif
		hit_object.m_amount = amount;

		if (m_max_hit_count <= m_hits->size())
		{
			HITS::iterator I = std::min_element(m_hits->begin(), m_hits->end(), SLevelTimePredicate<CEntityAlive>());
			VERIFY(m_hits->end() != I);
			if (I->m_object)
				m_object_indices.erase(object_id(I->m_object));
			*I = hit_object;
		}
		else
			m_hits->push_back(hit_object);
	}
	else
	{
		CHitObject& existing = (*m_hits)[hit_index];
		existing.fill(entity_alive, m_object,
		          (!m_stalker
			           ? existing.m_squad_mask.get()
			           : (existing.m_squad_mask.get() | m_stalker->agent_manager().member().mask(m_stalker))));
		existing.m_amount = _max(amount, existing.m_amount);
	}
}

void CHitMemoryManager::add(const CHitObject& _hit_object)
{
#ifndef MASTER_GOLD
	if (_hit_object.m_object && smart_cast<CActor const*>(_hit_object.m_object) && psAI_Flags.test(aiIgnoreActor))
		return;
#endif // MASTER_GOLD

	VERIFY(m_hits);
	if (!object().g_Alive())
		return;

	CHitObject hit_object = _hit_object;
	hit_object.m_squad_mask.set(!m_stalker ? squad_mask_type(-1) : m_stalker->agent_manager().member().mask(m_stalker),
	                            TRUE);

	const CEntityAlive* entity_alive = hit_object.m_object;
	const u16 hit_id = object_id(entity_alive);
	const u32 hit_index = object_index(hit_id);
	if (hit_index == u32(-1))
	{
		if (m_max_hit_count <= m_hits->size())
		{
			HITS::iterator I = std::min_element(m_hits->begin(), m_hits->end(), SLevelTimePredicate<CEntityAlive>());
			VERIFY(m_hits->end() != I);
			if (I->m_object)
				m_object_indices.erase(object_id(I->m_object));
			*I = hit_object;
		}
		else
			m_hits->push_back(hit_object);
	}
	else
	{
		CHitObject& existing = (*m_hits)[hit_index];
		hit_object.m_squad_mask.assign(hit_object.m_squad_mask.get() | existing.m_squad_mask.get());
		existing = hit_object;
	}
}

struct CRemoveOfflinePredicate
{
	bool operator()(const CHitObject& object) const
	{
		VERIFY(object.m_object);
		return (!object.m_object || !!object.m_object->getDestroy() || object.m_object->H_Parent());
	}
};

void CHitMemoryManager::update()
{
	START_PROFILE("Memory Manager/hits::update")
		clear_delayed_objects();

		VERIFY(m_hits);
		const u32 old_memory_count = m_hits->size();
		m_hits->erase(
			std::remove_if(
				m_hits->begin(),
				m_hits->end(),
				CRemoveOfflinePredicate()
			),
			m_hits->end()
		);
		if (m_hits->size() != old_memory_count)
			m_object_indices.clear();

#ifdef USE_SELECTED_HIT
	xr_delete					(m_selected_hit);
	u32							level_time = 0;
	HITS::const_iterator		I = m_hits->begin();
	HITS::const_iterator		E = m_hits->end();
	for ( ; I != E; ++I) {
		if ((*I).m_level_time > level_time) {
			xr_delete			(m_selected_hit);
			m_selected_hit		= xr_new<CHitObject>(*I);
			level_time			= (*I).m_level_time;
		}
	}
#endif
	STOP_PROFILE
}

void CHitMemoryManager::enable(const CObject* object, bool enable)
{
	const u32 index = object_index(object_id(object));
	if (index == u32(-1))
		return;

	(*m_hits)[index].m_enabled = enable;
}

void CHitMemoryManager::remove_links(CObject* object)
{
	if (m_last_hit_object_id == object->ID())
	{
		m_last_hit_object_id = ALife::_OBJECT_ID(-1);
		m_last_hit_time = 0;
	}

	VERIFY(m_hits);
	const u16 id = object_id(object);
	const u32 index = object_index(id);
	if (index != u32(-1))
	{
		m_hits->erase(m_hits->begin() + index);
		m_object_indices.erase(id);
	}

#ifdef USE_SELECTED_HIT
	if (!m_selected_hit)
		return;

	if (!m_selected_hit->m_object)
		return;

	if (m_selected_hit->m_object->ID() != object->ID())
		return;

	xr_delete					(m_selected_hit);
#endif
}

void CHitMemoryManager::save(NET_Packet& packet) const
{
	if (!m_object->g_Alive())
		return;

	packet.w_u8((u8)objects().size());

	HITS::const_iterator I = objects().begin();
	HITS::const_iterator E = objects().end();
	for (; I != E; ++I)
	{
		VERIFY((*I).m_object);
		packet.w_u16((*I).m_object->ID());
		// object params
		packet.w_u32((*I).m_object_params.m_level_vertex_id);
		packet.w_vec3((*I).m_object_params.m_position);
#ifdef USE_ORIENTATION
		packet.w_float			((*I).m_object_params.m_orientation.yaw);
		packet.w_float			((*I).m_object_params.m_orientation.pitch);
		packet.w_float			((*I).m_object_params.m_orientation.roll);
#endif // USE_ORIENTATION
		// self params
		packet.w_u32((*I).m_self_params.m_level_vertex_id);
		packet.w_vec3((*I).m_self_params.m_position);
#ifdef USE_ORIENTATION
		packet.w_float			((*I).m_self_params.m_orientation.yaw);
		packet.w_float			((*I).m_self_params.m_orientation.pitch);
		packet.w_float			((*I).m_self_params.m_orientation.roll);
#endif // USE_ORIENTATION
#ifdef USE_LEVEL_TIME
		packet.w_u32((Device.dwTimeGlobal > (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_level_time) : 0);
#endif // USE_LAST_LEVEL_TIME
#ifdef USE_LEVEL_TIME
		packet.w_u32((Device.dwTimeGlobal > (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_last_level_time) : 0);
#endif // USE_LAST_LEVEL_TIME
#ifdef USE_FIRST_LEVEL_TIME
		packet.w_u32			((Device.dwTimeGlobal >= (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_first_level_time) : 0);
#endif // USE_FIRST_LEVEL_TIME
		packet.w_vec3((*I).m_direction);
		packet.w_u16((*I).m_bone_index);
		packet.w_float((*I).m_amount);
	}
}

void CHitMemoryManager::load(IReader& packet)
{
	if (!m_object->g_Alive())
		return;

	typedef CClientSpawnManager::CALLBACK_TYPE CALLBACK_TYPE;
	CALLBACK_TYPE callback;
	callback.bind(&m_object->memory(), &CMemoryManager::on_requested_spawn);

	int count = packet.r_u8();
	for (int i = 0; i < count; ++i)
	{
		CDelayedHitObject delayed_object;
		delayed_object.m_object_id = packet.r_u16();

		CHitObject& object = delayed_object.m_hit_object;
		object.m_object = smart_cast<CEntityAlive*>(Level().Objects.net_Find(delayed_object.m_object_id));
		// object params
		object.m_object_params.m_level_vertex_id = packet.r_u32();
		packet.r_fvector3(object.m_object_params.m_position);
#ifdef USE_ORIENTATION
		packet.r_float				(object.m_object_params.m_orientation.yaw);
		packet.r_float				(object.m_object_params.m_orientation.pitch);
		packet.r_float				(object.m_object_params.m_orientation.roll);
#endif
		// self params
		object.m_self_params.m_level_vertex_id = packet.r_u32();
		packet.r_fvector3(object.m_self_params.m_position);
#ifdef USE_ORIENTATION
		packet.r_float				(object.m_self_params.m_orientation.yaw);
		packet.r_float				(object.m_self_params.m_orientation.pitch);
		packet.r_float				(object.m_self_params.m_orientation.roll);
#endif
#ifdef USE_LEVEL_TIME
		VERIFY(Device.dwTimeGlobal >= object.m_level_time);
		object.m_level_time = Device.dwTimeGlobal - packet.r_u32();
		if (object.m_level_time > Device.dwTimeGlobal)
			object.m_level_time = Device.dwTimeGlobal;
#endif // USE_LEVEL_TIME
#ifdef USE_LAST_LEVEL_TIME
		VERIFY(Device.dwTimeGlobal >= object.m_last_level_time);
		object.m_last_level_time = Device.dwTimeGlobal - packet.r_u32();
		if (object.m_last_level_time > Device.dwTimeGlobal)
			object.m_last_level_time = Device.dwTimeGlobal;
#endif // USE_LAST_LEVEL_TIME
#ifdef USE_FIRST_LEVEL_TIME
		VERIFY						(Device.dwTimeGlobal >= (*I).m_first_level_time);
		object.m_first_level_time	= packet.r_u32();
		object.m_first_level_time	+= Device.dwTimeGlobal;
#endif // USE_FIRST_LEVEL_TIME
		packet.r_fvector3(object.m_direction);
		object.m_bone_index = packet.r_u16();
		object.m_amount = packet.r_float();

		if (object.m_object)
		{
			add(object);
			continue;
		}

		m_delayed_objects.push_back(delayed_object);

		const CClientSpawnManager::CSpawnCallback* spawn_callback = Level().client_spawn_manager().callback(
			delayed_object.m_object_id,
			m_object->ID());
		if (!spawn_callback || !spawn_callback->m_object_callback)
			if (!g_dedicated_server)
				Level().client_spawn_manager().add(delayed_object.m_object_id, m_object->ID(), callback);
#ifdef DEBUG
		else {
			if (spawn_callback && spawn_callback->m_object_callback) {
				VERIFY				(spawn_callback->m_object_callback == callback);
			}
		}
#endif // DEBUG
	}
}

void CHitMemoryManager::clear_delayed_objects()
{
	if (m_delayed_objects.empty())
		return;

	CClientSpawnManager& manager = Level().client_spawn_manager();
	DELAYED_HIT_OBJECTS::const_iterator I = m_delayed_objects.begin();
	DELAYED_HIT_OBJECTS::const_iterator E = m_delayed_objects.end();
	for (; I != E; ++I)
		if (manager.callback((*I).m_object_id, m_object->ID()))
			manager.remove((*I).m_object_id, m_object->ID());

	m_delayed_objects.clear();
}

void CHitMemoryManager::on_requested_spawn(CObject* object)
{
	DELAYED_HIT_OBJECTS::iterator I = m_delayed_objects.begin();
	DELAYED_HIT_OBJECTS::iterator E = m_delayed_objects.end();
	for (; I != E; ++I)
	{
		if ((*I).m_object_id != object->ID())
			continue;

		if (m_object->g_Alive())
		{
			(*I).m_hit_object.m_object = smart_cast<CEntityAlive*>(object);
			VERIFY((*I).m_hit_object.m_object);
			add((*I).m_hit_object);
		}

		m_delayed_objects.erase(I);
		return;
	}
}
