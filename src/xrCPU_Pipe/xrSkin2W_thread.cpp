#include "stdafx.h"
#pragma hdrstop

extern xrSkin4W* skin4W_func;

struct SKIN_PARAMS
{
	LPVOID Dest;
	LPVOID Src;
	u32 Count;
	LPVOID Data;
};

void Skin4W_Stream(LPVOID lpvParams)
{
#ifdef _GPA_ENABLED
		TAL_SCOPED_TASK_NAMED( "xrSkin4W_Stream()" );
#endif // _GPA_ENABLED

	SKIN_PARAMS* sp = (SKIN_PARAMS*)lpvParams;

	vertRender* D = (vertRender*)sp->Dest;
	vertBoned4W* S = (vertBoned4W*)sp->Src;
	u32 vCount = sp->Count;
	CBoneInstance* Bones = (CBoneInstance*)sp->Data;

	skin4W_func(D, S, vCount, Bones);
}

void __stdcall xrSkin4W_thread(vertRender* D,
                               vertBoned4W* S,
                               u32 vCount,
                               CBoneInstance* Bones)
{
#ifdef _GPA_ENABLED
        TAL_SCOPED_TASK_NAMED("xrSkin4W()");
#endif // _GPA_ENABLED

    constexpr u32 kMinParallelVertices = 8192;
    constexpr u32 kTargetVerticesPerWorker = 4096;

    const u32 availableWorkers = ttapi_GetWorkersCount();
    if (availableWorkers < 2 || vCount < kMinParallelVertices)
    {
        skin4W_func(D, S, vCount, Bones);
        return;
    }

    // AVX2 substantially reduces work per vertex. Keep each job large enough to
    // amortize task submission, synchronization and the per-worker store fence.
    u32 nWorkers = vCount / kTargetVerticesPerWorker;
    if (nWorkers < 2)
        nWorkers = 2;
    if (nWorkers > availableWorkers)
        nWorkers = availableWorkers;

    SKIN_PARAMS* sknParams = static_cast<SKIN_PARAMS*>(_alloca(sizeof(SKIN_PARAMS) * nWorkers));

    // Keep slices reasonably even and on an 8-vertex boundary. vertRender is 32 bytes,
    // therefore every slice remains 32-byte aligned for AVX2 streaming stores.
    u32 nStep = (vCount / nWorkers) & ~7u;
    if (nStep == 0)
    {
        skin4W_func(D, S, vCount, Bones);
        return;
    }

    for (u32 i = 0; i < nWorkers; ++i)
    {
        const u32 offset = i * nStep;
        sknParams[i].Dest = static_cast<LPVOID>(D + offset);
        sknParams[i].Src = static_cast<LPVOID>(S + offset);
        sknParams[i].Count = (i == nWorkers - 1) ? (vCount - offset) : nStep;
        sknParams[i].Data = static_cast<LPVOID>(Bones);

        ttapi_AddWorker(Skin4W_Stream, static_cast<LPVOID>(&sknParams[i]));
    }

    ttapi_RunAllWorkers();
}
