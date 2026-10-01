////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_memory_manager.cpp
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent memory manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "agent_memory_manager.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "entity_alive.h"
#include "memory_space_impl.h"

int psAIVisualMemoryIndexedMerge = 1;

u32 CAgentMemoryManager::visual_merge_mask_index(squad_mask_type mask) const
{
	if (!mask || (mask & (mask - 1)))
		return u32(-1);

	u32 index = 0;
	while (!(mask & squad_mask_type(1)))
	{
		mask >>= 1;
		++index;
	}
	return index;
}

void CAgentMemoryManager::invalidate_visual_merge_candidates()
{
	m_visual_merge_indices_valid = false;
	++m_visual_merge_revision;
}

bool CAgentMemoryManager::visual_merge_relevant(const CVisibleObject& memory_object) const
{
	CGameObject* const game_object = const_cast<CGameObject*>(memory_object.m_object);
	if (!game_object)
		return false;

	// For stalker memory merging, only these runtime classes can reach one of the
	// three consumers in CMemoryManager::update(): enemy/danger or item. Runtime
	// class does not change for a live game object, while all dynamic predicates
	// (alive state, relations, restrictions, Lua usefulness, etc.) remain on the
	// original per-member path.
	return game_object->cast_entity_alive() || game_object->cast_inventory_item() || game_object->cast_explosive();
}

void CAgentMemoryManager::rebuild_visual_merge_indices()
{
	VERIFY(m_visible_objects);

	for (u32 bit = 0; bit < visual_merge_mask_bits; ++bit)
		m_visual_merge_indices[bit].clear_not_free();

	const VISIBLES& objects = *m_visible_objects;
	m_visual_merge_relevant.resize(objects.size());
	m_visual_merge_member_count = _min(
		static_cast<u32>(object().member().members().size()),
		static_cast<u32>(visual_merge_mask_bits));

	for (u32 bit = 0; bit < m_visual_merge_member_count; ++bit)
		m_visual_merge_indices[bit].reserve(objects.size());

	u32 indexed_count = 0;
	for (u32 index = 0, count = static_cast<u32>(objects.size()); index < count; ++index)
	{
		const CVisibleObject& memory_object = objects[index];
		const bool relevant = visual_merge_relevant(memory_object);
		m_visual_merge_relevant[index] = relevant ? 1u : 0u;
		if (!memory_object.m_enabled || !relevant)
			continue;

		squad_mask_type object_mask = memory_object.m_squad_mask.get();
		for (u32 bit = 0; bit < m_visual_merge_member_count; ++bit, object_mask >>= 1)
		{
			if (object_mask & squad_mask_type(1))
			{
				m_visual_merge_indices[bit].push_back(index);
				++indexed_count;
			}
		}
	}

	m_visual_merge_source_size = static_cast<u32>(objects.size());
	m_visual_merge_indices_valid = true;

}

bool CAgentMemoryManager::validate_visual_merge_indices(u32 mask_index, squad_mask_type mask)
{
	VERIFY(m_visible_objects);
	m_visual_merge_validation_scratch.clear_not_free();

	const VISIBLES& objects = *m_visible_objects;
	for (u32 index = 0, count = static_cast<u32>(objects.size()); index < count; ++index)
	{
		const CVisibleObject& memory_object = objects[index];
		if (memory_object.m_enabled && visual_merge_relevant(memory_object) && memory_object.m_squad_mask.test(mask))
			m_visual_merge_validation_scratch.push_back(index);
	}

	const VISUAL_MERGE_INDICES& indexed = m_visual_merge_indices[mask_index];
	return indexed.size() == m_visual_merge_validation_scratch.size() &&
		std::equal(indexed.begin(), indexed.end(), m_visual_merge_validation_scratch.begin());
}

const CAgentMemoryManager::VISUAL_MERGE_INDICES* CAgentMemoryManager::visual_merge_candidates(
	const VISIBLES& objects, squad_mask_type mask)
{
	if (!psAIVisualMemoryIndexedMerge || !m_visible_objects || m_visible_objects != &objects)
		return nullptr;

	const u32 mask_index = visual_merge_mask_index(mask);
	const u32 member_count = static_cast<u32>(object().member().members().size());
	if (mask_index >= visual_merge_mask_bits || mask_index >= member_count)
		return nullptr;

	if (!m_visual_merge_indices_valid || m_visual_merge_source_size != objects.size() ||
		m_visual_merge_member_count != member_count)
	{
		rebuild_visual_merge_indices();
	}

	if (psAIVisualMemoryIndexedMerge >= 2 && !validate_visual_merge_indices(mask_index, mask))
	{
		Msg("! AI visual-memory indexed merge mismatch; reverting to the legacy full scan");
		psAIVisualMemoryIndexedMerge = 0;
		invalidate_visual_merge_candidates();
		return nullptr;
	}

	const VISUAL_MERGE_INDICES& candidates = m_visual_merge_indices[mask_index];
	// When every source slot is eligible, the indexed loop can only add the
	// snapshot-copy overhead; use the identical legacy loop for that observer.
	return candidates.size() == objects.size() ? nullptr : &candidates;
}

void CAgentMemoryManager::visual_object_filter_changed(
	u32 index, squad_mask_type old_mask, bool old_enabled)
{
	if (!m_visual_merge_indices_valid)
		return;

	if (!m_visible_objects || m_visual_merge_source_size != m_visible_objects->size() ||
		m_visual_merge_member_count != object().member().members().size() || index >= m_visible_objects->size() ||
		m_visual_merge_relevant.size() != m_visible_objects->size())
	{
		invalidate_visual_merge_candidates();
		return;
	}

	const CVisibleObject& memory_object = (*m_visible_objects)[index];
	const bool old_relevant = m_visual_merge_relevant[index] != 0;
	const bool new_relevant = visual_merge_relevant(memory_object);
	m_visual_merge_relevant[index] = new_relevant ? 1u : 0u;
	const squad_mask_type new_mask = memory_object.m_squad_mask.get();
	const bool new_enabled = !!memory_object.m_enabled;
	bool changed = false;

	for (u32 bit = 0; bit < m_visual_merge_member_count; ++bit)
	{
		const squad_mask_type bit_mask = squad_mask_type(1) << bit;
		const bool was_eligible = old_enabled && old_relevant && !!(old_mask & bit_mask);
		const bool is_eligible = new_enabled && new_relevant && !!(new_mask & bit_mask);
		if (was_eligible == is_eligible)
			continue;
		changed = true;

		VISUAL_MERGE_INDICES& candidates = m_visual_merge_indices[bit];
		VISUAL_MERGE_INDICES::iterator position = std::lower_bound(candidates.begin(), candidates.end(), index);
		if (was_eligible)
		{
			if (position == candidates.end() || *position != index)
			{
				invalidate_visual_merge_candidates();
				return;
			}
			candidates.erase(position);
		}
		else
		{
			if (position != candidates.end() && *position == index)
			{
				invalidate_visual_merge_candidates();
				return;
			}
			candidates.insert(position, index);
		}
	}

	if (changed)
		++m_visual_merge_revision;
}

void CAgentMemoryManager::visual_object_appended(u32 index)
{
	if (!m_visual_merge_indices_valid)
		return;

	if (!m_visible_objects || index >= m_visible_objects->size() ||
		m_visible_objects->size() != static_cast<size_t>(m_visual_merge_source_size) + 1 ||
		index != m_visual_merge_source_size)
	{
		invalidate_visual_merge_candidates();
		return;
	}

	m_visual_merge_relevant.push_back(0u);
	m_visual_merge_source_size = static_cast<u32>(m_visible_objects->size());
	visual_object_filter_changed(index, 0, false);
}

template <typename T>
void CAgentMemoryManager::rebuild_object_index(const xr_vector<T>& objects, OBJECT_INDEX& index)
{
	index.clear();
	index.reserve(objects.size());

	for (u32 i = 0, n = (u32)objects.size(); i < n; ++i)
		index.push_back(std::make_pair((ALife::_OBJECT_ID)object_id(objects[i].m_object), i));

	std::sort(index.begin(), index.end());
}

template <typename T>
bool CAgentMemoryManager::object_index_matches(const xr_vector<T>& objects, const OBJECT_INDEX& index) const
{
	if (index.size() != objects.size())
		return false;

	for (OBJECT_INDEX::const_iterator I = index.begin(); I != index.end(); ++I)
	{
		if (I->second >= objects.size() ||
			object_id(objects[I->second].m_object) != I->first)
		{
			return false;
		}
	}
	return true;
}

template <typename T>
void CAgentMemoryManager::ensure_object_index(const xr_vector<T>& objects, OBJECT_INDEX& index)
{
	if (!object_index_matches(objects, index))
		rebuild_object_index(objects, index);
}

template <typename T>
u32 CAgentMemoryManager::find_object_index(
	const xr_vector<T>& objects,
	const OBJECT_INDEX& index,
	ALife::_OBJECT_ID id
) const
{
	const std::pair<ALife::_OBJECT_ID, u32> key = std::make_pair(id, 0u);
	OBJECT_INDEX::const_iterator I = std::lower_bound(index.begin(), index.end(), key);
	if ((I != index.end()) && (I->first == id))
	{
		const u32 object_index = I->second;
		if ((object_index < objects.size()) && (object_id(objects[object_index].m_object) == id))
			return (object_index);
	}

	// The squad memory can be modified between manager ticks. Preserve exact
	// behaviour through a validated linear fallback instead of trusting stale data.
	typename xr_vector<T>::const_iterator J = std::find(objects.begin(), objects.end(), id);
	return (J == objects.end()) ? u32(-1) : u32(J - objects.begin());
}

void CAgentMemoryManager::update()
{
	reset_memory_masks();
	ensure_object_index(visibles(), m_visible_index);
	ensure_object_index(sounds(), m_sound_index);
	ensure_object_index(hits(), m_hit_index);
}

void CAgentMemoryManager::remove_links(CObject* object)
{}

template <typename T>
IC bool CAgentMemoryManager::reset_memory_masks(T& objects)
{
	const squad_mask_type combat_mask = object().member().combat_mask();
	if (!combat_mask)
		return false;

	bool changed = false;
	typename T::iterator I = objects.begin();
	typename T::iterator E = objects.end();
	for (; I != E; ++I)
	{
		const squad_mask_type current_mask = (*I).m_squad_mask.get();
		if (!(combat_mask & current_mask))
			continue;

		const squad_mask_type merged_mask = current_mask | combat_mask;
		if (merged_mask == current_mask)
			continue;

		(*I).m_squad_mask.assign(merged_mask);
		changed = true;
	}
	return changed;
}

void CAgentMemoryManager::reset_memory_masks()
{
	if (reset_memory_masks(visibles()))
		invalidate_visual_merge_candidates();
	reset_memory_masks(sounds());
	reset_memory_masks(hits());
}

template <typename T>
IC void CAgentMemoryManager::update_memory_masks(const squad_mask_type& mask, T& objects)
{
	typename T::iterator I = objects.begin();
	typename T::iterator E = objects.end();
	for (; I != E; ++I)
	{
		squad_mask_type m = (*I).m_squad_mask.get();
		update_memory_mask(mask, m);
		(*I).m_squad_mask.assign(m);
	}
}

void CAgentMemoryManager::update_memory_masks(const squad_mask_type& mask)
{
	update_memory_masks(mask, visibles());
	// Removing a member shifts every higher mask bit. Preserve the legacy
	// ordering by rebuilding all per-member views after the shift.
	invalidate_visual_merge_candidates();
	update_memory_masks(mask, sounds());
	update_memory_masks(mask, hits());

	VISIBLES::iterator I = visibles().begin();
	VISIBLES::iterator E = visibles().end();
	for (; I != E; ++I)
	{
		squad_mask_type m = (*I).m_visible.get();
		update_memory_mask(mask, m);
		(*I).m_visible.assign(m);
	}
}

void CAgentMemoryManager::object_information(const CObject* object, u32& level_time, Fvector& position,
	                                         u32& visible_index, u32& sound_index, u32& hit_index)
{
	const ALife::_OBJECT_ID id = object_id(object);

	const VISIBLES& visible_objects = visibles();
	visible_index = find_object_index(visible_objects, m_visible_index, id);
	if (visible_index < visible_objects.size())
	{
		const CVisibleObject& visible = visible_objects[visible_index];
		level_time = visible.m_last_level_time;
		position = visible.m_object_params.m_position;
	}

	const SOUNDS& sound_objects = sounds();
	sound_index = find_object_index(sound_objects, m_sound_index, id);
	if (sound_index < sound_objects.size())
	{
		const CSoundObject& sound = sound_objects[sound_index];
		if (level_time < sound.m_last_level_time)
		{
			level_time = sound.m_last_level_time;
			position = sound.m_object_params.m_position;
		}
	}

	const HITS& hit_objects = hits();
	hit_index = find_object_index(hit_objects, m_hit_index, id);
	if (hit_index < hit_objects.size())
	{
		const CHitObject& hit = hit_objects[hit_index];
		if (level_time < hit.m_last_level_time)
		{
			level_time = hit.m_last_level_time;
			position = hit.m_object_params.m_position;
		}
	}
}
