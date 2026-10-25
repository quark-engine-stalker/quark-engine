#include "stdafx.h"

#include "../../xrEngine/render.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "../../xrEngine/CustomHUD.h"
#include "../../xrEngine/xr_object.h"

#include "FBasicVisual.h"
#include "FSkinned.h"
#include "../../xrCore/job_system.h"
#include <atomic>

using namespace R_dsgraph;

extern float r_ssaDISCARD;
extern float r_ssaDONTSORT;
extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;
extern int ps_r__static_multifrustum;

ICF float calcLOD(float ssa/*fDistSq*/, float R)
{
	return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

// The comparators below are closures rather than functions: std::sort inlines a
// closure, while a function pointer costs an indirect call per comparison.

// NORMAL
const auto cmp_normal_items = [](const _NormalItem& N1, const _NormalItem& N2)
{
	return (N1.ssa > N2.ssa);
};

void __fastcall mapNormal_Render(mapNormalItems& N, bool items_sorted)
{
	// *** DIRECT ***
	if (!items_sorted)
		std::sort(N.begin(), N.end(), cmp_normal_items);
	for (_NormalItem& Ni : N)
	{
		float LOD = calcLOD(Ni.ssa, Ni.pVisual->vis.sphere.R);
		RCache.LOD.set_LOD(LOD);
		Ni.pVisual->Render(LOD);
	}
}

// Matrix
const auto cmp_matrix_items = [](const _MatrixItem& N1, const _MatrixItem& N2)
{
	return (N1.ssa > N2.ssa);
};

void __fastcall mapMatrix_Render(mapMatrixItems& N, R_constant* material_constant, bool items_sorted)
{
	// *** DIRECT ***
	if (!items_sorted)
		std::sort(N.begin(), N.end(), cmp_matrix_items);
	for (_MatrixItem& Ni : N)
	{
		CSkeletonX* skeleton = nullptr;
		if (Ni.pVisual->Type == MT_SKELETON_GEOMDEF_ST)
			skeleton = static_cast<CSkeletonX_ST*>(Ni.pVisual);
		else if (Ni.pVisual->Type == MT_SKELETON_GEOMDEF_PM)
			skeleton = static_cast<CSkeletonX_PM*>(Ni.pVisual);
		if (skeleton && skeleton->uses_single_bone_transform())
			RCache.xforms.set_W_base_for_single_bone(Ni.Matrix);
		else
			RCache.set_xform_world(Ni.Matrix);
		RImplementation.apply_object(Ni.pObject);
		RImplementation.apply_lmaterial(material_constant);

		float LOD = calcLOD(Ni.ssa, Ni.pVisual->vis.sphere.R);
		RCache.LOD.set_LOD(LOD);
		Ni.pVisual->Render(LOD);
	}
	N.clear();
}

// ALPHA
void __fastcall sorted_L1(mapSorted_Node* N)
{
	VERIFY(N);
	dxRender_Visual* V = N->val.pVisual;
	VERIFY(V && V->shader._get());
	RCache.set_Element(N->val.se);
	RCache.set_xform_world(N->val.Matrix);
	RImplementation.apply_object(N->val.pObject);
	RImplementation.apply_lmaterial();
	V->Render(calcLOD(N->key, V->vis.sphere.R));
}

void __fastcall water_node_ssr(mapSorted_Node* N)
{
	VERIFY(N);
	dxRender_Visual* V = N->val.pVisual;
	VERIFY(V);

	RCache.set_Shader(RImplementation.Target->s_ssfx_water_ssr);

	RCache.set_xform_world(N->val.Matrix);
	RImplementation.apply_object(N->val.pObject);
	RImplementation.apply_lmaterial();

	RCache.set_c("cam_pos", RImplementation.Target->Position_previous.x, RImplementation.Target->Position_previous.y, RImplementation.Target->Position_previous.z, 0.0f);

	// Previous matrix data
	RCache.set_c("m_current", RImplementation.Target->Matrix_current);
	RCache.set_c("m_previous", RImplementation.Target->Matrix_previous);

	V->Render(calcLOD(N->key, V->vis.sphere.R));
}

void __fastcall water_node(mapSorted_Node* N)
{
	VERIFY(N);
	dxRender_Visual* V = N->val.pVisual;
	VERIFY(V);

	if (RImplementation.o.ssfx_water)
	{
		RCache.set_Shader(RImplementation.Target->s_ssfx_water);
	}

	RCache.set_xform_world(N->val.Matrix);
	RImplementation.apply_object(N->val.pObject);
	RImplementation.apply_lmaterial();

	// Wind settings
	float WindDir = g_pGamePersistent->Environment().CurrentEnv->wind_direction;
	float WindVel = g_pGamePersistent->Environment().CurrentEnv->wind_velocity;
	RCache.set_c("wind_setup", WindDir, WindVel, 0, 0);

	V->Render(calcLOD(N->key, V->vis.sphere.R));
}

const auto cmp_vs_nrm = [](mapNormalVS::TNode* N1, mapNormalVS::TNode* N2)
{
	return (N1->val.ssa > N2->val.ssa);
};

const auto cmp_vs_mat = [](mapMatrixVS::TNode* N1, mapMatrixVS::TNode* N2)
{
	return (N1->val.ssa > N2->val.ssa);
};

const auto cmp_ps_nrm = [](mapNormalPS::TNode* N1, mapNormalPS::TNode* N2)
{
	return (N1->val.mapCS.ssa > N2->val.mapCS.ssa);
};

const auto cmp_ps_mat = [](mapMatrixPS::TNode* N1, mapMatrixPS::TNode* N2)
{
	return (N1->val.mapCS.ssa > N2->val.mapCS.ssa);
};

const auto cmp_gs_nrm = [](mapNormalGS::TNode* N1, mapNormalGS::TNode* N2) { return (N1->val.ssa > N2->val.ssa); };
const auto cmp_gs_mat = [](mapMatrixGS::TNode* N1, mapMatrixGS::TNode* N2) { return (N1->val.ssa > N2->val.ssa); };

const auto cmp_cs_nrm = [](mapNormalCS::TNode* N1, mapNormalCS::TNode* N2) { return (N1->val.ssa > N2->val.ssa); };
const auto cmp_cs_mat = [](mapMatrixCS::TNode* N1, mapMatrixCS::TNode* N2) { return (N1->val.ssa > N2->val.ssa); };

const auto cmp_states_nrm = [](mapNormalStates::TNode* N1, mapNormalStates::TNode* N2) { return (N1->val.ssa > N2->val.ssa); };
const auto cmp_states_mat = [](mapMatrixStates::TNode* N1, mapMatrixStates::TNode* N2) { return (N1->val.ssa > N2->val.ssa); };

const auto cmp_textures_lex2_nrm = [](mapNormalTextures::TNode* N1, mapNormalTextures::TNode* N2)
{
	STextureList* t1 = N1->key;
	STextureList* t2 = N2->key;
	if ((*t1)[0] < (*t2)[0]) return true;
	if ((*t1)[0] > (*t2)[0]) return false;
	if ((*t1)[1] < (*t2)[1]) return true;
	else return false;
};

const auto cmp_textures_lex2_mat = [](mapMatrixTextures::TNode* N1, mapMatrixTextures::TNode* N2)
{
	STextureList* t1 = N1->key;
	STextureList* t2 = N2->key;
	if ((*t1)[0] < (*t2)[0]) return true;
	if ((*t1)[0] > (*t2)[0]) return false;
	if ((*t1)[1] < (*t2)[1]) return true;
	else return false;
};

const auto cmp_textures_lex3_nrm = [](mapNormalTextures::TNode* N1, mapNormalTextures::TNode* N2)
{
	STextureList* t1 = N1->key;
	STextureList* t2 = N2->key;
	if ((*t1)[0] < (*t2)[0]) return true;
	if ((*t1)[0] > (*t2)[0]) return false;
	if ((*t1)[1] < (*t2)[1]) return true;
	if ((*t1)[1] > (*t2)[1]) return false;
	if ((*t1)[2] < (*t2)[2]) return true;
	else return false;
};

const auto cmp_textures_lex3_mat = [](mapMatrixTextures::TNode* N1, mapMatrixTextures::TNode* N2)
{
	STextureList* t1 = N1->key;
	STextureList* t2 = N2->key;
	if ((*t1)[0] < (*t2)[0]) return true;
	if ((*t1)[0] > (*t2)[0]) return false;
	if ((*t1)[1] < (*t2)[1]) return true;
	if ((*t1)[1] > (*t2)[1]) return false;
	if ((*t1)[2] < (*t2)[2]) return true;
	else return false;
};

const auto cmp_textures_lexN_nrm = [](mapNormalTextures::TNode* N1, mapNormalTextures::TNode* N2)
{
	STextureList* t1 = N1->key;
	STextureList* t2 = N2->key;
	return std::lexicographical_compare(t1->begin(), t1->end(), t2->begin(), t2->end());
};

const auto cmp_textures_lexN_mat = [](mapMatrixTextures::TNode* N1, mapMatrixTextures::TNode* N2)
{
	STextureList* t1 = N1->key;
	STextureList* t2 = N2->key;
	return std::lexicographical_compare(t1->begin(), t1->end(), t2->begin(), t2->end());
};

const auto cmp_textures_ssa_nrm = [](mapNormalTextures::TNode* N1, mapNormalTextures::TNode* N2)
{
	return (N1->val.ssa > N2->val.ssa);
};

const auto cmp_textures_ssa_mat = [](mapMatrixTextures::TNode* N1, mapMatrixTextures::TNode* N2)
{
	return (N1->val.ssa > N2->val.ssa);
};

void sort_tlist_nrm
(
	xr_frame_vector<mapNormalTextures::TNode*>& lst,
	xr_frame_vector<mapNormalTextures::TNode*>& temp,
	mapNormalTextures& textures,
	BOOL bSSA
)
{
	int amount = textures.begin()->key->size();
	if (bSSA)
	{
		if (amount <= 1)
		{
			// Just sort by SSA
			textures.getANY_P(lst);
			std::sort(lst.begin(), lst.end(), cmp_textures_ssa_nrm);
		}
		else
		{
			// Split into 2 parts
			mapNormalTextures::TNode* _it = textures.begin();
			mapNormalTextures::TNode* _end = textures.end();
			for (; _it != _end; _it++)
			{
				if (_it->val.ssa > r_ssaHZBvsTEX) lst.push_back(_it);
				else temp.push_back(_it);
			}

			// 1st - part - SSA, 2nd - lexicographically
			std::sort(lst.begin(), lst.end(), cmp_textures_ssa_nrm);
			if (2 == amount) std::sort(temp.begin(), temp.end(), cmp_textures_lex2_nrm);
			else if (3 == amount) std::sort(temp.begin(), temp.end(), cmp_textures_lex3_nrm);
			else std::sort(temp.begin(), temp.end(), cmp_textures_lexN_nrm);

			// merge lists
			lst.insert(lst.end(), temp.begin(), temp.end());
		}
	}
	else
	{
		textures.getANY_P(lst);
		if (2 == amount) std::sort(lst.begin(), lst.end(), cmp_textures_lex2_nrm);
		else if (3 == amount) std::sort(lst.begin(), lst.end(), cmp_textures_lex3_nrm);
		else std::sort(lst.begin(), lst.end(), cmp_textures_lexN_nrm);
	}
}

void sort_tlist_mat
(
	xr_frame_vector<mapMatrixTextures::TNode*>& lst,
	xr_frame_vector<mapMatrixTextures::TNode*>& temp,
	mapMatrixTextures& textures,
	BOOL bSSA
)
{
	int amount = textures.begin()->key->size();
	if (bSSA)
	{
		if (amount <= 1)
		{
			// Just sort by SSA
			textures.getANY_P(lst);
			std::sort(lst.begin(), lst.end(), cmp_textures_ssa_mat);
		}
		else
		{
			// Split into 2 parts
			mapMatrixTextures::TNode* _it = textures.begin();
			mapMatrixTextures::TNode* _end = textures.end();
			for (; _it != _end; _it++)
			{
				if (_it->val.ssa > r_ssaHZBvsTEX) lst.push_back(_it);
				else temp.push_back(_it);
			}

			// 1st - part - SSA, 2nd - lexicographically
			std::sort(lst.begin(), lst.end(), cmp_textures_ssa_mat);
			if (2 == amount) std::sort(temp.begin(), temp.end(), cmp_textures_lex2_mat);
			else if (3 == amount) std::sort(temp.begin(), temp.end(), cmp_textures_lex3_mat);
			else std::sort(temp.begin(), temp.end(), cmp_textures_lexN_mat);

			// merge lists
			lst.insert(lst.end(), temp.begin(), temp.end());
		}
	}
	else
	{
		textures.getANY_P(lst);
		if (2 == amount) std::sort(lst.begin(), lst.end(), cmp_textures_lex2_mat);
		else if (3 == amount) std::sort(lst.begin(), lst.end(), cmp_textures_lex3_mat);
		else std::sort(lst.begin(), lst.end(), cmp_textures_lexN_mat);
	}
}

namespace
{
// Small/local-light graphs stay on the original serial path. Fork/join only
// pays for itself when several substantial, independent leaves are available.
constexpr u32 opaque_sort_min_items = 4096;
constexpr size_t opaque_sort_large_leaf = R_dsgraph_structure::opaque_sort_large_leaf_size;

// Depth-only passes (shadow maps, z-prefill, rain) keep the nearest fragment, which does
// not depend on draw order. Submission order leaves the mesh parts of one object adjacent,
// so the following parts reuse its uploaded bones and transforms instead of re-flushing.
IC bool keep_matrix_submission_order(u32 priority)
{
	return priority == 0 && RImplementation.phase == CRender::PHASE_SMAP;
}

template <typename VSMap>
void collect_opaque_sort_ranges(VSMap& vs,
	xr_frame_vector<R_dsgraph_structure::OpaqueSortRange>& ranges,
	u32& large_leaves, size_t& large_items)
{
	for (auto v = vs.begin(); v != vs.end(); ++v)
	for (auto g = v->val.begin(); g != v->val.end(); ++g)
	for (auto p = g->val.begin(); p != g->val.end(); ++p)
	for (auto c = p->val.mapCS.begin(); c != p->val.mapCS.end(); ++c)
	for (auto s = c->val.begin(); s != c->val.end(); ++s)
	for (auto t = s->val.begin(); t != s->val.end(); ++t)
	{
		auto& items = t->val;
		if (items.size() < opaque_sort_large_leaf)
			continue;
		if (items.size() >= opaque_sort_large_leaf)
		{
			++large_leaves;
			large_items += items.size();
		}
		R_dsgraph_structure::OpaqueSortRange range{};
		if constexpr (std::is_same_v<VSMap, mapNormalVS>)
			range.normal = &items;
		else
			range.matrix = &items;
		range.count = items.size();
		ranges.push_back(range);
	}
}

struct OpaqueSortContext
{
	R_dsgraph_structure::OpaqueSortRange* ranges;
	u32 count;
	std::atomic<u32> next{0};
};

void opaque_sort_worker(void* parameter)
{
	auto& context = *static_cast<OpaqueSortContext*>(parameter);
	for (;;)
	{
		const u32 index = context.next.fetch_add(1, std::memory_order_relaxed);
		if (index >= context.count)
			return;
		const auto& range = context.ranges[index];
		if (range.normal)
			std::sort(range.normal->begin(), range.normal->end(), cmp_normal_items);
		else
			std::sort(range.matrix->begin(), range.matrix->end(), cmp_matrix_items);
	}
}
} // namespace

bool R_dsgraph_structure::r_dsgraph_prepare_opaque_sort(u32 priority)
{
	if (m_opaque_sort_items[priority] < opaque_sort_min_items || m_opaque_sort_leaves[priority] < 4 ||
		!xr_jobs::worker_count() || !xr_jobs::idle())
		return false;

	m_opaque_sort_ranges.clear();
	u32 large_leaves = 0;
	size_t large_items = 0;
	for (u32 pass = 0; pass < SHADER_PASSES_MAX; ++pass)
	{
		collect_opaque_sort_ranges(mapNormalPasses[priority][pass],
			m_opaque_sort_ranges, large_leaves, large_items);
		if (!keep_matrix_submission_order(priority))
			collect_opaque_sort_ranges(mapMatrixPasses[priority][pass],
				m_opaque_sort_ranges, large_leaves, large_items);
	}
	if (large_leaves < 4 || large_items < opaque_sort_min_items)
	{
		m_opaque_sort_ranges.clear();
		return false;
	}

	// Long leaves go first to avoid one worker becoming the final straggler.
	// This changes only task assignment; each leaf keeps the original std::sort,
	// comparator, input order, and equal-SSA tie behavior.
	std::sort(m_opaque_sort_ranges.begin(), m_opaque_sort_ranges.end(),
		[](const OpaqueSortRange& a, const OpaqueSortRange& b) { return a.count > b.count; });
	OpaqueSortContext context{m_opaque_sort_ranges.data(), u32(m_opaque_sort_ranges.size())};
	xr_jobs::task_group group;
	const u32 lanes = _min(large_leaves, _min(xr_jobs::available_thread_count(),
		_max(2u, u32(CPU::ID.n_cores))));
	if (!xr_jobs::try_submit_many_idle(&opaque_sort_worker, &context, lanes - 1, &group, xr_jobs::priority::high))
	{
		m_opaque_sort_ranges.clear();
		return false;
	}
	opaque_sort_worker(&context);
	// Join every borrowed range before rendering callbacks, SSA resets, graph
	// clearing, arena reset, device reset, or resource destruction.
	{
		xr_jobs::wait(group);
	}
	m_opaque_sort_ranges.clear();
	return true;
}

void R_dsgraph_structure::r_dsgraph_render_graph(u32 _priority, bool _clear)
{
	//PIX_EVENT(r_dsgraph_render_graph);
	Device.Statistic->RenderDUMP.Begin();
	const bool items_sorted = r_dsgraph_prepare_opaque_sort(_priority);

	// **************************************************** NORMAL
	// Perform sorting based on ScreenSpaceArea
	// Sorting by SSA and changes minimizations
	{
		RCache.set_xform_world(Fidentity);

		// Render several passes
		for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
		{
			//mapNormalVS&	vs				= mapNormal	[_priority];
			mapNormalVS& vs = mapNormalPasses[_priority][iPass];
			vs.getANY_P(nrmVS);
			std::sort(nrmVS.begin(), nrmVS.end(), cmp_vs_nrm);
			for (u32 vs_id = 0; vs_id < nrmVS.size(); vs_id++)
			{
				mapNormalVS::TNode* Nvs = nrmVS[vs_id];
				RCache.set_VS(Nvs->key);

				//	GS setup
				mapNormalGS& gs = Nvs->val;
				gs.ssa = 0;

				gs.getANY_P(nrmGS);
				std::sort(nrmGS.begin(), nrmGS.end(), cmp_gs_nrm);
				for (u32 gs_id = 0; gs_id < nrmGS.size(); gs_id++)
				{
					mapNormalGS::TNode* Ngs = nrmGS[gs_id];
					RCache.set_GS(Ngs->key);

					mapNormalPS& ps = Ngs->val;
					ps.ssa = 0;

				ps.getANY_P(nrmPS);
				std::sort(nrmPS.begin(), nrmPS.end(), cmp_ps_nrm);
				for (u32 ps_id = 0; ps_id < nrmPS.size(); ps_id++)
				{
					mapNormalPS::TNode* Nps = nrmPS[ps_id];
					RCache.set_PS(Nps->key);
						mapNormalCS& cs = Nps->val.mapCS;
						cs.ssa = 0;
						RCache.set_HS(Nps->val.hs);
						RCache.set_DS(Nps->val.ds);
					cs.getANY_P(nrmCS);
					std::sort(nrmCS.begin(), nrmCS.end(), cmp_cs_nrm);
					for (u32 cs_id = 0; cs_id < nrmCS.size(); cs_id++)
					{
						mapNormalCS::TNode* Ncs = nrmCS[cs_id];
						RCache.set_Constants(Ncs->key);

						mapNormalStates& states = Ncs->val;
						states.ssa = 0;
						states.getANY_P(nrmStates);
						std::sort(nrmStates.begin(), nrmStates.end(), cmp_states_nrm);
						for (u32 state_id = 0; state_id < nrmStates.size(); state_id++)
						{
							mapNormalStates::TNode* Nstate = nrmStates[state_id];
							RCache.set_States(Nstate->key);

							mapNormalTextures& tex = Nstate->val;
							tex.ssa = 0;
							sort_tlist_nrm(nrmTextures, nrmTexturesTemp, tex, true);
							for (u32 tex_id = 0; tex_id < nrmTextures.size(); tex_id++)
							{
								mapNormalTextures::TNode* Ntex = nrmTextures[tex_id];
								RCache.set_Textures(Ntex->key);
								RImplementation.apply_lmaterial();

								mapNormalItems& items = Ntex->val;
								items.ssa = 0;
								mapNormal_Render(items, items_sorted && items.size() >= opaque_sort_large_leaf);
								if (_clear) items.clear();
							}
							nrmTextures.clear();
							nrmTexturesTemp.clear();
							if (_clear) tex.clear();
						}
						nrmStates.clear();
						if (_clear) states.clear();
					}
					nrmCS.clear();
					if (_clear) cs.clear();
				}
				nrmPS.clear();
				if (_clear) ps.clear();
				}
				nrmGS.clear();
				if (_clear) gs.clear();
			}
			nrmVS.clear();
			if (_clear) vs.clear();
		}
	}

	// **************************************************** MATRIX
	// Perform sorting based on ScreenSpaceArea
	// Sorting by SSA and changes minimizations
	// Render several passes
	const bool matrix_items_ordered = keep_matrix_submission_order(_priority);
	for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
	{
		//mapMatrixVS&	vs				= mapMatrix	[_priority];
		mapMatrixVS& vs = mapMatrixPasses[_priority][iPass];
		vs.getANY_P(matVS);
		std::sort(matVS.begin(), matVS.end(), cmp_vs_mat);
		for (u32 vs_id = 0; vs_id < matVS.size(); vs_id++)
		{
			mapMatrixVS::TNode* Nvs = matVS[vs_id];
			RCache.set_VS(Nvs->key);

			mapMatrixGS& gs = Nvs->val;
			gs.ssa = 0;

			gs.getANY_P(matGS);
			std::sort(matGS.begin(), matGS.end(), cmp_gs_mat);
			for (u32 gs_id = 0; gs_id < matGS.size(); gs_id++)
			{
				mapMatrixGS::TNode* Ngs = matGS[gs_id];
				RCache.set_GS(Ngs->key);

				mapMatrixPS& ps = Ngs->val;
				ps.ssa = 0;

			ps.getANY_P(matPS);
			std::sort(matPS.begin(), matPS.end(), cmp_ps_mat);
			for (u32 ps_id = 0; ps_id < matPS.size(); ps_id++)
			{
				mapMatrixPS::TNode* Nps = matPS[ps_id];
				RCache.set_PS(Nps->key);

					mapMatrixCS& cs = Nps->val.mapCS;
					cs.ssa = 0;
					RCache.set_HS(Nps->val.hs);
					RCache.set_DS(Nps->val.ds);
				cs.getANY_P(matCS);
				std::sort(matCS.begin(), matCS.end(), cmp_cs_mat);
				for (u32 cs_id = 0; cs_id < matCS.size(); cs_id++)
				{
					mapMatrixCS::TNode* Ncs = matCS[cs_id];
					RCache.set_Constants(Ncs->key);

					mapMatrixStates& states = Ncs->val;
					states.ssa = 0;
					states.getANY_P(matStates);
					std::sort(matStates.begin(), matStates.end(), cmp_states_mat);
					for (u32 state_id = 0; state_id < matStates.size(); state_id++)
					{
						mapMatrixStates::TNode* Nstate = matStates[state_id];
						RCache.set_States(Nstate->key);

						mapMatrixTextures& tex = Nstate->val;
						tex.ssa = 0;
						sort_tlist_mat(matTextures, matTexturesTemp, tex, true);
						for (u32 tex_id = 0; tex_id < matTextures.size(); tex_id++)
						{
							mapMatrixTextures::TNode* Ntex = matTextures[tex_id];
							RCache.set_Textures(Ntex->key);
							R_constant* material_constant = RCache.get_c(RImplementation.c_sbase);

							mapMatrixItems& items = Ntex->val;
							items.ssa = 0;
							// mapMatrix_Render applies the material after each object's
							// lighting update. Preserve the old state update if a retained
							// graph node has no items to draw.
							if (items.empty())
								RImplementation.apply_lmaterial(material_constant);
							mapMatrix_Render(items, material_constant, matrix_items_ordered ||
								(items_sorted && items.size() >= opaque_sort_large_leaf));
						}
						matTextures.clear();
						matTexturesTemp.clear();
						if (_clear) tex.clear();
					}
					matStates.clear();
					if (_clear) states.clear();
				}
				matCS.clear();
				if (_clear) cs.clear();
			}
			matPS.clear();
			if (_clear) ps.clear();
			}
			matGS.clear();
			if (_clear) gs.clear();
		}
		matVS.clear();
		if (_clear) vs.clear();
	}

	// Retained static graphs can use serial sorting on their next draw. This
	// count only controls dispatch; retention and draw ordering never depend on it.
	m_opaque_sort_items[_priority] = 0;
	m_opaque_sort_leaves[_priority] = 0;
	Device.Statistic->RenderDUMP.End();
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void R_dsgraph_structure::r_dsgraph_render_hud(bool NoPS)
{
	//PIX_EVENT(r_dsgraph_render_hud);

	// Change projection
	Fmatrix FTold = Device.mFullTransform;

	Device.mFullTransform = Device.mFullTransformHud;
	RCache.set_xform_project(Device.mProjectHud);

	// Apply HUD Matrix
	Fmatrix Pold_prev = Device.mProject_prev;
	RCache.set_xform_project_prev(Device.mProjectHud);

	// Rendering
	rmNear();
	if (!NoPS)
	{
		mapHUD.traverseLR(sorted_L1);
		mapHUD.clear();

		rmNormal();

		if (scope_3D_fake_enabled)
		{
			RCache.set_RT(RImplementation.Target->rt_ssfx_temp->pRT, 3); // Render scope_3D to any buffer

			mapScopeHUD.traverseLR(sorted_L1);

			if (!RImplementation.o.ssfx_motionvectors)
				RCache.set_RT(NULL, 3);
			else
				RCache.set_RT(RImplementation.Target->rt_ssfx_motion_vectors->pRT, 3);
		}
		mapScopeHUD.clear();

		if (mapCamAttached.size())
		{
			rmNear();

			// Change projection
			Fmatrix camproj;
			camproj.build_projection(
				deg2rad(83.f),
				Device.fASPECT, R_VIEWPORT_NEAR,
				g_pGamePersistent->Environment().CurrentEnv->far_plane);

			Device.mFullTransform.mul(camproj, Device.mView);
			RCache.set_xform_project(camproj);

			// Rendering
			mapCamAttached.traverseLR(sorted_L1);
			mapCamAttached.clear();

			rmNormal();
		}
	}
	/*else
	{
		HUDMask.traverseLR(hud_node);
		HUDMask.clear();

		if (HUDMaskCamAttached.size())
		{
			rmNear();

			// Change projection
			Fmatrix camproj;
			camproj.build_projection(
				deg2rad(83.f),
				Device.fASPECT, R_VIEWPORT_NEAR,
				g_pGamePersistent->Environment().CurrentEnv->far_plane);

			Device.mFullTransform.mul(camproj, Device.mView);
			RCache.set_xform_project(camproj);

			// Rendering
			HUDMaskCamAttached.traverseLR(hud_node);
			HUDMaskCamAttached.clear();
		}

		rmNormal();
	}*/

	// Restore projection
	Device.mFullTransform = FTold;
	RCache.set_xform_project(Device.mProject);

	// Restore Prev Matrix
	RCache.set_xform_project_prev(Pold_prev);
}

void R_dsgraph_structure::r_dsgraph_render_hud_ui()
{
	// Change projection
	Fmatrix FTold = Device.mFullTransform;

	Device.mFullTransform = Device.mFullTransformHud;
	RCache.set_xform_project(Device.mProjectHud);

	rmNear();
	g_hud->RenderActiveItemUI();
	rmNormal();

	// Restore projection
	Device.mFullTransform = FTold;
	RCache.set_xform_project(Device.mProject);
}

void R_dsgraph_structure::r_dsgraph_render_cam_ui()
{
	// Change projection
	Fmatrix FTold = Device.mFullTransform;

	Fmatrix camproj;
	camproj.build_projection(
		deg2rad(83.f),
		Device.fASPECT, R_VIEWPORT_NEAR,
		g_pGamePersistent->Environment().CurrentEnv->far_plane);

	Device.mFullTransform.mul(camproj, Device.mView);
	RCache.set_xform_project(camproj);

	// Rendering
	rmNear();
	g_hud->RenderCamAttachedUI();
	rmNormal();

	// Restore projection
	Device.mFullTransform = FTold;
	RCache.set_xform_project(Device.mProject);
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::r_dsgraph_render_sorted()
{
	// Sorted (back to front)
	mapSorted.traverseRL(sorted_L1);
	mapSorted.clear();

	// Change projection
	Fmatrix FTold = Device.mFullTransform;

	Device.mFullTransform = Device.mFullTransformHud;
	RCache.set_xform_project(Device.mProjectHud);

	// Rendering
	rmNear();
	mapHUDSorted.traverseRL(sorted_L1);
	mapHUDSorted.clear();

	// Camera Script Attachments
	if (mapCamAttachedSorted.size())
	{
		// Change projection
		Fmatrix camproj;
		camproj.build_projection(
			deg2rad(83.f),
			Device.fASPECT, R_VIEWPORT_NEAR,
			g_pGamePersistent->Environment().CurrentEnv->far_plane);

		Device.mFullTransform.mul(camproj, Device.mView);
		RCache.set_xform_project(camproj);

		// Rendering
		mapCamAttachedSorted.traverseRL(sorted_L1);
		mapCamAttachedSorted.clear();
	}

	rmNormal();

	// Restore projection
	Device.mFullTransform = FTold;
	RCache.set_xform_project(Device.mProject);
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::r_dsgraph_render_ScopeSorted()  //  Redotix99: for 3D Shader Based Scopes 	
{
	// Change projection
	Fmatrix FTold = Device.mFullTransform;

	Device.mFullTransform = Device.mFullTransformHud;
	RCache.set_xform_project(Device.mProjectHud);

	// Rendering
	rmNear();
	mapScopeHUDSorted.traverseRL(sorted_L1);
	mapScopeHUDSorted.clear();
	rmNormal();

	// Restore projection
	Device.mFullTransform = FTold;
	RCache.set_xform_project(Device.mProject);
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::r_dsgraph_render_emissive(bool clear, bool renderHUD)
{
	// Sorted (back to front)
	mapEmissive.traverseLR(sorted_L1);
	if (clear)
		mapEmissive.clear();

	// Change projection
	Fmatrix FTold = Device.mFullTransform;

	Device.mFullTransform = Device.mFullTransformHud;
	RCache.set_xform_project(Device.mProjectHud);

	// Rendering
	rmNear();
	// Sorted (back to front)
	mapHUDEmissive.traverseLR(sorted_L1);

	if (clear)
		mapHUDEmissive.clear();

	if (renderHUD)
		mapHUDSorted.traverseRL(sorted_L1);

	rmNormal();

	// Restore projection
	Device.mFullTransform = FTold;
	RCache.set_xform_project(Device.mProject);
}

void R_dsgraph_structure::r_dsgraph_render_water_ssr()
{
	mapWater.traverseLR(water_node_ssr);
}

void R_dsgraph_structure::r_dsgraph_render_water()
{
	mapWater.traverseLR(water_node);
	mapWater.clear();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::r_dsgraph_render_wmarks()
{
	// Sorted (back to front)
	mapWmark.traverseLR(sorted_L1);
	mapWmark.clear();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::r_dsgraph_render_distort()
{
	// Sorted (back to front)
	mapDistort.traverseRL(sorted_L1);
	mapDistort.clear();

	// Change projection
	Fmatrix FTold = Device.mFullTransform;
	Device.mFullTransform = Device.mFullTransformHud;
	RCache.set_xform_project(Device.mProjectHud);

	rmNear();
	mapHUDDistort.traverseLR(sorted_L1);
	mapHUDDistort.clear();
	rmNormal();

	// Restore projection
	Device.mFullTransform = FTold;
	RCache.set_xform_project(Device.mProject);
}

//////////////////////////////////////////////////////////////////////////
// sub-space rendering - shortcut to render with frustum extracted from matrix
void R_dsgraph_structure::r_dsgraph_render_subspace(IRender_Sector* _sector, Fmatrix& mCombined, Fvector& _cop,
                                                    BOOL _dynamic, BOOL _precise_portals, BOOL _static)
{
	CFrustum temp;
	temp.CreateFromMatrix(mCombined, FRUSTUM_P_ALL & (~FRUSTUM_P_NEAR));
	r_dsgraph_render_subspace(_sector, &temp, mCombined, _cop, _dynamic, _precise_portals, _static);
}

// sub-space rendering - main procedure
void R_dsgraph_structure::r_dsgraph_render_subspace(IRender_Sector* _sector, CFrustum* _frustum, Fmatrix& mCombined,
	                                                Fvector& _cop, BOOL _dynamic, BOOL _precise_portals, BOOL _static)
{
	// A spatial can temporarily have no sector near portals, outside static level
	// geometry or during level teardown. Release builds do not execute VERIFY, so
	// reject the empty subspace before touching global render/traversal state.
	if (!_sector || !_frustum)
		return;

	RImplementation.marker ++; // !!! critical here

	// Save and build new frustum, disable HOM
	CFrustum ViewSave = ViewBase;
	ViewBase = *_frustum;
	View = &ViewBase;

	if (_precise_portals && RImplementation.rmPortals)
	{
		// Check if camera is too near to some portal - if so force DualRender
		Fvector box_radius;
		box_radius.set(EPS_L * 20, EPS_L * 20, EPS_L * 20);
		RImplementation.Sectors_xrc.box_options(CDB::OPT_FULL_TEST);
		RImplementation.Sectors_xrc.box_query(RImplementation.rmPortals, _cop, box_radius);
		for (int K = 0; K < RImplementation.Sectors_xrc.r_count(); K++)
		{
			CPortal* pPortal = (CPortal*)RImplementation.Portals[RImplementation.rmPortals->get_tris()[RImplementation
			                                                                                           .Sectors_xrc.
			                                                                                           r_begin()[K].id].
				dummy];
			pPortal->bDualRender = TRUE;
		}
	}

	// Traverse sector/portal structure. Keep this serial: PortalTraverser and the
	// sector frustum lists are shared renderer state.
	PortalTraverser.traverse(_sector, ViewBase, _cop, mCombined, 0);

	// Visibility has already been produced by the portal traversal. Submit one
	// sector hierarchy against the union of its frustums instead of rebuilding
	// dsgraph once for every portal path.
	if (_static)
	{

		for (u32 s_it = 0; s_it < PortalTraverser.r_sectors.size(); ++s_it)
		{
			CSector* sector = (CSector*)PortalTraverser.r_sectors[s_it];
			if (!sector || !sector->root() || sector->r_frustums.empty())
				continue;

			// Keep subspace submission identical to the original Monolith renderer.
			// The reduced-mask multi-frustum path can produce incorrect static
			// coverage in deferred opaque/G-buffer passes.
			for (u32 v_it = 0; v_it < sector->r_frustums.size(); ++v_it)
			{
				set_Frustum(&sector->r_frustums[v_it]);
				add_Geometry(sector->root());
			}
		}
	}

	if (_dynamic)
	{
		set_Object(0);

		{

			g_SpatialSpace->q_frustum
			(
				lstRenderables,
				ISpatial_DB::O_ORDERED,
				STYPE_RENDERABLE,
				ViewBase
			);
		}

		// Determine visibility for dynamic part of scene. Do not move callbacks to
		// workers: renderable_Render() mutates the shared dsgraph and may execute
		// script/mod-owned render callbacks.
		const u32 renderable_count = static_cast<u32>(lstRenderables.size());

		for (u32 o_it = 0; o_it < renderable_count; o_it++)
		{
			ISpatial* spatial = lstRenderables[o_it];
			if (!spatial)
				continue;
			// Large destroy queues are committed over several frames. Their
			// objects must not keep casting shadows while awaiting net_Destroy.
			CObject* object = spatial->dcast_CObject();
			if (object && object->getDestroy())
				continue;
			CSector* sector = (CSector*)spatial->spatial.sector;
			if (0 == sector) continue; // disassociated from S/P structure
			if (PortalTraverser.i_marker != sector->r_marker) continue; // inactive (untouched) sector
			for (u32 v_it = 0; v_it < sector->r_frustums.size(); v_it++)
			{
				set_Frustum(&(sector->r_frustums[v_it]));
				if (!View->testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R)) continue;

				// renderable
				IRenderable* renderable = spatial->dcast_Renderable();
				if (0 == renderable) continue; // unknown, but renderable object (r1_glow???)

				renderable->renderable_Render();
			}
		}
	}

	// Restore
	ViewBase = ViewSave;
	View = 0;
}

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "flod.h"

void R_dsgraph_structure::r_dsgraph_render_R1_box(IRender_Sector* _S, Fbox& BB, int sh)
{
	CSector* S = (CSector*)_S;
	lstVisuals.clear();
	lstVisuals.push_back(S->root());

	for (u32 test = 0; test < lstVisuals.size(); test++)
	{
		dxRender_Visual* V = lstVisuals[test];

		// Visual is 100% visible - simply add it
		xr_vector<IRenderVisual*>::iterator I, E; // it may be usefull for 'hierrarhy' visuals

		switch (V->Type)
		{
		case MT_HIERRARHY:
			{
				// Add all children
				FHierrarhyVisual* pV = (FHierrarhyVisual*)V;
				I = pV->children.begin();
				E = pV->children.end();
				for (; I != E; ++I)
				{
					dxRender_Visual* T = (dxRender_Visual*)*I;
					if (BB.intersect(T->vis.box)) lstVisuals.push_back(T);
				}
			}
			break;
		case MT_SKELETON_ANIM:
		case MT_SKELETON_RIGID:
			{
				// Add all children	(s)
				CKinematics* pV = (CKinematics*)V;
				pV->CalculateBones(TRUE);
				I = pV->children.begin();
				E = pV->children.end();
				for (; I != E; ++I)
				{
					dxRender_Visual* T = (dxRender_Visual*)*I;
					if (BB.intersect(T->vis.box)) lstVisuals.push_back(T);
				}
			}
			break;
		case MT_LOD:
			{
				FLOD* pV = (FLOD*)V;
				I = pV->children.begin();
				E = pV->children.end();
				for (; I != E; ++I)
				{
					dxRender_Visual* T = (dxRender_Visual*)*I;
					if (BB.intersect(T->vis.box)) lstVisuals.push_back(T);
				}
			}
			break;
		default:
			{
				// Renderable visual
				ShaderElement* E = V->shader->E[sh]._get();
				if (E && !(E->flags.bDistort))
				{
					for (u32 pass = 0; pass < E->passes.size(); pass++)
					{
						RCache.set_Element(E, pass);
						V->Render(-1.f);
					}
				}
			}
			break;
		}
	}
}

void __fastcall pLandscape_0(mapLandscape_Node *N)
{
	VERIFY(N);
	dxRender_Visual *V = N->val.pVisual;
	VERIFY(V && V->shader._get());
	RCache.set_Element(N->val.se, 0);
	float LOD = calcLOD(N->val.ssa, V->vis.sphere.R);
	RCache.LOD.set_LOD(LOD);
	V->Render(LOD);
}

void __fastcall pLandscape_1(mapLandscape_Node *N)
{
	VERIFY(N);
	dxRender_Visual *V = N->val.pVisual;
	VERIFY(V && V->shader._get());
	RCache.set_Element(N->val.se, 1);
	RImplementation.apply_lmaterial();
	float LOD = calcLOD(N->val.ssa, V->vis.sphere.R);
	RCache.LOD.set_LOD(LOD);
	V->Render(LOD);
}

void R_dsgraph_structure::r_dsgraph_render_landscape(u32 pass, bool _clear)
{
	RCache.set_xform_world(Fidentity);

	if (pass == 0) 
		mapLandscape.traverseLR(pLandscape_0);
	else 
		mapLandscape.traverseLR(pLandscape_1);

	if (_clear)
		mapLandscape.clear();
}
