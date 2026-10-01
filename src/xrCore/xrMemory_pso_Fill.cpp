#include "stdafx.h"
#pragma hdrstop

#include <cstdint>
#include <cstring>
#include <immintrin.h>

namespace
{
constexpr u32 kAvx2FillMinimum = 8 * 1024;
constexpr u32 kAvx2FillMaximum = 128 * 1024;
constexpr u32 kFillBlockSize = 256;

__forceinline void fill_block_avx2(u8* destination, const __m256i value)
{
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 0), value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 32), value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 64), value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 96), value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 128), value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 160), value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 192), value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(destination + 224), value);
}
} // namespace

void __stdcall xrMemFill_x86(void* dest, int value, u32 count)
{
    memset(dest, value, count);
}

void __stdcall xrMemFill_AVX2(void* dest, int value, u32 count)
{
    if (count < kAvx2FillMinimum || count > kAvx2FillMaximum)
    {
        memset(dest, value, count);
        return;
    }

    u8* destination = static_cast<u8*>(dest);
    u32 remaining = count;

    const u32 alignmentBytes =
        (32u - (static_cast<u32>(reinterpret_cast<std::uintptr_t>(destination)) & 31u)) & 31u;
    if (alignmentBytes != 0)
    {
        memset(destination, value, alignmentBytes);
        destination += alignmentBytes;
        remaining -= alignmentBytes;
    }

    const __m256i fillValue = _mm256_set1_epi8(static_cast<char>(value));

    while (remaining >= kFillBlockSize)
    {
        fill_block_avx2(destination, fillValue);
        destination += kFillBlockSize;
        remaining -= kFillBlockSize;
    }

    while (remaining >= 32)
    {
        _mm256_store_si256(reinterpret_cast<__m256i*>(destination), fillValue);
        destination += 32;
        remaining -= 32;
    }

    _mm256_zeroupper();

    if (remaining != 0)
        memset(destination, value, remaining);
}
