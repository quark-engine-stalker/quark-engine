////////////////////////////////////////////////////////////////////////////
//	Module 		: danger_manager.cpp
//	Created 	: 11.02.2005
//  Modified 	: 11.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Danger manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "danger_manager.h"
#include "custommonster.h"
#include "memory_space.h"
#include "profiler.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "actor.h"
#include "object_broker.h"

// Danger perception multipliers. 1.0 = original engine behaviour.
// Applied as: base_score * multiplier. 0.0 disables that danger type entirely.
float g_ai_danger_ricochet_mult         = 1.f;
float g_ai_danger_attack_sound_mult     = 1.f;
float g_ai_danger_entity_attacked_mult  = 1.f;
float g_ai_danger_entity_death_mult     = 1.f;
float g_ai_danger_corpse_mult           = 1.f;
float g_ai_danger_attacked_mult         = 1.f;
float g_ai_danger_grenade_mult          = 1.f;
float g_ai_danger_enemy_sound_mult      = 1.f;

struct CDangerPredicate
{
	const CObject* m_object;

	IC CDangerPredicate(const CObject* object)
	{
		m_object = object;
	}

	IC bool operator()(const CDangerObject& object) const
	{
		return (m_object == object.object());
	}
};

struct CFindPredicate
{
	const CDangerObject* m_object;

	IC CFindPredicate(const CDangerObject& object)
	{
		m_object = &object;
	}

	IC bool operator()(const CDangerObject& object) const
	{
		return (*m_object == object);
	}
};

struct CRemoveByTimePredicate
{
	u32 m_time_line;
	CDangerManager* m_manager;

	IC CRemoveByTimePredicate(u32 time_line, CDangerManager* manager)
	{
		m_time_line = time_line;
		VERIFY(manager);
		m_manager = manager;
	}

	IC bool operator()(const CDangerObject& object) const
	{
		if (object.time() < m_time_line)
		{
			if (object.object() && (object.type() == CDangerObject::eDangerTypeFreshEntityCorpse))
				m_manager->ignore(object.object());

			return (true);
		}

		if (!object.object())
			return (false);

		if (!m_manager->useful(object))
		{
			if ((object.type() == CDangerObject::eDangerTypeFreshEntityCorpse))
				m_manager->ignore(object.object());

			return (true);
		}

		return (false);
	}
};

CDangerManager::~CDangerManager()
{
}

CDangerManager::DANGER_KEY CDangerManager::danger_key(const CDangerObject& object) const
{
	const u64 object_id = object.object() ? static_cast<u64>(object.object()->ID()) : 0xffffull;
	return object_id | (static_cast<u64>(object.type()) << 16) |
		(static_cast<u64>(object.perceive_type()) << 24);
}

void CDangerManager::rebuild_object_index()
{
	m_object_index.clear();
	m_object_index.reserve(m_objects.size() * 2 + 8);
	for (u32 index = 0; index < m_objects.size(); ++index)
		m_object_index[danger_key(m_objects[index])] = index;
}

void CDangerManager::Load(LPCSTR section)
{
}

void CDangerManager::reinit()
{
	m_objects.clear();
	m_object_index.clear();
	m_ignored.clear();
	m_time_line = 0;
	m_selected = 0;
}

void CDangerManager::reload(LPCSTR section)
{
}

void CDangerManager::update()
{
	START_PROFILE("Memory Manager/dangers::update")
		const size_t old_count = m_objects.size();
		m_objects.erase(
			std::remove_if(
				m_objects.begin(),
				m_objects.end(),
				CRemoveByTimePredicate(
					time_line(),
					this
				)
			),
			m_objects.end()
		);
		if (m_objects.size() != old_count)
			rebuild_object_index();

		float result = flt_max;
		m_selected = 0;
		OBJECTS::const_iterator I = m_objects.begin();
		OBJECTS::const_iterator E = m_objects.end();
		for (; I != E; ++I)
		{
			//		Msg					("%6d : Danger : [%d][%d]",(*I).time(),(*I).type(),(*I).perceive_type());
			float value = do_evaluate(*I);
			if (result > value)
			{
				result = value;
				m_selected = &*I;
			}
		}

	STOP_PROFILE
}

void CDangerManager::remove_links(const CObject* object)
{
	if (m_selected && (m_selected->object() == object))
		m_selected = 0;

	const size_t old_count = m_objects.size();
	m_objects.erase(
		std::remove_if(
			m_objects.begin(),
			m_objects.end(),
			CDangerPredicate(object)
		),
		m_objects.end()
	);
	if (m_objects.size() != old_count)
		rebuild_object_index();

	{
		OBJECTS::iterator I = m_objects.begin();
		OBJECTS::iterator E = m_objects.end();
		for (; I != E; ++I)
		{
			if (!(*I).dependent_object())
				continue;

			if ((*I).dependent_object() != object)
				continue;

			(*I).clear_dependent_object();
		}
	}

	IGNORED::iterator I = std::lower_bound(m_ignored.begin(), m_ignored.end(), object->ID());
	if ((I != m_ignored.end()) && (*I == object->ID()))
		m_ignored.erase(I);
}

bool CDangerManager::useful(const CDangerObject& object) const
{
	if (object.object() && !object.dependent_object())
	{
		if (std::binary_search(m_ignored.begin(), m_ignored.end(), object.object()->ID()))
			return (false);
	}

	if (object.time() >= time_line())
		return (true);

	return (false);
}

bool CDangerManager::is_useful(const CDangerObject& object) const
{
	return (m_object->useful(this, object));
}

float CDangerManager::evaluate(const CDangerObject& object) const
{
	return (m_object->evaluate(this, object));
}

float CDangerManager::do_evaluate(const CDangerObject& object) const
{
	float result = 0.f;
	switch (object.type())
	{
	case CDangerObject::eDangerTypeBulletRicochet:
		{
			// I perceived bullet(knife) ricochet
			result += 3000.f * g_ai_danger_ricochet_mult;
			break;
		}
	case CDangerObject::eDangerTypeAttackSound:
		{
			// someone is shooting
			result += 2500.f * g_ai_danger_attack_sound_mult;
			break;
		}
	case CDangerObject::eDangerTypeEntityAttacked:
		{
			// someone is hit
			result += 2000.f * g_ai_danger_entity_attacked_mult;
			break;
		}
	case CDangerObject::eDangerTypeEntityDeath:
		{
			// someone becomes dead
			result += 3000.f * g_ai_danger_entity_death_mult;
			break;
		}
	case CDangerObject::eDangerTypeFreshEntityCorpse:
		{
			// I see a corpse
			result += 2250.f * g_ai_danger_corpse_mult;
			break;
		}
	case CDangerObject::eDangerTypeAttacked:
		{
			// someone is attacked
			result += 2000.f * g_ai_danger_attacked_mult;
			break;
		}
	case CDangerObject::eDangerTypeGrenade:
		{
			// grenade to explode nearby
			result += 1000.f * g_ai_danger_grenade_mult;
			break;
		}
	case CDangerObject::eDangerTypeEnemySound:
		{
			// enemy sound nearby
			result += 1000.f * g_ai_danger_enemy_sound_mult;
			break;
		}
	default: NODEFAULT;
	}

	result *= 10.f;
	result += float(Device.dwTimeGlobal - object.time());

	return (result);
}

void CDangerManager::add(const CVisibleObject& object, const CEntityAlive* entity_alive)
{
	PROF_EVENT("DangerManager::add_VisibleObject");
	if (!object.m_enabled || object.m_object->getDestroy())
		return;

	if (entity_alive && !entity_alive->g_Alive() && (entity_alive->killer_id() != ALife::_OBJECT_ID(-1)))
	{
		add(CDangerObject(entity_alive, entity_alive->Position(), object.m_level_time,
		                  CDangerObject::eDangerTypeFreshEntityCorpse,
		                  CDangerObject::eDangerPerceiveTypeVisual));
		return;
	}
}

void CDangerManager::add(const CSoundObject& object, const CEntityAlive* entity_alive)
{
	PROF_EVENT("DangerManager::add_SoundObject");
	if (!object.m_enabled || object.m_object->getDestroy())
		return;

	if ((object.m_sound_type & SOUND_TYPE_BULLET_HIT) == SOUND_TYPE_BULLET_HIT)
	{
		add(CDangerObject(entity_alive, object.m_object_params.m_position, object.m_level_time,
		                  CDangerObject::eDangerTypeBulletRicochet, CDangerObject::eDangerPerceiveTypeSound));
		return;
	}

	if ((object.m_sound_type & SOUND_TYPE_WEAPON_SHOOTING) == SOUND_TYPE_WEAPON_SHOOTING)
	{
		add(CDangerObject(entity_alive, object.m_object_params.m_position, object.m_level_time,
		                  CDangerObject::eDangerTypeAttackSound, CDangerObject::eDangerPerceiveTypeSound));
		return;
	}

	if ((object.m_sound_type & SOUND_TYPE_INJURING) == SOUND_TYPE_INJURING)
	{
		add(CDangerObject(entity_alive, object.m_object_params.m_position, object.m_level_time,
		                  CDangerObject::eDangerTypeEntityAttacked, CDangerObject::eDangerPerceiveTypeSound));
		return;
	}

	if ((object.m_sound_type & SOUND_TYPE_DYING) == SOUND_TYPE_DYING)
	{
		add(CDangerObject(entity_alive, object.m_object_params.m_position, object.m_level_time,
		                  CDangerObject::eDangerTypeEntityDeath, CDangerObject::eDangerPerceiveTypeSound));
		return;
	}

	if (entity_alive && m_object->is_relation_enemy(entity_alive))
	{
		add(CDangerObject(entity_alive, object.m_object_params.m_position, object.m_level_time,
		                  CDangerObject::eDangerTypeEnemySound, CDangerObject::eDangerPerceiveTypeSound));
		return;
	}
}

void CDangerManager::add(const CHitObject& object, const CEntityAlive* entity_alive)
{
	PROF_EVENT("DangerManager::add_HitObject");
	if (!object.m_enabled || object.m_object->getDestroy())
		return;

	if (fis_zero(object.m_amount))
		return;

	if (object.m_object->ID() == m_object->ID())
		return;

	VERIFY(entity_alive == object.m_object);
	add(CDangerObject(entity_alive, entity_alive->Position(), object.m_level_time, CDangerObject::eDangerTypeAttacked,
	                  CDangerObject::eDangerPerceiveTypeHit));
}

void CDangerManager::add(const CDangerObject& object)
{
	PROF_EVENT("DangerManager::add_DangerObject");
	if (m_object->memory().enemy().selected() && object.object()) // && !object.object()->g_Alive())
		ignore(object.object());

	if (!is_useful(object))
		return;

	const DANGER_KEY key = danger_key(object);
	DANGER_INDEX::iterator found = m_object_index.find(key);
	if (found != m_object_index.end())
	{
		const u32 index = found->second;
		if (index < m_objects.size() && m_objects[index] == object)
		{
			m_objects[index] = object;
			return;
		}

		// A compaction or external load changed vector indices. Rebuild once and
		// retry before falling back to append.
		rebuild_object_index();
		found = m_object_index.find(key);
		if (found != m_object_index.end())
		{
			m_objects[found->second] = object;
			return;
		}
	}

	m_objects.push_back(object);
	m_object_index[key] = static_cast<u32>(m_objects.size() - 1);
}

void CDangerManager::ignore(const CGameObject* object)
{
	VERIFY(object);
	IGNORED::iterator I = std::lower_bound(m_ignored.begin(), m_ignored.end(), object->ID());
	if ((I != m_ignored.end()) && (*I == object->ID()))
		return;

	m_ignored.insert(I, object->ID());
}

void CDangerManager::save(NET_Packet& packet) const
{
	save_data(m_ignored, packet);
}

void CDangerManager::load(IReader& packet)
{
	load_data(m_ignored, packet);
	std::sort(m_ignored.begin(), m_ignored.end());
	m_ignored.erase(std::unique(m_ignored.begin(), m_ignored.end()), m_ignored.end());
}
