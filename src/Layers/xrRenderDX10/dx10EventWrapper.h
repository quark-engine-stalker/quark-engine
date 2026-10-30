#pragma once

#include "../../xrCore/profiler.h"

// Render phases marked for GPU debuggers are Tracy zones as well in profiling builds: a CPU
// zone, and a GPU zone timed by D3D11 timestamp queries (render thread only, immediate context).
#ifdef TRACY_ENABLE
#include "../../3rd party/tracy/public/tracy/TracyD3D11.hpp"
extern TracyD3D11Ctx g_tracy_d3d11; // dx10HW.cpp, lives as long as the device
#define PIX_EVENT(Name) dxPixEventWrapper pixEvent##Name(L#Name); ZoneNamedN(pixZone##Name, #Name, true); \
	TracyD3D11NamedZone(g_tracy_d3d11, gpuZone##Name, #Name, g_tracy_d3d11 != nullptr)
#else
#define PIX_EVENT(Name) dxPixEventWrapper pixEvent##Name(L#Name)
#endif

class dxPixEventWrapper
{
public:
    dxPixEventWrapper(LPCWSTR wszName);
    ~dxPixEventWrapper();
};
