#pragma once

#include <immintrin.h>
#include <cstddef>
#include <cstdint>

// SIMD helpers local to xrPhysics. The project is built with /arch:AVX2.
// Unaligned loads/stores are intentional: ODE and X-Ray math objects are not
// guaranteed to start on a 32-byte boundary.
namespace xr_physics_simd
{
    __forceinline void scale_12(float* values, const float scale) noexcept
    {
        const __m256 factor8 = _mm256_set1_ps(scale);
        const __m128 factor4 = _mm_set1_ps(scale);

        _mm256_storeu_ps(values, _mm256_mul_ps(_mm256_loadu_ps(values), factor8));
        _mm_storeu_ps(values + 8, _mm_mul_ps(_mm_loadu_ps(values + 8), factor4));
    }

    __forceinline void subtract_12(float* destination, const float* source) noexcept
    {
        _mm256_storeu_ps(destination,
            _mm256_sub_ps(_mm256_loadu_ps(destination), _mm256_loadu_ps(source)));
        _mm_storeu_ps(destination + 8,
            _mm_sub_ps(_mm_loadu_ps(destination + 8), _mm_loadu_ps(source + 8)));
    }

    // ODE dMatrix3 is three float4 rows. Fmatrix stores four float4 rows.
    // A 128-bit transpose is optimal for a 3x3 matrix; under /arch:AVX2 the
    // compiler emits VEX-encoded instructions without an SSE/AVX transition.
    __forceinline void ode_rotation_to_fmatrix(const float* ode_rotation, float* matrix) noexcept
    {
        __m128 row0 = _mm_loadu_ps(ode_rotation + 0);
        __m128 row1 = _mm_loadu_ps(ode_rotation + 4);
        __m128 row2 = _mm_loadu_ps(ode_rotation + 8);
        __m128 row3 = _mm_setzero_ps();

        _MM_TRANSPOSE4_PS(row0, row1, row2, row3);

        _mm_storeu_ps(matrix + 0, row0);
        _mm_storeu_ps(matrix + 4, row1);
        _mm_storeu_ps(matrix + 8, row2);
    }

    __forceinline void ode_transform_to_fmatrix(
        const float* ode_rotation, const float* position, float* matrix) noexcept
    {
        ode_rotation_to_fmatrix(ode_rotation, matrix);
        _mm_storeu_ps(matrix + 12, _mm_set_ps(1.0f, position[2], position[1], position[0]));
    }

    __forceinline void fmatrix_rotation_to_ode(const float* matrix, float* ode_rotation) noexcept
    {
        __m128 row0 = _mm_loadu_ps(matrix + 0);
        __m128 row1 = _mm_loadu_ps(matrix + 4);
        __m128 row2 = _mm_loadu_ps(matrix + 8);
        __m128 row3 = _mm_setzero_ps();

        _MM_TRANSPOSE4_PS(row0, row1, row2, row3);

        _mm_storeu_ps(ode_rotation + 0, row0);
        _mm_storeu_ps(ode_rotation + 4, row1);
        _mm_storeu_ps(ode_rotation + 8, row2);
    }

    __forceinline __m256 abs_ps(const __m256 value) noexcept
    {
        return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), value);
    }

    __forceinline __m256 outside_aabb_axis(
        const __m256 a, const __m256 b, const __m256 c,
        const __m256 negative_extent, const __m256 positive_extent) noexcept
    {
        const __m256 all_below = _mm256_and_ps(
            _mm256_and_ps(_mm256_cmp_ps(a, negative_extent, _CMP_LT_OQ),
                          _mm256_cmp_ps(b, negative_extent, _CMP_LT_OQ)),
            _mm256_cmp_ps(c, negative_extent, _CMP_LT_OQ));
        const __m256 all_above = _mm256_and_ps(
            _mm256_and_ps(_mm256_cmp_ps(a, positive_extent, _CMP_GT_OQ),
                          _mm256_cmp_ps(b, positive_extent, _CMP_GT_OQ)),
            _mm256_cmp_ps(c, positive_extent, _CMP_GT_OQ));
        return _mm256_or_ps(all_below, all_above);
    }

    // Coarse triangle-vs-AABB test for eight indexed triangles. It performs
    // the same axis and triangle-plane rejection as aabb_tri_aabb(), returning
    // one passing bit per triangle. Full SAT remains scalar for the few lanes
    // that survive this inexpensive batch filter.
    template <typename Triangle, typename Vector>
    __forceinline std::uint32_t triangle_aabb_plane_mask_8(
        const Triangle* triangle_array,
        const int* triangle_ids,
        const Vector* vertex_array,
        const float* center,
        const float* extents) noexcept
    {
        static_assert(sizeof(Vector) >= sizeof(float) * 3, "Vector must contain xyz floats");
        static_assert(sizeof(Vector) % sizeof(float) == 0, "Vector stride must be float-addressable");

        constexpr int vertex_stride = static_cast<int>(sizeof(Vector) / sizeof(float));

        const Triangle& t0 = triangle_array[triangle_ids[0]];
        const Triangle& t1 = triangle_array[triangle_ids[1]];
        const Triangle& t2 = triangle_array[triangle_ids[2]];
        const Triangle& t3 = triangle_array[triangle_ids[3]];
        const Triangle& t4 = triangle_array[triangle_ids[4]];
        const Triangle& t5 = triangle_array[triangle_ids[5]];
        const Triangle& t6 = triangle_array[triangle_ids[6]];
        const Triangle& t7 = triangle_array[triangle_ids[7]];

#define XR_PHYSICS_VERTEX_INDICES(corner) \
        _mm256_setr_epi32( \
            static_cast<int>(t0.verts[corner]) * vertex_stride, \
            static_cast<int>(t1.verts[corner]) * vertex_stride, \
            static_cast<int>(t2.verts[corner]) * vertex_stride, \
            static_cast<int>(t3.verts[corner]) * vertex_stride, \
            static_cast<int>(t4.verts[corner]) * vertex_stride, \
            static_cast<int>(t5.verts[corner]) * vertex_stride, \
            static_cast<int>(t6.verts[corner]) * vertex_stride, \
            static_cast<int>(t7.verts[corner]) * vertex_stride)

        const __m256i vertex0_indices = XR_PHYSICS_VERTEX_INDICES(0);
        const __m256i vertex1_indices = XR_PHYSICS_VERTEX_INDICES(1);
        const __m256i vertex2_indices = XR_PHYSICS_VERTEX_INDICES(2);
#undef XR_PHYSICS_VERTEX_INDICES

        const float* vertices = reinterpret_cast<const float*>(vertex_array);
        const __m256i component_y = _mm256_set1_epi32(1);
        const __m256i component_z = _mm256_set1_epi32(2);

        const __m256 v0x = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, vertex0_indices, sizeof(float)), _mm256_set1_ps(center[0]));
        const __m256 v0y = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, _mm256_add_epi32(vertex0_indices, component_y), sizeof(float)),
            _mm256_set1_ps(center[1]));
        const __m256 v0z = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, _mm256_add_epi32(vertex0_indices, component_z), sizeof(float)),
            _mm256_set1_ps(center[2]));

        const __m256 v1x = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, vertex1_indices, sizeof(float)), _mm256_set1_ps(center[0]));
        const __m256 v1y = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, _mm256_add_epi32(vertex1_indices, component_y), sizeof(float)),
            _mm256_set1_ps(center[1]));
        const __m256 v1z = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, _mm256_add_epi32(vertex1_indices, component_z), sizeof(float)),
            _mm256_set1_ps(center[2]));

        const __m256 v2x = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, vertex2_indices, sizeof(float)), _mm256_set1_ps(center[0]));
        const __m256 v2y = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, _mm256_add_epi32(vertex2_indices, component_y), sizeof(float)),
            _mm256_set1_ps(center[1]));
        const __m256 v2z = _mm256_sub_ps(
            _mm256_i32gather_ps(vertices, _mm256_add_epi32(vertex2_indices, component_z), sizeof(float)),
            _mm256_set1_ps(center[2]));

        const __m256 extent_x = _mm256_set1_ps(extents[0]);
        const __m256 extent_y = _mm256_set1_ps(extents[1]);
        const __m256 extent_z = _mm256_set1_ps(extents[2]);
        const __m256 neg_extent_x = _mm256_sub_ps(_mm256_setzero_ps(), extent_x);
        const __m256 neg_extent_y = _mm256_sub_ps(_mm256_setzero_ps(), extent_y);
        const __m256 neg_extent_z = _mm256_sub_ps(_mm256_setzero_ps(), extent_z);

        __m256 rejected = outside_aabb_axis(v0x, v1x, v2x, neg_extent_x, extent_x);
        rejected = _mm256_or_ps(rejected, outside_aabb_axis(v0y, v1y, v2y, neg_extent_y, extent_y));
        rejected = _mm256_or_ps(rejected, outside_aabb_axis(v0z, v1z, v2z, neg_extent_z, extent_z));

        const __m256 e0x = _mm256_sub_ps(v1x, v0x);
        const __m256 e0y = _mm256_sub_ps(v1y, v0y);
        const __m256 e0z = _mm256_sub_ps(v1z, v0z);
        const __m256 e1x = _mm256_sub_ps(v2x, v1x);
        const __m256 e1y = _mm256_sub_ps(v2y, v1y);
        const __m256 e1z = _mm256_sub_ps(v2z, v1z);

        const __m256 normal_x = _mm256_sub_ps(_mm256_mul_ps(e0y, e1z), _mm256_mul_ps(e0z, e1y));
        const __m256 normal_y = _mm256_sub_ps(_mm256_mul_ps(e0z, e1x), _mm256_mul_ps(e0x, e1z));
        const __m256 normal_z = _mm256_sub_ps(_mm256_mul_ps(e0x, e1y), _mm256_mul_ps(e0y, e1x));

        const __m256 plane_dot = _mm256_add_ps(
            _mm256_add_ps(_mm256_mul_ps(normal_x, v0x), _mm256_mul_ps(normal_y, v0y)),
            _mm256_mul_ps(normal_z, v0z));
        const __m256 plane_distance = _mm256_sub_ps(_mm256_setzero_ps(), plane_dot);

        const __m256 projection_radius = _mm256_add_ps(
            _mm256_add_ps(abs_ps(_mm256_mul_ps(normal_x, extent_x)),
                          abs_ps(_mm256_mul_ps(normal_y, extent_y))),
            abs_ps(_mm256_mul_ps(normal_z, extent_z)));
        const __m256 negative_radius = _mm256_sub_ps(_mm256_setzero_ps(), projection_radius);
        const __m256 plane_overlap = _mm256_and_ps(
            _mm256_cmp_ps(plane_distance, negative_radius, _CMP_GT_OQ),
            _mm256_cmp_ps(plane_distance, projection_radius, _CMP_LT_OQ));

        const __m256 axis_pass = _mm256_xor_ps(rejected, _mm256_castsi256_ps(_mm256_set1_epi32(-1)));
        const __m256 pass = _mm256_and_ps(axis_pass, plane_overlap);
        return static_cast<std::uint32_t>(_mm256_movemask_ps(pass));
    }
}
