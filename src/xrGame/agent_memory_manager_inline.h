////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_memory_manager.h
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent memory manager
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CAgentMemoryManager::CAgentMemoryManager(CAgentManager* object)
{
	VERIFY(object);
	m_object = object;
	m_visible_objects = 0;
	m_sound_objects = 0;
	m_hit_objects = 0;
	m_visual_merge_indices_valid = false;
	m_visual_merge_source_size = 0;
	m_visual_merge_member_count = 0;
	m_visual_merge_revision = 1;
}

IC CAgentManager& CAgentMemoryManager::object() const
{
	VERIFY(m_object);
	return (*m_object);
}

IC void CAgentMemoryManager::set_squad_objects(VISIBLES* objects)
{
	m_visible_objects = objects;
	m_visible_index.clear();
	invalidate_visual_merge_candidates();
}

IC void CAgentMemoryManager::set_squad_objects(SOUNDS* objects)
{
	m_sound_objects = objects;
	m_sound_index.clear();
}

IC void CAgentMemoryManager::set_squad_objects(HITS* objects)
{
	m_hit_objects = objects;
	m_hit_index.clear();
}

IC u32 CAgentMemoryManager::visual_merge_revision() const
{
	return m_visual_merge_revision;
}

IC CAgentMemoryManager::VISIBLES& CAgentMemoryManager::visibles() const
{
	VERIFY(m_visible_objects);
	return (*m_visible_objects);
}

IC CAgentMemoryManager::SOUNDS& CAgentMemoryManager::sounds() const
{
	VERIFY(m_sound_objects);
	return (*m_sound_objects);
}

IC CAgentMemoryManager::HITS& CAgentMemoryManager::hits() const
{
	VERIFY(m_hit_objects);
	return (*m_hit_objects);
}

IC void CAgentMemoryManager::update_memory_mask(const squad_mask_type& mask, squad_mask_type& current)
{
	// this function removes specified bit and shifts all the others
	current = (((mask ^ squad_mask_type(-1) ^ (mask - 1)) & current) >> 1) | (current & (mask - 1));
}
