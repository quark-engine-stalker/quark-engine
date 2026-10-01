// Frustum.h: interface for the CFrustum class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_FRUSTUM_H__E66ED755_F741_49CF_8B2A_404CCF7067F2__INCLUDED_)
#define AFX_FRUSTUM_H__E66ED755_F741_49CF_8B2A_404CCF7067F2__INCLUDED_

#include "xrCDB.h"

//#pragma once

#include "../xrcore/fixedvector.h"

#pragma pack(push,4)

enum EFC_Visible
{
	fcvNone = 0,
	fcvPartial,
	fcvFully,
	fcv_forcedword = u32(-1)
};


#define FRUSTUM_MAXPLANES	12
#define FRUSTUM_P_LEFT		(1<<0)
#define FRUSTUM_P_RIGHT		(1<<1)
#define FRUSTUM_P_TOP		(1<<2)
#define FRUSTUM_P_BOTTOM	(1<<3)
#define FRUSTUM_P_NEAR		(1<<4)
#define FRUSTUM_P_FAR		(1<<5)

#define FRUSTUM_P_LRTB		(FRUSTUM_P_LEFT|FRUSTUM_P_RIGHT|FRUSTUM_P_TOP|FRUSTUM_P_BOTTOM)
#define FRUSTUM_P_ALL		(FRUSTUM_P_LRTB|FRUSTUM_P_NEAR|FRUSTUM_P_FAR)

#define FRUSTUM_SAFE		(FRUSTUM_MAXPLANES*4)
typedef svector<Fvector,FRUSTUM_SAFE> sPoly;
extern u32 frustum_aabb_remap[8][6];

class XRCDB_API CFrustum
{
public:
	struct fplane : public Fplane
	{
		u32 aabb_overlap_id; // [0..7]
		void cache();
	};

	fplane planes [FRUSTUM_MAXPLANES];
	int p_count;

public:
	ICF EFC_Visible AABB_OverlapPlane(const fplane& P, const float* mM) const
	{
		// Derive the extreme-point indices from the cached normal signs. The old
		// lookup table required six dependent index loads before the six AABB
		// component loads in every plane test. This keeps the exact same point and
		// arithmetic order, but turns the indices into cheap integer arithmetic.
		const u32 signs = P.aabb_overlap_id;
		const u32 neg_x = ((signs >> 2) & 1u) * 3u;
		const u32 neg_y = 1u + ((signs >> 1) & 1u) * 3u;
		const u32 neg_z = 2u + (signs & 1u) * 3u;

		const float neg_classification = P.n.x * mM[neg_x] + P.n.y * mM[neg_y] +
			P.n.z * mM[neg_z] + P.d;
		if (neg_classification > 0.f) return fcvNone;

		const u32 pos_x = 3u - neg_x;
		const u32 pos_y = 5u - neg_y;
		const u32 pos_z = 7u - neg_z;
		const float pos_classification = P.n.x * mM[pos_x] + P.n.y * mM[pos_y] +
			P.n.z * mM[pos_z] + P.d;
		if (pos_classification <= 0.f) return fcvFully;

		return fcvPartial;
	}

public:
	IC void _clear() { p_count = 0; }
	void _add(Fplane& P);
	void _add(Fvector& P1, Fvector& P2, Fvector& P3);

	void SimplifyPoly_AABB(sPoly* P, Fplane& plane);

	void CreateOccluder(Fvector* p, int count, Fvector& vBase, CFrustum& clip);
	BOOL CreateFromClipPoly(Fvector* p, int count, Fvector& vBase, CFrustum& clip);
	// returns 'false' if creation failed
	void CreateFromPoints(Fvector* p, int count, Fvector& vBase);
	void CreateFromMatrix(Fmatrix& M, u32 mask);
	void CreateFromPortal(sPoly* P, Fvector& vPN, Fvector& vBase, Fmatrix& mFullXFORM);
	void CreateFromPlanes(Fplane* p, int count);

	sPoly* ClipPoly(sPoly& src, sPoly& dest) const;

	u32 getMask() const { return (1 << p_count) - 1; }

	EFC_Visible testSphere(const Fvector& c, float r, u32& test_mask) const;
	BOOL testSphere_dirty(const Fvector& c, float r) const;
	EFC_Visible testAABB(const float* mM, u32& test_mask) const;
	EFC_Visible testSAABB(const Fvector& c, float r, const float* mM, u32& test_mask) const;
	BOOL testPolyInside_dirty(Fvector* p, int count) const;

	IC BOOL testPolyInside(sPoly& src) const
	{
		sPoly d;
		return !!ClipPoly(src, d);
	}

	IC BOOL testPolyInside(Fvector* p, int count) const
	{
		sPoly src(p, count);
		return testPolyInside(src);
	}
};
#pragma pack(pop)

#endif // !defined(AFX_FRUSTUM_H__E66ED755_F741_49CF_8B2A_404CCF7067F2__INCLUDED_)
