#include "stdafx.h"
#include "upscaler.h"
#include "../xrRenderDX10/StateManager/dx10SamplerStateCache.h"

#include <dlss/nvsdk_ngx_helpers.h>
#include <nvapi.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h>

#pragma comment(lib, "nvsdk_ngx_d.lib")

extern u32 ps_r__upscaler, ps_r__upscaler_mode, ps_r__dlss_model;
extern float ps_r__fsr_sharpness, ps_r__dlss_sharpness;

namespace upscaler
{
namespace
{
// NGX, created once per device on the first use
bool pending = false; // request(): apply() rebuilds the upscaler at the next frame
bool fallback_pending = false;
bool gpu_tried = false, ngx_tried = false, ngx_ready = false, dlss_supported = false, dlss_old_driver = false;
NVSDK_NGX_Parameter* ngx_params = nullptr;
NVSDK_NGX_Handle* feature = nullptr;
u32 gpu_arch = 0; // NV_GPU_ARCHITECTURE_ID of the first NVIDIA GPU, 0 unknown

// AMD FSR 3.1 (sdk/FSR3: the DX11 backend of OGSR), created with the targets
FfxFsr3UpscalerContext* fsr = nullptr;
xr_vector<u64> fsr_scratch; // aligned and zeroed, as the backend wants
ID3D11Texture2D* fsr_shared[3] = {}; // dilated depth, dilated motion vectors, previous nearest depth
ID3D11SamplerState* fsr_samplers[2] = {}; // point and linear clamp: the backend does not create them

// This session of targets
bool on = false;
u32 state = IRenderDeviceRender::upscaler_off;
u32 render_w = 0, render_h = 0;
u32 chosen_preset = 0;
bool in_scene = false, main_view = false, reset_history = true;
u32 jitter_index = 0;
float jitter_px = 0.f, jitter_py = 0.f; // pixels of the render size, +x right, +y down

// A twin is a display-size copy of a scene target: at the end of the scene the two swap their surfaces, so the
// post-process, which reads and writes these names, sees display-size targets
struct twin
{
	ref_rt* scene;
	ref_rt display;
};
twin twins[9];
u32 twin_count = 0;
bool swapped = false; // the scene targets hold the display surfaces

ref_rt depth; // the scene's depth-stencil at the render size, also read by DLSS
ref_rt dlss_output; // DLSS writes here when r__dlss_sharpness sharpens it into rt_Generic_0's twin
ref_rt fsr_opaque, fsr_reactive;
ID3D11DepthStencilView* base_zb = nullptr; // HW.pBaseZB while the scene draws into depth

ID3D11VertexShader* vs = nullptr;
ID3D11PixelShader* ps_nearest = nullptr;
ID3D11PixelShader* ps_linear = nullptr;
ID3D11PixelShader* ps_rcas = nullptr;
ID3D11Buffer* cb = nullptr;
ID3D11DepthStencilState* depth_write = nullptr;
ID3D11DepthStencilState* depth_off = nullptr;
ID3D11RasterizerState* raster = nullptr;
ID3D11SamplerState* linear = nullptr;

const u32 nearest_targets = 5;

bool scaled()
{
	return render_w != Device.dwWidth || render_h != Device.dwHeight;
}

// NVAPI only: instant, the panel asks it every frame
void gpu_init()
{
	if (gpu_tried)
		return;
	gpu_tried = true;
	if (HW.Caps.id_vendor == 0x10DE && NvAPI_Initialize() == NVAPI_OK)
	{
		NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS] = {};
		NvU32 count = 0;
		NV_GPU_ARCH_INFO arch = {};
		arch.version = NV_GPU_ARCH_INFO_VER;
		if (NvAPI_EnumPhysicalGPUs(gpus, &count) == NVAPI_OK && count && NvAPI_GPU_GetArchInfo(gpus[0], &arch) == NVAPI_OK)
			gpu_arch = arch.architecture;
	}
}

// NGX loads and checks the signature of nvngx_dlss.dll (59 MB): about 1.5 s, once, when DLSS is chosen
void ngx_init()
{
	if (ngx_tried)
		return;
	ngx_tried = true;
	gpu_init();
	if (HW.Caps.id_vendor != 0x10DE)
		return;

	CTimer timer;
	timer.Start();

	string_path folder;
	FS.update_path(folder, "$app_data_root$", "");
	wchar_t wide[MAX_PATH] = {};
	MultiByteToWideChar(CP_ACP, 0, folder, -1, wide, MAX_PATH);
	// nvngx_dlss.dll is searched beside the executable
	const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID("6f1c2d7e-93a4-4b8e-b1f5-0c5a7e2d9b31",
		NVSDK_NGX_ENGINE_TYPE_CUSTOM, "QUARK", wide, HW.pDevice);
	if (NVSDK_NGX_FAILED(result))
	{
		Msg("! [DLSS] NGX initialization failed: 0x%08X", u32(result));
		return;
	}
	ngx_ready = true;
	if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&ngx_params)) || !ngx_params)
		return;
	int available = 0, needs_driver = 0;
	ngx_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	ngx_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
	dlss_supported = available != 0;
	dlss_old_driver = needs_driver != 0;
	Msg("* [DLSS] NGX ready in %u ms: super resolution %s, GPU architecture 0x%X", timer.GetElapsed_ms(),
		dlss_supported ? "available" : dlss_old_driver ? "needs a newer driver" : "not available", gpu_arch);
}

bool new_gpu() { return gpu_arch >= NV_GPU_ARCHITECTURE_AD100; } // FP8 tensor cores: RTX 40 and newer

// The method, model and mode in force. AUTO: RTX 20 and 30 DLSS 4, RTX 40 and newer DLSS 4.5, every other video card
// (AMD, Intel, a GeForce without DLSS once NGX has said so) FSR 3.1, always at quality.
void resolve(u32& method, u32& model, u32& m)
{
	method = ps_r__upscaler;
	model = ps_r__dlss_model;
	m = ps_r__upscaler_mode;
	if (method != method_auto)
		return;
	gpu_init();
	method = HW.Caps.id_vendor == 0x10DE && (!ngx_tried || dlss_supported) ? method_dlss : method_fsr;
	model = new_gpu() ? model_dlss45 : model_dlss4;
	m = mode_quality;
}

NVSDK_NGX_PerfQuality_Value quality(u32 m)
{
	switch (m)
	{
	case mode_dlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
	case mode_quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	case mode_balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
	case mode_performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	}
}

// The DLSS preset for one mode. DLSS 3 is the CNN model (preset E), cheap on every RTX; DLSS 4 the first
// transformer (K); DLSS 4.5 the second (M, L at ultra performance: NVIDIA's split), fast only with FP8.
NVSDK_NGX_DLSS_Hint_Render_Preset preset(u32 model, u32 m)
{
	switch (model)
	{
	case model_dlss3: return NVSDK_NGX_DLSS_Hint_Render_Preset_E;
	case model_dlss4: return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
	default: return m == mode_ultra_performance ? NVSDK_NGX_DLSS_Hint_Render_Preset_L : NVSDK_NGX_DLSS_Hint_Render_Preset_M;
	}
}

ID3DBlob* compile(const char* source, const char* profile)
{
	ID3DBlob* code = nullptr;
	ID3DBlob* errors = nullptr;
	if (FAILED(D3DCompile(source, xr_strlen(source), nullptr, nullptr, nullptr, "main", profile,
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
		Msg("! [upscaler] shader: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
	_RELEASE(errors);
	return code;
}

bool create_passes()
{
	static const char vs_source[] =
		"float4 main(uint id : SV_VertexID) : SV_Position"
		"{ return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }";
	// The post-process reads these G-buffer targets at the display size: nearest copies, as NVIDIA recommends
	static const char nearest_source[] =
		"Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2);"
		"Texture2D t3 : register(t3); Texture2D t4 : register(t4); Texture2D<float> d : register(t5);"
		"cbuffer c : register(b0) { float4 scale; };"
		"struct O { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 e : SV_Target3;"
		"float4 f : SV_Target4; float z : SV_Depth; };"
		"O main(float4 p : SV_Position) { int3 s = int3(p.xy * scale.xy, 0); O o;"
		"o.a = t0.Load(s); o.b = t1.Load(s); o.c = t2.Load(s); o.e = t3.Load(s); o.f = t4.Load(s); o.z = d.Load(s);"
		"return o; }";
	// Without DLSS for a frame (3D scope view): a plain stretch
	static const char linear_source[] =
		"Texture2D t0 : register(t0); SamplerState s0 : register(s0); cbuffer c : register(b0) { float4 scale; };"
		"float4 main(float4 p : SV_Position) : SV_Target { return t0.SampleLevel(s0, p.xy * scale.zw, 0); }";
	// RCAS of AMD FidelityFX FSR 1 (MIT), the sharpening FSR 3.1 runs after its upscale; con.x = exp2(-stops)
	static const char rcas_source[] =
		"Texture2D t0 : register(t0); cbuffer c : register(b0) { float4 con; };"
		"float luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }"
		"float4 main(float4 p : SV_Position) : SV_Target { int2 s = int2(p.xy); int2 edge = int2(con.yz) - 1;"
		"float3 b = t0.Load(int3(s.x, max(s.y - 1, 0), 0)).rgb;"
		"float3 d = t0.Load(int3(max(s.x - 1, 0), s.y, 0)).rgb; float4 e = t0.Load(int3(s, 0));"
		"float3 f = t0.Load(int3(min(s.x + 1, edge.x), s.y, 0)).rgb;"
		"float3 h = t0.Load(int3(s.x, min(s.y + 1, edge.y), 0)).rgb;"
		"float3 mn = min(min(b, d), min(f, h)); float3 mx = max(max(b, d), max(f, h));"
		"float3 hit_min = min(mn, e.rgb) / max(4.0 * mx, 1e-5);"
		"float3 hit_max = (1.0 - max(mx, e.rgb)) / min(4.0 * mn - 4.0, -1e-5);"
		"float3 l = max(-hit_min, hit_max); float lobe = max(-0.1875, min(max(l.r, max(l.g, l.b)), 0.0)) * con.x;"
		"float bl = luma(b), dl = luma(d), el = luma(e.rgb), fl = luma(f), hl = luma(h);"
		"float range = max(max(max(bl, dl), max(el, fl)), hl) - min(min(min(bl, dl), min(el, fl)), hl);"
		"lobe *= 1.0 - 0.5 * saturate(abs(0.25 * (bl + dl + fl + hl) - el) / max(range, 1e-5));" // noise removal
		"return float4((lobe * (b + d + f + h) + e.rgb) / (4.0 * lobe + 1.0), e.a); }";

	if (ID3DBlob* code = compile(vs_source, "vs_5_0"))
	{
		HW.pDevice->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
		code->Release();
	}
	if (ID3DBlob* code = compile(nearest_source, "ps_5_0"))
	{
		HW.pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps_nearest);
		code->Release();
	}
	if (ID3DBlob* code = compile(linear_source, "ps_5_0"))
	{
		HW.pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps_linear);
		code->Release();
	}
	if (ID3DBlob* code = compile(rcas_source, "ps_5_0"))
	{
		HW.pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps_rcas);
		code->Release();
	}
	D3D11_BUFFER_DESC buffer = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
	HW.pDevice->CreateBuffer(&buffer, nullptr, &cb);
	D3D11_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable = TRUE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
	HW.pDevice->CreateDepthStencilState(&ds, &depth_write);
	ds.DepthEnable = FALSE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	HW.pDevice->CreateDepthStencilState(&ds, &depth_off);
	D3D11_RASTERIZER_DESC rs = {};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.DepthClipEnable = TRUE;
	HW.pDevice->CreateRasterizerState(&rs, &raster);
	D3D11_SAMPLER_DESC ss = {};
	ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	ss.MaxLOD = D3D11_FLOAT32_MAX;
	HW.pDevice->CreateSamplerState(&ss, &linear);
	return vs && ps_nearest && ps_linear && ps_rcas && cb && depth_write && depth_off && raster && linear;
}

void release_passes()
{
	_RELEASE(vs);
	_RELEASE(ps_nearest);
	_RELEASE(ps_linear);
	_RELEASE(ps_rcas);
	_RELEASE(cb);
	_RELEASE(depth_write);
	_RELEASE(depth_off);
	_RELEASE(raster);
	_RELEASE(linear);
}

// One triangle over the display. The backend's caches are invalidated after the passes (scene_end).
void draw(ID3D11RenderTargetView* const* rtvs, u32 rtv_count, ID3D11DepthStencilView* dsv, ID3D11PixelShader* ps,
	ID3D11ShaderResourceView* const* srvs, u32 srv_count, ID3D11DepthStencilState* ds_state)
{
	ID3D11DeviceContext* context = HW.pContext;
	ID3D11ShaderResourceView* const none[8] = {};
	context->PSSetShaderResources(0, 8, none); // the targets may still be bound as textures
	context->OMSetRenderTargets(rtv_count, rtvs, dsv);
	const D3D11_VIEWPORT viewport = {0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight), 0.f, 1.f};
	context->RSSetViewports(1, &viewport);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(ps, nullptr, 0);
	context->PSSetShaderResources(0, srv_count, srvs);
	context->PSSetConstantBuffers(0, 1, &cb);
	context->PSSetSamplers(0, 1, &linear);
	context->OMSetDepthStencilState(ds_state, 0);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	context->RSSetState(raster);
	context->Draw(3, 0);
	context->PSSetShaderResources(0, 8, none);
}

void swap_twins(bool to_display)
{
	if (swapped == to_display)
		return;
	for (u32 i = 0; i < twin_count; ++i)
		if (twins[i].display)
			(*twins[i].scene)->swap(*twins[i].display);
	swapped = to_display;
	RCache.Invalidate(); // it keeps the texture views it bound by texture, not by surface
}

void fsr_release()
{
	if (fsr)
	{
		try
		{
			ffxFsr3UpscalerContextDestroy(fsr);
		}
		catch (...) {}
		xr_delete(fsr);
	}
	fsr_scratch.clear();
	for (ID3D11Texture2D*& texture : fsr_shared)
		_RELEASE(texture);
	for (ID3D11SamplerState*& sampler : fsr_samplers)
		_RELEASE(sampler);
}

// The backend throws on a failed D3D call: nothing of it may reach the engine
bool fsr_create(u32 w, u32 h)
{
	try
	{
		const size_t bytes = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
		fsr_scratch.assign((bytes + sizeof(u64) - 1) / sizeof(u64), 0);
		FfxFsr3UpscalerContextDescription description = {};
		FfxErrorCode result = ffxGetInterfaceDX11(&description.backendInterface, ffxGetDeviceDX11(HW.pDevice),
			fsr_scratch.data(), bytes, FFX_FSR3UPSCALER_CONTEXT_COUNT);
		if (result != FFX_OK)
		{
			Msg("! [FSR] backend interface: 0x%08X", u32(result));
			return false;
		}
		description.maxRenderSize = {w, h};
		description.maxUpscaleSize = {Device.dwWidth, Device.dwHeight};
		if (RImplementation.o.dx11_hdr10)
			description.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
		fsr = xr_new<FfxFsr3UpscalerContext>();
		ZeroMemory(fsr, sizeof(*fsr));
		result = ffxFsr3UpscalerContextCreate(fsr, &description);
		if (result != FFX_OK)
		{
			Msg("! [FSR] context: 0x%08X", u32(result));
			return false;
		}

		FfxFsr3UpscalerSharedResourceDescriptions shared = {};
		if (ffxFsr3UpscalerGetSharedResourceDescriptions(fsr, &shared) != FFX_OK)
			return false;
		const FfxCreateResourceDescription* const descriptions[3] = {&shared.dilatedDepth, &shared.dilatedMotionVectors,
			&shared.reconstructedPrevNearestDepth};
		for (u32 i = 0; i < 3; ++i)
		{
			const FfxResourceDescription& r = descriptions[i]->resourceDescription;
			D3D11_TEXTURE2D_DESC texture = {};
			texture.Width = r.width;
			texture.Height = r.height;
			texture.MipLevels = r.mipCount;
			texture.ArraySize = 1;
			texture.SampleDesc.Count = 1;
			texture.Format = r.format == FFX_SURFACE_FORMAT_R32_FLOAT ? DXGI_FORMAT_R32_FLOAT :
				r.format == FFX_SURFACE_FORMAT_R16G16_FLOAT ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_UINT;
			texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS |
				(r.usage & FFX_RESOURCE_USAGE_RENDERTARGET ? D3D11_BIND_RENDER_TARGET : 0);
			if (FAILED(HW.pDevice->CreateTexture2D(&texture, nullptr, &fsr_shared[i])))
				return false;
		}
		for (u32 i = 0; i < 2; ++i)
		{
			D3D11_SAMPLER_DESC sampler = {};
			sampler.Filter = i ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(HW.pDevice->CreateSamplerState(&sampler, &fsr_samplers[i])))
				return false;
		}
		return true;
	}
	catch (HRESULT result)
	{
		Msg("! [FSR] Direct3D: 0x%08X", u32(result));
		return false;
	}
	catch (...)
	{
		Msg("! [FSR] exception in the backend");
		return false;
	}
}

bool fsr_dispatch(CRenderTarget& T, CRT& color, CRT& output, bool reset)
{
	PIX_EVENT(FSR_upscale);
	auto resource = [](ID3D11Texture2D* texture, const wchar_t* name, bool written) {
		return ffxGetResourceDX11(texture, GetFfxResourceDescriptionDX11(texture), name,
			written ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
	};
	FfxFsr3UpscalerDispatchDescription d = {};
	d.commandList = ffxGetCommandListDX11(HW.pContext);
	d.color = resource(color.pSurface, L"FSR color", false);
	d.depth = resource(depth->pSurface, L"FSR depth", false);
	d.motionVectors = resource(T.rt_ssfx_motion_vectors->pSurface, L"FSR motion vectors", false);
	d.dilatedDepth = resource(fsr_shared[0], L"FSR dilated depth", true);
	d.dilatedMotionVectors = resource(fsr_shared[1], L"FSR dilated motion vectors", true);
	d.reconstructedPrevNearestDepth = resource(fsr_shared[2], L"FSR previous nearest depth", true);
	d.output = resource(output.pSurface, L"FSR output", true);
	d.jitterOffset = {jitter_px, jitter_py};
	d.motionVectorScale = {-float(render_w), -float(render_h)}; // as for DLSS
	d.renderSize = {render_w, render_h};
	d.upscaleSize = {Device.dwWidth, Device.dwHeight};
	d.enableSharpening = ps_r__fsr_sharpness > 0.f;
	d.sharpness = ps_r__fsr_sharpness;
	d.frameTimeDelta = _max(Device.fTimeDelta * 1000.f, 0.1f);
	d.preExposure = 1.f;
	d.reset = reset;
	// the main view's projection: near, far and the vertical field of view
	d.cameraNear = -Device.mProject._43 / Device.mProject._33;
	d.cameraFar = -Device.mProject._43 / (Device.mProject._33 - 1.f);
	d.cameraFovAngleVertical = 2.f * atanf(1.f / Device.mProject._22);
	d.viewSpaceToMetersFactor = 1.f;
	HW.pContext->CSSetSamplers(0, 2, fsr_samplers);
	FfxErrorCode result;
	try
	{
		FfxFsr3UpscalerGenerateReactiveDescription mask = {};
		mask.commandList = d.commandList;
		mask.colorOpaqueOnly = resource(fsr_opaque->pSurface, L"FSR opaque color", false);
		mask.colorPreUpscale = d.color;
		mask.outReactive = resource(fsr_reactive->pSurface, L"FSR reactive mask", true);
		mask.renderSize = d.renderSize;
		mask.scale = 1.f;
		mask.cutoffThreshold = 0.2f;
		mask.binaryValue = 0.9f;
		mask.flags = FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_THRESHOLD | FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
		if (RImplementation.o.dx11_hdr10)
			mask.flags |= FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_TONEMAP;
		result = ffxFsr3UpscalerContextGenerateReactiveMask(fsr, &mask);
		if (result == FFX_OK)
		{
			d.reactive = resource(fsr_reactive->pSurface, L"FSR reactive mask", false);
			result = ffxFsr3UpscalerContextDispatch(fsr, &d);
		}
	}
	catch (...)
	{
		result = FFX_ERROR_BACKEND_API_ERROR;
	}
	// The backend unbinds its views after each job, unless an exception interrupted it.
	if (result != FFX_OK)
	{
		ID3D11UnorderedAccessView* const no_uav[D3D11_1_UAV_SLOT_COUNT] = {};
		ID3D11ShaderResourceView* const no_srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
		HW.pContext->CSSetUnorderedAccessViews(0, _countof(no_uav), no_uav, nullptr);
		HW.pContext->CSSetShaderResources(0, _countof(no_srv), no_srv);
		Msg("! [FSR] dispatch failed: 0x%08X, returning to native rendering", u32(result));
	}
	HW.pContext->CSSetShader(nullptr, nullptr, 0);
	return result == FFX_OK;
}

void release_feature()
{
	if (feature)
		NVSDK_NGX_D3D11_ReleaseFeature(feature);
	feature = nullptr;
}
} // namespace

u32 width() { return in_scene ? render_w : Device.dwWidth; }
u32 height() { return in_scene ? render_h : Device.dwHeight; }
u32 render_width() { return on ? render_w : Device.dwWidth; }
u32 render_height() { return on ? render_h : Device.dwHeight; }
bool enabled() { return on; }
float mip_bias() { return on ? log2f(float(render_w) / float(Device.dwWidth)) : 0.f; }

void request()
{
	pending = true;
	fallback_pending = false;
}

void apply(CRenderTarget& T)
{
	if (!pending && !fallback_pending)
		return;
	CTimer timer;
	timer.Start();
	const bool was_on = on;
	const u32 previous_w = render_width(), previous_h = render_height();
	release();
	if (fallback_pending)
	{
		fallback_pending = false;
		render_w = Device.dwWidth;
		render_h = Device.dwHeight;
	}
	else
		configure();
	if (previous_w != render_width() || previous_h != render_height())
		T.resize_scene(render_width(), render_height());
	if (was_on || on)
		T.invalidate_temporal_history();
	create(T);
	Msg("* [upscaler] applied in %u ms", timer.GetElapsed_ms());
}

void configure()
{
	pending = false; // a device reset builds the targets anew
	fallback_pending = false;
	on = false;
	chosen_preset = 0;
	render_w = Device.dwWidth;
	render_h = Device.dwHeight;
	state = IRenderDeviceRender::upscaler_off;
	u32 method, model, m;
	resolve(method, model, m);
	if (method == method_off)
		return;

	if (!RImplementation.o.ssfx_motionvectors)
	{
		state = IRenderDeviceRender::upscaler_no_motion_vectors;
		return;
	}
	if (RImplementation.o.dx10_msaa)
	{
		state = IRenderDeviceRender::upscaler_msaa;
		return;
	}
	if (!vs && !create_passes())
	{
		Msg("! [upscaler] passes could not be created");
		release_passes();
		state = IRenderDeviceRender::upscaler_failed;
		return;
	}

	if (method == method_dlss)
	{
		ngx_init();
		if (!dlss_supported && ps_r__upscaler == method_auto)
			resolve(method, model, m); // the GeForce has no DLSS: now AUTO takes FSR
		else if (!dlss_supported)
		{
			state = HW.Caps.id_vendor != 0x10DE ? IRenderDeviceRender::upscaler_no_nvidia :
				dlss_old_driver ? IRenderDeviceRender::upscaler_old_driver : IRenderDeviceRender::upscaler_no_dlss;
			return;
		}
	}
	if (method == method_fsr)
	{
		u32 w = Device.dwWidth, h = Device.dwHeight;
		// FSR's quality modes 1..4 are ours; ours 0 is its native AA
		if (m != mode_dlaa)
			ffxFsr3UpscalerGetRenderResolutionFromQualityMode(&w, &h, Device.dwWidth, Device.dwHeight,
				FfxFsr3UpscalerQualityMode(m));
		// Direct3D 11.1: the luma pyramid pass has 64 UAV slots, the backend binds constant buffer ranges
		if (HW.FeatureLevel < D3D_FEATURE_LEVEL_11_1 || !w || !h || w > Device.dwWidth || h > Device.dwHeight ||
			!fsr_create(w, h))
		{
			Msg("! [FSR] context creation failed (Direct3D feature level %x)", u32(HW.FeatureLevel));
			fsr_release();
			state = IRenderDeviceRender::upscaler_failed;
			return;
		}
		render_w = w;
		render_h = h;
		on = true;
		state = IRenderDeviceRender::upscaler_active;
		Msg("* [FSR] %ux%u -> %ux%u, mode %u", w, h, Device.dwWidth, Device.dwHeight, m);
		return;
	}

	const NVSDK_NGX_PerfQuality_Value q = quality(m);
	u32 w = Device.dwWidth, h = Device.dwHeight;
	if (m != mode_dlaa)
	{
		unsigned int max_w, max_h, min_w, min_h;
		float sharpness;
		if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(ngx_params, Device.dwWidth, Device.dwHeight, q, &w, &h, &max_w,
			&max_h, &min_w, &min_h, &sharpness)) || !w || !h || w > Device.dwWidth || h > Device.dwHeight)
		{
			Msg("! [DLSS] no render size for %ux%u, mode %u", Device.dwWidth, Device.dwHeight, m);
			state = IRenderDeviceRender::upscaler_failed;
			return;
		}
	}

	const char* const hints[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance};
	for (u32 k = mode_dlaa; k <= mode_ultra_performance; ++k)
		NVSDK_NGX_Parameter_SetUI(ngx_params, hints[k], preset(model, k));
	chosen_preset = preset(model, m);

	NVSDK_NGX_DLSS_Create_Params create = {};
	create.Feature.InWidth = w;
	create.Feature.InHeight = h;
	create.Feature.InTargetWidth = Device.dwWidth;
	create.Feature.InTargetHeight = Device.dwHeight;
	create.Feature.InPerfQualityValue = q;
	// The motion vectors of SSS are at the render size and without the jitter. HDR10 keeps the scene in FP16.
	create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
		(RImplementation.o.dx11_hdr10 ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
	release_feature();
	CTimer timer;
	timer.Start();
	const NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(HW.pContext, &feature, ngx_params, &create);
	if (NVSDK_NGX_FAILED(result))
	{
		Msg("! [DLSS] feature creation failed: 0x%08X", u32(result));
		feature = nullptr;
		state = IRenderDeviceRender::upscaler_failed;
		return;
	}
	render_w = w;
	render_h = h;
	on = true;
	state = IRenderDeviceRender::upscaler_active;
	Msg("* [DLSS] %ux%u -> %ux%u, mode %u, preset %c, created in %u ms", w, h, Device.dwWidth, Device.dwHeight,
		m, 'A' + int(chosen_preset) - 1, timer.GetElapsed_ms());
}

void create(CRenderTarget& T)
{
	twin_count = 0;
	swapped = false;
	in_scene = false;
	reset_history = true;
	jitter_index = 0;
	if (!on)
		return;
	depth.create("$user$quark_scene_depth", render_w, render_h, D3DFMT_D24S8);
	if (fsr)
	{
		fsr_opaque.create("$user$quark_fsr_opaque", render_w, render_h, T.rt_Generic_0->fmt);
		fsr_reactive.create("$user$quark_fsr_reactive", render_w, render_h, D3DFMT_L8, 1, true);
	}
	SSManager.SetMipLODBias(ps_r__tf_Mipbias + mip_bias());

	// The order of the nearest copies is the order of the targets of the nearest pass
	ref_rt* const copied[nearest_targets] = {&T.rt_Position, &T.rt_Generic_1, &T.rt_ssfx_motion_vectors, &T.rt_Heat,
		&T.rt_ssfx_bloom_emissive};
	ref_rt* const written[] = {&T.rt_Generic_0, &T.rt_Color, &T.rt_Generic_temp, &T.rt_Generic_2};
	for (ref_rt* rt : copied)
		twins[twin_count++] = {rt, ref_rt()};
	for (ref_rt* rt : written)
		twins[twin_count++] = {rt, ref_rt()};
	for (u32 i = 0; i < twin_count; ++i)
	{
		twin& t = twins[i];
		if (!*t.scene || (!scaled() && t.scene != &T.rt_Generic_0))
			continue;
		string256 name;
		xr_sprintf(name, "%s_quark_display", (*t.scene)->cName.c_str());
		// DLSS writes rt_Generic_0's twin as an unordered access view
		t.display.create(name, Device.dwWidth, Device.dwHeight, (*t.scene)->fmt, 1, t.scene == &T.rt_Generic_0);
	}
	swap_twins(true); // outside the scene (main menu) every pass works at the display size
}

void release()
{
	scene_finish();
	swap_twins(false);
	for (twin& t : twins)
		t.display.destroy();
	twin_count = 0;
	depth.destroy();
	dlss_output.destroy();
	fsr_opaque.destroy();
	fsr_reactive.destroy();
	release_feature();
	fsr_release();
	if (on)
		SSManager.SetMipLODBias(ps_r__tf_Mipbias);
	on = false;
	in_scene = false;
}

void shutdown()
{
	release();
	release_passes();
	if (ngx_params)
		NVSDK_NGX_D3D11_DestroyParameters(ngx_params);
	ngx_params = nullptr;
	if (ngx_ready)
		NVSDK_NGX_D3D11_Shutdown1(HW.pDevice);
	ngx_ready = ngx_tried = dlss_supported = false;
}

void scene_begin()
{
	if (!on)
		return;
	swap_twins(false);
	base_zb = HW.pBaseZB;
	HW.pBaseZB = depth->pZRT;
	in_scene = true;

	// A 3D scope's view must neither take a jitter sample nor reach the main view's history
	main_view = !Device.m_SecondViewport.IsSVPFrame();
	jitter_px = jitter_py = 0.f;
	if (main_view && fsr)
	{
		const int phases = ffxFsr3UpscalerGetJitterPhaseCount(render_w, Device.dwWidth);
		ffxFsr3UpscalerGetJitterOffset(&jitter_px, &jitter_py, jitter_index++ % phases, phases);
	}
	else if (main_view)
	{
		// Halton (2, 3), at least 8 phases a pixel of the display (NVIDIA)
		const float ratio = float(Device.dwWidth) / float(render_w);
		const u32 phases = u32(_min(_max(ceilf(8.f * ratio * ratio), 8.f), 128.f));
		const u32 index = jitter_index++ % phases + 1;
		auto halton = [](u32 i, u32 base) {
			float value = 0.f, fraction = 1.f;
			for (; i; i /= base)
			{
				fraction /= float(base);
				value += fraction * float(i % base);
			}
			return value;
		};
		jitter_px = halton(index, 2) - 0.5f;
		jitter_py = halton(index, 3) - 0.5f;
	}
}

void capture_opaque(CRenderTarget& T)
{
	if (in_scene && main_view && fsr)
		HW.pContext->CopyResource(fsr_opaque->pSurface, T.rt_Generic_0->pSurface);
}

void scene_end(CRenderTarget& T, bool history_reset, bool distortion)
{
	if (!in_scene)
		return;
	in_scene = false;
	if (scaled())
		scene_finish();

	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(HW.pContext->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		const float scale[4] = {float(render_w) / float(Device.dwWidth), float(render_h) / float(Device.dwHeight),
			1.f / float(Device.dwWidth), 1.f / float(Device.dwHeight)};
		memcpy(mapped.pData, scale, sizeof(scale));
		HW.pContext->Unmap(cb, 0);
	}

	// The post-process uses its own stencil marks, for example the 3DSS lens and transparent sight frame.
	HW.pContext->ClearDepthStencilView(HW.pBaseZB, D3D11_CLEAR_STENCIL, 1.f, 0);
	if (scaled())
	{
		PIX_EVENT(Upscaler_scene_buffers);
		// In native AA the post-process can use the scene attachments directly.
		ID3D11RenderTargetView* rtvs[nearest_targets] = {};
		ID3D11ShaderResourceView* srvs[nearest_targets + 1] = {};
		const bool copy[nearest_targets] = {true, distortion, true, T.need_heat_buffer(), main_view};
		for (u32 i = 0; i < nearest_targets; ++i)
			if (copy[i] && twins[i].display)
			{
				rtvs[i] = twins[i].display->pRT;
				srvs[i] = (*twins[i].scene)->pTexture->get_SRView();
			}
		srvs[nearest_targets] = depth->pTexture->get_SRView();
		draw(rtvs, nearest_targets, HW.pBaseZB, ps_nearest, srvs, nearest_targets + 1, depth_write);
	}
	else
		HW.pContext->OMSetRenderTargets(0, nullptr, nullptr); // the SDK reads the scene's depth as an SRV

	CRT& color = *T.rt_Generic_0;
	CRT& output = *twins[nearest_targets].display;
	bool upscaled = false;
	if (main_view && feature)
	{
		PIX_EVENT(DLSS_upscale);
		const bool sharpen = ps_r__dlss_sharpness > 0.f;
		if (sharpen && !dlss_output)
			dlss_output.create("$user$quark_dlss_output", Device.dwWidth, Device.dwHeight, color.fmt, 1, true);
		NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
		eval.Feature.pInColor = color.pSurface;
		eval.Feature.pInOutput = sharpen ? dlss_output->pSurface : output.pSurface;
		eval.pInDepth = depth->pSurface;
		eval.pInMotionVectors = T.rt_ssfx_motion_vectors->pSurface;
		eval.InJitterOffsetX = jitter_px;
		eval.InJitterOffsetY = jitter_py;
		eval.InRenderSubrectDimensions = {render_w, render_h};
		eval.InReset = (reset_history || history_reset) ? 1 : 0;
		// SSS writes current minus previous position in UV; DLSS wants the way back in pixels
		eval.InMVScaleX = -float(render_w);
		eval.InMVScaleY = -float(render_h);
		eval.InFrameTimeDeltaInMsec = Device.fTimeDelta * 1000.f;
		const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(HW.pContext, feature, ngx_params, &eval);
		upscaled = NVSDK_NGX_SUCCEED(result);
		if (!upscaled)
		{
			Msg("! [DLSS] evaluation failed: 0x%08X, returning to native rendering", u32(result));
		}
		else if (sharpen)
		{
			// as FSR maps its sharpness: 0..1 is 2..0 stops
			if (SUCCEEDED(HW.pContext->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				const float con[4] = {exp2f(2.f * ps_r__dlss_sharpness - 2.f), float(Device.dwWidth), float(Device.dwHeight), 0.f};
				memcpy(mapped.pData, con, sizeof(con));
				HW.pContext->Unmap(cb, 0);
			}
			ID3D11RenderTargetView* rtv = output.pRT;
			ID3D11ShaderResourceView* srv = dlss_output->pTexture->get_SRView();
			draw(&rtv, 1, nullptr, ps_rcas, &srv, 1, depth_off);
		}
		reset_history = false;
	}
	else if (main_view && fsr)
	{
		upscaled = fsr_dispatch(T, color, output, reset_history || history_reset);
		reset_history = false;
	}
	if (!upscaled)
	{
		if (main_view)
		{
			state = IRenderDeviceRender::upscaler_failed;
			fallback_pending = true;
		}
		ID3D11RenderTargetView* rtv = output.pRT;
		ID3D11ShaderResourceView* srv = color.pTexture->get_SRView();
		draw(&rtv, 1, nullptr, ps_linear, &srv, 1, depth_off);
	}

	swap_twins(true);
	const D3D11_VIEWPORT viewport = {0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight), 0.f, 1.f};
	HW.pContext->RSSetViewports(1, &viewport);
}

void scene_finish()
{
	if (base_zb)
	{
		HW.pBaseZB = base_zb;
		base_zb = nullptr;
	}
}

void jitter(float& x, float& y)
{
	x = in_scene ? 2.f * jitter_px / float(render_w) : 0.f;
	y = in_scene ? -2.f * jitter_py / float(render_h) : 0.f;
}

void report(IRenderDeviceRender::upscaler_report& out)
{
	out.method = ps_r__upscaler;
	resolve(out.used_method, out.model, out.mode);
	out.state = state;
	out.nvidia = HW.Caps.id_vendor == 0x10DE;
	out.dlss_supported = dlss_supported;
	out.old_driver = dlss_old_driver;
	out.motion_vectors = RImplementation.o.ssfx_motionvectors;
	out.msaa = RImplementation.o.dx10_msaa;
	out.new_gpu = new_gpu();
	out.gpu_known = gpu_arch != 0;
	out.render_w = render_w;
	out.render_h = render_h;
	out.display_w = Device.dwWidth;
	out.display_h = Device.dwHeight;
	out.preset = on && feature ? char('A' + int(chosen_preset) - 1) : 0;
	out.sharpness = out.used_method == method_dlss ? ps_r__dlss_sharpness : ps_r__fsr_sharpness;
}
} // namespace upscaler
