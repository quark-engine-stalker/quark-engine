#include "stdafx.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "fvf.h"

CPortalTraverser PortalTraverser;
extern int ps_r__portal_traverse_optimized;

CPortalTraverser::CPortalTraverser()
{
	i_marker = 0xffffffff;
	traversal_task_cursor = 0;
	traversal_tasks.reserve(64);
	r_sectors.reserve(64);
	f_portals.reserve(16);
}

#ifdef DEBUG
xr_vector<IRender_Sector*>				dbg_sectors;
#endif

void CPortalTraverser::traverse(IRender_Sector* start, CFrustum& F, Fvector& vBase, Fmatrix& mXFORM, u32 options)
{
	Fmatrix m_viewport_01 = {
		1.f / 2.f, 0.0f, 0.0f, 0.0f,
		0.0f, -1.f / 2.f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		1.f / 2.f + 0 + 0, 1.f / 2.f + 0 + 0, 0.0f, 1.0f
	};

	if (options & VQ_FADE)
		f_portals.clear_and_reserve();

	// VERIFY is compiled out in Release. Advance the marker and clear all output
	// first so a temporarily missing start sector cannot reuse visibility from a
	// previous traversal or dereference a null CSector.
	++i_marker;
	i_options = options;
	i_vBase = vBase;
	i_mXFORM = mXFORM;
	i_mXFORM_01.mul(m_viewport_01, mXFORM);
	i_start = (CSector*)start;
	r_sectors.clear_and_reserve();
	traversal_tasks.clear_and_reserve();
	traversal_task_cursor = 0;
	if (!i_start)
		return;

	_scissor scissor;
	scissor.set(0, 0, 1, 1);
	scissor.depth = 0;

	if (ps_r__portal_traverse_optimized)
	{
		enqueue(i_start, F, scissor);

		// Iterative traversal removes recursion depth from complex portal graphs. A
		// task is copied before processing because enqueue() may reallocate the vector.
		while (traversal_task_cursor < traversal_tasks.size())
		{
			TraversalTask task = traversal_tasks[traversal_task_cursor++];
			if (!task.sector)
				continue;
			task.sector->traverse(task.frustum, task.scissor);
		}
	}
	else
	{
		// Compatibility fallback: original recursive/global-portal-marker path.
		i_start->traverse(F, scissor);
	}

	if (options & VQ_SCISSOR)
	{
		// dbg_sectors					= r_sectors;
		// merge scissor info
		for (u32 s = 0; s < r_sectors.size(); s++)
		{
			CSector* S = (CSector*)r_sectors[s];
			S->r_scissor_merged.invalidate();
			S->r_scissor_merged.depth = flt_max;
			for (u32 it = 0; it < S->r_scissors.size(); it++)
			{
				S->r_scissor_merged.merge(S->r_scissors[it]);
				if (S->r_scissors[it].depth < S->r_scissor_merged.depth)
					S->r_scissor_merged.depth = S->r_scissors[it].depth;
			}
		}
	}
}

void CPortalTraverser::enqueue(CSector* sector, const CFrustum& frustum, const _scissor& scissor)
{
	if (!sector)
		return;
	traversal_tasks.push_back(TraversalTask(sector, frustum, scissor));
}

void CPortalTraverser::prepare_portal_cache(CPortal* portal)
{
	VERIFY(portal);
	if (portal->traversal_cache_marker == i_marker)
		return;

	portal->traversal_cache_marker = i_marker;

	portal->traversal_cache_ssa = flt_max;
	if (i_options & VQ_SSA)
	{
		Fvector dir2portal;
		dir2portal.sub(portal->S.P, i_vBase);
		const float distSQ = dir2portal.square_magnitude() + EPS;
		dir2portal.mul(1.f / _sqrt(distSQ));
		portal->traversal_cache_ssa =
			(portal->S.R * portal->S.R / distSQ) * _abs(portal->P.n.dotproduct(dir2portal));
	}

	portal->traversal_cache_screen.invalidate();
	portal->traversal_cache_screen_valid = FALSE;
	if (!(i_options & VQ_SCISSOR))
		return;

	portal->traversal_cache_screen_valid = TRUE;
	svector<Fvector, 8>& poly = portal->getPoly();
	for (u32 i = 0; i < poly.size(); ++i)
	{
		const Fvector& v = poly[i];
		Fvector4 t;
		t.x = v.x * i_mXFORM_01._11 + v.y * i_mXFORM_01._21 + v.z * i_mXFORM_01._31 + i_mXFORM_01._41;
		t.y = v.x * i_mXFORM_01._12 + v.y * i_mXFORM_01._22 + v.z * i_mXFORM_01._32 + i_mXFORM_01._42;
		t.z = v.x * i_mXFORM_01._13 + v.y * i_mXFORM_01._23 + v.z * i_mXFORM_01._33 + i_mXFORM_01._43;
		t.w = v.x * i_mXFORM_01._14 + v.y * i_mXFORM_01._24 + v.z * i_mXFORM_01._34 + i_mXFORM_01._44;

		// Portals crossing the eye/near plane need the precise clipped path.
		if (t.w <= EPS)
		{
			portal->traversal_cache_screen_valid = FALSE;
			break;
		}

		t.mul(1.f / t.w);
		Fvector2 projected;
		projected.set(t.x, t.y);
		portal->traversal_cache_screen.modify(projected);
	}
}

void CPortalTraverser::fade_portal(CPortal* _p, float ssa)
{
	if (_p->fade_marker == i_marker)
		return;

	_p->fade_marker = i_marker;
	f_portals.push_back(mk_pair(_p, ssa));
}

void CPortalTraverser::initialize()
{
	f_shader.create("portal");
	f_geom.create(FVF::F_L, RCache.Vertex.Buffer(), 0);
}

void CPortalTraverser::destroy()
{
	release_level_memory();
	f_geom.destroy();
	f_shader.destroy();
}

void CPortalTraverser::release_level_memory()
{
	r_sectors.clear_and_free();
	f_portals.clear_and_free();
	traversal_tasks.clear_and_free();
	traversal_task_cursor = 0;
	i_start = nullptr;
}

ICF bool psort_pred(const std::pair<CPortal*, float>& _1, const std::pair<CPortal*, float>& _2)
{
	float d1 = PortalTraverser.i_vBase.distance_to_sqr(_1.first->S.P);
	float d2 = PortalTraverser.i_vBase.distance_to_sqr(_2.first->S.P);
	return d2 > d1; // descending, back to front
}

extern float r_ssaDISCARD;
extern float r_ssaLOD_A, r_ssaLOD_B;

void CPortalTraverser::fade_render()
{
	if (f_portals.empty()) return;

	// re-sort, back to front
	std::sort(f_portals.begin(), f_portals.end(), psort_pred);

	// calc poly-count
	u32 _pcount = 0;
	for (u32 _it = 0; _it < f_portals.size(); _it++) _pcount += f_portals[_it].first->getPoly().size() - 2;

	// fill buffers
	u32 _offset = 0;
	FVF::L* _v = (FVF::L*)RCache.Vertex.Lock(_pcount * 3, f_geom.stride(), _offset);
	float ssaRange = r_ssaLOD_A - r_ssaLOD_B;
	Fvector _ambient_f = g_pGamePersistent->Environment().CurrentEnv->ambient;
	u32 _ambient = color_rgba_f(_ambient_f.x, _ambient_f.y, _ambient_f.z, 0);
	for (u32 _it = 0; _it < f_portals.size(); _it++)
	{
		std::pair<CPortal*, float>& fp = f_portals[_it];
		CPortal* _P = fp.first;
		float _ssa = fp.second;
		float ssaDiff = _ssa - r_ssaLOD_B;
		float ssaScale = ssaDiff / ssaRange;
		int iA = iFloor((1 - ssaScale) * 255.5f);
		clamp(iA, 0, 255);
		u32 _clr = subst_alpha(_ambient, u32(iA));

		// fill polys
		u32 _polys = _P->getPoly().size() - 2;
		for (u32 _pit = 0; _pit < _polys; _pit++)
		{
			_v->set(_P->getPoly()[0], _clr);
			_v++;
			_v->set(_P->getPoly()[_pit + 1], _clr);
			_v++;
			_v->set(_P->getPoly()[_pit + 2], _clr);
			_v++;
		}
	}
	RCache.Vertex.Unlock(_pcount * 3, f_geom.stride());

	// render
	RCache.set_xform_world(Fidentity);
	RCache.set_Shader(f_shader);
	RCache.set_Geometry(f_geom);
	RCache.set_CullMode(CULL_NONE);
	RCache.Render(D3DPT_TRIANGLELIST, _offset, _pcount);
	RCache.set_CullMode(CULL_CCW);

	// cleanup
	f_portals.clear();
}

#ifdef DEBUG
void CPortalTraverser::dbg_draw		()
{
	RCache.OnFrameEnd		();
	RCache.set_xform_world	(Fidentity);
	RCache.set_xform_view	(Fidentity);
	RCache.set_xform_project(Fidentity);
	for (u32 s=0; s<dbg_sectors.size(); s++)	{
		CSector*	S		= (CSector*)dbg_sectors[s];
		FVF::L		verts	[5];
		Fbox2		bb		= S->r_scissor_merged;
		bb.min.x			= bb.min.x * 2 - 1;
		bb.max.x			= bb.max.x * 2 - 1;
		bb.min.y			= (1-bb.min.y) * 2 - 1;
		bb.max.y			= (1-bb.max.y) * 2 - 1;

		verts[0].set(bb.min.x,bb.min.y,EPS,0xffffffff);
		verts[1].set(bb.max.x,bb.min.y,EPS,0xffffffff);
		verts[2].set(bb.max.x,bb.max.y,EPS,0xffffffff);
		verts[3].set(bb.min.x,bb.max.y,EPS,0xffffffff);
		verts[4].set(bb.min.x,bb.min.y,EPS,0xffffffff);
		RCache.dbg_Draw		(D3DPT_LINESTRIP,verts,4);
	}
}
#endif
