#include "stdafx.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "../../xrEngine/irenderable.h"

#include "flod.h"
#include "particlegroup.h"
#include "FTreeVisual.h"

using namespace R_dsgraph;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Scene graph actual insertion and sorting ////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
float r_ssaDISCARD;
float r_ssaDONTSORT;
float r_ssaLOD_A, r_ssaLOD_B;
float r_ssaGLOD_start, r_ssaGLOD_end;
float r_ssaHZBvsTEX;

ICF float CalcSSA(float& distSQ, Fvector& C, dxRender_Visual* V)
{
	float R = V->vis.sphere.R + 0;
	distSQ = Device.vCameraPosition.distance_to_sqr(C) + EPS;
	return R / distSQ;
}

ICF float CalcSSA(float& distSQ, Fvector& C, float R)
{
	distSQ = Device.vCameraPosition.distance_to_sqr(C) + EPS;
	return R / distSQ;
}

extern int ps_r__ssaDISCARD_gradient;
extern float ps_r__ssaDISCARD_exp;
extern float ps_r__ssaDISCARD_fade_k;

namespace
{
#ifndef USE_RESOURCE_DEBUGGER
struct CStaticPassPacket
{
	const SPass* source = nullptr;
	SVS* vs = nullptr;
	ID3DGeometryShader* gs = nullptr;
	ID3DPixelShader* ps = nullptr;
	ID3D11HullShader* hs = nullptr;
	ID3D11DomainShader* ds = nullptr;
	R_constant_table* constants = nullptr;
	ID3DState* state = nullptr;
	STextureList* textures = nullptr;
};

struct CStaticShaderPacket
{
	// Hold the element alive while its raw pass/state pointers are cached. The
	// cache is explicitly cleared before renderer/device reset and destruction.
	ref_selement owner;
	u32 priority = 0;
	bool water = false;
	bool strict_b2f = false;
	bool emissive = false;
	bool wmark = false;
	bool landscape = false;
	xr_vector<CStaticPassPacket> passes;
};

typedef xr_unordered_flat_map<const ShaderElement*, CStaticShaderPacket> STATIC_SHADER_PACKET_CACHE;
STATIC_SHADER_PACKET_CACHE g_static_shader_packets;

const CStaticShaderPacket& static_shader_packet(ShaderElement* shader)
{
	VERIFY(shader);
	if (g_static_shader_packets.empty())
		g_static_shader_packets.reserve(1024);

	STATIC_SHADER_PACKET_CACHE::iterator found = g_static_shader_packets.find(shader);
	if (found == g_static_shader_packets.end())
		found = g_static_shader_packets.emplace(shader, CStaticShaderPacket()).first;

	CStaticShaderPacket& packet = found->second;
	if (!packet.owner)
		packet.owner = ref_selement(shader);

	const u32 priority = shader->flags.iPriority / 2;
	const bool water = !!shader->flags.isWater;
	const bool strict_b2f = !!shader->flags.bStrictB2F;
	const bool emissive = !!shader->flags.bEmissive;
	const bool wmark = !!shader->flags.bWmark;
	const bool landscape = !!shader->flags.bLandscape;

	bool valid = packet.priority == priority && packet.water == water &&
		packet.strict_b2f == strict_b2f && packet.emissive == emissive &&
		packet.wmark == wmark && packet.landscape == landscape &&
		packet.passes.size() == shader->passes.size();
	if (valid)
	{
		for (u32 i = 0; i < shader->passes.size(); ++i)
		{
			if (packet.passes[i].source != &*shader->passes[i])
			{
				valid = false;
				break;
			}
		}
	}
	if (valid)
		return packet;

	packet.priority = priority;
	packet.water = water;
	packet.strict_b2f = strict_b2f;
	packet.emissive = emissive;
	packet.wmark = wmark;
	packet.landscape = landscape;
	packet.passes.resize(shader->passes.size());

	for (u32 i = 0; i < shader->passes.size(); ++i)
	{
		SPass& pass = *shader->passes[i];
		CStaticPassPacket& cached = packet.passes[i];
		cached.source = &pass;
		cached.vs = &*pass.vs;
		cached.gs = pass.gs->gs;
		cached.ps = pass.ps->ps;
		cached.hs = pass.hs->sh;
		cached.ds = pass.ds->sh;
		cached.constants = pass.constants._get();
		cached.state = pass.state->state;
		cached.textures = pass.T._get();
	}

	return packet;
}
#endif

}

void r_dsgraph_static_packet_cache_clear()
{
#ifndef USE_RESOURCE_DEBUGGER
	g_static_shader_packets.clear();
#endif
}

ICF u32 stable_visual_position_hash(const Fvector& position)
{
	union float_bits
	{
		float value;
		u32 bits;
	};

	float_bits x, y, z;
	x.value = position.x;
	y.value = position.y;
	z.value = position.z;

	// FNV-1a over the exact static visual position. Unlike frame-based random
	// thinning this is temporally stable and cannot create shimmering.
	u32 hash = 2166136261u;
	hash = (hash ^ x.bits) * 16777619u;
	hash = (hash ^ y.bits) * 16777619u;
	hash = (hash ^ z.bits) * 16777619u;
	return hash;
}

ICF bool keep_static_by_ssa_gradient(float ssa, const Fvector& position)
{
	if (ssa <= r_ssaDISCARD)
		return false;

	if (!ps_r__ssaDISCARD_gradient || ps_r__ssaDISCARD_fade_k <= 1.f)
		return true;

	const float fade_start = r_ssaDISCARD * ps_r__ssaDISCARD_fade_k;
	if (ssa >= fade_start || fade_start <= r_ssaDISCARD + EPS)
		return true;

	const float survival = clampr((ssa - r_ssaDISCARD) / (fade_start - r_ssaDISCARD), 0.f, 1.f);
	const float threshold = powf(survival, ps_r__ssaDISCARD_exp);
	const float sample = stable_visual_position_hash(position) * (1.f / 4294967296.f);
	return sample <= threshold;
}

void R_dsgraph_structure::r_dsgraph_insert_dynamic(dxRender_Visual* pVisual, Fvector& Center)
{
	if (!pVisual)
		return;

	// A visual can temporarily have no shader (for example while particle
	// resources are being destroyed/recreated, or when an effect definition
	// has no valid cached shader). VERIFY is compiled out in Release, so never
	// dereference the ref_shader until its raw pointer has been validated.
	Shader* visual_shader = pVisual->shader._get();
	if (!visual_shader)
		return;

	CRender& RI = RImplementation;

	if (pVisual->vis.marker == RI.marker) return;
	pVisual->vis.marker = RI.marker;

	float distSQ;
	float SSA = CalcSSA(distSQ, Center, pVisual);
	if (SSA <= r_ssaDISCARD) return;

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	ShaderElement* sh_d = visual_shader->E[4]._get();
	if (RImplementation.o.distortion && sh_d && sh_d->flags.bDistort && pmask[sh_d->flags.iPriority / 2])
	{
		mapSorted_T& test = RI.val_bHUD ? mapHUDDistort : mapDistort;
		mapSorted_Node* N = test.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = RI.val_pObject;
		N->val.pVisual = pVisual;
		N->val.Matrix = *RI.val_pTransform;
		N->val.se = sh_d; // 4=L_special
	}

	// Select shader

	ShaderElement* sh = RImplementation.rimp_select_sh_dynamic(pVisual, distSQ);
	if (0 == sh) return;
	if (!pmask[sh->flags.iPriority / 2]) return;

	// Create common node
	// NOTE: Invisible elements exist only in R1
	_MatrixItem item = {SSA, RI.val_pObject, pVisual, *RI.val_pTransform};

	switch (sh->flags.iScopeLense) {	
		case 0:
			break;

		case 1: {
			mapHUD_Node* N = mapHUD.insertInAnyWay(EPS);
			N->val.ssa = SSA;
			N->val.pObject = RI.val_pObject;
			N->val.pVisual = pVisual;
			N->val.Matrix = *RI.val_pTransform;
			N->val.se = sh;

			// SSS: Deprecated
			/*if (!sh->passes[0]->ps->hud_disabled)
			{
				HUDMask_Node* N2 = HUDMask.insertInAnyWay(EPS);
				N2->val.ssa = SSA;
				N2->val.pObject = RI.val_pObject;
				N2->val.pVisual = pVisual;
				N2->val.Matrix = *RI.val_pTransform;
				N2->val.se = sh;
			}*/
			return;
		}

		case 2: {
			mapHUD_Node * N = mapScopeHUD.insertInAnyWay(distSQ);
			N->val.ssa = SSA;
			N->val.pObject = RI.val_pObject;
			N->val.pVisual = pVisual;
			N->val.Matrix = *RI.val_pTransform;
			N->val.se = sh;
			return;
		}

		case 3: {
			mapSorted_Node * N = mapScopeHUDSorted.insertInAnyWay(distSQ);
			N->val.ssa = SSA;
			N->val.pObject = RI.val_pObject;
			N->val.pVisual = pVisual;
			N->val.Matrix = *RI.val_pTransform;
			N->val.se = sh;
			return;
		}
	}

	// HUD rendering
	if (RI.val_bHUD)
	{
		if (sh->flags.bStrictB2F)
		{
			if (sh->flags.bEmissive && sh_d)
			{
				mapSorted_Node* N = RI.val_bCamAttached ? mapCamAttachedEmissive.insertInAnyWay(distSQ) : mapHUDEmissive.insertInAnyWay(distSQ);
				N->val.ssa = SSA;
				N->val.pObject = RI.val_pObject;
				N->val.pVisual = pVisual;
				N->val.Matrix = *RI.val_pTransform;
				N->val.se = sh_d; // 4=L_special
			}
			mapSorted_Node* N = RI.val_bCamAttached ? mapCamAttachedSorted.insertInAnyWay(distSQ) : mapHUDSorted.insertInAnyWay(distSQ);
			N->val.ssa = SSA;
			N->val.pObject = RI.val_pObject;
			N->val.pVisual = pVisual;
			N->val.Matrix = *RI.val_pTransform;
			N->val.se = sh;
			return;
		}
		else
		{
			mapHUD_Node* N = RI.val_bCamAttached ? mapCamAttached.insertInAnyWay(distSQ) : mapHUD.insertInAnyWay(distSQ);
			N->val.ssa = SSA;
			N->val.pObject = RI.val_pObject;
			N->val.pVisual = pVisual;
			N->val.Matrix = *RI.val_pTransform;
			N->val.se = sh;

			if (sh->flags.bEmissive && sh_d)
			{
				mapSorted_Node* N = RI.val_bCamAttached ? mapCamAttachedEmissive.insertInAnyWay(distSQ) : mapHUDEmissive.insertInAnyWay(distSQ);
				N->val.ssa = SSA;
				N->val.pObject = RI.val_pObject;
				N->val.pVisual = pVisual;
				N->val.Matrix = *RI.val_pTransform;
				N->val.se = sh_d; // 4=L_special
			}
			return;
		}
	}

	// Shadows registering
	if (RI.val_bInvisible) return;

	// strict-sorting selection. Particle visuals are submitted by their
	// dedicated render path; inserting them here renders strict effects twice.
	if (sh->flags.bStrictB2F && !pVisual->dcast_ParticleCustom())
	{
		mapSorted_Node* N = mapSorted.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = RI.val_pObject;
		N->val.pVisual = pVisual;
		N->val.Matrix = *RI.val_pTransform;
		N->val.se = sh;
		return;
	}

	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive && sh_d)
	{
		mapSorted_Node* N = mapEmissive.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = RI.val_pObject;
		N->val.pVisual = pVisual;
		N->val.Matrix = *RI.val_pTransform;
		N->val.se = sh_d; // 4=L_special
	}
	if (sh->flags.bWmark && pmask_wmark)
	{
		mapSorted_Node* N = mapWmark.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = RI.val_pObject;
		N->val.pVisual = pVisual;
		N->val.Matrix = *RI.val_pTransform;
		N->val.se = sh;
		return;
	}

	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		// the most common node
		//SPass&						pass	= *sh->passes.front	();
		//mapMatrix_T&				map		= mapMatrix			[sh->flags.iPriority/2];
		SPass& pass = *sh->passes[iPass];
		mapMatrix_T& map = mapMatrixPasses[sh->flags.iPriority / 2][iPass];

#ifdef USE_RESOURCE_DEBUGGER
		mapMatrixVS::TNode*			Nvs		= map.insert		(pass.vs);
		mapMatrixGS::TNode*			Ngs		= Nvs->val.insert	(pass.gs);
		mapMatrixPS::TNode*			Nps		= Ngs->val.insert	(pass.ps);
#else
		mapMatrixVS::TNode* Nvs = map.insert(&*pass.vs);
		mapMatrixGS::TNode* Ngs = Nvs->val.insert(pass.gs->gs);
		mapMatrixPS::TNode* Nps = Ngs->val.insert(pass.ps->ps);
#endif

#	ifdef USE_RESOURCE_DEBUGGER
		Nps->val.hs = pass.hs;
		Nps->val.ds = pass.ds;
		mapMatrixCS::TNode*			Ncs		= Nps->val.mapCS.insert	(pass.constants._get());
#	else
		Nps->val.hs = pass.hs->sh;
		Nps->val.ds = pass.ds->sh;
		mapMatrixCS::TNode* Ncs = Nps->val.mapCS.insert(pass.constants._get());
#	endif
		mapMatrixStates::TNode* Nstate = Ncs->val.insert(pass.state->state);
		mapMatrixTextures::TNode* Ntex = Nstate->val.insert(pass.T._get());
		mapMatrixItems& items = Ntex->val;
		items.push_back(item);

		// Need to sort for HZB efficient use
		if (SSA > Ntex->val.ssa)
		{
			Ntex->val.ssa = SSA;
			if (SSA > Nstate->val.ssa)
			{
				Nstate->val.ssa = SSA;
				if (SSA > Ncs->val.ssa)
				{
					Ncs->val.ssa = SSA;
					if (SSA > Nps->val.mapCS.ssa)
					{
						Nps->val.mapCS.ssa = SSA;
						if (SSA > Ngs->val.ssa)
						{
							Ngs->val.ssa = SSA;
						if (SSA > Nvs->val.ssa)
						{
							Nvs->val.ssa = SSA;
							}
						}
					}
				}
			}
		}
	}

	if (val_recorder)
	{
		Fbox3 temp;
		Fmatrix& xf = *RI.val_pTransform;
		temp.xform(pVisual->vis.box, xf);
		val_recorder->push_back(temp);
	}
}

void R_dsgraph_structure::r_dsgraph_insert_static(dxRender_Visual* pVisual)
{
	if (!pVisual)
		return;

	Shader* visual_shader = pVisual->shader._get();
	if (!visual_shader)
		return;

	CRender& RI = RImplementation;

	if (pVisual->vis.marker == RI.marker) return;
	pVisual->vis.marker = RI.marker;

	float distSQ;
	float SSA = CalcSSA(distSQ, pVisual->vis.sphere.P, pVisual);

	ShaderElement* sh_d = visual_shader->E[4]._get();
	Flags16& flags = pVisual->flags;
	if (!(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
	{
		if (!keep_static_by_ssa_gradient(SSA, pVisual->vis.sphere.P))
			return;
	}

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	if (RImplementation.o.distortion && sh_d && sh_d->flags.bDistort && pmask[sh_d->flags.iPriority / 2])
	{
		mapSorted_Node* N = mapDistort.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = NULL;
		N->val.pVisual = pVisual;
		N->val.Matrix = Fidentity;
		N->val.se = sh_d; // 4=L_special
	}

	// Keep the original shader-selection semantics. Only the immutable pass/state
	// description is retained and reused below.
	ShaderElement* sh = RImplementation.rimp_select_sh_static(pVisual, distSQ);
	if (0 == sh) return;
	const u32 shader_priority = sh->flags.iPriority / 2;
	// Priority filtering is independent of the pass/state packet. Do it before
	// looking up and validating the cached packet, which is wasted work for the
	// disabled half of shadow/sun submissions.
	if (!pmask[shader_priority]) return;

	// Water rendering
	if (sh->flags.isWater && RImplementation.o.ssfx_water)
	{
		mapWater_Node* N = mapWater.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = NULL;
		N->val.pVisual = pVisual;
		N->val.Matrix = Fidentity;
		N->val.se = sh;
		return;
	}

	// strict-sorting selection. Particle visuals are submitted by their
	// dedicated render path; inserting them here renders strict effects twice.
	if (sh->flags.bStrictB2F && !pVisual->dcast_ParticleCustom())
	{
		mapSorted_Node* N = mapSorted.insertInAnyWay(distSQ);
		N->val.pObject = NULL;
		N->val.pVisual = pVisual;
		N->val.Matrix = Fidentity;
		N->val.se = sh;
		return;
	}

	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive && sh_d)
	{
		mapSorted_Node* N = mapEmissive.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = NULL;
		N->val.pVisual = pVisual;
		N->val.Matrix = Fidentity;
		N->val.se = sh_d; // 4=L_special
	}
	if (sh->flags.bWmark && pmask_wmark)
	{
		mapSorted_Node* N = mapWmark.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = NULL;
		N->val.pVisual = pVisual;
		N->val.Matrix = Fidentity;
		N->val.se = sh;
		return;
	}

	if (val_feedback && counter_S == val_feedback_breakp) val_feedback->rfeedback_static(pVisual);

	counter_S ++;

	if (sh->flags.bLandscape && RI.phase == CRender::PHASE_NORMAL)
	{
		mapLandscape_Node* N = mapLandscape.insertInAnyWay(distSQ);
		N->val.ssa = SSA;
		N->val.pObject = NULL;
		N->val.pVisual = pVisual;
		N->val.Matrix = Fidentity;
		N->val.se = sh;
		return;
	}

#ifndef USE_RESOURCE_DEBUGGER
	// Only ordinary geometry reaches this point. Special routes above do not
	// consume pass/state data, so avoid constructing and validating the packet
	// cache for them. Runtime script flag mutations remain visible because the
	// routing decisions above read the live ShaderElement flags directly.
	const CStaticShaderPacket& shader_packet = static_shader_packet(sh);
	VERIFY(shader_packet.priority == shader_priority);

	for (u32 iPass = 0; iPass < shader_packet.passes.size(); ++iPass)
	{
		const CStaticPassPacket& pass = shader_packet.passes[iPass];
		mapNormal_T& map = mapNormalPasses[shader_packet.priority][iPass];
		mapNormalVS::TNode* Nvs = map.insert(pass.vs);
		mapNormalGS::TNode* Ngs = Nvs->val.insert(pass.gs);
		mapNormalPS::TNode* Nps = Ngs->val.insert(pass.ps);
		Nps->val.hs = pass.hs;
		Nps->val.ds = pass.ds;
		mapNormalCS::TNode* Ncs = Nps->val.mapCS.insert(pass.constants);
		mapNormalStates::TNode* Nstate = Ncs->val.insert(pass.state);
		mapNormalTextures::TNode* Ntex = Nstate->val.insert(pass.textures);
		mapNormalItems& items = Ntex->val;
		_NormalItem item = {SSA, pVisual};
		items.push_back(item);
#else
	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		SPass& pass = *sh->passes[iPass];
		mapNormal_T& map = mapNormalPasses[sh->flags.iPriority / 2][iPass];
		mapNormalVS::TNode* Nvs = map.insert(pass.vs);
		mapNormalGS::TNode* Ngs = Nvs->val.insert(pass.gs);
		mapNormalPS::TNode* Nps = Ngs->val.insert(pass.ps);
		Nps->val.hs = pass.hs;
		Nps->val.ds = pass.ds;
		mapNormalCS::TNode* Ncs = Nps->val.mapCS.insert(pass.constants._get());
		mapNormalStates::TNode* Nstate = Ncs->val.insert(pass.state->state);
		mapNormalTextures::TNode* Ntex = Nstate->val.insert(pass.T._get());
		mapNormalItems& items = Ntex->val;
		_NormalItem item = {SSA, pVisual};
		items.push_back(item);
#endif

		// Need to sort for HZB efficient use
		if (SSA > Ntex->val.ssa)
		{
			Ntex->val.ssa = SSA;
			if (SSA > Nstate->val.ssa)
			{
				Nstate->val.ssa = SSA;
				if (SSA > Ncs->val.ssa)
				{
					Ncs->val.ssa = SSA;
					if (SSA > Nps->val.mapCS.ssa)
					{
						Nps->val.mapCS.ssa = SSA;
						//	if (SSA>Nvs->val.ssa)		{ Nvs->val.ssa = SSA;
						//	} } } } }
						if (SSA > Ngs->val.ssa)
						{
							Ngs->val.ssa = SSA;
						if (SSA > Nvs->val.ssa)
						{
							Nvs->val.ssa = SSA;
							}
						}
					}
				}
			}
		}
	}

	if (val_recorder)
	{
		val_recorder->push_back(pVisual->vis.box);
	}
}

// Static geometry optimization
#define O_S_L1_S_LOW    10.f // geometry 3d volume size
#define O_S_L1_D_LOW    150.f // distance, after which it is not rendered
#define O_S_L2_S_LOW    100.f
#define O_S_L2_D_LOW    200.f
#define O_S_L3_S_LOW    500.f
#define O_S_L3_D_LOW    250.f
#define O_S_L4_S_LOW    2500.f
#define O_S_L4_D_LOW    350.f
#define O_S_L5_S_LOW    7000.f
#define O_S_L5_D_LOW    400.f

#define O_S_L1_S_MED    25.f
#define O_S_L1_D_MED    50.f
#define O_S_L2_S_MED    200.f
#define O_S_L2_D_MED    150.f
#define O_S_L3_S_MED    1000.f
#define O_S_L3_D_MED    200.f
#define O_S_L4_S_MED    2500.f
#define O_S_L4_D_MED    300.f
#define O_S_L5_S_MED    7000.f
#define O_S_L5_D_MED    400.f

#define O_S_L1_S_HII    50.f
#define O_S_L1_D_HII    50.f
#define O_S_L2_S_HII    400.f
#define O_S_L2_D_HII    150.f
#define O_S_L3_S_HII    1500.f
#define O_S_L3_D_HII    200.f
#define O_S_L4_S_HII    5000.f
#define O_S_L4_D_HII    300.f
#define O_S_L5_S_HII    20000.f
#define O_S_L5_D_HII    350.f

#define O_S_L1_S_ULT    50.f
#define O_S_L1_D_ULT    35.f
#define O_S_L2_S_ULT    500.f
#define O_S_L2_D_ULT    125.f
#define O_S_L3_S_ULT    1750.f
#define O_S_L3_D_ULT    175.f
#define O_S_L4_S_ULT    5250.f
#define O_S_L4_D_ULT    250.f
#define O_S_L5_S_ULT    25000.f
#define O_S_L5_D_ULT    300.f

// Dyn geometry optimization

#define O_D_L1_S_LOW    1.f // geometry 3d volume size
#define O_D_L1_D_LOW    80.f // distance, after which it is not rendered
#define O_D_L2_S_LOW    3.f
#define O_D_L2_D_LOW    150.f
#define O_D_L3_S_LOW    4000.f
#define O_D_L3_D_LOW    250.f

#define O_D_L1_S_MED    1.f
#define O_D_L1_D_MED    40.f
#define O_D_L2_S_MED    4.f
#define O_D_L2_D_MED    100.f
#define O_D_L3_S_MED    4000.f
#define O_D_L3_D_MED    200.f

#define O_D_L1_S_HII    1.4f
#define O_D_L1_D_HII    30.f
#define O_D_L2_S_HII    4.f
#define O_D_L2_D_HII    80.f
#define O_D_L3_S_HII    4000.f
#define O_D_L3_D_HII    150.f

#define O_D_L1_S_ULT    2.0f
#define O_D_L1_D_ULT    30.f
#define O_D_L2_S_ULT    8.f
#define O_D_L2_D_ULT    50.f
#define O_D_L3_S_ULT    4000.f
#define O_D_L3_D_ULT    110.f

Fvector4 o_optimize_static_l1_dist = {O_S_L1_D_LOW, O_S_L1_D_MED, O_S_L1_D_HII, O_S_L1_D_ULT};
Fvector4 o_optimize_static_l1_size = {O_S_L1_S_LOW, O_S_L1_S_MED, O_S_L1_S_HII, O_S_L1_S_ULT};
Fvector4 o_optimize_static_l2_dist = {O_S_L2_D_LOW, O_S_L2_D_MED, O_S_L2_D_HII, O_S_L2_D_ULT};
Fvector4 o_optimize_static_l2_size = {O_S_L2_S_LOW, O_S_L2_S_MED, O_S_L2_S_HII, O_S_L2_S_ULT};
Fvector4 o_optimize_static_l3_dist = {O_S_L3_D_LOW, O_S_L3_D_MED, O_S_L3_D_HII, O_S_L3_D_ULT};
Fvector4 o_optimize_static_l3_size = {O_S_L3_S_LOW, O_S_L3_S_MED, O_S_L3_S_HII, O_S_L3_S_ULT};
Fvector4 o_optimize_static_l4_dist = {O_S_L4_D_LOW, O_S_L4_D_MED, O_S_L4_D_HII, O_S_L4_D_ULT};
Fvector4 o_optimize_static_l4_size = {O_S_L4_S_LOW, O_S_L4_S_MED, O_S_L4_S_HII, O_S_L4_S_ULT};
Fvector4 o_optimize_static_l5_dist = {O_S_L5_D_LOW, O_S_L5_D_MED, O_S_L5_D_HII, O_S_L5_D_ULT};
Fvector4 o_optimize_static_l5_size = {O_S_L5_S_LOW, O_S_L5_S_MED, O_S_L5_S_HII, O_S_L5_S_ULT};

Fvector4 o_optimize_dynamic_l1_dist = {O_D_L1_D_LOW, O_D_L1_D_MED, O_D_L1_D_HII, O_D_L1_D_ULT};
Fvector4 o_optimize_dynamic_l1_size = {O_D_L1_S_LOW, O_D_L1_S_MED, O_D_L1_S_HII, O_D_L1_S_ULT};
Fvector4 o_optimize_dynamic_l2_dist = {O_D_L2_D_LOW, O_D_L2_D_MED, O_D_L2_D_HII, O_D_L2_D_ULT};
Fvector4 o_optimize_dynamic_l2_size = {O_D_L2_S_LOW, O_D_L2_S_MED, O_D_L2_S_HII, O_D_L2_S_ULT};
Fvector4 o_optimize_dynamic_l3_dist = {O_D_L3_D_LOW, O_D_L3_D_MED, O_D_L3_D_HII, O_D_L3_D_ULT};
Fvector4 o_optimize_dynamic_l3_size = {O_D_L3_S_LOW, O_D_L3_S_MED, O_D_L3_S_HII, O_D_L3_S_ULT};

constexpr float BASE_FOV = 67.f;

IC float GetAdjustedDistanceSqr(const Fvector& from_position)
// Approximate distance adjusted by FOV, kept squared to avoid a square root for every tested visual.
{
	const float fov_scale = Device.fFOV / BASE_FOV;
	if (fov_scale <= 0.f)
		return -1.f;

	return Device.vCameraPosition.distance_to_sqr(from_position) * fov_scale * fov_scale;
}

IC bool IsBeyondOptimizationCutoff(float sphere_volume, float adjusted_distance_sqr,
	const Fvector4& size_cutoffs, const Fvector4& distance_cutoffs, u32 level_index)
{
	const float distance_cutoff = distance_cutoffs[level_index];
	return sphere_volume < size_cutoffs[level_index] &&
		adjusted_distance_sqr > distance_cutoff * distance_cutoff;
}

IC u32 OptimizationLevelIndex(int level)
{
	return level >= 2 && level <= 4 ? u32(level - 1) : 0;
}

IC bool IsValuableToRender(dxRender_Visual* pVisual, bool isStatic, bool sm, Fmatrix& transform_matrix)
{
	const int optimization_level = isStatic ? opt_static : opt_dynamic;
	const bool optimize_shadow_geometry = sm && !!psDeviceFlags2.test(rsOptShadowGeom);
	// Match the Monolith contract: level 0 disables geometry optimization
	// entirely, including the optional shadow-map geometry filter. Applying the
	// shadow-only cutoff with opt_static == 0 drops opaque building/wall casters
	// while Details/grass are rendered through a separate path.
	if (optimization_level < 1)
		return true;

	if (isStatic && !optimize_shadow_geometry &&
		(pVisual->Type == MT_LOD || pVisual->Type == MT_TREE_PM || pVisual->Type == MT_TREE_ST))
	{
		return true;
	}

	const float sphere_volume = pVisual->getVisData().sphere.volume();
	Fvector world_position;
	if (isStatic)
		world_position = pVisual->vis.sphere.P;
	else
		transform_matrix.transform_tiny(world_position, pVisual->vis.sphere.P);

	const float adjusted_distance_sqr = GetAdjustedDistanceSqr(world_position);

	if (optimize_shadow_geometry) // Highest cut off for shadow map
	{
		const float shadow_distance = ps_ssfx_shadow_cascades.z;
		if (sphere_volume < 50000.f && adjusted_distance_sqr > shadow_distance * shadow_distance)
			// Don't need geometry behind the farthest sun shadow cascade.
			return false;

		constexpr u32 shadow_level_index = 2;
		return !(
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l1_size, o_optimize_static_l1_dist, shadow_level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l2_size, o_optimize_static_l2_dist, shadow_level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l3_size, o_optimize_static_l3_dist, shadow_level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l4_size, o_optimize_static_l4_dist, shadow_level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l5_size, o_optimize_static_l5_dist, shadow_level_index));
	}

	const u32 level_index = OptimizationLevelIndex(optimization_level);
	if (isStatic)
	{
		return !(
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l1_size, o_optimize_static_l1_dist, level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l2_size, o_optimize_static_l2_dist, level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l3_size, o_optimize_static_l3_dist, level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l4_size, o_optimize_static_l4_dist, level_index) ||
			IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
				o_optimize_static_l5_size, o_optimize_static_l5_dist, level_index));
	}

	return !(
		IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
			o_optimize_dynamic_l1_size, o_optimize_dynamic_l1_dist, level_index) ||
		IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
			o_optimize_dynamic_l2_size, o_optimize_dynamic_l2_dist, level_index) ||
		IsBeyondOptimizationCutoff(sphere_volume, adjusted_distance_sqr,
			o_optimize_dynamic_l3_size, o_optimize_dynamic_l3_dist, level_index));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void CRender::add_leafs_Dynamic(dxRender_Visual* pVisual)
{
	if (!pVisual)
		return;

	Flags16& flags = pVisual->flags;

	if (phase != PHASE_NORMAL && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	if (!!!flags.test(IRenderVisualFlags::eIgnoreOptimization) && !IsValuableToRender(pVisual, false, phase == 1, *val_pTransform))
		return;

	// Visual is 100% visible - simply add it
	xr_vector<IRenderVisual*>::iterator I, E; // it may be useful for 'hierrarhy' visual

	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
		{
			// Add all children, doesn't perform any tests
			PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
			for (PS::CParticleGroup::SItemVecIt i_it = pG->items.begin(); i_it != pG->items.end(); ++i_it)
			{
				PS::CParticleGroup::SItem& I = *i_it;
				if (I._effect) add_leafs_Dynamic(I._effect);
				for (xr_vector<dxRender_Visual*>::iterator pit = I._children_related.begin(); pit != I
				                                                                                     ._children_related.
				                                                                                     end(); ++pit)
					add_leafs_Dynamic(*pit);
				for (xr_vector<dxRender_Visual*>::iterator pit = I._children_free.begin(); pit != I._children_free.end()
				     ; ++pit)
					add_leafs_Dynamic(*pit);
			}
		}
		return;
	case MT_HIERRARHY:
		{
			// Add all children, doesn't perform any tests
			FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
			I = pV->children.begin();
			E = pV->children.end();
			for (; I != E; ++I) add_leafs_Dynamic((dxRender_Visual*)*I);
		}
		return;
	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
		{
			// Add all children, doesn't perform any tests
			CKinematics* pV = (CKinematics*)pVisual;
			BOOL _use_lod = FALSE;
			if (pV->m_lod)
			{
				Fvector Tpos;
				float D;
				val_pTransform->transform_tiny(Tpos, pV->vis.sphere.P);
				float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R / 2.f); // assume dynamics never consume full sphere
				if (ssa < r_ssaLOD_A) _use_lod = TRUE;
			}
			if (_use_lod)
			{
				add_leafs_Dynamic(pV->m_lod);
			}
			else
			{
				{
					pV->CalculateBones(TRUE);
					pV->CalculateWallmarks(); //. bug?
				}
				I = pV->children.begin();
				E = pV->children.end();
				for (; I != E; ++I) add_leafs_Dynamic((dxRender_Visual*)*I);
			}
		}
		return;
	default:
		{
			// General type of visual
			// Calculate distance to it's center
			Fvector Tpos;
			val_pTransform->transform_tiny(Tpos, pVisual->vis.sphere.P);
			{
				r_dsgraph_insert_dynamic(pVisual, Tpos);
			}
		}
		return;
	}
}

void CRender::add_leafs_Static(dxRender_Visual* pVisual)
{
	if (!HOM.visible(pVisual->vis))
		return;

	Flags16& flags = pVisual->dcast_RenderVisual()->flags;

	if (phase != PHASE_NORMAL && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	if (!!!flags.test(IRenderVisualFlags::eIgnoreOptimization) && !IsValuableToRender(pVisual, true, phase == 1, *val_pTransform))
		return;

	// Visual is 100% visible - simply add it
	xr_vector<IRenderVisual*>::iterator I, E; // it may be usefull for 'hierrarhy' visuals

	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
		{
			// Add all children, doesn't perform any tests
			PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
			for (PS::CParticleGroup::SItemVecIt i_it = pG->items.begin(); i_it != pG->items.end(); ++i_it)
			{
				PS::CParticleGroup::SItem& I = *i_it;
				if (I._effect) add_leafs_Dynamic(I._effect);
				for (xr_vector<dxRender_Visual*>::iterator pit = I._children_related.begin(); pit != I
				                                                                                     ._children_related.
				                                                                                     end(); ++pit)
					add_leafs_Dynamic(*pit);
				for (xr_vector<dxRender_Visual*>::iterator pit = I._children_free.begin(); pit != I._children_free.end()
				     ; ++pit)
					add_leafs_Dynamic(*pit);
			}
		}
		return;
	case MT_HIERRARHY:
		{
			// Add all children, doesn't perform any tests
			FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
			I = pV->children.begin();
			E = pV->children.end();
			for (; I != E; ++I) add_leafs_Static((dxRender_Visual*)*I);
		}
		return;
	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
		{
			// Add all children, doesn't perform any tests
			CKinematics* pV = (CKinematics*)pVisual;
			pV->CalculateBones(TRUE);
			I = pV->children.begin();
			E = pV->children.end();
			for (; I != E; ++I) add_leafs_Static((dxRender_Visual*)*I);
		}
		return;
	case MT_LOD:
		{
			FLOD* pV = (FLOD*)pVisual;
			float D;
			float ssa = CalcSSA(D, pV->vis.sphere.P, pV);
			ssa *= pV->lod_factor;
			if (ssa < r_ssaLOD_A)
			{
				if (ssa < r_ssaDISCARD) return;
				mapLOD_Node* N = mapLOD.insertInAnyWay(D);
				N->val.ssa = ssa;
				N->val.pVisual = pVisual;
			}
			if (ssa > r_ssaLOD_B || phase == PHASE_SMAP)
			{
				// Add all children, doesn't perform any tests
				I = pV->children.begin();
				E = pV->children.end();
				for (; I != E; ++I) add_leafs_Static((dxRender_Visual*)*I);
			}
		}
		return;
	case MT_TREE_PM:
	case MT_TREE_ST:
		{
			// General type of visual
			r_dsgraph_insert_static(pVisual);
		}
		return;
	default:
		{
			// General type of visual
			r_dsgraph_insert_static(pVisual);
		}
		return;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
BOOL CRender::add_Dynamic(dxRender_Visual* pVisual, u32 planes)
{
	Flags16& flags = pVisual->dcast_RenderVisual()->flags;

	if (phase != PHASE_NORMAL && !!flags.test(IRenderVisualFlags::eNoShadow))
		return FALSE;

	if (!!!flags.test(IRenderVisualFlags::eIgnoreOptimization) && !IsValuableToRender(pVisual, false, phase == 1, *val_pTransform))
		return FALSE;

	// Check frustum visibility and calculate distance to visual's center
	Fvector Tpos; // transformed position
	EFC_Visible VIS;

	val_pTransform->transform_tiny(Tpos, pVisual->vis.sphere.P);
	VIS = View->testSphere(Tpos, pVisual->vis.sphere.R, planes);
	if (fcvNone == VIS) return FALSE;

	// If we get here visual is visible or partially visible
	xr_vector<IRenderVisual*>::iterator I, E; // it may be usefull for 'hierrarhy' visuals

	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
		{
			// Add all children, doesn't perform any tests
			PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
			for (PS::CParticleGroup::SItemVecIt i_it = pG->items.begin(); i_it != pG->items.end(); i_it++)
			{
				PS::CParticleGroup::SItem& I = *i_it;
				if (fcvPartial == VIS)
				{
					if (I._effect) add_Dynamic(I._effect, planes);
					for (xr_vector<dxRender_Visual*>::iterator pit = I._children_related.begin(); pit != I
					                                                                                     .
					                                                                                     _children_related
					                                                                                     .end(); ++pit)
						add_Dynamic(*pit, planes);
					for (xr_vector<dxRender_Visual*>::iterator pit = I._children_free.begin(); pit != I
					                                                                                  ._children_free.
					                                                                                  end(); ++pit)
						add_Dynamic(*pit, planes);
				}
				else
				{
					if (I._effect) add_leafs_Dynamic(I._effect);
					for (xr_vector<dxRender_Visual*>::iterator pit = I._children_related.begin(); pit != I
					                                                                                     .
					                                                                                     _children_related
					                                                                                     .end(); ++pit)
						add_leafs_Dynamic(*pit);
					for (xr_vector<dxRender_Visual*>::iterator pit = I._children_free.begin(); pit != I
					                                                                                  ._children_free.
					                                                                                  end(); ++pit)
						add_leafs_Dynamic(*pit);
				}
			}
		}
		break;
	case MT_HIERRARHY:
		{
			// Add all children
			FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
			I = pV->children.begin();
			E = pV->children.end();
			if (fcvPartial == VIS)
			{
				for (; I != E; ++I) add_Dynamic((dxRender_Visual*)*I, planes);
			}
			else
			{
				for (; I != E; ++I) add_leafs_Dynamic((dxRender_Visual*)*I);
			}
		}
		break;
	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
		{
			// Add all children, doesn't perform any tests
			CKinematics* pV = (CKinematics*)pVisual;
			BOOL _use_lod = FALSE;
			if (pV->m_lod)
			{
				Fvector Tpos;
				float D;
				val_pTransform->transform_tiny(Tpos, pV->vis.sphere.P);
				float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R / 2.f); // assume dynamics never consume full sphere
				if (ssa < r_ssaLOD_A) _use_lod = TRUE;
			}
			if (_use_lod)
			{
				add_leafs_Dynamic(pV->m_lod);
			}
			else
			{
				pV->CalculateBones(TRUE);
				pV->CalculateWallmarks(); //. bug?
				I = pV->children.begin();
				E = pV->children.end();
				for (; I != E; ++I) add_leafs_Dynamic((dxRender_Visual*)*I);
			}
			/*
			I = pV->children.begin		();
			E = pV->children.end		();
			if (fcvPartial==VIS) {
				for (; I!=E; I++)	add_Dynamic			(*I,planes);
			} else {
				for (; I!=E; I++)	add_leafs_Dynamic	(*I);
			}
			*/
		}
		break;
	default:
		{
			// General type of visual
			r_dsgraph_insert_dynamic(pVisual, Tpos);
		}
		break;
	}
	return TRUE;
}

void CRender::add_Static(dxRender_Visual* pVisual, u32 planes)
{
	Flags16& flags = pVisual->dcast_RenderVisual()->flags;

	if (phase != PHASE_NORMAL && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	if (!!!flags.test(IRenderVisualFlags::eIgnoreOptimization) && !IsValuableToRender(pVisual, true, phase == 1, *val_pTransform))
		return;

	// Check frustum visibility and calculate distance to visual's center
	EFC_Visible VIS;
	vis_data& vis = pVisual->vis;
	VIS = View->testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planes);
	if (VIS == fcvNone)
		return;

	if (!HOM.visible(vis))
		return;

	// If we get here visual is visible or partially visible
	xr_vector<IRenderVisual*>::iterator I, E; // it may be usefull for 'hierrarhy' visuals

	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
		{
			// Add all children, doesn't perform any tests
			PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
			for (PS::CParticleGroup::SItem& I : pG->items)
			{
				if (fcvPartial == VIS)
				{
					if (I._effect) add_Dynamic(I._effect, planes);

					for (dxRender_Visual* childRelated : I._children_related)
					{
						add_Dynamic(childRelated, planes);
					}

					for (dxRender_Visual* childFree : I._children_free)
					{
						add_Dynamic(childFree, planes);
					}
				}
				else
				{
					if (I._effect) add_leafs_Dynamic(I._effect);

					for (dxRender_Visual* childRelated : I._children_related)
					{
						add_leafs_Dynamic(childRelated);
					}

					for (dxRender_Visual* childFree : I._children_free)
					{
						add_leafs_Dynamic(childFree);
					}
				}
			}
		}
		break;
	case MT_HIERRARHY:
		{
			// Add all children
			FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
			if (VIS == fcvPartial)
			{
				for (IRenderVisual* childRenderable : pV->children)
				{
					add_Static((dxRender_Visual*)childRenderable, planes);
				}
			}
			else
			{
				for (IRenderVisual* childRenderable : pV->children)
				{
					add_leafs_Static((dxRender_Visual*)childRenderable);
				}
			}
		}
		break;
	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
		{
			// Add all children, doesn't perform any tests
			CKinematics* pV = (CKinematics*)pVisual;
			pV->CalculateBones(TRUE);
			if (VIS == fcvPartial)
			{
				for (IRenderVisual* childRenderable : pV->children)
				{
					add_Static((dxRender_Visual*)childRenderable, planes);
				}
			}
			else
			{
				for (IRenderVisual* childRenderable : pV->children)
				{
					add_leafs_Static((dxRender_Visual*)childRenderable);
				}
			}
		}
		break;
	case MT_LOD:
		{
			FLOD* pV = (FLOD*)pVisual;
			float D;
			float ssa = CalcSSA(D, pV->vis.sphere.P, pV);
			ssa *= pV->lod_factor;
			if (ssa < r_ssaLOD_A)
			{
				if (ssa < r_ssaDISCARD) return;
				mapLOD_Node* N = mapLOD.insertInAnyWay(D);
				N->val.ssa = ssa;
				N->val.pVisual = pVisual;
			}
			if (ssa > r_ssaLOD_B || phase == PHASE_SMAP)
			{
				// Add all children, perform tests
				for (IRenderVisual* childRenderable : pV->children)
				{
					add_leafs_Static((dxRender_Visual*)childRenderable);
				}
			}
		}
		break;
	case MT_TREE_ST:
	case MT_TREE_PM:
		{
			// General type of visual
			r_dsgraph_insert_static(pVisual);
		}
		return;
	default:
		{
			// General type of visual
			r_dsgraph_insert_static(pVisual);
		}
		break;
	}
}

void CRender::gather_sun_static(dxRender_Visual* pVisual, u32 sector_index, u32 cascade_mask,
	u32 fully_visible_mask)
{
	if (!pVisual || !cascade_mask || sector_index >= m_sunSectorGatherUsed)
		return;

	Flags16& flags = pVisual->dcast_RenderVisual()->flags;
	if (flags.test(IRenderVisualFlags::eNoShadow))
		return;

	if (!flags.test(IRenderVisualFlags::eIgnoreOptimization) && !IsValuableToRender(pVisual, true, true, *val_pTransform))
		return;

	if (!HOM.visible(pVisual->vis))
		return;

	SunSectorGather& sector = m_sunSectorGather[sector_index];
	vis_data& vis = pVisual->vis;
	u32 visible_mask = fully_visible_mask & cascade_mask;
	u32 child_fully_visible = visible_mask;
	const u32 needs_test = cascade_mask & ~fully_visible_mask;

	for (u32 cascade = 0; cascade < m_sun_cascades.size() && cascade < SUN_CASCADE_GATHER_COUNT; ++cascade)
	{
		const u32 bit = 1u << cascade;
		if (!(needs_test & bit))
			continue;

		bool visible = false;
		bool fully = false;
		xr_vector<CFrustum>& frustums = sector.frustums[cascade];
		for (u32 f = 0; f < frustums.size(); ++f)
		{
			u32 plane_mask = frustums[f].getMask();
			const EFC_Visible result = frustums[f].testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), plane_mask);
			if (result == fcvNone)
				continue;

			visible = true;
			if (result == fcvFully)
			{
				fully = true;
				break;
			}
		}

		if (visible)
			visible_mask |= bit;
		if (fully)
			child_fully_visible |= bit;
	}

	if (!visible_mask)
		return;

	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
	{
		// Particle groups contain dynamic child transforms. Keep their original
		// add_Static/add_leafs behavior at submission time, but the parent is still
		// classified only once here.
		SunStaticFallback item = {pVisual, sector_index, visible_mask, child_fully_visible & visible_mask};
		m_sunStaticFallback.push_back(item);
	}
		return;

	case MT_HIERRARHY:
	{
		FHierrarhyVisual* hierarchy = (FHierrarhyVisual*)pVisual;
		for (u32 i = 0; i < hierarchy->children.size(); ++i)
			gather_sun_static((dxRender_Visual*)hierarchy->children[i], sector_index, visible_mask,
				child_fully_visible & visible_mask);
	}
		return;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		// A skeleton can be invalidated by animation/IK callbacks within the frame.
		// Keep mutable skeleton traversal on the legacy per-cascade path instead of
		// freezing a child list/pose into the shared static gather.
		SunStaticFallback item = {pVisual, sector_index, visible_mask, child_fully_visible & visible_mask};
		m_sunStaticFallback.push_back(item);
	}
		return;

	case MT_LOD:
	{
		FLOD* lod = (FLOD*)pVisual;
		float distance;
		float ssa = CalcSSA(distance, lod->vis.sphere.P, lod) * lod->lod_factor;
		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;

			// Preserve the legacy mapLOD side effect even though the shadow pass itself
			// renders the LOD children in PHASE_SMAP.
			SunLodCaster lod_item = {pVisual, visible_mask, distance, ssa};
			m_sunLodCasters.push_back(lod_item);
		}

		// In PHASE_SMAP the legacy path switches to add_leafs_Static() once the LOD
		// node itself is visible. Mark every surviving cascade fully visible for
		// descendants to preserve that behavior exactly.
		for (u32 i = 0; i < lod->children.size(); ++i)
			gather_sun_static((dxRender_Visual*)lod->children[i], sector_index, visible_mask, visible_mask);
	}
		return;

	case MT_TREE_PM:
	case MT_TREE_ST:
	default:
	{
		SunStaticCaster caster = {pVisual, visible_mask};
		m_sunStaticCasters.push_back(caster);
	}
		return;
	}
}

void CRender::add_Static_MultiFrustum(dxRender_Visual* pVisual, const xr_vector<CFrustum>& frustums)
{
	if (!pVisual || frustums.empty())
		return;

	// Portal sectors normally expose only a handful of frustums (their persistent
	// storage starts at four). Keep that common path entirely on the stack; maps
	// with an unusually large portal fan-out retain a safe dynamic fallback.
	constexpr u32 inline_frustum_count = 16u;
	const u32 mask_count = static_cast<u32>(frustums.size());
	u32 inline_masks[inline_frustum_count];
	xr_vector<u32> overflow_masks;
	u32* masks = inline_masks;
	if (mask_count > inline_frustum_count)
	{
		overflow_masks.resize(mask_count);
		masks = overflow_masks.data();
	}

	for (u32 i = 0; i < mask_count; ++i)
		masks[i] = frustums[i].getMask();

	add_Static_MultiFrustum(pVisual, frustums, masks, mask_count);
}

void CRender::add_Static_MultiFrustum(dxRender_Visual* pVisual, const xr_vector<CFrustum>& frustums,
	const u32* masks, u32 mask_count)
{
	constexpr u32 fully_visible_mask = u32(-1);
	constexpr u32 inline_frustum_count = 16u;

	if (!pVisual || !masks || !mask_count || frustums.size() < mask_count)
		return;

	Flags16& flags = pVisual->dcast_RenderVisual()->flags;
	if (phase != PHASE_NORMAL && flags.test(IRenderVisualFlags::eNoShadow))
		return;

	if (!flags.test(IRenderVisualFlags::eIgnoreOptimization) && !IsValuableToRender(pVisual, true, phase == PHASE_SMAP, *val_pTransform))
		return;

	vis_data& vis = pVisual->vis;
	bool any_visible = false;
	bool fully_visible = false;
	const bool particle_group = pVisual->Type == MT_PARTICLE_GROUP;
	const bool propagates_masks = particle_group || pVisual->Type == MT_HIERRARHY ||
		pVisual->Type == MT_SKELETON_ANIM || pVisual->Type == MT_SKELETON_RIGID;

	// Leaves and LODs only need union visibility, so they allocate no mask array
	// and stop at the first intersecting frustum. Hierarchies preserve the reduced
	// masks for their children, using inline storage in the overwhelmingly common
	// <=16-frustum case.
	u32 inline_child_masks[inline_frustum_count];
	xr_vector<u32> overflow_child_masks;
	u32* child_masks = nullptr;
	if (propagates_masks)
	{
		child_masks = inline_child_masks;
		if (mask_count > inline_frustum_count)
		{
			overflow_child_masks.resize(mask_count);
			child_masks = overflow_child_masks.data();
		}
		ZeroMemory(child_masks, sizeof(u32) * mask_count);
	}

	for (u32 i = 0; i < mask_count; ++i)
	{
		u32 plane_mask = masks[i];
		if (!plane_mask)
			continue;

		if (plane_mask == fully_visible_mask)
		{
			any_visible = true;
			fully_visible = true;
			if (child_masks)
				child_masks[i] = fully_visible_mask;
			if (!particle_group)
				break;
			continue;
		}

		const EFC_Visible visibility =
			frustums[i].testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), plane_mask);
		if (visibility == fcvNone)
			continue;

		any_visible = true;
		if (visibility == fcvFully)
		{
			fully_visible = true;
			if (child_masks)
				child_masks[i] = fully_visible_mask;
			if (!particle_group)
				break;
		}
		else if (child_masks)
		{
			// testSAABB removes planes that fully contain this node. Propagating the
			// reduced mask saves tests for all descendants of the hierarchy.
			child_masks[i] = plane_mask;
		}

		if (!child_masks)
			break;
	}

	if (!any_visible || !HOM.visible(vis))
		return;

	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
		// Static particle groups are rare and use dynamic child transforms. Keep the
		// legacy path for this special case while all normal static hierarchies use
		// the multi-frustum traversal below.
		for (u32 i = 0; i < mask_count; ++i)
		{
			if (!child_masks[i])
				continue;
			CFrustum& frustum = const_cast<CFrustum&>(frustums[i]);
			set_Frustum(&frustum);
			add_Static(pVisual, frustum.getMask());
		}
		return;

	case MT_HIERRARHY:
	{
		FHierrarhyVisual* hierarchy = (FHierrarhyVisual*)pVisual;
		if (fully_visible)
		{
			for (IRenderVisual* child : hierarchy->children)
				add_leafs_Static((dxRender_Visual*)child);
		}
		else
		{
			for (IRenderVisual* child : hierarchy->children)
				add_Static_MultiFrustum((dxRender_Visual*)child, frustums, child_masks, mask_count);
		}
	}
		return;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		CKinematics* skeleton = (CKinematics*)pVisual;
		skeleton->CalculateBones(TRUE);
		if (fully_visible)
		{
			for (IRenderVisual* child : skeleton->children)
				add_leafs_Static((dxRender_Visual*)child);
		}
		else
		{
			for (IRenderVisual* child : skeleton->children)
				add_Static_MultiFrustum((dxRender_Visual*)child, frustums, child_masks, mask_count);
		}
	}
		return;

	case MT_LOD:
	{
		FLOD* lod = (FLOD*)pVisual;
		float distance;
		float ssa = CalcSSA(distance, lod->vis.sphere.P, lod) * lod->lod_factor;
		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;
			mapLOD_Node* node = mapLOD.insertInAnyWay(distance);
			node->val.ssa = ssa;
			node->val.pVisual = pVisual;
		}
		if (ssa > r_ssaLOD_B || phase == PHASE_SMAP)
		{
			for (IRenderVisual* child : lod->children)
				add_leafs_Static((dxRender_Visual*)child);
		}
	}
		return;

	case MT_TREE_ST:
	case MT_TREE_PM:
	default:
		r_dsgraph_insert_static(pVisual);
		return;
	}
}
