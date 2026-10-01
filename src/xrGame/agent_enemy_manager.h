////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_enemy_manager.h
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent enemy manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "member_enemy.h"

class CAgentManager;
class CMemberOrder;
class CEntityAlive;
class CAI_Stalker;
class CEnemyManager;

class CAgentEnemyManager
{
public:
	typedef xr_vector<CMemberEnemy> ENEMIES;
	typedef MemorySpace::squad_mask_type squad_mask_type;
	typedef std::pair<ALife::_OBJECT_ID, bool> WOUNDED;
	typedef std::pair<const CEntityAlive *, WOUNDED> WOUNDED_ENEMY;
	typedef xr_vector<WOUNDED_ENEMY> WOUNDED_ENEMIES;
	typedef std::pair<const CEntityAlive*, u32> ENEMY_LOOKUP_ENTRY;
	typedef xr_vector<ENEMY_LOOKUP_ENTRY> ENEMY_LOOKUP;

private:
	CAgentManager* m_object;
	ENEMIES m_enemies;
	ENEMY_LOOKUP m_enemy_lookup;
	WOUNDED_ENEMIES m_wounded;
	bool m_only_wounded_left;
	bool m_is_any_wounded;
	xr_vector<CMemberOrder*> m_effectiveness_members;
	xr_vector<squad_mask_type> m_effectiveness_member_masks;
	xr_vector<float> m_member_enemy_effectiveness;
	u32 m_effectiveness_enemy_count;
	u32 m_effectiveness_row_by_member[sizeof(squad_mask_type) * 8];
	xr_vector<const CMemberEnemy*> m_member_merge_scratch;

protected:
	template <typename T>
	IC void setup_mask(xr_vector<T>& objects, CMemberEnemy& enemy, u32& cached_index,
	                   const squad_mask_type& non_combat_members);
	IC void setup_mask(CMemberEnemy& enemy, const squad_mask_type& non_combat_members);
	void fill_enemies();
	void compute_enemy_danger();
	void assign_enemies();
	void permutate_enemies();
	void assign_wounded();
	void assign_enemy_masks();
	float evaluate(const CEntityAlive* object0, const CEntityAlive* object1) const;
	void prepare_effectiveness_cache();
	float member_enemy_effectiveness(squad_mask_type member_mask, u32 enemy_index);
	CMemberOrder* effectiveness_member(squad_mask_type member_mask) const;
	void swap_effectiveness_columns(u32 enemy_index0, u32 enemy_index1);
	u32 member_mask_index(squad_mask_type member_mask) const;
	u32 find_enemy_index(const CEntityAlive* enemy) const;
	void rebuild_enemy_lookup();
	void exchange_enemies(CMemberOrder& member0, squad_mask_type mask0,
	                     CMemberOrder& member1, squad_mask_type mask1);
	IC CAgentManager& object() const;

public:
	IC CAgentEnemyManager(CAgentManager* object);
	void update();
	void distribute_enemies();
	u32 merge_distributed_enemies(CEnemyManager& enemy_manager, squad_mask_type member_mask);
	IC ENEMIES& enemies();
	void remove_links(CObject* object);

private:
	void wounded_processor(const CEntityAlive* object, const ALife::_OBJECT_ID& wounded_processor_id);

public:
	ALife::_OBJECT_ID wounded_processor(const CEntityAlive* object);
	void wounded_processed(const CEntityAlive* object, bool value);
	bool wounded_processed(const CEntityAlive* object) const;
	bool assigned_wounded(const CEntityAlive* wounded, const CAI_Stalker* member);
	bool useful_enemy(const CEntityAlive* enemy, const CAI_Stalker* member) const;
};

#include "agent_enemy_manager_inline.h"
