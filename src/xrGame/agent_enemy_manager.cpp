////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_enemy_manager.cpp
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent enemy manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "agent_enemy_manager.h"
#include "agent_manager.h"
#include "agent_memory_manager.h"
#include "agent_member_manager.h"
#include "ai_space.h"
#include "ef_storage.h"
#include "ef_pattern.h"
#include "member_order.h"
#include "ai/stalker/ai_stalker.h"
#include "../xrServerEntities/script_engine.h"

#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "hit_memory_manager.h"
#include "enemy_manager.h"
#include "memory_space_impl.h"

#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#pragma warning(pop)

const float wounded_enemy_reached_distance = 3.f;

const unsigned __int32 __c0 = 0x55555555;
const unsigned __int32 __c1 = 0x33333333;
const unsigned __int32 __c2 = 0x0f0f0f0f;
const unsigned __int32 __c3 = 0x00ff00ff;
const unsigned __int32 __c4 = 0x0000003f;

IC u32 population(const u32& b)
{
	u32 a = b;
	a = (a & __c0) + ((a >> 1) & __c0);
	a = (a & __c1) + ((a >> 2) & __c1);
	a = (a + (a >> 4)) & __c2;
	a = (a + (a >> 8)) & __c3;
	a = (a + (a >> 16)) & __c4;
	return (a);
}

IC u32 population(const u64& b)
{
	return (population((u32)b) + population(u32(b >> 32)));
}

struct enemy_lookup_less
{
	IC bool operator()(const CAgentEnemyManager::ENEMY_LOOKUP_ENTRY& left,
	                   const CAgentEnemyManager::ENEMY_LOOKUP_ENTRY& right) const
	{
		return std::less<const CEntityAlive*>()(left.first, right.first);
	}
};

struct CEnemyFiller
{
	typedef CAgentEnemyManager::ENEMIES ENEMIES;
	typedef CAgentEnemyManager::ENEMY_LOOKUP ENEMY_LOOKUP;
	typedef CAgentEnemyManager::ENEMY_LOOKUP_ENTRY ENEMY_LOOKUP_ENTRY;
	ENEMIES* m_enemies;
	ENEMY_LOOKUP* m_enemy_lookup;
	squad_mask_type m_mask;

	IC CEnemyFiller(ENEMIES* enemies, ENEMY_LOOKUP* enemy_lookup, squad_mask_type mask)
	{
		m_enemies = enemies;
		m_enemy_lookup = enemy_lookup;
		m_mask = mask;
	}

	IC void operator()(const CEntityAlive* enemy) const
	{
		const ENEMY_LOOKUP_ENTRY key = std::make_pair(enemy, u32(0));
		ENEMY_LOOKUP::iterator I = std::lower_bound(
			m_enemy_lookup->begin(),
			m_enemy_lookup->end(),
			key,
			enemy_lookup_less()
		);
		if ((I == m_enemy_lookup->end()) || (I->first != enemy))
		{
			const u32 enemy_index = (u32)m_enemies->size();
			m_enemies->push_back(CMemberEnemy(enemy, m_mask));
			m_enemy_lookup->insert(I, std::make_pair(enemy, enemy_index));
			return;
		}

		VERIFY(I->second < m_enemies->size());
		(*m_enemies)[I->second].m_mask.set(m_mask,TRUE);
	}
};

struct remove_wounded_predicate
{
	IC bool operator()(const CMemberEnemy& enemy) const
	{
		const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(enemy.m_object);
		if (!stalker)
			return (false);

		if (!stalker->wounded())
			return (false);

		return (true);
	}
};

struct wounded_enemy_missing_predicate
{
	const CAgentEnemyManager::ENEMIES* m_enemies;

	IC wounded_enemy_missing_predicate(const CAgentEnemyManager::ENEMIES& enemies) :
		m_enemies(&enemies)
	{}

	IC bool operator()(const CAgentEnemyManager::WOUNDED_ENEMY& wounded) const
	{
		return std::find(m_enemies->begin(), m_enemies->end(), wounded.first) == m_enemies->end();
	}
};

void CAgentEnemyManager::fill_enemies()
{
	m_enemies.clear();
	m_enemy_lookup.clear_not_free();
	if (m_enemy_lookup.capacity() < m_enemies.capacity())
		m_enemy_lookup.reserve(m_enemies.capacity());

	{
		CAgentMemberManager::iterator I = object().member().combat_members().begin();
		CAgentMemberManager::iterator E = object().member().combat_members().end();
		for (; I != E; ++I)
		{
			(*I)->probability(1.f);
			(*I)->object().memory().fill_enemies(
				CEnemyFiller(&m_enemies, &m_enemy_lookup, object().member().mask(&(*I)->object()))
			);
		}
	}

	if (m_enemies.empty())
		return;

	VERIFY(!m_enemies.empty());

	m_wounded.erase(
		std::remove_if(
			m_wounded.begin(),
			m_wounded.end(),
			wounded_enemy_missing_predicate(m_enemies)
		),
		m_wounded.end()
	);

	m_only_wounded_left = true;
	m_is_any_wounded = false;
	{
		CAgentMemoryManager& memory = object().memory();
		ENEMIES::iterator I = enemies().begin();
		ENEMIES::iterator E = enemies().end();
		for (; I != E; ++I)
		{
			if (m_only_wounded_left)
			{
				const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>((*I).m_object);
				if (!stalker || !stalker->wounded())
					m_only_wounded_left = false;
				else
					m_is_any_wounded = true;
			}
			else
			{
				if (!m_is_any_wounded)
				{
					const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>((*I).m_object);
					if (stalker && stalker->wounded())
						m_is_any_wounded = true;
				}
			}

			memory.object_information(
				(*I).m_object,
				(*I).m_level_time,
				(*I).m_enemy_position,
				(*I).m_visible_index,
				(*I).m_sound_index,
				(*I).m_hit_index
			);
		}
	}

	if (!m_only_wounded_left && m_is_any_wounded)
	{
		enemies().erase(
			std::remove_if(
				enemies().begin(),
				enemies().end(),
				remove_wounded_predicate()
			),
			enemies().end()
		);
	}

	VERIFY(!m_enemies.empty());
}

u32 CAgentEnemyManager::find_enemy_index(const CEntityAlive* enemy) const
{
	const ENEMY_LOOKUP_ENTRY key = std::make_pair(enemy, u32(0));
	const ENEMY_LOOKUP::const_iterator found = std::lower_bound(
		m_enemy_lookup.begin(), m_enemy_lookup.end(), key, enemy_lookup_less());
	if ((found == m_enemy_lookup.end()) || (found->first != enemy))
		return u32(-1);

	const u32 index = found->second;
	if ((index >= m_enemies.size()) || (m_enemies[index].m_object != enemy))
		return u32(-1);

	return index;
}

void CAgentEnemyManager::rebuild_enemy_lookup()
{
	m_enemy_lookup.clear_not_free();
	if (m_enemy_lookup.capacity() < m_enemies.size())
		m_enemy_lookup.reserve(m_enemies.size());

	for (u32 i = 0, n = static_cast<u32>(m_enemies.size()); i < n; ++i)
		m_enemy_lookup.push_back(std::make_pair(m_enemies[i].m_object, i));

	std::sort(m_enemy_lookup.begin(), m_enemy_lookup.end(), enemy_lookup_less());
}

float CAgentEnemyManager::evaluate(const CEntityAlive* object0, const CEntityAlive* object1) const
{
	ai().ef_storage().non_alife().member_item() = 0;
	ai().ef_storage().non_alife().enemy_item() = 0;
	ai().ef_storage().non_alife().member() = object0;
	ai().ef_storage().non_alife().enemy() = object1;
	return (ai().ef_storage().m_pfVictoryProbability->ffGetValue() / 100.f);
}

u32 CAgentEnemyManager::member_mask_index(squad_mask_type member_mask) const
{
	VERIFY(member_mask);
	VERIFY(!(member_mask & (member_mask - 1)));
	return population(member_mask - 1);
}

void CAgentEnemyManager::prepare_effectiveness_cache()
{
	m_effectiveness_members.assign(
		object().member().combat_members().begin(),
		object().member().combat_members().end()
	);
	m_effectiveness_member_masks.resize(m_effectiveness_members.size());
	m_effectiveness_enemy_count = (u32)m_enemies.size();

	for (u32 i = 0; i < sizeof(m_effectiveness_row_by_member) / sizeof(m_effectiveness_row_by_member[0]); ++i)
		m_effectiveness_row_by_member[i] = u32(-1);

	for (u32 row = 0; row < m_effectiveness_members.size(); ++row)
	{
		const squad_mask_type member_mask = object().member().mask(&m_effectiveness_members[row]->object());
		m_effectiveness_member_masks[row] = member_mask;
		m_effectiveness_row_by_member[member_mask_index(member_mask)] = row;
	}

	m_member_enemy_effectiveness.assign(
		m_effectiveness_members.size() * m_effectiveness_enemy_count,
		flt_max
	);
}

float CAgentEnemyManager::member_enemy_effectiveness(squad_mask_type member_mask, u32 enemy_index)
{
	VERIFY(enemy_index < m_effectiveness_enemy_count);
	const u32 member_index = member_mask_index(member_mask);
	VERIFY(member_index < sizeof(m_effectiveness_row_by_member) / sizeof(m_effectiveness_row_by_member[0]));
	const u32 row = m_effectiveness_row_by_member[member_index];
	VERIFY(row != u32(-1));
	VERIFY(row < m_effectiveness_members.size());

	float& value = m_member_enemy_effectiveness[row * m_effectiveness_enemy_count + enemy_index];
	if (value == flt_max)
		value = evaluate(&m_effectiveness_members[row]->object(), m_enemies[enemy_index].m_object);

	return value;
}

CMemberOrder* CAgentEnemyManager::effectiveness_member(squad_mask_type member_mask) const
{
	const u32 member_index = member_mask_index(member_mask);
	VERIFY(member_index < sizeof(m_effectiveness_row_by_member) / sizeof(m_effectiveness_row_by_member[0]));
	const u32 row = m_effectiveness_row_by_member[member_index];
	VERIFY(row != u32(-1));
	VERIFY(row < m_effectiveness_members.size());
	return m_effectiveness_members[row];
}

void CAgentEnemyManager::swap_effectiveness_columns(u32 enemy_index0, u32 enemy_index1)
{
	VERIFY(enemy_index0 < m_effectiveness_enemy_count);
	VERIFY(enemy_index1 < m_effectiveness_enemy_count);

	for (u32 row = 0; row < m_effectiveness_members.size(); ++row)
	{
		float* values = &m_member_enemy_effectiveness[row * m_effectiveness_enemy_count];
		std::swap(values[enemy_index0], values[enemy_index1]);
	}
}

void CAgentEnemyManager::exchange_enemies(CMemberOrder& member0, squad_mask_type mask0,
	                                      CMemberOrder& member1, squad_mask_type mask1)
{
	u32 enemy0 = member0.selected_enemy();
	u32 enemy1 = member1.selected_enemy();
	m_enemies[enemy0].m_distribute_mask.set(mask0,FALSE);
	m_enemies[enemy1].m_distribute_mask.set(mask1,FALSE);
	m_enemies[enemy0].m_distribute_mask.set(mask1,TRUE);
	m_enemies[enemy1].m_distribute_mask.set(mask0,TRUE);
	member0.selected_enemy(enemy1);
	member1.selected_enemy(enemy0);
}

void CAgentEnemyManager::compute_enemy_danger()
{
	ENEMIES::iterator I = m_enemies.begin();
	ENEMIES::iterator E = m_enemies.end();
	for (; I != E; ++I)
	{
		float best = -1.f;
		xr_vector<CMemberOrder*>::const_iterator i = m_effectiveness_members.begin();
		xr_vector<CMemberOrder*>::const_iterator e = m_effectiveness_members.end();
		for (; i != e; ++i)
		{
			float value = evaluate((*I).m_object, &(*i)->object());
			if (value > best)
				best = value;
		}
		(*I).m_probability = best;
	}

	std::sort(m_enemies.begin(), m_enemies.end());
}

void CAgentEnemyManager::assign_enemies()
{
	for (;;)
	{
		squad_mask_type J, K, N = 0;
		float best = flt_max;

		ENEMIES::iterator I = m_enemies.begin();
		ENEMIES::iterator E = m_enemies.end();
		for (; I != E; ++I)
		{
			const u32 enemy_index = u32(I - m_enemies.begin());
			J = (*I).m_mask.get();
			N = 0;
			best = -1.f;
			for (; J; J &= J - 1)
			{
				K = (J & (J - 1)) ^ J;
				CMemberOrder* member = effectiveness_member(K);
				if (!fsimilar(member->probability(), 1.f))
					continue;

				float value = member_enemy_effectiveness(K, enemy_index);
				if (value > best)
				{
					best = value;
					N = K;
				}
			}
			if (N)
				break;
		}
		if (!N)
			break;

		(*I).m_distribute_mask.set(N,TRUE);
		CMemberOrder* member = effectiveness_member(N);
		member->probability(best);
		(*I).m_probability *= 1.f - best;

		// recovering sort order
		for (u32 i = 0, n = m_enemies.size() - 1; i < n; ++i)
			if (m_enemies[i + 1] < m_enemies[i])
			{
				std::swap(m_enemies[i], m_enemies[i + 1]);
				swap_effectiveness_columns(i, i + 1);
			}
			else
				break;
	}
}

void CAgentEnemyManager::permutate_enemies()
{
	// filling member enemies
	for (u32 member_index = 0; member_index < m_effectiveness_members.size(); ++member_index)
	{
		CMemberOrder* member = m_effectiveness_members[member_index];
		// clear enemies
		member->enemies().clear();
		// setup procesed flag
		member->processed(false);
		// get member squad mask
		const squad_mask_type member_mask = m_effectiveness_member_masks[member_index];
		// setup if player has enemy
		bool enemy_selected = false;
		// iterate on enemies
		ENEMIES::const_iterator i = m_enemies.begin(), b = i;
		ENEMIES::const_iterator e = m_enemies.end();
		for (; i != e; ++i)
		{
			if ((*i).m_mask.is(member_mask))
				member->enemies().push_back(u32(i - b));

			if ((*i).m_distribute_mask.is(member_mask))
			{
				member->selected_enemy(u32(i - b));
				enemy_selected = true;
			}
		}
		// if there is enemy - all is ok
		if (enemy_selected)
			continue;

		// otherwise temporary make the member processed
		member->processed(true);
	}

	// perform permutations
	bool changed;
	do
	{
		changed = false;
		for (u32 member_index = 0; member_index < m_effectiveness_members.size(); ++member_index)
		{
			CMemberOrder* member = m_effectiveness_members[member_index];
			// if member is processed the continue;
			if (member->processed())
				continue;

			const squad_mask_type current_member_mask = m_effectiveness_member_masks[member_index];
			float best_distance_sqr = member->object().Position().distance_to_sqr(
				m_enemies[member->selected_enemy()].m_object->Position());
			bool found = false;
			xr_vector<u32>::const_iterator i = member->enemies().begin();
			xr_vector<u32>::const_iterator e = member->enemies().end();
			for (; i != e; ++i)
			{
				if (member->selected_enemy() == *i)
					continue;
				const float my_distance_sqr = member->object().Position().distance_to_sqr(
					m_enemies[*i].m_object->Position());
				if (my_distance_sqr < best_distance_sqr)
				{
					// check if we can exchange enemies
					squad_mask_type J = m_enemies[*i].m_distribute_mask.get(), K;
					// iterating on members, whose current enemy is the new one
					for (; J; J &= J - 1)
					{
						K = (J & (J - 1)) ^ J;
						CMemberOrder* other_member = effectiveness_member(K);
						xr_vector<u32>::iterator ii = std::find(other_member->enemies().begin(), other_member->enemies().end(),
						                                        member->selected_enemy());
						// check if member can my current enemy
						if (ii == other_member->enemies().end())
							continue;

						// check if I'm closer to the enemy
						const float member_distance_sqr = other_member->object().Position().distance_to_sqr(
							m_enemies[*i].m_object->Position());
						if (member_distance_sqr <= my_distance_sqr)
							continue;

						// check if our effectiveness is near the same
						float my_to_his = member_enemy_effectiveness(current_member_mask, other_member->selected_enemy());
						float his_to_my = member_enemy_effectiveness(K, member->selected_enemy());
						if (!fsimilar(my_to_his, other_member->probability()) || !fsimilar(his_to_my, member->probability()))
							continue;

						exchange_enemies(*member, current_member_mask, *other_member, K);

						found = true;
						best_distance_sqr = my_distance_sqr;
						break;
					}
				}

				if (found)
					break;
			}

			if (!found)
			{
				member->processed(true);
				continue;
			}

			changed = true;
		}
	}
	while (changed);

	VERIFY(!m_enemies.empty());
	if (!m_only_wounded_left)
	{
		for (u32 member_index = 0; member_index < m_effectiveness_members.size(); ++member_index)
		{
			CMemberOrder* member = m_effectiveness_members[member_index];
			const squad_mask_type member_mask = m_effectiveness_member_masks[member_index];
			CVisualMemoryManager& visual = member->object().memory().visual();
			CHitMemoryManager& hit = member->object().memory().hit();
			const ALife::_OBJECT_ID last_hit_object_id = hit.last_hit_object_id();
			ENEMIES::iterator i = m_enemies.begin();
			ENEMIES::iterator e = m_enemies.end();
			for (; i != e; ++i)
			{
				if (visual.visible_now((*i).m_object))
				{
					(*i).m_distribute_mask.assign(
						(*i).m_distribute_mask.get() | member_mask);
					continue;
				}

				if (last_hit_object_id != (*i).m_object->ID())
					continue;

				(*i).m_distribute_mask.assign((*i).m_distribute_mask.get() | member_mask);
			}
		}
	}
}

template <typename T>
IC void CAgentEnemyManager::setup_mask(xr_vector<T>& objects, CMemberEnemy& enemy,
	                                   u32& cached_index,
	                                   const squad_mask_type& non_combat_members)
{
	typename xr_vector<T>::iterator I = objects.end();
	if ((cached_index < objects.size()) && (objects[cached_index] == enemy.m_object->ID()))
		I = objects.begin() + cached_index;
	else
	{
		I = std::find(objects.begin(), objects.end(), enemy.m_object->ID());
		cached_index = I == objects.end() ? u32(-1) : u32(I - objects.begin());
	}

	if (I != objects.end())
	{
		(*I).m_squad_mask.assign(
			(*I).m_squad_mask.get() |
			enemy.m_distribute_mask.get()
		);
	}
}

IC void CAgentEnemyManager::setup_mask(CMemberEnemy& enemy, const squad_mask_type& non_combat_members)
{
	setup_mask(object().memory().visibles(), enemy, enemy.m_visible_index, non_combat_members);
	setup_mask(object().memory().sounds(), enemy, enemy.m_sound_index, non_combat_members);
	setup_mask(object().memory().hits(), enemy, enemy.m_hit_index, non_combat_members);
}

void CAgentEnemyManager::assign_enemy_masks()
{
	CAgentMemberManager::MEMBER_STORAGE& combat_members = object().member().combat_members();
	const squad_mask_type combat_mask = object().member().combat_mask();

	if (combat_mask && !combat_members.empty())
	{
		ENEMIES::iterator I = m_enemies.begin();
		ENEMIES::iterator E = m_enemies.end();
		for (; I != E; ++I)
			(*I).m_visible_index = combat_members.front()->object().memory().visual().add_fictitious_visible_mask(
				(*I).m_object, combat_mask);
	}

	squad_mask_type non_combat_members = object().member().non_combat_members_mask();

	ENEMIES::iterator I = m_enemies.begin();
	ENEMIES::iterator E = m_enemies.end();
	for (; I != E; ++I)
		setup_mask(*I, non_combat_members);
}

void CAgentEnemyManager::assign_wounded()
{
	VERIFY(m_only_wounded_left);

#if 0//def DEBUG
	u32						enemy_mask = 0;
	ENEMIES::iterator		I = m_enemies.begin();
	ENEMIES::iterator		E = m_enemies.end();
	for ( ; I != E; ++I) {
		VERIFY				(!(*I).m_distribute_mask.get());
		enemy_mask			|= (*I).m_mask.get();
	}
	VERIFY					(enemy_mask == object().member().combat_mask());
#endif // DEBUG

	u32 previous_wounded_count = m_wounded.size();
	WOUNDED_ENEMY* previous_wounded = (WOUNDED_ENEMY*)_alloca(previous_wounded_count * sizeof(WOUNDED_ENEMY));
	std::copy(m_wounded.begin(), m_wounded.end(), previous_wounded);
	m_wounded.clear();

#ifdef DEBUG
	{
		ENEMIES::iterator	I = m_enemies.begin();
		ENEMIES::iterator	E = m_enemies.end();
		for ( ; I != E; ++I) {
			VERIFY			(!(*I).m_distribute_mask.get());
			VERIFY			((*I).m_mask.get());
		}
	}
#endif // DEBUG

	squad_mask_type assigned = 0;
	{
		WOUNDED_ENEMY* I = previous_wounded;
		WOUNDED_ENEMY* E = previous_wounded + previous_wounded_count;
		for (; I != E; ++I)
		{
			ENEMIES::iterator J = std::find(m_enemies.begin(), m_enemies.end(), (*I).first);
			if (J == m_enemies.end())
				continue;

			CMemberOrder* member_order = object().member().get_member((*I).second.first);
			if (!member_order)
				continue;

			squad_mask_type mask = object().member().mask((*I).second.first);
			if (!(object().member().combat_mask() & mask))
				continue;

			CAgentMemberManager::iterator i = object().member().member(mask);
			if ((*I).first->Position().distance_to_sqr((*i)->object().Position()) > _sqr(wounded_enemy_reached_distance)
			)
				continue;

			if (wounded_processor((*J).m_object) != ALife::_OBJECT_ID(-1))
				continue;

			wounded_processor((*J).m_object, (*I).second.first);
			(*J).m_distribute_mask.set(mask,TRUE);
			VERIFY((assigned | mask) != assigned);
			assigned |= mask;
		}
	}

	u32 combat_member_count = population(object().member().combat_mask());
	VERIFY(combat_member_count == object().member().combat_members().size());

	u32 population_level = 0;
	while (population(assigned) < combat_member_count)
	{
		CMemberEnemy* enemy = 0;
		const CAI_Stalker* processor = 0;
		float best_distance_sqr = flt_max;

		for (int i = 0; i < 2; ++i)
		{
			ENEMIES::iterator I = m_enemies.begin();
			ENEMIES::iterator E = m_enemies.end();
			for (; I != E; ++I)
			{
				if (population((*I).m_distribute_mask.get()) > population_level)
					continue;

				squad_mask_type J = (*I).m_mask.get();
				J &= (assigned ^ squad_mask_type(-1));
				for (; J; J &= J - 1)
				{
					squad_mask_type K = (J & (J - 1)) ^ J;
					CAgentMemberManager::iterator i = object().member().member(K);
					float distance_sqr = (*i)->object().Position().distance_to_sqr((*I).m_object->Position());
					if (distance_sqr < best_distance_sqr)
					{
						best_distance_sqr = distance_sqr;
						enemy = &*I;
						processor = &(*i)->object();
					}
				}
			}

			if (enemy)
				break;

			++population_level;
		}

#ifdef DEBUG
		if (!enemy) {
			Msg						(" ");
			Msg						(" ");
			Msg						("error will occur now, dumping valuable info");
			Msg						("wounded enemies(%d):",m_enemies.size());
			{
				typedef ENEMIES::iterator	iterator;
				iterator			I = m_enemies.begin();
				iterator			E = m_enemies.end();
				for ( ; I != E; ++I)
					Msg				(
						"  [%s][0x%08x][0x%08x][%.2f]",
						*(*I).m_object->cName(),
						(*I).m_mask.get(),
						(*I).m_distribute_mask.get(),
						(*I).m_probability
					);
			}
			Msg						("combat members(%d):",object().member().combat_members().size());
			{
				typedef CAgentMemberManager::MEMBER_STORAGE::const_iterator	const_iterator;
				const_iterator		I = object().member().combat_members().begin();
				const_iterator		E = object().member().combat_members().end();
				for ( ; I != E; ++I)
					Msg				(
						"  [%s][0x%08x][0x%08x]",
						*(*I)->object().cName(),
						object().member().mask(&(*I)->object()),
						(*I)->selected_enemy()
					);
			}
		}
#endif

		//		VERIFY						(enemy);
		//		VERIFY						(processor);

		// this situation is possible
		// for example
		// 2 soldiers in group
		// has 2 different enemies
		// one of the enemy is going offline
		// soldier, whose enemy went offline
		// nulls the selected enemy
		// agent manager updates before
		// soldier update and soldier knows nothing about the second enemy
		// we have situation where agent_manager
		// is trying to assign wounded for the soldier
		// who doesn't have enemies at the moment
		// since the last enemy went offline and he knows nothing about the second one
		// so, in this case we just need to stop iterating
		// since nest procedure (setup_enemy_masks)
		// will make the second enemy known for the soldier
		// and on its update he will select it
		if (!enemy)
			return;

		//		Msg							("wounded enemy [%s] is assigned to member [%s]",*enemy->m_object->cName(),*processor->cName());

		if (wounded_processor(enemy->m_object) == ALife::_OBJECT_ID(-1))
			wounded_processor(enemy->m_object, processor->ID());

		squad_mask_type mask = object().member().mask(processor);
		enemy->m_distribute_mask.set(mask,TRUE);
		VERIFY((assigned | mask) != assigned);
		assigned |= mask;
	}

	//	Msg								("[%6d] assigned = %x",Device.dwTimeGlobal,assigned);
	//	ENEMIES::iterator				I = m_enemies.begin();
	//	ENEMIES::iterator				E = m_enemies.end();
	//	for ( ; I != E; ++I)
	//		Msg							("[%6d] [%s] = %x",Device.dwTimeGlobal,*(*I).m_object->cName(),(*I).m_distribute_mask.get());
}

void CAgentEnemyManager::distribute_enemies()
{
	if (!object().member().combat_mask())
		return;

	const CMemberOrder* representative = object().member().combat_members().empty() ? nullptr :
		object().member().combat_members().front();
	LPCSTR profile_name = representative ? *representative->object().cName() : "agent_group";
	const u32 profile_id = representative ? static_cast<u32>(representative->object().ID()) : u32(-1);
	const u32 member_count = static_cast<u32>(object().member().combat_members().size());
	CScopedLuaRecordPhase total_phase(ai().script_engine(), "agent_enemy", "distribute.total",
		profile_name, profile_id, member_count);

	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "fill", profile_name, profile_id, member_count);

		fill_enemies();
	}

	if (m_enemies.empty())
		return;

	const u32 enemy_count = static_cast<u32>(m_enemies.size());
	if (m_only_wounded_left)
	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "assign_wounded", profile_name, profile_id, enemy_count);

		assign_wounded();
	}
	else if (m_enemies.size() == 1)
	{
		// With one non-wounded enemy there is no choice to optimize: the generic
		// algorithm evaluates the expensive VictoryProbability pattern only to end
		// up assigning that sole enemy to every combat member who already knows it.
		// Build the same member/enemy lists and masks without those evaluator calls.
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "single_enemy_fast", profile_name, profile_id, member_count);

		prepare_effectiveness_cache();
		m_enemies.front().m_distribute_mask.assign(m_enemies.front().m_mask.get());
		permutate_enemies();
	}
	else
	{
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "prepare", profile_name, profile_id, enemy_count);
			prepare_effectiveness_cache();
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "danger", profile_name, profile_id, enemy_count);

			compute_enemy_danger();
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "assign", profile_name, profile_id, enemy_count);

			assign_enemies();
		}
		{
			CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "permutate", profile_name, profile_id, enemy_count);

			permutate_enemies();
		}
	}

	{
		CScopedLuaRecordPhase phase(ai().script_engine(), "agent_enemy", "masks", profile_name, profile_id, enemy_count);

		assign_enemy_masks();
	}

	// The danger/assignment stages reorder m_enemies.  Restore the pointer
	// lookup once so every member's hot useful_enemy() query remains logarithmic
	// and still addresses the final distribution mask.
	rebuild_enemy_lookup();
}

u32 CAgentEnemyManager::merge_distributed_enemies(CEnemyManager& enemy_manager, squad_mask_type member_mask)
{
	m_member_merge_scratch.clear_not_free();
	if (m_member_merge_scratch.capacity() < m_enemies.size())
		m_member_merge_scratch.reserve(m_enemies.size());

	for (ENEMIES::const_iterator enemy = m_enemies.begin(); enemy != m_enemies.end(); ++enemy)
		if (enemy->m_distribute_mask.test(member_mask))
			m_member_merge_scratch.push_back(&*enemy);

	// CMemoryManager historically rescanned visual memory in vector order after
	// distribution.  Keep that observable callback order while visiting only
	// the actual assigned enemies.
	std::sort(
		m_member_merge_scratch.begin(), m_member_merge_scratch.end(),
		[](const CMemberEnemy* left, const CMemberEnemy* right)
		{
			if (left->m_visible_index != right->m_visible_index)
				return left->m_visible_index < right->m_visible_index;
			return std::less<const CEntityAlive*>()(left->m_object, right->m_object);
		});

	for (xr_vector<const CMemberEnemy*>::const_iterator enemy = m_member_merge_scratch.begin();
	     enemy != m_member_merge_scratch.end(); ++enemy)
		enemy_manager.add((*enemy)->m_object);

	return static_cast<u32>(m_member_merge_scratch.size());
}

struct wounded_predicate
{
	CObject* m_object;

	IC wounded_predicate(CObject* object)
	{
		VERIFY(object);
		m_object = object;
	}

	IC bool operator()(const CAgentEnemyManager::WOUNDED_ENEMY& wounded_enemy) const
	{
		if (wounded_enemy.first == m_object)
			return (true);

		if (wounded_enemy.second.first == m_object->ID())
			return (true);

		return (false);
	}
};

void CAgentEnemyManager::remove_links(CObject* object)
{
	m_wounded.erase(
		std::remove_if(
			m_wounded.begin(),
			m_wounded.end(),
			wounded_predicate(object)
		),
		m_wounded.end()
	);
}

void CAgentEnemyManager::update()
{}

ALife::_OBJECT_ID CAgentEnemyManager::wounded_processor(const CEntityAlive* object)
{
	WOUNDED_ENEMIES::const_iterator I = m_wounded.begin();
	WOUNDED_ENEMIES::const_iterator E = m_wounded.end();
	for (; I != E; ++I)
	{
		if ((*I).first == object)
			return ((*I).second.first);
	}

	return (ALife::_OBJECT_ID(-1));
}

class find_wounded_predicate
{
private:
	const CEntityAlive* m_object;

public:
	IC find_wounded_predicate(const CEntityAlive* object)
	{
		m_object = object;
		VERIFY(m_object);
	}

	IC bool operator()(const CAgentEnemyManager::WOUNDED_ENEMY& enemy) const
	{
		return (enemy.first == m_object);
	}
};

void CAgentEnemyManager::wounded_processor(const CEntityAlive* object, const ALife::_OBJECT_ID& wounded_processor_id)
{
	VERIFY(
		std::find_if(
			m_wounded.begin(),
			m_wounded.end(),
			find_wounded_predicate(object)
		) ==
		m_wounded.end()
	);
	m_wounded.push_back(std::make_pair(object, std::make_pair(wounded_processor_id, false)));
}

void CAgentEnemyManager::wounded_processed(const CEntityAlive* object, bool value)
{
	VERIFY(value);
	WOUNDED_ENEMIES::iterator I = std::find_if(m_wounded.begin(), m_wounded.end(), find_wounded_predicate(object));
	if (I == m_wounded.end())
		return;
	VERIFY((*I).second.first != ALife::_OBJECT_ID(-1));
	//	VERIFY							(!(*I).second.second);
	(*I).second.second = true;
}

bool CAgentEnemyManager::wounded_processed(const CEntityAlive* object) const
{
	WOUNDED_ENEMIES::const_iterator I = std::
		find_if(m_wounded.begin(), m_wounded.end(), find_wounded_predicate(object));
	if (I == m_wounded.end())
		return (false);
	return ((*I).second.second);
}

bool CAgentEnemyManager::assigned_wounded(const CEntityAlive* wounded, const CAI_Stalker* member)
{
	const u32 index = find_enemy_index(wounded);
	if (index >= m_enemies.size())
		return false;

	return !!m_enemies[index].m_distribute_mask.test(object().member().mask(member));
}

bool CAgentEnemyManager::useful_enemy(const CEntityAlive* enemy, const CAI_Stalker* member) const
{
	CAgentMemberManager& members = object().member();
	const squad_mask_type member_mask = members.mask(member);
	if (!(members.combat_mask() & member_mask))
		return (true);

	const u32 index = find_enemy_index(enemy);
	if (index >= m_enemies.size())
		return (true);

	return (!!m_enemies[index].m_distribute_mask.test(member_mask));
}
