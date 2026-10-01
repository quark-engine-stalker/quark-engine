////////////////////////////////////////////////////////////////////////////
//	Module 		: memory_manager.cpp
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Memory manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "hit_memory_manager.h"
#include "enemy_manager.h"
#include "item_manager.h"
#include "danger_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "memory_space_impl.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "profiler.h"
#include "agent_enemy_manager.h"
#include "agent_memory_manager.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "../xrServerEntities/script_engine.h"

CMemoryManager::MERGE_CLASS_CACHE CMemoryManager::m_merge_class_cache;

CMemoryManager::CMemoryManager(CEntityAlive* entity_alive, CSound_UserDataVisitor* visitor)
{
	VERIFY(entity_alive);
	m_object = smart_cast<CCustomMonster*>(entity_alive);
	m_stalker = smart_cast<CAI_Stalker*>(m_object);

	if (m_stalker)
		m_visual = xr_new<CVisualMemoryManager>(m_stalker);
	else
		m_visual = xr_new<CVisualMemoryManager>(m_object);

	m_sound = xr_new<CSoundMemoryManager>(m_object, m_stalker, visitor);
	m_hit = xr_new<CHitMemoryManager>(m_object, m_stalker);
	m_enemy = xr_new<CEnemyManager>(m_object);
	if (m_stalker)
	{
		m_visual_enemy_candidates.reserve(16);
	}
	m_visual_merge_plan.entries.reserve(64);
	m_sound_merge_plan.entries.reserve(32);
	m_hit_merge_plan.entries.reserve(16);
	if (m_merge_class_cache.empty())
		m_merge_class_cache.reserve(4096);
	m_visual_enemy_candidates_valid = false;
	m_item = xr_new<CItemManager>(m_object);
	m_danger = xr_new<CDangerManager>(m_object);
}

CMemoryManager::~CMemoryManager()
{
	xr_delete(m_visual);
	xr_delete(m_sound);
	xr_delete(m_hit);
	xr_delete(m_enemy);
	xr_delete(m_item);
	xr_delete(m_danger);
}

void CMemoryManager::Load(LPCSTR section)
{
	sound().Load(section);
	hit().Load(section);
	enemy().Load(section);
	item().Load(section);
	danger().Load(section);
}

void CMemoryManager::reinit()
{
	m_visual_enemy_candidates.clear();
	m_visual_enemy_candidates_valid = false;
	m_visual_merge_plan.reset();
	m_sound_merge_plan.reset();
	m_hit_merge_plan.reset();
	visual().reinit();
	sound().reinit();
	hit().reinit();
	enemy().reinit();
	item().reinit();
	danger().reinit();
}

void CMemoryManager::reload(LPCSTR section)
{
	visual().reload(section);
	sound().reload(section);
	hit().reload(section);
	enemy().reload(section);
	item().reload(section);
	danger().reload(section);
}

#ifdef _DEBUG
extern bool g_enemy_manager_second_update;
#endif // _DEBUG

void CMemoryManager::update_enemies(const bool& registered_in_combat)
{
	const bool record_stalker = m_stalker && ai().script_engine().lua_recording() && CPU::qpc_freq;
	LPCSTR record_name = m_object ? *m_object->cName() : "<memory>";
	const u32 record_id = m_object ? static_cast<u32>(m_object->ID()) : 0u;
#ifdef _DEBUG
	g_enemy_manager_second_update	= false;
#endif // _DEBUG
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.enemies.select_primary",
			record_name, record_id, 0, record_stalker);

		enemy().update();
	}

	if (
		m_stalker &&
		(
			!enemy().selected() ||
			(
				smart_cast<const CAI_Stalker*>(enemy().selected()) &&
				smart_cast<const CAI_Stalker*>(enemy().selected())->wounded()
			)
		) &&
		registered_in_combat
	)
	{
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.enemies.distribute",
				record_name, record_id, 0, record_stalker);

			m_stalker->agent_manager().enemy().distribute_enemies();
		}

		{
			CAgentEnemyManager& agent_enemies = m_stalker->agent_manager().enemy();
			const squad_mask_type member_mask = m_stalker->agent_manager().member().mask(m_stalker);
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.enemies.merge_assigned",
				record_name, record_id, static_cast<u32>(agent_enemies.enemies().size()), record_stalker);

			agent_enemies.merge_distributed_enemies(enemy(), member_mask);
		}

#ifdef _DEBUG
		g_enemy_manager_second_update	= true;
#endif // _DEBUG
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.enemies.select_secondary",
				record_name, record_id, 0, record_stalker);

			enemy().update();
		}
	}
}

void CMemoryManager::update(float time_delta)
{
	const bool record_stalker = m_stalker && ai().script_engine().lua_recording() && CPU::qpc_freq;
	LPCSTR record_name = m_object ? *m_object->cName() : "<memory>";
	const u32 record_id = m_object ? static_cast<u32>(m_object->ID()) : 0u;
	CScopedLuaRecordPhase total_phase(ai().script_engine(), "ai_memory", "memory.core.total",
		record_name, record_id, 0, record_stalker);

	START_PROFILE("Memory Manager")
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.visual.update",
				record_name, record_id, 0, record_stalker);

			visual().update(time_delta);
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.sound.update",
				record_name, record_id, 0, record_stalker);

			sound().update();
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.hit.update",
				record_name, record_id, 0, record_stalker);

			hit().update();
		}

		u64 observer_mask = 0;
		bool registered_in_combat = false;
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.registered_in_combat",
				record_name, record_id, 0, record_stalker);

			if (m_stalker)
			{
				CAgentMemberManager& members = m_stalker->agent_manager().member();
				observer_mask = static_cast<u64>(members.mask(m_stalker));
				registered_in_combat = !!(members.combat_mask() & observer_mask);
			}
		}

		// update enemies and items
		m_visual_enemy_candidates.clear();
		m_visual_enemy_candidates_valid = false;
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.reset",
				record_name, record_id, 0, record_stalker);
			enemy().reset();
			item().reset();
		}

		if (visual().enabled())
		{
			const CVisualMemoryManager::VISIBLES& visual_objects = visual().objects();
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.merge.visual",
				record_name, record_id, static_cast<u32>(visual_objects.size()), record_stalker);

			const xr_vector<u32>* candidate_indices = nullptr;
			if (m_stalker)
			{
				candidate_indices = m_stalker->agent_manager().memory().visual_merge_candidates(
					visual_objects, static_cast<MemorySpace::squad_mask_type>(observer_mask));
			}

			const size_t expected_enemy_candidates =
				candidate_indices ? candidate_indices->size() : visual_objects.size();
			if (m_visual_enemy_candidates.capacity() < expected_enemy_candidates)
				m_visual_enemy_candidates.reserve(expected_enemy_candidates);

			update(visual_objects, m_visual_merge_plan, true, true, candidate_indices, observer_mask);
			m_visual_enemy_candidates_valid = true;
		}
		else
		{
			// An empty valid snapshot avoids falling back to a disabled visual
			// manager later in the group-agent update.
			m_visual_enemy_candidates_valid = true;
		}

		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.merge.sound",
				record_name, record_id, static_cast<u32>(sound().objects().size()), record_stalker);

			update(sound().objects(), m_sound_merge_plan, registered_in_combat ? true : false,
				false, nullptr, observer_mask);
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.merge.hit",
				record_name, record_id, static_cast<u32>(hit().objects().size()), record_stalker);

			update(hit().objects(), m_hit_merge_plan, registered_in_combat ? true : false,
				false, nullptr, observer_mask);
		}

		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.enemies.update",
				record_name, record_id, 0, record_stalker);

			update_enemies(registered_in_combat);
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.item.update",
				record_name, record_id, 0, record_stalker);

			item().update();
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "ai_memory", "memory.danger.update",
				record_name, record_id, 0, record_stalker);

			danger().update();
		}

	STOP_PROFILE
}

void CMemoryManager::enable(const CObject* object, bool enable)
{
	visual().enable(object, enable);
	sound().enable(object, enable);
	hit().enable(object, enable);
}

const CMemoryManager::CMergeObjectClass& CMemoryManager::merge_object_class(const CGameObject* object)
{
	VERIFY(object);
	const u16 id = object->ID();
	auto found = m_merge_class_cache.find(id);
	if (found != m_merge_class_cache.end() && found->second.object == object)
		return found->second;

	CGameObject* mutable_object = const_cast<CGameObject*>(object);
	CMergeObjectClass value;
	value.object = object;
	value.entity_alive = mutable_object->cast_entity_alive();
	value.stalker = value.entity_alive && mutable_object->cast_stalker();
	value.inventory_item = mutable_object->cast_inventory_item() != nullptr;
	value.explosive = mutable_object->cast_explosive() != nullptr;

	if (found == m_merge_class_cache.end())
		found = m_merge_class_cache.emplace(id, value).first;
	else
		found->second = value;
	return found->second;
}

namespace
{
IC bool merge_has_intrinsic_danger(const MemorySpace::CVisibleObject&)
{
	return false;
}

IC bool merge_has_intrinsic_danger(const MemorySpace::CHitObject&)
{
	// Hit memory always references an entity and is handled through entity_alive.
	return false;
}

IC bool merge_has_intrinsic_danger(const MemorySpace::CSoundObject& object)
{
	return
		((object.m_sound_type & SOUND_TYPE_BULLET_HIT) == SOUND_TYPE_BULLET_HIT) ||
		((object.m_sound_type & SOUND_TYPE_WEAPON_SHOOTING) == SOUND_TYPE_WEAPON_SHOOTING) ||
		((object.m_sound_type & SOUND_TYPE_INJURING) == SOUND_TYPE_INJURING) ||
		((object.m_sound_type & SOUND_TYPE_DYING) == SOUND_TYPE_DYING);
}
}

template <typename T>
void CMemoryManager::update(const xr_vector<T>& objects, CMergePlan& plan, bool add_enemies,
	bool collect_visual_candidates, const xr_vector<u32>* candidate_indices, const u64 observer_mask)
{
	// Keep a positional plan between scheduled updates. Runtime type never changes
	// for a live C++ object, so unchanged entries can reuse their cast/classification
	// result. Erases, reorders and ID/pointer reuse self-invalidate per slot. Dynamic
	// gameplay predicates below are still evaluated on every update.
	if (plan.entries.size() < objects.size())
		plan.entries.resize(objects.size());
	else if (plan.entries.size() > objects.size())
		plan.entries.resize(objects.size());

	const squad_mask_type mask = m_stalker ?
		(observer_mask ? static_cast<squad_mask_type>(observer_mask) :
			m_stalker->agent_manager().member().mask(m_stalker)) : squad_mask_type(0);
	const bool observer_eats_corpses = !m_stalker && !!m_object->cast_base_monster();
	const auto process_object = [&](size_t index)
	{
		if (index >= objects.size())
			return;

		const T& memory_object = objects[index];
		if (!memory_object.m_enabled)
			return;

		// Recheck the authoritative entry even on the indexed path. This keeps
		// enable/mask mutations made by a callback observable at the same point
		// as in the legacy loop.
		if (m_stalker && !memory_object.m_squad_mask.test(mask))
			return;

		const CGameObject* object = memory_object.m_object;
		if (!object || object->getDestroy())
			return;

		CMergePlanEntry& object_class = plan.entries[index];
		if (object_class.object != object || object_class.object_id != object->ID())
		{
			const CMergeObjectClass& live_class = merge_object_class(object);
			object_class.object = object;
			object_class.object_id = object->ID();
			object_class.entity_alive = live_class.entity_alive;
			object_class.stalker = live_class.stalker;
			object_class.inventory_item = live_class.inventory_item;
			object_class.explosive = live_class.explosive;
		}

		const CEntityAlive* entity_alive = object_class.entity_alive;
		const bool entity_is_alive = entity_alive && entity_alive->g_Alive();
		// A memory entry that is neither an entity nor an item/explosive can only
		// matter when its source itself encodes a danger event (specific sound
		// types). Skip the guaranteed no-op path before touching the observer-owned
		// danger/enemy/item managers. All dynamic predicates remain unchanged for
		// entries that can affect gameplay state.
		if (!entity_alive && !object_class.inventory_item && !object_class.explosive &&
			!merge_has_intrinsic_danger(memory_object))
		{
			return;
		}

		if (!collect_visual_candidates ||
			(entity_alive && !entity_is_alive && (entity_alive->killer_id() != ALife::_OBJECT_ID(-1))))
		{
			danger().add(memory_object, entity_alive);
		}

		// CEnemyManager::useful_native rejects dead entities before any Lua
		// usefulness callback. Avoid entering the agent/enemy-manager path for
		// corpses while preserving their visual danger handling above.
		if (add_enemies && entity_is_alive && enemy().add(entity_alive))
		{
			if (collect_visual_candidates)
				m_visual_enemy_candidates.push_back(entity_alive);
			return;
		}

		if (m_stalker && object_class.stalker)
			return;

		const bool item_candidate = object_class.inventory_item ||
			(observer_eats_corpses && !!entity_alive) || (m_stalker && object_class.explosive);
		if (item_candidate)
			item().add(object);
	};

	if (candidate_indices)
	{
		// Consume the shared ordered index directly. The vector object itself is a
		// stable CAgentMemoryManager member; Lua/gameplay callbacks may mutate its
		// storage, so never keep an iterator/reference across process_object().
		// A revision change falls back to the authoritative legacy range at the same
		// point as before, preserving GAMMA callback semantics without copying the
		// whole candidate vector for every NPC update.
		CAgentMemoryManager& merge_index = m_stalker->agent_manager().memory();
		const u32 merge_revision = merge_index.visual_merge_revision();
		const size_t legacy_count = objects.size();
		for (size_t candidate_pos = 0; candidate_pos < candidate_indices->size(); ++candidate_pos)
		{
			const u32 source_index = (*candidate_indices)[candidate_pos];
			process_object(source_index);
			if (merge_revision == merge_index.visual_merge_revision())
				continue;

			for (size_t index = static_cast<size_t>(source_index) + 1; index < legacy_count; ++index)
				process_object(index);
			break;
		}
	}
	else
	{
		for (size_t index = 0, count = objects.size(); index < count; ++index)
			process_object(index);
	}
}

CMemoryInfo CMemoryManager::memory(const CObject* object) const
{
	CMemoryInfo result;
	if (!this->object().g_Alive())
		return (result);

	u32 level_time = 0;
	const CGameObject* game_object = smart_cast<const CGameObject*>(object);
	VERIFY(game_object);
	squad_mask_type mask = m_stalker ? m_stalker->agent_manager().member().mask(m_stalker) : squad_mask_type(-1);

	const u16 id = object_id(object);
	if (const CVisibleObject* visible_object = visual().object_by_id(id))
	{
		(CMemoryObject<CGameObject>&)result = (CMemoryObject<CGameObject>&)(*visible_object);
		result.visible(visible_object->visible(mask));
		result.m_visual_info = true;
		level_time = visible_object->m_level_time;
		VERIFY(result.m_object);
	}

	if (const CSoundObject* sound_object = sound().object_by_id(id))
	{
		if (level_time < sound_object->m_level_time)
		{
			(CMemoryObject<CGameObject>&)result = (CMemoryObject<CGameObject>&)(*sound_object);
			result.m_sound_info = true;
			level_time = sound_object->m_level_time;
			VERIFY(result.m_object);
		}
	}

	if (const CHitObject* hit_object = hit().object_by_id(id))
	{
		if (level_time < hit_object->m_level_time)
		{
			(CMemoryObject<CGameObject>&)result = (CMemoryObject<CGameObject>&)(*hit_object);
			result.m_object = game_object;
			result.m_hit_info = true;
			VERIFY(result.m_object);
		}
	}

	return (result);
}

u32 CMemoryManager::memory_time(const CObject* object) const
{
	u32 result = 0;
	if (!this->object().g_Alive())
		return (0);

	const CGameObject* game_object = smart_cast<const CGameObject*>(object);
	VERIFY(game_object);

	const u16 id = object_id(object);
	if (const CVisibleObject* visible_object = visual().object_by_id(id))
		result = visible_object->m_level_time;

	if (const CSoundObject* sound_object = sound().object_by_id(id))
		if (result < sound_object->m_level_time)
			result = sound_object->m_level_time;

	if (const CHitObject* hit_object = hit().object_by_id(id))
		if (result < hit_object->m_level_time)
			result = hit_object->m_level_time;

	return (result);
}

Fvector CMemoryManager::memory_position(const CObject* object) const
{
	u32 time = 0;
	Fvector result = Fvector().set(0.f, 0.f, 0.f);
	if (!this->object().g_Alive())
		return (result);

	const CGameObject* game_object = smart_cast<const CGameObject*>(object);
	VERIFY(game_object);

	const u16 id = object_id(object);
	if (const CVisibleObject* visible_object = visual().object_by_id(id))
	{
		time = visible_object->m_level_time;
		result = visible_object->m_object_params.m_position;
	}

	if (const CSoundObject* sound_object = sound().object_by_id(id))
	{
		if (time < sound_object->m_level_time)
		{
			time = sound_object->m_level_time;
			result = sound_object->m_object_params.m_position;
		}
	}

	if (const CHitObject* hit_object = hit().object_by_id(id))
	{
		if (time < hit_object->m_level_time)
		{
			time = hit_object->m_level_time;
			result = hit_object->m_object_params.m_position;
		}
	}

	return (result);
}

void CMemoryManager::remove_links(CObject* object)
{
	// Classification is shared only to avoid repeating immutable casts. Drop the
	// entry while relcase still owns a valid object so ID plus allocator-address
	// reuse cannot make a future object inherit a stale runtime classification.
	if (object)
	{
		const CGameObject* game_object = smart_cast<const CGameObject*>(object);
		if (game_object)
		{
			auto found = m_merge_class_cache.find(game_object->ID());
			if (found != m_merge_class_cache.end() && found->second.object == game_object)
				m_merge_class_cache.erase(found);
		}
	}

	if (object && !m_visual_enemy_candidates.empty())
	{
		const CEntityAlive* entity = smart_cast<const CEntityAlive*>(object);
		if (entity)
			m_visual_enemy_candidates.erase(
				std::remove(m_visual_enemy_candidates.begin(), m_visual_enemy_candidates.end(), entity),
				m_visual_enemy_candidates.end());
	}

	if (m_object->g_Alive())
	{
		visual().remove_links(object);
		sound().remove_links(object);
		hit().remove_links(object);
	}

	danger().remove_links(object);
	enemy().remove_links(object);
	item().remove_links(object);
}

void CMemoryManager::on_restrictions_change()
{
	if (!m_object->g_Alive())
		return;

	//	danger().on_restrictions_change	();
	//	enemy().on_restrictions_change	();
	item().on_restrictions_change();
}

void CMemoryManager::make_object_visible_somewhen(const CEntityAlive* enemy)
{
	if (!enemy)
		return;

	squad_mask_type mask = stalker().agent_manager().member().mask(&stalker());
	visual().add_fictitious_visible_mask(enemy, mask);
}

void CMemoryManager::save(NET_Packet& packet) const
{
	visual().save(packet);
	sound().save(packet);
	hit().save(packet);
	danger().save(packet);
}

void CMemoryManager::load(IReader& packet)
{
	visual().load(packet);
	sound().load(packet);
	hit().load(packet);
	danger().load(packet);
}

// we do this due to the limitation of client spawn manager
// should be revisited from the acrhitectural point of view
void CMemoryManager::on_requested_spawn(CObject* object)
{
	visual().on_requested_spawn(object);
	sound().on_requested_spawn(object);
	hit().on_requested_spawn(object);
}
