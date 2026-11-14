// Upscaling-only dispatcher for the vendored AMD SDK/DX11 port.
// Do not link frame interpolation, optical flow or unused FSR1/2 effects.
#include "src/backends/shared/ffx_shader_blobs.h"
#include "src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.h"

FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage,
    uint32_t options, FfxShaderBlob* blob)
{
    if (effect != FFX_EFFECT_FSR3UPSCALER) return FFX_ERROR_INVALID_ARGUMENT;
    return fsr3UpscalerGetPermutationBlobByIndex(static_cast<FfxFsr3UpscalerPass>(pass), options, blob);
}
FfxErrorCode ffxIsWave64(FfxEffect effect, uint32_t options, bool& wave64)
{
    wave64 = false;
    if (effect != FFX_EFFECT_FSR3UPSCALER) return FFX_ERROR_INVALID_ARGUMENT;
    return fsr3UpscalerIsWave64(options, wave64);
}
