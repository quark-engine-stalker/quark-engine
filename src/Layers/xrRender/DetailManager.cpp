// DetailManager.cpp: implementation of the CDetailManager class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "DetailManager.h"
#include "cl_intersect.h"
#include "../../xrCDB/Frustum.h"
#include "../../xrCore/job_system.h"
#include <xmmintrin.h>

#ifdef _EDITOR
#	include "ESceneClassList.h"
#	include "Scene.h"
#	include "SceneObject.h"
#	include "igame_persistent.h"
#	include "environment.h"
#else
#	include "../../xrEngine/igame_persistent.h"
#	include "../../xrEngine/environment.h"
#endif

const float dbgOffset = 0.f;
const int dbgItems = 128;

namespace
{
IC u32 detail_hash(u32 value)
{
	value ^= value >> 16;
	value *= 0x7FEB352Du;
	value ^= value >> 15;
	value *= 0x846CA68Bu;
	value ^= value >> 16;
	return value;
}

IC u32 detail_slot_jitter(const void* slot, u32 frame, u32 minimum, u32 maximum)
{
	R_ASSERT(maximum > minimum);
	const uintptr_t value = reinterpret_cast<uintptr_t>(slot);
	const u32 seed = frame ^ u32(value) ^ u32(value >> 32);
	return minimum + detail_hash(seed) % (maximum - minimum);
}

IC bool advance_detail_alpha(CDetailManager::SlotItemVec& items, float alpha_step)
{
	CDetailManager::SlotItem* const* item = items.data();
	CDetailManager::SlotItem* const* const item_end = item + items.size();
	constexpr size_t prefetch_distance = 8;
	bool still_fading = false;
	for (; item != item_end; ++item)
	{
		if (static_cast<size_t>(item_end - item) > prefetch_distance)
			_mm_prefetch(reinterpret_cast<const char*>(item[prefetch_distance]) +
				offsetof(CDetailManager::SlotItem, alpha), _MM_HINT_T0);

		CDetailManager::SlotItem& instance = **item;
		VERIFY(instance.alpha_target != 0);
		if (instance.alpha < 1.f)
		{
			instance.alpha = _min(instance.alpha + alpha_step, 1.f);
			still_fading = still_fading || instance.alpha < 1.f;
		}
	}
	return still_fading;
}

constexpr u32 detail_expand_block_size = 4096;

struct detail_expand_context
{
	DetailSlot* destination;
	const DetailSlot_v3* source;
	u32 count;
	__declspec(align(64)) volatile LONG next;
};

void expand_detail_slots_job(void* raw_context)
{
	detail_expand_context& context = *static_cast<detail_expand_context*>(raw_context);
	for (;;)
	{
		const LONG start_value = InterlockedExchangeAdd(&context.next, static_cast<LONG>(detail_expand_block_size));
		if (start_value < 0 || static_cast<u32>(start_value) >= context.count)
			return;

		const u32 start = static_cast<u32>(start_value);
		const u32 end = _min(start + detail_expand_block_size, context.count);
		for (u32 index = start; index < end; ++index)
			expand_v3(context.destination[index], context.source[index]);
	}
}
}

//--------------------------------------------------- Decompression
static int magic4x4[4][4] =
{
	{0, 14, 3, 13},
	{11, 5, 8, 6},
	{12, 2, 15, 1},
	{7, 9, 4, 10}
};

void bwdithermap(int levels, int magic[16][16])
{
	/* Get size of each step */
	float N = 255.0f / (levels - 1);

	/*
	* Expand 4x4 dither pattern to 16x16.  4x4 leaves obvious patterning,
	* and doesn't give us full intensity range (only 17 sublevels).
	*
	* magicfact is (N - 1)/16 so that we get numbers in the matrix from 0 to
	* N - 1: mod N gives numbers in 0 to N - 1, don't ever want all
	* pixels incremented to the next level (this is reserved for the
	* pixel value with mod N == 0 at the next level).
	*/

	float magicfact = (N - 1) / 16;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			for (int k = 0; k < 4; k++)
				for (int l = 0; l < 4; l++)
					magic[4 * k + i][4 * l + j] =
						(int)(0.5 + magic4x4[i][j] * magicfact +
							(magic4x4[k][l] / 16.) * magicfact);
}

//--------------------------------------------------- Decompression

void CDetailManager::SSwingValue::lerp(const SSwingValue& A, const SSwingValue& B, float f)
{
	float fi = 1.f - f;
	amp1 = fi * A.amp1 + f * B.amp1;
	amp2 = fi * A.amp2 + f * B.amp2;
	rot1 = fi * A.rot1 + f * B.rot1;
	rot2 = fi * A.rot2 + f * B.rot2;
	speed = fi * A.speed + f * B.speed;
}

//---------------------------------------------------

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CDetailManager::CDetailManager()
{
	dtFS = 0;
	dtSlots = 0;
	soft_Geom = 0;
	hw_Geom = 0;
	hw_BatchSize = 0;
	hw_VB = 0;
	hw_IB = 0;
	m_time_rot_1 = 0;
	m_time_rot_2 = 0;
	m_time_pos = 0;
	m_global_time_old = 0;
	m_frame_calc = -1;
	m_frame_rendered = -1;

#ifdef DETAIL_RADIUS
	// KD: variable detail radius
	dm_size = dm_current_size;
	dm_cache_line = dm_current_cache_line;
	dm_cache1_line = dm_current_cache1_line;
	dm_cache_size = dm_current_cache_size;
	dm_fade = dm_current_fade;
	ps_r__Detail_density = ps_current_detail_density;
	ps_r__Detail_height = ps_current_detail_height;
	cache_level1 = (CacheSlot1**)Memory.mem_alloc(dm_cache1_line * sizeof(CacheSlot1*)
#ifdef USE_MEMORY_MONITOR
        , "CDetailManager::cache_level1"
#endif
	);
	for (u32 i = 0; i < dm_cache1_line; ++i)
	{
		cache_level1[i] = (CacheSlot1*)Memory.mem_alloc(dm_cache1_line * sizeof(CacheSlot1)
#ifdef USE_MEMORY_MONITOR
            , "CDetailManager::cache_level1 " + i
#endif
		);
		for (u32 j = 0; j < dm_cache1_line; ++j)
			new(&(cache_level1[i][j])) CacheSlot1();
	}

	cache = (Slot***)Memory.mem_alloc(dm_cache_line * sizeof(Slot**)
#ifdef USE_MEMORY_MONITOR
        , "CDetailManager::cache"
#endif
	);
	for (u32 i = 0; i < dm_cache_line; ++i)
		cache[i] = (Slot**)Memory.mem_alloc(dm_cache_line * sizeof(Slot*)
#ifdef USE_MEMORY_MONITOR
        , "CDetailManager::cache " + i
#endif
		);

	cache_pool = (Slot *)Memory.mem_alloc(dm_cache_size * sizeof(Slot)
#ifdef USE_MEMORY_MONITOR
        , "CDetailManager::cache_pool"
#endif
	);
	for (u32 i = 0; i < dm_cache_size; ++i)
		new(&(cache_pool[i])) Slot();
	/*
	CacheSlot1 						cache_level1[dm_cache1_line][dm_cache1_line];
	Slot*							cache		[dm_cache_line][dm_cache_line];	// grid-cache itself
	Slot							cache_pool	[dm_cache_size];				// just memory for slots */
#endif
}

CDetailManager::~CDetailManager()
{
	if (dtFS)
	{
		FS.r_close(dtFS);
		dtFS = 0;
	}
#ifdef DETAIL_RADIUS
	for (u32 i = 0; i < dm_cache_size; ++i)
		cache_pool[i].~Slot();
	Memory.mem_free(cache_pool);

	for (u32 i = 0; i < dm_cache_line; ++i)
		Memory.mem_free(cache[i]);
	Memory.mem_free(cache);

	for (u32 i = 0; i < dm_cache1_line; ++i)
	{
		for (u32 j = 0; j < dm_cache1_line; ++j)
			cache_level1[i][j].~CacheSlot1();
		Memory.mem_free(cache_level1[i]);
	}
	Memory.mem_free(cache_level1);
#endif
}

/*
*/
#ifndef _EDITOR

/*
void dump	(CDetailManager::vis_list& lst)
{
	for (int i=0; i<lst.size(); i++)
	{
		Msg("%8x / %8x / %8x",	lst[i]._M_start, lst[i]._M_finish, lst[i]._M_end_of_storage._M_data);
	}
}
*/
void CDetailManager::Load()
{
	// Open directly; FS.r_open() already performs the VFS lookup.
	dtFS = FS.r_open("$level$", "level.details");
	if (!dtFS)
		return;

	// Header
	dtFS->r_chunk_safe(0, &dtH, sizeof(dtH));
	R_ASSERT(dtH.version == DETAIL_VERSION_3 || dtH.version == DETAIL_VERSION_4);
	u32 m_count = dtH.object_count;
	R_ASSERT(m_count <= (u32)dm_max_objects);

	// Models. Iterate the subchunks once instead of repeatedly searching from
	// the parent reader, and reserve the final pointer array up front.
	objects.reserve(m_count);
	IReader* m_fs = dtFS->open_chunk(1);
	R_ASSERT(m_fs);
	u32 model_chunk_id = 0;
	for (IReader* model_reader = m_fs->open_chunk_iterator(model_chunk_id); model_reader;
		model_reader = m_fs->open_chunk_iterator(model_chunk_id, model_reader))
	{
		CDetail* detail = xr_new<CDetail>();
		detail->Load(model_reader);
		objects.push_back(detail);
	}
	R_ASSERT(objects.size() == m_count);
	m_fs->close();

	// Slots: copy into a heap-owned wide (v4) array, expanding v3 slots on the fly.
	u32 slot_count = dtH.size_x * dtH.size_z;
	dtSlots = xr_alloc_uninitialized<DetailSlot>(slot_count);
	IReader* m_slots = dtFS->open_chunk(2);
	R_ASSERT(m_slots);
	if (dtH.version == DETAIL_VERSION_4)
	{
		R_ASSERT(m_slots->length() >= slot_count * sizeof(DetailSlot));
		memcpy(dtSlots, m_slots->pointer(), slot_count * sizeof(DetailSlot));
	}
	else // DETAIL_VERSION_3: 16-byte slots -> expand into 20-byte working slots
	{
		R_ASSERT(m_slots->length() >= slot_count * sizeof(DetailSlot_v3));
		const DetailSlot_v3* source = static_cast<const DetailSlot_v3*>(m_slots->pointer());

		detail_expand_context context{dtSlots, source, slot_count, 0};
		const u32 block_count = (slot_count + detail_expand_block_size - 1) / detail_expand_block_size;
		const bool parallel_expand = block_count > 1 && xr_jobs::worker_count() > 0 &&
			!(Core.Params && strstr(Core.Params, "-no_mt_detail_load"));
		const u32 lane_count = parallel_expand ?
			_min(block_count, xr_jobs::available_thread_count()) : 1;

		xr_jobs::task_group group;
		xr_jobs::submit_many(&expand_detail_slots_job, &context,
			lane_count - 1, &group, xr_jobs::priority::normal);

		expand_detail_slots_job(&context);
		if (lane_count > 1)
			xr_jobs::wait(group);
	}
	m_slots->close();

	// Initialize 'vis' and 'cache'
	for (u32 i = 0; i < 3; ++i) m_visibles[i].resize(objects.size());
	cache_Initialize();

	// Make dither matrix
	bwdithermap(2, dither);

	// Hardware specific optimizations
	if (UseVS()) hw_Load();
	else soft_Load();

	// swing desc
	// normal
	swing_desc[0].amp1 = pSettings->r_float("details", "swing_normal_amp1");
	swing_desc[0].amp2 = pSettings->r_float("details", "swing_normal_amp2");
	swing_desc[0].rot1 = pSettings->r_float("details", "swing_normal_rot1");
	swing_desc[0].rot2 = pSettings->r_float("details", "swing_normal_rot2");
	swing_desc[0].speed = pSettings->r_float("details", "swing_normal_speed");
	// fast
	swing_desc[1].amp1 = pSettings->r_float("details", "swing_fast_amp1");
	swing_desc[1].amp2 = pSettings->r_float("details", "swing_fast_amp2");
	swing_desc[1].rot1 = pSettings->r_float("details", "swing_fast_rot1");
	swing_desc[1].rot2 = pSettings->r_float("details", "swing_fast_rot2");
	swing_desc[1].speed = pSettings->r_float("details", "swing_fast_speed");
}
#endif
void CDetailManager::Unload()
{
	ResetFrameState();
	if (UseVS()) hw_Unload();
	else soft_Unload();

	for (DetailIt it = objects.begin(); it != objects.end(); it++)
	{
		(*it)->Unload();
		xr_delete(*it);
	}
	objects.clear();
	m_visibles[0].clear();
	m_visibles[1].clear();
	m_visibles[2].clear();
	FS.r_close(dtFS);
	dtFS = 0;
	xr_free(dtSlots); // heap-owned wide slot array (was a VFS alias pre-v4)
}

extern ECORE_API float r_ssaDISCARD;

void CDetailManager::UpdateVisibleM()
{
	Fvector EYE = RDEVICE.vCameraPosition_saved;

	CFrustum View;
	View.CreateFromMatrix(RDEVICE.mFullTransform_saved, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);

	float fade_limit = dm_fade;
	fade_limit = fade_limit * fade_limit;
	float fade_start = 1.f;
	fade_start = fade_start * fade_start;
	float fade_range = fade_limit - fade_start;
	float r_ssaCHEAP = 16 * r_ssaDISCARD;
	const bool no_scale = psDeviceFlags2.test(rsNoScale);
	const float alpha_step = RDEVICE.fTimeDelta;

	// Initialize 'vis' and 'cache'
	// Collect objects for rendering
	RDEVICE.Statistic->RenderDUMP_DT_VIS.Begin();
	for (u32 _mz = 0; _mz < dm_cache1_line; _mz++)
	{
		for (u32 _mx = 0; _mx < dm_cache1_line; _mx++)
		{
			CacheSlot1& MS = cache_level1[_mz][_mx];
			if (MS.empty)
			{
				continue;
			}
			u32 mask = 0xff;
			u32 res = View.testSphere(MS.vis.sphere.P, MS.vis.sphere.R, mask);
			if (fcvNone == res)
			{
				continue; // invisible-view frustum
			}
			// test slots

			u32 dwCC = dm_cache1_count * dm_cache1_count;

			for (u32 _i = 0; _i < dwCC; _i++)
			{
				Slot* PS = *MS.slots[_i];
				Slot& S = *PS;

				// if slot empty - continue
				if (S.empty)
				{
					continue;
				}

				// if upper test = fcvPartial - test inner slots
				if (fcvPartial == res)
				{
					u32 _mask = mask;
					u32 _res = View.testSphere(S.vis.sphere.P, S.vis.sphere.R, _mask);
					if (fcvNone == _res)
					{
						continue; // invisible-view frustum
					}
				}
#ifndef _EDITOR
				if (!RImplementation.HOM.visible(S.vis))
				{
					continue; // invisible-occlusion
				}
#endif
				// Add to visibility structures
				if (RDEVICE.dwFrame > S.frame)
				{
					// Calc fade factor	(per slot)
					float dist_sq = EYE.distance_to_sqr(S.vis.sphere.P);
					if (dist_sq > fade_limit)
					{
						S.hidden = true;
						continue;
					}
					float alpha = (dist_sq < fade_start) ? 0.f : (dist_sq - fade_start) / fade_range;
					float alpha_i = 1.f - alpha;
					float dist_sq_rcp = 1.f / dist_sq;
					S.distance = dist_sq;
					S.visibility_scale = no_scale ? 1.f : alpha_i;

					S.frame = RDEVICE.dwFrame + detail_slot_jitter(&S, RDEVICE.dwFrame, 15, 30);
					for (int sp_id = 0; sp_id < dm_obj_in_slot; sp_id++)
					{
						SlotPart& sp = S.G[sp_id];
						if (sp.id == DetailSlot::ID_Empty) continue;

						sp.r_items[0].clear_not_free();
						sp.r_items[1].clear_not_free();
						sp.r_items[2].clear_not_free();
						sp.alpha_fading_mask = 0;

						float R = objects[sp.id]->bv_sphere.R;
						float Rq_drcp = R * R * dist_sq_rcp; // reordered expression for 'ssa' calc

						for (u32 item_index = 0; item_index < sp.items.size(); ++item_index)
						{
							SlotItem& Item = sp.items[item_index];
							float scale = Item.scale * S.visibility_scale;
							float ssa = no_scale ? scale : scale * scale * Rq_drcp;
							if (ssa < r_ssaDISCARD)
							{
								Item.alpha_target = 0;
								continue;
							}
							u32 vis_id = 0;
							if (ssa > r_ssaCHEAP) vis_id = Item.vis_ID;

							sp.r_items[vis_id].push_back(&Item);

							if (S.hidden)
							{
								Item.alpha = 0;
								S.hidden = false;
							}
							Item.alpha_target = 1;
							if (Item.alpha < 1.f)
								sp.alpha_fading_mask |= static_cast<u8>(1u << vis_id);
							//2							visible[vis_id][sp.id].push_back(&Item);
						}
					}
				}
				for (int sp_id = 0; sp_id < dm_obj_in_slot; sp_id++)
				{
					SlotPart& sp = S.G[sp_id];
					if (sp.id == DetailSlot::ID_Empty) continue;
					if (!sp.r_items[0].empty())
					{
						if ((sp.alpha_fading_mask & 1u) &&
							!advance_detail_alpha(sp.r_items[0], alpha_step))
							sp.alpha_fading_mask &= static_cast<u8>(~1u);
						m_visibles[0][sp.id].push_back({&sp.r_items[0], &S});
					}
					if (!sp.r_items[1].empty())
					{
						if ((sp.alpha_fading_mask & 2u) &&
							!advance_detail_alpha(sp.r_items[1], alpha_step))
							sp.alpha_fading_mask &= static_cast<u8>(~2u);
						m_visibles[1][sp.id].push_back({&sp.r_items[1], &S});
					}
					if (!sp.r_items[2].empty())
					{
						if ((sp.alpha_fading_mask & 4u) &&
							!advance_detail_alpha(sp.r_items[2], alpha_step))
							sp.alpha_fading_mask &= static_cast<u8>(~4u);
						m_visibles[2][sp.id].push_back({&sp.r_items[2], &S});
					}
				}
			}
		}
	}
	RDEVICE.Statistic->RenderDUMP_DT_VIS.End();
}

void CDetailManager::prepare_frame_state()
{
	const u32 current_frame = RDEVICE.dwFrame;
	if (m_frame_state.frame == current_frame)
	{
		return;
	}

#ifndef _EDITOR
	const float factor = g_pGamePersistent->Environment().wind_strength_factor;
#else
	const float factor = 0.3f;
#endif
	swing_current.lerp(swing_desc[0], swing_desc[1], factor);

	float fDelta = Device.fTimeGlobal - m_global_time_old;
	if ((fDelta < 0) || (fDelta > 1))
		fDelta = 0.03f;
	m_global_time_old = Device.fTimeGlobal;

	m_time_rot_1 += (PI_MUL_2 * fDelta / swing_current.rot1);
	m_time_rot_2 += (PI_MUL_2 * fDelta / swing_current.rot2);
	m_time_pos += fDelta * swing_current.speed;

	const float tm_rot1 = m_time_rot_1;
	const float tm_rot2 = m_time_rot_2;
	Fvector4 dir1;
	Fvector4 dir2;
	dir1.set(_sin(tm_rot1), 0, _cos(tm_rot1), 0).normalize().mul(swing_current.amp1);
	dir2.set(_sin(tm_rot2), 0, _cos(tm_rot2), 0).normalize().mul(swing_current.amp2);

	const float scale = 1.f / 16384.f;
	m_frame_state.animated_consts.set(scale, scale, ps_r__Detail_l_aniso, ps_r__Detail_l_ambient);
	m_frame_state.still_consts.set(scale, scale, scale, 1.f);
	m_frame_state.wave[0].set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, m_time_pos);
	m_frame_state.wave[1].set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos);
	m_frame_state.previous_wave[0].set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, m_previous_time);
	m_frame_state.previous_wave[1].set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_previous_time);
	m_frame_state.wave[0].div(PI_MUL_2);
	m_frame_state.wave[1].div(PI_MUL_2);
	m_frame_state.previous_wave[0].div(PI_MUL_2);
	m_frame_state.previous_wave[1].div(PI_MUL_2);
	m_frame_state.wind[0].set(dir1);
	m_frame_state.wind[1].set(dir2);
	m_frame_state.previous_wind[0].set(m_previous_dir1);
	m_frame_state.previous_wind[1].set(m_previous_dir2);

	m_frame_state.bender_count = 0;
	m_frame_state.current_benders = 0;
	m_frame_state.previous_benders = 0;
	if (g_pGamePersistent)
	{
		g_pGamePersistent->PrepareGrassFrameState();
		const IGame_Persistent::grass_data& grass_data = g_pGamePersistent->grass_shader_data;
		m_frame_state.bender_count = grass_data.render_bender_count;
		if (m_frame_state.bender_count)
		{
			m_frame_state.current_benders = grass_data.render_benders;
			m_frame_state.previous_benders = grass_data.render_previous_benders;
		}
	}

	m_previous_time = m_time_pos;
	m_previous_dir1.set(dir1);
	m_previous_dir2.set(dir2);
	m_frame_state.frame = current_frame;

}

void CDetailManager::prepare_spot_candidates()
{
	u32 generation = ++m_grass_spot_candidate_generation;
	if (!generation)
	{
		for (u32 z = 0; z < dm_cache_line; ++z)
			for (u32 x = 0; x < dm_cache_line; ++x)
				cache[z][x]->grass_spot_candidate_generation = 0;
		generation = ++m_grass_spot_candidate_generation;
	}

	// The existing light fade becomes zero at sqrt(200) XZ units. Add two
	// cells so the grid broad phase remains conservative; the original exact
	// slot fade test still decides whether anything is rendered.
	const float influence_radius = 14.14213562f + dm_slot_size * 2.f;
	const int min_sx = iFloor((light_position.x - influence_radius) / dm_slot_size);
	const int max_sx = iFloor((light_position.x + influence_radius) / dm_slot_size);
	const int min_sz = iFloor((light_position.z - influence_radius) / dm_slot_size);
	const int max_sz = iFloor((light_position.z + influence_radius) / dm_slot_size);

	for (int sx = min_sx; sx <= max_sx; ++sx)
	{
		const int gx = w2cg_X(sx);
		if (gx < 0 || gx >= int(dm_cache_line))
			continue;
		for (int sz = min_sz; sz <= max_sz; ++sz)
		{
			const int gz = w2cg_Z(sz);
			if (gz < 0 || gz >= int(dm_cache_line))
				continue;

			Slot* const slot = cache[gz][gx];
			if (!slot->empty && slot->type == stReady)
				slot->grass_spot_candidate_generation = generation;
		}
	}
}

void CDetailManager::PrepareSunShadowCascades(const CFrustum* const* frustums, u32 count)
{
	const u32 cascade_count = _min(count, grass_sun_cascade_capacity);
	m_grass_sun_cascade_count = cascade_count;
	m_grass_sun_cascade_index = u32(-1);

	for (u32 i = 0; i < grass_sun_cascade_capacity; ++i)
		m_grass_sun_cascade_frustums[i] = i < cascade_count ? frustums[i] : 0;

	// A new prepared sun volume means the old per-slot masks are stale. The slot
	// data itself is kept in the detail cache; a generation tag avoids touching
	// every cached slot here and computes each mask lazily only if that slot is
	// actually referenced by a visible detail list.
	++m_grass_sun_cascade_generation;
	if (m_grass_sun_cascade_generation == 0)
	{
		m_grass_sun_cascade_generation = 1;
		if (dtFS)
		{
			for (u32 z = 0; z < dm_cache_line; ++z)
				for (u32 x = 0; x < dm_cache_line; ++x)
					if (Slot* slot = cache[z][x])
						slot->grass_sun_cascade_generation = 0;
		}
	}

	// Build a tiny model-level mask once. Slot masks remain the authoritative
	// filter; this OR only avoids entering shader/pass setup when every slot of a
	// detail model is outside the current cascade.
	for (u32 variant = 0; variant < 3; ++variant)
	{
		xr_vector<u8>& object_masks = m_grass_sun_object_masks[variant];
		object_masks.assign(objects.size(), 0);
		vis_list& visible = m_visibles[variant];
		const u32 model_count = _min(static_cast<u32>(visible.size()), static_cast<u32>(object_masks.size()));
		for (u32 object_index = 0; object_index < model_count; ++object_index)
		{
			u32 object_mask = 0;
			xr_vector<VisibleSlotItems>& slots = visible[object_index];
			for (u32 slot_index = 0; slot_index < slots.size(); ++slot_index)
			{
				object_mask |= sun_shadow_mask_for_slot(*slots[slot_index].slot);
				if (object_mask == ((1u << cascade_count) - 1u))
					break;
			}
			object_masks[object_index] = static_cast<u8>(object_mask);
		}
	}
}

void CDetailManager::SetSunShadowCascade(u32 cascade_index)
{
	m_grass_sun_cascade_index = cascade_index < m_grass_sun_cascade_count ? cascade_index : u32(-1);
}

void CDetailManager::ClearSunShadowCascades()
{
	m_grass_sun_cascade_count = 0;
	m_grass_sun_cascade_index = u32(-1);
	for (u32 i = 0; i < grass_sun_cascade_capacity; ++i)
		m_grass_sun_cascade_frustums[i] = 0;
}

u32 CDetailManager::sun_shadow_mask_for_slot(Slot& slot)
{
	if (!m_grass_sun_cascade_count)
		return 0xffffffffu;

	if (slot.grass_sun_cascade_generation == m_grass_sun_cascade_generation)
		return slot.grass_sun_cascade_mask;

	u32 mask = 0;
	for (u32 cascade = 0; cascade < m_grass_sun_cascade_count; ++cascade)
	{
		const CFrustum* const frustum = m_grass_sun_cascade_frustums[cascade];
		if (frustum && frustum->testSphere_dirty(slot.vis.sphere.P, slot.vis.sphere.R))
			mask |= 1u << cascade;
	}

	slot.grass_sun_cascade_mask = mask;
	slot.grass_sun_cascade_generation = m_grass_sun_cascade_generation;
	return mask;
}

void CDetailManager::Render()
{
#ifndef _EDITOR
	if (0 == dtFS) return;
	if (!psDeviceFlags.is(rsDetails)) return;
#endif

	// MT
	MT_SYNC();

	RDEVICE.Statistic->RenderDUMP_DT_Render.Begin();
	g_pGamePersistent->m_pGShaderConstants->m_blender_mode.w = 1.0f; //--#SM+#-- Флaa нaчaлa ?aндa?a o?aвu [begin of grass render]
	prepare_frame_state();

	const bool spot_shadow_pass = fade_distance <= -1.f;

	if (UseVS() && spot_shadow_pass)
		prepare_spot_candidates();
	++m_grass_render_fade_generation;

	RCache.set_CullMode(CULL_NONE);
	RCache.set_xform_world(Fidentity);
	if (UseVS()) hw_Render();
	else soft_Render();
	RCache.set_CullMode(CULL_CCW);

	g_pGamePersistent->m_pGShaderConstants->m_blender_mode.w = 0.0f; //--#SM+#-- Флaa eонцa ?aндa?a o?aвu [end of grass render]	

	RDEVICE.Statistic->RenderDUMP_DT_Render.End();
	InterlockedExchange(&m_frame_rendered, static_cast<LONG>(RDEVICE.dwFrame));
}

void CDetailManager::ResetFrameState()
{
	m_frame_state.frame = u32(-1);
	m_grass_spot_candidate_generation = 0;
	ClearSunShadowCascades();
	// A level is allowed to have no level.details. In that case Load() returns
	// before cache_Initialize() and the cache entries have no valid Slot objects.
	// Keep the per-manager state reset, but only touch slots owned by a loaded
	// details database. The null check also makes a partial teardown idempotent.
	if (dtFS)
	{
		for (u32 z = 0; z < dm_cache_line; ++z)
			for (u32 x = 0; x < dm_cache_line; ++x)
				if (Slot* slot = cache[z][x])
					slot->grass_spot_candidate_generation = 0;
	}
}

void __stdcall CDetailManager::MT_CALC()
{
	if (!this || !MT.IsValid()) return; // DIIIRTY HACK !!!

#ifndef _EDITOR
	if (0 == RImplementation.Details) return; // possibly deleted
	if (0 == dtFS) return;
	if (!psDeviceFlags.is(rsDetails)) return;
#endif

	MT.Enter();
	const u32 current_frame = RDEVICE.dwFrame;
	const u32 calculated_frame = static_cast<u32>(InterlockedCompareExchange(&m_frame_calc, 0, 0));
	if (calculated_frame != current_frame)
	{
		const u32 rendered_frame = static_cast<u32>(InterlockedCompareExchange(&m_frame_rendered, 0, 0));
		if ((rendered_frame + 1) == current_frame) //already rendered
		{
			Fvector EYE = RDEVICE.vCameraPosition_saved;

			int s_x = iFloor(EYE.x / dm_slot_size + .5f);
			int s_z = iFloor(EYE.z / dm_slot_size + .5f);

			RDEVICE.Statistic->RenderDUMP_DT_Cache.Begin();
			cache_Update(s_x, s_z, EYE, dm_max_decompress);
			RDEVICE.Statistic->RenderDUMP_DT_Cache.End();

			UpdateVisibleM();
			InterlockedExchange(&m_frame_calc, static_cast<LONG>(current_frame));
		}
	}
	MT.Leave();
}

void CDetailManager::details_clear()
{

	// Disable fade, next render will be scene
	fade_distance = 99999;

	if (ps_ssfx_grass_shadows.x <= 0)
		return;

	for (u32 x = 0; x < 3; x++)
	{
		vis_list& list = m_visibles[x];

		for (u32 O = 0; O < objects.size(); O++)
		{
			CDetail& Object = *objects[O];
			xr_vector<VisibleSlotItems>& vis = list[O];
			if (!vis.empty())
			{
				vis.clear_not_free();
			}
		}
	}
}
