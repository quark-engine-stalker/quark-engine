#pragma once

// QUARK upscalers (r__upscaler, r__upscaler_mode, r__dlss_model, r__dlss_sharpness, r__fsr_sharpness, set by
// QUARK-PANEL). The scene
// (G-buffer, lights, transparent) renders at the render size; at the end of the scene NVIDIA DLSS (super resolution) or
// AMD FSR 3.1 brings it to the display size and the post-process runs there as before. Main thread only.
class CRenderTarget;

namespace upscaler
{
enum method : u32 { method_off, method_dlss, method_fsr };
enum mode : u32 { mode_dlaa, mode_quality, mode_balanced, mode_performance, mode_ultra_performance };
enum model : u32 { model_auto, model_dlss45, model_dlss4, model_dlss3 };

// The size of the pass being drawn: the render size inside the scene, the display size elsewhere
u32 width();
u32 height();
// The size the scene renders at (the display size when the upscaler is off)
u32 render_width();
u32 render_height();
// The targets of this session render the scene at its own size
bool enabled();
// Added to r__tf_mipbias while enabled: textures keep the sharpness of the display size
float mip_bias();

// A console change of the upscaler: applied by apply() at the next frame without a device reset
void request();
void apply(CRenderTarget& T); // CRender::Render, before anything is drawn
void configure(); // CRenderTarget constructor, before any target: picks the render size, creates the DLSS feature
void create(CRenderTarget& T); // CRenderTarget constructor, after the targets
void release(); // CRenderTarget destructor
void shutdown(); // the device is destroyed

void scene_begin(); // CRender::Render: the scene's targets, depth and jitter
void scene_end(CRenderTarget& T, bool history_reset); // phase_combine before the post-process: upscale, display targets

// ssfx_jitter: the sub-pixel offset of the projection in clip space, zero outside the main view's scene
void jitter(float& x, float& y);

void report(IRenderDeviceRender::upscaler_report& out);
}
