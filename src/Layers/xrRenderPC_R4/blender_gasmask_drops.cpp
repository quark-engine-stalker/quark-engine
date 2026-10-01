#include "stdafx.h"

#include "blender_gasmask_drops.h"

CBlender_gasmask_drops::CBlender_gasmask_drops(bool useGenericSource) : m_useGenericSource(useGenericSource) { description.CLS = 0; }

CBlender_gasmask_drops::~CBlender_gasmask_drops()
{
}

void CBlender_gasmask_drops::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	C.r_Pass("stub_screen_space", "gasmask_drops", FALSE, FALSE, FALSE);
	C.r_dx10Texture("s_image", m_useGenericSource ? r2_RT_generic : r2_RT_generic0);
	C.r_dx10Texture("s_noise", "shaders\\gasmasks\\mask_noise"); //Precomputed noise

	C.r_dx10Sampler("smp_base");
	C.r_dx10Sampler("smp_nofilter");
	C.r_dx10Sampler("smp_rtlinear");
	C.r_dx10Sampler("smp_linear");
	C.r_End();
}
