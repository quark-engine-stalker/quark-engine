#ifndef RenderDeviceRender_included
#define RenderDeviceRender_included
#pragma once

class IRenderDeviceRender
{
public:
	enum DeviceState
	{
		dsOK = 0,
		dsLost,
		dsNeedReset
	};

public:
	virtual ~IRenderDeviceRender() { ; }
	virtual void Copy(IRenderDeviceRender& _in) = 0;

	//	Gamma correction functions
	virtual void setGamma(float fGamma) = 0;
	virtual void setBrightness(float fGamma) = 0;
	virtual void setContrast(float fGamma) = 0;
	virtual void updateGamma() = 0;

	//	Destroy
	virtual void OnDeviceDestroy(BOOL bKeepTextures) = 0;
	virtual void ValidateHW() = 0;
	virtual void DestroyHW() = 0;
	virtual void Reset(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2) = 0;
	//	Init
	virtual void SetupStates() = 0;
	virtual void OnDeviceCreate(LPCSTR shName) = 0;
	virtual void Create(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2, bool) = 0;
	virtual void SetupGPU(BOOL bForceGPU_SW, BOOL bForceGPU_NonPure, BOOL bForceGPU_REF) = 0;
	//	Overdraw
	virtual void overdrawBegin() = 0;
	virtual void overdrawEnd() = 0;

	//	Resources control
	virtual void DeferredLoad(BOOL E) = 0;
	virtual void ResourcesDeferredUpload() = 0;
	virtual void ResourcesDeferredUnload() = 0;
	virtual void ResourcesGetMemoryUsage(u64& m_base, u32& c_base, u64& m_lmaps, u32& c_lmaps) = 0;
	virtual void ResourcesDestroyNecessaryTextures() = 0;
	virtual void ResourcesStoreNecessaryTextures() = 0;
	virtual void ResourcesDumpMemoryUsage() = 0;
	virtual void ResourcesPrefetchCreateTexture(LPCSTR name) = 0;
	virtual void ResourcesPrefetchCreateModelTexture(LPCSTR name) = 0;
	virtual void ResourcesPinTexture(LPCSTR name) = 0;

	//	HWSupport
	virtual bool HWSupportsShaderYUV2RGB() = 0;

	//	Device state
	virtual DeviceState GetDeviceState() = 0;
	virtual BOOL GetForceGPU_REF() = 0;
	virtual u32 GetCacheStatPolys() = 0;
	virtual void Begin() = 0;
	virtual void Clear() = 0;
	virtual void End() = 0;
	enum class latency_frame_limit { late, before_input, driver };
	virtual latency_frame_limit LatencyFrameBegin(int fps_limit) = 0;
	virtual void LatencyRenderBegin() = 0;
	// r__low_latency for QUARK-PANEL, per mode off, on, reflex, reflex_boost; 0 where a mode is not measured yet.
	struct latency_report
	{
		u32 mode;
		bool reflex_missing; // Reflex is chosen but the driver refused it: the mode works as off
		bool cannot_measure;
		float ms[4], fps[4];
		float wait_ms; // the low latency wait before the input (the engine's or the Reflex sleep), a frame, the last second
	};
	// measure keeps the latency measured; only in a level, the menu's frame limit falsifies it
	virtual void LatencyReport(latency_report& report, bool measure) = 0;
	// r__upscaler for QUARK-PANEL
	enum upscaler_state : u32
	{
		upscaler_off, upscaler_active, upscaler_no_nvidia, upscaler_old_driver, upscaler_no_dlss,
		upscaler_no_motion_vectors, upscaler_msaa, upscaler_failed
	};
	struct upscaler_report
	{
		u32 method; // the console value, upscaler::method
		u32 used_method, mode, model; // in force: AUTO resolved to DLSS or FSR at quality
		u32 state; // of the targets in use, upscaler_state
		bool nvidia, dlss_supported, old_driver, motion_vectors, msaa;
		bool new_gpu, gpu_known; // RTX 40 or newer (FP8): AUTO takes DLSS 4.5, older RTX DLSS 4
		u32 render_w, render_h, display_w, display_h;
		char preset; // DLSS preset in use, 0 otherwise
		float sharpness; // r__dlss_sharpness or r__fsr_sharpness, of the chosen method
	};
	virtual void UpscalerReport(upscaler_report& report) = 0;
	virtual void ClearTarget() = 0;
	virtual void SetCacheXform(Fmatrix& mView, Fmatrix& mProject) = 0;
	virtual void SetCacheXform_prev(Fmatrix& mView, Fmatrix& mProject) = 0;
	virtual void OnAssetsChanged() = 0;

	virtual bool SwitchOutputMonitor(HMONITOR hTargetMon, HWND hWnd, u32 g_screenmode, u32& vidModeW, u32& vidModeH) { return false; }
};

#endif	//	RenderDeviceRender_included
