#include "stdafx.h"
#include "dxRenderDeviceRender.h"

#include "ResourceManager.h"

dxRenderDeviceRender::dxRenderDeviceRender()
	: Resources(0), m_lastPresentResult(S_OK)
{}

void dxRenderDeviceRender::Copy(IRenderDeviceRender& _in)
{
	*this = *(dxRenderDeviceRender*)&_in;
}

void dxRenderDeviceRender::setGamma(float fGamma)
{
	m_Gamma.Gamma(fGamma);
}

void dxRenderDeviceRender::setBrightness(float fGamma)
{
	m_Gamma.Brightness(fGamma);
}

void dxRenderDeviceRender::setContrast(float fGamma)
{
	m_Gamma.Contrast(fGamma);
}

void dxRenderDeviceRender::updateGamma()
{
	m_Gamma.Update();
}

void dxRenderDeviceRender::OnDeviceDestroy(BOOL bKeepTextures)
{
	m_WireShader.destroy();
	m_SelectionShader.destroy();

	Resources->OnDeviceDestroy(bKeepTextures);
	RCache.OnDeviceDestroy();
}

void dxRenderDeviceRender::ValidateHW()
{
	HW.Validate();
}

void dxRenderDeviceRender::DestroyHW()
{
	xr_delete(Resources);
	HW.DestroyDevice();
}

void dxRenderDeviceRender::Reset(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2)
{
#ifdef DEBUG
    _SHOW_REF("*ref -CRenderDevice::ResetTotal: DeviceREF:",HW.pDevice);
#endif // DEBUG

	Resources->reset_begin();
	Memory.mem_compact();

	HW.Reset(hWnd);
	m_lastPresentResult = S_OK;

	dwWidth = HW.m_ChainDesc.Width;
	dwHeight = HW.m_ChainDesc.Height;

	fWidth_2 = float(dwWidth / 2);
	fHeight_2 = float(dwHeight / 2);
	Resources->reset_end();

#ifdef DEBUG
    _SHOW_REF("*ref +CRenderDevice::ResetTotal: DeviceREF:",HW.pDevice);
#endif // DEBUG
}

void dxRenderDeviceRender::SetupStates()
{
	HW.Caps.Update();

	//	TODO: DX10: Implement Resetting of render states into default mode
	//VERIFY(!"dxRenderDeviceRender::SetupStates not implemented.");
	SSManager.SetMaxAnisotropy(ps_r__tf_Anisotropic);
	SSManager.SetMipLODBias(ps_r__tf_Mipbias);
}

void dxRenderDeviceRender::OnDeviceCreate(LPCSTR shName)
{
	// Signal everyone - device created
	RCache.OnDeviceCreate();
	m_Gamma.Update();
	Resources->OnDeviceCreate(shName);
	::Render->create();
	Device.Statistic->OnDeviceCreate();

	//#ifndef DEDICATED_SERVER
	if (!g_dedicated_server)
	{
		m_WireShader.create("editor\\wire");
		m_SelectionShader.create("editor\\selection");

		DUImpl.OnDeviceCreate();
	}
	//#endif
}

void dxRenderDeviceRender::Create(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2,
                                  bool move_window)
{
	HW.CreateDevice(hWnd, move_window);
	m_lastPresentResult = S_OK;
	dwWidth = HW.m_ChainDesc.Width;
	dwHeight = HW.m_ChainDesc.Height;
	fWidth_2 = float(dwWidth / 2);
	fHeight_2 = float(dwHeight / 2);
	Resources = xr_new<CResourceManager>();
}

void dxRenderDeviceRender::SetupGPU(BOOL bForceGPU_SW, BOOL bForceGPU_NonPure, BOOL bForceGPU_REF)
{
	HW.Caps.bForceGPU_SW = bForceGPU_SW;
	HW.Caps.bForceGPU_NonPure = bForceGPU_NonPure;
	HW.Caps.bForceGPU_REF = bForceGPU_REF;
}

void dxRenderDeviceRender::overdrawBegin()
{
	//	TODO: DX10: Implement overdrawBegin
	VERIFY(!"dxRenderDeviceRender::overdrawBegin not implemented.");
}

void dxRenderDeviceRender::overdrawEnd()
{
	//	TODO: DX10: Implement overdrawEnd
	VERIFY(!"dxRenderDeviceRender::overdrawBegin not implemented.");
}

void dxRenderDeviceRender::DeferredLoad(BOOL E)
{
	Resources->DeferredLoad(E);
}

void dxRenderDeviceRender::ResourcesDeferredUpload()
{
	Resources->DeferredUpload();
}

void dxRenderDeviceRender::ResourcesDeferredUnload()
{
	Resources->DeferredUnload();
}

void dxRenderDeviceRender::ResourcesPrefetchCreateTexture(LPCSTR name)
{
	Resources->_CreateTexture(name);
}

void dxRenderDeviceRender::ResourcesPrefetchCreateModelTexture(LPCSTR name)
{
	if (!name || !name[0] || !xr_strcmp(name, "null") || strstr(name, "$user$"))
		return;

	// Worker-side D3D resource creation is enabled only when the driver reports
	// DriverConcurrentCreates. Otherwise speculative creation can serialize the
	// immediate context in the driver and create exactly the hitch we avoid.
	if (!RImplementation.driver_concurrent_creates())
		return;

	string_path resolved;
	if (FS.exist(resolved, "$game_textures$", name, ".ogm") ||
		FS.exist(resolved, "$game_textures$", name, ".avi") ||
		FS.exist(resolved, "$game_textures$", name, ".seq") ||
		FS.exist(resolved, "$game_textures$", name, ".gif"))
		return;
	if (!FS.exist(resolved, "$level$", name, ".dds") &&
		!FS.exist(resolved, "$game_saves$", name, ".dds") &&
		!FS.exist(resolved, "$game_textures$", name, ".dds"))
		return;

	if (!RImplementation.acquire_worker_resource_creation_lane())
		return;
	struct lane_release
	{
		~lane_release() { RImplementation.release_worker_resource_creation_lane(); }
	} release_lane;

	// prefer_gpu_resident bypasses the legacy STAGING -> immediate-context
	// CopyResource first-use path. ID3D11Device creation/SRV methods are
	// free-threaded; no ID3D11DeviceContext method is issued on this worker.
	const shared_str base_name = name;
	Resources->PrefetchTextureGpuResident(base_name.c_str());
	const shared_str bump = Resources->m_textures_description.GetBumpName(base_name);
	if (!bump.size())
		return;

	if (FS.exist(resolved, "$game_textures$", bump.c_str(), ".dds"))
		Resources->PrefetchTextureGpuResident(bump.c_str());
	string_path bump_x;
	xr_strcpy(bump_x, bump.c_str());
	if (xr_strlen(bump_x) + 1 < sizeof(bump_x))
	{
		xr_strcat(bump_x, sizeof(bump_x), "#");
		if (FS.exist(resolved, "$game_textures$", bump_x, ".dds"))
			Resources->PrefetchTextureGpuResident(bump_x);
	}
}

void dxRenderDeviceRender::ResourcesPinTexture(LPCSTR name)
{
	Resources->PinTexture(name);
}

void dxRenderDeviceRender::ResourcesGetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps)
{
	if (Resources)
		Resources->_GetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);
}

void dxRenderDeviceRender::ResourcesStoreNecessaryTextures()
{
	dxRenderDeviceRender::Instance().Resources->StoreNecessaryTextures();
}

void dxRenderDeviceRender::ResourcesDumpMemoryUsage()
{
	dxRenderDeviceRender::Instance().Resources->_DumpMemoryUsage();
}

dxRenderDeviceRender::DeviceState dxRenderDeviceRender::GetDeviceState()
{
	HW.Validate();
	bool fullscreen_restored = false;
	if (!HW.TryRestoreFullscreen(fullscreen_restored))
		return dsLost;

	if (fullscreen_restored)
		m_lastPresentResult = S_OK;

	if (m_lastPresentResult == DXGI_STATUS_MODE_CHANGED)
		return dsNeedReset;

	// A successful HRESULT can still be a DXGI status. A real Present moves us
	// into the idle/occluded state; only then use Present(TEST) to determine
	// when rendering may resume. This keeps the normal frame path submission-free.
	if (m_lastPresentResult != S_OK && SUCCEEDED(m_lastPresentResult))
	{
		m_lastPresentResult = HW.m_pSwapChain->Present(0, DXGI_PRESENT_TEST);
		if (m_lastPresentResult != S_OK && SUCCEEDED(m_lastPresentResult))
			return dsLost;
	}

	// Present(TEST) is intended for probing an application that is already idle or
	// occluded. Issuing it before every real Present adds a second DXGI submission
	// to the critical render path and can itself synchronize with the compositor.
	// The real Present result from the previous frame carries the same device-loss
	// information without doing extra work.
	if (FAILED(m_lastPresentResult))
	{
		HRESULT reason = HW.pDevice ? HW.pDevice->GetDeviceRemovedReason() : m_lastPresentResult;
		if (SUCCEEDED(reason))
			reason = m_lastPresentResult;

		if (reason == DXGI_ERROR_DEVICE_RESET)
			return dsNeedReset;

		return dsLost;
	}

	return dsOK;
}

BOOL dxRenderDeviceRender::GetForceGPU_REF()
{
	return HW.Caps.bForceGPU_REF;
}

u32 dxRenderDeviceRender::GetCacheStatPolys()
{
	return RCache.stat.polys;
}

void dxRenderDeviceRender::Begin()
{
	RCache.OnFrameBegin();
	RCache.set_CullMode(CULL_CW);
	RCache.set_CullMode(CULL_CCW);
	if (HW.Caps.SceneMode) overdrawBegin();
}

void dxRenderDeviceRender::Clear()
{
	HW.pContext->ClearDepthStencilView(RCache.get_ZB(),
	                                   D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);

	if (psDeviceFlags.test(rsClearBB))
	{
		FLOAT ColorRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		HW.pContext->ClearRenderTargetView(RCache.get_RT(), ColorRGBA);
	}
}

void DoAsyncScreenshot();

void dxRenderDeviceRender::End()
{
	VERIFY(HW.pDevice);

	if (HW.Caps.SceneMode) overdrawEnd();

	{

		RCache.OnFrameEnd();
	}

	Memory.dbg_check();

	{

		DoAsyncScreenshot();
	}

    UINT present_flags = 0;
	bool use_vsync = !!psDeviceFlags.test(rsVSync);
	UINT present_interval = (use_vsync) ? 1 : 0;

	// NOTE: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
    BOOL is_windowed = HW.m_ChainDescFullscreen.Windowed;
	if (is_windowed && !use_vsync && HW.m_SupportsVRR) {
        present_flags |= DXGI_PRESENT_ALLOW_TEARING;
	}

	const bool presented = !Device.m_SecondViewport.IsSVPFrame() && !Device.m_SecondViewport.isCamReady;
	if (presented) {

		{

			m_lastPresentResult = HW.m_pSwapChain->Present(present_interval, present_flags);
		}

	}

	//HRESULT _hr		= HW.pDevice->Present( NULL, NULL, NULL, NULL );
	//if				(D3DERR_DEVICELOST==_hr)	return;			// we will handle this later
}

void dxRenderDeviceRender::ResourcesDestroyNecessaryTextures()
{
	Resources->DestroyNecessaryTextures();
}

void dxRenderDeviceRender::ClearTarget()
{
	FLOAT ColorRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	HW.pContext->ClearRenderTargetView(RCache.get_RT(), ColorRGBA);
}

void dxRenderDeviceRender::SetCacheXform(Fmatrix& mView, Fmatrix& mProject)
{
	RCache.set_xform_view(mView);
	RCache.set_xform_project(mProject);
}

void dxRenderDeviceRender::SetCacheXform_prev(Fmatrix& mView, Fmatrix& mProject)
{
	RCache.set_xform_view_prev(mView);
	RCache.set_xform_project_prev(mProject);
}

bool dxRenderDeviceRender::HWSupportsShaderYUV2RGB()
{
	u32 v_dev = CAP_VERSION(HW.Caps.raster_major, HW.Caps.raster_minor);
	u32 v_need = CAP_VERSION(2, 0);
	return (v_dev >= v_need);
}

void dxRenderDeviceRender::OnAssetsChanged()
{
	Resources->m_textures_description.UnLoad();
	Resources->m_textures_description.Load();
}

extern ENGINE_API void SetStartupMonitor(HMONITOR h);
extern XRAPI_API xr_token* vid_mode_token;
void fill_vid_mode_list(CHW* _hw);
void free_vid_mode_list();

// Windowed: keep user's pick if present in vid_mode_token, else monitor native.
// Borderless/fullscreen: always monitor native.  Moves the window to match.
// Caller must have set HW.m_pOutput / vid_mode_token to reflect the target
// monitor before calling.
static void FinalizeMonitorGeometry(const MONITORINFO& mi, HWND hWnd,
                                    u32 g_screenmode_,
                                    u32& vidModeW, u32& vidModeH)
{
    const int monX = mi.rcMonitor.left;
    const int monY = mi.rcMonitor.top;
    const int monW = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    u32 finalW = (u32)monW;
    u32 finalH = (u32)monH;
    if (g_screenmode_ == 0)
    {
        string32 cur_buf;
        xr_sprintf(cur_buf, sizeof(cur_buf), "%ux%u", vidModeW, vidModeH);
        for (xr_token* t = vid_mode_token; t && t->name; ++t)
        {
            if (!xr_strcmp(t->name, cur_buf))
            {
                finalW = vidModeW;
                finalH = vidModeH;
                break;
            }
        }
    }
    vidModeW = finalW;
    vidModeH = finalH;

    int wx, wy, ww = (int)finalW, wh = (int)finalH;
    if (g_screenmode_ == 0)
    {
        wx = monX + (monW - ww) / 2;
        wy = monY + (monH - wh) / 2;
    }
    else
    {
        wx = monX;
        wy = monY;
    }
    SetWindowPos(hWnd, HWND_TOP, wx, wy, ww, wh,
                 SWP_FRAMECHANGED | SWP_NOCOPYBITS | SWP_DRAWFRAME);
}

bool dxRenderDeviceRender::SwitchOutputMonitor(HMONITOR hTargetMon, HWND hWnd,
                                               u32 g_screenmode_,
                                               u32& vidModeW, u32& vidModeH)
{
    if (hTargetMon == NULL)
        return false;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hTargetMon, &mi))
    {
        Msg("! vid_monitor: GetMonitorInfoA failed for target monitor");
        return false;
    }

    IDXGIOutput* new_output = HW.FindOutputOnCurrentAdapter(hTargetMon);
    if (!new_output)
    {
        Msg("! vid_monitor: target monitor is not on the current adapter, restart to apply");
        return false;
    }

    SetStartupMonitor(hTargetMon);

    if (g_screenmode_ == 2)
        HW.m_pSwapChain->SetFullscreenState(FALSE, NULL); // best-effort

    _RELEASE(HW.m_pOutput);
    HW.m_pOutput = new_output;

    free_vid_mode_list();
    fill_vid_mode_list(&HW);

    FinalizeMonitorGeometry(mi, hWnd, g_screenmode_, vidModeW, vidModeH);
    Msg("* vid_monitor: output swapped, final mode %ux%u", vidModeW, vidModeH);
    return true;

}
