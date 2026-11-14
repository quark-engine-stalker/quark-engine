FSR 3.1.2 upscaler / community D3D11 backend
==========================================
Vendored from the locally tested OGSR dependency:
3rd_party/Src/FidelityFX-SDK/FidelityFX-SDK (OGSR checkout 8adb7cf1650120987d84fcc8be7e73ad8a6f9945).
The SDK directory is an extracted dependency, not a Git submodule; the OGSR
revision identifies the consuming integration, NOT an upstream SDK commit.
The API header identifies the upscaler as 3.1.2; the old top-level SDK readme
is stale. This is not AMD's officially supported DX12/Vulkan backend.

Preserved AMD MIT license and third-party notices. Source headers, DX11 backend,
FSR3 upscaler component and precompiled DXBC permutation headers are included.
No downloads, external OGSR paths or shader compiler are needed to build.
fsr3_dx11.props compiles only the upscaler/backend. upscaler_blobs.cpp selects
only FSR3 upscaling, excluding frame generation, optical flow and FSR1/2.

Monolith corrections to the imported backend:
- Explicit mmsystem.h include for MAKEFOURCC (standalone compilation).
- Replace invalid persistent constant-buffer mapping with per-dispatch
  WRITE_DISCARD upload/unmap, copying all constant ranges before binding.
- Remove the corresponding persistent Unmap from destruction.
- Preserve failed HRESULTs at the wrapper exception boundary, restore context
  state with RAII, and permit cleanup of partial backend creation.
The wrapper also supplies the point/linear clamp samplers that the community
backend leaves unimplemented, and isolates compute calls in a D3D11 context state.
