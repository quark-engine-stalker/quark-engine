#pragma once

class CMonsterSquad;
class CEntity;

class CMonsterSquadManager
{
	//------------------------------------------------------------------------
	// Monster classification: Team -> Level -> Squad
	// Note: Its names differ from global ones, which are: Team -> Squad -> Group
	//		 but nesting hierarchy logically means the same
	//		 Team->Level->Squad used only for private members and functions
	//------------------------------------------------------------------------

	DEFINE_VECTOR(CMonsterSquad*, MONSTER_SQUAD_VEC, MONSTER_SQUAD_VEC_IT);
	DEFINE_VECTOR(MONSTER_SQUAD_VEC, MONSTER_LEVEL_VEC, MONSTER_LEVEL_VEC_IT);
	DEFINE_VECTOR(MONSTER_LEVEL_VEC, MONSTER_TEAM_VEC, MONSTER_TEAM_VEC_IT);

	MONSTER_TEAM_VEC team;
	// CBaseMonster broadcasts the same relcase to this global manager from every
	// living monster. Keep the first ordered cleanup and suppress duplicate
	// all-squad scans for the same object in the same frame.
	u32 m_relcase_frame = u32(-1);
	xr_vector<u16> m_relcase_object_ids;

public:
	CMonsterSquadManager();
	~CMonsterSquadManager();

	void register_member(u8 team_id, u8 squad_id, u8 group_id, CEntity* e);
	void remove_member(u8 team_id, u8 squad_id, u8 group_id, CEntity* e);

	CMonsterSquad* get_squad(u8 team_id, u8 squad_id, u8 group_id);
	CMonsterSquad* get_squad(const CEntity* entity);

	void update(CEntity* entity);

	void remove_links(CObject* O);
};


IC CMonsterSquadManager& monster_squad();
extern CMonsterSquadManager* g_monster_squad;

#include "ai_monster_squad_manager_inline.h"
