#include "stdafx.h"
#pragma hdrstop

//BOOL WINAPI DllMain( HINSTANCE hinstDLL , DWORD fdwReason , LPVOID lpvReserved )
BOOL DllMainIgnore2(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	return TRUE;
}

extern xrSkin1W xrSkin1W_x86;
extern xrSkin2W xrSkin2W_x86;
extern xrSkin3W xrSkin3W_x86;
extern xrSkin4W xrSkin4W_x86;

extern xrSkin1W xrSkin1W_AVX;
extern xrSkin2W xrSkin2W_AVX;
extern xrSkin3W xrSkin3W_AVX;
extern xrSkin4W xrSkin4W_AVX;

extern xrSkin4W xrSkin4W_thread;

xrSkin4W* skin4W_func = NULL;

extern "C" {
//__declspec(dllexport)
void __cdecl xrBind_PSGP(xrDispatchTable* T, _processor_info* ID)
{
	// This target is built specifically for AVX-capable x64 CPUs.
	// Keep scalar routines linked as a diagnostic fallback, but bind the hot skinning path to AVX.
	T->skin1W = xrSkin1W_AVX;
	T->skin2W = xrSkin2W_AVX;
	T->skin3W = xrSkin3W_AVX;
	T->skin4W = xrSkin4W_AVX;
	skin4W_func = xrSkin4W_AVX;


	// Init helper threads
	ttapi_Init(ID);

	if (ttapi_GetWorkersCount() > 1)
	{
		// We can use threading
		T->skin4W = xrSkin4W_thread;
	}
}
};
