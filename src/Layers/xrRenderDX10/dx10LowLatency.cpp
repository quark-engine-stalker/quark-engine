#include "stdafx.h"
#pragma hdrstop

#include "dx10LowLatency.h"
#include "../xrRender/HW.h"
#include "../xrRender/xrRender_console.h"

#include <nvapi.h>
#include <d3d11_4.h>
#include <atomic>

// By default DXGI queues 3 frames, so at the GPU limit input waits for all of them: 47 ms on the 1080p stand.
// on:     a queue of 1 frame and a wait before input that leaves Present blocking for 1 ms. 17 ms, same FPS, any GPU.
// reflex: the same idea in the NVIDIA driver. 16.5 ms, but the driver leaves the GPU idle ~1.2 ms a frame: -10 % FPS.
namespace
{
enum { mode_off, mode_on, mode_reflex, mode_reflex_boost };

constexpr double present_block_target_us = 1000.0;
constexpr double wait_max_us = 30000.0;
constexpr UINT dxgi_default_queue = 3;

ID3D11Device* applied_device = nullptr;
u32 applied_mode = mode_off;
u32 applied_interval_us = 0;
bool queue_lowered = false;
bool reflex_active = false;
bool frame_open = false;
bool render_open = false;
NvU64 frame_id = 0;
double wait_us = 0.0;
u64 present_start = 0;

// The console line: start of frame -> GPU finished the frame, by a GPU fence. Runs only while the console draws the line.
constexpr u32 meter_slots = 64;
constexpr u32 meter_settle_frames = 30;
constexpr u32 mode_count = 4;
const char* const mode_names[mode_count] = {"off", "on", "reflex", "reflex_boost"};

ID3D11Fence* meter_fence = nullptr;
ID3D11DeviceContext4* meter_context = nullptr;
bool meter_failed = false;
bool meter_requested = false;
bool meter_active = false;
u64 meter_frame_start = 0;
u64 meter_value = 0;
HANDLE meter_work = nullptr;
HANDLE meter_events[meter_slots];
u64 meter_start[meter_slots];
u32 meter_mode[meter_slots];
std::atomic<long> meter_pending{0};
std::atomic<float> mode_ms[mode_count];
std::atomic<float> mode_fps[mode_count];

// Publishes once a second, per mode. A mode's first frames, and frames after the console was closed, are not counted.
void meter_thread(void*)
{
	const u64 second = CPU::qpc_freq;
	u64 next = 0, window_start = 0, last_done = 0, sum = 0;
	u32 window_mode = mode_count, settle = 0, count = 0;
	while (WaitForSingleObject(meter_work, INFINITE) == WAIT_OBJECT_0)
	{
		const u32 slot = u32(next++ % meter_slots);
		const bool done = WaitForSingleObject(meter_events[slot], 1000) == WAIT_OBJECT_0;
		const u64 now = CPU::QPC();
		const u32 mode = meter_mode[slot];
		const u64 start = meter_start[slot];
		--meter_pending;
		if (!done)
			continue;
		if (mode != window_mode || now - last_done > second / 2)
		{
			window_mode = mode;
			settle = meter_settle_frames;
		}
		last_done = now;
		if (settle)
		{
			--settle;
			window_start = now;
			sum = 0;
			count = 0;
			continue;
		}
		sum += now - start;
		++count;
		if (now - window_start >= second)
		{
			mode_ms[mode] = float(double(sum) / double(count) * 1000.0 / double(second));
			mode_fps[mode] = float(double(count) * double(second) / double(now - window_start));
			window_start = now;
			sum = 0;
			count = 0;
		}
	}
}

bool meter_ready()
{
	if (meter_fence)
		return true;
	if (meter_failed)
		return false;

	ID3D11Device5* device = nullptr;
	meter_failed = FAILED(HW.pDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&device)) ||
		FAILED(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void**)&meter_fence)) ||
		FAILED(HW.pContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&meter_context));
	_RELEASE(device);
	if (meter_failed)
	{
		_RELEASE(meter_fence);
		return false;
	}

	if (!meter_work)
	{
		meter_work = CreateSemaphoreW(nullptr, 0, meter_slots, nullptr);
		for (HANDLE& event : meter_events)
			event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		thread_spawn(meter_thread, "Latency meter", 0, nullptr);
	}
	return true;
}

void meter_present_done()
{
	const u64 start = meter_frame_start;
	meter_frame_start = 0;
	meter_active = meter_requested;
	meter_requested = false;
	if (!start || meter_pending >= long(meter_slots) || !meter_ready())
		return;

	const u64 value = ++meter_value;
	const u32 slot = u32((value - 1) % meter_slots);
	meter_start[slot] = start;
	meter_mode[slot] = applied_mode;
	meter_context->Signal(meter_fence, value);
	meter_fence->SetEventOnCompletion(value, meter_events[slot]);
	meter_context->Flush();
	++meter_pending;
	ReleaseSemaphore(meter_work, 1, nullptr);
}

void send_marker(NV_LATENCY_MARKER_TYPE type)
{
	NV_LATENCY_MARKER_PARAMS params = {};
	params.version = NV_LATENCY_MARKER_PARAMS_VER;
	params.frameID = frame_id;
	params.markerType = type;
	NvAPI_D3D_SetLatencyMarker(HW.pDevice, &params);
}

void set_frame_queue(UINT frames)
{
	IDXGIDevice1* dxgi = nullptr;
	if (SUCCEEDED(HW.pDevice->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgi)))
	{
		dxgi->SetMaximumFrameLatency(frames);
		dxgi->Release();
	}
}

void apply_settings(int fps_limit)
{
	const u32 mode = ps_r__low_latency;
	const bool reflex_mode = mode == mode_reflex || mode == mode_reflex_boost;
	const u32 interval_us = reflex_mode && fps_limit > 0 ? 1000000u / u32(fps_limit) : 0u;
	if (HW.pDevice == applied_device && mode == applied_mode && interval_us == applied_interval_us)
		return;

	applied_device = HW.pDevice;
	applied_mode = mode;
	applied_interval_us = interval_us;
	frame_open = false;
	render_open = false;
	wait_us = 0.0;

	// Off and never started: neither DXGI nor NVAPI is touched.
	if (mode == mode_off && !queue_lowered && !reflex_active)
		return;

	if (mode == mode_on || queue_lowered)
	{
		set_frame_queue(mode == mode_on ? 1 : dxgi_default_queue);
		queue_lowered = mode == mode_on;
	}

	if (reflex_mode || reflex_active)
	{
		NvAPI_Status status = NvAPI_Initialize();
		if (status == NVAPI_OK)
		{
			NV_SET_SLEEP_MODE_PARAMS params = {};
			params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
			params.bLowLatencyMode = reflex_mode;
			params.bLowLatencyBoost = mode == mode_reflex_boost;
			params.minimumIntervalUs = interval_us;
			status = NvAPI_D3D_SetSleepMode(HW.pDevice, &params);
		}
		reflex_active = reflex_mode && status == NVAPI_OK;
		if (reflex_mode && !reflex_active)
			Msg("! NVIDIA Reflex is not available on this system (NVAPI status %d)", int(status));
	}

	Msg("* r__low_latency: %s", reflex_active ? (mode == mode_reflex_boost ? "NVIDIA Reflex + boost" : "NVIDIA Reflex") :
		mode == mode_on ? "on" : "off");
}

// A timer up to 0.5 ms before the end, a spin after it: Sleep could wake too late and starve the GPU.
void wait_precise(double us)
{
	static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	const u64 start = CPU::QPC();
	const u64 ticks = u64(us * double(CPU::qpc_freq) / 1000000.0);
	if (timer && us > 1000.0)
	{
		LARGE_INTEGER due;
		due.QuadPart = -LONGLONG((us - 500.0) * 10.0);
		if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
			WaitForSingleObject(timer, INFINITE);
	}
	while (CPU::QPC() - start < ticks)
		_mm_pause();
}
}

namespace low_latency
{
bool frame_begin(int fps_limit)
{
	apply_settings(fps_limit);

	// A scope's second viewport pass presents nothing: it and the next pass are one frame.
	if ((reflex_active || applied_mode == mode_on) && !frame_open)
	{
		if (reflex_active)
		{
			NvAPI_D3D_Sleep(HW.pDevice);
			++frame_id;
			send_marker(SIMULATION_START);
		}
		// With VSync the queue of 1 is enough: this wait makes frames miss the vblank, -5.5 % FPS on the stand.
		else if (!psDeviceFlags.test(rsVSync) && wait_us > 0.0)
			wait_precise(wait_us);
		frame_open = true;
		render_open = false;
	}
	if (meter_active && !meter_frame_start)
		meter_frame_start = CPU::QPC();
	return reflex_active;
}

void render_begin()
{
	if (reflex_active && frame_open && !render_open)
	{
		send_marker(SIMULATION_END);
		send_marker(RENDERSUBMIT_START);
		render_open = true;
	}
}

void present_begin()
{
	if (!frame_open)
		return;
	if (reflex_active)
	{
		send_marker(RENDERSUBMIT_END);
		send_marker(PRESENT_START);
	}
	present_start = CPU::QPC();
}

void present_end()
{
	meter_present_done();
	if (!frame_open)
		return;
	if (reflex_active)
		send_marker(PRESENT_END);
	else
	{
		// Slowly up, quickly back: a starved GPU costs FPS, a little extra wait in Present costs almost nothing.
		const double block_us = double(CPU::QPC() - present_start) * 1000000.0 / double(CPU::qpc_freq);
		const double error_us = block_us - present_block_target_us;
		wait_us = std::clamp(wait_us + error_us * (error_us > 0.0 ? 0.05 : 0.5), 0.0, wait_max_us);
	}
	frame_open = false;
	render_open = false;
}

void device_destroyed()
{
	_RELEASE(meter_context);
	_RELEASE(meter_fence);
	meter_failed = false;
	meter_active = false;
	meter_requested = false;
	meter_frame_start = 0;
}

void status_line(char* text, u32 size)
{
	meter_requested = true;
	const u32 mode = ps_r__low_latency;
	const float ms = mode_ms[mode];
	const float off_ms = mode_ms[mode_off];
	if (meter_failed)
		xr_sprintf(text, size, "r__low_latency %s: this system cannot measure the latency", mode_names[mode]);
	else if (ms == 0.f)
		xr_sprintf(text, size, "r__low_latency %s: measuring...", mode_names[mode]);
	else if (mode == mode_off)
		xr_sprintf(text, size, "r__low_latency off: latency %.1f ms, %.0f fps", ms, float(mode_fps[mode]));
	else if (off_ms == 0.f)
		xr_sprintf(text, size, "r__low_latency %s: latency %.1f ms, %.0f fps | enter r__low_latency off to compare",
			mode_names[mode], ms, float(mode_fps[mode]));
	else
		xr_sprintf(text, size, "r__low_latency %s: latency %.1f ms, %.0f fps | off: %.1f ms, %.0f fps | %+.1f ms (%+.0f%%)",
			mode_names[mode], ms, float(mode_fps[mode]), off_ms, float(mode_fps[mode_off]), ms - off_ms, (ms - off_ms) * 100.f / off_ms);
}
}
