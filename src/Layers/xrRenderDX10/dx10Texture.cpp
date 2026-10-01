// Texture.cpp: implementation of the CTexture class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "../xrRender/dxRenderDeviceRender.h"
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/x_ray.h"

#include <atomic>
#include <climits>
#include <cstring>

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

bool create_from_memory(const void* data, size_t size, int requested_lod, bool staging,
    ID3DBaseTexture** out_texture, u32& out_mip_count, int& out_loaded_lod)
{
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
            if (!surface_info(w, h, format, row_bytes, slice_bytes))
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
                sub.pSysMem = src;
                sub.SysMemPitch = static_cast<UINT>(row_bytes);
                sub.SysMemSlicePitch = static_cast<UINT>(slice_bytes);
                init_data.push_back(sub);
            }

            src += mip_bytes;
            w = _max(1u, w >> 1u);
            h = _max(1u, h >> 1u);
            d = _max(1u, d >> 1u);
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

ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize, bool bStaging)
{
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
			u32 modern_mips = 0;
			int modern_lod = img_loaded_lod;
			if (quark_dds::create_from_memory(
				S->pointer(), S->length(), img_loaded_lod, bStaging,
				&pTexture2D, modern_mips, modern_lod))
			{
				FS.r_close(S);
				mip_cnt = modern_mips;
				img_loaded_lod = modern_lod;
				ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, img_size);
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
