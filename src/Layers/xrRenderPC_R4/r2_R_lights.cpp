#include "stdafx.h"
#include "../../xrCore/profiler.h"

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

void CRender::render_lights(light_Package& LP)
{

	RDEVICE.Statistic->RenderDUMP_Lights.Begin();
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
			Lights_LastFrame.push_back(L);

			// render
			RDEVICE.Statistic->RenderDUMP_Srender.Begin();
			phase = PHASE_SMAP;
			if (RImplementation.o.Tshadows) r_pmask(true, true);
			else r_pmask(true, false);
			L->svis.begin();
			PIX_EVENT(SHADOWED_LIGHTS_RENDER_SUBSPACE);

			// Resolve the sector and build the shadow dsgraph in the original light order.
			{

				IRender_Sector* shadow_sector = L->spatial.sector;
				if (!shadow_sector)
				{
					L->spatial.type |= STYPEFLAG_INVALIDSECTOR;
					L->spatial_updatesector();
					shadow_sector = L->spatial.sector;
				}
				if (!shadow_sector)
					shadow_sector = detectSector(L->position);
				if (shadow_sector)
					r_dsgraph_render_subspace(shadow_sector, L->X.S.combine, L->position, TRUE);
			}

			bool bNormal = mapNormalPasses[0][0].size() || mapMatrixPasses[0][0].size();
			bool bSpecial = mapNormalPasses[1][0].size() || mapMatrixPasses[1][0].size() || mapSorted.size();
			{

				if (bNormal || bSpecial)
				{
					{

						if (!smap_page_has_casters)
						{
							Target->phase_smap_spot_clear();
							smap_page_has_casters = true;
						}
						stats.s_merged ++;
						L_spot_s.push_back(L);
						Target->phase_smap_spot(L);
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
				}
			}
			{

				L->svis.end();
			}
			r_pmask(true, false);
			RDEVICE.Statistic->RenderDUMP_Srender.End();
		}

		PIX_EVENT(UNSHADOWED_LIGHTS);

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
	// Point lighting (unshadowed, if left)
	if (!LP.v_point.empty())
	{
		xr_vector<light*>& Lvec = LP.v_point;
		for (u32 pid = 0; pid < Lvec.size(); pid++)
		{

			Lvec[pid]->vis_update();
			if (Lvec[pid]->vis.visible)
			{
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
