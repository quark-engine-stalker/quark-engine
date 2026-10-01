#ifndef XRENGINE_ISPATIAL_H_INCLUDED
#define XRENGINE_ISPATIAL_H_INCLUDED

//#pragma once
#include "../xrCore/xrPool.h"
#include "../xrCore/xrFrameArena.h"

#include "xr_collide_defs.h"

#pragma pack(push,4)

/*
Requirements:
0. Generic
	* O(1) insertion
		- radius completely determines	"level"
		- position completely detemines "node"
	* O(1) removal
	* 
1. Rendering
	* Should live inside spatial DB
	* Should have at least "bounding-sphere" or "bounding-box"
	* Should have pointer to "sector" it lives in
	* Approximate traversal order relative to point ("camera")
2. Spatial queries
	* Should live inside spatial DB
	* Should have at least "bounding-sphere" or "bounding-box"
*/

const float c_spatial_min = 8.f;

//////////////////////////////////////////////////////////////////////////
enum
{
	STYPE_RENDERABLE = (1 << 0),
	STYPE_LIGHTSOURCE = (1 << 1),
	STYPE_COLLIDEABLE = (1 << 2),
	STYPE_VISIBLEFORAI = (1 << 3),
	STYPE_REACTTOSOUND = (1 << 4),
	STYPE_PHYSIC = (1 << 5),
	STYPE_OBSTACLE = (1 << 6),
	STYPE_SHAPE = (1 << 7),
	STYPE_LIGHTSOURCEHEMI = (1 << 8),
#ifdef SPATIAL_CHANGE
	STYPE_FEELVISIONIGNORE = (1 << 9),
#endif

	STYPEFLAG_INVALIDSECTOR = (1 << 16)
};

//////////////////////////////////////////////////////////////////////////
// Comment: 
//		ordinal objects			- renderable?, collideable?, visibleforAI?
//		physical-decorations	- renderable, collideable
//		lights					- lightsource
//		particles(temp-objects)	- renderable
//		glow					- renderable
//		sound					- ???
//////////////////////////////////////////////////////////////////////////
//class 				IRender_Sector;
//class 				ISpatial;
//class 				ISpatial_NODE;
//class 				ISpatial_DB;

//////////////////////////////////////////////////////////////////////////
// Fast type conversion
//class 			CObject;
//class 			IRenderable;
//class 			IRender_Light;
//
//namespace Feel { class Sound; }

//////////////////////////////////////////////////////////////////////////
class ISpatial_NODE;
class IRender_Sector;
class ISpatial_DB;

namespace Feel
{
	class Sound;
}

class IRenderable;
class IRender_Light;

class XRCDB_API ISpatial
{
private:
	Fvector last_sector_point;
public:
	struct _spatial
	{
		u32 type;
		Fsphere sphere;
		Fvector node_center; // Cached node center for TBV optimization
		float node_radius; // Cached node bounds for TBV optimization
		ISpatial_NODE* node_ptr; // Cached parent node for "empty-members" optimization
		IRender_Sector* sector;
		ISpatial_DB* space; // allow different spaces

		_spatial() : type(0)
		{
		} // safe way to enhure type is zero before any contstructors takes place
	} spatial;

public:
	BOOL spatial_inside();
	void spatial_updatesector_internal();
public:
	virtual void spatial_register();
	virtual void spatial_unregister();
	BENCH_SEC_SCRAMBLEVTBL2
	virtual void spatial_move();
	virtual Fvector spatial_sector_point() { return spatial.sphere.P; }
	ICF void spatial_updatesector()
	{
		if (0 == (spatial.type & STYPEFLAG_INVALIDSECTOR)) return;
		spatial_updatesector_internal();
	};

	virtual CObject* dcast_CObject() { return 0; }
	virtual Feel::Sound* dcast_FeelSound() { return 0; }
	virtual IRenderable* dcast_Renderable() { return 0; }
	virtual IRender_Light* dcast_Light() { return 0; }

	// demonized: Check if eligible for bone calc optimizations
	virtual bool canOptimizeCalculateBones() { return true; }

	ISpatial(ISpatial_DB* space);
	virtual ~ISpatial();
};

// Result of a single spatial-tree traversal against several frustums.
// Each bit in frustum_mask corresponds to the input frustum at the same index.
// Keeping the mask next to the spatial pointer lets callers (sun cascades in
// particular) gather candidates once and defer exact per-pass submission.
struct ISpatialFrustumMask
{
	ISpatial* spatial;
	u32 frustum_mask;
};

//////////////////////////////////////////////////////////////////////////
//class ISpatial_NODE;
class ISpatial_NODE
{
public:
	typedef __w64 unsigned ptrt;
public:
	ISpatial_NODE* parent; // parent node for "empty-members" optimization
	ISpatial_NODE* children [8]; // children nodes
	xr_vector<ISpatial*> items; // own items
public:
	void _init(ISpatial_NODE* _parent);
	void _remove(ISpatial* _S);
	void _insert(ISpatial* _S);

	BOOL _empty()
	{
		return items.empty() && (
			0 == (
				ptrt(children[0]) | ptrt(children[1]) |
				ptrt(children[2]) | ptrt(children[3]) |
				ptrt(children[4]) | ptrt(children[5]) |
				ptrt(children[6]) | ptrt(children[7])
			)
		);
	}
};

////////////


//template <class T, int granularity>
//class	poolSS;
#ifndef	DLL_API
#	define DLL_API					__declspec(dllimport)
#endif // #ifndef	DLL_API

//////////////////////////////////////////////////////////////////////////
class XRCDB_API ISpatial_DB
{
public:
	using ray_filter_callback = BOOL (*)(ISpatial* spatial, LPVOID context);

private:
	// Spatial traversal is read-only after result storage was moved out of the
	// database object. Queries share the lock; tree mutation remains exclusive.
	xrSRWLock cs;

	poolSS<ISpatial_NODE, 128> allocator;

	xr_vector<ISpatial_NODE*> allocator_pool;
	ISpatial* rt_insert_object;
public:
	ISpatial_NODE* m_root;
	Fvector m_center;
	float m_bounds;
	u32 stat_nodes;
	u32 stat_objects;
	CStatTimer stat_insert;
	CStatTimer stat_remove;
private:
	IC u32 _octant(u32 x, u32 y, u32 z) { return z * 4 + y * 2 + x; }
	IC u32 _octant(Fvector& base, Fvector& rel)
	{
		u32 o = 0;
		if (rel.x > base.x) o += 1;
		if (rel.y > base.y) o += 2;
		if (rel.z > base.z) o += 4;
		return o;
	}

	ISpatial_NODE* _node_create();
	void _node_destroy(ISpatial_NODE* & P);

	void _insert(ISpatial_NODE* N, Fvector& n_center, float n_radius);
	void _remove(ISpatial_NODE* N, ISpatial_NODE* N_sub);
public:
	ISpatial_DB();
	~ISpatial_DB();

	// managing
	void initialize(Fbox& BB);
	//void							destroy			();
	void insert(ISpatial* S);
	void remove(ISpatial* S);
	void update(u32 nodes = 8);
	BOOL verify();

public:
	enum
	{
		O_ONLYFIRST = (1 << 0),
		O_ONLYNEAREST = (1 << 1),
		O_ORDERED = (1 << 2),
		O_force_u32 = u32(-1)
	};

	// query
	void q_ray(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_and, const Fvector& _start, const Fvector& _dir,
	           float _range);
	void q_ray(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask_and, const Fvector& _start, const Fvector& _dir,
	           float _range);
	// Returns spatial entries matching at least one bit from _mask_or. This is
	// useful for callers that need one broadphase snapshot for several logical
	// query classes and apply the exact per-class filtering afterwards.
	void q_ray_or(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _start, const Fvector& _dir,
	              float _range);
	void q_ray_or(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _start, const Fvector& _dir,
	              float _range);
	void q_ray_mt(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_and, const Fvector& _start, const Fvector& _dir,
	              float _range);
	void q_ray_mt(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask_and, const Fvector& _start, const Fvector& _dir,
	              float _range);
	// Batch-oriented read access for callers that execute several independent
	// scene queries while holding one shared lock.
	void acquire_read();
	void release_read();
	BOOL q_ray_any_filtered_unlocked(u32 _mask_and, const Fvector& _start, const Fvector& _dir, float _range,
	                                ray_filter_callback filter, LPVOID context);
	BOOL q_ray_any_filtered_or_unlocked(u32 _mask_or, const Fvector& _start, const Fvector& _dir, float _range,
	                                   ray_filter_callback filter, LPVOID context);
	void q_box(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center, const Fvector& _size);
	void q_box(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center, const Fvector& _size);
	// Batch-oriented box query for callers that already hold the shared spatial
	// read lock and need to copy a coherent candidate snapshot before releasing it.
	void q_box_unlocked(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center,
	                    const Fvector& _size);
	void q_box_unlocked(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center,
	                    const Fvector& _size);
	void q_sphere(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center, const float _radius);
	void q_sphere(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center, const float _radius);
	void q_frustum(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const CFrustum& _frustum);
	void q_frustum(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const CFrustum& _frustum);
	// Traverse the octree once for up to 32 frustums and return one entry per
	// spatial with a bit-mask of the frustums it intersects. This is intended for
	// multi-view passes (sun shadow cascades) that otherwise repeat the same tree
	// walk once per view.
	void q_frustums_mask(xr_vector<ISpatialFrustumMask>& R, u32 _o, u32 _mask_or,
	                     const CFrustum* _frustums, u32 _frustum_count);
};

XRCDB_API extern ISpatial_DB* g_SpatialSpace;
XRCDB_API extern ISpatial_DB* g_SpatialSpacePhysic;

#pragma pack(pop)

#endif // #ifndef XRENGINE_ISPATIAL_H_INCLUDED
