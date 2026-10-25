#include "stdafx.h"

void CRenderTarget::phase_smap_spot_clear()
{
	/*
	if (RImplementation.b_HW_smap)		u_setrt	(rt_smap_surf, NULL, NULL, rt_smap_d_depth->pRT);
	else								u_setrt	(rt_smap_surf, NULL, NULL, rt_smap_d_ZB);
	CHK_DX								(HW.pDevice->Clear( 0L, NULL, D3DCLEAR_ZBUFFER,	0xffffffff,	1.0f, 0L));
	*/

	HW.pContext->ClearDepthStencilView(rt_smap_depth->pZRT, D3D_CLEAR_DEPTH, 1.0f, 0L);
}

void CRenderTarget::phase_smap_spot(light* L)
{
	// Targets + viewport
	//	TODO: DX10: CHeck if we don't need old-style SMAP
	if (RImplementation.o.HW_smap) u_setrt(rt_smap_surf, NULL, NULL, rt_smap_depth->pZRT);
		//else								u_setrt	(rt_smap_surf, NULL, NULL, rt_smap_ZB);
	else
		VERIFY(!"Use HW SMap only for DX10!");
	D3D_VIEWPORT VP = {(float)L->X.S.posX, (float)L->X.S.posY, (float)L->X.S.size, (float)L->X.S.size, 0, 1};
	//CHK_DX								(HW.pDevice->SetViewport(&VP));
	HW.pContext->RSSetViewports(1, &VP);

	// Misc		- draw only front-faces //back-faces
	RCache.set_CullMode(CULL_CCW);
	RCache.set_Stencil(FALSE);
	// no transparency
#pragma todo("can optimize for multi-lights covering more than say 50%...")
	if (RImplementation.o.HW_smap) RCache.set_ColorWriteEnable(FALSE);
	//CHK_DX								(HW.pDevice->Clear( 0L, NULL, D3DCLEAR_ZBUFFER,	0xffffffff,	1.0f, 0L));
	//	Do it once per smap generation pass in phase_smap_spot_clear
	//HW.pContext->ClearDepthStencilView( rt_smap_depth->pZRT, D3D_CLEAR_DEPTH, 1.0f, 0L);
}

// A depth-stencil view cannot be cleared or copied partially: one triangle covering
// the bound viewport writes depth into dsv instead, from the vertex shader (ps == null)
// or from ps reading srv. Every device state it touches is restored, so the backend's
// state caches stay valid.
void CRenderTarget::smap_cache_triangle(ID3D11DepthStencilView* dsv, ID3D11PixelShader* ps_new,
	ID3D11ShaderResourceView* srv_new)
{
	ID3D11DeviceContext* context = HW.pContext;
	ID3D11InputLayout* layout = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	ID3D11GeometryShader* gs = nullptr;
	ID3D11HullShader* hs = nullptr;
	ID3D11DomainShader* ds = nullptr;
	ID3D11DepthStencilState* depth = nullptr;
	UINT stencil_ref = 0;
	ID3D11RasterizerState* raster = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11Buffer* cb = nullptr;
	ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* dsv_old = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv_old);
	context->IAGetInputLayout(&layout);
	context->IAGetPrimitiveTopology(&topology);
	context->VSGetShader(&vs, nullptr, nullptr);
	context->PSGetShader(&ps, nullptr, nullptr);
	context->GSGetShader(&gs, nullptr, nullptr);
	context->HSGetShader(&hs, nullptr, nullptr);
	context->DSGetShader(&ds, nullptr, nullptr);
	context->OMGetDepthStencilState(&depth, &stencil_ref);
	context->RSGetState(&raster);
	context->PSGetShaderResources(0, 1, &srv);
	context->PSGetConstantBuffers(0, 1, &cb);

	context->OMSetRenderTargets(0, nullptr, dsv);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(smap_cache_clear_vs, nullptr, 0);
	context->PSSetShader(ps_new, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->OMSetDepthStencilState(smap_cache_clear_depth, 0);
	context->RSSetState(smap_cache_clear_raster);
	context->PSSetShaderResources(0, 1, &srv_new);
	context->PSSetConstantBuffers(0, 1, &smap_cache_copy_cb);
	context->Draw(3, 0);

	context->IASetInputLayout(layout);
	context->IASetPrimitiveTopology(topology);
	context->VSSetShader(vs, nullptr, 0);
	context->PSSetShader(ps, nullptr, 0);
	context->GSSetShader(gs, nullptr, 0);
	context->HSSetShader(hs, nullptr, 0);
	context->DSSetShader(ds, nullptr, 0);
	context->OMSetDepthStencilState(depth, stencil_ref);
	context->RSSetState(raster);
	context->PSSetShaderResources(0, 1, &srv); // before the targets: srv_new may alias dsv_old
	context->PSSetConstantBuffers(0, 1, &cb);
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv_old);
	for (ID3D11RenderTargetView*& rtv : rtvs)
		_RELEASE(rtv);
	_RELEASE(dsv_old);
	_RELEASE(layout);
	_RELEASE(vs);
	_RELEASE(ps);
	_RELEASE(gs);
	_RELEASE(hs);
	_RELEASE(ds);
	_RELEASE(depth);
	_RELEASE(raster);
	_RELEASE(srv);
	_RELEASE(cb);

	RCache.set_CullMode(CULL_CCW);
	RCache.set_Stencil(FALSE);
	RCache.set_ColorWriteEnable(FALSE);
}

void CRenderTarget::phase_smap_cache(light* L)
{
	u_setrt(rt_smap_surf, NULL, NULL, rt_smap_cache->pZRT);
	D3D_VIEWPORT VP = {(float)L->smap_cache.x, (float)L->smap_cache.y, (float)L->X.S.size, (float)L->X.S.size, 0, 1};
	HW.pContext->RSSetViewports(1, &VP);
	smap_cache_triangle(rt_smap_cache->pZRT, nullptr, nullptr); // far depth over the slot
}

void CRenderTarget::smap_cache_blit(light* L)
{
	// The atlas slot bound by phase_smap_spot gets the depth of the face's cache slot.
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(HW.pContext->Map(smap_cache_copy_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	const int offset[4] = {int(L->smap_cache.x) - int(L->X.S.posX), int(L->smap_cache.y) - int(L->X.S.posY), 0, 0};
	memcpy(mapped.pData, offset, sizeof(offset));
	HW.pContext->Unmap(smap_cache_copy_cb, 0);
	smap_cache_triangle(rt_smap_depth->pZRT, smap_cache_copy_ps, rt_smap_cache->pTexture->get_SRView());
}

void CRenderTarget::phase_smap_spot_tsh(light* L)
{
	VERIFY(!"Implement clear of the buffer for tsh!");
	VERIFY(RImplementation.o.Tshadows);
	RCache.set_ColorWriteEnable();
	if (IRender_Light::OMNIPART == L->flags.type)
	{
		// omni-part
		//CHK_DX							(HW.pDevice->Clear( 0L, NULL, D3DCLEAR_TARGET,	0xffffffff,	1.0f, 0L));
		FLOAT ColorRGBA[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		HW.pContext->ClearRenderTargetView(RCache.get_RT(), ColorRGBA);
	}
	else
	{
		// real-spot
		// Select color-mask
		ref_shader shader = L->s_spot;
		if (!shader) shader = s_accum_spot;
		RCache.set_Element(shader->E[SE_L_FILL]);

		// Fill vertex buffer
		Fvector2 p0, p1;
		u32 Offset;
		u32 C = color_rgba(255, 255, 255, 255);
		float _w = float(L->X.S.size);
		float _h = float(L->X.S.size);
		float d_Z = EPS_S;
		float d_W = 1.f;
		p0.set(.5f / _w, .5f / _h);
		p1.set((_w + .5f) / _w, (_h + .5f) / _h);

		FVF::TL* pv = (FVF::TL*)RCache.Vertex.Lock(4, g_combine->vb_stride, Offset);
		pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
		pv++;
		pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
		pv++;
		pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
		pv++;
		pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
		pv++;
		RCache.Vertex.Unlock(4, g_combine->vb_stride);
		RCache.set_Geometry(g_combine);

		// draw
		RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
	}
}
