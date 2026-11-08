#pragma once

// Console: r__low_latency off|on|reflex|reflex_boost. All calls come from the main thread.
namespace low_latency
{
// Before the frame's input and update. True while NVIDIA Reflex is on: it then also holds the frame limit.
bool frame_begin(int fps_limit);
void render_begin();
void present_begin();
void present_end();
void device_destroyed();
// The console's line. Asking for it keeps the latency measured: nothing is measured while nobody asks.
void status_line(char* text, u32 size);
}
