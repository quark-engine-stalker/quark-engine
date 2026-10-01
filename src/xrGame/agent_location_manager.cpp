////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_location_manager.cpp
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent location manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "agent_location_manager.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "agent_enemy_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "cover_point.h"

const float MIN_SUITABLE_ENEMY_DISTANCE = 3.f; //10.f;

struct CRemoveOldDangerCover
{
	typedef CAgentMemberManager::MEMBER_STORAGE MEMBER_STORAGE;

	CAgentMemberManager* m_members;

	IC CRemoveOldDangerCover(CAgentMemberManager* members)
	{
		VERIFY(members);
		m_members = members;
	}

	IC bool operator()(const CAgentLocationManager::CDangerLocationPtr& location) const
	{
		if (!location->useful())
		{
			MEMBER_STORAGE::iterator I = m_members->members().begin();
			MEMBER_STORAGE::iterator E = m_members->members().end();
			for (; I != E; ++I)
			{
				if (!location->mask().test(m_members->mask(&(*I)->object())))
					continue;

				(*I)->object().on_danger_location_remove(*location);
			}
		}

		return (!location->useful());
	}
};

struct CDangerLocationPredicate
{
	Fvector m_position;

	IC CDangerLocationPredicate(const Fvector& position)
	{
		m_position = position;
	}

	IC bool operator()(const CAgentLocationManager::CDangerLocationPtr& location) const
	{
		return (*location == m_position);
	}
};

IC CAgentLocationManager::CDangerLocationPtr CAgentLocationManager::location(const Fvector& position)
{
	LOCATIONS::iterator I = std::find_if(m_danger_locations.begin(), m_danger_locations.end(),
	                                     CDangerLocationPredicate(position));
	if (I != m_danger_locations.end())
		return (*I);
	return (0);
}

bool CAgentLocationManager::suitable(CAI_Stalker* object, const CCoverPoint* location, bool use_enemy_info) const
{
	const CAgentMemberManager& member_manager = this->object().member();
	const CAgentMemberManager::MEMBER_STORAGE& members = member_manager.members();
	const CAgentMemberManager::squad_mask_type combat_mask = member_manager.combat_mask();
	const ALife::_OBJECT_ID object_id = object->ID();
	const Fvector& location_position = location->position();
	float object_to_location_sqr = 0.f;
	bool object_to_location_actual = false;

	for (u32 index = 0, count = static_cast<u32>(members.size()); index < count; ++index)
	{
		const CMemberOrder* member = members[index];
		if (member->object().ID() == object_id)
			continue;

		if (!member->cover())
		{
			if (combat_mask & (CAgentMemberManager::squad_mask_type(1) << index))
				continue;

			if (member->object().Position().distance_to_sqr(location_position) <= _sqr(5.f))
				return (false);

			continue;
		}

		// check if member cover is too close
		if (member->cover()->m_position.distance_to_sqr(location_position) <= _sqr(5.f))
		{
			if (!object_to_location_actual)
			{
				object_to_location_sqr = object->Position().distance_to_sqr(location_position);
				object_to_location_actual = true;
			}

			// so member cover is too close
			//			if ((*I)->object().Position().distance_to_sqr(location->position()) <= object->Position().distance_to_sqr(location->position()))
			// check if member to its cover is more close than we to our cover
			if (member->object().Position().distance_to_sqr(member->cover()->m_position) <= object_to_location_sqr + 2.f)
				return (false);
		}
	}

	if (use_enemy_info)
	{
		CAgentEnemyManager::ENEMIES::const_iterator I = this->object().enemy().enemies().begin();
		CAgentEnemyManager::ENEMIES::const_iterator E = this->object().enemy().enemies().end();
		for (; I != E; ++I)
			if ((*I).m_enemy_position.distance_to_sqr(location_position) < _sqr(MIN_SUITABLE_ENEMY_DISTANCE))
				return (false);
	}

	return (true);
}

void CAgentLocationManager::make_suitable(CAI_Stalker* object, const CCoverPoint* location) const
{
	this->object().member().member(object).cover(location);

	if (!location)
		return;

	const Fvector& location_position = location->position();
	const ALife::_OBJECT_ID object_id = object->ID();

	CAgentMemberManager::const_iterator I = this->object().member().members().begin();
	CAgentMemberManager::const_iterator E = this->object().member().members().end();
	for (; I != E; ++I)
	{
		if ((*I)->object().ID() == object_id)
			continue;

		if (!(*I)->cover())
			continue;

		// check if member cover is too close
		if ((*I)->cover()->m_position.distance_to_sqr(location_position) <= _sqr(5.f))
		{
			//			Msg						("%6d : object [%s] disabled cover for object [%s]",Device.dwFrame,*object->cName(),*(*I)->object().cName());
			(*I)->object().on_cover_blocked((*I)->cover());
			(*I)->cover(0);
		}
	}
}

void CAgentLocationManager::add(CDangerLocationPtr location)
{
	typedef CAgentMemberManager::MEMBER_STORAGE MEMBER_STORAGE;
	MEMBER_STORAGE::iterator I = object().member().members().begin();
	MEMBER_STORAGE::iterator E = object().member().members().end();
	for (; I != E; ++I)
	{
		if (!location->mask().test(object().member().mask(&(*I)->object())))
			continue;

		(*I)->object().on_danger_location_add(*location);
	}

	CDangerLocationPtr danger = this->location(location->position());
	if (!danger)
	{
		m_danger_locations.push_back(location);
		return;
	}

	danger->m_level_time = location->m_level_time;

	if (danger->m_interval < location->m_interval)
		danger->m_interval = location->m_interval;

	if (danger->m_radius < location->m_radius)
		danger->m_radius = location->m_radius;
}

void CAgentLocationManager::remove_old_danger_covers()
{
	m_danger_locations.erase(
		std::remove_if(
			m_danger_locations.begin(),
			m_danger_locations.end(),
			CRemoveOldDangerCover(
				&object().member()
			)
		),
		m_danger_locations.end()
	);
}

float CAgentLocationManager::danger(const CCoverPoint* cover, MemorySpace::squad_mask_type member_mask) const
{
	float result = 1;
	const u32 current_time = Device.dwTimeGlobal;
	const Fvector& cover_position = cover->position();
	LOCATIONS::const_iterator I = m_danger_locations.begin();
	LOCATIONS::const_iterator E = m_danger_locations.end();
	for (; I != E; ++I)
	{
		if (current_time > (*I)->m_level_time + (*I)->m_interval)
			continue;

		if (!(*I)->mask().test(member_mask))
			continue;

		float distance = 1.f + (*I)->position().distance_to(cover_position);
		if (distance > (*I)->m_radius)
			continue;

		result *=
			float(current_time - (*I)->m_level_time) / float((*I)->m_interval);
	}

	return (result);
}

void CAgentLocationManager::update()
{
	remove_old_danger_covers();
}

void CAgentLocationManager::remove_links(CObject* object)
{
	m_danger_locations.erase(
		std::remove_if(
			m_danger_locations.begin(),
			m_danger_locations.end(),
			CRemoveDangerObject(object)
		),
		m_danger_locations.end()
	);
}
