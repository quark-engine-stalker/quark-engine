#include "stdafx.h"
#include "ISpatial.h"

#if defined(__AVX2__) || defined(_M_AVX2)
#	include <immintrin.h>
#endif

extern Fvector c_spatial_offset[8];

#if defined(__AVX2__) || defined(_M_AVX2)
namespace
{
	constexpr u32 spatial_box_avx2_batch_size = 8;

	// Keep the pointer-based ISpatial storage compatible with every game object and
	// addon.  Only the transient culling input is converted to a tiny SoA batch.
	u32 test_box_batch_avx2(ISpatial* const* items, const u32 type_mask, const Fvector& box_min,
		const Fvector& box_max)
	{
		alignas(32) float center_x[spatial_box_avx2_batch_size];
		alignas(32) float center_y[spatial_box_avx2_batch_size];
		alignas(32) float center_z[spatial_box_avx2_batch_size];
		alignas(32) float radius[spatial_box_avx2_batch_size];
		u32 active_mask = 0;

		for (u32 lane = 0; lane < spatial_box_avx2_batch_size; ++lane)
		{
			const ISpatial* spatial = items[lane];
			const Fsphere& sphere = spatial->spatial.sphere;
			center_x[lane] = sphere.P.x;
			center_y[lane] = sphere.P.y;
			center_z[lane] = sphere.P.z;
			radius[lane] = sphere.R;
			if (spatial->spatial.type & type_mask)
				active_mask |= 1u << lane;
		}

		if (!active_mask)
			return 0;

		const __m256 x = _mm256_load_ps(center_x);
		const __m256 y = _mm256_load_ps(center_y);
		const __m256 z = _mm256_load_ps(center_z);
		const __m256 r = _mm256_load_ps(radius);
		const __m256 min_x = _mm256_set1_ps(box_min.x);
		const __m256 min_y = _mm256_set1_ps(box_min.y);
		const __m256 min_z = _mm256_set1_ps(box_min.z);
		const __m256 max_x = _mm256_set1_ps(box_max.x);
		const __m256 max_y = _mm256_set1_ps(box_max.y);
		const __m256 max_z = _mm256_set1_ps(box_max.z);

		__m256 rejected = _mm256_cmp_ps(_mm256_add_ps(x, r), min_x, _CMP_LT_OQ);
		rejected = _mm256_or_ps(rejected, _mm256_cmp_ps(_mm256_add_ps(y, r), min_y, _CMP_LT_OQ));
		rejected = _mm256_or_ps(rejected, _mm256_cmp_ps(_mm256_add_ps(z, r), min_z, _CMP_LT_OQ));
		rejected = _mm256_or_ps(rejected, _mm256_cmp_ps(_mm256_sub_ps(x, r), max_x, _CMP_GT_OQ));
		rejected = _mm256_or_ps(rejected, _mm256_cmp_ps(_mm256_sub_ps(y, r), max_y, _CMP_GT_OQ));
		rejected = _mm256_or_ps(rejected, _mm256_cmp_ps(_mm256_sub_ps(z, r), max_z, _CMP_GT_OQ));
		return active_mask & ~static_cast<u32>(_mm256_movemask_ps(rejected));
	}
}
#endif

template <bool b_first, typename result_type>
class walker
{
public:
	u32 mask;
	Fvector box_min;
	Fvector box_max;
	result_type* result;
public:
	walker(result_type& _result, u32 _mask, const Fvector& _center, const Fvector& _size)
	{
		mask = _mask;
		box_min.sub(_center, _size);
		box_max.add(_center, _size);
		result = &_result;
	}

	void walk(ISpatial_NODE* N, Fvector& n_C, float n_R)
	{
		// box
		const float n_vR = 2.f * n_R;
		if (n_C.x + n_vR < box_min.x || n_C.y + n_vR < box_min.y || n_C.z + n_vR < box_min.z ||
			n_C.x - n_vR > box_max.x || n_C.y - n_vR > box_max.y || n_C.z - n_vR > box_max.z)
			return;

		// test items
		u32 item_index = 0;
		const u32 item_count = static_cast<u32>(N->items.size());

#if defined(__AVX2__) || defined(_M_AVX2)
		for (; item_index + spatial_box_avx2_batch_size <= item_count; item_index += spatial_box_avx2_batch_size)
		{
			const u32 visible_mask = test_box_batch_avx2(
				&N->items[item_index], mask, box_min, box_max);
			for (u32 lane = 0; lane < spatial_box_avx2_batch_size; ++lane)
			{
				if (!(visible_mask & (1u << lane)))
					continue;
				result->push_back(N->items[item_index + lane]);
				if (b_first)
					return;
			}
		}
#endif

		for (; item_index < item_count; ++item_index)
		{
			ISpatial* S = N->items[item_index];
			if (0 == (S->spatial.type & mask)) continue;

			const Fvector& sC = S->spatial.sphere.P;
			const float sR = S->spatial.sphere.R;
			if (sC.x + sR < box_min.x || sC.y + sR < box_min.y || sC.z + sR < box_min.z ||
				sC.x - sR > box_max.x || sC.y - sR > box_max.y || sC.z - sR > box_max.z)
				continue;

			result->push_back(S);
			if (b_first) return;
		}

		// recurse
		float c_R = n_R / 2;
		for (u32 octant = 0; octant < 8; octant++)
		{
			if (0 == N->children[octant]) continue;
			Fvector c_C;
			c_C.mad(n_C, c_spatial_offset[octant], c_R);
			walk(N->children[octant], c_C, c_R);
			if (b_first && !result->empty()) return;
		}
	}
};

template <typename result_type>
static void q_box_unlocked_impl(ISpatial_DB& space, result_type& result, u32 options, u32 mask,
	const Fvector& center, const Fvector& size)
{
	result.clear_not_free();
	if (!space.m_root || !mask)
		return;

	if (options & ISpatial_DB::O_ONLYFIRST)
	{
		walker<true, result_type> query(result, mask, center, size);
		query.walk(space.m_root, space.m_center, space.m_bounds);
	}
	else
	{
		walker<false, result_type> query(result, mask, center, size);
		query.walk(space.m_root, space.m_center, space.m_bounds);
	}
}

void ISpatial_DB::q_box_unlocked(xr_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center,
	const Fvector& _size)
{
	q_box_unlocked_impl(*this, R, _o, _mask, _center, _size);
}

void ISpatial_DB::q_box_unlocked(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center,
	const Fvector& _size)
{
	q_box_unlocked_impl(*this, R, _o, _mask, _center, _size);
}

void ISpatial_DB::q_box(xr_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center, const Fvector& _size)
{
	xrSRWLockGuard guard(cs, true);
	q_box_unlocked_impl(*this, R, _o, _mask, _center, _size);
}

void ISpatial_DB::q_box(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center,
	const Fvector& _size)
{
	xrSRWLockGuard guard(cs, true);
	q_box_unlocked_impl(*this, R, _o, _mask, _center, _size);
}

void ISpatial_DB::q_sphere(xr_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center, const float _radius)
{
	Fvector _size = {_radius, _radius, _radius};
	q_box(R, _o, _mask, _center, _size);
}

void ISpatial_DB::q_sphere(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center,
	const float _radius)
{
	Fvector _size = {_radius, _radius, _radius};
	q_box(R, _o, _mask, _center, _size);
}
