// Texture.cpp: implementation of the CTexture class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "../xrRender/dxRenderDeviceRender.h"
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/x_ray.h"
#include "../../xrCore/adaptive_memory_policy.h"
#include "bc7_encoder.h"

#include <atomic>
#include <cmath>
#include <climits>
#include <cstring>
#include <immintrin.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// #include "std_classes.h"
// #include "xr_avi.h"

namespace
{
class runtime_texture_warmup_observer
{
public:
	explicit runtime_texture_warmup_observer(LPCSTR texture_name)
		: name(texture_name), started(CPU::qpc_freq ? CPU::QPC() : 0)
	{}

	~runtime_texture_warmup_observer()
	{
		if (!started || !CPU::qpc_freq || !g_appLoaded || !g_pGamePersistent ||
			Device.dwPrecacheFrame || !psTextureWarmupManifest)
			return;

		const u64 elapsed_ticks = CPU::QPC() - started;
		const u32 elapsed_ms = static_cast<u32>(_min<u64>(
			(elapsed_ticks * 1000ull + CPU::qpc_freq - 1ull) / CPU::qpc_freq, u32(-1)));
		g_pGamePersistent->RecordSlowTexture(name, elapsed_ms);
	}

private:
	LPCSTR name;
	u64 started;
};
}

namespace quark_dds
{
constexpr u32 magic = 0x20534444u; // "DDS "
constexpr u32 ddpf_alpha = 0x2u;
constexpr u32 ddpf_fourcc = 0x4u;
constexpr u32 ddpf_rgb = 0x40u;
constexpr u32 ddpf_luminance = 0x20000u;
constexpr u32 ddpf_bumpdudv = 0x80000u;
constexpr u32 caps2_cubemap = 0x200u;
constexpr u32 caps2_cubemap_allfaces = 0xFC00u;
constexpr u32 caps2_volume = 0x200000u;
constexpr u32 misc_texturecube = 0x4u;
constexpr u32 dim_texture1d = 2u;
constexpr u32 dim_texture2d = 3u;
constexpr u32 dim_texture3d = 4u;
constexpr u32 max_subresources = 30720u;

struct pixel_format
{
    u32 size;
    u32 flags;
    u32 fourcc;
    u32 rgb_bit_count;
    u32 r_mask;
    u32 g_mask;
    u32 b_mask;
    u32 a_mask;
};

struct header
{
    u32 size;
    u32 flags;
    u32 height;
    u32 width;
    u32 pitch_or_linear_size;
    u32 depth;
    u32 mip_map_count;
    u32 reserved1[11];
    pixel_format format;
    u32 caps;
    u32 caps2;
    u32 caps3;
    u32 caps4;
    u32 reserved2;
};

struct header_dx10
{
    DXGI_FORMAT format;
    u32 resource_dimension;
    u32 misc_flag;
    u32 array_size;
    u32 misc_flags2;
};

static_assert(sizeof(pixel_format) == 32, "DDS pixel format layout mismatch");
static_assert(sizeof(header) == 124, "DDS header layout mismatch");
static_assert(sizeof(header_dx10) == 20, "DDS DX10 header layout mismatch");

inline u32 fourcc(char a, char b, char c, char d)
{
    return static_cast<u32>(static_cast<u8>(a)) |
        (static_cast<u32>(static_cast<u8>(b)) << 8) |
        (static_cast<u32>(static_cast<u8>(c)) << 16) |
        (static_cast<u32>(static_cast<u8>(d)) << 24);
}

inline bool is_mask(const pixel_format& pf, u32 r, u32 g, u32 b, u32 a)
{
    return pf.r_mask == r && pf.g_mask == g && pf.b_mask == b && pf.a_mask == a;
}

DXGI_FORMAT legacy_format(const pixel_format& pf)
{
    if (pf.flags & ddpf_rgb)
    {
        if (pf.rgb_bit_count == 32)
        {
            if (is_mask(pf, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            if (is_mask(pf, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000))
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            if (is_mask(pf, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000))
                return DXGI_FORMAT_B8G8R8X8_UNORM;
            if (is_mask(pf, 0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000))
                return DXGI_FORMAT_R10G10B10A2_UNORM;
            if (is_mask(pf, 0x0000ffff, 0xffff0000, 0x00000000, 0x00000000))
                return DXGI_FORMAT_R16G16_UNORM;
            if (is_mask(pf, 0xffffffff, 0x00000000, 0x00000000, 0x00000000))
                return DXGI_FORMAT_R32_FLOAT;
        }
        else if (pf.rgb_bit_count == 16)
        {
            if (is_mask(pf, 0x0000f800, 0x000007e0, 0x0000001f, 0x00000000))
                return DXGI_FORMAT_B5G6R5_UNORM;
            if (is_mask(pf, 0x00007c00, 0x000003e0, 0x0000001f, 0x00008000))
                return DXGI_FORMAT_B5G5R5A1_UNORM;
            if (is_mask(pf, 0x00000f00, 0x000000f0, 0x0000000f, 0x0000f000))
                return DXGI_FORMAT_B4G4R4A4_UNORM;
        }
    }
    else if (pf.flags & ddpf_luminance)
    {
        if (pf.rgb_bit_count == 8 && is_mask(pf, 0x000000ff, 0, 0, 0))
            return DXGI_FORMAT_R8_UNORM;
        if (pf.rgb_bit_count == 16 && is_mask(pf, 0x0000ffff, 0, 0, 0))
            return DXGI_FORMAT_R16_UNORM;
        if (pf.rgb_bit_count == 16 && is_mask(pf, 0x000000ff, 0, 0, 0x0000ff00))
            return DXGI_FORMAT_R8G8_UNORM;
    }
    else if ((pf.flags & ddpf_alpha) && pf.rgb_bit_count == 8)
    {
        return DXGI_FORMAT_A8_UNORM;
    }
    else if (pf.flags & ddpf_bumpdudv)
    {
        if (pf.rgb_bit_count == 16 && is_mask(pf, 0x00ff, 0xff00, 0, 0))
            return DXGI_FORMAT_R8G8_SNORM;
        if (pf.rgb_bit_count == 32 && is_mask(pf, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))
            return DXGI_FORMAT_R8G8B8A8_SNORM;
    }

    if (!(pf.flags & ddpf_fourcc))
        return DXGI_FORMAT_UNKNOWN;

    switch (pf.fourcc)
    {
    case 0x31545844u: return DXGI_FORMAT_BC1_UNORM; // DXT1
    case 0x32545844u: // DXT2 (premultiplied BC2 payload)
    case 0x33545844u: return DXGI_FORMAT_BC2_UNORM; // DXT3
    case 0x34545844u: // DXT4 (premultiplied BC3 payload)
    case 0x35545844u: return DXGI_FORMAT_BC3_UNORM; // DXT5
    case 0x31495441u: // ATI1
    case 0x55344342u: return DXGI_FORMAT_BC4_UNORM; // BC4U
    case 0x53344342u: return DXGI_FORMAT_BC4_SNORM; // BC4S
    case 0x32495441u: // ATI2
    case 0x55354342u: return DXGI_FORMAT_BC5_UNORM; // BC5U
    case 0x53354342u: return DXGI_FORMAT_BC5_SNORM; // BC5S
    case 0x47424752u: return DXGI_FORMAT_R8G8_B8G8_UNORM; // RGBG
    case 0x42475247u: return DXGI_FORMAT_G8R8_G8B8_UNORM; // GRGB
    case 36: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 110: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 111: return DXGI_FORMAT_R16_FLOAT;
    case 112: return DXGI_FORMAT_R16G16_FLOAT;
    case 113: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 114: return DXGI_FORMAT_R32_FLOAT;
    case 115: return DXGI_FORMAT_R32G32_FLOAT;
    case 116: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

u32 bits_per_pixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: return 128;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: return 96;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT: return 64;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return 32;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM: return 16;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM: return 8;
    default: return 0;
    }
}

bool surface_info(u32 width, u32 height, DXGI_FORMAT format, size_t& row_bytes, size_t& num_bytes)
{
    size_t num_rows = 0;
    size_t bytes_per_block = 0;
    switch (format)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        bytes_per_block = 8;
        break;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        bytes_per_block = 16;
        break;
    default:
        break;
    }

    if (bytes_per_block)
    {
        const size_t blocks_wide = width ? _max<size_t>(1, (width + 3u) / 4u) : 0;
        const size_t blocks_high = height ? _max<size_t>(1, (height + 3u) / 4u) : 0;
        row_bytes = blocks_wide * bytes_per_block;
        num_rows = blocks_high;
    }
    else if (format == DXGI_FORMAT_R8G8_B8G8_UNORM || format == DXGI_FORMAT_G8R8_G8B8_UNORM)
    {
        row_bytes = ((static_cast<size_t>(width) + 1u) >> 1u) * 4u;
        num_rows = height;
    }
    else
    {
        const u32 bpp = bits_per_pixel(format);
        if (!bpp)
            return false;
        row_bytes = (static_cast<size_t>(width) * bpp + 7u) / 8u;
        num_rows = height;
    }

    if (row_bytes && num_rows > SIZE_MAX / row_bytes)
        return false;
    num_bytes = row_bytes * num_rows;
    return true;
}

// RGB24 has no DXGI resource format. Expand directly to the byte-identical
// RGBA8 image used by the legacy loader, without its intermediate float image.
// Both SIMD loads stay within the current eight pixels (24 source bytes).
void expand_rgb24(const u8* source, u8* destination, size_t pixels)
{
    const __m128i first = _mm_setr_epi8(2, 1, 0, -128, 5, 4, 3, -128,
        8, 7, 6, -128, 11, 10, 9, -128);
    const __m128i second = _mm_setr_epi8(6, 5, 4, -128, 9, 8, 7, -128,
        12, 11, 10, -128, 15, 14, 13, -128);
    const __m128i alpha = _mm_set1_epi32(static_cast<int>(0xff000000u));
    while (pixels >= 8)
    {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + 8));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination),
            _mm_or_si128(_mm_shuffle_epi8(a, first), alpha));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 16),
            _mm_or_si128(_mm_shuffle_epi8(b, second), alpha));
        source += 24;
        destination += 32;
        pixels -= 8;
    }
    while (pixels--)
    {
        destination[0] = source[2];
        destination[1] = source[1];
        destination[2] = source[0];
        destination[3] = 255;
        source += 3;
        destination += 4;
    }
}

struct rgb24_pixels_deleter
{
    void operator()(u8* pixels) const { xr_free(pixels); }
};

constexpr size_t bc_block_small = 8u;
constexpr size_t bc_block_big = 16u;

// True when every texel of the BC3 alpha block is exactly 255. Interpolated entries do not count:
// their rounding is up to the GPU.
inline bool bc3_alpha_block_opaque(const u8* block)
{
    u64 indices = 0;
    std::memcpy(&indices, block + 2, 6);
    if (block[0] == 255 && block[1] == 255)
    {
        // Every palette entry is 255 except index 6, the fixed 0 of the a0 <= a1 mode
        for (u32 i = 0; i < 16; ++i, indices >>= 3)
            if ((indices & 7u) == 6u)
                return false;
        return true;
    }
    if (block[0] == 255)
        return indices == 0; // a0 > a1: only index 0 is a0
    if (block[1] == 255)
    {
        // a0 < a1: index 1 is a1, index 7 is the fixed 255
        for (u32 i = 0; i < 16; ++i, indices >>= 3)
            if ((indices & 7u) != 1u && (indices & 7u) != 7u)
                return false;
        return true;
    }
    return false;
}

inline bool bc3_opaque(const u8* blocks, size_t bytes)
{
    for (size_t i = 0; i < bytes; i += bc_block_big)
        if (!bc3_alpha_block_opaque(blocks + i))
            return false;
    return true;
}

// The colour half of a BC3 block always decodes in the four colour mode. BC1 picks its mode by the endpoint order
// (c0 <= c1 is the mode with transparent black), so such blocks get their endpoints swapped and indices remapped.
void bc3_to_bc1(const u8* source, u8* destination, size_t bytes)
{
    for (size_t i = 0; i < bytes; i += bc_block_big, destination += bc_block_small)
    {
        u16 c0, c1;
        u32 indices;
        std::memcpy(&c0, source + i + 8, 2);
        std::memcpy(&c1, source + i + 10, 2);
        std::memcpy(&indices, source + i + 12, 4);
        if (c0 < c1)
        {
            std::swap(c0, c1);
            indices ^= 0x55555555u; // 0<->1, 2<->3
        }
        else if (c0 == c1)
            indices = 0; // all four entries are this colour; index 3 of the BC1 mode would be transparent
        std::memcpy(destination, &c0, 2);
        std::memcpy(destination + 2, &c1, 2);
        std::memcpy(destination + 4, &indices, 4);
    }
}

bool create_from_memory(const void* data, size_t size, int requested_lod, bool staging,
    ID3DBaseTexture** out_texture, u32& out_mip_count, int& out_loaded_lod, bool& out_converted)
{
    out_converted = false;
    if (!data || !out_texture || size < sizeof(u32) + sizeof(header))
        return false;

    *out_texture = nullptr;
    const u8* bytes = static_cast<const u8*>(data);
    u32 file_magic = 0;
    header hdr_storage = {};
    std::memcpy(&file_magic, bytes, sizeof(file_magic));
    std::memcpy(&hdr_storage, bytes + sizeof(u32), sizeof(hdr_storage));
    const header* hdr = &hdr_storage;
    if (file_magic != magic || hdr->size != sizeof(header) ||
        hdr->format.size != sizeof(pixel_format) || !hdr->width || !hdr->height)
        return false;

    size_t offset = sizeof(u32) + sizeof(header);
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 dimension = dim_texture2d;
    u32 array_size = 1;
    bool cube = false;
    bool rgb24 = false;
    u32 depth = hdr->depth ? hdr->depth : 1;

    if ((hdr->format.flags & ddpf_fourcc) && hdr->format.fourcc == fourcc('D', 'X', '1', '0'))
    {
        if (size < offset + sizeof(header_dx10))
            return false;
        header_dx10 ext_storage = {};
        std::memcpy(&ext_storage, bytes + offset, sizeof(ext_storage));
        const header_dx10* ext = &ext_storage;
        offset += sizeof(header_dx10);
        format = ext->format;
        dimension = ext->resource_dimension;
        array_size = ext->array_size;
        cube = (ext->misc_flag & misc_texturecube) != 0;
        if (!array_size)
            return false;
        if (dimension == dim_texture3d && array_size != 1)
            return false;
        if (cube)
        {
            if (dimension != dim_texture2d || array_size > u32(-1) / 6u)
                return false;
            array_size *= 6u;
        }
    }
    else
    {
        format = legacy_format(hdr->format);
        // Unknown/malformed masks, SRGB metadata, palettes and non-packed
        // rows keep the legacy compatibility path. Never reinterpret them.
        if (format == DXGI_FORMAT_UNKNOWN && requested_lod == 0 &&
            !(hdr->caps2 & (caps2_volume | caps2_cubemap)) &&
            hdr->format.flags == ddpf_rgb &&
            hdr->format.rgb_bit_count == 24 &&
            is_mask(hdr->format, 0x00ff0000, 0x0000ff00, 0x000000ff, 0) &&
            (!(hdr->flags & 0x8u) ||
                static_cast<u64>(hdr->pitch_or_linear_size) == static_cast<u64>(hdr->width) * 3u))
        {
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rgb24 = true;
        }
        if (hdr->caps2 & caps2_volume)
            dimension = dim_texture3d;
        if (hdr->caps2 & caps2_cubemap)
        {
            if ((hdr->caps2 & caps2_cubemap_allfaces) != caps2_cubemap_allfaces)
                return false;
            cube = true;
            array_size = 6;
        }
    }

    if (format == DXGI_FORMAT_UNKNOWN || offset > size)
        return false;

    u32 mip_count = hdr->mip_map_count ? hdr->mip_map_count : 1;
    int lod = cube ? 0 : _max(0, requested_lod);
    if (lod >= static_cast<int>(mip_count))
        lod = static_cast<int>(mip_count) - 1;
    const u32 load_mips = mip_count - static_cast<u32>(lod);
    if (!load_mips || array_size > max_subresources / load_mips)
        return false;

    if (dimension != dim_texture1d && dimension != dim_texture2d && dimension != dim_texture3d)
        return false;
    if (mip_count > D3D11_REQ_MIP_LEVELS)
        return false;
    if (dimension == dim_texture1d)
    {
        if (hdr->height != 1 || hdr->width > D3D11_REQ_TEXTURE1D_U_DIMENSION ||
            array_size > D3D11_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION)
            return false;
    }
    else if (dimension == dim_texture2d)
    {
        if (hdr->width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            hdr->height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            array_size > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
            return false;
    }
    else if (hdr->width > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
        hdr->height > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
        depth > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION)
    {
        return false;
    }

    xr_vector<D3D11_SUBRESOURCE_DATA> init_data;
    init_data.reserve(dimension == dim_texture3d ? load_mips : array_size * load_mips);
    // Own conversion storage until CreateTexture has consumed every initial
    // subresource. No decoded image or extra cache survives this call.
    std::unique_ptr<u8, rgb24_pixels_deleter> converted_mips[D3D11_REQ_MIP_LEVELS];
    const u8* src = bytes + offset;
    const u8* end = bytes + size;

    const u32 source_slices = dimension == dim_texture3d ? 1u : array_size;
    for (u32 slice = 0; slice < source_slices; ++slice)
    {
        u32 w = hdr->width;
        u32 h = dimension == dim_texture1d ? 1u : hdr->height;
        u32 d = dimension == dim_texture3d ? depth : 1u;
        for (u32 mip = 0; mip < mip_count; ++mip)
        {
            size_t row_bytes = 0;
            size_t slice_bytes = 0;
            if (rgb24)
            {
                row_bytes = static_cast<size_t>(w) * 3u;
                slice_bytes = row_bytes * h;
            }
            else if (!surface_info(w, h, format, row_bytes, slice_bytes))
                return false;
            if (d && slice_bytes > SIZE_MAX / d)
                return false;
            const size_t mip_bytes = slice_bytes * d;
            if (mip_bytes > static_cast<size_t>(end - src))
                return false;

            if (mip >= static_cast<u32>(lod))
            {
                if (row_bytes > UINT_MAX || slice_bytes > UINT_MAX)
                    return false;
                D3D11_SUBRESOURCE_DATA sub = {};
                if (rgb24)
                {
                    const size_t output_row = static_cast<size_t>(w) * 4u;
                    const size_t output_slice = output_row * h;
                    if (output_row > UINT_MAX || output_slice > UINT_MAX ||
                        (d && output_slice > SIZE_MAX / d))
                        return false;
                    // Every output byte is written by expand_rgb24. Do not
                    // zero a large temporary image just to overwrite it.
                    converted_mips[mip].reset(static_cast<u8*>(
                        xr_malloc_uninitialized(output_slice * d)));
                    u8* pixels = converted_mips[mip].get();
                    expand_rgb24(src, pixels, static_cast<size_t>(w) * h * d);
                    sub.pSysMem = pixels;
                    sub.SysMemPitch = static_cast<UINT>(output_row);
                    sub.SysMemSlicePitch = static_cast<UINT>(output_slice);
                }
                else
                {
                    sub.pSysMem = src;
                    sub.SysMemPitch = static_cast<UINT>(row_bytes);
                    sub.SysMemSlicePitch = static_cast<UINT>(slice_bytes);
                }
                init_data.push_back(sub);
            }

            src += mip_bytes;
            w = _max(1u, w >> 1u);
            h = _max(1u, h >> 1u);
            d = _max(1u, d >> 1u);
        }
    }

    // BC3 with all alpha 255 (mods save opaque textures as DXT5 all the time) samples the same from BC1 at half the memory
    std::unique_ptr<u8, rgb24_pixels_deleter> bc1_pixels;
    if (format == DXGI_FORMAT_BC3_UNORM && dimension == dim_texture2d)
    {
        size_t total = 0;
        bool opaque = true;
        for (const D3D11_SUBRESOURCE_DATA& sub : init_data)
        {
            total += sub.SysMemSlicePitch;
            if (!bc3_opaque(static_cast<const u8*>(sub.pSysMem), sub.SysMemSlicePitch))
            {
                opaque = false;
                break;
            }
        }
        if (opaque)
        {
            bc1_pixels.reset(static_cast<u8*>(xr_malloc_uninitialized(total / 2u)));
            u8* out = bc1_pixels.get();
            for (D3D11_SUBRESOURCE_DATA& sub : init_data)
            {
                bc3_to_bc1(static_cast<const u8*>(sub.pSysMem), out, sub.SysMemSlicePitch);
                sub.pSysMem = out;
                sub.SysMemPitch /= 2u;
                sub.SysMemSlicePitch /= 2u;
                out += sub.SysMemSlicePitch;
            }
            format = DXGI_FORMAT_BC1_UNORM;
            out_converted = true;
        }
    }

    HRESULT hr = E_FAIL;
    const D3D11_USAGE usage = staging ? D3D_USAGE_STAGING : D3D_USAGE_IMMUTABLE;
    const UINT bind_flags = staging ? 0u : D3D_BIND_SHADER_RESOURCE;
    const UINT cpu_flags = staging ? D3D11_CPU_ACCESS_WRITE : 0u;

    const u32 width = _max(1u, hdr->width >> lod);
    const u32 height = _max(1u, hdr->height >> lod);
    const u32 load_depth = _max(1u, depth >> lod);

    if (dimension == dim_texture1d)
    {
        D3D11_TEXTURE1D_DESC desc = {};
        desc.Width = width;
        desc.MipLevels = load_mips;
        desc.ArraySize = array_size;
        desc.Format = format;
        desc.Usage = staging ? D3D_USAGE_DEFAULT : usage; // ProcessStaging has no 1D path.
        desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        ID3D11Texture1D* texture = nullptr;
        hr = HW.pDevice->CreateTexture1D(&desc, init_data.data(), &texture);
        *out_texture = texture;
    }
    else if (dimension == dim_texture2d)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = load_mips;
        desc.ArraySize = array_size;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bind_flags;
        desc.CPUAccessFlags = cpu_flags;
        desc.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ID3D11Texture2D* texture = nullptr;
        hr = HW.pDevice->CreateTexture2D(&desc, init_data.data(), &texture);
        *out_texture = texture;
    }
    else
    {
        D3D11_TEXTURE3D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.Depth = load_depth;
        desc.MipLevels = load_mips;
        desc.Format = format;
        desc.Usage = usage;
        desc.BindFlags = bind_flags;
        desc.CPUAccessFlags = cpu_flags;
        ID3D11Texture3D* texture = nullptr;
        hr = HW.pDevice->CreateTexture3D(&desc, init_data.data(), &texture);
        *out_texture = texture;
    }

    if (FAILED(hr) || !*out_texture)
    {
        _RELEASE(*out_texture);
        return false;
    }

    out_mip_count = mip_count;
    out_loaded_lod = lod;
    return true;
}
} // namespace quark_dds

// Mods ship the same image under many names: textures with an identical file share one immutable resource.
// The owner count is ours because D3D counts views and pipeline bindings too.
namespace
{
struct shared_surface_key
{
    u64 hash;
    u64 size;
    int lod;
    bool operator==(const shared_surface_key& other) const { return hash == other.hash && size == other.size && lod == other.lod; }
};

struct shared_surface_key_hash
{
    size_t operator()(const shared_surface_key& key) const { return static_cast<size_t>(key.hash ^ (key.size << 8) ^ static_cast<u64>(key.lod)); }
};

struct shared_surface
{
    ID3DBaseTexture* surface;
    u32 owners;
};

struct shared_surface_registry
{
    xrCriticalSection lock;
    std::unordered_map<shared_surface_key, shared_surface, shared_surface_key_hash> surfaces;
    std::unordered_map<ID3DBaseTexture*, shared_surface_key> keys;
    std::unordered_map<ID3DBaseTexture*, shared_surface_key> staged; // staging copies waiting for their first use
};

// Never destroyed: CTextures may outlive other statics at exit
shared_surface_registry& shared_surfaces()
{
    static shared_surface_registry* registry = new shared_surface_registry();
    return *registry;
}

// XXH64
u64 content_hash(const u8* p, size_t size)
{
    constexpr u64 P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
        P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
    const auto read64 = [](const u8* at) { u64 v; std::memcpy(&v, at, 8); return v; };
    const auto round = [&](u64 acc, u64 input) { return _rotl64(acc + input * P2, 31) * P1; };
    const auto merge = [&](u64 acc, u64 val) { return (acc ^ round(0, val)) * P1 + P4; };
    const u8* const end = p + size;
    u64 h = P5;
    if (size >= 32)
    {
        u64 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
        for (const u8* const limit = end - 32; p <= limit; p += 32)
        {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
        }
        h = _rotl64(v1, 1) + _rotl64(v2, 7) + _rotl64(v3, 12) + _rotl64(v4, 18);
        h = merge(merge(merge(merge(h, v1), v2), v3), v4);
    }
    h += size;
    for (; p + 8 <= end; p += 8)
        h = _rotl64(h ^ round(0, read64(p)), 27) * P1 + P4;
    if (p + 4 <= end)
    {
        u32 v;
        std::memcpy(&v, p, 4);
        h = _rotl64(h ^ (v * P1), 23) * P2 + P3;
        p += 4;
    }
    for (; p < end; ++p)
        h = _rotl64(h ^ (*p * P5), 11) * P1;
    h = (h ^ (h >> 33)) * P2;
    h = (h ^ (h >> 29)) * P3;
    return h ^ (h >> 32);
}

ID3DBaseTexture* acquire_shared_surface(const shared_surface_key& key)
{
    shared_surface_registry& registry = shared_surfaces();
    xrCriticalSectionGuard guard(registry.lock);
    const auto found = registry.surfaces.find(key);
    if (found == registry.surfaces.end())
        return nullptr;
    ++found->second.owners;
    found->second.surface->AddRef();
    return found->second.surface;
}

// Returns the surface the caller owns: another worker may have created the same image meanwhile
ID3DBaseTexture* publish_shared_surface(const shared_surface_key& key, ID3DBaseTexture* created)
{
    shared_surface_registry& registry = shared_surfaces();
    xrCriticalSectionGuard guard(registry.lock);
    const auto inserted = registry.surfaces.emplace(key, shared_surface{ created, 1u });
    if (inserted.second)
    {
        created->AddRef();
        registry.keys.emplace(created, key);
        return created;
    }
    ++inserted.first->second.owners;
    inserted.first->second.surface->AddRef();
    created->Release();
    return inserted.first->second.surface;
}

// Uncompressed pictures (UI atlases, skies, some weapon textures) take four times the memory of BC7. A low priority
// thread encodes each one once, into cache\textures_bc7 keyed by the content of the source file; the next load takes
// the small version. Nothing here touches a frame: the running texture stays as it is until it is loaded again.
struct rgba8_layout
{
    u32 width, height, mips, slices;
    size_t offset;
    bool cube, bgr, no_alpha;
};

bool rgba8_file_layout(const u8* bytes, size_t size, rgba8_layout& out)
{
    using namespace quark_dds;
    if (size < sizeof(u32) + sizeof(header))
        return false;
    u32 file_magic;
    header hdr;
    std::memcpy(&file_magic, bytes, sizeof(file_magic));
    std::memcpy(&hdr, bytes + sizeof(u32), sizeof(hdr));
    if (file_magic != magic || hdr.size != sizeof(header) || hdr.format.size != sizeof(pixel_format) || !hdr.width || !hdr.height)
        return false;
    out.offset = sizeof(u32) + sizeof(header);
    out.slices = 1;
    out.cube = false;
    DXGI_FORMAT format;
    if ((hdr.format.flags & ddpf_fourcc) && hdr.format.fourcc == fourcc('D', 'X', '1', '0'))
    {
        if (size < out.offset + sizeof(header_dx10))
            return false;
        header_dx10 ext;
        std::memcpy(&ext, bytes + out.offset, sizeof(ext));
        out.offset += sizeof(ext);
        format = ext.format;
        out.cube = (ext.misc_flag & misc_texturecube) != 0;
        out.slices = ext.array_size * (out.cube ? 6u : 1u);
        if (ext.resource_dimension != dim_texture2d || !out.slices || out.slices > 6u)
            return false;
    }
    else
    {
        format = legacy_format(hdr.format);
        if (hdr.caps2 & caps2_volume)
            return false;
        if (hdr.caps2 & caps2_cubemap)
        {
            if ((hdr.caps2 & caps2_cubemap_allfaces) != caps2_cubemap_allfaces)
                return false;
            out.cube = true;
            out.slices = 6;
        }
    }
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8X8_UNORM)
        return false;
    // A BC texture must have a top level of whole blocks
    if ((hdr.width & 3) || (hdr.height & 3))
        return false;
    out.width = hdr.width;
    out.height = hdr.height;
    out.mips = hdr.mip_map_count ? hdr.mip_map_count : 1;
    if (out.mips > D3D11_REQ_MIP_LEVELS)
        return false;
    out.bgr = format != DXGI_FORMAT_R8G8B8A8_UNORM;
    out.no_alpha = format == DXGI_FORMAT_B8G8R8X8_UNORM;
    size_t total = 0;
    u32 w = out.width, h = out.height;
    for (u32 mip = 0; mip < out.mips; ++mip)
    {
        total += size_t(w) * h * 4u;
        w = _max(1u, w >> 1u);
        h = _max(1u, h >> 1u);
    }
    return out.offset + total * out.slices <= size;
}

// Shaders read these as data, not as pictures: they stay exact
bool picture_name(LPCSTR lower)
{
    for (LPCSTR folder : { "shaders\\", "fx\\", "pfx\\", "water\\", "lights\\", "terrain\\", "ed\\" })
        if (lower == strstr(lower, folder))
            return false;
    for (LPCSTR part : { "font", "lut", "noise", "ramp" })
        if (strstr(lower, part))
            return false;
    return true;
}

// Below the size limit the gain is nothing
bool transcode_candidate(LPCSTR lower, const void* data, size_t size)
{
    constexpr size_t min_bytes = 2u * 1024u * 1024u;
    if (size < min_bytes || !picture_name(lower))
        return false;
    rgba8_layout layout;
    return rgba8_file_layout(static_cast<const u8*>(data), size, layout);
}

struct transcode_job
{
    shared_surface_key key;
    u8* data;
    size_t size;
    float min_db; // above zero: look for a useless top mip instead of encoding
};

struct transcode_state
{
    xrCriticalSection lock;
    xr_vector<transcode_job> queue;
    std::unordered_set<u64> seen;
    u64 queued_bytes = 0;
    HANDLE thread = nullptr;
    bool running = false;
    std::atomic<bool> stop{ false };
    string_path folder = {};
    u32 encoded = 0, rejected = 0, trim_checked = 0, trim_found = 0;
    u64 bytes_in = 0, bytes_out = 0;
};

// Never destroyed, as the shared surfaces
transcode_state& transcoder()
{
    static transcode_state* state = new transcode_state();
    return *state;
}

LPCSTR transcode_folder()
{
    transcode_state& t = transcoder();
    xrCriticalSectionGuard guard(t.lock);
    if (!t.folder[0])
    {
        FS.update_path(t.folder, "$app_data_root$", "cache\\textures_bc7\\");
        VerifyPath(t.folder);
    }
    return t.folder;
}

void transcode_path(const shared_surface_key& key, LPCSTR extension, string_path& out)
{
    xr_sprintf(out, "%s%016llx_%llx%s", transcode_folder(), key.hash, key.size, extension);
}

// The DDS of the BC7 version, followed by the hash of the DDS. Anything else counts as absent.
u8* load_transcoded(const shared_surface_key& key, size_t& out_size, bool& rejected)
{
    string_path path;
    transcode_path(key, ".dds", path);
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") || !file)
    {
        transcode_path(key, ".no", path);
        if (!fopen_s(&file, path, "rb") && file)
        {
            fclose(file);
            rejected = true;
        }
        return nullptr;
    }
    u8* data = nullptr;
    _fseeki64(file, 0, SEEK_END);
    const __int64 total = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (total > __int64(sizeof(u64)))
    {
        data = static_cast<u8*>(xr_malloc_uninitialized(size_t(total)));
        u64 stored = 0;
        if (fread(data, 1, size_t(total), file) == size_t(total))
            std::memcpy(&stored, data + total - sizeof(u64), sizeof(u64));
        if (!stored || stored != content_hash(data, size_t(total) - sizeof(u64)))
        {
            Msg("! [texture-bc7] broken cache file dropped: %016llx", key.hash);
            xr_free(data);
            data = nullptr;
        }
        else
            out_size = size_t(total) - sizeof(u64);
    }
    fclose(file);
    if (!data)
        DeleteFileA(path);
    return data;
}

void write_transcoded(const shared_surface_key& key, const u8* data, size_t size)
{
    string_path path, temp;
    transcode_path(key, ".tmp", temp);
    FILE* file = nullptr;
    if (fopen_s(&file, temp, "wb") || !file)
        return;
    const bool written = fwrite(data, 1, size, file) == size;
    fclose(file);
    transcode_path(key, size ? ".dds" : ".no", path);
    if (!written || !MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(temp);
}

// A top mip that holds no detail: upscaled art and flat correction maps. When the next mip, as the GPU stretches it,
// matches the top one within the limit, the top one is not loaded at all (a quarter of the memory per level).
// The answer is found once on the encoder thread and kept in miptrim.bin, so a load only looks it up.
constexpr u32 mip_trim_max_levels = 3;

struct mip_trim_record
{
    u64 hash;
    u64 size;
    u64 levels;
};

struct mip_trim_table
{
    xrCriticalSection lock;
    std::unordered_map<u64, u8> levels;
    bool loaded = false;
};

mip_trim_table& mip_trims()
{
    static mip_trim_table* table = new mip_trim_table();
    return *table;
}

void mip_trim_path(string_path& out)
{
    xr_sprintf(out, "%smiptrim.bin", transcode_folder());
}

inline u64 mip_trim_key(const shared_surface_key& key) { return key.hash ^ (key.size * 0x9E3779B97F4A7C15ull); }

// False: not analysed yet
bool mip_trim_lookup(const shared_surface_key& key, u32& levels)
{
    mip_trim_table& table = mip_trims();
    string_path path;
    mip_trim_path(path);
    xrCriticalSectionGuard guard(table.lock);
    if (!table.loaded)
    {
        table.loaded = true;
        FILE* file = nullptr;
        if (!fopen_s(&file, path, "rb") && file)
        {
            mip_trim_record record;
            while (fread(&record, sizeof(record), 1, file) == 1)
                if (record.levels <= mip_trim_max_levels)
                    table.levels[record.hash ^ (record.size * 0x9E3779B97F4A7C15ull)] = u8(record.levels);
            fclose(file);
        }
    }
    const auto found = table.levels.find(mip_trim_key(key));
    if (found == table.levels.end())
        return false;
    levels = found->second;
    return true;
}

void mip_trim_store(const shared_surface_key& key, u32 levels)
{
    mip_trim_table& table = mip_trims();
    string_path path;
    mip_trim_path(path);
    xrCriticalSectionGuard guard(table.lock);
    table.levels[mip_trim_key(key)] = u8(levels);
    FILE* file = nullptr;
    if (!fopen_s(&file, path, "ab") && file)
    {
        const mip_trim_record record = { key.hash, key.size, levels };
        fwrite(&record, sizeof(record), 1, file);
        fclose(file);
    }
}

// Whole BC1 or BC3 levels of a plain 2D DDS with a mip chain; otherwise there is nothing to trim
struct bc_chain
{
    bool bc3;
    u32 width, height, mips;
    const u8* pixels;
    size_t block;
};

bool bc_file_chain(const u8* bytes, size_t size, bc_chain& out)
{
    using namespace quark_dds;
    if (size < sizeof(u32) + sizeof(header))
        return false;
    u32 file_magic;
    header hdr;
    std::memcpy(&file_magic, bytes, sizeof(file_magic));
    std::memcpy(&hdr, bytes + sizeof(u32), sizeof(hdr));
    if (file_magic != magic || hdr.size != sizeof(header) || hdr.format.size != sizeof(pixel_format) || !(hdr.format.flags & ddpf_fourcc))
        return false;
    if (hdr.format.fourcc != fourcc('D', 'X', 'T', '1') && hdr.format.fourcc != fourcc('D', 'X', 'T', '5'))
        return false;
    if (hdr.caps2 & (caps2_volume | caps2_cubemap))
        return false;
    out.bc3 = hdr.format.fourcc == fourcc('D', 'X', 'T', '5');
    out.block = out.bc3 ? 16u : 8u;
    out.width = hdr.width;
    out.height = hdr.height;
    out.mips = hdr.mip_map_count;
    out.pixels = bytes + sizeof(u32) + sizeof(header);
    if (out.mips < 2 || out.mips > D3D11_REQ_MIP_LEVELS || !out.width || !out.height)
        return false;
    size_t total = 0;
    u32 w = out.width, h = out.height;
    for (u32 mip = 0; mip < out.mips; ++mip)
    {
        total += size_t((w + 3u) / 4u) * ((h + 3u) / 4u) * out.block;
        w = _max(1u, w >> 1u);
        h = _max(1u, h >> 1u);
    }
    return sizeof(u32) + sizeof(header) + total <= size;
}

// A level keeps being a valid top of a BC texture only with whole blocks
inline bool mip_trim_fits(u32 width, u32 height, u32 mips, u32 levels)
{
    const u32 w = width >> levels, h = height >> levels;
    return levels < mips && w >= 4u && h >= 4u && !(w & 3u) && !(h & 3u);
}

void decode_bc_level(const u8* data, u32 w, u32 h, bool bc3, xr_vector<u8>& out)
{
    out.resize(size_t(w) * h * 4u);
    const u32 blocks_w = (w + 3u) / 4u, blocks_h = (h + 3u) / 4u;
    const size_t block = bc3 ? 16u : 8u;
    for (u32 by = 0; by < blocks_h; ++by)
        for (u32 bx = 0; bx < blocks_w; ++bx)
        {
            const u8* b = data + (size_t(by) * blocks_w + bx) * block;
            const u8* color = bc3 ? b + 8 : b;
            u16 c0, c1;
            u32 indices;
            std::memcpy(&c0, color, 2);
            std::memcpy(&c1, color + 2, 2);
            std::memcpy(&indices, color + 4, 4);
            int palette[4][4];
            const auto unpack = [](u16 c, int* to)
            {
                to[0] = ((c >> 11) & 31) * 255 / 31;
                to[1] = ((c >> 5) & 63) * 255 / 63;
                to[2] = (c & 31) * 255 / 31;
                to[3] = 255;
            };
            unpack(c0, palette[0]);
            unpack(c1, palette[1]);
            palette[2][3] = palette[3][3] = 255;
            if (bc3 || c0 > c1)
                for (u32 k = 0; k < 3; ++k)
                {
                    palette[2][k] = (2 * palette[0][k] + palette[1][k]) / 3;
                    palette[3][k] = (palette[0][k] + 2 * palette[1][k]) / 3;
                }
            else
            {
                for (u32 k = 0; k < 3; ++k)
                {
                    palette[2][k] = (palette[0][k] + palette[1][k]) / 2;
                    palette[3][k] = 0;
                }
                palette[3][3] = 0;
            }
            int alpha[8] = {};
            u64 alpha_bits = 0;
            if (bc3)
            {
                alpha[0] = b[0];
                alpha[1] = b[1];
                if (alpha[0] > alpha[1])
                    for (int i = 0; i < 6; ++i)
                        alpha[i + 2] = ((6 - i) * alpha[0] + (i + 1) * alpha[1]) / 7;
                else
                {
                    for (int i = 0; i < 4; ++i)
                        alpha[i + 2] = ((4 - i) * alpha[0] + (i + 1) * alpha[1]) / 5;
                    alpha[7] = 255;
                }
                std::memcpy(&alpha_bits, b + 2, 6);
            }
            for (u32 i = 0; i < 16; ++i, indices >>= 2, alpha_bits >>= 3)
            {
                const u32 x = bx * 4u + (i & 3u), y = by * 4u + (i >> 2u);
                if (x >= w || y >= h)
                    continue;
                u8* to = &out[(size_t(y) * w + x) * 4u];
                const int* entry = palette[indices & 3u];
                to[0] = u8(entry[0]);
                to[1] = u8(entry[1]);
                to[2] = u8(entry[2]);
                to[3] = bc3 ? u8(alpha[alpha_bits & 7u]) : u8(entry[3]);
            }
        }
}

// Does the level, stretched by bilinear filtering, stay within min_db of the top level, colour and alpha apart?
bool stretched_level_matches(const xr_vector<u8>& top, u32 w, u32 h, const xr_vector<u8>& level, u32 lw, u32 lh, float min_db, const std::atomic<bool>& stop)
{
    u64 error[4] = { 0, 0, 0, 0 };
    const double texels = double(w) * h;
    const double limit = 255.0 * 255.0 / pow(10.0, double(min_db) / 10.0);
    for (u32 y = 0; y < h; ++y)
    {
        if (stop.load(std::memory_order_relaxed) || double(error[0] + error[1] + error[2]) > limit * 3.0 * texels || double(error[3]) > limit * texels)
            return false;
        const float fy = (float(y) + 0.5f) * float(lh) / float(h) - 0.5f;
        const int y0 = fy < 0 ? 0 : int(fy), y1 = y0 + 1 < int(lh) ? y0 + 1 : y0;
        const int wy = fy < 0 ? 0 : int((fy - float(y0)) * 256.0f);
        for (u32 x = 0; x < w; ++x)
        {
            const float fx = (float(x) + 0.5f) * float(lw) / float(w) - 0.5f;
            const int x0 = fx < 0 ? 0 : int(fx), x1 = x0 + 1 < int(lw) ? x0 + 1 : x0;
            const int wx = fx < 0 ? 0 : int((fx - float(x0)) * 256.0f);
            const u8* a = &level[(size_t(y0) * lw + x0) * 4u];
            const u8* b = &level[(size_t(y0) * lw + x1) * 4u];
            const u8* c = &level[(size_t(y1) * lw + x0) * 4u];
            const u8* d = &level[(size_t(y1) * lw + x1) * 4u];
            const u8* t = &top[(size_t(y) * w + x) * 4u];
            for (u32 k = 0; k < 4; ++k)
            {
                const int up = a[k] * (256 - wx) + b[k] * wx, down = c[k] * (256 - wx) + d[k] * wx;
                const int e = ((up * (256 - wy) + down * wy + (1 << 15)) >> 16) - t[k];
                error[k] += u64(e * e);
            }
        }
    }
    return double(error[0] + error[1] + error[2]) / (3.0 * texels) <= limit && double(error[3]) / texels <= limit;
}

// How many top levels this image can do without
u32 find_mip_trim(const bc_chain& chain, float min_db, const std::atomic<bool>& stop)
{
    xr_vector<u8> top, level;
    decode_bc_level(chain.pixels, chain.width, chain.height, chain.bc3, top);
    const u8* data = chain.pixels;
    u32 w = chain.width, h = chain.height, levels = 0;
    while (levels < mip_trim_max_levels && mip_trim_fits(chain.width, chain.height, chain.mips, levels + 1))
    {
        data += size_t((w + 3u) / 4u) * ((h + 3u) / 4u) * chain.block;
        w = _max(1u, w >> 1u);
        h = _max(1u, h >> 1u);
        decode_bc_level(data, w, h, chain.bc3, level);
        if (!stretched_level_matches(top, chain.width, chain.height, level, w, h, min_db, stop))
            break;
        ++levels;
    }
    return levels;
}

void transcode_one(transcode_state& t, const transcode_job& job)
{
    using namespace quark_dds;
    if (job.min_db > 0.0f)
    {
        bc_chain chain;
        if (!bc_file_chain(job.data, job.size, chain))
            return;
        const u32 levels = find_mip_trim(chain, job.min_db, t.stop);
        if (t.stop.load(std::memory_order_relaxed))
            return;
        mip_trim_store(job.key, levels);
        xrCriticalSectionGuard guard(t.lock);
        ++t.trim_checked;
        if (levels)
            ++t.trim_found;
        return;
    }
    rgba8_layout layout;
    if (!rgba8_file_layout(job.data, job.size, layout))
        return;

    size_t blocks_bytes = 0;
    {
        u32 w = layout.width, h = layout.height;
        for (u32 mip = 0; mip < layout.mips; ++mip)
        {
            blocks_bytes += size_t((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u;
            w = _max(1u, w >> 1u);
            h = _max(1u, h >> 1u);
        }
    }
    blocks_bytes *= layout.slices;

    const size_t head_bytes = sizeof(u32) + sizeof(header) + sizeof(header_dx10);
    const size_t file_bytes = head_bytes + blocks_bytes + sizeof(u64);
    std::unique_ptr<u8, rgb24_pixels_deleter> out(static_cast<u8*>(xr_malloc_uninitialized(file_bytes)));

    header hdr = {};
    hdr.size = sizeof(header);
    hdr.flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000; // caps, height, width, pixel format, mip count, linear size
    hdr.height = layout.height;
    hdr.width = layout.width;
    hdr.pitch_or_linear_size = ((layout.width + 3u) / 4u) * ((layout.height + 3u) / 4u) * 16u;
    hdr.depth = 1;
    hdr.mip_map_count = layout.mips;
    hdr.format.size = sizeof(pixel_format);
    hdr.format.flags = ddpf_fourcc;
    hdr.format.fourcc = fourcc('D', 'X', '1', '0');
    hdr.caps = 0x1000 | 0x8 | 0x400000; // texture, complex, mipmap
    header_dx10 ext = {};
    ext.format = DXGI_FORMAT_BC7_UNORM;
    ext.resource_dimension = dim_texture2d;
    ext.misc_flag = layout.cube ? misc_texturecube : 0u;
    ext.array_size = layout.cube ? layout.slices / 6u : layout.slices;
    std::memcpy(out.get(), &magic, sizeof(u32));
    std::memcpy(out.get() + sizeof(u32), &hdr, sizeof(hdr));
    std::memcpy(out.get() + sizeof(u32) + sizeof(hdr), &ext, sizeof(ext));

    const u8* source = job.data + layout.offset;
    u8* block = out.get() + head_bytes;
    double visible_error = 0, alpha_error = 0, texel_count = 0;
    for (u32 slice = 0; slice < layout.slices; ++slice)
    {
        u32 w = layout.width, h = layout.height;
        for (u32 mip = 0; mip < layout.mips; ++mip)
        {
            for (u32 by = 0; by < (h + 3u) / 4u; ++by)
            {
                if (t.stop.load(std::memory_order_relaxed))
                    return;
                for (u32 bx = 0; bx < (w + 3u) / 4u; ++bx, block += 16)
                {
                    u8 texels[64], decoded[64];
                    for (u32 y = 0; y < 4; ++y)
                        for (u32 x = 0; x < 4; ++x)
                        {
                            // The small mips are smaller than a block: their last texels repeat
                            const u8* in = source + (size_t(_min(by * 4u + y, h - 1u)) * w + _min(bx * 4u + x, w - 1u)) * 4u;
                            u8* to = texels + (y * 4u + x) * 4u;
                            to[0] = layout.bgr ? in[2] : in[0];
                            to[1] = in[1];
                            to[2] = layout.bgr ? in[0] : in[2];
                            to[3] = layout.no_alpha ? u8(255) : in[3];
                        }
                    bc7::encode_block(texels, block);
                    bc7::decode_block(block, decoded);
                    for (u32 i = 0; i < 64; i += 4)
                    {
                        const double a = double(texels[i + 3]) / 255.0;
                        double e = 0;
                        for (u32 c = 0; c < 3; ++c)
                        {
                            const double d = double(decoded[i + c]) - double(texels[i + c]);
                            e += d * d;
                        }
                        visible_error += e * a * a;
                        const double d = double(decoded[i + 3]) - double(texels[i + 3]);
                        alpha_error += d * d;
                    }
                    texel_count += 16.0;
                }
            }
            source += size_t(w) * h * 4u;
            w = _max(1u, w >> 1u);
            h = _max(1u, h >> 1u);
        }
    }

    // Quality gate. Colour counts as far as it is seen; about 40 dB is where a difference starts to be visible.
    const auto psnr = [](double error, double samples) { return error > 0 ? 10.0 * log10(255.0 * 255.0 * samples / error) : 100.0; };
    const bool good = psnr(visible_error, texel_count * 3.0) >= 40.0 && psnr(alpha_error, texel_count) >= 36.0;
    if (!good)
    {
        write_transcoded(job.key, nullptr, 0); // an empty .no file: do not try this one again
        xrCriticalSectionGuard guard(t.lock);
        ++t.rejected;
        return;
    }
    const u64 hash = content_hash(out.get(), file_bytes - sizeof(u64));
    std::memcpy(out.get() + file_bytes - sizeof(u64), &hash, sizeof(u64));
    write_transcoded(job.key, out.get(), file_bytes);
    xrCriticalSectionGuard guard(t.lock);
    ++t.encoded;
    t.bytes_in += job.size;
    t.bytes_out += file_bytes;
}

DWORD WINAPI transcode_thread(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    transcode_state& t = transcoder();
    for (;;)
    {
        transcode_job job;
        {
            xrCriticalSectionGuard guard(t.lock);
            if (t.queue.empty() || t.stop.load(std::memory_order_relaxed))
            {
                if (t.encoded || t.rejected || t.trim_checked)
                    Msg("* [texture-bc7] background: %u textures encoded, %u -> %u MB, %u stay uncompressed; %u checked for useless top mips, %u have them",
                        t.encoded, u32(t.bytes_in >> 20), u32(t.bytes_out >> 20), t.rejected, t.trim_checked, t.trim_found);
                t.encoded = t.rejected = t.trim_checked = t.trim_found = 0;
                t.bytes_in = t.bytes_out = 0;
                t.running = false;
                return 0;
            }
            job = t.queue.back();
            t.queue.pop_back();
        }
        transcode_one(t, job);
        xr_free(job.data);
        xrCriticalSectionGuard guard(t.lock);
        t.queued_bytes -= job.size;
    }
}

void queue_transcode(const shared_surface_key& key, const void* data, size_t size, float min_db = 0.0f)
{
    transcode_state& t = transcoder();
    // The file stays in memory until its turn: this bounds it. A texture past the limit gets its chance on the next load.
    static const u64 queue_limit = u64(xr_memory_policy::query().mode == xr_memory_policy::tier::constrained ? 128u : 1024u) * 1024u * 1024u;
    {
        xrCriticalSectionGuard guard(t.lock);
        if (t.stop.load(std::memory_order_relaxed) || t.queued_bytes + size > queue_limit || !t.seen.insert(key.hash).second)
            return;
        t.queued_bytes += size;
    }
    u8* copy = static_cast<u8*>(xr_malloc_uninitialized(size));
    std::memcpy(copy, data, size);
    xrCriticalSectionGuard guard(t.lock);
    t.queue.push_back({ key, copy, size, min_db });
    if (!t.running)
    {
        if (t.thread)
        {
            WaitForSingleObject(t.thread, INFINITE); // it left its loop under this lock's last hold, nearly gone
            CloseHandle(t.thread);
        }
        t.running = true;
        t.thread = CreateThread(nullptr, 0, &transcode_thread, nullptr, 0, nullptr);
        if (!t.thread)
            t.running = false;
    }
}
}

void CRender::texture_transcode_stop()
{
    transcode_state& t = transcoder();
    HANDLE thread;
    {
        xrCriticalSectionGuard guard(t.lock);
        t.stop.store(true);
        thread = t.thread;
    }
    if (thread)
        WaitForSingleObject(thread, INFINITE);
    xrCriticalSectionGuard guard(t.lock);
    for (transcode_job& job : t.queue)
        xr_free(job.data);
    t.queue.clear();
    t.queued_bytes = 0;
    t.seen.clear();
    if (t.thread)
        CloseHandle(t.thread);
    t.thread = nullptr;
    t.stop.store(false);
}

ID3DBaseTexture* CRender::texture_staged_shared(ID3DBaseTexture* staging)
{
    shared_surface_registry& registry = shared_surfaces();
    shared_surface_key key;
    {
        xrCriticalSectionGuard guard(registry.lock);
        const auto found = registry.staged.find(staging);
        if (found == registry.staged.end())
            return nullptr;
        key = found->second;
    }
    return acquire_shared_surface(key);
}

ID3DBaseTexture* CRender::texture_staged_publish(ID3DBaseTexture* staging, ID3DBaseTexture* target)
{
    shared_surface_registry& registry = shared_surfaces();
    shared_surface_key key;
    {
        xrCriticalSectionGuard guard(registry.lock);
        const auto found = registry.staged.find(staging);
        if (found == registry.staged.end())
            return target;
        key = found->second;
    }
    return publish_shared_surface(key, target);
}

void CRender::texture_unload(ID3DBaseTexture* surface)
{
    if (!surface)
        return;
    shared_surface_registry& registry = shared_surfaces();
    xrCriticalSectionGuard guard(registry.lock);
    if (registry.staged.erase(surface))
        return;
    const auto key = registry.keys.find(surface);
    if (key == registry.keys.end())
        return;
    const auto found = registry.surfaces.find(key->second);
    if (--found->second.owners == 0)
    {
        found->second.surface->Release();
        registry.surfaces.erase(found);
        registry.keys.erase(key);
    }
}

u64 CTexture::ResidentBytes() const
{
    // Texture payload estimate, not a driver heap allocation measurement. Do not
    // convert a staging resource or issue an immediate-context call to price it.
    if (!pSurface || flags.bLoadedAsStaging)
        return 0;

    D3D_RESOURCE_DIMENSION dimension;
    pSurface->GetType(&dimension);
    u32 width = 0, height = 1, depth = 1, mips = 0, slices = 1, samples = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    switch (dimension)
    {
    case D3D_RESOURCE_DIMENSION_TEXTURE1D:
        {
            D3D11_TEXTURE1D_DESC d = {};
            static_cast<ID3D11Texture1D*>(pSurface)->GetDesc(&d);
            if (d.Usage == D3D11_USAGE_STAGING) return 0;
            width = d.Width; mips = d.MipLevels; slices = d.ArraySize; format = d.Format;
        }
        break;
    case D3D_RESOURCE_DIMENSION_TEXTURE2D:
        {
            D3D11_TEXTURE2D_DESC d = {};
            static_cast<ID3D11Texture2D*>(pSurface)->GetDesc(&d);
            if (d.Usage == D3D11_USAGE_STAGING) return 0;
            width = d.Width; height = d.Height; mips = d.MipLevels;
            slices = d.ArraySize; samples = d.SampleDesc.Count; format = d.Format;
        }
        break;
    case D3D_RESOURCE_DIMENSION_TEXTURE3D:
        {
            D3D11_TEXTURE3D_DESC d = {};
            static_cast<ID3D11Texture3D*>(pSurface)->GetDesc(&d);
            if (d.Usage == D3D11_USAGE_STAGING) return 0;
            width = d.Width; height = d.Height; depth = d.Depth; mips = d.MipLevels; format = d.Format;
        }
        break;
    default:
        return 0;
    }

    u64 bytes = 0;
    for (u32 mip = 0; mip < mips; ++mip)
    {
        size_t row = 0, slice = 0;
        if (!quark_dds::surface_info(width, height, format, row, slice))
            return 0; // Unknown formats stay on the conservative source-byte price.
        bytes += static_cast<u64>(slice) * depth * slices * samples;
        width = _max(1u, width >> 1u);
        height = _max(1u, height >> 1u);
        depth = _max(1u, depth >> 1u);
    }
    return bytes;
}

static std::atomic<u32> g_quark_dds_legacy_fallbacks{0};

void fix_texture_name(LPSTR fn)
{
	LPSTR _ext = strext(fn);
	if (_ext &&
		(0 == stricmp(_ext, ".tga") ||
			0 == stricmp(_ext, ".dds") ||
			0 == stricmp(_ext, ".bmp") ||
			0 == stricmp(_ext, ".ogm") ||
            0 == stricmp(_ext, ".gif")))
		*_ext = 0;
}

int get_texture_load_lod(LPCSTR fn)
{
	CInifile::Sect& sect = pSettings->r_section("reduce_lod_texture_list");
	CInifile::SectCIt it_ = sect.Data.begin();
	CInifile::SectCIt it_e_ = sect.Data.end();

	ENGINE_API bool is_enough_address_space_available();
	static bool enough_address_space_available = is_enough_address_space_available();

	CInifile::SectCIt it = it_;
	CInifile::SectCIt it_e = it_e_;

	for (; it != it_e; ++it)
	{
		if (strstr(fn, it->first.c_str()))
		{
			if (psTextureLOD < 1)
			{
				if (enough_address_space_available)
					return 0;
				else
					return 1;
			}
			else if (psTextureLOD < 3)
				return 1;
			else
				return 2;
		}
	}

	if (psTextureLOD < 2)
	{
		//		if ( enough_address_space_available )
		return 0;
		//		else
		//			return 1;
	}
	else if (psTextureLOD < 4)
		return 1;
	else
		return 2;
}

u32 calc_texture_size(int lod, u32 mip_cnt, u32 orig_size)
{
	if (1 == mip_cnt)
		return orig_size;

	int _lod = lod;
	float res = float(orig_size);

	while (_lod > 0)
	{
		--_lod;
		res -= res / 1.333f;
	}
	return iFloor(res);
}

const float _BUMPHEIGH = 8.f;
//////////////////////////////////////////////////////////////////////
// Utility pack
//////////////////////////////////////////////////////////////////////
IC u32 GetPowerOf2Plus1(u32 v)
{
	u32 cnt = 0;
	while (v)
	{
		v >>= 1;
		cnt++;
	};
	return cnt;
}

IC void Reduce(int& w, int& h, int& l, int& skip)
{
	while ((l > 1) && skip)
	{
		w /= 2;
		h /= 2;
		l -= 1;

		skip--;
	}
	if (w < 1) w = 1;
	if (h < 1) h = 1;
}

IC void Reduce(UINT& w, UINT& h, int l, int skip)
{
	while ((l > 1) && skip)
	{
		w /= 2;
		h /= 2;
		l -= 1;

		skip--;
	}
	if (w < 1) w = 1;
	if (h < 1) h = 1;
}

void TW_Save(ID3DTexture2D* T, LPCSTR name, LPCSTR prefix, LPCSTR postfix)
{
	string256 fn;
	strconcat(sizeof(fn), fn, name, "_", prefix, "-", postfix);
	for (int it = 0; it < int(xr_strlen(fn)); it++)
		if ('\\' == fn[it]) fn[it] = '_';
	string256 fn2;
	strconcat(sizeof(fn2), fn2, "debug\\", fn, ".dds");
	Log("* debug texture save: ", fn2);
	R_CHK(D3DX11SaveTextureToFile(HW.pContext, T, D3DX11_IFF_DDS, fn2));
}

ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize, bool bStaging, u32* top_mips_skipped)
{
	if (top_mips_skipped)
		*top_mips_skipped = 0;
	runtime_texture_warmup_observer texture_warmup_observer(fRName);

	//	Moved here just to avoid warning
	D3DX11_IMAGE_INFO IMG;
	ZeroMemory(&IMG, sizeof(IMG));

	//	Staging control
	static bool bAllowStaging = !RImplementation.o.no_ram_textures;
	bStaging &= bAllowStaging;

	ID3DBaseTexture* pTexture2D = NULL;
	string_path fn;
	u32 img_size = 0;
	int img_loaded_lod = 0;
	u32 mip_cnt = u32(-1);
	// validation
	R_ASSERT(fRName);
	R_ASSERT(fRName[0]);

	// make file name
	string_path fname;
	xr_strcpy(fname, fRName); //. andy if (strext(fname)) *strext(fname)=0;
	fix_texture_name(fname);
	IReader* S = NULL;
	//if (!FS.exist(fn,"$game_textures$",	fname,	".dds")	&& strstr(fname,"_bump"))	goto _BUMP_from_base;
	if (strstr(fname, "_bump"))
	{
		if (!FS.exist(fn, "$game_textures$", fname, ".dds"))
			goto _BUMP_from_base;
		else if (strstr(Core.Params, "-no_bump_mode2"))
		{
			if (strstr(fname, "_bump#"))
			{
				R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump#", ".dds"), "ed_dummy_bump#");
				S = FS.r_open(fn);
				R_ASSERT2(S, fn);
				img_size = S->length();
				goto _DDS_LOAD;
			}

			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump", ".dds"), "ed_dummy_bump");
			S = FS.r_open(fn);

			R_ASSERT2(S, fn);

			img_size = S->length();
			goto _DDS_LOAD;
		}
		else if (strstr(Core.Params, "-no_bump_mode1") && strstr(fname, "_bump#"))
		{
			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump#", ".dds"), "ed_dummy_bump#");
			S = FS.r_open(fn);
			R_ASSERT2(S, fn);
			img_size = S->length();
			goto _DDS_LOAD;
		}
	}
	if (FS.exist(fn, "$level$", fname, ".dds")) goto _DDS;
	if (FS.exist(fn, "$game_saves$", fname, ".dds")) goto _DDS;
	if (FS.exist(fn, "$game_textures$", fname, ".dds")) goto _DDS;

#ifdef _EDITOR
	ELog.Msg(mtError,"Can't find texture '%s'",fname);
	return 0;
#else

	Msg("! Can't find texture '%s'", fname);
	R_ASSERT(FS.exist(fn,"$game_textures$", "ed\\ed_not_existing_texture",".dds"));
	goto _DDS;

	//	Debug.fatal(DEBUG_INFO,"Can't find texture '%s'",fname);

#endif

_DDS:
	{
		S = FS.r_open(fn);
#ifdef DEBUG
		Msg("* Loaded: %s[%d]", fn, S->length());
#endif // DEBUG
		img_size = S->length();
		R_ASSERT(S);

	_DDS_LOAD:
		{
			string_path lod_name;
			xr_strcpy(lod_name, fn);
			strlwr(lod_name);
			img_loaded_lod = get_texture_load_lod(lod_name);
			string_path lower_name;
			xr_strcpy(lower_name, fname);
			strlwr(lower_name);
			shared_surface_key shared_key = { content_hash(static_cast<const u8*>(S->pointer()), S->length()),
				static_cast<u64>(S->length()), 0 };
			bc_chain chain;
			if (top_mips_skipped && S->length() >= 1024u * 1024u && !strstr(lower_name, "ui\\") && picture_name(lower_name) &&
				bc_file_chain(static_cast<const u8*>(S->pointer()), S->length(), chain))
			{
				u32 trim = 0;
				if (!mip_trim_lookup(shared_key, trim))
					// A correction map is a small signal on top of the normal map, the rest is what the eye gets
					queue_transcode(shared_key, S->pointer(), S->length(), strstr(lower_name, "_bump#") ? 40.0f : 44.0f);
				else if (trim && mip_trim_fits(chain.width, chain.height, chain.mips, u32(img_loaded_lod) + trim))
				{
					img_loaded_lod += int(trim);
					*top_mips_skipped = trim;
				}
			}
			shared_key.lod = img_loaded_lod;
			u32 modern_mips = 0;
			int modern_lod = img_loaded_lod;
			bool bc1_converted = false;
			if ((pTexture2D = acquire_shared_surface(shared_key)) != nullptr)
			{
				FS.r_close(S);
				ret_msize = 0; // the first owner carries the price
				return pTexture2D;
			}
			const void* image = S->pointer();
			size_t image_size = S->length();
			std::unique_ptr<u8, quark_dds::rgb24_pixels_deleter> transcoded;
			if (transcode_candidate(lower_name, image, image_size))
			{
				size_t cached_size = 0;
				bool rejected = false;
				transcoded.reset(load_transcoded(shared_key, cached_size, rejected));
				if (transcoded)
				{
					image = transcoded.get();
					image_size = cached_size;
				}
				else if (!rejected)
					queue_transcode(shared_key, image, image_size);
			}
			if (quark_dds::create_from_memory(
				image, image_size, img_loaded_lod, bStaging,
				&pTexture2D, modern_mips, modern_lod, bc1_converted))
			{
				FS.r_close(S);
				mip_cnt = modern_mips;
				img_loaded_lod = modern_lod;
				ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, bc1_converted ? img_size / 2u : transcoded ? u32(image_size) : img_size);
				if (!bStaging)
					return publish_shared_surface(shared_key, pTexture2D);
				shared_surface_registry& registry = shared_surfaces();
				xrCriticalSectionGuard guard(registry.lock);
				registry.staged.emplace(pTexture2D, shared_key);
				return pTexture2D;
			}

			const u32 fallback_index = g_quark_dds_legacy_fallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
			if (fallback_index <= 8)
				Msg("! DDS runtime-loader fallback #%u: %s", fallback_index, fn);
		}

		// Compatibility-only path for an uncommon DDS layout/legacy format not
		// representable by the zero-conversion runtime loader.
		R_CHK2(D3DX11GetImageInfoFromMemory(S->pointer(), S->length(), 0, &IMG, 0), fn);
		if (IMG.MiscFlags & D3D_RESOURCE_MISC_TEXTURECUBE) goto _DDS_CUBE;
		else goto _DDS_2D;

	_DDS_CUBE:
		{
			//	Inited to default by provided default constructor
			D3DX11_IMAGE_LOAD_INFO LoadInfo;
			//LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
			if (bStaging)
			{
				LoadInfo.Usage = D3D_USAGE_STAGING;
				LoadInfo.BindFlags = 0;
				LoadInfo.CpuAccessFlags = D3D_CPU_ACCESS_WRITE;
			}
			else
			{
				LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
				LoadInfo.BindFlags = D3D_BIND_SHADER_RESOURCE;
			}

			LoadInfo.pSrcInfo = &IMG;

			R_CHK(D3DX11CreateTextureFromMemory(
				HW.pDevice,
				S->pointer(),S->length(),
				&LoadInfo,
				0,
				&pTexture2D,
				0
			));

			FS.r_close(S);

			// OK
			mip_cnt = IMG.MipLevels;
			ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, img_size);
			return pTexture2D;
		}
	_DDS_2D:
		{
			// Check for LMAP and compress if needed
			strlwr(fn);

			// Load   SYS-MEM-surface, bound to device restrictions
			img_loaded_lod = get_texture_load_lod(fn);

			//	Inited to default by provided default constructor
			D3DX11_IMAGE_LOAD_INFO LoadInfo;
			LoadInfo.FirstMipLevel = img_loaded_lod;
			LoadInfo.MipLevels = IMG.MipLevels;
			LoadInfo.Width = IMG.Width;
			LoadInfo.Height = IMG.Height;

			if (img_loaded_lod)
			{
				Reduce(LoadInfo.Width, LoadInfo.Height, IMG.MipLevels, img_loaded_lod);
			}

			//LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
			if (bStaging)
			{
				LoadInfo.Usage = D3D_USAGE_STAGING;
				LoadInfo.BindFlags = 0;
				LoadInfo.CpuAccessFlags = D3D_CPU_ACCESS_WRITE;
			}
			else
			{
				LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
				LoadInfo.BindFlags = D3D_BIND_SHADER_RESOURCE;
			}
			LoadInfo.pSrcInfo = &IMG;

			R_CHK2(D3DX11CreateTextureFromMemory
			       (
				       HW.pDevice,S->pointer(),S->length(),
				       &LoadInfo,
				       0,
				       &pTexture2D,
				       0
			       ), fn);
			FS.r_close(S);
			mip_cnt = IMG.MipLevels;
			// OK
			ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, img_size);
			return pTexture2D;
		}
	}

_BUMP_from_base:
	{
		//Msg			("! auto-generated bump map: %s",fname);
		Msg("! Fallback to default bump map: %s", fname);
		//////////////////
		if (strstr(fname, "_bump#"))
		{
			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump#", ".dds"), "ed_dummy_bump#");
			S = FS.r_open(fn);
			R_ASSERT2(S, fn);
			img_size = S->length();
			goto _DDS_LOAD;
		}
		if (strstr(fname, "_bump"))
		{
			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump", ".dds"), "ed_dummy_bump");
			S = FS.r_open(fn);

			R_ASSERT2(S, fn);

			img_size = S->length();
			goto _DDS_LOAD;
		}
		//////////////////
	}

	return 0;
}
