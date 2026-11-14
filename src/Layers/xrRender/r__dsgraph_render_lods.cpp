#include "stdafx.h"
#include "flod.h"

#ifdef _EDITOR
#include "igame_persistent.h"
#include "environment.h"
#else
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#endif

extern float r_ssaLOD_A;
extern float r_ssaLOD_B;

void R_dsgraph_structure::r_dsgraph_render_lods(bool _setup_zb, bool _clear)
{
	PROF_EVENT();
	if (_setup_zb) mapLOD.getLR(lstLODs); // front-to-back
	else mapLOD.getRL(lstLODs); // back-to-front
	if (lstLODs.empty()) return;

	// *** Fill VB and generate groups
	u32 shid = _setup_zb ? SE_R1_LMODELS : SE_R1_NORMAL_LQ;
	FLOD* firstV = (FLOD*)lstLODs[0].pVisual;
	ref_selement cur_S = firstV->shader->E[shid];
	float ssaRange = r_ssaLOD_A - r_ssaLOD_B;
	if (ssaRange < EPS_S) ssaRange = EPS_S;

	const u32 uiVertexPerImposter = 4;
	const u32 uiImpostersFit = RCache.Vertex.GetSize()
		/ (firstV->geom->vb_stride * uiVertexPerImposter);

	//Msg						("dbg_lods: shid[%d],firstV[%X]",shid,u32((void*)firstV));
	//Msg						("dbg_lods: shader[%X]",u32((void*)firstV->shader._get()));
	//Msg						("dbg_lods: shader_E[%X]",u32((void*)cur_S._get()));

	for (u32 i = 0; i < lstLODs.size(); i++)
	{
		const u32 iBatchSize = _min(lstLODs.size() - i, uiImpostersFit);
		int cur_count = 0;
		u32 vOffset;
		FLOD::_hw* V = (FLOD::_hw*)RCache.Vertex.Lock(iBatchSize * uiVertexPerImposter, firstV->geom->vb_stride,
		                                              vOffset);

		for (u32 j = 0; j < iBatchSize; ++j, ++i)
		{
			// sort out redundancy
			R_dsgraph::_LodItem& P = lstLODs[i];
			if (P.pVisual->shader->E[shid] == cur_S) cur_count++;
			else
			{
				lstLODgroups.push_back(cur_count);
				cur_S = P.pVisual->shader->E[shid];
				cur_count = 1;
			}

			// calculate alpha
			float ssaDiff = P.ssa - r_ssaLOD_B;
			float scale = ssaDiff / ssaRange;
			int iA = iFloor((1 - scale) * 255.f);
			u32 uA = u32(clampr(iA, 0, 255));

			// calculate direction and shift
			FLOD* lodV = (FLOD*)P.pVisual;
			Fvector Ldir, shift;
			Ldir.sub(lodV->vis.sphere.P, Device.vCameraPosition).normalize();
			shift.mul(Ldir, -.5f * lodV->vis.sphere.R);

			// gen geometry
			FLOD::_face* facets = lodV->facets;
			// The three facets facing the camera most, as the end of the stable sort of the eight dot products left
			// them (8 elements: insertion sort): a later facet with the same dot product ranks higher.
			float best_dot[3];
			u32 best_id[3];
			u32 best_count = 0;
			for (u32 s = 0; s < 8; s++)
			{
				const float dot = Ldir.dotproduct(facets[s].N);
				u32 pos = best_count;
				while (pos > 0 && dot >= best_dot[pos - 1])
					--pos;
				if (pos == 3)
					continue;
				for (u32 k = _min(best_count, 2u); k > pos; --k)
				{
					best_dot[k] = best_dot[k - 1];
					best_id[k] = best_id[k - 1];
				}
				best_dot[pos] = dot;
				best_id[pos] = s;
				best_count = _min(best_count + 1, 3u);
			}

			float dot_best = best_dot[0];
			float dot_next = best_dot[1];
			float dot_next_2 = best_dot[2];
			u32 id_best = best_id[0];
			u32 id_next = best_id[1];

			// Now we have two "best" planes, calculate factor, and approx normal
			float fA = dot_best, fB = dot_next, fC = dot_next_2;
			float alpha = 0.5f + 0.5f * (1 - (fB - fC) / (fA - fC));
			int iF = iFloor(alpha * 255.5f);
			u32 uF = u32(clampr(iF, 0, 255));

			// Fill VB
			FLOD::_face& FA = facets[id_best];
			FLOD::_face& FB = facets[id_next];
			static int vid [4] = {3, 0, 2, 1};
			for (u32 vit = 0; vit < 4; vit++)
			{
				int id = vid[vit];
				V->p0.add(FB.v[id].v, shift);
				V->p1.add(FA.v[id].v, shift);
				V->n0 = FB.N;
				V->n1 = FA.N;
				V->sun_af = color_rgba(FB.v[id].c_sun, FA.v[id].c_sun, uA, uF);
				V->t0 = FB.v[id].t;
				V->t1 = FA.v[id].t;
				V->rgbh0 = FB.v[id].c_rgb_hemi;
				V->rgbh1 = FA.v[id].c_rgb_hemi;
				V++;
			}
		}
		lstLODgroups.push_back(cur_count);
		RCache.Vertex.Unlock(iBatchSize * uiVertexPerImposter, firstV->geom->vb_stride);

		// *** Render
		RCache.set_xform_world(Fidentity);
		for (u32 uiPass = 0; uiPass < SHADER_PASSES_MAX; ++uiPass)
		{
			int current = 0;
			u32 vCurOffset = vOffset;

			for (u32 g = 0; g < lstLODgroups.size(); g++)
			{
				int p_count = lstLODgroups[g];
				u32 uiNumPasses = lstLODs[current].pVisual->shader->E[shid]->passes.size();
				if (uiPass < uiNumPasses)
				{
					RCache.set_Element(lstLODs[current].pVisual->shader->E[shid], uiPass);
					RCache.set_Geometry(firstV->geom);
					RCache.Render(D3DPT_TRIANGLELIST, vCurOffset, 0, 4 * p_count, 0, 2 * p_count);
				}
				RCache.stat.r.s_flora_lods.add(4 * p_count);
				current += p_count;
				vCurOffset += 4 * p_count;
			}
		}

		lstLODgroups.clear();
	}

	lstLODs.clear();

	if (_clear) mapLOD.clear();
}
