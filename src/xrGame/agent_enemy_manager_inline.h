////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_enemy_manager_inline.h
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent enemy manager inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CAgentEnemyManager::CAgentEnemyManager(CAgentManager* object)
{
	VERIFY(object);
	m_object = object;
	m_only_wounded_left = false;
	m_is_any_wounded = false;
	m_effectiveness_enemy_count = 0;
	m_member_merge_scratch.reserve(16);
	for (u32 i = 0; i < sizeof(m_effectiveness_row_by_member) / sizeof(m_effectiveness_row_by_member[0]); ++i)
		m_effectiveness_row_by_member[i] = u32(-1);
}

IC CAgentManager& CAgentEnemyManager::object() const
{
	VERIFY(m_object);
	return (*m_object);
}

IC CAgentEnemyManager::ENEMIES& CAgentEnemyManager::enemies()
{
	return (m_enemies);
}
