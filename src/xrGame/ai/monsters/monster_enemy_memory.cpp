#include "pch_script.h"
#include "monster_enemy_memory.h"
#include "BaseMonster/base_monster.h"
#include "../../memory_manager.h"
#include "../../visual_memory_manager.h"
#include "../../enemy_manager.h"
#include "../../ai_object_location.h"
#include "monster_home.h"
#include "Dog/dog.h"
#include "ai_monster_squad.h"
#include "ai_monster_squad_manager.h"
#include "../../Actor.h"
#include "../../actor_memory.h"

CMonsterEnemyMemory::CMonsterEnemyMemory()
{
	monster = 0;
	time_memory = 15000;
}

CMonsterEnemyMemory::~CMonsterEnemyMemory()
{
}

void CMonsterEnemyMemory::init_external(CBaseMonster* M, TTime mem_time)
{
	monster = M;
	time_memory = mem_time;
}

extern CActor* g_actor;

void CMonsterEnemyMemory::update()
{
	VERIFY(monster->g_Alive());

	CMonsterHitMemory& monster_hit_memory = monster->HitMemory;
	const Fvector monster_position = monster->Position();
	const float feel_recent_hit_distance = monster->get_feel_enemy_who_just_hit_max_distance();
	const float feel_sound_distance = monster->get_feel_enemy_who_made_sound_max_distance();
	const float feel_enemy_distance = monster->get_feel_enemy_max_distance();
	const float feel_recent_hit_distance_sqr = _sqr(feel_recent_hit_distance);
	const float feel_sound_distance_sqr = _sqr(feel_sound_distance);
	const float feel_enemy_distance_sqr = _sqr(feel_enemy_distance);

	typedef CObjectManager<const CEntityAlive>::OBJECTS objects_list;

	objects_list const& objects = monster->memory().enemy().objects();

	if (monster_hit_memory.is_hit() && time() < monster_hit_memory.get_last_hit_time() + 1000)
	{
		if (CEntityAlive* enemy = smart_cast<CEntityAlive*>(monster->HitMemory.get_last_hit_object()))
		{
			if (monster->CCustomMonster::useful(&monster->memory().enemy(), enemy) &&
				feel_recent_hit_distance > 0.f &&
				monster_position.distance_to_sqr(enemy->Position()) < feel_recent_hit_distance_sqr)
			{
				add_enemy(enemy);

				bool const self_is_dog = !!smart_cast<const CAI_Dog*>(monster);
				if (self_is_dog)
				{
					CMonsterSquad* const squad = monster_squad().get_squad(monster);
					squad->set_home_in_danger();
				}
			}
		}
	}

	if (monster->SoundMemory.IsRememberSound() && g_actor
		&& g_actor->memory().visual().visible_now(monster))
	{
		SoundElem sound;
		bool dangerous;
		monster->SoundMemory.GetSound(sound, dangerous);
		if (dangerous && Device.dwTimeGlobal < sound.time + 2000)
		{
			if (CEntityAlive const* enemy = smart_cast<CEntityAlive const*>(sound.who))
			{
				const Fvector enemy_position = enemy->Position();
				const float xz_dist_sqr = monster_position.distance_to_xz_sqr(enemy_position);
				const float y_dist = _abs(monster_position.y - enemy_position.y);

				if (monster->CCustomMonster::useful(&monster->memory().enemy(), enemy) &&
					y_dist < 10 &&
					feel_sound_distance > 0.f &&
					xz_dist_sqr < feel_sound_distance_sqr)
				{
					add_enemy(enemy);

					bool const self_is_dog = !!smart_cast<const CAI_Dog*>(monster);
					if (self_is_dog)
					{
						CMonsterSquad* const squad = monster_squad().get_squad(monster);
						squad->set_home_in_danger();
					}
				}
			}
		}
	}

	for (objects_list::const_iterator I = objects.begin();
	     I != objects.end();
	     ++I)
	{
		const CEntityAlive* enemy = *I;
		const bool feel_enemy = feel_enemy_distance > 0.f &&
			monster_position.distance_to_sqr(enemy->Position()) < feel_enemy_distance_sqr;

		if (feel_enemy || monster->memory().visual().visible_now(*I))
			add_enemy(*I);
	}

	if (g_actor)
	{
		const Fvector actor_position = g_actor->Position();
		const float xz_dist_sqr = monster_position.distance_to_xz_sqr(actor_position);
		const float y_dist = _abs(monster_position.y - actor_position.y);

		if (feel_enemy_distance > 0.f &&
			xz_dist_sqr < feel_enemy_distance_sqr &&
			y_dist < 10 &&
			monster->memory().enemy().is_useful(g_actor) &&
			g_actor->memory().visual().visible_now(monster))
		{
			add_enemy(g_actor);
		}
	}

	// удалить устаревших врагов
	remove_non_actual();

	// обновить опасность 
	for (ENEMIES_MAP_IT it = m_objects.begin(); it != m_objects.end(); it++)
	{
		u8 relation_value = u8(monster->tfGetRelationType(it->first));
		float dist = monster_position.distance_to(it->second.position);
		it->second.danger = (1 + relation_value * relation_value * relation_value) / (1 + dist);
	}
}

void CMonsterEnemyMemory::add_enemy(const CEntityAlive* enemy)
{
	SMonsterEnemy enemy_info;
	enemy_info.position = enemy->Position();
	enemy_info.vertex = enemy->ai_location().level_vertex_id();
	enemy_info.time = Device.dwTimeGlobal;
	enemy_info.danger = 0.f;

	ENEMIES_MAP_IT it = m_objects.find(enemy);
	if (it != m_objects.end())
	{
		// обновить данные о враге
		it->second = enemy_info;
	}
	else
	{
		// добавить врага в список объектов
		m_objects.insert(mk_pair(enemy, enemy_info));
	}
}

void CMonsterEnemyMemory::add_enemy(const CEntityAlive* enemy, const Fvector& pos, u32 vertex, u32 time)
{
	SMonsterEnemy enemy_info;
	enemy_info.position = pos;
	enemy_info.vertex = vertex;
	enemy_info.time = time;
	enemy_info.danger = 0.f;

	ENEMIES_MAP_IT it = m_objects.find(enemy);
	if (it != m_objects.end())
	{
		// обновить данные о враге
		if (it->second.time < enemy_info.time) it->second = enemy_info;
	}
	else
	{
		// добавить врага в список объектов
		m_objects.insert(mk_pair(enemy, enemy_info));
	}
}

void CMonsterEnemyMemory::remove_non_actual()
{
	TTime cur_time = Device.dwTimeGlobal;

	// удалить 'старых' врагов и тех, расстояние до которых > 30м и др.
	for (ENEMIES_MAP_IT it = m_objects.begin(), nit;
	     it != m_objects.end();
	     it = nit)
	{
		nit = it;
		++nit;
		// проверить условия удаления
		if (!it->first ||
			!it->first->g_Alive() ||
			it->first->getDestroy() ||
			(it->second.time + time_memory < cur_time) ||
			(it->first->g_Team() == monster->g_Team()) ||
			!monster->memory().enemy().is_useful(it->first))
		{
			m_objects.erase(it);
		}
	}
}

const CEntityAlive* CMonsterEnemyMemory::get_enemy()
{
	ENEMIES_MAP_IT it = find_best_enemy();
	if (it != m_objects.end()) return it->first;
	return (0);
}

const CEntityAlive* CMonsterEnemyMemory::get_enemy(SMonsterEnemy& enemy_info)
{
	enemy_info.time = 0;

	ENEMIES_MAP_IT it = find_best_enemy();
	if (it != m_objects.end())
	{
		enemy_info = it->second;
		return it->first;
	}

	return (0);
}

SMonsterEnemy CMonsterEnemyMemory::get_enemy_info()
{
	SMonsterEnemy ret_val;
	ret_val.time = 0;

	ENEMIES_MAP_IT it = find_best_enemy();
	if (it != m_objects.end()) ret_val = it->second;

	return ret_val;
}

ENEMIES_MAP_IT CMonsterEnemyMemory::find_best_enemy()
{
	ENEMIES_MAP_IT best_at_home = m_objects.end();
	ENEMIES_MAP_IT best_anywhere = m_objects.end();
	float max_home_value = 0.f;
	float max_any_value = 0.f;

	for (ENEMIES_MAP_IT I = m_objects.begin(); I != m_objects.end(); I++)
	{
		if (I->second.danger > max_any_value)
		{
			max_any_value = I->second.danger;
			best_anywhere = I;
		}

		if (monster->Home->at_home(I->second.position) && I->second.danger > max_home_value)
		{
			max_home_value = I->second.danger;
			best_at_home = I;
		}
	}

	return best_at_home != m_objects.end() ? best_at_home : best_anywhere;
}

bool CMonsterEnemyMemory::remove_links(CObject* O)
{
	for (ENEMIES_MAP_IT I = m_objects.begin(); I != m_objects.end(); ++I)
	{
		if ((*I).first == O)
		{
			m_objects.erase(I);
			return true;
		}
	}

	return false;
}
