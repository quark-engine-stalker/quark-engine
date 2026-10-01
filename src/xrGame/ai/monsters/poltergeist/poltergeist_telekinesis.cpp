#include "stdafx.h"
#include "poltergeist.h"
#include "../../../PhysicsShellHolder.h"
#include "../../../level.h"
#include "../../../actor.h"
#include "../../../../xrPhysics/icolisiondamageinfo.h"
#include "inventory_item.h"

CPolterTele::CPolterTele(CPoltergeist* polter) : inherited(polter), m_pmt_object_collision_damage(0.5f)
{
	m_tele_candidates.reserve(20);
}

CPolterTele::~CPolterTele()
{
}

void CPolterTele::load(LPCSTR section)
{
	inherited::load(section);

	m_pmt_radius = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Find_Radius", 10.f);
	m_pmt_object_min_mass = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Object_Min_Mass", 40.f);
	m_pmt_object_max_mass = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Object_Max_Mass", 500.f);
	m_pmt_object_count = READ_IF_EXISTS(pSettings, r_u32, section, "Tele_Object_Count", 10);
	m_pmt_time_to_hold = READ_IF_EXISTS(pSettings, r_u32, section, "Tele_Hold_Time", 3000);
	m_pmt_time_to_wait = READ_IF_EXISTS(pSettings, r_u32, section, "Tele_Wait_Time", 3000);
	m_pmt_time_to_wait_in_objects = READ_IF_EXISTS(pSettings, r_u32, section, "Tele_Delay_Between_Objects_Time", 500);
	m_pmt_distance = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Distance", 50.f);
	m_pmt_object_height = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Object_Height", 10.f);
	m_pmt_time_object_keep = READ_IF_EXISTS(pSettings, r_u32, section, "Tele_Time_Object_Keep", 10000);
	m_pmt_raise_speed = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Raise_Speed", 3.f);
	m_pmt_raise_time_to_wait_in_objects = READ_IF_EXISTS(pSettings, r_u32, section,
	                                                     "Tele_Delay_Between_Objects_Raise_Time", 500);
	m_pmt_fly_velocity = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Fly_Velocity", 30.f);
	m_pmt_object_collision_damage = READ_IF_EXISTS(pSettings, r_float, section, "Tele_Collision_Damage", 0.5f);
	::Sound->create(m_sound_tele_hold, pSettings->r_string(section, "sound_tele_hold"), st_Effect, SOUND_TYPE_WORLD);
	::Sound->create(m_sound_tele_throw, pSettings->r_string(section, "sound_tele_throw"), st_Effect, SOUND_TYPE_WORLD);

	m_state = eWait;
	m_time = 0;
	m_time_next = 0;
}

void CPolterTele::update_frame()
{
	inherited::update_frame();
}

void CPolterTele::update_schedule()
{
	inherited::update_schedule();

	if (!m_object->g_Alive() || m_object->get_actor_ignore())
		return;

	Fvector const actor_pos = Actor()->Position();
	const float dist_to_actor_sqr = actor_pos.distance_to_sqr(m_object->Position());

	if (m_pmt_distance < 0.f || dist_to_actor_sqr > _sqr(m_pmt_distance))
		return;

	if (m_object->get_current_detection_level() < m_object->get_detection_success_level())
		return;

	switch (m_state)
	{
	case eStartRaiseObjects:
		if (m_time + m_time_next < time())
		{
			if (!tele_raise_objects(actor_pos))
				m_state = eRaisingObjects;

			m_time = time();
			m_time_next = m_pmt_raise_time_to_wait_in_objects / 2 + Random.randI(
				m_pmt_raise_time_to_wait_in_objects / 2);
		}

		if (m_state == eStartRaiseObjects)
		{
			if (m_object->CTelekinesis::get_objects_count() >= m_pmt_object_count)
			{
				m_state = eRaisingObjects;
				m_time = time();
			}
		}

		break;
	case eRaisingObjects:
		if (m_time + m_pmt_time_to_hold > time())
			break;

		m_time = time();
		m_time_next = 0;
		m_state = eFireObjects;
	case eFireObjects:
		if (m_time + m_time_next < time())
		{
			tele_fire_objects();

			m_time = time();
			m_time_next = m_pmt_time_to_wait_in_objects / 2 + Random.randI(m_pmt_time_to_wait_in_objects / 2);
		}

		if (m_object->CTelekinesis::get_objects_count() == 0)
		{
			m_state = eWait;
			m_time = time();
		}
		break;
	case eWait:
		if (m_time + m_pmt_time_to_wait < time())
		{
			m_time_next = 0;
			m_state = eStartRaiseObjects;
		}
		break;
	}
}

//////////////////////////////////////////////////////////////////////////
// Выбор подходящих объектов для телекинеза
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool CPolterTele::trace_object(CObject* obj, const Fvector& target)
{
	Fvector trace_from;
	obj->Center(trace_from);

	Fvector dir;
	float range;
	dir.sub(target, trace_from);

	range = dir.magnitude();
	if (range < 0.0001f)
		return false;

	dir.div(range);

	collide::rq_result l_rq;
	if (Level().ObjectSpace.RayPick(trace_from, dir, range, collide::rqtBoth, l_rq, obj))
	{
		if (l_rq.O == Actor()) return true;
	}

	return false;
}

extern BOOL g_telekinetic_objects_include_corpses;
void CPolterTele::tele_find_objects(const Fvector& pos, const Fvector& actor_center, const Fvector& actor_head)
{
	m_nearest.clear_not_free();
	Level().ObjectSpace.GetNearest(m_nearest, pos, m_pmt_radius, NULL);

	for (u32 i = 0; i < m_nearest.size(); i++)
	{
		CPhysicsShellHolder* obj = smart_cast<CPhysicsShellHolder *>(m_nearest[i]);
		CCustomMonster* custom_monster = smart_cast<CCustomMonster *>(m_nearest[i]);
		CInventoryItem* itm = smart_cast<CInventoryItem*>(m_nearest[i]);
		if (!obj ||
			!obj->PPhysicsShell() ||
			!obj->PPhysicsShell()->isActive() ||
			custom_monster && (!g_telekinetic_objects_include_corpses || custom_monster->g_Alive()) ||
			(obj->spawn_ini() && obj->spawn_ini()->section_exist("ph_heavy")) ||
			(obj->m_pPhysicsShell->getMass() < m_pmt_object_min_mass) ||
			(obj->m_pPhysicsShell->getMass() > m_pmt_object_max_mass) ||
			(obj == m_object) ||
			m_object->CTelekinesis::is_active_object(obj) ||
			!obj->m_pPhysicsShell->get_ApplyByGravity() ||
			(itm && itm->IsQuestItem()))
			continue;

		if (trace_object(obj, actor_center) || trace_object(obj, actor_head))
			m_tele_candidates.push_back(obj);
	}
}

bool CPolterTele::tele_raise_objects(const Fvector& actor_pos)
{
	m_tele_candidates.clear_not_free();
	Fvector actor_center;
	Actor()->Center(actor_center);
	const Fvector actor_head = get_head_position(Actor());

	// получить список объектов вокруг врага
	tele_find_objects(actor_pos, actor_center, actor_head);

	// получить список объектов вокруг монстра
	tele_find_objects(m_object->Position(), actor_center, actor_head);

	// получить список объектов между монстром и врагом
	Fvector pos;
	pos.add(m_object->Position(), actor_pos).mul(0.5f);
	tele_find_objects(pos, actor_center, actor_head);

	// выбрать ближайший к актёру объект без полной сортировки
	CObject* nearest_object = 0;
	float nearest_distance_sqr = flt_max;
	for (u32 i = 0; i < m_tele_candidates.size(); ++i)
	{
		const float distance_sqr = actor_pos.distance_to_sqr(m_tele_candidates[i]->Position());
		if (distance_sqr < nearest_distance_sqr)
		{
			nearest_distance_sqr = distance_sqr;
			nearest_object = m_tele_candidates[i];
		}
	}
	m_tele_candidates.clear_not_free();

	if (nearest_object)
	{
		CPhysicsShellHolder* obj = smart_cast<CPhysicsShellHolder *>(nearest_object);

		// применить телекинез на объект
		bool rotate = false;

		CTelekineticObject* tele_obj = m_object->CTelekinesis::activate(obj, m_pmt_raise_speed, m_pmt_object_height,
		                                                                m_pmt_time_object_keep, rotate);
		tele_obj->set_sound(m_sound_tele_hold, m_sound_tele_throw);

		return true;
	}

	return false;
}

struct SCollisionHitCallback :
	public ICollisionHitCallback

{
	//	CollisionHitCallbackFun				*m_collision_hit_callback																																						;
	CPoltergeist* m_object;
	float m_pmt_object_collision_damage;
    bool done = false;

	SCollisionHitCallback(CPoltergeist* obj, float pmt_object_collision_damage):
		m_object(obj), m_pmt_object_collision_damage(pmt_object_collision_damage)
	{
		VERIFY(obj);
	}

	void call(CObject*& obj, float min_cs, float max_cs, float& cs, float& hl, ICollisionDamageInfo* di) override
	{
        if (done) return;
        done = true;

        obj = nullptr;

        if (cs > min_cs * 0.5f)
        {
            hl = m_pmt_object_collision_damage;
            obj = m_object;
        }

		di->SetInitiated();
	}
};

void CPolterTele::tele_fire_objects()
{
	for (u32 i = 0; i < m_object->CTelekinesis::get_objects_total_count(); i++)
	{
		CTelekineticObject tele_object = m_object->CTelekinesis::get_object_by_index(i);
		//if (tele_object.get_state() != TS_Fire) {
		if ((tele_object.get_state() == TS_Raise) || (tele_object.get_state() == TS_Keep))
		{
			CPhysicsShellHolder* hobj = tele_object.get_object();
			VERIFY(hobj);

            const Fvector enemy_pos = get_head_position(Actor());
            const float fire_time = hobj->Position().distance_to(enemy_pos) / m_pmt_fly_velocity;
			hobj->set_collision_hit_callback(xr_new<SCollisionHitCallback>(m_object, m_pmt_object_collision_damage));
			m_object->CTelekinesis::fire_t(hobj, enemy_pos, fire_time);
			return;
		}
	}
}
