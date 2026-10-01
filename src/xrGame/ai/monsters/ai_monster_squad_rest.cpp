#include "stdafx.h"
#include "ai_monster_squad.h"
#include "../../entity.h"
#include "../../ai_object_location.h"

void CMonsterSquad::ProcessIdle()
{
	m_temp_entities.clear();
	VERIFY(leader && !leader->getDestroy());

	// Выделить элементы с общими врагами и состянием атаки 
	for (MEMBER_GOAL_MAP_IT it_goal = m_goals.begin(); it_goal != m_goals.end(); it_goal++)
	{
		SMemberGoal goal = it_goal->second;
		if ((goal.type == MG_Rest) || (goal.type == MG_WalkGraph))
		{
			m_temp_entities.push_back(it_goal->first);
		}
	}

	Idle_AssignAction(m_temp_entities);
}

struct CPredicateSideSort
{
	Fvector target;

	CPredicateSideSort(Fvector pos) { target = pos; }

	bool operator()(CEntity* e1, CEntity* e2)
	{
		return (e1->Position().distance_to_sqr(target) > e2->Position().distance_to_sqr(target));
	}
};

u32 CMonsterSquad::idle_member_index(CEntity* entity) const
{
	auto it = m_idle_member_slots.find(entity);
	VERIFY(it != m_idle_member_slots.end());
	return it->second;
}

void CMonsterSquad::idle_remove_entity(ENTITY_VEC& side, CEntity* entity, u32 member_index,
	u32 SIdleSideIndices::* index_member)
{
	u32& side_index = m_idle_side_indices[member_index].*index_member;
	VERIFY(side_index < side.size() && side[side_index] == entity);

	CEntity* moved_entity = side.back();
	side[side_index] = moved_entity;
	side.pop_back();

	if (side_index < side.size())
	{
		const u32 moved_member_index = idle_member_index(moved_entity);
		m_idle_side_indices[moved_member_index].*index_member = side_index;
	}

	side_index = u32(-1);
}

#define CENTER_CIRCLE_DIST  20
#define CIRCLE_RADIUS_MIN	10
#define CIRCLE_RADIUS_MAX	15

void CMonsterSquad::Idle_AssignAction(ENTITY_VEC& members)
{
	// получить цель лидера
	SMemberGoal& goal = GetGoal(leader);

	if (goal.type == MG_WalkGraph)
	{
		front.clear();
		back.clear();
		left.clear();
		right.clear();

		for (ENTITY_VEC_IT IT = members.begin(); IT != members.end(); IT++)
		{
			if ((*IT) == leader) continue;

			front.push_back(*IT);
			back.push_back(*IT);
			left.push_back(*IT);
			right.push_back(*IT);
		}

		Fvector front_pos;
		Fvector back_pos;
		Fvector left_pos;
		Fvector right_pos;

		Fvector dir = leader->Direction();
		front_pos.mad(leader->Position(), dir, CENTER_CIRCLE_DIST);
		std::sort(front.begin(), front.end(), CPredicateSideSort(front_pos));

		dir.invert();
		back_pos.mad(leader->Position(), dir, CENTER_CIRCLE_DIST);
		std::sort(back.begin(), back.end(), CPredicateSideSort(back_pos));

		dir = leader->XFORM().i;
		right_pos.mad(leader->Position(), dir, CENTER_CIRCLE_DIST);
		std::sort(right.begin(), right.end(), CPredicateSideSort(right_pos));

		dir.invert();
		left_pos.mad(leader->Position(), dir, CENTER_CIRCLE_DIST);
		std::sort(left.begin(), left.end(), CPredicateSideSort(left_pos));

		m_idle_side_indices.resize(members.size());
		m_idle_member_slots.clear();
		const SIdleSideIndices invalid_indices = {u32(-1), u32(-1), u32(-1), u32(-1)};
		std::fill(m_idle_side_indices.begin(), m_idle_side_indices.end(), invalid_indices);
		for (u32 i = 0; i < members.size(); ++i)
			m_idle_member_slots.emplace(members[i], i);

		for (u32 i = 0; i < front.size(); ++i)
		{
			m_idle_side_indices[idle_member_index(front[i])].front_index = i;
			m_idle_side_indices[idle_member_index(back[i])].back_index = i;
			m_idle_side_indices[idle_member_index(left[i])].left_index = i;
			m_idle_side_indices[idle_member_index(right[i])].right_index = i;
		}

		SSquadCommand command;
		command.type = SC_FOLLOW;
		command.entity = leader;
		command.direction = leader->Direction();

		u8 cur_type = 0;
		while (!front.empty())
		{
			float random_r;
			Fvector random_dir;

			random_dir.random_dir();
			random_r = Random.randF(CIRCLE_RADIUS_MIN, CIRCLE_RADIUS_MAX);

			CEntity* entity = 0;
			switch (cur_type)
			{
			case 0: // front
				entity = front.back();
				command.position.mad(front_pos, random_dir, random_r);
				break;
			case 1: // back
				entity = back.back();
				command.position.mad(back_pos, random_dir, random_r);
				break;
			case 2: // left
				entity = left.back();
				command.position.mad(left_pos, random_dir, random_r);
				break;
			case 3: // right
				entity = right.back();
				command.position.mad(right_pos, random_dir, random_r);
				break;
			default:
				NODEFAULT;
			}

			const u32 member_index = idle_member_index(entity);
			idle_remove_entity(front, entity, member_index, &SIdleSideIndices::front_index);
			idle_remove_entity(back, entity, member_index, &SIdleSideIndices::back_index);
			idle_remove_entity(left, entity, member_index, &SIdleSideIndices::left_index);
			idle_remove_entity(right, entity, member_index, &SIdleSideIndices::right_index);

			cur_type++;
			if (cur_type > 3) cur_type = 0;

			UpdateCommand(entity, command);
		}
	}
	else if (goal.type == MG_Rest)
	{
		// пересчитать положение в команде в соответствие с целью лидера
		for (ENTITY_VEC_IT it = members.begin(); it != members.end(); it++)
		{
			if ((*it) == leader) continue;

			SSquadCommand command;
			command.type = SC_REST;
			command.position = leader->Position();
			command.node = leader->ai_location().level_vertex_id();
			command.entity = 0;

			UpdateCommand(*it, command);
		}
	}
}
