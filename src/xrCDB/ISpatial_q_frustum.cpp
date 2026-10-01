#include "stdafx.h"
#include "ISpatial.h"
#include "frustum.h"

#if defined(__AVX2__) || defined(_M_AVX2)
#	include <immintrin.h>
#endif

extern Fvector c_spatial_offset[8];

#if defined(__AVX2__) || defined(_M_AVX2)
namespace
{
	constexpr u32 spatial_avx2_batch_size = 8;

	// ISpatial nodes keep pointers to objects, not a structure-of-arrays.  Packing one
	// node's sphere data into a small stack SoA is still worthwhile for dense nodes:
	// it lets the six frustum planes classify eight objects with 256-bit operations.
	// The mask returned here preserves both the original item order and testSphere's
	// rejection rule; the per-sphere test mask is local in q_frustum and is not used
	// after the visibility decision.
	u32 test_sphere_batch_avx2(ISpatial* const* items, const u32 type_mask, const CFrustum& frustum,
		u32 frustum_mask)
	{
		alignas(32) float center_x[spatial_avx2_batch_size];
		alignas(32) float center_y[spatial_avx2_batch_size];
		alignas(32) float center_z[spatial_avx2_batch_size];
		alignas(32) float radius[spatial_avx2_batch_size];
		u32 active_mask = 0;

		for (u32 lane = 0; lane < spatial_avx2_batch_size; ++lane)
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
		u32 visible_mask = active_mask;

		for (int plane_index = 0; plane_index < frustum.p_count && visible_mask;
			++plane_index, frustum_mask >>= 1)
		{
			if (!(frustum_mask & 1u))
				continue;

			const CFrustum::fplane& plane = frustum.planes[plane_index];
			const __m256 classify_x = _mm256_mul_ps(x, _mm256_set1_ps(plane.n.x));
			const __m256 classify_xy = _mm256_add_ps(classify_x, _mm256_mul_ps(y, _mm256_set1_ps(plane.n.y)));
			const __m256 classify_xyz = _mm256_add_ps(classify_xy, _mm256_mul_ps(z, _mm256_set1_ps(plane.n.z)));
			const __m256 classify = _mm256_add_ps(classify_xyz, _mm256_set1_ps(plane.d));
			const u32 rejected = static_cast<u32>(_mm256_movemask_ps(_mm256_cmp_ps(classify, r, _CMP_GT_OQ)));
			visible_mask &= ~rejected;
		}

		return visible_mask;
	}
}
#endif

template <typename result_type>
class walker
{
public:
	u32 mask;
	CFrustum* F;
	result_type* result;
public:
	walker(result_type& _result, u32 _mask, const CFrustum* _F)
	{
		mask = _mask;
		F = (CFrustum*)_F;
		result = &_result;
	}

	void walk(ISpatial_NODE* N, Fvector& n_C, float n_R, u32 fmask)
	{
		// box
		float n_vR = 2 * n_R;
		Fbox BB;
		BB.set(n_C.x - n_vR, n_C.y - n_vR, n_C.z - n_vR, n_C.x + n_vR, n_C.y + n_vR, n_C.z + n_vR);
		if (fcvNone == F->testAABB(BB.data(), fmask)) return;

		// test items
		u32 item_index = 0;
		const u32 item_count = static_cast<u32>(N->items.size());

#if defined(__AVX2__) || defined(_M_AVX2)
		for (; item_index + spatial_avx2_batch_size <= item_count; item_index += spatial_avx2_batch_size)
		{
			const u32 visible_mask = test_sphere_batch_avx2(
				&N->items[item_index], mask, *F, fmask);
			for (u32 lane = 0; lane < spatial_avx2_batch_size; ++lane)
				if (visible_mask & (1u << lane))
					result->push_back(N->items[item_index + lane]);
		}
#endif

		for (; item_index < item_count; ++item_index)
		{
			ISpatial* S = N->items[item_index];
			if (0 == (S->spatial.type & mask)) continue;

			const Fvector& sC = S->spatial.sphere.P;
			const float sR = S->spatial.sphere.R;
			u32 tmask = fmask;
			if (fcvNone == F->testSphere(sC, sR, tmask)) continue;

			result->push_back(S);
		}

		// recurse
		float c_R = n_R / 2;
		for (u32 octant = 0; octant < 8; octant++)
		{
			if (0 == N->children[octant]) continue;
			Fvector c_C;
			c_C.mad(n_C, c_spatial_offset[octant], c_R);
			walk(N->children[octant], c_C, c_R, fmask);
		}
	}
};

template <typename result_type>
static void q_frustum_unlocked_impl(ISpatial_DB& space, result_type& result, u32 mask,
	const CFrustum& frustum)
{
	result.clear_not_free();
	if (!space.m_root || !mask)
		return;

	walker<result_type> query(result, mask, &frustum);
	query.walk(space.m_root, space.m_center, space.m_bounds, frustum.getMask());
}

void ISpatial_DB::q_frustum(xr_vector<ISpatial*>& R, u32 _o, u32 _mask, const CFrustum& _frustum)
{
	xrSRWLockGuard guard(cs, true);
	q_frustum_unlocked_impl(*this, R, _mask, _frustum);
}

void ISpatial_DB::q_frustum(xr_frame_vector<ISpatial*>& R, u32 _o, u32 _mask, const CFrustum& _frustum)
{
	xrSRWLockGuard guard(cs, true);
	q_frustum_unlocked_impl(*this, R, _mask, _frustum);
}

namespace
{
	class multi_frustum_walker
	{
	public:
		xr_vector<ISpatialFrustumMask>* result;
		u32 type_mask;
		const CFrustum* frustums;
		u32 frustum_count;

		multi_frustum_walker(xr_vector<ISpatialFrustumMask>& out, u32 mask, const CFrustum* views, u32 count)
			: result(&out), type_mask(mask), frustums(views), frustum_count(count)
		{
		}

		void walk(ISpatial_NODE* node, Fvector& node_center, float node_radius, u32 active_views)
		{
			if (!node || !active_views)
				return;

			const float node_extent = 2.f * node_radius;
			Fbox node_box;
			node_box.set(node_center.x - node_extent, node_center.y - node_extent, node_center.z - node_extent,
			             node_center.x + node_extent, node_center.y + node_extent, node_center.z + node_extent);

			// Cull the octree node against every still-active view. A single tree walk
			// is shared by all cascades; only the compact view mask is propagated.
			u32 node_views = 0;
			for (u32 view_index = 0; view_index < frustum_count; ++view_index)
			{
				const u32 bit = 1u << view_index;
				if (!(active_views & bit))
					continue;

				u32 plane_mask = frustums[view_index].getMask();
				if (frustums[view_index].testAABB(node_box.data(), plane_mask) != fcvNone)
					node_views |= bit;
			}

			if (!node_views)
				return;

			for (u32 item_index = 0; item_index < node->items.size(); ++item_index)
			{
				ISpatial* spatial = node->items[item_index];
				if (!spatial || !(spatial->spatial.type & type_mask))
					continue;

				u32 item_views = 0;
				const Fvector& center = spatial->spatial.sphere.P;
				const float radius = spatial->spatial.sphere.R;
				for (u32 view_index = 0; view_index < frustum_count; ++view_index)
				{
					const u32 bit = 1u << view_index;
					if (!(node_views & bit))
						continue;

					u32 plane_mask = frustums[view_index].getMask();
					if (frustums[view_index].testSphere(center, radius, plane_mask) != fcvNone)
						item_views |= bit;
				}

				if (item_views)
				{
					ISpatialFrustumMask item = {spatial, item_views};
					result->push_back(item);
				}
			}

			const float child_radius = node_radius * 0.5f;
			for (u32 octant = 0; octant < 8; ++octant)
			{
				ISpatial_NODE* child = node->children[octant];
				if (!child)
					continue;

				Fvector child_center;
				child_center.mad(node_center, c_spatial_offset[octant], child_radius);
				walk(child, child_center, child_radius, node_views);
			}
		}
	};
}

void ISpatial_DB::q_frustums_mask(xr_vector<ISpatialFrustumMask>& R, u32 _o, u32 _mask,
	const CFrustum* _frustums, u32 _frustum_count)
{
	(void)_o;
	R.clear_not_free();
	if (!m_root || !_mask || !_frustums || !_frustum_count)
		return;

	const u32 frustum_count = _min(_frustum_count, 32u);
	const u32 active_views = frustum_count == 32u ? u32(-1) : ((1u << frustum_count) - 1u);

	xrSRWLockGuard guard(cs, true);
	multi_frustum_walker query(R, _mask, _frustums, frustum_count);
	query.walk(m_root, m_center, m_bounds, active_views);
}
