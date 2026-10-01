////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_member_manager.cpp
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent member manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "agent_member_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "object_broker.h"
#include "agent_manager.h"
#include "agent_memory_manager.h"
#include "explosive.h"
#include "sound_player.h"
#include "cover_point.h"
#include "grenade.h"

void CAgentMemberManager::rebuild_member_index()
{
	m_member_id_index.clear();
	m_member_id_index.reserve(m_members.size());

	for (u32 i = 0, n = (u32)m_members.size(); i < n; ++i)
		m_member_id_index.push_back(std::make_pair(m_members[i]->object().ID(), i));

	std::sort(m_member_id_index.begin(), m_member_id_index.end());
}

u32 CAgentMemberManager::member_index(const ALife::_OBJECT_ID& object_id) const
{
	const std::pair<ALife::_OBJECT_ID, u32> key = std::make_pair(object_id, 0u);
	MEMBER_ID_INDEX::const_iterator I = std::lower_bound(m_member_id_index.begin(), m_member_id_index.end(), key);
	if ((I == m_member_id_index.end()) || (I->first != object_id))
		return (u32(-1));

	const u32 index = I->second;
	if ((index >= m_members.size()) || (m_members[index]->object().ID() != object_id))
		return (u32(-1));

	return (index);
}

u32 CAgentMemberManager::member_index(const CAI_Stalker* object) const
{
	VERIFY(object);
	const u32 index = member_index(object->ID());
	if ((index < m_members.size()) && (&m_members[index]->object() == object))
		return (index);

	// Defensive fallback for an unexpected stale/reused object ID.
	const_iterator I = std::find_if(m_members.begin(), m_members.end(), CMemberPredicate(object));
	return (I == m_members.end()) ? u32(-1) : u32(I - m_members.begin());
}

CAgentMemberManager::~CAgentMemberManager()
{
	delete_data(m_members);
}

void CAgentMemberManager::add(CEntity* member)
{
	CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(member);
	if (!stalker || !stalker->g_Alive())
		return;

	VERIFY2(
		sizeof(squad_mask_type)*8 > members().size(),
		make_string(
			"too many stalkers in group ([team:%d][squad:%d][group:%d]!",
			m_members.front()->object().g_Team(),
			m_members.front()->object().g_Squad(),
			m_members.front()->object().g_Group()
		)
	);

	VERIFY(member_index(stalker->ID()) == u32(-1));
	m_members.push_back(xr_new<CMemberOrder>(stalker));
	rebuild_member_index();
}

void CAgentMemberManager::remove(CEntity* member)
{
	CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(member);
	if (!stalker)
		return;

	if (registered_in_combat(stalker))
		unregister_in_combat(stalker);

	squad_mask_type m = mask(stalker);
	object().memory().update_memory_masks(m);
	object().memory().update_memory_mask(m, m_combat_mask);

	const u32 index = member_index(stalker);
	VERIFY(index < m_members.size());
	iterator I = m_members.begin() + index;
	xr_delete(*I);
	m_members.erase(I);
	rebuild_member_index();
}

void CAgentMemberManager::update()
{
}

void CAgentMemberManager::remove_links(CObject* object)
{
	MEMBER_STORAGE::iterator I = m_members.begin();
	MEMBER_STORAGE::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		if ((*I)->grenade_reaction().m_grenade)
		{
			const CGameObject* explosive = smart_cast<const CGameObject*>((*I)->grenade_reaction().m_grenade);
			VERIFY(explosive);
			if (explosive->ID() == object->ID())
				(*I)->grenade_reaction().clear();
			else
			{
				CGrenade const* grenade = smart_cast<CGrenade const*>(explosive);
				if (grenade && grenade->CurrentParentID() == object->ID())
					(*I)->grenade_reaction().clear();
			}
		}

		if ((*I)->grenade_reaction().m_game_object && ((*I)->grenade_reaction().m_game_object->ID() == object->ID()))
			(*I)->grenade_reaction().clear();

		if ((*I)->member_death_reaction().m_member && ((*I)->member_death_reaction().m_member->ID() == object->ID()))
			(*I)->member_death_reaction().clear();
	}
}

void CAgentMemberManager::register_in_combat(const CAI_Stalker* object)
{
	//	if (!object->group_behaviour())
	//		return;

#if 0//def DEBUG
	Msg							(
		"%6d registering stalker %s in combat: 0x%08x -> 0x%08x",
		Device.dwTimeGlobal,
		*object->cName(),
		m_combat_mask,
		m_combat_mask | mask(object)
	);
#endif // DEBUG

	squad_mask_type m = mask(object);
	m_actuality = m_actuality && ((m_combat_mask | m) == m_combat_mask);
	m_combat_mask |= m;
}

void CAgentMemberManager::unregister_in_combat(const CAI_Stalker* object)
{
	//	if (!object->group_behaviour()) {
	//		VERIFY					(!registered_in_combat(object));
	//		return;
	//	}

#if 0//def DEBUG
	Msg							(
		"%6d UNregistering stalker %s in combat: 0x%08x -> 0x%08x",
		Device.dwTimeGlobal,
		*object->cName(),
		m_combat_mask,
		(m_combat_mask & (squad_mask_type(-1) ^ mask(object)))
	);
#endif // DEBUG

	squad_mask_type m = mask(object);
	m_actuality = m_actuality && ((m_combat_mask & (squad_mask_type(-1) ^ m)) == m_combat_mask);
	m_combat_mask &= squad_mask_type(-1) ^ m;
}

bool CAgentMemberManager::registered_in_combat(const CAI_Stalker* object) const
{
	return (!!(m_combat_mask & mask(object)));
}

CAgentMemberManager::MEMBER_STORAGE& CAgentMemberManager::combat_members()
{
	if (m_actuality)
		return (m_combat_members);

	m_actuality = true;

	m_combat_members.clear();
	for (u32 i = 0, n = (u32)m_members.size(); i < n; ++i)
	{
		if (m_combat_mask & (squad_mask_type(1) << i))
			m_combat_members.push_back(m_members[i]);
	}

	return (m_combat_members);
}

CAgentMemberManager::squad_mask_type CAgentMemberManager::non_combat_members_mask() const
{
	const u32 count = (u32)m_members.size();
	const u32 bit_count = sizeof(squad_mask_type)*8;
	const squad_mask_type all_members =
		(count >= bit_count) ? squad_mask_type(-1) : ((squad_mask_type(1) << count) - 1);
	return (all_members & ~m_combat_mask);
}

u32 CAgentMemberManager::in_detour() const
{
	u32 in_detour = 0;
	MEMBER_STORAGE::const_iterator I = members().begin();
	MEMBER_STORAGE::const_iterator E = members().end();
	for (; I != E; ++I)
		if ((*I)->detour())
			++in_detour;

	return (in_detour);
}

bool CAgentMemberManager::can_detour() const
{
	u32 in_detour_count = in_detour();
	return (!in_detour_count || (in_detour_count < (members().size() / 2)));
}

bool CAgentMemberManager::cover_detouring() const
{
	MEMBER_STORAGE::const_iterator I = members().begin();
	MEMBER_STORAGE::const_iterator E = members().end();
	for (; I != E; ++I)
		if ((*I)->detour())
			return (true);
	return (false);
}

bool CAgentMemberManager::can_cry_noninfo_phrase() const
{
	for (u32 i = 0, n = (u32)m_members.size(); i < n; ++i)
	{
		if (!(m_combat_mask & (squad_mask_type(1) << i)))
			continue;

		if (m_members[i]->object().sound().active_sound_count(false))
			return (false);
	}

	return (true);
}

MemorySpace::squad_mask_type CAgentMemberManager::mask(const ALife::_OBJECT_ID& object_id) const
{
	const u32 index = member_index(object_id);
	VERIFY(index < m_members.size());
	return (MemorySpace::squad_mask_type(1) << index);
}

CMemberOrder* CAgentMemberManager::get_member(const ALife::_OBJECT_ID& object_id)
{
	const u32 index = member_index(object_id);
	return (index < m_members.size()) ? m_members[index] : 0;
}

bool CAgentMemberManager::can_throw_grenade(const Fvector& location) const
{
	if (Device.dwTimeGlobal <= m_last_throw_time + m_throw_time_interval)
		return (false);

	typedef CAgentMemberManager::MEMBER_STORAGE MEMBER_STORAGE;
	const float member_danger_radius_sqr = _sqr(5.f);
	const float cover_danger_radius_sqr = _sqr(5.f);
	MEMBER_STORAGE::const_iterator I = members().begin();
	MEMBER_STORAGE::const_iterator E = members().end();
	for (; I != E; ++I)
	{
		if ((*I)->object().Position().distance_to_sqr(location) <= member_danger_radius_sqr)
			return (false);

		if (!(*I)->cover())
			continue;

		if ((*I)->cover()->m_position.distance_to_sqr(location) <= cover_danger_radius_sqr)
			return (false);
	}

	return (true);
}

void CAgentMemberManager::on_throw_completed()
{
	m_last_throw_time = Device.dwTimeGlobal;
}
