#include "stdafx.h"
#include "upscaler.h"

void CRenderTarget::phase_occq()
{
	// Light visibility only tests depth and stencil. The back buffer can be larger than scene depth.
	u_setrt(upscaler::width(), upscaler::height(), nullptr, nullptr, nullptr,
		RImplementation.o.dx10_msaa ? rt_MSAADepth->pZRT : HW.pBaseZB);
	RCache.set_Shader(s_occq);
	RCache.set_CullMode(CULL_CCW);
	RCache.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00);
	RCache.set_ColorWriteEnable(FALSE);
}
