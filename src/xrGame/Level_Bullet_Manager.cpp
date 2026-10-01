// Level_Bullet_Manager.cpp:	для обеспечения полета пули по траектории
//								все пули и осколки передаются сюда
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Level.h"
#include "Level_Bullet_Manager.h"
#include "game_cl_base.h"
#include "Actor.h"
#include "gamepersistent.h"
#include "mt_config.h"
#include "game_cl_base_weapon_usage_statistic.h"
#include "game_cl_mp.h"
#include "reward_event_generator.h"
#include "material_manager.h"
#include "Weapon.h"
#include "script_engine.h"
#include "../xrCore/job_system.h"
#include "../xrCDB/ISpatial.h"

#include "../Include/xrRender/UIRender.h"
#include "../Include/xrRender/Kinematics.h"

#ifdef DEBUG
#	include "debug_renderer.h"
#endif

#define HIT_POWER_EPSILON 0.05f
#define WALLMARK_SIZE 0.04f

float CBulletManager::m_fMinBulletSpeed = 2.f;
float const CBulletManager::parent_ignore_distance = 3.f;

#ifdef DEBUG
	float air_resistance_epsilon					= .1f;
#else // #ifdef DEBUG
static float const air_resistance_epsilon = .1f;
#endif // #ifdef DEBUG
float g_bullet_time_factor = 1.f;

namespace
{
const ::luabind::functor<void>* bullet_init_callback()
{
	static cached_script_functor<void> callback_cache("_G.CBulletOnInit");
	return callback_cache.get(ai().script_engine());
}

const ::luabind::functor<void>* bullet_update_callback()
{
	static cached_script_functor<void> callback_cache("_G.CBulletOnUpdate");
	return callback_cache.get(ai().script_engine());
}

const ::luabind::functor<void>* bullet_impact_callback()
{
	static cached_script_functor<void> callback_cache("_G.CBulletOnImpact");
	return callback_cache.get(ai().script_engine());
}

const ::luabind::functor<void>* bullet_remove_callback()
{
	static cached_script_functor<void> callback_cache("_G.CBulletOnRemove");
	return callback_cache.get(ai().script_engine());
}
}

SBullet::SBullet()
{
}

SBullet::~SBullet()
{
}

void CBulletManager::SpeculativeBulletState::capture(const SBullet& bullet)
{
	bullet_pos = bullet.bullet_pos;
	dir = bullet.dir;
	tracer_start_position = bullet.tracer_start_position;
	start_position = bullet.start_position;
	start_velocity = bullet.start_velocity;
	speed = bullet.speed;
	fly_dist = bullet.fly_dist;
	life_time = bullet.life_time;
	max_dist = bullet.max_dist;
	change_rajectory_count = bullet.change_rajectory_count;
	parent_id = bullet.parent_id;
	ricochet_was = bullet.flags.ricochet_was;
}

void CBulletManager::SpeculativeBulletState::commit(SBullet& bullet) const
{
	bullet.bullet_pos = bullet_pos;
	bullet.dir = dir;
	bullet.tracer_start_position = tracer_start_position;
	bullet.speed = speed;
	bullet.fly_dist = fly_dist;
	bullet.life_time = life_time;
	bullet.change_rajectory_count = change_rajectory_count;
	bullet.flags.ricochet_was = ricochet_was;
}

BOOL g_allow_silencer_hide_tracer = 0;
u32 SBullet::bulletCount = 0;

void SBullet::Init(const Fvector& position,
                   const Fvector& direction,
                   float starting_speed,
                   float power,
                   //.				   float power_critical,
                   float impulse,
                   u16 sender_id,
                   u16 sendersweapon_id,
                   ALife::EHitType e_hit_type,
                   float maximum_distance,
                   const CCartridge& cartridge,
                   float const air_resistance_factor,
                   bool SendHit, int iShotNum)
{
	section = *cartridge.m_ammoSect;
	flags._storage = 0;
	bullet_pos = position;
	starting_speed *= cartridge.param_s.kBulletSpeed;
	speed = max_speed = starting_speed;
	VERIFY(speed > 0.f);

	start_position = position;
	start_velocity.mul(direction, starting_speed);
	born_time = Device.dwTimeGlobal;
	life_time = 0.f;

	VERIFY(direction.magnitude() > 0.f);
	dir.normalize(direction);

	hit_param.power = power * cartridge.param_s.kHit;
	hit_param.impulse = impulse * cartridge.param_s.kImpulse;

	max_dist = maximum_distance * cartridge.param_s.kDist;
	fly_dist = 0;
	tracer_start_position = bullet_pos;

	parent_id = sender_id;
	flags.allow_sendhit = SendHit;
	weapon_id = sendersweapon_id;
	hit_type = e_hit_type;

	armor_piercing = cartridge.param_s.kAP;
	air_resistance = cartridge.param_s.kAirRes * air_resistance_factor;
	wallmark_size = cartridge.param_s.fWallmarkSize;
	m_u8ColorID = cartridge.param_s.u8ColorID;

	bullet_material_idx = cartridge.bullet_material_idx;
	VERIFY(u16(-1) != bullet_material_idx);

	flags.allow_tracer = !!cartridge.m_flags.test(CCartridge::cfTracer);

	//Alundaio: Tracer for every 5th bullet
	if (flags.allow_tracer && cartridge.m_4to1_tracer && iShotNum % 5 != 0)
		flags.allow_tracer = false;
	//-Alundaio

	// demonized: hide tracer when weapon has silencer
	if (!cartridge.param_s.tracer_silenced && g_allow_silencer_hide_tracer)		// momopate: Make it optional
	{
		CObject* g_obj = Level().Objects.net_Find(weapon_id);
		if (g_obj) 
		{
			CWeapon* weapon = smart_cast<CWeapon*>(g_obj);
			if (weapon && weapon->IsSilencerAttached())
				flags.allow_tracer = (weapon->GetSilencedTracers());	// momopate: customize per weapon
		}
	}

	flags.allow_ricochet = !!cartridge.m_flags.test(CCartridge::cfRicochet);
	flags.explosive = !!cartridge.m_flags.test(CCartridge::cfExplosive);
	flags.magnetic_beam = !!cartridge.m_flags.test(CCartridge::cfMagneticBeam);
	//	flags.skipped_frame		= 0;

	init_frame_num = Device.dwFrame;

	targetID = 0;
	density_mode = 0;

	catridgeSection = cartridge.m_ammoSect.c_str();
	m_dwID = bulletCount++;
	bulletId = m_dwID;
}

// demonized: construct bullet table for Lua
void populateBulletTable (
	::luabind::object& table,
	const Fvector& position,
	const Fvector& direction,
	float speed,
	float fly_dist,
	LPCSTR catridgeSection,
	u32 bulletId,
	u16 weaponId,
	u16 parentId,
	u16 targetId,
	LPCSTR materialName,
	float life_time,
	int element
) {
	table["position"] = position;
	table["direction"] = direction;
	table["speed"] = speed;
	table["distance"] = fly_dist;
	table["section"] = catridgeSection;
	table["bullet_id"] = bulletId;
	table["weapon_id"] = weaponId;
	table["parent_id"] = parentId;
	table["target_id"] = targetId;
	table["material"] = materialName;
	table["life_time"] = life_time;
	table["element"] = element;
}


CBulletManager::CBulletManager()
#if 0//def PROFILE_CRITICAL_SECTIONS
	: m_Lock(MUTEX_PROFILE_ID(CBulletManager))
#	ifdef DEBUG
		,m_thread_id(GetCurrentThreadId())
#	endif // #ifdef DEBUG
#else // #ifdef PROFILE_CRITICAL_SECTIONS
#	ifdef DEBUG
		: m_thread_id(GetCurrentThreadId())
#	endif // #ifdef DEBUG
#endif // #ifdef PROFILE_CRITICAL_SECTIONS
{
	m_Bullets.clear();
	m_Bullets.reserve(100);
	m_BulletsRendered.reserve(100);
	m_SpeculativeResults.reserve(100);
	m_Events.reserve(128);
}

CBulletManager::~CBulletManager()
{
	m_Bullets.clear();
	m_WhineSounds.clear();
	m_Events.clear();
}

void CBulletManager::Load()
{
	char const* bullet_manager_sect = "bullet_manager";
	if (!IsGameTypeSingle())
	{
		bullet_manager_sect = "mp_bullet_manager";
	}
	m_fTracerWidth = pSettings->r_float(bullet_manager_sect, "tracer_width");
	m_fTracerLengthMax = pSettings->r_float(bullet_manager_sect, "tracer_length_max");
	m_fTracerLengthMin = pSettings->r_float(bullet_manager_sect, "tracer_length_min");

	m_fGravityConst = pSettings->r_float(bullet_manager_sect, "gravity_const");
	m_fAirResistanceK = pSettings->r_float(bullet_manager_sect, "air_resistance_k");

	m_fMinBulletSpeed = pSettings->r_float(bullet_manager_sect, "min_bullet_speed");
	m_fCollisionEnergyMin = pSettings->r_float(bullet_manager_sect, "collision_energy_min");
	m_fCollisionEnergyMax = pSettings->r_float(bullet_manager_sect, "collision_energy_max");

	m_fHPMaxDist = pSettings->r_float(bullet_manager_sect, "hit_probability_max_dist");

	if (pSettings->line_exist(bullet_manager_sect, "bullet_velocity_time_factor"))
	{
		g_bullet_time_factor = pSettings->r_float(bullet_manager_sect, "bullet_velocity_time_factor");
	}


	LPCSTR whine_sounds = pSettings->r_string(bullet_manager_sect, "whine_sounds");
	int cnt = _GetItemCount(whine_sounds);
	xr_string tmp;
	for (int k = 0; k < cnt; ++k)
	{
		m_WhineSounds.push_back(ref_sound());
		m_WhineSounds.back().create(_GetItem(whine_sounds, k, tmp), st_Effect, sg_SourceType);
	}

	LPCSTR explode_particles = pSettings->r_string(bullet_manager_sect, "explode_particles");
	cnt = _GetItemCount(explode_particles);
	for (int k = 0; k < cnt; ++k)
		m_ExplodeParticles.push_back(_GetItem(explode_particles, k, tmp));
}

void CBulletManager::PlayExplodePS(const Fmatrix& xf)
{
	if (m_ExplodeParticles.empty())
		return;

	shared_str const& ps_name = m_ExplodeParticles[Random.randI(0, m_ExplodeParticles.size())];
	CParticlesObject* const ps = CParticlesObject::Create(*ps_name,TRUE);
	ps->UpdateParent(xf, zero_vel);
	GamePersistent().ps_needtoplay.push_back(ps);
}

void CBulletManager::PlayWhineSound(SBullet* bullet, CObject* object, const Fvector& pos)
{
	if (m_WhineSounds.empty()) return;
	if (bullet->m_whine_snd._feedback() != NULL) return;
	if (bullet->hit_type != ALife::eHitTypeFireWound) return;

	bullet->m_whine_snd = m_WhineSounds[Random.randI(0, m_WhineSounds.size())];
	bullet->m_whine_snd.play_at_pos(object, pos);
}

void CBulletManager::Clear()
{
	m_Bullets.clear();
	m_BulletsRendered.clear_not_free();
	m_SpeculativeResults.clear_not_free();
	m_SpeculativeCooldown = 0;
	m_SpeculativeFallbackEma = 0.f;
	m_SpeculativeHistoryValid = false;
	m_Events.clear();
}

void CBulletManager::AddBullet(const Fvector& position,
                               const Fvector& direction,
                               float starting_speed,
                               float power,
                               //.							   float power_critical,
                               float impulse,
                               u16 sender_id,
                               u16 sendersweapon_id,
                               ALife::EHitType e_hit_type,
                               float maximum_distance,
                               const CCartridge& cartridge,
                               float const air_resistance_factor,
                               bool SendHit,
                               bool AimBullet,
                               int iShotNum)
{
#ifdef DEBUG
	VERIFY(m_thread_id == GetCurrentThreadId());
#endif

	VERIFY(u16(-1)!=cartridge.bullet_material_idx);
	//	u32 CurID					= Level().CurrentControlEntity()->ID();
	//	u32 OwnerID					= sender_id;
	m_Bullets.push_back(SBullet());
	SBullet& bullet = m_Bullets.back();
	bullet.Init(position, direction, starting_speed, power, /*power_critical,*/ impulse, sender_id, sendersweapon_id,
	            e_hit_type, maximum_distance, cartridge, air_resistance_factor, SendHit, iShotNum);
	//	bullet.frame_num			= Device.dwFrame;
	bullet.flags.aim_bullet = AimBullet;

	// demonized - bullet on init callback
	if (const ::luabind::functor<void>* funct = bullet_init_callback()) {
		::luabind::object table = ::luabind::newtable(ai().script_engine().lua());
		populateBulletTable(
			table,
			position,
			direction,
			starting_speed * cartridge.param_s.kBulletSpeed,
			0,
			bullet.catridgeSection,
			bullet.bulletId,
			bullet.weapon_id,
			bullet.parent_id,
			65535,
			NULL,
			bullet.life_time,
			-1
		);
		(*funct)(table);
	}

	if (!IsGameTypeSingle())
	{
		if (SendHit)
			Game().m_WeaponUsageStatistic->OnBullet_Fire(&bullet, cartridge);
		game_cl_mp* tmp_cl_game = smart_cast<game_cl_mp*>(&Game());
		if (tmp_cl_game->get_reward_generator())
			tmp_cl_game->get_reward_generator()->OnBullet_Fire(sender_id, sendersweapon_id, position, direction);
	}
}

u32 CBulletManager::speculative_minimum_bullet_count() const
{
	const u32 workers = _max(1u, xr_jobs::worker_count());
	u32 threshold = _min(32u, _max(12u, workers * 2u));

	if (!m_SpeculativeHistoryValid)
		return threshold;

	// Proven clear-flight workloads can enter the prepass earlier. Collision-heavy
	// batches are controlled by cooldown rather than a raised threshold that could
	// permanently prevent smaller future workloads from being sampled again.
	if (m_SpeculativeFallbackEma <= .25f)
		threshold = _max(8u, threshold - 4u);

	return threshold;
}

bool CBulletManager::can_use_speculative_parallel_path(bool update_script_hook_present,
	u32 minimum_bullet_count) const
{
#ifdef DEBUG
	return false;
#else
	if (!g_mt_config.test(mtBullets) || !IsGameTypeSingle() || xr_jobs::worker_count() == 0 ||
		m_Bullets.size() < minimum_bullet_count || update_script_hook_present)
		return false;

	return true;
#endif
}

namespace
{
class spatial_batch_read_guard
{
public:
	explicit spatial_batch_read_guard(ISpatial_DB* database) : db(database)
	{
		if (db)
			db->acquire_read();
	}

	~spatial_batch_read_guard()
	{
		if (db)
			db->release_read();
	}

	spatial_batch_read_guard(const spatial_batch_read_guard&) = delete;
	spatial_batch_read_guard& operator=(const spatial_batch_read_guard&) = delete;

private:
	ISpatial_DB* db;
};
}

void CBulletManager::speculative_worker(void* context)
{
	SpeculativeWorkload& workload = *static_cast<SpeculativeWorkload*>(context);
	for (;;)
	{
		const LONG claimed = InterlockedExchangeAdd(&workload.next_index, static_cast<LONG>(workload.chunk_size));
		if (claimed < 0 || static_cast<u32>(claimed) >= workload.bullet_count)
			return;

		const u32 begin = static_cast<u32>(claimed);
		const u32 end = _min(begin + workload.chunk_size, workload.bullet_count);
		// Hold one shared spatial lock for the claimed block instead of taking and
		// releasing the same SRW lock for every ray segment of every bullet.
		spatial_batch_read_guard spatial_guard(g_SpatialSpace);
		for (u32 index = begin; index < end; ++index)
		{
			SpeculativeResult& result = (*workload.results)[index];
			result.status = workload.manager->process_bullet_speculative(result.bullet, workload.delta_time);
		}
	}
}

void CBulletManager::UpdateWorkload()
{
	//	VERIFY						( m_thread_id == GetCurrentThreadId() );

	rq_storage.r_clear();

	const u32 time_delta = u32(Device.dwTimeDelta * g_bullet_time_factor);
	if (!time_delta || m_Bullets.empty())
		return;

	// The per-update hook can mutate every clear-flight bullet, so its presence
	// requires the exact legacy serial path. ObjectHit is different: every possible
	// collision already falls back before that callback can run.
	const bool has_update_hook = bullet_update_callback() != nullptr;

	collide::rq_result dummy;
	const u32 bullet_count = m_Bullets.size();
	const u32 minimum_bullet_count = speculative_minimum_bullet_count();
	const bool cooldown_active = m_SpeculativeCooldown != 0;
	if (cooldown_active)
		--m_SpeculativeCooldown;

	if (cooldown_active || !can_use_speculative_parallel_path(has_update_hook, minimum_bullet_count))
	{
		for (u32 index = bullet_count; index > 0; --index)
		{
			SBullet& bullet = m_Bullets[index - 1];
			// Preserve the legacy callback lookup semantics on the complete serial path.
			if (process_bullet(rq_storage, bullet, time_delta, true))
				continue;

			R_ASSERT(index - 1 <= u16(-1));
			RegisterEvent(EVENT_REMOVE, FALSE, &bullet, Fvector().set(0, 0, 0), dummy, u16(index - 1));
		}
		return;
	}

	// Capture only the ballistic hot state. Sound handles, script strings, hit data
	// and other cold SBullet fields remain in the owning live vector.
	m_SpeculativeResults.resize(bullet_count);
	for (u32 index = 0; index < bullet_count; ++index)
		m_SpeculativeResults[index].bullet.capture(m_Bullets[index]);

	SpeculativeWorkload workload;
	workload.manager = this;
	workload.results = &m_SpeculativeResults;
	workload.bullet_count = bullet_count;
	const u32 lane_capacity = _max(1u, xr_jobs::worker_count());
	const u32 target_chunks = _max(1u, lane_capacity * 4u);
	const u32 estimated_chunk = (bullet_count + target_chunks - 1) / target_chunks;
	workload.chunk_size = _min(16u, _max(4u, estimated_chunk));
	workload.delta_time = time_delta;

	xr_jobs::task_group group;
	const u32 requested_lanes = (bullet_count + workload.chunk_size - 1) / workload.chunk_size;
	// UpdateWorkload itself already occupies one worker-pool lane. Count that lane
	// instead of enqueueing one more task than the pool can execute concurrently.
	const u32 lane_count = _min(xr_jobs::available_thread_count(), requested_lanes);
	xr_jobs::submit_many(&CBulletManager::speculative_worker, &workload,
		lane_count - 1, &group, xr_jobs::priority::high);

	// The current seqParallel lane participates instead of idling in wait().
	speculative_worker(&workload);
	xr_jobs::wait(group);

	// Commit in the original reverse order. Any static hit or non-owner dynamic
	// broadphase candidate is rerun through the unchanged serial narrowphase,
	// preserving RNG, sound, Lua and event ordering.
	u32 fallback_count = 0;
	for (u32 index = bullet_count; index > 0; --index)
	{
		const u32 bullet_index = index - 1;
		SpeculativeResult& result = m_SpeculativeResults[bullet_index];

		if (result.status == SpeculativeStatus::Fallback)
		{
			++fallback_count;
			SBullet& bullet = m_Bullets[bullet_index];
			// Fallback is the original path, including per-substep Lua functor lookup.
			if (process_bullet(rq_storage, bullet, time_delta, true))
				continue;

			R_ASSERT(bullet_index <= u16(-1));
			RegisterEvent(EVENT_REMOVE, FALSE, &bullet, Fvector().set(0, 0, 0), dummy, u16(bullet_index));
			continue;
		}

		result.bullet.commit(m_Bullets[bullet_index]);
		if (result.status == SpeculativeStatus::Remove)
		{
			R_ASSERT(bullet_index <= u16(-1));
			RegisterEvent(EVENT_REMOVE, FALSE, &m_Bullets[bullet_index], Fvector().set(0, 0, 0), dummy,
			              u16(bullet_index));
		}
	}

	const float fallback_ratio = bullet_count ? float(fallback_count) / float(bullet_count) : 0.f;
	if (m_SpeculativeHistoryValid)
		m_SpeculativeFallbackEma = m_SpeculativeFallbackEma * .75f + fallback_ratio * .25f;
	else
	{
		m_SpeculativeFallbackEma = fallback_ratio;
		m_SpeculativeHistoryValid = true;
	}

	// Scale back more aggressively only when the measured batch was dominated by
	// fallback. Moderately collision-heavy frames skip one prepass; clear-flight
	// workloads continue without an artificial fixed delay.
	if (fallback_ratio >= .75f)
		m_SpeculativeCooldown = 3;
	else if (fallback_ratio >= .50f)
		m_SpeculativeCooldown = 1;

	m_SpeculativeResults.clear_not_free();
}

static Fvector parabolic_velocity(
	Fvector const& start_velocity,
	Fvector const& gravity,
	float const air_resistance,
	float const time
)
{
	return (
		Fvector(start_velocity).mul(
			_max(0.f, 1.f - air_resistance * time)
		).mad(
			gravity,
			time
		)
	);
}

static Fvector trajectory_velocity(
	Fvector const& start_velocity,
	Fvector const& gravity,
	float const air_resistance,
	float const time
)
{
	float const parabolic_time = _max(0.f, 2.f / air_resistance - air_resistance_epsilon);
	float const fall_down_time = time - parabolic_time;
	//	float const fake_velocity	= start_velocity*2.f;
	if (fall_down_time < 0.f)
	{
		Fvector const xz_velocity = Fvector().set(start_velocity.x, 0.f, start_velocity.z);
		// this could be since we could fire in different directions
		// for example, vertically into the ground
		if (!fis_zero(xz_velocity.square_magnitude()))
		{
			return (
				parabolic_velocity(
					start_velocity,
					gravity,
					air_resistance,
					time
				)
			);
		}

		// this fake since our formula doesn't take into account
		// directions correctly
		return (
			Fvector(start_velocity).mad(
				gravity,
				time
			)
		);
	}

	Fvector parabolic_velocity =
		::parabolic_velocity(
			start_velocity,
			gravity,
			air_resistance,
			parabolic_time
		);

	VERIFY(
		!fis_zero(air_resistance_epsilon) || fis_zero(_sqr(parabolic_velocity.x) + _sqr(parabolic_velocity.z), EPS_L));
	return (
		parabolic_velocity.mad(
			gravity,
			fall_down_time
		)
	);
}

static Fvector parabolic_position(
	Fvector const& start_position,
	Fvector const& start_velocity,
	Fvector const& gravity,
	float const air_resistance,
	float const time
)
{
	float const sqr_t_div_2 = _sqr(time) * .5f;
	return (
		Fvector().mad(
			start_position,
			start_velocity,
			time
		).mad(
			Fvector(start_velocity).mul(-air_resistance),
			sqr_t_div_2
		).mad(
			gravity,
			sqr_t_div_2
		)
	);
}

//BOOL g_use_new_ballistics	= 0;
#ifdef DEBUG
float dbg_bullet_time_factor = 1.f;
#endif

static Fvector trajectory_position(
	Fvector const& start_position,
	Fvector const& base_start_velocity,
	Fvector const& base_gravity,
	float base_air_resistance,
	float const base_time
)
{
	Fvector const& gravity = base_gravity;
	//g_use_new_ballistics ? Fvector(base_gravity).mul(_sqr(factor)) : base_gravity;
	float const& air_resistance = base_air_resistance;
	//g_use_new_ballistics ? base_air_resistance*factor : base_air_resistance;
	Fvector const& start_velocity = base_start_velocity;
	//g_use_new_ballistics ? Fvector(base_start_velocity).mul( factor ) : base_start_velocity;
	float const time = base_time;

	float const parabolic_time = _max(0.f, 1.f / air_resistance - air_resistance_epsilon);
	float const fall_down_time = time - parabolic_time;
	if (fall_down_time < 0.f)
	{
		Fvector const xz_velocity = Fvector().set(start_velocity.x, 0.f, start_velocity.z);
		if (!fis_zero(xz_velocity.square_magnitude()))
			return (
				parabolic_position(
					start_position,
					start_velocity,
					gravity,
					air_resistance,
					time
				)
			);

		return (
			Fvector(start_position).mad(
				start_velocity,
				time
			).mad(
				gravity,
				_sqr(time) * .5f
			)
		);
	}

	Fvector const parabolic_position =
		::parabolic_position(
			start_position,
			start_velocity,
			gravity,
			air_resistance,
			parabolic_time
		);
	Fvector const parabolic_velocity =
		::parabolic_velocity(
			start_velocity,
			gravity,
			air_resistance,
			parabolic_time
		);
	return (
		Fvector(parabolic_position).mad(
			parabolic_velocity,
			fall_down_time
		).mad(
			gravity,
			_sqr(fall_down_time) * .5f
		)
	);
}

inline static float trajectory_max_error_time(
	float const t0,
	float const t1
)
{
	return ((t1 + t0) * .5f);
	// this is correct even in our case
	// y(t) = V0y*t - V0y*ar*t^2/2 - g*t^2/2
	// x(t) = V0x*t - V0x*ar*t^2/2
}

static float trajectory_pick_error(
	float const low,
	float const high,
	Fvector const& start,
	Fvector const& position,
	Fvector const& velocity,
	Fvector const& gravity,
	float const air_resistance
)
{
	float max_error_time = trajectory_max_error_time(low, high);

	Fvector const target = trajectory_position(position, velocity, gravity, air_resistance, high);
	Fvector const max_error = trajectory_position(position, velocity, gravity, air_resistance, max_error_time);

	Fvector start_to_max_error = Fvector().sub(max_error, start);
	float magnitude = start_to_max_error.magnitude();
	start_to_max_error.mul(1.f / magnitude);
	Fvector start_to_target = Fvector().sub(target, start).normalize();
	float cosine_alpha = _max(-1.f, _min(start_to_max_error.dotproduct(start_to_target), 1.f));
	float sine_alpha = _sqrt(1.f - _sqr(cosine_alpha));
	return (magnitude * sine_alpha);
}

template <typename TBullet>
static float trajectory_select_pick_gravity(
	TBullet& bullet,
	float start_low,
	float const high,
	Fvector const& gravity,
	float const air_resistance
)
{
	float const max_test_distance = bullet.max_dist - bullet.fly_dist;
	float const time_delta = high - start_low;
	float const time_to_fly = Fvector(bullet.start_velocity)
	                          .mul(time_delta).mad(gravity, _sqr(time_delta) * .5f).magnitude();
	if (time_to_fly <= max_test_distance)
		return (high);

	float const fall_down_velocity_magnitude = bullet.speed;
	float const positive_gravity = -gravity.y;
	float time = (
		_sqrt(_sqr(fall_down_velocity_magnitude) + 2.f * max_test_distance * positive_gravity) -
		fall_down_velocity_magnitude
	) / positive_gravity;
	VERIFY(time >= 0.f);

	VERIFY(high >= start_low);
	float result = start_low + time;
	clamp(result, start_low, high);
	VERIFY2(result <= high,
	        make_string("result[%f], high[%f], start_low[%f], air_resistance[%f]", result, high, start_low,
		        air_resistance));
	return (result);
}

template <typename TBullet>
static float trajectory_select_pick_parabolic(
	TBullet& bullet,
	float const start_low,
	float high,
	Fvector const& gravity,
	float const air_resistance
)
{
	float const max_test_distance = bullet.max_dist - bullet.fly_dist;
	Fvector const start = trajectory_position(bullet.start_position, bullet.start_velocity, gravity, air_resistance,
	                                          start_low);
	float const start_high = high;
	float low = start_low;
	float check_time = high;
	while (!fsimilar(low, high))
	{
		Fvector const intermediate = trajectory_position(bullet.start_position, bullet.start_velocity, gravity,
		                                                 air_resistance, start_low + (check_time - start_low) * .5f);
		Fvector const target = trajectory_position(bullet.start_position, bullet.start_velocity, gravity,
		                                           air_resistance, check_time);
		float const distance = start.distance_to(intermediate) + intermediate.distance_to(target);
		if (distance < max_test_distance)
			low = check_time;
		else
			high = check_time;

		check_time = (low + high) * .5f;
	}

	VERIFY(low <= start_high);
	return (low);
}

template <typename TBullet>
static bool trajectory_select_pick_ranges(
	float& result,
	TBullet& bullet,
	float const low,
	float const high,
	Fvector const& gravity,
	float const air_resistance
)
{
	float const max_test_distance = bullet.max_dist - bullet.fly_dist;
	VERIFY(max_test_distance > 0.f);

	if (air_resistance * (low + air_resistance_epsilon) >= 1.f)
	{
		result = trajectory_select_pick_gravity(bullet, low, high, gravity, air_resistance);
		return (true);
	}

	if (air_resistance * (high + air_resistance_epsilon) < 1.f)
	{
		result = trajectory_select_pick_parabolic(bullet, low, high, gravity, air_resistance);
		return (false);
	}

	float const fall_down_time = _max(0.f, 1.f / air_resistance - air_resistance_epsilon);
	if (!fsimilar(fall_down_time, low))
	{
		result = trajectory_select_pick_parabolic(bullet, low, fall_down_time, gravity, air_resistance);
		return (false);
	}

	result = trajectory_select_pick_gravity(bullet, fall_down_time, high, gravity, air_resistance);
	return (false);
}

template <typename TBullet>
static float trajectory_select_pick_time(
	TBullet& bullet,
	float const start_low,
	float high,
	Fvector const& gravity,
	float const air_resistance
)
{
	VERIFY2(start_low < high, make_string("start_low[%f] high[%f]", start_low, high));
	float const start_high = high;
	if (trajectory_select_pick_ranges(high, bullet, start_low, high, gravity, air_resistance))
	{
		if (high <= start_high)
			return (high);

		return (start_high);
	}

	float low = start_low;
	float check_time = high;
	float const epsilon = .1f;
	if (!fsimilar(low, high))
	{
		// The start point is fixed throughout the binary search. Recomputing its
		// trajectory on every iteration duplicates the same ballistic work.
		Fvector const start = trajectory_position(bullet.start_position, bullet.start_velocity, gravity,
			air_resistance, start_low);
		do
		{
			float distance = trajectory_pick_error(start_low, check_time, start, bullet.start_position,
				bullet.start_velocity, gravity, air_resistance);

			if (distance < epsilon)
				low = check_time;
			else
				high = check_time;

			check_time = (low + high) * .5f;
		} while (!fsimilar(low, high));
	}

	VERIFY2(low <= start_high, make_string("low[%f], high[%f]", low, start_high));
	return (low);
}

void CBulletManager::add_bullet_point(
	Fvector const& start_position,
	Fvector& previous_position,
	Fvector const& start_velocity,
	Fvector const& gravity,
	float const air_resistance,
	float const current_time
)
{
#ifdef DEBUG
	Fvector	const temp			= trajectory_position(start_position, start_velocity, gravity, air_resistance, current_time);
	m_bullet_points.push_back	(previous_position);
	m_bullet_points.push_back	(temp);
	previous_position			= temp;
#endif // #ifdef DEBUG
}

static void update_bullet_parabolic(
	SBullet& bullet,
	bullet_test_callback_data& data,
	Fvector const& gravity,
	float const air_resistance
)
{
	Fvector xz_projection = Fvector(data.collide_position).sub(bullet.start_position);
	xz_projection.y = 0;
	float const xz_range = xz_projection.magnitude();
	Fvector const xz_velocity = Fvector().set(bullet.start_velocity.x, 0.f, bullet.start_velocity.z);

	VERIFY(air_resistance >= 0.f);
	if (air_resistance > 0.f)
	{
		float value = 2 * air_resistance * xz_range / xz_velocity.magnitude();
		clamp(value, 0.f, 1.f);
		VERIFY(value <= 1.f);
		VERIFY(value >= 0.f);
		data.collide_time = (1.f - _sqrt(1.f - value)) / air_resistance;
	}
	else
		data.collide_time = xz_range / xz_velocity.magnitude();

	VERIFY(data.collide_time >= 0.f);

	//	VERIFY						(data.collide_time <= data.high_time);
	//	VERIFY						(data.collide_time >= bullet.life_time);
	//	VERIFY						(data.collide_time <= bullet.life_time + Device.fTimeGlobal);
	clamp(data.collide_time, bullet.life_time, data.high_time);

	data.collide_position = trajectory_position(bullet.start_position, bullet.start_velocity, gravity, air_resistance,
	                                            data.collide_time);
	Fvector const new_velocity = trajectory_velocity(bullet.start_velocity, gravity, air_resistance, data.collide_time);
	bullet.speed = new_velocity.magnitude();
	bullet.dir = Fvector(new_velocity).normalize_safe();
}

static void update_bullet_gravitation(
	SBullet& bullet,
	bullet_test_callback_data& data,
	Fvector const& gravity,
	float const air_resistance,
	float const fall_down_time
)
{
	Fvector const fall_down_position = trajectory_position(bullet.start_position, bullet.start_velocity, gravity,
	                                                       air_resistance, fall_down_time);
	Fvector const fall_down_velocity = trajectory_velocity(bullet.start_velocity, gravity, air_resistance,
	                                                       fall_down_time);
	VERIFY(
		!fis_zero(air_resistance_epsilon) || fis_zero( _sqr(fall_down_velocity.x) + _sqr(fall_down_velocity.z), EPS_L));
	float const fall_down_velocity_magnitude = fall_down_velocity.magnitude();

	Fvector xz_projection = Fvector(data.collide_position).sub(fall_down_position);
	xz_projection.y = 0;
	float const xz_range = xz_projection.magnitude();
	Fvector const xz_velocity = Fvector().set(fall_down_velocity.x, 0.f, fall_down_velocity.z);

	if (!fis_zero(xz_velocity.magnitude()))
	{
		data.collide_time = fall_down_time + xz_range / xz_velocity.magnitude();
		VERIFY(data.collide_time >= 0.f);

		//		VERIFY					(data.collide_time <= data.high_time);
		//		VERIFY					(data.collide_time >= bullet.life_time);
		//		VERIFY					(data.collide_time <= bullet.life_time + Device.fTimeGlobal);
		clamp(data.collide_time, bullet.life_time, data.high_time);
	}
	else
	{
		float const positive_gravity = -gravity.y;
		float const distance = fall_down_position.distance_to(data.collide_position);
		data.collide_time = fall_down_time +
		(
			_sqrt(_sqr(fall_down_velocity_magnitude) + 2.f * distance * positive_gravity) -
			fall_down_velocity_magnitude
		) / positive_gravity;
		VERIFY(data.collide_time >= 0.f);

		//		VERIFY					(data.collide_time <= data.high_time);
		//		VERIFY					(data.collide_time >= bullet.life_time);
		//		VERIFY					(data.collide_time <= bullet.life_time + Device.fTimeGlobal);
		clamp(data.collide_time, bullet.life_time, data.high_time);
	}

	Fvector const new_velocity = trajectory_velocity(bullet.start_velocity, gravity, air_resistance, data.collide_time);
	bullet.speed = new_velocity.magnitude();
	bullet.dir = Fvector(new_velocity).normalize_safe();
}

static void update_bullet(
	SBullet& bullet,
	bullet_test_callback_data& data,
	Fvector const& gravity,
	float const air_resistance
)
{
	if (air_resistance * (bullet.life_time + air_resistance_epsilon) >= 1.f)
	{
		update_bullet_gravitation(bullet, data, gravity, air_resistance,
		                          _max(0.f, 1.f / air_resistance - air_resistance_epsilon));
		return;
	}

	Fvector const xz_velocity = Fvector().set(bullet.start_velocity.x, 0.f, bullet.start_velocity.z);
	if (fis_zero(xz_velocity.square_magnitude()))
	{
		update_bullet_gravitation(bullet, data, gravity, air_resistance, 0.f);
		return;
	}

	update_bullet_parabolic(bullet, data, gravity, air_resistance);
}

BOOL CBulletManager::firetrace_callback(collide::rq_result& result, LPVOID params)
{
	bullet_test_callback_data& data = *(bullet_test_callback_data*)params;
	SBullet& bullet = *data.pBullet;

	Fvector& collide_position = data.collide_position;
	collide_position = Fvector().mad(bullet.bullet_pos, bullet.dir, result.range);

	float const air_resistance = (GameID() == eGameIDSingle)
		                             ? Level().BulletManager().m_fAirResistanceK
		                             : bullet.air_resistance;

	CBulletManager& bullet_manager = Level().BulletManager();
	Fvector const gravity = {0.f, -bullet_manager.m_fGravityConst, 0.f};
	update_bullet(bullet, data, gravity, air_resistance);
	if (fis_zero(bullet.speed))
		return (FALSE);

	if (fis_zero(data.collide_time))
		return (TRUE);

	//статический объект
	if (!result.O)
	{
		CDB::TRI const& triangle = *(Level().ObjectSpace.GetStaticTris() + result.element);
		bullet_manager.RegisterEvent(EVENT_HIT, FALSE, &bullet, collide_position, result, triangle.material);
		return (FALSE);
	}

	//динамический объект
	VERIFY(!(result.O->ID() == bullet.parent_id && bullet.fly_dist < parent_ignore_distance));
	IKinematics* const kinematics = smart_cast<IKinematics*>(result.O->Visual());
	if (!kinematics)
		return (FALSE);

#ifdef CBULLETMANAGER_EX
    if (result.O->GetBulletCheckVisual() && !bullet_manager.ValidateHitDynamicVisualMesh(bullet, result, collide_position, data.max_distance))
    {
        /* Ignore this hit and allow the bullet to continue flying. */
        return TRUE;
    }
#endif

	CBoneData const& bone_data = kinematics->LL_GetData((u16)result.element);
	bullet_manager.RegisterEvent(EVENT_HIT, TRUE, &bullet, collide_position, result, bone_data.game_mtl_idx);
	return (FALSE);
}

bool CBulletManager::trajectory_check_error(
	Fvector& previous_position,
	collide::rq_results& storage,
	SBullet& bullet,
	float& low,
	float& high,
	Fvector const& gravity,
	float const air_resistance
)
{
	Fvector const& position = bullet.start_position;
	Fvector const& velocity = bullet.start_velocity;
	Fvector const start = trajectory_position(position, velocity, gravity, air_resistance, low);
	Fvector const target = trajectory_position(position, velocity, gravity, air_resistance, high);
	Fvector start_to_target = Fvector().sub(target, start);
	float const distance = start_to_target.magnitude();
	if (fis_zero(distance))
		return (true);

	start_to_target.mul(1.f / distance);

	bullet_test_callback_data data;
	data.pBullet = &bullet;
#ifdef CBULLETMANAGER_EX
    data.max_distance = distance;
#endif
#if 1//def DEBUG
	data.high_time = high;
#endif // #ifdef DEBUG
	bullet.flags.ricochet_was = 0;
	bullet.dir = start_to_target;

	collide::ray_defs RD(start, start_to_target, distance, CDB::OPT_FULL_TEST, collide::rqtBoth);
	BOOL const result = Level().ObjectSpace.RayQuery(storage, RD, CBulletManager::firetrace_callback, &data,
	                                                 CBulletManager::test_callback, NULL);
	if (!result || (data.collide_time == 0.f))
	{
		add_bullet_point(bullet.start_position, previous_position, bullet.start_velocity, gravity, air_resistance,
		                 high);
		return (true);
	}

	add_bullet_point(bullet.start_position, previous_position, bullet.start_velocity, gravity, air_resistance,
	                 data.collide_time);

	low = 0.f;

	VERIFY(high >= data.collide_time);
	high -= data.collide_time;

	++bullet.change_rajectory_count;
	bullet.start_position = data.collide_position;
	bullet.tracer_start_position = bullet.bullet_pos;
	bullet.bullet_pos = data.collide_position;
	bullet.start_velocity = Fvector().mul(bullet.dir, bullet.speed);
	bullet.born_time += iFloor(data.collide_time * 1000.f);
	bullet.life_time = 0.f;

	// Add RQ range to the bullet fly dist
	for (auto i = storage.r_begin(); i != storage.r_end();) {
		bullet.fly_dist += i->range;
		break;
	}
	
	return (false);
}

template <typename TBullet>
static bool try_update_bullet_state(TBullet& bullet, Fvector const& gravity, float const air_resistance,
                                    float const time)
{
	Fvector const new_position = trajectory_position(bullet.start_position, bullet.start_velocity, gravity,
	                                                 air_resistance, time);
	bullet.fly_dist += bullet.bullet_pos.distance_to(new_position);

	if (bullet.fly_dist >= bullet.max_dist)
		return false;

	Fbox const level_box = Level().ObjectSpace.GetBoundingVolume();
	if (
		(new_position.x < level_box.x1) ||
		(new_position.x > level_box.x2) ||
		(new_position.y < level_box.y1) ||
		//		(new_position.y > level_box.y2) ||
		(new_position.z < level_box.z1) ||
		(new_position.z > level_box.z2)
	)
		return false;

	Fvector const new_velocity = trajectory_velocity(bullet.start_velocity, gravity, air_resistance, bullet.life_time);
	bullet.speed = new_velocity.magnitude();
	if (fis_zero(bullet.speed))
		return false;

	bullet.bullet_pos = new_position;
	bullet.dir = Fvector(new_velocity).normalize_safe();
	bullet.life_time = time;
	return true;
}

static bool try_update_bullet(SBullet& bullet, Fvector const& gravity, float const air_resistance, float const time,
                              bool allow_script_callback_lookup)
{
	if (!try_update_bullet_state(bullet, gravity, air_resistance, time))
		return false;

	// Script callbacks stay on the original serial path. The speculative path is
	// enabled only when this hook is absent and never constructs Lua objects.
	if (allow_script_callback_lookup)
	{
		const ::luabind::functor<void>* funct = bullet_update_callback();
		if (funct)
		{
			::luabind::object table = ::luabind::newtable(ai().script_engine().lua());
			populateBulletTable(
				table,
				bullet.bullet_pos,
				bullet.dir,
				bullet.speed,
				bullet.fly_dist,
				bullet.catridgeSection,
				bullet.bulletId,
				bullet.weapon_id,
				bullet.parent_id,
				65535,
				NULL,
				bullet.life_time,
				-1
			);
			(*funct)(table);
		}
	}

	return true;
}


bool CBulletManager::process_bullet(collide::rq_results& storage, SBullet& bullet, u32 const delta_time,
                                    bool allow_update_script_lookup)
{
	float const time_delta = float(delta_time) / 1000.f;
	Fvector const gravity = Fvector().set(0.f, -m_fGravityConst, 0.f);

	float const air_resistance = (GameID() == eGameIDSingle) ? m_fAirResistanceK : bullet.air_resistance;
	bullet.tracer_start_position = bullet.bullet_pos;

#if 0//def DEBUG
	extern BOOL g_bDrawBulletHit;
	if (g_bDrawBulletHit)
	{
		Msg	(
			"free fly velocity: %f",
			trajectory_velocity(
				bullet.start_velocity,
				gravity,
				air_resistance,
				fis_zero(air_resistance) ?
				0.f :
				(1.f/air_resistance - air_resistance_epsilon)
			).magnitude()
		);
	}
#endif

	Fvector const& start_position = bullet.bullet_pos;
	Fvector previous_position = start_position;
	float low = bullet.life_time;
	float high = bullet.life_time + time_delta;
	//	Msg							("process_bullet0: low[%f], high[%f]", low, high);

	bullet.change_rajectory_count = 0;

	for (;;)
	{
		for (;;)
		{
			if (bullet.speed < 1.f)
				return (false);

			if (bullet.change_rajectory_count >= 32)
				return (false);

			float time = trajectory_select_pick_time(bullet, low, high, gravity, air_resistance);
			if (time == low)
				return (false);

			float safe_time = time;
			VERIFY2(safe_time <= high, make_string("safe_time[%f], high[%f]", safe_time, high));
			if (!trajectory_check_error(previous_position, storage, bullet, low, time, gravity, air_resistance))
			{
				VERIFY2(safe_time >= time, make_string("safe_time[%f], time[%f]", safe_time, time));
				VERIFY2(safe_time <= high, make_string("safe_time[%f], high[%f]", safe_time, high));
				//				clamp			(safe_time, time, high);
				high = high - safe_time + time;
				VERIFY2(low <= high, make_string("start_low[%f] high[%f]", low, high));
				if (fsimilar(low, high))
					return (!fis_zero(bullet.speed));

				break;
			}

			if (!try_update_bullet(bullet, gravity, air_resistance, time, allow_update_script_lookup))
				return (false);

			if (fsimilar(time, high))
				return (true);

			VERIFY2(low < high, make_string("start_low[%f] high[%f]", low, high));
			low = time;
			VERIFY2(low < high, make_string("start_low[%f] high[%f]", low, high));
		}

		if (fis_zero(bullet.speed))
			return (false);
	}
}

BOOL CBulletManager::speculative_test_callback(const collide::ray_defs& rd, CObject* object, LPVOID params)
{
	(void)rd;
	if (!object)
		return FALSE;

	SpeculativeQueryData& data = *static_cast<SpeculativeQueryData*>(params);
	SpeculativeBulletState& bullet = *data.bullet;

	if (object->ID() == bullet.parent_id && bullet.fly_dist < parent_ignore_distance &&
		!bullet.ricochet_was)
		return FALSE;

	// Dynamic collision forms can rebuild skeleton/bone state and gameplay filters
	// can consume RNG or emit sounds. Any non-owner broadphase candidate therefore
	// falls back to the unchanged serial narrowphase and callback pipeline.
	data.requires_serial = true;
	return TRUE;
}

bool CBulletManager::trajectory_check_clear_speculative(SpeculativeBulletState& bullet, float low, float high,
                                                        const Fvector& gravity, float air_resistance)
{
	const Fvector start = trajectory_position(bullet.start_position, bullet.start_velocity, gravity,
	                                          air_resistance, low);
	const Fvector target = trajectory_position(bullet.start_position, bullet.start_velocity, gravity,
	                                           air_resistance, high);
	Fvector direction;
	direction.sub(target, start);
	const float distance = direction.magnitude();
	if (fis_zero(distance))
		return true;

	direction.mul(1.f / distance);
	bullet.ricochet_was = 0;
	bullet.dir = direction;

	SpeculativeQueryData data;
	data.bullet = &bullet;

	const collide::ray_defs ray(start, direction, distance, CDB::OPT_FULL_TEST, collide::rqtBoth);
	const BOOL possible_collision = Level().ObjectSpace.RayTestPotentialMTLocked(ray,
		&CBulletManager::speculative_test_callback, &data, nullptr);

	return !possible_collision && !data.requires_serial;
}

CBulletManager::SpeculativeStatus CBulletManager::process_bullet_speculative(SpeculativeBulletState& bullet,
	u32 delta_time)
{
	const float time_delta = float(delta_time) / 1000.f;
	const Fvector gravity = Fvector().set(0.f, -m_fGravityConst, 0.f);
	const float air_resistance = m_fAirResistanceK;

	bullet.tracer_start_position = bullet.bullet_pos;
	float low = bullet.life_time;
	const float high = bullet.life_time + time_delta;
	bullet.change_rajectory_count = 0;

	for (;;)
	{
		if (bullet.speed < 1.f)
			return SpeculativeStatus::Remove;

		const float time = trajectory_select_pick_time(bullet, low, high, gravity, air_resistance);
		if (time == low)
			return SpeculativeStatus::Remove;

		if (!trajectory_check_clear_speculative(bullet, low, time, gravity, air_resistance))
			return SpeculativeStatus::Fallback;

		if (!try_update_bullet_state(bullet, gravity, air_resistance, time))
			return SpeculativeStatus::Remove;

		if (fsimilar(time, high))
			return SpeculativeStatus::Alive;

		R_ASSERT(low < high);
		low = time;
	}
}

#ifdef DEBUG
	BOOL g_bDrawBulletHit = FALSE;
#endif

float SqrDistancePointToSegment(const Fvector& pt, const Fvector& orig, const Fvector& dir)
{
	Fvector diff;
	diff.sub(pt, orig);
	float fT = diff.dotproduct(dir);

	if (fT <= 0.0f)
	{
		fT = 0.0f;
	}
	else
	{
		float fSqrLen = dir.square_magnitude();
		if (fT >= fSqrLen)
		{
			fT = 1.0f;
			diff.sub(dir);
		}
		else
		{
			fT /= fSqrLen;
			diff.sub(Fvector().mul(dir, fT));
		}
	}

	return diff.square_magnitude();
}

BOOL g_render_short_tracers = 1;
void CBulletManager::Render()
{
#ifdef DEBUG
	if (g_bDrawBulletHit && !m_bullet_points.empty()) {
		VERIFY							(!(m_bullet_points.size() % 2));
		CDebugRenderer& renderer		= Level().debug_renderer();
		Fmatrix	sphere					= Fmatrix().scale(.05f, .05f, .05f);
		BulletPoints::const_iterator	i = m_bullet_points.begin();
		BulletPoints::const_iterator	e = m_bullet_points.end();
		for ( ; i != e; i+=2) {
			sphere.c					= *i;
			renderer.draw_ellipse		(sphere, D3DCOLOR_XRGB(255, 0, 0));

			renderer.draw_line			(Fidentity, *i, *(i + 1), D3DCOLOR_XRGB(0, 255, 0));

			sphere.c					= *(i + 1);
			renderer.draw_ellipse		(sphere, D3DCOLOR_XRGB(255, 0, 0));
		}

		if (m_bullet_points.size() > 32768)
			m_bullet_points.clear_not_free	();
	}
	else
		m_bullet_points.clear_not_free	();

	//0-рикошет
	//1-застрявание пули в материале
	//2-пробивание материала
	if (g_bDrawBulletHit) {
		extern FvectorVec g_hit[];
		FvectorIt it;
		u32 C[3] = {0xffff0000,0xff00ff00,0xff0000ff};
		//RCache.set_xform_world(Fidentity);
		DRender->CacheSetXformWorld(Fidentity);
		for(int i=0; i<3; ++i)
			for(it=g_hit[i].begin();it!=g_hit[i].end();++it){
				Level().debug_renderer().draw_aabb(*it,0.01f,0.01f,0.01f,C[i]);
			}
	}
#endif

	if (m_BulletsRendered.empty() || !psActorFlags.test(AF_USE_TRACERS)) return;

	//u32	vOffset			=	0	;
	u32 bullet_num = m_BulletsRendered.size();

	UIRender->StartPrimitive((u32)bullet_num * 12, IUIRender::ptTriList, IUIRender::pttLIT);

	for (RenderBulletVecIt it = m_BulletsRendered.begin(); it != m_BulletsRendered.end(); ++it)
	{
		SRenderBullet* bullet = &(*it);
		if (!bullet->CanBeRenderedNow())
			continue;

		Fvector const tracer = Fvector().sub(bullet->bullet_pos, bullet->tracer_start_position);
		float length = tracer.magnitude();
		Fvector const tracer_direction = length >= EPS_L
			                                 ? Fvector(tracer).mul(1.f / length)
			                                 : Fvector().set(0.f, 0.f, 1.f);

		if (length < m_fTracerLengthMin)
		{
			// momopate: Optionally cap tracers to their minimum length instead of not rendering them
			if (g_render_short_tracers)
				length = m_fTracerLengthMin;
			else
				continue;
		}

		if (length > m_fTracerLengthMax)
			length = m_fTracerLengthMax;

		float width = m_fTracerWidth;
		float dist2segSqr = SqrDistancePointToSegment(Device.vCameraPosition, bullet->bullet_pos, tracer);
		//---------------------------------------------
		float MaxDistSqr = 1.0f;
		float MinDistSqr = 0.09f;
		if (dist2segSqr < MaxDistSqr)
		{
			if (dist2segSqr < MinDistSqr) dist2segSqr = MinDistSqr;

			width *= _sqrt(dist2segSqr / MaxDistSqr);
		}
		if (Device.vCameraPosition.distance_to_sqr(bullet->bullet_pos) < (length * length))
		{
			length = Device.vCameraPosition.distance_to(bullet->bullet_pos) - 0.3f;
		}

		Fvector center;
		center.mad(bullet->bullet_pos, tracer_direction, -length * .5f);
		bool bActor = false;
		if (Level().CurrentViewEntity())
		{
			bActor = (bullet->parent_id == Level().CurrentViewEntity()->ID());
		}
		tracers.Render(bullet->bullet_pos, center, tracer_direction, length, width, bullet->color_id, bullet->speed,
		               bActor);
	}

	UIRender->CacheSetCullMode(IUIRender::cmNONE);
	UIRender->CacheSetXformWorld(Fidentity);
	UIRender->SetShader(*tracers.sh_Tracer);
	UIRender->FlushPrimitive();
	UIRender->CacheSetCullMode(IUIRender::cmCCW);
}

void CBulletManager::CommitRenderSet() // @ the end of frame
{
	m_BulletsRendered.clear_not_free();
	if (m_BulletsRendered.capacity() < m_Bullets.size())
		m_BulletsRendered.reserve(m_Bullets.size());

	for (BulletVec::const_iterator it = m_Bullets.begin(); it != m_Bullets.end(); ++it)
	{
		if (!it->flags.allow_tracer)
			continue;

		SRenderBullet render_bullet;
		render_bullet.init_frame_num = it->init_frame_num;
		render_bullet.bullet_pos = it->bullet_pos;
		render_bullet.tracer_start_position = it->tracer_start_position;
		render_bullet.speed = it->speed;
		render_bullet.parent_id = it->parent_id;
		render_bullet.color_id = it->m_u8ColorID;
		m_BulletsRendered.push_back(render_bullet);
	}

	if (m_Events.capacity() < m_Bullets.size() * 2)
		m_Events.reserve(m_Bullets.size() * 2);

	if (g_mt_config.test(mtBullets))
	{
		Device.add_to_seq_parallel(
			fastdelegate::FastDelegate0<>(this, &CBulletManager::UpdateWorkload), "bullet_workload");
	}
	else
	{
		UpdateWorkload();
	}
}

void CBulletManager::CommitEvents() // @ the start of frame
{
	if (m_Events.size() > 1000)
		Msg("! too many bullets during single frame: %d", m_Events.size());

	for (u32 _it = 0; _it < m_Events.size(); _it++)
	{
		_event& E = m_Events[_it];
		SBullet* bullet = &E.bullet;
		Fvector& end_point = E.point;
		switch (E.Type)
		{
		case EVENT_HIT:
			{
				SGameMtl* mt = GMLib.GetMaterialByIdx(E.tgt_material);
				// demonized: bullet on impact callback
				if (const ::luabind::functor<void>* funct = bullet_impact_callback()) {
					::luabind::object table = ::luabind::newtable(ai().script_engine().lua());
					populateBulletTable(
						table,
						!fis_zero(end_point.x) && !fis_zero(end_point.y) && !fis_zero(end_point.z) ? end_point : bullet->bullet_pos,
						bullet->dir,
						bullet->speed,
						bullet->fly_dist + E.R.range,
						bullet->catridgeSection,
						bullet->bulletId,
						bullet->weapon_id,
						bullet->parent_id,
						E.dynamic && E.R.O ? E.R.O->ID() : 65535,
						mt ? mt->m_Name.c_str() : NULL,
						bullet->life_time,
						E.R.element
					);
					(*funct)(table);
				}

				if (E.dynamic) DynamicObjectHit(E);
				else StaticObjectHit(E);
			}

			break;
		case EVENT_REMOVE:
			{
				// demonized: bullet on remove callback
				if (const ::luabind::functor<void>* funct = bullet_remove_callback()) {
					::luabind::object table = ::luabind::newtable(ai().script_engine().lua());
					populateBulletTable(
						table,
						!fis_zero(end_point.x) && !fis_zero(end_point.y) && !fis_zero(end_point.z) ? end_point : bullet->bullet_pos,
						bullet->dir,
						bullet->speed,
						bullet->fly_dist,
						bullet->catridgeSection,
						bullet->bulletId,
						bullet->weapon_id,
						bullet->parent_id,
						65535,
						NULL,
						bullet->life_time,
						-1
					);
					(*funct)(table);
				}

				if (E.bullet.flags.allow_sendhit && GameID() != eGameIDSingle)
					Game().m_WeaponUsageStatistic->OnBullet_Remove(&E.bullet);
				if (E.tgt_material >= m_Bullets.size())
				{
					Msg("! invalid bullet removal index: %u, bullets: %u", E.tgt_material, m_Bullets.size());
					break;
				}
				m_Bullets[E.tgt_material] = m_Bullets.back();
				m_Bullets.pop_back();
			}
			break;
		}
	}
	m_Events.clear_and_reserve();
}

void CBulletManager::RegisterEvent(EventType Type, BOOL _dynamic, SBullet* bullet, const Fvector& end_point,
                                   collide::rq_result& R, u16 tgt_material)
{
#if 0//def DEBUG
	if (m_Events.size() > 1000) {
		static bool breakpoint = true;
		if (breakpoint)
			__asm int 3;
	}
#endif // #ifdef DEBUG

	m_Events.emplace_back();
	_event& E = m_Events.back();
	E.Type = Type;
	E.bullet = *bullet;

	switch (Type)
	{
	case EVENT_HIT:
		{
			E.dynamic = _dynamic;
			E.point = end_point;
			E.R = R;
			E.tgt_material = tgt_material;

			ObjectHit(&E.hit_result, bullet, end_point, R, tgt_material, E.normal);

			if (_dynamic)
			{
				//	E.Repeated = (R.O->ID() == E.bullet.targetID);
				//	bullet->targetID = R.O->ID();

				E.Repeated = (R.O->ID() == E.bullet.targetID);
				if (GameID() == eGameIDSingle)
				{
					bullet->targetID = R.O->ID();
				}
				else
				{
					if (bullet->targetID != R.O->ID())
					{
						CGameObject* pGO = smart_cast<CGameObject*>(R.O);
						if (!pGO || !pGO->BonePassBullet(R.element))
							bullet->targetID = R.O->ID();
					}
				}
			};
		}
		break;
	case EVENT_REMOVE:
		{
			E.tgt_material = tgt_material;
		}
		break;
	}
}

#ifdef CBULLETMANAGER_EX
bool CBulletManager::ValidateHitDynamicVisualMesh(SBullet& B, collide::rq_result& R, Fvector& collide_position, float range)
{
    IKinematics::pick_result res;
    if (R.O->Visual()->dcast_PKinematics()->PickBone(R.O->XFORM(), res, range, B.bullet_pos, B.dir, (u16)R.element))
    {
        collide_position.mad(B.bullet_pos, B.dir, res.dist);
        return true;
    }
    return false;
}
#endif
