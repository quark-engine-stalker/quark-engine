#include "stdafx.h"
#pragma hdrstop

#include "blender_hdr10_bloom.h"

CBlender_hdr10_bloom_downsample::CBlender_hdr10_bloom_downsample(bool useGenericSource) : m_useGenericSource(useGenericSource)	{ description.CLS = 0; }
CBlender_hdr10_bloom_downsample::~CBlender_hdr10_bloom_downsample()	{	}

void CBlender_hdr10_bloom_downsample::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

	C.r_Pass("stub_screen_space", "hdr10_bloom_downsample", FALSE, FALSE, FALSE);

    switch (C.iElement)
    {
		case 0: {
			C.r_dx10Texture("s_hdr10_game", m_useGenericSource ? r2_RT_generic : r2_RT_generic0);
		} break;
    }

	C.r_dx10Sampler("smp_rtlinear");
	C.r_End();
}

CBlender_hdr10_bloom_blur::CBlender_hdr10_bloom_blur()	{ description.CLS = 0; }
CBlender_hdr10_bloom_blur::~CBlender_hdr10_bloom_blur()	{	}

void CBlender_hdr10_bloom_blur::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

	C.r_Pass("stub_screen_space", "hdr10_bloom_blur", FALSE, FALSE, FALSE);

    switch (C.iElement)
    {
		case 0: {
			C.r_dx10Texture("s_hdr10_halfres", r4_RT_HDR10_halfres0);
		} break;

		case 1: {
			C.r_dx10Texture("s_hdr10_halfres", r4_RT_HDR10_halfres1);
		} break;
    }

	C.r_dx10Sampler("smp_rtlinear");
	C.r_End();
}

CBlender_hdr10_bloom_upsample::CBlender_hdr10_bloom_upsample(bool useGenericSource) : m_useGenericSource(useGenericSource)	{ description.CLS = 0; }
CBlender_hdr10_bloom_upsample::~CBlender_hdr10_bloom_upsample()	{	}

void CBlender_hdr10_bloom_upsample::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

	C.r_Pass("stub_screen_space", "hdr10_bloom_upsample", FALSE, FALSE, FALSE);

    switch (C.iElement)
    {
		case 0: {
			C.r_dx10Texture("s_hdr10_halfres", r4_RT_HDR10_halfres0);
		} break;

		case 1: {
			C.r_dx10Texture("s_hdr10_halfres", r4_RT_HDR10_halfres1);
		} break;
    }

	C.r_dx10Texture("s_hdr10_game", m_useGenericSource ? r2_RT_generic : r2_RT_generic0);
	C.r_dx10Sampler("smp_rtlinear");
	C.r_End();
}
