#include "stdafx.h"
#include "monster_corpse_manager.h"
#include "BaseMonster/base_monster.h"
#include "../../ai_object_location.h"

CMonsterCorpseManager::CMonsterCorpseManager()
{
	vertex = 0;
	time_last_seen = 0;
	monster = 0;
	corpse = 0;
	forced = false;
}

CMonsterCorpseManager::~CMonsterCorpseManager()
{
}

void CMonsterCorpseManager::init_external(CBaseMonster* M)
{
	monster = M;
}

void CMonsterCorpseManager::update()
{
	if (forced)
	{
		if (corpse->m_fFood < 1)
		{
			corpse = 0;
			return;
		}
	}
	else
	{
		SMonsterCorpse corpse_info;
		corpse = monster->CorpseMemory.get_corpse(corpse_info);

		if (corpse)
		{
			position = corpse_info.position;
			vertex = corpse_info.vertex;
			time_last_seen = corpse_info.time;
		}
	}
}

void CMonsterCorpseManager::force_corpse(const CEntityAlive* corpse)
{
	this->corpse = corpse;
	position = corpse->Position();
	vertex = corpse->ai_location().level_vertex_id();
	time_last_seen = Device.dwTimeGlobal;

	forced = true;
}

void CMonsterCorpseManager::unforce_corpse()
{
	SMonsterCorpse corpse_info;
	corpse = monster->CorpseMemory.get_corpse(corpse_info);

	if (corpse)
	{
		position = corpse_info.position;
		vertex = corpse_info.vertex;
		time_last_seen = corpse_info.time;
	}

	forced = false;
}

void CMonsterCorpseManager::reinit()
{
	corpse = 0;
	forced = false;
	time_last_seen = 0;
}

void CMonsterCorpseManager::remove_links(CObject* O)
{
	if (corpse == O)
	{
		corpse = NULL;
		// A forced corpse cannot remain forced after its object is destroyed.
		// CMonsterCorpseManager::update() dereferences forced corpses directly.
		forced = false;
	}
}
