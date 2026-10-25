#include "stdafx.h"
#pragma hdrstop

#include "Frustum.h"
#include <intrin.h>

#if defined(__AVX2__) || defined(_M_AVX2)
#	include <immintrin.h>
#endif

int ps_frustum_simd = 1;

//////////////////////////////////////////////////////////////////////
void CFrustum::fplane::cache()
{
	if (positive(n.x))
	{
		if (positive(n.y))
		{
			if (positive(n.z)) aabb_overlap_id = 0;
			else aabb_overlap_id = 1;
		}
		else
		{
			if (positive(n.z)) aabb_overlap_id = 2;
			else aabb_overlap_id = 3;
		}
	}
	else
	{
		if (positive(n.y))
		{
			if (positive(n.z)) aabb_overlap_id = 4;
			else aabb_overlap_id = 5;
		}
		else
		{
			if (positive(n.z)) aabb_overlap_id = 6;
			else aabb_overlap_id = 7;
		}
	}
}

void CFrustum::set_simd_lane(int i)
{
	simd[0][i] = planes[i].n.x;
	simd[1][i] = planes[i].n.y;
	simd[2][i] = planes[i].n.z;
	simd[3][i] = planes[i].d;
}

void CFrustum::_add(Fplane& P)
{
	VERIFY(p_count<FRUSTUM_MAXPLANES);
	planes[p_count].set(P);
	planes[p_count].cache();
	set_simd_lane(p_count);
	p_count ++;
}

void CFrustum::_add(Fvector& P1, Fvector& P2, Fvector& P3)
{
	VERIFY(p_count<FRUSTUM_MAXPLANES);
	planes[p_count].build_precise(P1, P2, P3);
	planes[p_count].cache();
	set_simd_lane(p_count);
	p_count ++;
}

#define			mx			0
#define			my			1
#define			mz			2
#define			Mx			3
#define			My			4
#define			Mz			5

u32 frustum_aabb_remap [8][6] =
{
	{Mx,My,Mz,mx,my,mz},
	{Mx,My,mz,mx,my,Mz},
	{Mx,my,Mz,mx,My,mz},
	{Mx,my,mz,mx,My,Mz},
	{mx,My,Mz,Mx,my,mz},
	{mx,My,mz,Mx,my,Mz},
	{mx,my,Mz,Mx,My,mz},
	{mx,my,mz,Mx,My,Mz}
};

#if defined(__AVX2__) || defined(_M_AVX2)
namespace
{
	// Classifies the volume against eight planes at once, without the per-plane branches
	// of the scalar loop. The verdict does not depend on the plane order (any rejection
	// wins, every fully-containing plane leaves the mask), and the products and sums are
	// done in the scalar order with mul/add (no FMA), so the result is bit-exact.

	// ((n.x * x + n.y * y) + n.z * z) + d, as fplane::classify() and AABB_OverlapPlane()
	ICF __m256 classify8(__m256 nx, __m256 ny, __m256 nz, __m256 d, __m256 x, __m256 y, __m256 z)
	{
		const __m256 xy = _mm256_add_ps(_mm256_mul_ps(nx, x), _mm256_mul_ps(ny, y));
		return _mm256_add_ps(_mm256_add_ps(xy, _mm256_mul_ps(nz, z)), d);
	}

	IC u32 bits8(__m256 mask) { return u32(_mm256_movemask_ps(mask)); }

	// testSphere / testAABB / testSAABB of the scalar code, eight planes per pass
	template <bool use_sphere, bool use_box>
	EFC_Visible test_planes_avx2(const CFrustum& F, const Fvector& c, float r, const float* mM, u32& test_mask)
	{
		const u32 active = test_mask & F.getMask();
		u32 rejected = 0;
		for (u32 base = 0; base < u32(F.p_count); base += 8)
		{
			const u32 lanes = (active >> base) & 0xFFu;
			if (!lanes)
				continue;

			const __m256 nx = _mm256_loadu_ps(F.simd[0] + base);
			const __m256 ny = _mm256_loadu_ps(F.simd[1] + base);
			const __m256 nz = _mm256_loadu_ps(F.simd[2] + base);
			const __m256 d = _mm256_loadu_ps(F.simd[3] + base);
			// the sphere rejects or accepts most volumes, the box is tested only for the
			// planes the sphere straddles
			u32 none = 0, full = 0, inner = lanes;
			if (use_sphere)
			{
				const __m256 radius = _mm256_set1_ps(r);
				const __m256 cls = classify8(nx, ny, nz, d, _mm256_set1_ps(c.x), _mm256_set1_ps(c.y), _mm256_set1_ps(c.z));
				const __m256 abs_cls = _mm256_and_ps(cls, _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff)));
				none = bits8(_mm256_cmp_ps(cls, radius, _CMP_GT_OQ)) & lanes;
				full = bits8(_mm256_cmp_ps(abs_cls, radius, _CMP_GE_OQ)) & lanes;
				inner = none ? 0 : lanes & ~full;
			}
			if (use_box && inner)
			{
				// nearest corner takes max where !(n >= 0), as fplane::cache() decides
				const __m256 zero = _mm256_setzero_ps();
				const __m256 neg_x = _mm256_cmp_ps(nx, zero, _CMP_NGE_UQ);
				const __m256 neg_y = _mm256_cmp_ps(ny, zero, _CMP_NGE_UQ);
				const __m256 neg_z = _mm256_cmp_ps(nz, zero, _CMP_NGE_UQ);
				const __m256 min_x = _mm256_set1_ps(mM[0]), max_x = _mm256_set1_ps(mM[3]);
				const __m256 min_y = _mm256_set1_ps(mM[1]), max_y = _mm256_set1_ps(mM[4]);
				const __m256 min_z = _mm256_set1_ps(mM[2]), max_z = _mm256_set1_ps(mM[5]);
				const __m256 near_cls = classify8(nx, ny, nz, d, _mm256_blendv_ps(min_x, max_x, neg_x),
					_mm256_blendv_ps(min_y, max_y, neg_y), _mm256_blendv_ps(min_z, max_z, neg_z));
				const __m256 far_cls = classify8(nx, ny, nz, d, _mm256_blendv_ps(max_x, min_x, neg_x),
					_mm256_blendv_ps(max_y, min_y, neg_y), _mm256_blendv_ps(max_z, min_z, neg_z));
				none |= bits8(_mm256_cmp_ps(near_cls, zero, _CMP_GT_OQ)) & inner;
				full |= bits8(_mm256_cmp_ps(far_cls, zero, _CMP_LE_OQ)) & inner;
			}
			rejected |= none & lanes;
			test_mask &= ~((full & lanes) << base);
		}
		test_mask = rejected ? 0 : test_mask;
		return rejected ? fcvNone : test_mask ? fcvPartial : fcvFully;
	}

	// The vector pass costs the same for any number of planes; with four or fewer active
	// planes the scalar loop and its early exits are cheaper (measured on the stand).
	IC bool use_avx2(const CFrustum& F, u32 test_mask)
	{
		return ps_frustum_simd && __popcnt(test_mask & F.getMask()) > 4;
	}
}
#endif

//////////////////////////////////////////////////////////////////////
EFC_Visible CFrustum::testSphere(const Fvector& c, float r, u32& test_mask) const
{
#if defined(__AVX2__) || defined(_M_AVX2)
	if (use_avx2(*this, test_mask))
		return test_planes_avx2<true, false>(*this, c, r, nullptr, test_mask);
#endif
	u32 active_mask = test_mask & ((1u << p_count) - 1u);
	while (active_mask)
	{
		unsigned long plane_index;
		_BitScanForward(&plane_index, active_mask);
		const u32 bit = 1u << plane_index;
		active_mask &= active_mask - 1u;

		float cls = planes[plane_index].classify(c);
		if (cls > r)
		{
			test_mask = 0;
			return fcvNone;
		} // none  - return
		if (_abs(cls) >= r) test_mask &= ~bit; // fully - no need to test this plane
	}
	return test_mask ? fcvPartial : fcvFully;
}

BOOL CFrustum::testSphere_dirty(const Fvector& c, float r) const
{
	switch (p_count)
	{
	case 12: if (planes[11].classify(c) > r) return FALSE;
	case 11: if (planes[10].classify(c) > r) return FALSE;
	case 10: if (planes[9].classify(c) > r) return FALSE;
	case 9: if (planes[8].classify(c) > r) return FALSE;
	case 8: if (planes[7].classify(c) > r) return FALSE;
	case 7: if (planes[6].classify(c) > r) return FALSE;
	case 6: if (planes[5].classify(c) > r) return FALSE;
	case 5: if (planes[4].classify(c) > r) return FALSE;
	case 4: if (planes[3].classify(c) > r) return FALSE;
	case 3: if (planes[2].classify(c) > r) return FALSE;
	case 2: if (planes[1].classify(c) > r) return FALSE;
	case 1: if (planes[0].classify(c) > r) return FALSE;
	case 0: break;
	default: NODEFAULT;
	}
	return TRUE;
}

EFC_Visible CFrustum::testAABB(const float* mM, u32& test_mask) const
{
#if defined(__AVX2__) || defined(_M_AVX2)
	if (use_avx2(*this, test_mask))
		return test_planes_avx2<false, true>(*this, Fvector(), 0.f, mM, test_mask);
#endif
	// go for trivial rejection or acceptance using "faster overlap test"
	u32 active_mask = test_mask & ((1u << p_count) - 1u);
	while (active_mask)
	{
		unsigned long plane_index;
		_BitScanForward(&plane_index, active_mask);
		const u32 bit = 1u << plane_index;
		active_mask &= active_mask - 1u;

		EFC_Visible r = AABB_OverlapPlane(planes[plane_index], mM);
		if (fcvFully == r) test_mask &= ~bit; // fully - no need to test this plane
		else if (fcvNone == r)
		{
			test_mask = 0;
			return fcvNone;
		} // none - return
	}
	return test_mask ? fcvPartial : fcvFully;
}

EFC_Visible CFrustum::testSAABB(const Fvector& c, float r, const float* mM, u32& test_mask) const
{
#if defined(__AVX2__) || defined(_M_AVX2)
	if (use_avx2(*this, test_mask))
		return test_planes_avx2<true, true>(*this, c, r, mM, test_mask);
#endif
	u32 active_mask = test_mask & ((1u << p_count) - 1u);
	while (active_mask)
	{
		unsigned long plane_index;
		_BitScanForward(&plane_index, active_mask);
		const u32 bit = 1u << plane_index;
		active_mask &= active_mask - 1u;

		float cls = planes[plane_index].classify(c);
		if (cls > r)
		{
			test_mask = 0;
			return fcvNone;
		} // none  - return
		if (_abs(cls) >= r) test_mask &= ~bit; // fully - no need to test this plane
		else
		{
			EFC_Visible r = AABB_OverlapPlane(planes[plane_index], mM);
			if (fcvFully == r) test_mask &= ~bit; // fully - no need to test this plane
			else if (fcvNone == r)
			{
				test_mask = 0;
				return fcvNone;
			} // none - return
		}
	}
	return test_mask ? fcvPartial : fcvFully;
}

BOOL CFrustum::testPolyInside_dirty(Fvector* p, int count) const
{
	Fvector* e = p + count;
	for (int i = 0; i < p_count; i++)
	{
		const fplane& P = planes[i];
		for (Fvector* I = p; I != e; I++)
			if (P.classify(*I) > 0) return false;
	}
	return true;
}

//////////////////////////////////////////////////////////////////////
void CFrustum::CreateFromPoints(Fvector* p, int count, Fvector& COP)
{
	VERIFY(count<FRUSTUM_MAXPLANES);
	VERIFY(count>=3);

	_clear();
	for (int i = 1; i < count; i++)
		_add(COP, p[i - 1], p[i]);
	_add(COP, p[count - 1], p[0]);
}

void CFrustum::CreateFromPlanes(Fplane* p, int count)
{
	_clear();
	for (int k = 0; k < count; k++)
		planes[k].set(p[k]);

	for (int i = 0; i < count; i++)
	{
		float denom = 1.0f / planes[i].n.magnitude(); // Get magnitude of Vector
		planes[i].n.x *= denom;
		planes[i].n.y *= denom;
		planes[i].n.z *= denom;
		planes[i].d *= denom;
		planes[i].cache();
		set_simd_lane(i);
	}

	p_count = count;
}

void CFrustum::CreateFromPortal(sPoly* poly, Fvector& vPN, Fvector& vBase, Fmatrix& mFullXFORM)
{
	Fplane P;
	P.build_precise((*poly)[0], (*poly)[1], (*poly)[2]);

	if (poly->size() > 6)
	{
		SimplifyPoly_AABB(poly, P);
		P.build_precise((*poly)[0], (*poly)[1], (*poly)[2]);
	}

	// Check plane orientation relative to viewer
	// and reverse if needed
	if (P.classify(vBase) < 0)
	{
		std::reverse(poly->begin(), poly->end());
		P.build_precise((*poly)[0], (*poly)[1], (*poly)[2]);
	}

	// Base creation
	CreateFromPoints(poly->begin(), poly->size(), vBase);

	// Near clipping plane
	_add(P);

	// Far clipping plane
	Fmatrix& M = mFullXFORM;
	P.n.x = -(M._14 - M._13);
	P.n.y = -(M._24 - M._23);
	P.n.z = -(M._34 - M._33);
	P.d = -(M._44 - M._43);
	float denom = 1.0f / P.n.magnitude();
	P.n.x *= denom;
	P.n.y *= denom;
	P.n.z *= denom;
	P.d *= denom;
	_add(P);
}

void CFrustum::SimplifyPoly_AABB(sPoly* poly, Fplane& plane)
{
	Fmatrix mView, mInv;
	Fvector from, up, right, y;
	from.set((*poly)[0]);
	y.set(0, 1, 0);
	if (_abs(plane.n.y) > 0.99f) y.set(1, 0, 0);
	right.crossproduct(y, plane.n);
	up.crossproduct(plane.n, right);
	mView.build_camera_dir(from, plane.n, up);

	// Project and find extents
	Fvector2 min, max;
	min.set(flt_max,flt_max);
	max.set(flt_min,flt_min);
	for (u32 i = 0; i < poly->size(); i++)
	{
		Fvector2 tmp;
		mView.transform_tiny32(tmp, (*poly)[i]);
		min.min(tmp.x, tmp.y);
		max.max(tmp.x, tmp.y);
	}

	// Build other 2 points and inverse project
	Fvector2 p1, p2;
	p1.set(min.x, max.y);
	p2.set(max.x, min.y);
	mInv.invert(mView);
	poly->clear();

	mInv.transform_tiny23(poly->last(), min);
	poly->inc();
	mInv.transform_tiny23(poly->last(), p1);
	poly->inc();
	mInv.transform_tiny23(poly->last(), max);
	poly->inc();
	mInv.transform_tiny23(poly->last(), p2);
	poly->inc();
}

void CFrustum::CreateOccluder(Fvector* p, int count, Fvector& vBase, CFrustum& clip)
{
	VERIFY(count<FRUSTUM_SAFE);
	VERIFY(count>=3);

	BOOL edge[FRUSTUM_SAFE];
	float cls [FRUSTUM_SAFE];
	ZeroMemory(edge, sizeof(edge));
	for (int i = 0; i < clip.p_count; i++)
	{
		// classify all points relative to plane #i
		fplane& P = clip.planes[i];
		for (int j = 0; j < count; j++) cls[j] = _abs(P.classify(p[j]));

		// test edges to see which lies directly on plane
		for (int j = 0; j < count; j++)
		{
			if (cls[j] < EPS_L)
			{
				int next = j + 1;
				if (next >= count) next = 0;
				if (cls[next] < EPS_L)
				{
					// both points lies on plane - mark as 'open'
					edge[j] = true;
				}
			}
		}
	}

	// here we have all edges marked accordenly to 'open' / 'closed' classification
	_clear();
	_add(p[0], p[1], p[2]); // main plane
	for (int i = 0; i < count; i++)
	{
		if (!edge[i])
		{
			int next = i + 1;
			if (next >= count) next = 0;
			_add(vBase, p[i], p[next]);
		}
	}
}

sPoly* CFrustum::ClipPoly(sPoly& S, sPoly& D) const
{
	sPoly* src = &D;
	sPoly* dest = &S;
	for (int i = 0; i < p_count; i++)
	{
		// cache plane and swap lists
		const fplane& P = planes[i];
		std::swap(src, dest);
		dest->clear();

		// classify all points relative to plane #i
		float cls [FRUSTUM_SAFE];
		for (u32 j = 0; j < src->size(); j++) cls[j] = P.classify((*src)[j]);

		// clip everything to this plane
		cls[src->size()] = cls[0];
		src->push_back((*src)[0]);
		Fvector D;
		float denum, t;
		for (int j = 0; j < src->size() - 1; j++)
		{
			if ((*src)[j].similar((*src)[j + 1], EPS_S)) continue;

			if (negative(cls[j]))
			{
				dest->push_back((*src)[j]);
				if (positive(cls[j + 1]))
				{
					// segment intersects plane
					D.sub((*src)[j + 1], (*src)[j]);
					denum = P.n.dotproduct(D);
					if (denum != 0)
					{
						t = -cls[j] / denum; //VERIFY(t<=1.f && t>=0);
						dest->last().mad((*src)[j], D, t);
						dest->inc();
					}
				}
			}
			else
			{
				// J - outside
				if (negative(cls[j + 1]))
				{
					// J+1  - inside
					// segment intersects plane
					D.sub((*src)[j + 1], (*src)[j]);
					denum = P.n.dotproduct(D);
					if (denum != 0)
					{
						t = -cls[j] / denum; //VERIFY(t<=1.f && t>=0);
						dest->last().mad((*src)[j], D, t);
						dest->inc();
					}
				}
			}
		}

		// here we end up with complete polygon in 'dest' which is inside plane #i
		if (dest->size() < 3) return 0;
	}
	return dest;
}

BOOL CFrustum::CreateFromClipPoly(Fvector* p, int count, Fvector& vBase, CFrustum& clip)
{
	VERIFY(count<FRUSTUM_MAXPLANES);
	VERIFY(count>=3);

	sPoly poly1(p, count);
	sPoly poly2;
	sPoly* dest = clip.ClipPoly(poly1, poly2);

	// here we end up with complete frustum-polygon in 'dest'
	if (0 == dest) return false;

	CreateFromPoints(dest->begin(), dest->size(), vBase);
	return true;
}

void CFrustum::CreateFromMatrix(Fmatrix& M, u32 mask)
{
	VERIFY(_valid(M));
	_clear();

	// Left clipping plane
	if (mask & FRUSTUM_P_LEFT)
	{
		planes[p_count].n.x = -(M._14 + M._11);
		planes[p_count].n.y = -(M._24 + M._21);
		planes[p_count].n.z = -(M._34 + M._31);
		planes[p_count].d = -(M._44 + M._41);
		p_count++;
	}

	// Right clipping plane
	if (mask & FRUSTUM_P_RIGHT)
	{
		planes[p_count].n.x = -(M._14 - M._11);
		planes[p_count].n.y = -(M._24 - M._21);
		planes[p_count].n.z = -(M._34 - M._31);
		planes[p_count].d = -(M._44 - M._41);
		p_count++;
	}

	// Top clipping plane
	if (mask & FRUSTUM_P_TOP)
	{
		planes[p_count].n.x = -(M._14 - M._12);
		planes[p_count].n.y = -(M._24 - M._22);
		planes[p_count].n.z = -(M._34 - M._32);
		planes[p_count].d = -(M._44 - M._42);
		p_count++;
	}

	// Bottom clipping plane
	if (mask & FRUSTUM_P_BOTTOM)
	{
		planes[p_count].n.x = -(M._14 + M._12);
		planes[p_count].n.y = -(M._24 + M._22);
		planes[p_count].n.z = -(M._34 + M._32);
		planes[p_count].d = -(M._44 + M._42);
		p_count++;
	}

	// Far clipping plane
	if (mask & FRUSTUM_P_FAR)
	{
		planes[p_count].n.x = -(M._14 - M._13);
		planes[p_count].n.y = -(M._24 - M._23);
		planes[p_count].n.z = -(M._34 - M._33);
		planes[p_count].d = -(M._44 - M._43);
		p_count++;
	}

	// Near clipping plane
	if (mask & FRUSTUM_P_NEAR)
	{
		planes[p_count].n.x = -(M._14 + M._13);
		planes[p_count].n.y = -(M._24 + M._23);
		planes[p_count].n.z = -(M._34 + M._33);
		planes[p_count].d = -(M._44 + M._43);
		p_count++;
	}

	for (int i = 0; i < p_count; i++)
	{
		float denom = 1.0f / planes[i].n.magnitude(); // Get magnitude of Vector
		planes[i].n.x *= denom;
		planes[i].n.y *= denom;
		planes[i].n.z *= denom;
		planes[i].d *= denom;
		planes[i].cache();
		set_simd_lane(i);
	}
}
