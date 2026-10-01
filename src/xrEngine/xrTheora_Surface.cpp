#include "stdafx.h"
#include <immintrin.h>
#include <cstring>
#include <array>
#include "xrtheora_surface.h"
#include "xrtheora_stream.h"

CTheoraSurface::CTheoraSurface()
{
	ready = FALSE;
	// streams
	m_rgb = 0;
	m_alpha = 0;
	// timing
	tm_play = 0;
	tm_total = 0;
	// sdl
#ifdef SDL_OUTPUT
    sdl_screen = 0;
    sdl_yuv_overlay = 0;
#endif
	// controls
	playing = FALSE;
	looped = FALSE;
	bShaderYUV2RGB = TRUE;
	prefetch = -2;
}

CTheoraSurface::~CTheoraSurface()
{
	xr_delete(m_rgb);
	xr_delete(m_alpha);
#ifdef SDL_OUTPUT
    SDL_Quit ();
#endif
}

void CTheoraSurface::Reset()
{
	if (m_rgb) m_rgb->Reset();
	if (m_alpha) m_alpha->Reset();
	tm_play = 0;
}

BOOL CTheoraSurface::Valid()
{
	return ready;
}

void CTheoraSurface::Play(BOOL _looped, u32 _time)
{
	playing = TRUE;
	looped = _looped;
	tm_start = _time;
	prefetch = -2;
}

BOOL CTheoraSurface::Update(u32 _time)
{
	VERIFY(Valid());
	BOOL redraw = FALSE;

	if (prefetch < 0) //fake. first updated frame is data loading
	{
		++prefetch;
		if (prefetch == 0)
			tm_start = _time;

		tm_play = 0;
	}
	else
	{
		if (playing)
			tm_play = _time - tm_start;
	}
	if (playing)
	{
		if (tm_play >= tm_total)
		{
			if (looped)
			{
				tm_start = tm_start + tm_total;
				Reset();
			}
			else
			{
				Stop();
				return FALSE;
			}
		}
		if (m_rgb) redraw |= m_rgb->Decode(tm_play);
		if (m_alpha) redraw |= m_alpha->Decode(tm_play);
	}

	return redraw;
}

BOOL CTheoraSurface::Load(const char* fname)
{
	VERIFY(FALSE == ready);
	m_rgb = xr_new<CTheoraStream>();
	BOOL res = m_rgb->Load(fname);
	if (res)
	{
		string_path alpha, ext;
		xr_strcpy(alpha, fname);
		pstr pext = strext(alpha);
		if (pext)
		{
			xr_strcpy(ext, pext);
			*pext = 0;
		}
		strconcat(sizeof(alpha), alpha, alpha, "#alpha", ext);
		if (FS.exist(alpha))
		{
			m_alpha = xr_new<CTheoraStream>();
			if (!m_alpha->Load(alpha)) res = FALSE;
		}
	}
	if (res)
	{
#ifdef DEBUG
        if (m_alpha)
        {
            VERIFY(m_rgb->tm_total == m_alpha->tm_total);
            VERIFY(m_rgb->t_info.frame_width == m_alpha->t_info.frame_width);
            VERIFY(m_rgb->t_info.frame_height == m_alpha->t_info.frame_height);
            VERIFY(m_rgb->t_info.pixelformat == m_alpha->t_info.pixelformat);
        }
#endif
		//. VERIFY3 (btwIsPow2(m_rgb->t_info.frame_width)&&btwIsPow2(m_rgb->t_info.frame_height),"Invalid size.",fname);
		tm_total = m_rgb->tm_total;
		VERIFY(0 != tm_total);
		// reset playback
		Reset();
		// open SDL video
#ifdef SDL_OUTPUT
        open_sdl_video ();
#endif
		ready = TRUE;
	}
	else
	{
		xr_delete(m_rgb);
		xr_delete(m_alpha);
	}
	if (res)
	{
		// TODO: get shader version here for theora surface
		//VERIFY(0);

		//u32 v_dev = CAP_VERSION(HW.Caps.raster_major, HW.Caps.raster_minor);
		//u32 v_need = CAP_VERSION(2,0);
		//bShaderYUV2RGB = (v_dev>=v_need);
#ifndef _EDITOR
		R_ASSERT(Device.m_pRender);
		bShaderYUV2RGB = Device.m_pRender->HWSupportsShaderYUV2RGB();
#else // _EDITOR
        bShaderYUV2RGB = false;
#endif // _EDITOR
	}
	return res;
}

u32 CTheoraSurface::Width(bool bRealSize)
{
	// return m_rgb->t_info.frame_width;

	if (bRealSize)
		return m_rgb->t_info.frame_width;
	else
		return btwPow2_Ceil((u32)m_rgb->t_info.frame_width);
}

u32 CTheoraSurface::Height(bool bRealSize)
{
	// return m_rgb->t_info.frame_height;

	if (bRealSize)
		return m_rgb->t_info.frame_height;
	else
		return btwPow2_Ceil((u32)m_rgb->t_info.frame_height);;
}


namespace
{
IC __m256i load_8_luma(const u8* source)
{
    return _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(source)));
}

IC __m256i load_4_chroma_duplicated(const u8* source)
{
    int packed;
    std::memcpy(&packed, source, sizeof(packed));
    __m128i bytes = _mm_cvtsi32_si128(packed);
    bytes = _mm_unpacklo_epi8(bytes, bytes);
    return _mm256_cvtepu8_epi32(bytes);
}

IC __m256i clamp_byte_epi32(__m256i value)
{
    const __m256i zero = _mm256_setzero_si256();
    const __m256i maximum = _mm256_set1_epi32(255);
    return _mm256_min_epi32(_mm256_max_epi32(value, zero), maximum);
}

IC __m256i yuv420_to_rgba8(const u8* y_source, const u8* u_source, const u8* v_source)
{
    const __m256i y = load_8_luma(y_source);
    const __m256i u = load_4_chroma_duplicated(u_source);
    const __m256i v = load_4_chroma_duplicated(v_source);

    const __m256i c = _mm256_sub_epi32(y, _mm256_set1_epi32(16));
    const __m256i d = _mm256_sub_epi32(u, _mm256_set1_epi32(128));
    const __m256i e = _mm256_sub_epi32(v, _mm256_set1_epi32(128));
    const __m256i rounding = _mm256_set1_epi32(128);

    __m256i red = _mm256_add_epi32(_mm256_mullo_epi32(c, _mm256_set1_epi32(298)),
        _mm256_mullo_epi32(e, _mm256_set1_epi32(409)));
    red = _mm256_srai_epi32(_mm256_add_epi32(red, rounding), 8);

    __m256i green = _mm256_sub_epi32(_mm256_mullo_epi32(c, _mm256_set1_epi32(298)),
        _mm256_mullo_epi32(d, _mm256_set1_epi32(100)));
    green = _mm256_sub_epi32(green, _mm256_mullo_epi32(e, _mm256_set1_epi32(208)));
    green = _mm256_srai_epi32(_mm256_add_epi32(green, rounding), 8);

    __m256i blue = _mm256_add_epi32(_mm256_mullo_epi32(c, _mm256_set1_epi32(298)),
        _mm256_mullo_epi32(d, _mm256_set1_epi32(516)));
    blue = _mm256_srai_epi32(_mm256_add_epi32(blue, rounding), 8);

    red = clamp_byte_epi32(red);
    green = clamp_byte_epi32(green);
    blue = clamp_byte_epi32(blue);

    return _mm256_or_si256(_mm256_set1_epi32(static_cast<int>(0xff000000u)),
        _mm256_or_si256(_mm256_slli_epi32(red, 16),
            _mm256_or_si256(_mm256_slli_epi32(green, 8), blue)));
}

IC __m256i pack_yuv420_shader8(const u8* y_source, const u8* u_source, const u8* v_source)
{
    const __m256i y = load_8_luma(y_source);
    const __m256i u = load_4_chroma_duplicated(u_source);
    const __m256i v = load_4_chroma_duplicated(v_source);

    return _mm256_or_si256(_mm256_set1_epi32(static_cast<int>(0xff000000u)),
        _mm256_or_si256(_mm256_slli_epi32(y, 16),
            _mm256_or_si256(_mm256_slli_epi32(u, 8), v)));
}
}

void CTheoraSurface::DecompressFrame(u32* data, u32 _width, int& _pos)
{
	VERIFY(m_rgb);
	yuv_buffer* yuv_rgb = m_rgb->CurrentFrame();
	yuv_buffer* yuv_alpha = m_alpha ? m_alpha->CurrentFrame() : 0;

	u32 width = Width(true);
	u32 height = Height(true);

	static const float K = 0.256788f + 0.504129f + 0.097906f;

	// we use ffmpeg2theora for encoding, so only OC_PF_420 valid
	// u32 pixelformat = m_rgb->t_info.pixelformat;

	// rgb
	if (yuv_rgb)
	{
		yuv_buffer& yuv = *yuv_rgb;

		u32 pos = 0;

		if (!bShaderYUV2RGB)
		{
			const u32 destination_stride = width + _width;
			for (u32 h = 0; h < height; ++h)
			{
				const u32 uv_stride_add = yuv.uv_stride * (h >> 1);
				const u8* Y = yuv.y + yuv.y_stride * h;
				const u8* U = yuv.u + uv_stride_add;
				const u8* V = yuv.v + uv_stride_add;
				u32* destination = data + h * destination_stride;

				u32 w = 0;
				for (; w + 8 <= width; w += 8)
				{
					const __m256i rgba = yuv420_to_rgba8(Y + w, U + (w >> 1), V + (w >> 1));
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + w), rgba);
				}

				for (; w < width; ++w)
				{
					const u32 uv_idx = w >> 1;
					const int C = Y[w] - 16;
					const int D = U[uv_idx] - 128;
					const int E = V[uv_idx] - 128;
					const int R = clampr((298 * C + 409 * E + 128) >> 8, 0, 255);
					const int G = clampr((298 * C - 100 * D - 208 * E + 128) >> 8, 0, 255);
					const int B = clampr((298 * C + 516 * D + 128) >> 8, 0, 255);
					destination[w] = color_rgba(R, G, B, 255);
				}
			}
			pos = destination_stride * height;
		}
		else
		{
			const u32 buff_step = width + _width;
			const u32 buff_double_step = buff_step << 1;

			for (u32 y_h = 0, uv_h = 0; y_h < height; y_h += 2, ++uv_h, pos += buff_double_step)
			{
				const u32 uv_stride_add = yuv.uv_stride * uv_h;
				const u8* Y0 = yuv.y + yuv.y_stride * y_h;
				const u8* U = yuv.u + uv_stride_add;
				const u8* Y1 = Y0 + yuv.y_stride;
				const u8* V = yuv.v + uv_stride_add;
				u32* destination0 = data + pos;
				u32* destination1 = destination0 + buff_step;

				u32 y_w = 0;
				for (; y_w + 8 <= width; y_w += 8)
				{
					const u32 uv_w = y_w >> 1;
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(destination0 + y_w),
						pack_yuv420_shader8(Y0 + y_w, U + uv_w, V + uv_w));
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(destination1 + y_w),
						pack_yuv420_shader8(Y1 + y_w, U + uv_w, V + uv_w));
				}

				for (; y_w < width; y_w += 2)
				{
					const u32 uv_w = y_w >> 1;
					const u32 common_part = 255u << 24 | static_cast<u32>(U[uv_w]) << 8 | V[uv_w];
					destination0[y_w] = common_part | static_cast<u32>(Y0[y_w]) << 16;
					destination0[y_w + 1] = common_part | static_cast<u32>(Y0[y_w + 1]) << 16;
					destination1[y_w] = common_part | static_cast<u32>(Y1[y_w]) << 16;
					destination1[y_w + 1] = common_part | static_cast<u32>(Y1[y_w + 1]) << 16;
				}
			}
		}
		_mm256_zeroupper();
		_pos = pos;
	}

	// alpha
	if (yuv_alpha)
	{
		static const std::array<u32, 256> alpha_table = []
		{
			std::array<u32, 256> table{};
			constexpr float conversion = 0.256788f + 0.504129f + 0.097906f;
			for (u32 value = 0; value < table.size(); ++value)
				table[value] = static_cast<u32>(iFloor(float(static_cast<int>(value) - 16) / conversion) & 0xff) << 24;
			return table;
		}();

		yuv_buffer& yuv = *yuv_alpha;
		const __m256i rgb_mask = _mm256_set1_epi32(0x00ffffff);
		u32 pos = 0;
		for (u32 h = 0; h < height; ++h)
		{
			const u8* Y = yuv.y + yuv.y_stride * h;
			u32* destination = data + pos + 1;
			u32 w = 0;
			for (; w + 8 <= width; w += 8)
			{
				const __m256i indices = load_8_luma(Y + w);
				const __m256i alpha_values = _mm256_i32gather_epi32(
					reinterpret_cast<const int*>(alpha_table.data()), indices, sizeof(u32));
				const __m256i colors = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destination + w));
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + w),
					_mm256_or_si256(_mm256_and_si256(colors, rgb_mask), alpha_values));
			}
			for (; w < width; ++w)
				destination[w] = subst_alpha(destination[w], iFloor(float((Y[w] - 16)) / K));
			pos += width;
		}
		_mm256_zeroupper();
	}
}

#ifdef SDL_OUTPUT
void CTheoraSurface::open_sdl_video()
{
    VERIFY(m_rgb);
    theora_info& t_info = m_rgb->t_info;

    if ( SDL_Init(SDL_INIT_VIDEO) < 0 )
    {
        msg ("Unable to init SDL: %s", SDL_GetError());
        return;
    }

    sdl_screen = SDL_SetVideoMode(t_info.frame_width, t_info.frame_height, 0, SDL_SWSURFACE);
    if ( sdl_screen == NULL )
    {
        msg ("Unable to set %dx%d video: %s", t_info.frame_width,t_info.frame_height,SDL_GetError());
        return;
    }

    sdl_yuv_overlay = SDL_CreateYUVOverlay(t_info.frame_width, t_info.frame_height, SDL_YV12_OVERLAY, sdl_screen);
    if ( sdl_yuv_overlay == NULL )
    {
        msg ("SDL: Couldn't create SDL_yuv_overlay: %s", SDL_GetError());
        return;
    }
    sdl_rect.x = 0;
    sdl_rect.y = 0;
    sdl_rect.w = t_info.frame_width;
    sdl_rect.h = t_info.frame_height;

    SDL_DisplayYUVOverlay (sdl_yuv_overlay, &sdl_rect);
}

void CTheoraSurface::write_sdl_video()
{
    VERIFY(m_rgb);
    theora_info& t_info = m_rgb->t_info;
    yuv_buffer& t_yuv_buffer= *m_rgb->current_yuv_buffer();
    int i;
    int crop_offset;
    // Lock SDL_yuv_overlay
    if ( SDL_MUSTLOCK(sdl_screen) )
        if ( SDL_LockSurface(sdl_screen) < 0 ) return;
    if (SDL_LockYUVOverlay(sdl_yuv_overlay) < 0) return;
    // let's draw the data (*yuv[3]) on a SDL screen (*screen)
// deal with border stride
// reverse u and v for SDL
    // and crop input properly, respecting the encoded frame rect
    crop_offset=t_info.offset_x+t_yuv_buffer.y_stride*t_info.offset_y;
    for(i=0; i<sdl_yuv_overlay->h; i++)
        mem_copy(sdl_yuv_overlay->pixels[0]+sdl_yuv_overlay->pitches[0]*i, t_yuv_buffer.y+crop_offset+t_yuv_buffer.y_stride*i, sdl_yuv_overlay->w);
    crop_offset=(t_info.offset_x/2)+(t_yuv_buffer.uv_stride)*(t_info.offset_y/2);
    for(i=0; i<sdl_yuv_overlay->h/2; i++)
    {
        mem_copy(sdl_yuv_overlay->pixels[1]+sdl_yuv_overlay->pitches[1]*i, t_yuv_buffer.v+crop_offset+t_yuv_buffer.uv_stride*i, sdl_yuv_overlay->w/2);
        mem_copy(sdl_yuv_overlay->pixels[2]+sdl_yuv_overlay->pitches[2]*i, t_yuv_buffer.u+crop_offset+t_yuv_buffer.uv_stride*i, sdl_yuv_overlay->w/2);
    }
    // Unlock SDL_yuv_overlay
    if ( SDL_MUSTLOCK(sdl_screen) ) SDL_UnlockSurface(sdl_screen);
    SDL_UnlockYUVOverlay(sdl_yuv_overlay);
    // Show, baby, show!
    SDL_DisplayYUVOverlay(sdl_yuv_overlay, &sdl_rect);
}
#endif
