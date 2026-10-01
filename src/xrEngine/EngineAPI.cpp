// EngineAPI.cpp: implementation of the CEngineAPI class.

#include "stdafx.h"
#include "EngineAPI.h"
#include "../xrcdb/xrXRC.h"

extern xr_token* vid_quality_token;

void __cdecl dummy(void)
{
}

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "vfw32.lib")
#pragma comment(lib, "nvapi.lib")

#if !defined(STATIC_RENDERER_R4)
#error QUARK ENGINE supports only the DirectX 11 (R4) renderer
#endif

#pragma comment(lib, "xrRender_R4.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dx11.lib")
#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern BOOL DllMainXrRenderR4(HANDLE hModule, DWORD reason, LPVOID reserved);
extern BOOL DllMainXrGame(HANDLE hModule, u32 reason, LPVOID reserved);

extern "C" DLL_Pure* __cdecl xrFactory_Create(CLASS_ID clsid);
extern "C" void __cdecl xrFactory_Destroy(DLL_Pure* object);

extern u32 renderer_value; // console renderer token

ENGINE_API int g_current_renderer = 4;

namespace
{
bool renderer_attached = false;

void AttachRenderer()
{
	if (renderer_attached)
		return;

	Log("Initializing built-in DirectX 11 renderer:", "xrRender_R4");
	R_ASSERT(DllMainXrRenderR4(NULL, DLL_PROCESS_ATTACH, NULL));
	renderer_attached = true;
}
} // namespace

CEngineAPI::CEngineAPI()
{
	hTuner = 0;
	pCreate = 0;
	pDestroy = 0;
	tune_pause = dummy;
	tune_resume = dummy;
}

CEngineAPI::~CEngineAPI()
{
	if (vid_quality_token)
	{
		xr_free(vid_quality_token);
		vid_quality_token = NULL;
	}
}

ENGINE_API bool is_enough_address_space_available()
{
	return true; // QUARK ENGINE is 64-bit only.
}

void CEngineAPI::Initialize(void)
{
	psDeviceFlags.set(rsR2, FALSE);
	psDeviceFlags.set(rsR3, FALSE);
	psDeviceFlags.set(rsR4, TRUE);
	renderer_value = 0;
	g_current_renderer = 4;

	AttachRenderer();
	Device.ConnectToRender();

	// The engine and game are linked into one executable.
	{
		Log("Initializing built-in game module:", "xrGame");
		DllMainXrGame(NULL, DLL_PROCESS_ATTACH, NULL);
		pCreate = xrFactory_Create;
		R_ASSERT(pCreate);
		pDestroy = xrFactory_Destroy;
		R_ASSERT(pDestroy);
	}

	tune_enabled = FALSE;
	if (strstr(Core.Params, "-tune"))
	{
		LPCSTR tuner_name = "vTuneAPI.dll";
		Log("Loading DLL:", tuner_name);
		hTuner = LoadLibrary(tuner_name);
		if (0 == hTuner)
			R_CHK(GetLastError());
		R_ASSERT2(hTuner, "Intel vTune is not installed");
		tune_enabled = TRUE;
		tune_pause = (VTPause*)GetProcAddress(hTuner, "VTPause");
		R_ASSERT(tune_pause);
		tune_resume = (VTResume*)GetProcAddress(hTuner, "VTResume");
		R_ASSERT(tune_resume);
	}
}

void CEngineAPI::Destroy(void)
{
	DllMainXrGame(NULL, DLL_PROCESS_DETACH, NULL);

	if (renderer_attached)
	{
		DllMainXrRenderR4(NULL, DLL_PROCESS_DETACH, NULL);
		renderer_attached = false;
	}

	pCreate = 0;
	pDestroy = 0;

	if (hTuner)
	{
		FreeLibrary(hTuner);
		hTuner = 0;
		tune_enabled = FALSE;
		tune_pause = dummy;
		tune_resume = dummy;
	}

	Engine.Event._destroy();
	XRC.r_clear_compact();
}

void CEngineAPI::CreateRendererList()
{
	if (vid_quality_token != NULL)
		return;

#ifndef DEDICATED_SERVER
	// Device.Create performs the authoritative D3D11 initialization. Avoid
	// creating and destroying a second test window, device and swap chain.
	AttachRenderer();
#endif

	vid_quality_token = xr_alloc<xr_token>(2);
	vid_quality_token[0].id = 0;
	vid_quality_token[0].name = "renderer_r4"; // GAMMA config/UI compatibility contract.
	vid_quality_token[1].id = -1;
	vid_quality_token[1].name = NULL;

	Msg("Available render modes[1]:");
	Msg("[renderer_r4]");
}
