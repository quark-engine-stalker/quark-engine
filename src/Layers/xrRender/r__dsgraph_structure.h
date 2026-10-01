#pragma once

#include "../../xrEngine/render.h"
#include "../../xrCore/xrFrameArena.h"
#include "../../xrcdb/ispatial.h"
#include "r__dsgraph_types.h"
#include "r__sector.h"

void r_dsgraph_static_packet_cache_clear();

//////////////////////////////////////////////////////////////////////////
// feedback	for receiving visuals										//
//////////////////////////////////////////////////////////////////////////
class R_feedback
{
public:
	virtual void rfeedback_static(dxRender_Visual* V) = 0;
};

//////////////////////////////////////////////////////////////////////////
// common part of interface implementation for all D3D renderers		//
//////////////////////////////////////////////////////////////////////////
class R_dsgraph_structure : public IRender_interface, public pureFrame
{
public:
	IRenderable* val_pObject;
	Fmatrix* val_pTransform;
	BOOL val_bHUD;
	BOOL val_bCamAttached;
	BOOL val_bInvisible;
	BOOL val_bRecordMP; // record nearest for multi-pass
	R_feedback* val_feedback; // feedback for geometry being rendered
	u32 val_feedback_breakp; // breakpoint
	xr_vector<Fbox3,render_alloc<Fbox3>>* val_recorder; // coarse structure recorder
	u32 phase;
	u32 marker;
	bool pmask [2];
	bool pmask_wmark;
public:
	// Dynamic scene graph
	//R_dsgraph::mapNormal_T										mapNormal	[2]		;	// 2==(priority/2)
	R_dsgraph::mapNormalPasses_T mapNormalPasses [2]; // 2==(priority/2)
	//R_dsgraph::mapMatrix_T										mapMatrix	[2]		;
	R_dsgraph::mapMatrixPasses_T mapMatrixPasses [2];
	R_dsgraph::mapSorted_T mapSorted;
	R_dsgraph::mapHUD_T mapHUD;
	R_dsgraph::mapHUD_T mapCamAttached;
	R_dsgraph::mapLOD_T mapLOD;
	R_dsgraph::mapSorted_T mapDistort;
	R_dsgraph::mapHUD_T mapHUDSorted;
	R_dsgraph::mapHUD_T mapCamAttachedSorted;
	R_dsgraph::mapScopeHUD_T mapScopeHUD;	//  Redotix99: for 3D Shader Based Scopes
	R_dsgraph::mapScopeHUD_T mapScopeHUDSorted;
	R_dsgraph::mapLandscape_T mapLandscape;
	//R_dsgraph::HUDMask_T HUDMask; // SSS 23: Deprecated
	R_dsgraph::HUDMask_T HUDMaskCamAttached;
	R_dsgraph::mapWater_T mapWater;

	R_dsgraph::mapSorted_T										mapWmark;			// sorted
	R_dsgraph::mapSorted_T										mapEmissive;
	R_dsgraph::mapSorted_T										mapHUDEmissive;
	R_dsgraph::mapSorted_T										mapCamAttachedEmissive;
	R_dsgraph::mapSorted_T										mapHUDDistort;

	// Frame-local render scratch. The arena is reset once per Device.dwFrame after
	// every backed container has detached its old storage. Persistent dsgraph maps
	// keep their existing allocator and ordering semantics.
	xr_frame_arena m_dsgraph_frame_arena;
	u32 m_dsgraph_arena_frame;

	xr_frame_vector<R_dsgraph::mapNormalVS::TNode*> nrmVS;
	xr_frame_vector<R_dsgraph::mapNormalGS::TNode*> nrmGS;
	xr_frame_vector<R_dsgraph::mapNormalPS::TNode*> nrmPS;
	xr_frame_vector<R_dsgraph::mapNormalCS::TNode*> nrmCS;
	xr_frame_vector<R_dsgraph::mapNormalStates::TNode*> nrmStates;
	xr_frame_vector<R_dsgraph::mapNormalTextures::TNode*> nrmTextures;
	xr_frame_vector<R_dsgraph::mapNormalTextures::TNode*> nrmTexturesTemp;

	xr_frame_vector<R_dsgraph::mapMatrixVS::TNode*> matVS;
	xr_frame_vector<R_dsgraph::mapMatrixGS::TNode*> matGS;
	xr_frame_vector<R_dsgraph::mapMatrixPS::TNode*> matPS;
	xr_frame_vector<R_dsgraph::mapMatrixCS::TNode*> matCS;
	xr_frame_vector<R_dsgraph::mapMatrixStates::TNode*> matStates;
	xr_frame_vector<R_dsgraph::mapMatrixTextures::TNode*> matTextures;
	xr_frame_vector<R_dsgraph::mapMatrixTextures::TNode*> matTexturesTemp;

	struct SpatialSortItem
	{
		ISpatial* spatial;
		float distance_sq;
		u32 original_index;
	};

	xr_frame_vector<R_dsgraph::_LodItem> lstLODs;
	xr_frame_vector<int> lstLODgroups;
	xr_frame_vector<ISpatial*> lstRenderables;
	xr_frame_vector<SpatialSortItem> lstRenderableSortKeys;
	xr_frame_vector<ISpatial*> lstSpatial;
	xr_frame_vector<dxRender_Visual*> lstVisuals;

	xr_vector<dxRender_Visual*,render_alloc<dxRender_Visual*>> lstRecorded;

	u32 counter_S;
	u32 counter_D;
	BOOL b_loaded;
public:
	virtual void set_Transform(Fmatrix* M)
	{
		VERIFY(M);
		val_pTransform = M;
	}

	virtual void set_HUD(BOOL V) { val_bHUD = V; }
	virtual BOOL get_HUD() { return val_bHUD; }
	virtual void set_CamAttached(BOOL V) { val_bCamAttached = V; }
	virtual BOOL get_CamAttached() { return val_bCamAttached; }
	virtual void set_Invisible(BOOL V) { val_bInvisible = V; }

	void set_Feedback(R_feedback* V, u32 id)
	{
		val_feedback_breakp = id;
		val_feedback = V;
	}

	void set_Recorder(xr_vector<Fbox3,render_alloc<Fbox3>>* dest)
	{
		val_recorder = dest;
		if (dest) dest->clear();
	}

	void get_Counters(u32& s, u32& d)
	{
		s = counter_S;
		d = counter_D;
	}

	void clear_Counters() { counter_S = counter_D = 0; }
public:
	R_dsgraph_structure()
		: m_dsgraph_frame_arena(512u * 1024u),
		  m_dsgraph_arena_frame(u32(-1)),
		  nrmVS(m_dsgraph_frame_arena), nrmGS(m_dsgraph_frame_arena),
		  nrmPS(m_dsgraph_frame_arena), nrmCS(m_dsgraph_frame_arena),
		  nrmStates(m_dsgraph_frame_arena), nrmTextures(m_dsgraph_frame_arena),
		  nrmTexturesTemp(m_dsgraph_frame_arena),
		  matVS(m_dsgraph_frame_arena), matGS(m_dsgraph_frame_arena),
		  matPS(m_dsgraph_frame_arena), matCS(m_dsgraph_frame_arena),
		  matStates(m_dsgraph_frame_arena), matTextures(m_dsgraph_frame_arena),
		  matTexturesTemp(m_dsgraph_frame_arena),
		  lstLODs(m_dsgraph_frame_arena), lstLODgroups(m_dsgraph_frame_arena),
		  lstRenderables(m_dsgraph_frame_arena), lstRenderableSortKeys(m_dsgraph_frame_arena),
		  lstSpatial(m_dsgraph_frame_arena), lstVisuals(m_dsgraph_frame_arena)
	{
		val_pObject = NULL;
		val_pTransform = NULL;
		val_bHUD = FALSE;
		val_bCamAttached = FALSE;
		val_bInvisible = FALSE;
		val_bRecordMP = FALSE;
		val_feedback = 0;
		val_feedback_breakp = 0;
		val_recorder = 0;
		marker = 0;
		r_pmask(true, true);
		b_loaded = FALSE;
	};

	void r_dsgraph_begin_frame(u32 frame)
	{
		if (m_dsgraph_arena_frame == frame)
			return;

		// Detach all vector buffers before the monotonic storage is rewound.
		nrmVS.discard_storage();
		nrmGS.discard_storage();
		nrmPS.discard_storage();
		nrmCS.discard_storage();
		nrmStates.discard_storage();
		nrmTextures.discard_storage();
		nrmTexturesTemp.discard_storage();
		matVS.discard_storage();
		matGS.discard_storage();
		matPS.discard_storage();
		matCS.discard_storage();
		matStates.discard_storage();
		matTextures.discard_storage();
		matTexturesTemp.discard_storage();
		lstLODs.discard_storage();
		lstLODgroups.discard_storage();
		lstRenderables.discard_storage();
		lstRenderableSortKeys.discard_storage();
		lstSpatial.discard_storage();
		lstVisuals.discard_storage();

		// Retain the normal high-water mark while preventing a one-off scene spike
		// from permanently increasing renderer memory usage.
		m_dsgraph_frame_arena.reset_and_trim(16u * 1024u * 1024u);

		const size_t vector_reserve_limit = 512u * 1024u;
		nrmVS.prepare_storage(vector_reserve_limit);
		nrmGS.prepare_storage(vector_reserve_limit);
		nrmPS.prepare_storage(vector_reserve_limit);
		nrmCS.prepare_storage(vector_reserve_limit);
		nrmStates.prepare_storage(vector_reserve_limit);
		nrmTextures.prepare_storage(vector_reserve_limit);
		nrmTexturesTemp.prepare_storage(vector_reserve_limit);
		matVS.prepare_storage(vector_reserve_limit);
		matGS.prepare_storage(vector_reserve_limit);
		matPS.prepare_storage(vector_reserve_limit);
		matCS.prepare_storage(vector_reserve_limit);
		matStates.prepare_storage(vector_reserve_limit);
		matTextures.prepare_storage(vector_reserve_limit);
		matTexturesTemp.prepare_storage(vector_reserve_limit);
		lstLODs.prepare_storage(vector_reserve_limit);
		lstLODgroups.prepare_storage(vector_reserve_limit);
		lstRenderables.prepare_storage(vector_reserve_limit);
		lstRenderableSortKeys.prepare_storage(vector_reserve_limit);
		lstSpatial.prepare_storage(vector_reserve_limit);
		lstVisuals.prepare_storage(vector_reserve_limit);

		m_dsgraph_arena_frame = frame;
	}

	void r_dsgraph_destroy()
	{
		r_dsgraph_static_packet_cache_clear();
		nrmVS.discard_storage();
		nrmGS.discard_storage();
		nrmPS.discard_storage();
		nrmCS.discard_storage();
		nrmStates.discard_storage();
		nrmTextures.discard_storage();
		nrmTexturesTemp.discard_storage();

		matVS.discard_storage();
		matGS.discard_storage();
		matPS.discard_storage();
		matCS.discard_storage();
		matStates.discard_storage();
		matTextures.discard_storage();
		matTexturesTemp.discard_storage();

		lstLODs.discard_storage();
		lstLODgroups.discard_storage();
		lstRenderables.discard_storage();
		lstRenderableSortKeys.discard_storage();
		lstSpatial.discard_storage();
		lstVisuals.discard_storage();

		lstRecorded.clear();

		m_dsgraph_frame_arena.release();
		m_dsgraph_arena_frame = u32(-1);

		//mapNormal[0].destroy	();
		//mapNormal[1].destroy	();
		//mapMatrix[0].destroy	();
		//mapMatrix[1].destroy	();
		for (int i = 0; i < SHADER_PASSES_MAX; ++i)
		{
			mapNormalPasses[0][i].destroy();
			mapNormalPasses[1][i].destroy();
			mapMatrixPasses[0][i].destroy();
			mapMatrixPasses[1][i].destroy();
		}
		mapSorted.destroy();
		mapHUD.destroy();
		mapCamAttached.destroy();
		mapLOD.destroy();
		mapDistort.destroy();
		mapHUDSorted.destroy();
		mapHUDDistort.destroy();
		mapCamAttachedSorted.destroy();
		mapLandscape.destroy();
		//HUDMask.destroy(); // SSS 23: Deprecated
		HUDMaskCamAttached.destroy();
		mapWater.destroy();

		mapWmark.destroy();
		mapEmissive.destroy();
		mapHUDEmissive.destroy();
		mapCamAttachedEmissive.destroy();
	}

	void r_pmask(bool _1, bool _2, bool _wm = false)
	{
		pmask[0] = _1;
		pmask[1] = _2;
		pmask_wmark = _wm;
	}

	void r_dsgraph_insert_dynamic(dxRender_Visual* pVisual, Fvector& Center);
	void r_dsgraph_insert_static(dxRender_Visual* pVisual);

	void r_dsgraph_render_graph(u32 _priority, bool _clear = true);
	void r_dsgraph_render_hud(bool NoPS = false);
	void r_dsgraph_render_hud_ui();
	void r_dsgraph_render_cam_ui();
	void r_dsgraph_render_lods(bool _setup_zb, bool _clear);
	void r_dsgraph_render_sorted();
	void r_dsgraph_render_ScopeSorted(); // Redotix99: for 3D Shader Based Scopes
	void r_dsgraph_render_emissive(bool clear = true, bool renderHUD = false);
	void r_dsgraph_render_wmarks();
	void r_dsgraph_render_distort();
	void r_dsgraph_render_subspace(IRender_Sector* _sector, CFrustum* _frustum, Fmatrix& mCombined, Fvector& _cop,
	                               BOOL _dynamic, BOOL _precise_portals = FALSE);
	void r_dsgraph_render_subspace(IRender_Sector* _sector, Fmatrix& mCombined, Fvector& _cop, BOOL _dynamic,
	                               BOOL _precise_portals = FALSE);
	void r_dsgraph_render_R1_box(IRender_Sector* _sector, Fbox& _bb, int _element);

	void r_dsgraph_render_landscape(u32 pass, bool _clear);
	void r_dsgraph_render_water_ssr();
	void r_dsgraph_render_water();


public:
	virtual u32 memory_usage()
	{
#ifdef USE_DOUG_LEA_ALLOCATOR_FOR_RENDER
		return	(g_render_lua_allocator.get_allocated_size());
#else // USE_DOUG_LEA_ALLOCATOR_FOR_RENDER
		return (0);
#endif // USE_DOUG_LEA_ALLOCATOR_FOR_RENDER
	}
};
