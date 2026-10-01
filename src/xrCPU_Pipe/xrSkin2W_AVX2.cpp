#include "stdafx.h"
#pragma hdrstop

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

namespace
{
static_assert(sizeof(Fvector) == 12, "Unexpected Fvector layout");
static_assert(sizeof(Fmatrix) == 64, "Unexpected Fmatrix layout");
static_assert(sizeof(vertRender) == 32, "AVX2 output store requires a 32-byte vertRender");
static_assert(sizeof(vertBoned1W) == 60, "Unexpected vertBoned1W layout");
static_assert(sizeof(vertBoned2W) == 64, "Unexpected vertBoned2W layout");
static_assert(sizeof(vertBoned3W) == 70, "Unexpected vertBoned3W layout");
static_assert(sizeof(vertBoned4W) == 76, "Unexpected vertBoned4W layout");

constexpr u32 kSourcePrefetchDistance = 4;

template <typename TVertex>
__forceinline __m256 load_position_normal_pair(const TVertex& vertex)
{
    // P and N are adjacent in every skinned vertex format.
    // Load four floats at once, then clear the fourth component that belongs to
    // the next field. This removes six scalar loads/inserts from every vertex.
    const __m128 zero = _mm_setzero_ps();
    const __m128 homogeneousPosition = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    const __m128 p = _mm_blend_ps(_mm_loadu_ps(&vertex.P.x), homogeneousPosition, 0x8);
    const __m128 n = _mm_blend_ps(_mm_loadu_ps(&vertex.N.x), zero, 0x8);
    return _mm256_set_m128(n, p);
}

// Transforms position in the low 128-bit lane and normal in the high lane.
// Position carries homogeneous w=1 while normal carries w=0, so one shared
// row-3 FMA applies translation only to the position lane.
__forceinline __m256 transform_position_normal(const __m256 positionNormal, const Fmatrix& matrix)
{
    const __m256 x = _mm256_permute_ps(positionNormal, _MM_SHUFFLE(0, 0, 0, 0));
    const __m256 y = _mm256_permute_ps(positionNormal, _MM_SHUFFLE(1, 1, 1, 1));
    const __m256 z = _mm256_permute_ps(positionNormal, _MM_SHUFFLE(2, 2, 2, 2));
    const __m256 w = _mm256_permute_ps(positionNormal, _MM_SHUFFLE(3, 3, 3, 3));

    const __m256 row0 = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(&matrix._11));
    const __m256 row1 = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(&matrix._21));
    const __m256 row2 = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(&matrix._31));
    const __m256 row3 = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(&matrix._41));

    __m256 result = _mm256_mul_ps(x, row0);
    result = _mm256_fmadd_ps(y, row1, result);
    result = _mm256_fmadd_ps(z, row2, result);
    return _mm256_fmadd_ps(w, row3, result);
}

template <typename TVertex>
__forceinline __m128 load_uv(const TVertex& vertex)
{
    // 3W/4W formats are packed to two-byte alignment, so u can be unaligned.
    // Copy through the object representation instead of binding an aligned float reference.
    std::uint64_t packed = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&vertex);
    std::memcpy(&packed, bytes + offsetof(TVertex, u), sizeof(packed));
    return _mm_castsi128_ps(_mm_cvtsi64_si128(static_cast<__int64>(packed)));
}

__forceinline __m256 pack_render_vertex(const __m256 positionNormal, const __m128 uv)
{
    const __m128 position = _mm256_castps256_ps128(positionNormal);
    const __m128 normal = _mm256_extractf128_ps(positionNormal, 1);

    // low 128: Px, Py, Pz, Nx
    const __m128 normalX = _mm_shuffle_ps(normal, normal, _MM_SHUFFLE(0, 0, 0, 0));
    const __m128 low = _mm_blend_ps(position, normalX, 0x8);

    // high 128: Ny, Nz, u, v
    const __m128 high = _mm_shuffle_ps(normal, uv, _MM_SHUFFLE(1, 0, 2, 1));

    return _mm256_set_m128(high, low);
}

__forceinline void store_render_vertex(vertRender* destination, const __m256 value, const bool streaming)
{
    if (streaming)
        _mm256_stream_ps(reinterpret_cast<float*>(destination), value);
    else
        _mm256_storeu_ps(reinterpret_cast<float*>(destination), value);
}

__forceinline bool can_stream(const vertRender* destination)
{
    return (reinterpret_cast<std::uintptr_t>(destination) & 31u) == 0;
}

template <typename TVertex>
__forceinline void prefetch_source(const TVertex* source, const u32 index, const u32 count)
{
    if (index + kSourcePrefetchDistance < count)
        _mm_prefetch(reinterpret_cast<const char*>(source + index + kSourcePrefetchDistance), _MM_HINT_T0);
}

__forceinline void finish_avx2_loop(const bool streaming)
{
    if (streaming)
        _mm_sfence();

    // The engine still contains SSE-heavy code. Avoid the AVX-to-SSE transition penalty.
    _mm256_zeroupper();
}
} // namespace

void __stdcall xrSkin1W_AVX2(vertRender* D, vertBoned1W* S, u32 vCount, CBoneInstance* Bones)
{
    vertRender* __restrict destination = D;
    const vertBoned1W* __restrict source = S;
    const CBoneInstance* __restrict bones = Bones;
    const bool streaming = vCount != 0 && can_stream(destination);

    for (u32 i = 0; i < vCount; ++i)
    {
        prefetch_source(source, i, vCount);

        const vertBoned1W& vertex = source[i];
        const __m256 positionNormal = load_position_normal_pair(vertex);
        const __m256 transformed = transform_position_normal(positionNormal, bones[vertex.matrix].mRenderTransform);
        const __m256 output = pack_render_vertex(transformed, load_uv(vertex));
        store_render_vertex(destination + i, output, streaming);
    }

    finish_avx2_loop(streaming);
}

void __stdcall xrSkin2W_AVX2(vertRender* D, vertBoned2W* S, u32 vCount, CBoneInstance* Bones)
{
    vertRender* __restrict destination = D;
    const vertBoned2W* __restrict source = S;
    const CBoneInstance* __restrict bones = Bones;
    const bool streaming = vCount != 0 && can_stream(destination);

    for (u32 i = 0; i < vCount; ++i)
    {
        prefetch_source(source, i, vCount);

        const vertBoned2W& vertex = source[i];
        const __m256 positionNormal = load_position_normal_pair(vertex);
        __m256 transformed = transform_position_normal(positionNormal, bones[vertex.matrix0].mRenderTransform);

        if (vertex.matrix1 != vertex.matrix0)
        {
            const __m256 transformed1 = transform_position_normal(positionNormal, bones[vertex.matrix1].mRenderTransform);
            const __m256 weight = _mm256_set1_ps(vertex.w);
            transformed = _mm256_fmadd_ps(_mm256_sub_ps(transformed1, transformed), weight, transformed);
        }

        const __m256 output = pack_render_vertex(transformed, load_uv(vertex));
        store_render_vertex(destination + i, output, streaming);
    }

    finish_avx2_loop(streaming);
}

void __stdcall xrSkin3W_AVX2(vertRender* D, vertBoned3W* S, u32 vCount, CBoneInstance* Bones)
{
    vertRender* __restrict destination = D;
    const vertBoned3W* __restrict source = S;
    const CBoneInstance* __restrict bones = Bones;
    const bool streaming = vCount != 0 && can_stream(destination);

    for (u32 i = 0; i < vCount; ++i)
    {
        prefetch_source(source, i, vCount);

        const vertBoned3W& vertex = source[i];
        const __m256 positionNormal = load_position_normal_pair(vertex);
        const float w0 = vertex.w[0];
        const float w1 = vertex.w[1];

        const __m256 transformed0 = transform_position_normal(positionNormal, bones[vertex.m[0]].mRenderTransform);
        const __m256 transformed1 = transform_position_normal(positionNormal, bones[vertex.m[1]].mRenderTransform);
        const __m256 transformed2 = transform_position_normal(positionNormal, bones[vertex.m[2]].mRenderTransform);

        // w2 = 1 - w0 - w1. Use transform2 as the affine base:
        // T2 + w0 * (T0 - T2) + w1 * (T1 - T2).
        // This removes one weight broadcast and one vector multiply.
        __m256 transformed = transformed2;
        transformed = _mm256_fmadd_ps(_mm256_sub_ps(transformed0, transformed2), _mm256_set1_ps(w0), transformed);
        transformed = _mm256_fmadd_ps(_mm256_sub_ps(transformed1, transformed2), _mm256_set1_ps(w1), transformed);

        const __m256 output = pack_render_vertex(transformed, load_uv(vertex));
        store_render_vertex(destination + i, output, streaming);
    }

    finish_avx2_loop(streaming);
}

void __stdcall xrSkin4W_AVX2(vertRender* D, vertBoned4W* S, u32 vCount, CBoneInstance* Bones)
{
    vertRender* __restrict destination = D;
    const vertBoned4W* __restrict source = S;
    const CBoneInstance* __restrict bones = Bones;
    const bool streaming = vCount != 0 && can_stream(destination);

    for (u32 i = 0; i < vCount; ++i)
    {
        prefetch_source(source, i, vCount);

        const vertBoned4W& vertex = source[i];
        const __m256 positionNormal = load_position_normal_pair(vertex);
        const float w0 = vertex.w[0];
        const float w1 = vertex.w[1];
        const float w2 = vertex.w[2];

        const __m256 transformed0 = transform_position_normal(positionNormal, bones[vertex.m[0]].mRenderTransform);
        const __m256 transformed1 = transform_position_normal(positionNormal, bones[vertex.m[1]].mRenderTransform);
        const __m256 transformed2 = transform_position_normal(positionNormal, bones[vertex.m[2]].mRenderTransform);
        const __m256 transformed3 = transform_position_normal(positionNormal, bones[vertex.m[3]].mRenderTransform);

        // w3 = 1 - w0 - w1 - w2. Use transform3 as the affine base:
        // T3 + sum(wi * (Ti - T3), i=0..2).
        // This removes the derived fourth weight, one broadcast and one multiply.
        __m256 transformed = transformed3;
        transformed = _mm256_fmadd_ps(_mm256_sub_ps(transformed0, transformed3), _mm256_set1_ps(w0), transformed);
        transformed = _mm256_fmadd_ps(_mm256_sub_ps(transformed1, transformed3), _mm256_set1_ps(w1), transformed);
        transformed = _mm256_fmadd_ps(_mm256_sub_ps(transformed2, transformed3), _mm256_set1_ps(w2), transformed);

        const __m256 output = pack_render_vertex(transformed, load_uv(vertex));
        store_render_vertex(destination + i, output, streaming);
    }

    finish_avx2_loop(streaming);
}
