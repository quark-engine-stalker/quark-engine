#include "stdafx.h"
#include "../../xrCore/profiler.h"
#include "../../xrEngine/frame_stats.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/xr_object.h"

IC bool pred_area(light* _1, light* _2)
{
	u32 a0 = _1->X.S.size;
	u32 a1 = _2->X.S.size;
	return a0 > a1; // reverse -> descending
}

bool check_grass_shadow(light* L, const CFrustum& VB)
{
	// Grass shadows are allowed?
	if (ps_ssfx_grass_shadows.x < 3 || !psDeviceFlags2.test(rsGrassShadow))
		return false;

	// Inside the range?
	if (L->vis.distance > ps_ssfx_grass_shadows.z)
		return false;

	// Is in view? L->vis.visible?
	u32 mask = 0xff;
	if (!VB.testSphere(L->position, L->range * 0.6f, mask))
		return false;

	return true;
}

void CRender::smap_cache_release(light* L)
{
	light::_smap_cache& C = L->smap_cache;
	for (u32 y = 0; y < C.cells; ++y)
		for (u32 x = 0; x < C.cells; ++x)
			m_smapCacheOwner[C.cell + y * SMAP_CACHE_GRID + x] = nullptr;
	C.cells = 0;
	C.valid = false;
}

void CRender::smap_cache_invalidate()
{
	for (light* owner : m_smapCacheOwner)
		if (owner)
			owner->smap_cache.valid = false;
}

// Gives a face that holds still a slot of the cache page, keeping the one it already
// has. Returns false when the face is drawn the usual way this frame.
bool CRender::smap_cache_acquire(light* L)
{
	light::_smap_cache& C = L->smap_cache;
	if (C.size != L->X.S.size || memcmp(&C.combine, &L->X.S.combine, sizeof(Fmatrix)))
	{
		// A moving or resizing face would re-render its slot every frame.
		g_frame_stats.count(L->flags.type == IRender_Light::OMNIPART ? fcLightCacheMovedPoint : fcLightCacheMovedSpot, 1);
		C.size = L->X.S.size;
		C.combine = L->X.S.combine;
		C.valid = false;
		C.uncacheable = false;
		return false;
	}
	if (C.uncacheable || L->get_hud_mode())
	{
		g_frame_stats.count(C.uncacheable ? fcLightCacheVolatile : fcLightCacheNoSlot, 1);
		smap_cache_release(L);
		return false;
	}

	const u32 rows = o.smapsize / SMAP_CACHE_CELL;
	const u32 columns = 2 * rows;
	const u32 cells = (L->X.S.size + SMAP_CACHE_CELL - 1) / SMAP_CACHE_CELL;
	if (C.cells != cells)
	{
		smap_cache_release(L);

		// Prefer free cells, then the slots unused for the longest time; never evict
		// a slot that served this or the previous frame: when the page is full, faces
		// would evict each other and re-render their slots every frame.
		u32 best = u32(-1);
		u32 best_age = u32(-1);
		for (u32 y = 0; best_age && y + cells <= rows; ++y)
			for (u32 x = 0; best_age && x + cells <= columns; ++x)
			{
				u32 youngest = 0; // 0 = all cells free
				bool usable = true;
				for (u32 cy = y; usable && cy < y + cells; ++cy)
					for (u32 cx = x; cx < x + cells; ++cx)
					{
						const light* owner = m_smapCacheOwner[cy * SMAP_CACHE_GRID + cx];
						if (!owner)
							continue;
						if (owner->smap_cache.used_frame + 1 >= Device.dwFrame)
						{
							usable = false;
							break;
						}
						youngest = _max(youngest, owner->smap_cache.used_frame + 1);
					}
				if (usable && youngest < best_age)
				{
					best = y * SMAP_CACHE_GRID + x;
					best_age = youngest;
				}
			}
		if (best == u32(-1))
		{
			g_frame_stats.count(fcLightCacheNoSlot, 1);
			return false;
		}

		for (u32 y = 0; y < cells; ++y)
			for (u32 x = 0; x < cells; ++x)
			{
				light*& owner = m_smapCacheOwner[best + y * SMAP_CACHE_GRID + x];
				if (owner)
					smap_cache_release(owner);
				owner = L;
			}
		C.cell = u16(best);
		C.cells = u8(cells);
		C.x = u16(best % SMAP_CACHE_GRID * SMAP_CACHE_CELL);
		C.y = u16(best / SMAP_CACHE_GRID * SMAP_CACHE_CELL);
	}
	C.used_frame = Device.dwFrame;
	return true;
}

// build_cached_shadow() will draw the face from its slot without walking its static
// casters. Not exact only when an earlier face evicts the slot this frame; that face
// then walks them in render_shadow_face() itself.
bool CRender::smap_cache_reusable(light* L)
{
	const light::_smap_cache& C = L->smap_cache;
	if (!C.valid || C.uncacheable || L->get_hud_mode())
		return false;
	if (C.size != L->X.S.size || memcmp(&C.combine, &L->X.S.combine, sizeof(Fmatrix)))
		return false;
	if (C.cells != (L->X.S.size + SMAP_CACHE_CELL - 1) / SMAP_CACHE_CELL)
		return false;
	const float camera_distance = Device.vCameraPosition.distance_to(L->position);
	return !(_abs(camera_distance - C.camera_distance) > 0.15f * C.camera_distance);
}

// The portal traversal of every face runs here, its results are copied, and job workers
// walk the static casters and query the renderables of all faces at once. The draws then
// replay them in render_shadow_face(); dsgraph and game code stay on this thread.
void CRender::prepare_shadow_faces(xr_vector<light*>& faces, bool shadow_cache)
{
	m_shadowFacesUsed = static_cast<u32>(faces.size());
	if (m_shadowFaces.size() < faces.size())
		m_shadowFaces.resize(faces.size());
	if (faces.empty())
		return;

	// The walk reads the phase; the face loop sets the same one.
	phase = PHASE_SMAP;
	for (u32 i = 0; i < faces.size(); ++i)
	{
		light* L = faces[i];
		ShadowFace& face = m_shadowFaces[i];
		face.frustums.clear_not_free();
		face.sectors.clear_not_free();
		face.statics.clear_not_free();
		face.dynamics.clear_not_free();

		IRender_Sector* sector = L->spatial.sector;
		if (!sector)
		{
			L->spatial.type |= STYPEFLAG_INVALIDSECTOR;
			L->spatial_updatesector();
			sector = L->spatial.sector;
		}
		if (!sector)
			sector = detectSector(L->position);

		face.L = L;
		face.sector = sector;
		face.with_static = !(shadow_cache && sector && smap_cache_reusable(L));
		if (!sector)
			continue;

		face.view.CreateFromMatrix(L->X.S.combine, FRUSTUM_P_ALL & (~FRUSTUM_P_NEAR));
		PortalTraverser.traverse(sector, face.view, L->position, L->X.S.combine, 0);
		for (IRender_Sector* traversed : PortalTraverser.r_sectors)
		{
			CSector* S = (CSector*)traversed;
			if (!S || S->r_frustums.empty())
				continue;
			ShadowFaceSector record = {S, static_cast<u32>(face.frustums.size()), static_cast<u32>(S->r_frustums.size())};
			face.sectors.push_back(record);
			face.frustums.insert(face.frustums.end(), S->r_frustums.begin(), S->r_frustums.end());
		}
	}

	run_parallel(m_shadowFacesUsed, &CRender::gather_shadow_face);
}

// Job worker: the part of r_dsgraph_render_subspace() that only reads level data.
void CRender::gather_shadow_face(u32 index)
{
	ShadowFace& face = m_shadowFaces[index];
	if (!face.sector)
		return;

	if (face.with_static)
	{
		for (const ShadowFaceSector& S : face.sectors)
		{
			dxRender_Visual* root = S.sector->root();
			if (!root)
				continue;
			for (u32 f = S.first; f < S.first + S.count; ++f)
				record_Static(root, face.frustums[f], face.frustums[f].getMask(), face.statics);
		}
	}

	g_SpatialSpace->q_frustum(face.spatials, ISpatial_DB::O_ORDERED, STYPE_RENDERABLE, face.view);
	for (ISpatial* spatial : face.spatials)
	{
		if (!spatial)
			continue;
		CSector* sector = (CSector*)spatial->spatial.sector;
		if (!sector)
			continue;
		for (const ShadowFaceSector& S : face.sectors)
		{
			if (S.sector != sector)
				continue;
			for (u32 f = S.first; f < S.first + S.count; ++f)
			{
				if (!face.frustums[f].testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
					continue;
				ShadowFaceDynamic item = {spatial, &face.frustums[f]};
				face.dynamics.push_back(item);
			}
			break;
		}
	}
}

// r_dsgraph_render_subspace() of a prepared face, with the same dsgraph insertions in
// the same order.
void CRender::render_shadow_face(ShadowFace& face, BOOL _dynamic, BOOL _static)
{
	if (!face.sector)
		return;

	if (_static && !face.with_static)
	{
		r_dsgraph_render_subspace(face.sector, face.L->X.S.combine, face.L->position, _dynamic, FALSE, _static);
		return;
	}

	marker++;
	CFrustum ViewSave = ViewBase;
	ViewBase = face.view;
	View = &ViewBase;

	if (_static)
		replay_Static(face.statics);

	if (_dynamic)
	{
		set_Object(0);
		for (const ShadowFaceDynamic& item : face.dynamics)
		{
			// Large destroy queues are committed over several frames. Their
			// objects must not keep casting shadows while awaiting net_Destroy.
			CObject* object = item.spatial->dcast_CObject();
			if (object && object->getDestroy())
				continue;
			IRenderable* renderable = item.spatial->dcast_Renderable();
			if (!renderable)
				continue;
			set_Frustum(const_cast<CFrustum*>(item.view));
			renderable->renderable_Render();
		}
	}

	ViewBase = ViewSave;
	View = 0;
}

// Builds the dsgraph of a face that owns a cache slot. Its static casters are drawn into
// the slot only when they may have changed; the graph gets the dynamic casters and trees
// that are drawn over them every frame. Returns true when the atlas slot has to receive
// the static casters from the cache slot first. svis_open: svis.end() is still owed.
bool CRender::build_cached_shadow(ShadowFace& face, bool& svis_open)
{
	light* L = face.L;
	light::_smap_cache& C = L->smap_cache;
	// Static casters are culled and LOD-ed by camera distance; bound how far that
	// drifts from the cached render.
	const float camera_distance = Device.vCameraPosition.distance_to(L->position);
	if (!C.valid || _abs(camera_distance - C.camera_distance) > 0.15f * C.camera_distance)
	{
		Lights_LastFrame.push_back(L);
		L->svis.begin();
		svis_open = true;
		C.trees.clear();
		ShadowStatic record = {&C.trees};
		{
			SFrameScope stats_traverse_scope(fsLightTraverse);
			val_shadow_static = &record;
			render_shadow_face(face, FALSE, TRUE);
			val_shadow_static = nullptr;
		}
		if (record.volatile_content || mapNormalPasses[1][0].size() || mapMatrixPasses[1][0].size() || mapSorted.size())
		{
			// Translucent or self-changing static casters: the face stays in the graph
			// and is drawn the usual way, now and until it changes.
			C.uncacheable = true;
		}
		else
		{
			C.static_casters = mapNormalPasses[0][0].size() || mapMatrixPasses[0][0].size();
			if (C.static_casters)
			{
				SFrameScope stats_draw_scope(fsLightSmapDraw);
				Target->phase_smap_cache(L);
				RCache.set_xform_view(L->X.S.view);
				RCache.set_xform_project(L->X.S.project);
				r_dsgraph_render_graph(0);
			}
			L->svis.end(); // its occlusion test draws into the bound slot
			svis_open = false;
			C.valid = true;
			C.camera_distance = camera_distance;
			g_frame_stats.count(fcLightCacheRendered, 1);
		}
	}
	else
		g_frame_stats.count(fcLightCacheReused, 1);

	SFrameScope stats_traverse_scope(fsLightTraverse);
	render_shadow_face(face, TRUE, FALSE);
	for (dxRender_Visual* tree : C.trees)
		r_dsgraph_insert_static(tree);
	return C.valid && C.static_casters;
}

void CRender::render_lights(light_Package& LP)
{

	RDEVICE.Statistic->RenderDUMP_Lights.Begin();
	SFrameScope stats_setup_scope(fsLightSetup);
	m_hudLightStateScratch.clear_and_reserve();

	//////////////////////////////////////////////////////////////////////////
	// 0. apply hud_mode projection if necessary. The legacy map allocated one
	// tree node per HUD light on every call; a renderer-owned vector is cheaper
	// and keeps only the normal working set between frames.
	auto applyHudLights = [this](xr_vector<light*>& source)
	{
		for (u32 it = 0; it < source.size(); ++it)
		{
			light* L = source[it];
			if (!L->get_hud_mode())
				continue;

			bool saved = false;
			for (const HudLightState& state : m_hudLightStateScratch)
			{
				if (state.source == L)
				{
					saved = true;
					break;
				}
			}

			if (!saved)
			{
				HudLightState state;
				state.source = L;
				state.position = L->position;
				state.direction = L->direction;
				m_hudLightStateScratch.push_back(state);
			}

			Device.hud_to_world(L->position);
			Device.hud_to_world_dir(L->direction);
		}
	};

	{

		applyHudLights(LP.v_shadowed);
		applyHudLights(LP.v_point);
		applyHudLights(LP.v_spot);
	}

	g_frame_stats.count(fcLightFacesIn, LP.v_shadowed.size());

	RDEVICE.Statistic->RenderDUMP_Scalc.Begin();

	// Refactor order based on ability to pack shadow-maps
	// 1. calculate area + sort in descending order
	// const	u16		smap_unassigned		= u16(-1);
	{

		xr_vector<light*>& source = LP.v_shadowed;
		u32 visible_count = 0;
		for (u32 it = 0; it < source.size(); ++it)
		{
			light* L = source[it];
			L->vis_update();
			if (!L->vis.visible)
				continue;

			LR.compute_xf_spot(L);
			// A point light exports six shadow faces. Skip a face only when a
			// sphere containing its entire contribution misses the camera view.
			// GI may bounce from an offscreen face, so keep all faces with GI.
			if (L->flags.type == IRender_Light::OMNIPART && !ps_r2_ls_flags.test(R2FLAG_GI))
			{
				Fvector face_center;
				float face_radius;
				if (RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS) &&
					(L->flags.bVolumetric || ps_ssfx_volumetric.x > 0.f))
				{
					// Volumetric slices use the shadow projection, including its
					// widened edges; bound the full pyramid rather than the light mesh.
					const float far_distance = L->range + EPS_S;
					const float half_depth = far_distance * 0.5f;
					const float half_width = far_distance / _abs(L->X.S.project._11);
					const float half_height = far_distance / _abs(L->X.S.project._22);
					face_center.mad(L->position, L->direction, half_depth);
					face_radius = _sqrt(_sqr(half_width) + _sqr(half_height) + _sqr(half_depth));
				}
				else
				{
					// The OMNIPART sphere-part mesh fits in this smaller bound.
					face_center.mad(L->position, L->direction, L->range * 0.5f);
					face_radius = L->range;
				}
				if (!ViewBase.testSphere_dirty(face_center, face_radius * 1.01f + EPS_S))
					continue;
			}

			source[visible_count++] = L;
		}
		source.resize(visible_count);
		g_frame_stats.count(fcLightFacesVisible, visible_count);

		// Keep the authoritative portal query immediately before each shadow draw.
	}

	// 2. refactor - infact we could go from the backside and sort in ascending order
	{

		xr_vector<light*>& source = LP.v_shadowed;
		xr_vector<light*>& refactored = m_shadowRefactorScratch;
		refactored.clear_and_reserve();
		if (refactored.capacity() < source.size())
			refactored.reserve(source.size());

		// The failed lights are compacted in their current traversal order. With
		// unique areas that order is already strictly descending after the first
		// pass, so sorting again for every subsequent atlas page is redundant. If
		// equal areas exist, retain the legacy repeated sort because pred_area has
		// no tie-break and std::sort is not stable for equivalent elements.
		std::sort(source.begin(), source.end(), pred_area);
		bool has_equal_areas = false;
		for (u32 index = 1; index < source.size(); ++index)
		{
			if (source[index - 1]->X.S.size == source[index]->X.S.size)
			{
				has_equal_areas = true;
				break;
			}
		}

		for (u16 smap_ID = 0; !source.empty(); ++smap_ID)
		{
			LP_smap_pool.initialize(RImplementation.o.smapsize);
			g_frame_stats.count(fcLightAtlasPages, 1);
			if (has_equal_areas && smap_ID != 0)
				std::sort(source.begin(), source.end(), pred_area);
			const u32 source_count = static_cast<u32>(source.size());
			u32 remaining_count = 0;
			for (u32 test = 0; test < source_count; ++test)
			{
				light* L = source[test];
				SMAP_Rect R;
				if (LP_smap_pool.push(R, L->X.S.size))
				{
					// OK
					L->X.S.posX = R.min.x;
					L->X.S.posY = R.min.y;
					L->vis.smap_ID = smap_ID;
					refactored.push_back(L);
				}
				else
				{
					// Stable in-place compaction preserves the exact retry order of the
					// erase-based packer without its quadratic pointer shifting.
					source[remaining_count++] = L;
				}
			}
			source.resize(remaining_count);
		}

		// save (lights are popped from back)
		std::reverse(refactored.begin(), refactored.end());
		LP.v_shadowed = refactored;
	}

	RDEVICE.Statistic->RenderDUMP_Scalc.End();
	stats_setup_scope.stop();

	PIX_EVENT(SHADOWED_LIGHTS);

	//////////////////////////////////////////////////////////////////////////
	// sort lights by importance???
	// while (has_any_lights_that_cast_shadows) {
	//		if (has_point_shadowed)		->	generate point shadowmap
	//		if (has_spot_shadowed)		->	generate spot shadowmap
	//		switch-to-accumulator
	//		if (has_point_unshadowed)	-> 	accum point unshadowed
	//		if (has_spot_unshadowed)	-> 	accum spot unshadowed
	//		if (was_point_shadowed)		->	accum point shadowed
	//		if (was_spot_shadowed)		->	accum spot shadowed
	//	}
	//	if (left_some_lights_that_doesn't cast shadows)
	//		accumulate them
	HOM.Disable();
	m_shadowSpotScratch.clear_and_reserve();
	const bool shadow_cache = ps_r__light_shadow_cache && Target->rt_smap_cache._get() && Target->rt_smap_cache->pZRT;
	{
		SFrameScope stats_traverse_scope(fsLightTraverse);
		prepare_shadow_faces(LP.v_shadowed, shadow_cache);
	}
	while (LP.v_shadowed.size())
	{
		// if (has_spot_shadowed)
		xr_vector<light*>& L_spot_s = m_shadowSpotScratch;
		L_spot_s.clear_not_free();
		stats.s_used ++;

		// Clear the atlas only when a light actually submits shadow casters.
		// Empty pages are never sampled by the accumulation pass.
		bool smap_page_has_casters = false;
		xr_vector<light*>& source = LP.v_shadowed;
		light* L = source.back();
		u16 sid = L->vis.smap_ID;
		while (true)
		{
			if (source.empty()) break;
			L = source.back();
			if (L->vis.smap_ID != sid) break;
			source.pop_back();

			// render
			RDEVICE.Statistic->RenderDUMP_Srender.Begin();
			phase = PHASE_SMAP;
			if (RImplementation.o.Tshadows) r_pmask(true, true);
			else r_pmask(true, false);
			PIX_EVENT(SHADOWED_LIGHTS_RENDER_SUBSPACE);

			// Faces are popped from the back: this one was prepared at the index left.
			ShadowFace& face = m_shadowFaces[source.size()];
			VERIFY(face.L == L);

			// Build the shadow dsgraph in the original light order.
			bool svis_open = false;
			bool cached_static = false;
			if (shadow_cache && face.sector && smap_cache_acquire(L))
				cached_static = build_cached_shadow(face, svis_open);
			else
			{
				Lights_LastFrame.push_back(L);
				L->svis.begin();
				svis_open = true;
				SFrameScope stats_traverse_scope(fsLightTraverse);
				render_shadow_face(face, TRUE, TRUE);
			}

			bool bNormal = mapNormalPasses[0][0].size() || mapMatrixPasses[0][0].size();
			bool bSpecial = mapNormalPasses[1][0].size() || mapMatrixPasses[1][0].size() || mapSorted.size();
			{
				SFrameScope stats_draw_scope(fsLightSmapDraw);
				const u32 stats_calls_before = RCache.stat.calls;
				const u32 stats_polys_before = RCache.stat.polys;

				if (bNormal || bSpecial || cached_static)
				{
					g_frame_stats.count(fcLightMaps, 1);
					{

						if (!smap_page_has_casters)
						{
							Target->phase_smap_spot_clear();
							smap_page_has_casters = true;
						}
						stats.s_merged ++;
						L_spot_s.push_back(L);
						Target->phase_smap_spot(L);
						if (cached_static)
							Target->smap_cache_blit(L);
						RCache.set_xform_view(L->X.S.view);
						RCache.set_xform_project(L->X.S.project);
					}
					{

						r_dsgraph_render_graph(0);
					}
					if (Details && check_grass_shadow(L, ViewBase))
					{

						Details->fade_distance = -1; // Use light position to calc "fade"
						Details->light_position.set(L->position);
						Details->Render();
					}
					L->X.S.transluent = FALSE;
					if (bSpecial)
					{
						L->X.S.transluent = TRUE;
						Target->phase_smap_spot_tsh(L);
						PIX_EVENT(SHADOWED_LIGHTS_RENDER_GRAPH);
						{

							r_dsgraph_render_graph(1); // normal level, secondary priority
						}
						PIX_EVENT(SHADOWED_LIGHTS_RENDER_SORTED);
						{

							r_dsgraph_render_sorted(); // strict-sorted geoms
						}
					}
				}
				else
				{
					stats.s_finalclip ++;
					g_frame_stats.count(fcLightEmpty, 1);
				}
				g_frame_stats.count(fcLightSmapCalls, RCache.stat.calls - stats_calls_before);
				g_frame_stats.count(fcLightSmapPolys, RCache.stat.polys - stats_polys_before);
			}
			if (svis_open)
				L->svis.end();
			r_pmask(true, false);
			RDEVICE.Statistic->RenderDUMP_Srender.End();
		}

		PIX_EVENT(UNSHADOWED_LIGHTS);
		SFrameScope stats_accum_scope(fsLightAccum);

		//		switch-to-accumulator
		Target->phase_accumulator();
		HOM.Disable();

		PIX_EVENT(POINT_LIGHTS);

		//		if (has_point_unshadowed)	-> 	accum point unshadowed
		if (!LP.v_point.empty())
		{
			light* L = LP.v_point.back();

			LP.v_point.pop_back();
			L->vis_update();
			if (L->vis.visible)
			{
				g_frame_stats.count(fcLightAccumPoint, 1);
				Target->accum_point(L);
				render_indirect(L);
			}
		}

		PIX_EVENT(SPOT_LIGHTS);

		//		if (has_spot_unshadowed)	-> 	accum spot unshadowed
		if (!LP.v_spot.empty())
		{
			light* L = LP.v_spot.back();

			LP.v_spot.pop_back();
			L->vis_update();
			if (L->vis.visible)
			{
				g_frame_stats.count(fcLightAccumSpot, 1);
				LR.compute_xf_spot(L);
				Target->accum_spot(L);
				render_indirect(L);
			}
		}

		PIX_EVENT(SPOT_LIGHTS_ACCUM_VOLUMETRIC);

		//		if (was_spot_shadowed)		->	accum spot shadowed
		if (!L_spot_s.empty())
		{
			PIX_EVENT(ACCUM_SPOT);
			PIX_EVENT(ACCUM_VOLUMETRIC);
			g_frame_stats.count(fcLightAccumShadow, L_spot_s.size());
			for (u32 it = 0; it < L_spot_s.size(); it++)
			{

				Target->accum_spot(L_spot_s[it]);
				render_indirect(L_spot_s[it]);

				if (RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
				{
					// Current Resolution
					float w = float(Device.dwWidth);
					float h = float(Device.dwHeight);

					// Adjust resolution
					if (RImplementation.o.ssfx_volumetric)
						Target->set_viewport_size(HW.pContext, w / 8, h / 8);

					Target->accum_volumetric(L_spot_s[it]);

					// Restore resolution
					if (RImplementation.o.ssfx_volumetric)
						Target->set_viewport_size(HW.pContext, w, h);
				}
			}

		}
	}

	PIX_EVENT(POINT_LIGHTS_ACCUM);
	SFrameScope stats_accum_tail_scope(fsLightAccum);
	// Point lighting (unshadowed, if left)
	if (!LP.v_point.empty())
	{
		xr_vector<light*>& Lvec = LP.v_point;
		for (u32 pid = 0; pid < Lvec.size(); pid++)
		{

			Lvec[pid]->vis_update();
			if (Lvec[pid]->vis.visible)
			{
				g_frame_stats.count(fcLightAccumPoint, 1);
				render_indirect(Lvec[pid]);
				Target->accum_point(Lvec[pid]);
			}
		}
		Lvec.clear();
	}

	PIX_EVENT(SPOT_LIGHTS_ACCUM);
	// Spot lighting (unshadowed, if left)
	if (!LP.v_spot.empty())
	{
		xr_vector<light*>& Lvec = LP.v_spot;
		for (u32 pid = 0; pid < Lvec.size(); pid++)
		{

			Lvec[pid]->vis_update();
			if (Lvec[pid]->vis.visible)
			{
				g_frame_stats.count(fcLightAccumSpot, 1);
				LR.compute_xf_spot(Lvec[pid]);
				render_indirect(Lvec[pid]);
				Target->accum_spot(Lvec[pid]);
			}
		}
		Lvec.clear();
	}

	// restore world projection if necessary
	for (const HudLightState& state : m_hudLightStateScratch)
	{
		light* L = state.source;
		if (!L->get_hud_mode())
			continue;

		L->position = state.position;
		L->direction = state.direction;
	}
	RDEVICE.Statistic->RenderDUMP_Lights.End();
}

void CRender::render_indirect(light* L)
{
	if (!ps_r2_ls_flags.test(R2FLAG_GI)) return;

	light LIGEN;
	LIGEN.set_type(IRender_Light::REFLECTED);
	LIGEN.set_shadow(false);
	LIGEN.set_cone(PI_DIV_2 * 2.f);

	xr_vector<light_indirect>& Lvec = L->indirect;
	if (Lvec.empty()) return;
	float LE = L->color.intensity();
	for (u32 it = 0; it < Lvec.size(); it++)
	{
		light_indirect& LI = Lvec[it];

		// energy and color
		float LIE = LE * LI.E;
		if (LIE < ps_r2_GI_clip) continue;
		Fvector T;
		T.set(L->color.r, L->color.g, L->color.b).mul(LI.E);
		LIGEN.set_color(T.x, T.y, T.z);

		// geometric
		Fvector L_up, L_right;
		L_up.set(0, 1, 0);
		if (_abs(L_up.dotproduct(LI.D)) > .99f) L_up.set(0, 0, 1);
		L_right.crossproduct(L_up, LI.D).normalize();
		LIGEN.spatial.sector = LI.S;
		LIGEN.set_position(LI.P);
		LIGEN.set_rotation(LI.D, L_right);

		// range
		// dist^2 / range^2 = A - has infinity number of solutions
		// approximate energy by linear fallof Emax / (1 + x) = Emin
		float Emax = LIE;
		float Emin = 1.f / 255.f;
		float x = (Emax - Emin) / Emin;
		if (x < 0.1f) continue;
		LIGEN.set_range(x);

		Target->accum_reflected(&LIGEN);
	}
}
