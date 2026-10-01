// Engine.cpp: implementation of the CEngine class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Engine.h"
#include "dedicated_server_only.h"
#include "../xrCore/job_system.h"

CEngine Engine;
xrDispatchTable PSGP;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CEngine::CEngine()
{
}

CEngine::~CEngine()
{
}

extern void msCreate(LPCSTR name);
extern void msDestroy();

extern "C" void __cdecl xrBind_PSGP(xrDispatchTable* T, _processor_info* ID);

PROTECT_API void CEngine::Initialize(void)
{
	// The shared worker pool is engine-owned and available to all subsystems.
	xr_jobs::initialize();

	// Bind PSGP
	//hPSGP = LoadLibrary("xrCPU_Pipe.dll");
	hPSGP = 0;
	//R_ASSERT(hPSGP);
	//xrBinder* bindCPU = (xrBinder*)GetProcAddress(hPSGP, "xrBind_PSGP");
	xrBinder* bindCPU = xrBind_PSGP;
	R_ASSERT(bindCPU);
	bindCPU(&PSGP, &CPU::ID);

	// Other stuff
	Engine.Sheduler.Initialize();
	//
#ifdef DEBUG
    msCreate("game");
#endif
}

typedef void __cdecl ttapi_Done_func(void);

extern "C" void __cdecl ttapi_Done();

void CEngine::Destroy()
{
#ifdef DEBUG
    msDestroy();
#endif

	// A frame may have queued the non-RT scheduler before shutdown or level exit.
	// Drain it while the worker pool and all scheduled objects are still alive.
	Device.WaitFrameParallelWork();
	Engine.Sheduler.Destroy();
#ifdef DEBUG_MEMORY_MANAGER
    extern void dbg_dump_leaks_prepare();
    if (Memory.debug_mode) dbg_dump_leaks_prepare();
#endif // DEBUG_MEMORY_MANAGER
	Engine.External.Destroy();

	//if (hPSGP)
	{
		//ttapi_Done_func* ttapi_Done = (ttapi_Done_func*)GetProcAddress(hPSGP, "ttapi_Done");
		ttapi_Done_func* ttapiDone = ttapi_Done;
		R_ASSERT(ttapiDone);
		if (ttapiDone)
			ttapiDone();

		//FreeLibrary(hPSGP);
		hPSGP = 0;
		ZeroMemory(&PSGP, sizeof(PSGP));
	}

	xr_jobs::shutdown();
}
