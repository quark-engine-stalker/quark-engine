#include "stdafx.h"
#include "monster_hit_memory.h"
#include "BaseMonster/base_monster.h"

CMonsterHitMemory::CMonsterHitMemory()
{
	monster = 0;
	time_memory = 10000;
	reset_last_hit();
}

CMonsterHitMemory::~CMonsterHitMemory()
{
}

void CMonsterHitMemory::init_external(CBaseMonster* M, TTime mem_time)
{
	monster = M;
	time_memory = mem_time;
}


void CMonsterHitMemory::update()
{
	// удалить устаревшие hits
	remove_non_actual();
}

bool CMonsterHitMemory::is_hit(CObject* pO)
{
	return (std::find(m_hits.begin(), m_hits.end(), pO) != m_hits.end());
}

void CMonsterHitMemory::add_hit(CObject* who, EHitSide side)
{
	SMonsterHit new_hit_info;
	new_hit_info.object = who;
	new_hit_info.time = Device.dwTimeGlobal;
	new_hit_info.side = side;
	new_hit_info.position = monster->Position();

	MONSTER_HIT_VECTOR_IT it = std::find(m_hits.begin(), m_hits.end(), who);

	if (it == m_hits.end()) m_hits.push_back(new_hit_info);
	else *it = new_hit_info;

	refresh_last_hit();
}

struct predicate_old_hit
{
	TTime cur_time;
	TTime mem_time;

	predicate_old_hit(TTime mem_time, TTime cur_time)
	{
		this->cur_time = cur_time;
		this->mem_time = mem_time;
	}

	IC bool operator()(const SMonsterHit& hit_info)
	{
		if ((mem_time + hit_info.time) < cur_time) return true;
		if (hit_info.object)
		{
			CEntityAlive* entity = smart_cast<CEntityAlive *>(hit_info.object);
			if (entity && !entity->g_Alive()) return true;
		}
		return false;
	}
};

void CMonsterHitMemory::remove_non_actual()
{
	const u32 previous_size = m_hits.size();
	m_hits.erase(
		std::remove_if(
			m_hits.begin(),
			m_hits.end(),
			predicate_old_hit(
				time_memory,
				Device.dwTimeGlobal
			)
		),
		m_hits.end()
	);

	if (m_hits.size() != previous_size)
		refresh_last_hit();
}

Fvector CMonsterHitMemory::get_last_hit_dir()
{
	Fvector dir = monster->Direction();

	// если есть хит, вычислить направление
	if (m_last_hit.time != 0)
	{
		float h, p;
		dir.getHP(h, p);

		switch (m_last_hit.side)
		{
		case eSideBack:
			h += PI;
			break;
		case eSideLeft:
			h += PI_DIV_2;
			break;
		case eSideRight:
			h -= PI_DIV_2;
			break;
		}

		dir.setHP(h, p);
		dir.normalize();
	}

	return dir;
}

TTime CMonsterHitMemory::get_last_hit_time()
{
	return m_last_hit.time;
}

CObject* CMonsterHitMemory::get_last_hit_object()
{
	return m_last_hit.object;
}

Fvector CMonsterHitMemory::get_last_hit_position()
{
	return m_last_hit.position;
}

struct predicate_old_info
{
	const CObject* object;

	predicate_old_info(const CObject* obj) : object(obj)
	{
	}

	IC bool operator()(const SMonsterHit& hit_info)
	{
		return (object == hit_info.object);
	}
};

bool CMonsterHitMemory::remove_hit_info(const CObject* obj)
{
	const u32 previous_size = m_hits.size();
	m_hits.erase(
		std::remove_if(
			m_hits.begin(),
			m_hits.end(),
			predicate_old_info(obj)
		),
		m_hits.end()
	);

	const bool removed = m_hits.size() != previous_size;
	if (removed)
		refresh_last_hit();

	return removed;
}

void CMonsterHitMemory::clear()
{
	m_hits.clear();
	reset_last_hit();
}

void CMonsterHitMemory::reset_last_hit()
{
	m_last_hit.object = 0;
	m_last_hit.time = 0;
	m_last_hit.side = eSideFront;
	m_last_hit.position.set(0.f, 0.f, 0.f);
}

void CMonsterHitMemory::refresh_last_hit()
{
	reset_last_hit();
	for (u32 i = 0; i < m_hits.size(); ++i)
	{
		if (m_hits[i].time > m_last_hit.time)
			m_last_hit = m_hits[i];
	}
}
