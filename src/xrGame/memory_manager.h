////////////////////////////////////////////////////////////////////////////
//	Module 		: memory_manager.h
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Memory manager
////////////////////////////////////////////////////////////////////////////

#pragma once

class CVisualMemoryManager;
class CSoundMemoryManager;
class CHitMemoryManager;
class CEnemyManager;
class CItemManager;
class CDangerManager;
class CCustomMonster;
class CAI_Stalker;
class CEntityAlive;
class CGameObject;
class CSound_UserDataVisitor;

namespace MemorySpace
{
	struct CMemoryInfo;
};

class CMemoryManager
{
public:
	typedef MemorySpace::CMemoryInfo CMemoryInfo;

protected:
	CVisualMemoryManager* m_visual;
	CSoundMemoryManager* m_sound;
	CHitMemoryManager* m_hit;
	CEnemyManager* m_enemy;
	CItemManager* m_item;
	CDangerManager* m_danger;

protected:
	CCustomMonster* m_object;
	CAI_Stalker* m_stalker;

private:
	// Compact visual-only enemy snapshot built during this member's own memory
	// merge. Agent-group aggregation can reuse it instead of rescanning and
	// smart-casting the full visual memory list for every squad member.
	xr_vector<const CEntityAlive*> m_visual_enemy_candidates;
	bool m_visual_enemy_candidates_valid;

	struct CMergeObjectClass
	{
		const CGameObject* object = nullptr;
		const CEntityAlive* entity_alive = nullptr;
		bool stalker = false;
		bool inventory_item = false;
		bool explosive = false;
	};
	typedef xr_unordered_flat_map<u16, CMergeObjectClass> MERGE_CLASS_CACHE;
	static MERGE_CLASS_CACHE m_merge_class_cache;
	const CMergeObjectClass& merge_object_class(const CGameObject* object);

	struct CMergePlanEntry
	{
		const CGameObject* object = nullptr;
		u16 object_id = u16(-1);
		const CEntityAlive* entity_alive = nullptr;
		bool stalker = false;
		bool inventory_item = false;
		bool explosive = false;
	};

	struct CMergePlan
	{
		xr_vector<CMergePlanEntry> entries;

		void reset() { entries.clear(); }
	};

	CMergePlan m_visual_merge_plan;
	CMergePlan m_sound_merge_plan;
	CMergePlan m_hit_merge_plan;

	void update_enemies(const bool& registered_in_combat);

protected:
	template <typename T>
	void update(const xr_vector<T>& objects, CMergePlan& plan, bool add_enemies,
	            bool collect_visual_candidates = false, const xr_vector<u32>* candidate_indices = nullptr,
	            u64 observer_mask = 0);

public:
	CMemoryManager(CEntityAlive* entity_alive, CSound_UserDataVisitor* visitor);
	virtual ~CMemoryManager();
	virtual void Load(LPCSTR section);
	virtual void reinit();
	virtual void reload(LPCSTR section);
	virtual void update(float time_delta);
	void remove_links(CObject* object);
	virtual void on_restrictions_change();

public:
	void enable(const CObject* object, bool enable);
	CMemoryInfo memory(const CObject* object) const;
	u32 memory_time(const CObject* object) const;
	Fvector memory_position(const CObject* object) const;
	void make_object_visible_somewhen(const CEntityAlive* enemy);

public:
	template <typename T, typename _predicate>
	IC void fill_enemies(const xr_vector<T>& objects, const _predicate& predicate) const;
	template <typename _predicate>
	IC void fill_enemies(const _predicate& predicate) const;

public:
	IC CVisualMemoryManager& visual() const;
	IC CSoundMemoryManager& sound() const;
	IC CHitMemoryManager& hit() const;
	IC CEnemyManager& enemy() const;
	IC CItemManager& item() const;
	IC CDangerManager& danger() const;
	IC CCustomMonster& object() const;
	IC CAI_Stalker& stalker() const;

public:
	void save(NET_Packet& packet) const;
	void load(IReader& packet);
	void xr_stdcall on_requested_spawn(CObject* object);
};

#include "memory_manager_inline.h"
