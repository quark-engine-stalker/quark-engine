// Portal.h: interface for the CPortal class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(_PORTAL_H_)
#define _PORTAL_H_
#pragma once

class CPortal;
class CSector;

struct _scissor : public Fbox2
{
	float depth;
};

// Connector
class CPortal : public IRender_Portal
#ifdef DEBUG
	, public pureRender
#endif
{
private:
	svector<Fvector, 8> poly;
	CSector *pFace, *pBack;
public:
	Fplane P;
	Fsphere S;
	u32 marker;
	BOOL bDualRender;

	// Per-traversal cache. The camera and view-projection matrix are stable for
	// one CPortalTraverser marker, so SSA and conservative screen bounds only
	// need to be calculated once even when the portal is reached from several
	// sector frustums.
	u32 traversal_cache_marker;
	float traversal_cache_ssa;
	Fbox2 traversal_cache_screen;
	BOOL traversal_cache_screen_valid;
	u32 fade_marker;

	void Setup(const Fvector* V, int vcnt, CSector* face, CSector* back);

	svector<Fvector, 8>& getPoly() { return poly; }
	CSector* Back() { return pBack; }
	CSector* Front() { return pFace; }
	CSector* getSector(CSector* pFrom) { return pFrom == pFace ? pBack : pFace; }

	CSector* getSectorFacing(const Fvector& V)
	{
		if (P.classify(V) > 0) return pFace;
		else return pBack;
	}

	CSector* getSectorBack(const Fvector& V)
	{
		if (P.classify(V) > 0) return pBack;
		else return pFace;
	}

	float distance(const Fvector& V) { return _abs(P.classify(V)); }

	CPortal();
	virtual ~CPortal();

#ifdef DEBUG
	virtual void					OnRender		();
#endif
};

class dxRender_Visual;

// Main 'Sector' class
class CSector : public IRender_Sector
{
protected:
	dxRender_Visual* m_root; // whole geometry of that sector
	xr_vector<CPortal*> m_portals;
public:
	xr_vector<CFrustum> r_frustums;
	xr_vector<_scissor> r_scissors;
	xr_vector<CPortal*> r_visited_portals;
	_scissor r_scissor_merged;
	u32 r_marker;
public:
	// Main interface
	dxRender_Visual* root() { return m_root; }
	void traverse(CFrustum& F, _scissor& R);
	void load(IReader& fs);

	CSector()
	{
		m_root = NULL;
		r_marker = 0xffffffff;
		r_frustums.reserve(4);
		r_scissors.reserve(4);
		r_visited_portals.reserve(8);
	}
	virtual ~CSector();
};

class CPortalTraverser
{
public:
	struct TraversalTask
	{
		CSector* sector;
		CFrustum frustum;
		_scissor scissor;

		TraversalTask(CSector* value_sector, const CFrustum& value_frustum, const _scissor& value_scissor)
			: sector(value_sector), frustum(value_frustum), scissor(value_scissor)
		{
		}
	};

	enum
	{
		VQ_HOM = (1 << 0),
		VQ_SSA = (1 << 1),
		VQ_SCISSOR = (1 << 2),
		VQ_FADE = (1 << 3),
		// requires SSA to work
	};

public:
	u32 i_marker; // input
	u32 i_options; // input:	culling options
	Fvector i_vBase; // input:	"view" point
	Fmatrix i_mXFORM; // input:	4x4 xform
	Fmatrix i_mXFORM_01; // 
	CSector* i_start; // input:	starting point
	xr_vector<IRender_Sector*> r_sectors; // result
	xr_vector<std::pair<CPortal*, float>> f_portals; // 
	ref_shader f_shader;
	ref_geom f_geom;

	// Reused traversal workspace. Keeping it on the traverser removes recursive
	// stack growth and repeated temporary polygon/vector allocation.
	xr_vector<TraversalTask> traversal_tasks;
	u32 traversal_task_cursor;
	sPoly clip_source;
	sPoly clip_dest;
public:
	CPortalTraverser();
	void initialize();
	void destroy();
	void release_level_memory();
	void traverse(IRender_Sector* start, CFrustum& F, Fvector& vBase, Fmatrix& mXFORM, u32 options);
	void enqueue(CSector* sector, const CFrustum& frustum, const _scissor& scissor);
	void prepare_portal_cache(CPortal* portal);
	void fade_portal(CPortal* _p, float ssa);
	void fade_render();
#ifdef DEBUG
	void							dbg_draw		();
#endif
};

extern CPortalTraverser PortalTraverser;

#endif // !defined(AFX_PORTAL_H__1FC2D371_4A19_49EA_BD1E_2D0F8DEBBF15__INCLUDED_)
