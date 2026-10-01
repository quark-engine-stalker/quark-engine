#pragma once

#include <immintrin.h>
#include <cstddef>

namespace r4_avx2
{
struct points8
{
    __m256 x;
    __m256 y;
    __m256 z;
};

IC float horizontal_min8_scalar(__m256 value)
{
    __m128 reduced = _mm_min_ps(_mm256_castps256_ps128(value), _mm256_extractf128_ps(value, 1));
    reduced = _mm_min_ps(reduced, _mm_movehl_ps(reduced, reduced));
    reduced = _mm_min_ss(reduced, _mm_shuffle_ps(reduced, reduced, _MM_SHUFFLE(1, 1, 1, 1)));
    return _mm_cvtss_f32(reduced);
}

IC float horizontal_max8_scalar(__m256 value)
{
    __m128 reduced = _mm_max_ps(_mm256_castps256_ps128(value), _mm256_extractf128_ps(value, 1));
    reduced = _mm_max_ps(reduced, _mm_movehl_ps(reduced, reduced));
    reduced = _mm_max_ss(reduced, _mm_shuffle_ps(reduced, reduced, _MM_SHUFFLE(1, 1, 1, 1)));
    return _mm_cvtss_f32(reduced);
}

IC points8 load_points8(const Fvector3* source)
{
    static_assert(sizeof(Fvector3) == sizeof(float) * 3, "Fvector3 must be tightly packed");

    const __m256i offsets = _mm256_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21);
    const float* base = &source[0].x;

    points8 result;
    result.x = _mm256_i32gather_ps(base, offsets, sizeof(float));
    result.y = _mm256_i32gather_ps(base + 1, offsets, sizeof(float));
    result.z = _mm256_i32gather_ps(base + 2, offsets, sizeof(float));
    return result;
}

template <typename Box>
IC points8 make_box_corners8(const Box& box)
{
    points8 result;
    result.x = _mm256_setr_ps(box.min.x, box.max.x, box.min.x, box.max.x,
                              box.min.x, box.max.x, box.min.x, box.max.x);
    result.y = _mm256_setr_ps(box.min.y, box.min.y, box.max.y, box.max.y,
                              box.min.y, box.min.y, box.max.y, box.max.y);
    result.z = _mm256_setr_ps(box.min.z, box.min.z, box.min.z, box.min.z,
                              box.max.z, box.max.z, box.max.z, box.max.z);
    return result;
}

IC __m256 transform_component(const points8& source, float m1, float m2, float m3, float m4)
{
    __m256 result = _mm256_set1_ps(m4);
    result = _mm256_fmadd_ps(source.x, _mm256_set1_ps(m1), result);
    result = _mm256_fmadd_ps(source.y, _mm256_set1_ps(m2), result);
    result = _mm256_fmadd_ps(source.z, _mm256_set1_ps(m3), result);
    return result;
}

IC points8 transform_points8_perspective(const Fmatrix& matrix, const points8& source)
{
    points8 result;
    result.x = transform_component(source, matrix._11, matrix._21, matrix._31, matrix._41);
    result.y = transform_component(source, matrix._12, matrix._22, matrix._32, matrix._42);
    result.z = transform_component(source, matrix._13, matrix._23, matrix._33, matrix._43);
    const __m256 w = transform_component(source, matrix._14, matrix._24, matrix._34, matrix._44);
    const __m256 inv_w = _mm256_div_ps(_mm256_set1_ps(1.0f), w);
    result.x = _mm256_mul_ps(result.x, inv_w);
    result.y = _mm256_mul_ps(result.y, inv_w);
    result.z = _mm256_mul_ps(result.z, inv_w);
    return result;
}

IC Fvector3 transform_point_perspective(const Fmatrix& matrix, const Fvector3& source)
{
    __m128 result = _mm_loadu_ps(&matrix._41);
    result = _mm_fmadd_ps(_mm_set1_ps(source.z), _mm_loadu_ps(&matrix._31), result);
    result = _mm_fmadd_ps(_mm_set1_ps(source.y), _mm_loadu_ps(&matrix._21), result);
    result = _mm_fmadd_ps(_mm_set1_ps(source.x), _mm_loadu_ps(&matrix._11), result);

    const __m128 wwww = _mm_shuffle_ps(result, result, _MM_SHUFFLE(3, 3, 3, 3));
    result = _mm_div_ps(result, wwww);

    alignas(16) float values[4];
    _mm_store_ps(values, result);

    Fvector3 output;
    output.set(values[0], values[1], values[2]);
    return output;
}

IC void store_points8(const points8& source, Fvector3* destination)
{
    alignas(32) float x[8];
    alignas(32) float y[8];
    alignas(32) float z[8];
    _mm256_store_ps(x, source.x);
    _mm256_store_ps(y, source.y);
    _mm256_store_ps(z, source.z);

    for (u32 i = 0; i < 8; ++i)
        destination[i].set(x[i], y[i], z[i]);
}

IC void store_points8(const points8& source, Fvector4* destination, float w = 1.0f)
{
    static_assert(sizeof(Fvector4) == sizeof(float) * 4, "Fvector4 must be tightly packed");

    __m128 x0 = _mm256_castps256_ps128(source.x);
    __m128 y0 = _mm256_castps256_ps128(source.y);
    __m128 z0 = _mm256_castps256_ps128(source.z);
    __m128 w0 = _mm_set1_ps(w);
    _MM_TRANSPOSE4_PS(x0, y0, z0, w0);
    _mm_storeu_ps(&destination[0].x, x0);
    _mm_storeu_ps(&destination[1].x, y0);
    _mm_storeu_ps(&destination[2].x, z0);
    _mm_storeu_ps(&destination[3].x, w0);

    __m128 x1 = _mm256_extractf128_ps(source.x, 1);
    __m128 y1 = _mm256_extractf128_ps(source.y, 1);
    __m128 z1 = _mm256_extractf128_ps(source.z, 1);
    __m128 w1 = _mm_set1_ps(w);
    _MM_TRANSPOSE4_PS(x1, y1, z1, w1);
    _mm_storeu_ps(&destination[4].x, x1);
    _mm_storeu_ps(&destination[5].x, y1);
    _mm_storeu_ps(&destination[6].x, z1);
    _mm_storeu_ps(&destination[7].x, w1);
}

IC void transform_points8(const Fmatrix& matrix, const Fvector3* source, Fvector3* destination)
{
    const points8 transformed = transform_points8_perspective(matrix, load_points8(source));
    store_points8(transformed, destination);
    _mm256_zeroupper();
}

IC void transform_points8(const Fmatrix& matrix, const Fvector3* source, Fvector4* destination, float w = 1.0f)
{
    const points8 transformed = transform_points8_perspective(matrix, load_points8(source));
    store_points8(transformed, destination, w);
    _mm256_zeroupper();
}

template <typename Box>
IC void merge_transformed_bounds(const points8& transformed, Box& bounds)
{
    bounds.min.x = _min(bounds.min.x, horizontal_min8_scalar(transformed.x));
    bounds.min.y = _min(bounds.min.y, horizontal_min8_scalar(transformed.y));
    bounds.min.z = _min(bounds.min.z, horizontal_min8_scalar(transformed.z));
    bounds.max.x = _max(bounds.max.x, horizontal_max8_scalar(transformed.x));
    bounds.max.y = _max(bounds.max.y, horizontal_max8_scalar(transformed.y));
    bounds.max.z = _max(bounds.max.z, horizontal_max8_scalar(transformed.z));
}

template <typename Box>
IC void transform_points8_bounds(const Fmatrix& matrix, const Fvector3* source, Box& bounds)
{
    merge_transformed_bounds(transform_points8_perspective(matrix, load_points8(source)), bounds);
    _mm256_zeroupper();
}

template <typename Box>
IC void transform_points_bounds(const Fmatrix& matrix, const Fvector3* source, size_t count, Box& bounds)
{
    size_t index = 0;
    for (; index + 8 <= count; index += 8)
        merge_transformed_bounds(transform_points8_perspective(matrix, load_points8(source + index)), bounds);

    _mm256_zeroupper();

    for (; index < count; ++index)
        bounds.modify(transform_point_perspective(matrix, source[index]));
}

template <typename SourceBox, typename BoundsBox>
IC void transform_aabb_bounds(const Fmatrix& matrix, const SourceBox& source, BoundsBox& bounds)
{
    merge_transformed_bounds(transform_points8_perspective(matrix, make_box_corners8(source)), bounds);
    _mm256_zeroupper();
}

template <typename Box>
IC void transform_aabb_z_bounds(const Fmatrix& matrix, const Box& source, float& min_z, float& max_z)
{
    const points8 transformed = transform_points8_perspective(matrix, make_box_corners8(source));
    min_z = _min(min_z, horizontal_min8_scalar(transformed.z));
    max_z = _max(max_z, horizontal_max8_scalar(transformed.z));
    _mm256_zeroupper();
}

template <typename SourceBox, typename BoundsBox>
IC void transform_aabbs_bounds(const Fmatrix& matrix, const SourceBox* source, size_t count, BoundsBox& bounds)
{
    for (size_t index = 0; index < count; ++index)
        merge_transformed_bounds(transform_points8_perspective(matrix, make_box_corners8(source[index])), bounds);

    _mm256_zeroupper();
}

template <typename Box>
IC void transform_aabbs_z_bounds(const Fmatrix& matrix, const Box* source, size_t count, float& min_z, float& max_z)
{
    for (size_t index = 0; index < count; ++index)
    {
        const points8 transformed = transform_points8_perspective(matrix, make_box_corners8(source[index]));
        min_z = _min(min_z, horizontal_min8_scalar(transformed.z));
        max_z = _max(max_z, horizontal_max8_scalar(transformed.z));
    }

    _mm256_zeroupper();
}

}
