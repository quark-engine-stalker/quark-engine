#include "stdafx.h"
#include "../../xrEngine/igame_persistent.h"
#include "../xrRender/FBasicVisual.h"
#include "../../xrEngine/customhud.h"
#include "../../xrEngine/xr_object.h"

#include "../xrRender/QueryHelper.h"
#include "../../xrCore/profiler.h"

extern int ps_r__static_multifrustum;
extern int ps_r__visibility_submission;
extern int ps_r__forward_visibility_reuse;

void CRender::render_main(Fmatrix& m_ViewProjection, bool _fportals)
{
	// The experimental visibility-submission/multi-frustum path is not safe for
	// the deferred opaque pass: it can submit a static visual with a reduced
	// portal mask that does not match the current G-buffer view.  The resulting
	// stale/missing static fragments show up as light and dark patches on walls,
	// while Details (grass) remains unaffected.  Keep the original Monolith
	// per-frustum submission until that path is made render-equivalent.
	constexpr bool use_optimized_visibility_submission = false;

	PIX_EVENT(render_main);
	//	Msg						("---begin");
	marker ++;
	// Forward geometry uses the same camera and world visibility snapshot later in
	// this frame. Reuse only the dynamic HOM/frustum result; keep the original
	// portal/static submission path untouched for GAMMA render compatibility.
	const bool reuse_forward_dynamic_visibility =
		ps_r__forward_visibility_reuse && !_fportals && phase == PHASE_NORMAL &&
		m_forwardDynamicVisibilityValid && m_forwardDynamicVisibilityFrame == Device.dwFrame;
	const bool capture_forward_dynamic_visibility =
		ps_r__forward_visibility_reuse && _fportals && phase == PHASE_NORMAL;
	if (capture_forward_dynamic_visibility)
	{
		m_forwardDynamicVisibility.clear_not_free();
		m_forwardDynamicVisibilityValid = false;
	}

	// Calculate sector(s) and their objects
	if (pLastSector)
	{
		//!!!
		//!!! BECAUSE OF PARALLEL HOM RENDERING TRY TO DELAY ACCESS TO HOM AS MUCH AS POSSIBLE
		//!!!
		{

			// Traverse object database. Keep query and sorting separately visible in
			// CPU profiles because both scale with populated GAMMA scenes.
			START_PROFILE("render_main/q_frustum");
			g_SpatialSpace->q_frustum
			(
				lstRenderables,
				ISpatial_DB::O_ORDERED,
				STYPE_RENDERABLE + STYPE_LIGHTSOURCE,
				ViewBase
			);

			STOP_PROFILE;

			// Preserve the original exact front-to-back result, but calculate each
			// distance once instead of twice for every std::sort comparison. The
			// scratch allocation is retained between frames.
			START_PROFILE("render_main/sort_dynamic");
			lstRenderableSortKeys.clear_not_free();
			lstRenderableSortKeys.reserve(lstRenderables.size());
			for (u32 sort_index = 0; sort_index < lstRenderables.size(); ++sort_index)
			{
				SpatialSortItem item;
				item.spatial = lstRenderables[sort_index];
				item.distance_sq = item.spatial->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition);
				item.original_index = sort_index;
				lstRenderableSortKeys.push_back(item);
			}

			std::sort(lstRenderableSortKeys.begin(), lstRenderableSortKeys.end(),
				[](const SpatialSortItem& left, const SpatialSortItem& right)
				{
					if (left.distance_sq != right.distance_sq)
						return left.distance_sq < right.distance_sq;
					return left.original_index < right.original_index;
				});

			for (u32 sort_index = 0; sort_index < lstRenderableSortKeys.size(); ++sort_index)
				lstRenderables[sort_index] = lstRenderableSortKeys[sort_index].spatial;

			STOP_PROFILE;

			// Determine visibility for dynamic part of scene
			set_Object(0);
			u32 uID_LTRACK = 0xffffffff;
			if (phase == PHASE_NORMAL)
			{
				uLastLTRACK ++;
				if (lstRenderables.size()) uID_LTRACK = uLastLTRACK % lstRenderables.size();

				// update light-vis for current entity / actor
				CObject* O = g_pGameLevel->CurrentViewEntity();
				if (O)
				{
					CROS_impl* R = (CROS_impl*)O->ROS();
					if (R) R->update(O);
				}

				// update light-vis for selected entity
				// track lighting environment
				if (lstRenderables.size())
				{
					IRenderable* renderable = lstRenderables[uID_LTRACK]->dcast_Renderable();
					if (renderable)
					{
						CROS_impl* T = (CROS_impl*)renderable->renderable_ROS();
						if (T) T->update(renderable);
					}
				}
			}
		}

		{

			// Traverse sector/portal structure
			START_PROFILE("render_main/portal_traversal");
			PortalTraverser.traverse
			(
				pLastSector,
				ViewBase,
				Device.vCameraPosition,
				m_ViewProjection,
				CPortalTraverser::VQ_HOM + CPortalTraverser::VQ_SSA + CPortalTraverser::VQ_FADE
				//. disabled scissoring (HW.Caps.bScissor?CPortalTraverser::VQ_SCISSOR:0)	// generate scissoring info
			);

			STOP_PROFILE;
		}

		{

			START_PROFILE("render_main/static_visibility_submission");
			if (use_optimized_visibility_submission && ps_r__visibility_submission)
			{
			// Static visibility stage: portal traversal has produced sector/frustum
			// records, but no static visual has submitted packets yet.
			m_staticVisibility.clear_and_reserve();
			for (u32 s_it = 0; s_it < PortalTraverser.r_sectors.size(); ++s_it)
			{
				CSector* sector = (CSector*)PortalTraverser.r_sectors[s_it];
				if (!sector || sector->r_frustums.empty() || !sector->root())
					continue;

				StaticVisibilityItem item;
				item.sector = sector;
				item.root = sector->root();
				m_staticVisibility.push_back(item);
			}

			// Static submission stage. Preserve the original static-before-dynamic
			// ordering while traversing every sector hierarchy only once.
			for (u32 i = 0; i < m_staticVisibility.size(); ++i)
			{
				StaticVisibilityItem& item = m_staticVisibility[i];
				xr_vector<CFrustum>& frustums = item.sector->r_frustums;
				if (!ps_r__static_multifrustum || frustums.size() == 1)
				{
					for (u32 v_it = 0; v_it < frustums.size(); ++v_it)
					{
						set_Frustum(&frustums[v_it]);
						add_Geometry(item.root);
					}
				}
				else
				{
					add_Static_MultiFrustum(item.root, frustums);
				}
			}
			}
			else
			{
			// Compatibility fallback: original immediate static submission order.
			for (u32 s_it = 0; s_it < PortalTraverser.r_sectors.size(); ++s_it)
			{
				CSector* sector = (CSector*)PortalTraverser.r_sectors[s_it];
				if (!sector || sector->r_frustums.empty() || !sector->root())
					continue;

				for (u32 v_it = 0; v_it < sector->r_frustums.size(); ++v_it)
				{
					set_Frustum(&sector->r_frustums[v_it]);
					add_Geometry(sector->root());
				}
			}
			}
			STOP_PROFILE;

		}

		// Dynamic visibility stage. HOM/frustum tests populate a compact list;
		{

			START_PROFILE("render_main/dynamic_visibility");
			// renderable_Render() is deferred to the following submission pass.
			m_dynamicVisibility.clear_and_reserve();
			set_Object(0);
			if (!reuse_forward_dynamic_visibility)
			{
				for (u32 o_it = 0; o_it < lstRenderables.size(); ++o_it)
				{
					ISpatial* spatial = lstRenderables[o_it];
					spatial->spatial_updatesector();
					CSector* sector = (CSector*)spatial->spatial.sector;
					if (!sector)
						continue;

					if (spatial->spatial.type & STYPE_LIGHTSOURCE)
					{
						light* L = (light*)spatial->dcast_Light();
						VERIFY(L);
						if (L->get_LOD() > EPS_L)
						{
							vis_data& vis = L->get_homdata();
							if (HOM.visible(vis))
								Lights.add_light(L);
						}
						continue;
					}

					if (PortalTraverser.i_marker != sector->r_marker)
						continue;
					// Deferred destruction leaves objects in the spatial tree until
					// their batch is committed. Do not submit those objects to DSGraph.
					CObject* object = spatial->dcast_CObject();
					if (object && object->getDestroy())
						continue;

					for (u32 v_it = 0; v_it < sector->r_frustums.size(); ++v_it)
					{
						CFrustum& view = sector->r_frustums[v_it];
						if (!view.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
							continue;

						if (spatial->spatial.type & STYPE_RENDERABLE)
						{
							IRenderable* renderable = spatial->dcast_Renderable();
							VERIFY(renderable);

							vis_data& original = ((dxRender_Visual*)renderable->renderable.visual)->vis;
							vis_data transformed = original;
							transformed.box.xform(renderable->renderable.xform);
							const BOOL visible = HOM.visible(transformed);
							original.marker = transformed.marker;
							original.accept_frame = transformed.accept_frame;
							original.hom_frame = transformed.hom_frame;
							original.hom_tested = transformed.hom_tested;
							if (!visible)
								break;

							if (capture_forward_dynamic_visibility)
							{
								ForwardDynamicVisibilityItem forward_item;
								forward_item.renderable = renderable;
								forward_item.frustum = view;
								m_forwardDynamicVisibility.push_back(forward_item);
							}

							if (use_optimized_visibility_submission && ps_r__visibility_submission)
							{
								DynamicVisibilityItem item;
								item.renderable = renderable;
								item.frustum = &view;
								m_dynamicVisibility.push_back(item);
							}
							else
							{
								// Compatibility fallback: original immediate dsgraph submission.
								set_Object(renderable);
								renderable->renderable_Render();
								set_Object(0);
							}
						}
						break;
					}
				}
			}
			if (capture_forward_dynamic_visibility)
			{
				m_forwardDynamicVisibilityFrame = Device.dwFrame;
				m_forwardDynamicVisibilityValid = true;
			}
			STOP_PROFILE;

		}

		// Dynamic dsgraph submission stage.
		{

			START_PROFILE("render_main/dynamic_submission");
			if (reuse_forward_dynamic_visibility)
			{
				for (u32 i = 0; i < m_forwardDynamicVisibility.size(); ++i)
				{
					ForwardDynamicVisibilityItem& item = m_forwardDynamicVisibility[i];
					set_Frustum(&item.frustum);
					set_Object(item.renderable);

					{

						item.renderable->renderable_Render();
					}
					set_Object(0);
				}
			}
			else if (use_optimized_visibility_submission && ps_r__visibility_submission)
			{
				for (u32 i = 0; i < m_dynamicVisibility.size(); ++i)
				{
					DynamicVisibilityItem& item = m_dynamicVisibility[i];
					set_Frustum(item.frustum);
					set_Object(item.renderable);

					{

						item.renderable->renderable_Render();
					}
					set_Object(0);
				}
			}

			STOP_PROFILE;

		}
		if (g_pGameLevel && (phase == PHASE_NORMAL))
		{
			g_hud->Render_Last(); // HUD
			if (g_hud->RenderActiveItemUIQuery())
				r_dsgraph_render_hud_ui();
			if (g_hud->RenderCamAttachedUIQuery())
				r_dsgraph_render_cam_ui();
		}
	}
	else
	{
		set_Object(0);
		if (g_pGameLevel && (phase == PHASE_NORMAL))
		{
			g_hud->Render_Last(); // HUD
			if (g_hud->RenderActiveItemUIQuery())
				r_dsgraph_render_hud_ui();
			if (g_hud->RenderCamAttachedUIQuery())
				r_dsgraph_render_cam_ui();
		}
	}
}

void CRender::render_menu()
{

	PIX_EVENT(render_menu);
	//	Globals
	RCache.set_CullMode(CULL_CCW);
	RCache.set_Stencil(FALSE);
	RCache.set_ColorWriteEnable();

	// Main Render
	{

		Target->u_setrt(Target->rt_Generic_0, 0, 0, HW.pBaseZB); // LDR RT
		g_pGamePersistent->OnRenderPPUI_main(); // PP-UI
	}

	// Distort
	{

		FLOAT ColorRGBA[4] = {127.0f / 255.0f, 127.0f / 255.0f, 0.0f, 127.0f / 255.0f};
		Target->u_setrt(Target->rt_Generic_1, 0, 0, HW.pBaseZB); // Now RT is a distortion mask
		HW.pContext->ClearRenderTargetView(Target->rt_Generic_1->pRT, ColorRGBA);
		g_pGamePersistent->OnRenderPPUI_PP(); // PP-UI
	}

	// Actual Display

	Target->u_setrt(Device.dwWidth, Device.dwHeight, HW.pBaseRT,NULL,NULL, HW.pBaseZB);
	RCache.set_Shader(Target->s_menu);
	RCache.set_Geometry(Target->g_menu);

	Fvector2 p0, p1;
	u32 Offset;
	u32 C = color_rgba(255, 255, 255, 255);
	float _w = float(Device.dwWidth);
	float _h = float(Device.dwHeight);
	float d_Z = EPS_S;
	float d_W = 1.f;
	p0.set(.5f / _w, .5f / _h);
	p1.set((_w + .5f) / _w, (_h + .5f) / _h);

	FVF::TL* pv = (FVF::TL*)RCache.Vertex.Lock(4, Target->g_menu->vb_stride, Offset);
	pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
	pv++;
	pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
	pv++;
	pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
	pv++;
	pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
	pv++;
	RCache.Vertex.Unlock(4, Target->g_menu->vb_stride);
	RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

extern u32 g_r;

void CRender::Render()
{

	PIX_EVENT(CRender_Render);
	r_dsgraph_begin_frame(Device.dwFrame);
	m_forwardDynamicVisibilityValid = false;

	VERIFY(0 == mapDistort.size() + mapHUDDistort.size());

	rmNormal();

	bool _menu_pp = g_pGamePersistent ? g_pGamePersistent->OnRenderPPUI_query() : false;
	if (_menu_pp)
	{
		render_menu();
		return;
	};

	IMainMenu* pMainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : 0;
	bool bMenu = pMainMenu ? pMainMenu->CanSkipSceneRendering() : false;

	if (!(g_pGameLevel && g_hud)
		|| bMenu)
	{
		Target->u_setrt(Device.dwWidth, Device.dwHeight, HW.pBaseRT,NULL,NULL, HW.pBaseZB);
		return;
	}

	if (m_bFirstFrameAfterReset)
	{
		m_bFirstFrameAfterReset = false;
		return;
	}

	//.	VERIFY					(g_pGameLevel && g_pGameLevel->pHUD);

	// Configure
	RImplementation.o.distortion = FALSE; // disable distorion
	Fcolor sun_color = ((light*)Lights.sun_adapted._get())->color;
	BOOL bSUN = ps_r2_ls_flags.test(R2FLAG_SUN) && (u_diffuse2s(sun_color.r, sun_color.g, sun_color.b)>EPS) && !strstr(Core.Params, "-r4_dev");
	if (o.sunstatic) bSUN = FALSE;
	// Msg						("sstatic: %s, sun: %s",o.sunstatic?;"true":"false", bSUN?"true":"false");

	{

		// HOM
		ViewBase.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);
		View = 0;
		if (!ps_r2_ls_flags.test(R2FLAG_EXP_MT_CALC))
		{
			HOM.Enable();
			HOM.Render(ViewBase);
		}
	}

	//******* Z-prefill calc - DEFERRER RENDERER
	if (ps_r2_ls_flags.test(R2FLAG_ZFILL))
	{

		PIX_EVENT(DEFER_Z_FILL);
		Device.Statistic->RenderCALC.Begin();
		float z_distance = ps_r2_zfill;
		Fmatrix m_zfill, m_project;
		m_project.build_projection(
			deg2rad(Device.fFOV/* *Device.fASPECT*/),
			Device.fASPECT, VIEWPORT_NEAR,
			z_distance * g_pGamePersistent->Environment().CurrentEnv->far_plane);
		m_zfill.mul(m_project, Device.mView);
		r_pmask(true, false); // enable priority "0"
		set_Recorder(NULL);
		phase = PHASE_SMAP;
		render_main(m_zfill, false);
		r_pmask(true, false); // disable priority "1"
		Device.Statistic->RenderCALC.End();

		// flush
		Target->phase_scene_prepare();
		RCache.set_ColorWriteEnable(FALSE);
		r_dsgraph_render_graph(0);
		RCache.set_ColorWriteEnable();
	}
	else
	{
		Target->phase_scene_prepare();
	}

	{

		//*******
		// Sync point
		Device.Statistic->RenderDUMP_Wait_S.Begin();
		// The old q-sync path stalled every frame even on a single GPU. It was
		// designed for multi-GPU pacing and has no useful work to perform when only
		// one adapter is active.
		if (ps_r2_qsync && HW.Caps.iGPUNum > 1)
		{
			CTimer T;
			T.Start();
			BOOL result = FALSE;
			HRESULT hr = S_FALSE;
			while ((hr = HW.pContext->GetData(q_sync_point[q_sync_count], &result, sizeof(result), 0)) == S_FALSE)
			{
				if (!SwitchToThread()) Sleep(ps_r2_wait_sleep);
				if (T.GetElapsed_ms() > 500)
					break;
			}
		}
		Device.Statistic->RenderDUMP_Wait_S.End();
		q_sync_count = (q_sync_count + 1) % HW.Caps.iGPUNum;
		//CHK_DX										(q_sync_point[q_sync_count]->Issue(D3DISSUE_END));
		CHK_DX(EndQuery(q_sync_point[q_sync_count]));
	}

	//******* Main calc - DEFERRER RENDERER
	// Main calc
	{

		Device.Statistic->RenderCALC.Begin();
		r_pmask(true, false, true); // enable priority "0",+ capture wmarks
		if (bSUN) set_Recorder(&main_coarse_structure);
		else set_Recorder(NULL);
		phase = PHASE_NORMAL;
		render_main(Device.mFullTransform, true);
		set_Recorder(NULL);
		r_pmask(true, false); // disable priority "1"
		Device.Statistic->RenderCALC.End();
	}

	/*if (RImplementation.o.ssfx_core) // SSS23: DEPRECATED
	{
		// HUD Masking rendering
		FLOAT ColorRGBA[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
		HW.pContext->ClearRenderTargetView(Target->rt_ssfx_hud->pRT, ColorRGBA);

		Target->u_setrt(Target->rt_ssfx_hud, NULL, NULL, HW.pBaseZB);
		r_dsgraph_render_hud(true);

		// Reset Depth
		HW.pContext->ClearDepthStencilView(HW.pBaseZB, D3D_CLEAR_DEPTH, 1.0f, 0);
	}*/

	if (RImplementation.o.ssfx_motionvectors)
	{

		Target->u_setrt(Device.dwWidth, Device.dwHeight, 0, 0, Target->rt_ssfx_motion_vectors->pRT, 0);

		FLOAT ColorRGBA[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		HW.pContext->ClearRenderTargetView(Target->rt_ssfx_motion_vectors->pRT, ColorRGBA);

		RCache.set_Stencil(FALSE);
		g_pGamePersistent->Environment().RenderSky(true);

		RCache.Index.Flush();
		RCache.Vertex.Flush();

		RCache.set_xform_world(Fidentity);
	}

	if (ps_r2_ls_flags.test(R2FLAG_TERRAIN_PREPASS))
	{

		Target->u_setrt(Device.dwWidth, Device.dwHeight, NULL, NULL, NULL, !RImplementation.o.dx10_msaa ? HW.pBaseZB : Target->rt_MSAADepth->pZRT);
		r_dsgraph_render_landscape(0, false);
	}

	BOOL split_the_scene_to_minimize_wait = FALSE;
	if (ps_r2_ls_flags.test(R2FLAG_EXP_SPLIT_SCENE)) split_the_scene_to_minimize_wait = TRUE;

	//******* Main render :: PART-0	-- first
	if (!split_the_scene_to_minimize_wait)
	{

		PIX_EVENT(DEFER_PART0_NO_SPLIT);
		// level, DO NOT SPLIT
		{

			Target->phase_scene_begin();
		}
		{

			r_dsgraph_render_hud();
		}
		{

			r_dsgraph_render_graph(0);
		}
		{

			r_dsgraph_render_lods(true, true);
		}
		if (Details)
		{

			Details->Render();
		}
		if (ps_r2_ls_flags.test(R2FLAG_TERRAIN_PREPASS))
		{

			r_dsgraph_render_landscape(1, true);
		}
		{

			Target->phase_scene_end();
		}
	}
	else
	{

		PIX_EVENT(DEFER_PART0_SPLIT);
		// level, SPLIT
		{

			Target->phase_scene_begin();
		}
		{

			r_dsgraph_render_graph(0);
		}
		{

			Target->disable_aniso();
		}
	}

	//  Redotix99: for 3D Shader Based Scopes 	
	if (scope_3D_fake_enabled)
	{

		ID3D11Resource* zbuffer_res;
		HW.pBaseZB->GetResource(&zbuffer_res);
		HW.pContext->CopyResource(RImplementation.Target->rt_tempzb->pSurface, zbuffer_res);
	}

	//******* Occlusion testing of volume-limited light-sources
	Target->phase_occq();
	LP_normal.clear();
	LP_pending.clear();
	if (RImplementation.o.dx10_msaa)
		RCache.set_ZB(RImplementation.Target->rt_MSAADepth->pZRT);
	{

		PIX_EVENT(DEFER_TEST_LIGHT_VIS);
		// perform tests
		u32 count = 0;
		light_Package& LP = Lights.package;

		// stats
		stats.l_shadowed = LP.v_shadowed.size();
		stats.l_unshadowed = LP.v_point.size() + LP.v_spot.size();
		stats.l_total = stats.l_shadowed + stats.l_unshadowed;

		// perform tests
		count = _max(count, LP.v_point.size());
		count = _max(count, LP.v_spot.size());
		count = _max(count, LP.v_shadowed.size());
		for (u32 it = 0; it < count; it++)
		{
			if (it < LP.v_point.size())
			{
				light* L = LP.v_point[it];
				L->vis_prepare();
				if (L->vis.pending) LP_pending.v_point.push_back(L);
				else LP_normal.v_point.push_back(L);
			}
			if (it < LP.v_spot.size())
			{
				light* L = LP.v_spot[it];
				L->vis_prepare();
				if (L->vis.pending) LP_pending.v_spot.push_back(L);
				else LP_normal.v_spot.push_back(L);
			}
			if (it < LP.v_shadowed.size())
			{
				light* L = LP.v_shadowed[it];
				L->vis_prepare();
				if (L->vis.pending) LP_pending.v_shadowed.push_back(L);
				else LP_normal.v_shadowed.push_back(L);
			}
		}
	}
	LP_normal.sort();
	LP_pending.sort();

	//******* Main render :: PART-1 (second)
	if (split_the_scene_to_minimize_wait)
	{

		PIX_EVENT(DEFER_PART1_SPLIT);
		// skybox can be drawn here

		if (0)
		{
			if (!RImplementation.o.dx10_msaa)
				Target->u_setrt(Target->rt_Generic_0, Target->rt_Generic_1, 0, HW.pBaseZB);
			else
				Target->u_setrt(Target->rt_Generic_0_r, Target->rt_Generic_1, 0,
				                RImplementation.Target->rt_MSAADepth->pZRT);
			RCache.set_CullMode(CULL_NONE);
			RCache.set_Stencil(FALSE);

			// draw skybox
			RCache.set_ColorWriteEnable();
			//CHK_DX(HW.pDevice->SetRenderState			( D3DRS_ZENABLE,	FALSE				));
			RCache.set_Z(FALSE);
			g_pGamePersistent->Environment().RenderSky();
			//CHK_DX(HW.pDevice->SetRenderState			( D3DRS_ZENABLE,	TRUE				));
			RCache.set_Z(TRUE);
		}

		// level
		Target->phase_scene_begin();
		r_dsgraph_render_hud();
		r_dsgraph_render_lods(true, true);
		if (Details) Details->Render();
		if (ps_r2_ls_flags.test(R2FLAG_TERRAIN_PREPASS)) r_dsgraph_render_landscape(1, true);
		Target->phase_scene_end();
	}

	// Wall marks
	if (Wallmarks)
	{

		PIX_EVENT(DEFER_WALLMARKS);
		Target->phase_wallmarks();

		Wallmarks->Render(); // wallmarks has priority as normal geometry
	}

	// Update incremental shadowmap-visibility solver
	{

		PIX_EVENT(DEFER_FLUSH_OCCLUSION);
		u32 it = 0;
		for (it = 0; it < Lights_LastFrame.size(); it++)
		{
			if (0 == Lights_LastFrame[it]) continue ;
			try
			{
				Lights_LastFrame[it]->svis.flushoccq();
			}
			catch (...)
			{
				Msg("! Failed to flush-OCCq on light [%d] %X", it, *(u32*)(&Lights_LastFrame[it]));
			}
		}
		Lights_LastFrame.clear_and_reserve();
	}

	// full screen pass to mark msaa-edge pixels in highest stencil bit
	if (RImplementation.o.dx10_msaa)
	{

		PIX_EVENT(MARK_MSAA_EDGES);
		Target->mark_msaa_edges();
	}

	//	TODO: DX10: Implement DX10 rain.
	if (ps_r2_ls_flags.test(R3FLAG_DYN_WET_SURF))
	{

		PIX_EVENT(DEFER_RAIN);
		render_rain();
	}

	{
		// Save previus and current matrices
		{
			static Fmatrix mm_saved_viewproj;

			if (!Device.m_SecondViewport.IsSVPFrame())
			{
				Target->Matrix_previous.mul(mm_saved_viewproj, Device.mInvView);
				Target->Matrix_current.set(Device.mProject);
				mm_saved_viewproj.set(Device.mFullTransform);
			}
		}

		if (RImplementation.o.ssfx_sss && !Device.m_SecondViewport.IsSVPFrame())
		{
			static bool sss_rendered, sss_extended_rendered;

			// SSS Shadows
			if (ps_ssfx_sss_quality.z > 0)
			{
				Target->phase_ssfx_sss();
				sss_rendered = true;
			}
			else
			{
				if (sss_rendered) // Clear buffer
				{
					sss_rendered = false;
					FLOAT ColorRGBA[4] = { 1,1,1,1 };
					HW.pContext->ClearRenderTargetView(Target->rt_ssfx_sss->pRT, ColorRGBA);
				}
			}

			if (ps_ssfx_sss_quality.w > 0)
			{
				// Extra lights
				Target->phase_ssfx_sss_ext(Lights.package);
				sss_extended_rendered = true;
			}
			else
			{
				if (sss_extended_rendered) // Clear buffer
				{
					sss_extended_rendered = false;
					FLOAT ColorRGBA[4] = { 1,1,1,1 };
					HW.pContext->ClearRenderTargetView(Target->rt_ssfx_sss_tmp->pRT, ColorRGBA);
				}
			}
		}
	}

	// Directional light - fucking sun
	if (bSUN) //bSUN && Device.dwFrame & 1 --Delayed sun update. Worth to check it in future
	{

		PIX_EVENT(DEFER_SUN);
		RImplementation.stats.l_visible ++;
		if (!ps_r2_ls_flags_ext.is(R2FLAGEXT_SUN_OLD))
			render_sun_cascades();
		else
		{
			render_sun_near();
			render_sun();
			render_sun_filtered();
		}
		Target->accum_direct_blend();
	}

	{

		PIX_EVENT(DEFER_SELF_ILLUM);
		Target->phase_accumulator();
		// Render emissive geometry, stencil - write 0x0 at pixel pos
		RCache.set_xform_project(Device.mProject);
		RCache.set_xform_view(Device.mView);
		// Stencil - write 0x1 at pixel pos - 
		if (!RImplementation.o.dx10_msaa)
			RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE,
			                   D3DSTENCILOP_KEEP);
		else
			RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE,
			                   D3DSTENCILOP_KEEP);
		//RCache.set_Stencil				(TRUE,D3DCMP_ALWAYS,0x00,0xff,0xff,D3DSTENCILOP_KEEP,D3DSTENCILOP_REPLACE,D3DSTENCILOP_KEEP);
		RCache.set_CullMode(CULL_CCW);
		RCache.set_ColorWriteEnable();
		RImplementation.r_dsgraph_render_emissive(RImplementation.o.ssfx_bloom ? false : true);
	}

	if (RImplementation.o.ssfx_bloom)
	{

		// Render Emissive on `rt_ssfx_bloom_emissive`
		FLOAT ColorRGBA[4] = { 0,0,0,0 };
		HW.pContext->ClearRenderTargetView(Target->rt_ssfx_bloom_emissive->pRT, ColorRGBA);
		Target->u_setrt(Target->rt_ssfx_bloom_emissive, NULL, NULL, !RImplementation.o.dx10_msaa ? HW.pBaseZB : Target->rt_MSAADepth->pZRT);
		RImplementation.r_dsgraph_render_emissive(true, true);
	}

	// Lighting, non dependant on OCCQ
	{

		PIX_EVENT(DEFER_LIGHT_NO_OCCQ);
		Target->phase_accumulator();
		HOM.Disable();
		render_lights(LP_normal);
	}

	// Lighting, dependant on OCCQ
	{

		PIX_EVENT(DEFER_LIGHT_OCCQ);
		render_lights(LP_pending);
	}

	{

		if (RImplementation.o.ssfx_volumetric)
			Target->phase_ssfx_volumetric_blur();
	}

	// Postprocess
	{

		PIX_EVENT(DEFER_LIGHT_COMBINE);
		Target->phase_combine();
	}

	if (Details)
		Details->details_clear();

	VERIFY(0 == mapDistort.size() + mapHUDDistort.size());
}

void CRender::render_forward()
{
	VERIFY(0 == mapDistort.size() + mapHUDDistort.size());
	RImplementation.o.distortion = RImplementation.o.distortion_enabled; // enable distorion

	//******* Main render - second order geometry (the one, that doesn't support deffering)
	//.todo: should be done inside "combine" with estimation of of luminance, tone-mapping, etc.
	{
		// level
		r_pmask(false, true); // enable priority "1"
		phase = PHASE_NORMAL;
		render_main(Device.mFullTransform, false); //
		//	Igor: we don't want to render old lods on next frame.
		mapLOD.clear();
		r_dsgraph_render_graph(1); // normal level, secondary priority
		PortalTraverser.fade_render(); // faded-portals
		r_dsgraph_render_sorted(); // strict-sorted geoms
		//g_pGamePersistent->Environment().RenderLast(); // rain/thunder-bolts
	}

	RImplementation.o.distortion = FALSE; // disable distorion
}

// Redotix99: for 3D Shader Based Scopes
void CRender::render_Reticle()
{
	VERIFY(0 == mapDistort.size() + mapHUDDistort.size());
	RImplementation.o.distortion = RImplementation.o.distortion_enabled;

	r_dsgraph_render_ScopeSorted();

	RImplementation.o.distortion = FALSE;
}

void CRender::RenderToTarget(RRT target)
{
	ref_rt* RT = nullptr;

	switch (target)
	{
	case rtPDA:
		RT = &Target->rt_ui_pda;
		break;
	case rtSVP:
		RT = &Target->rt_secondVP;
		break;
	default:
		Debug.fatal(DEBUG_INFO, "None or wrong Target specified: %i", target);
		break;
	}

	ID3DTexture2D* pBuffer = nullptr;
	HW.m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBuffer);
	HW.pContext->CopyResource((*RT)->pSurface, pBuffer);
	pBuffer->Release();
}
