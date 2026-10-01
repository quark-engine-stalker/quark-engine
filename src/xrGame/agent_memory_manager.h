////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_memory_manager_inline.h
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent memory manager inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <utility>

#include "memory_space.h"

class CAgentManager;

// 0 = legacy full scan, 1 = indexed merge, 2 = indexed merge with a full
// semantic validation scan. Mode 2 is intended for compatibility testing.
extern int psAIVisualMemoryIndexedMerge;

class CAgentMemoryManager
{
public:
	typedef MemorySpace::CVisibleObject CVisibleObject;
	typedef MemorySpace::CSoundObject CSoundObject;
	typedef MemorySpace::CHitObject CHitObject;

public:
	typedef xr_vector<CVisibleObject> VISIBLES;
	typedef xr_vector<CSoundObject> SOUNDS;
	typedef xr_vector<CHitObject> HITS;
	typedef MemorySpace::squad_mask_type squad_mask_type;
	typedef xr_vector<std::pair<ALife::_OBJECT_ID, u32> > OBJECT_INDEX;
	typedef xr_vector<u32> VISUAL_MERGE_INDICES;
	enum { visual_merge_mask_bits = sizeof(squad_mask_type) * 8 };

protected:
	CAgentManager* m_object;
	VISIBLES* m_visible_objects;
	SOUNDS* m_sound_objects;
	HITS* m_hit_objects;
	OBJECT_INDEX m_visible_index;
	OBJECT_INDEX m_sound_index;
	OBJECT_INDEX m_hit_index;
	// One ordered list of eligible visual-memory slots per squad member bit.
	// The source vector remains authoritative; this only skips entries whose
	// enabled/mask state proves that the observer cannot consume them.
	VISUAL_MERGE_INDICES m_visual_merge_indices[visual_merge_mask_bits];
	VISUAL_MERGE_INDICES m_visual_merge_validation_scratch;
	// Stable per-slot classification used by the indexed visual merge. A stalker
	// can only consume visual entries that are entities, inventory items or
	// explosives; everything else is a guaranteed no-op in CMemoryManager::update.
	// Keep this alongside the source vector so the shared squad index filters such
	// entries once instead of making every squad member rediscover that fact.
	xr_vector<u8> m_visual_merge_relevant;
	bool m_visual_merge_indices_valid;
	u32 m_visual_merge_source_size;
	u32 m_visual_merge_member_count;
	u32 m_visual_merge_revision;

protected:
	IC CAgentManager& object() const;

	template <typename T>
	void rebuild_object_index(const xr_vector<T>& objects, OBJECT_INDEX& index);

	template <typename T>
	bool object_index_matches(const xr_vector<T>& objects, const OBJECT_INDEX& index) const;

	template <typename T>
	void ensure_object_index(const xr_vector<T>& objects, OBJECT_INDEX& index);

	template <typename T>
	u32 find_object_index(const xr_vector<T>& objects, const OBJECT_INDEX& index, ALife::_OBJECT_ID id) const;

	void rebuild_visual_merge_indices();
	bool visual_merge_relevant(const CVisibleObject& object) const;
	u32 visual_merge_mask_index(squad_mask_type mask) const;
	bool validate_visual_merge_indices(u32 mask_index, squad_mask_type mask);

public:
	IC CAgentMemoryManager(CAgentManager* object);
	void update();
	void remove_links(CObject* object);

public:
	IC void set_squad_objects(VISIBLES* objects);
	IC void set_squad_objects(SOUNDS* objects);
	IC void set_squad_objects(HITS* objects);

public:
	IC VISIBLES& visibles() const;
	IC SOUNDS& sounds() const;
	IC HITS& hits() const;

public:
	template <typename T>
	IC bool reset_memory_masks(T& objects);
	void reset_memory_masks();

	template <typename T>
	IC void update_memory_masks(const squad_mask_type& mask, T& objects);
	IC void update_memory_mask(const squad_mask_type& mask, squad_mask_type& current);
	void update_memory_masks(const squad_mask_type& mask);
	void object_information(const CObject* object, u32& level_time, Fvector& position,
	                        u32& visible_index, u32& sound_index, u32& hit_index);

	const VISUAL_MERGE_INDICES* visual_merge_candidates(const VISIBLES& objects, squad_mask_type mask);
	void visual_object_filter_changed(u32 index, squad_mask_type old_mask, bool old_enabled);
	void visual_object_appended(u32 index);
	void invalidate_visual_merge_candidates();
	IC u32 visual_merge_revision() const;
};

#include "agent_memory_manager_inline.h"
