#include "stdafx.h"
#pragma hdrstop

#include "cl_intersect.h"
#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_TargetA.h"
#include "SoundRender_Source.h"
#include "SoundRender_CoreA.h"

namespace
{
	constexpr u32 target_prime_count_budget = 64;
	constexpr u32 target_prime_minimum_progress = 2;
	constexpr u32 target_prime_time_budget_us = 3000;
}

CSoundRender_Emitter* CSoundRender_Core::i_play(ref_sound* S, BOOL _loop, float delay)
{
	VERIFY(S->_p->feedback==0);
	CSoundRender_Emitter* E = xr_new<CSoundRender_Emitter>();
	S->_p->feedback = E;
	E->start(S, _loop, delay);
	s_emitters.push_back(E);
	return E;
}

void CSoundRender_Core::update(const Fvector& P, const Fvector& D, const Fvector& N)
{
	u32 it;

	if (0 == bReady) return;

	if (InterlockedExchange(&bPendingDeviceListRefresh, FALSE))
	{
		refresh_devices();
	}
	if (InterlockedExchange(&bPendingDefaultDeviceSwitch, FALSE))
	{
		default_device_changed();
	}

	InterlockedExchange(&bLocked, TRUE);
	float new_tm = Timer.GetElapsed_sec();
	fTimer_Delta = new_tm - fTimer_Value;
	//.	float dt					= float(Timer_Delta)/1000.f;
	float dt_sec = fTimer_Delta;
	fTimer_Value = new_tm;

	s_emitters_u ++;
	m_occlusion_queries = 0;
	m_occlusion_query_time_ticks = 0;
	m_occlusion_resume_stagger_active =
		InterlockedExchange(&m_occlusion_resume_stagger_pending, FALSE) != FALSE;
	// Render-target emitters are processed first. Count and CPU-time limits therefore
	// preserve the freshest occlusion for audible sounds while cached results let the
	// remaining simulated emitters retry over subsequent updates. Query cost varies
	// sharply when level/SOM pages are cold, so a count-only limit cannot bound the
	// sound worker's contribution to the frame barrier. Setting
	// snd_occlusion_update_ms to 0 keeps the legacy every-update behavior and
	// intentionally disables both limits.
	constexpr u32 occlusion_time_budget_us = 2000;
	const u32 target_count = static_cast<u32>(s_targets.size());
	m_occlusion_query_budget = _min(128u, _max(64u, (target_count + 1u) / 2u));
	m_occlusion_query_time_budget_ticks = CPU::qpc_freq
		? CPU::qpc_freq * occlusion_time_budget_us / 1000000u
		: 0;

	// Firstly update emitters, which are now being rendered
	//Msg	("! update: r-emitters");
	u32 rendered_emitters = 0;
	for (it = 0; it < s_targets.size(); it++)
	{
		CSoundRender_Target* T = s_targets[it];
		CSoundRender_Emitter* E = T->get_emitter();
		if (E)
		{
			++rendered_emitters;
			E->update(dt_sec);
			E->marker = s_emitters_u;
			E = T->get_emitter(); // update can stop itself
			if (E) T->priority = E->priority();
			else T->priority = -1;
		}
		else
		{
			T->priority = -1;
		}
	}

	// Update emmitters
	//Msg	("! update: emitters");
	u32 emitter_write = 0;
	for (it = 0; it < s_emitters.size(); ++it)
	{
		CSoundRender_Emitter* pEmitter = s_emitters[it];
		if (pEmitter->marker != s_emitters_u)
		{
			pEmitter->update(dt_sec);
			pEmitter->marker = s_emitters_u;
		}
		if (!pEmitter->isPlaying())
		{
			xr_delete(pEmitter);
			continue;
		}

		if (emitter_write != it)
			s_emitters[emitter_write] = pEmitter;
		++emitter_write;
	}
	if (emitter_write != s_emitters.size())
		s_emitters.resize(emitter_write);

	// Get currently rendering emitters
	//Msg	("! update: targets");
	s_targets_defer.clear();
	m_target_buffers_primed = 0;
	m_target_prime_time_ticks = 0;
	m_target_prime_time_budget_ticks = CPU::qpc_freq
		? CPU::qpc_freq * target_prime_time_budget_us / 1000000u
		: 0;

	// Every newly started target already owns a short playback block. Prime the
	// second/third safety blocks under a global budget instead of letting hundreds
	// of targets decode in one update. The rotating start index makes all targets
	// receive their second block before any fixed prefix can monopolize the budget.
	const u32 target_count_for_update = static_cast<u32>(s_targets.size());
	const u32 target_update_start = target_count_for_update
		? s_targets_pu % target_count_for_update
		: 0;
	u32 prime_cursor_advance = 1;
	for (u32 offset = 0; offset < target_count_for_update; ++offset)
	{
		const u32 target_index = (target_update_start + offset) % target_count_for_update;
		CSoundRender_Target* T = s_targets[target_index];
		if (T->get_emitter() && T->get_Rendering())
		{
			T->fill_parameters();
			const u32 primed_before = m_target_buffers_primed;
			T->update();
			if (m_target_buffers_primed != primed_before)
				prime_cursor_advance = offset + 1;
		}
	}
	if (target_count_for_update)
		s_targets_pu = (target_update_start + prime_cursor_advance) % target_count_for_update;

	// Preserve the stable target order for physical starts and parameter commits.
	for (it = 0; it < s_targets.size(); ++it)
	{
		CSoundRender_Target* T = s_targets[it];
		if (T->get_emitter() && !T->get_Rendering())
			s_targets_defer.push_back(T);
	}

	// Commit parameters from pending targets
	if (!s_targets_defer.empty())
	{
		//Msg	("! update: start render - commit");
		s_targets_defer.erase(std::unique(s_targets_defer.begin(), s_targets_defer.end()), s_targets_defer.end());
		for (it = 0; it < s_targets_defer.size(); it++)
			s_targets_defer[it]->fill_parameters();
	}

	// update EFX
	if (m_is_supported)
	{
		if (bListenerMoved)
		{
			bListenerMoved = FALSE;
			e_target_ptr = get_environment(P);
			if (!e_target_ptr)
				e_target_ptr = &e_identity;
		}

		// demonized: Interpolate from e_current to 95% of e_target in close to exact time
		constexpr float percent = 0.95f;
		float alpha = 1.f;
		if (snd_efx_environment_change_time > EPS_S)
			alpha = 1.0f - std::exp(std::log(1.0f - percent) * dt_sec / snd_efx_environment_change_time);
		clamp(alpha, 0.f, 1.f);
		//Msg("interpolating from e_current to e_target %.2f", std::min(e_current.Reverb, e_target_ptr->Reverb) / std::max(e_current.Reverb, e_target_ptr->Reverb));
		e_current.lerp(e_current, *e_target_ptr, alpha);

		set_listener(e_current);
		commit();
	}

	// update listener
	update_listener(P, D, N, dt_sec);

	// Start rendering of pending targets
	if (!s_targets_defer.empty())
	{
		CSoundRender_CoreA* Core = (CSoundRender_CoreA*)this;
		//Msg	("! update: start render");
		for (it = 0; it < s_targets_defer.size(); it++)
		{
			CSoundRender_TargetA* Ptr = (CSoundRender_TargetA*)s_targets_defer[it];
			if (m_is_supported)
				Ptr->SetSlot(Core->slot);
			Ptr->render();
		}
	}


	// Events
	update_events();

	InterlockedExchange(&bLocked, FALSE);
}

bool CSoundRender_Core::try_begin_occlusion_query()
{
	if (psSoundOcclusionUpdateMS <= 0)
		return true;

	const bool count_budget_exhausted = m_occlusion_queries >= m_occlusion_query_budget;
	const bool time_budget_exhausted = m_occlusion_queries != 0 &&
		m_occlusion_query_time_budget_ticks != 0 &&
		m_occlusion_query_time_ticks >= m_occlusion_query_time_budget_ticks;
	if (!count_budget_exhausted && !time_budget_exhausted)
		return true;

	return false;
}

bool CSoundRender_Core::try_begin_target_prime()
{
	const bool count_budget_exhausted = m_target_buffers_primed >= target_prime_count_budget;
	const bool time_budget_exhausted =
		m_target_buffers_primed >= target_prime_minimum_progress &&
		m_target_prime_time_budget_ticks != 0 &&
		m_target_prime_time_ticks >= m_target_prime_time_budget_ticks;
	if (!count_budget_exhausted && !time_budget_exhausted)
		return true;

	return false;
}

static u32 g_saved_event_count = 0;

void CSoundRender_Core::update_events()
{
	g_saved_event_count = s_events.size();
	for (u32 it = 0; it < s_events.size(); it++)
	{
		event& E = s_events[it];
		Handler(E.first, E.second);
	}
	s_events.clear_not_free();
}

void CSoundRender_Core::statistic(CSound_stats* dest, CSound_stats_ext* ext)
{
	if (dest)
	{
		dest->_rendered = 0;
		for (u32 it = 0; it < s_targets.size(); it++)
		{
			CSoundRender_Target* T = s_targets[it];
			if (T->get_emitter() && T->get_Rendering()) dest->_rendered++;
		}
		dest->_simulated = s_emitters.size();
		dest->_cache_hits = cache._stat_hit;
		dest->_cache_misses = cache._stat_miss;
		dest->_events = g_saved_event_count;
		cache.stats_clear();
	}
	if (ext)
	{
		for (u32 it = 0; it < s_emitters.size(); it++)
		{
			CSoundRender_Emitter* _E = s_emitters[it];
			CSound_stats_ext::SItem _I;
			_I._3D = !_E->b2D;
			_I._rendered = !!_E->target;
			_I.params = _E->p_source;
			_I.volume = _E->smooth_volume;
			if (_E->owner_data)
			{
				_I.name = _E->source()->fname;
				_I.game_object = _E->owner_data->g_object;
				_I.game_type = _E->owner_data->g_type;
				_I.type = _E->owner_data->s_type;
			}
			else
			{
				_I.game_object = 0;
				_I.game_type = 0;
				_I.type = st_Effect;
			}
			ext->append(_I);
		}
	}
}

float CSoundRender_Core::get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion)
{
	float occ_value = 1.f;

	if (0 != geom_SOM)
	{
		// Calculate RAY params
		Fvector pos, dir;
		pos.random_dir();
		pos.mul(dispersion);
		pos.add(snd_pt);
		dir.sub(pos, hear_pt);
		const float range = dir.magnitude();
		if (range <= EPS_S)
			return occ_value;
		dir.div(range);

#ifdef _EDITOR
		ETOOLS::ray_options		(CDB::OPT_CULL);
		ETOOLS::ray_query		(geom_SOM,hear_pt,dir,range);
		u32 r_cnt				= ETOOLS::r_count();
		CDB::RESULT*	_B 		= ETOOLS::r_begin();
#else
		geom_DB.ray_options(CDB::OPT_CULL);
		geom_DB.ray_query(geom_SOM, hear_pt, dir, range);
		u32 r_cnt = geom_DB.r_count();
		CDB::RESULT* _B = geom_DB.r_begin();
#endif
		if (0 != r_cnt)
		{
			for (u32 k = 0; k < r_cnt; k++)
			{
				CDB::RESULT* R = _B + k;
				float triangle_occlusion = 1.f;
				CopyMemory(&triangle_occlusion, &R->dummy, sizeof(triangle_occlusion));
				occ_value *= triangle_occlusion;
			}
		}
	}
	return occ_value;
}

float CSoundRender_Core::get_occlusion(Fvector& P, float R, Fvector* occ)
{
	++m_occlusion_queries;
	float occ_value = 1.f;

	// Calculate RAY params
	Fvector base = listener_position();
	Fvector pos, dir;
	float range;
	pos.random_dir();
	pos.mul(R);
	pos.add(P);
	dir.sub(pos, base);
	range = dir.magnitude();
	if (range <= EPS_S)
		return occ_value;
	dir.div(range);

	if (0 != geom_MODEL)
	{
		bool bNeedFullTest = true;
		// 1. Check cached polygon
		float _u, _v, _range;
		if (CDB::TestRayTri(base, dir, occ, _u, _v, _range, true))
			if (_range > 0 && _range < range)
			{
				occ_value = psSoundOcclusionScale;
				bNeedFullTest = false;
			}
		// 2. Polygon doesn't picked up - real database query
		if (bNeedFullTest)
		{
#ifdef _EDITOR
			ETOOLS::ray_options		(CDB::OPT_ONLYNEAREST);
			ETOOLS::ray_query		(geom_MODEL,base,dir,range);
			if (0!=ETOOLS::r_count()){ 
				// cache polygon
				const CDB::RESULT*	R = ETOOLS::r_begin			();
#else
			geom_DB.ray_options(CDB::OPT_ONLYNEAREST);
			geom_DB.ray_query(geom_MODEL, base, dir, range);
			if (0 != geom_DB.r_count())
			{
				// cache polygon
				const CDB::RESULT* R = geom_DB.r_begin();
#endif
				const CDB::TRI& T = geom_MODEL->get_tris()[R->id];
				const Fvector* V = geom_MODEL->get_verts();
				occ[0].set(V[T.verts[0]]);
				occ[1].set(V[T.verts[1]]);
				occ[2].set(V[T.verts[2]]);
				occ_value = psSoundOcclusionScale;
			}
		}
	}
	if (0 != geom_SOM)
	{
#ifdef _EDITOR
		ETOOLS::ray_options		(CDB::OPT_CULL);
		ETOOLS::ray_query		(geom_SOM,base,dir,range);
		u32 r_cnt				= ETOOLS::r_count();
        CDB::RESULT*	_B 		= ETOOLS::r_begin();
#else
		geom_DB.ray_options(CDB::OPT_CULL);
		geom_DB.ray_query(geom_SOM, base, dir, range);
		u32 r_cnt = geom_DB.r_count();
		CDB::RESULT* _B = geom_DB.r_begin();
#endif
		if (0 != r_cnt)
		{
			for (u32 k = 0; k < r_cnt; k++)
			{
				CDB::RESULT* R = _B + k;
				float triangle_occlusion = 1.f;
				CopyMemory(&triangle_occlusion, &R->dummy, sizeof(triangle_occlusion));
				occ_value *= triangle_occlusion;
			}
		}
	}
	return occ_value;
}
