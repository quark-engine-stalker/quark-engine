#pragma once

// QUARK-PANEL, the window of quark_panel: mt_panel.cpp draws the window, the TASKS and LATENCY tabs, mt_threads.cpp the
// THREADS tab and samples the threads of the process. Russian texts are UTF-8 (u8"", the files carry a BOM).
#include <imgui.h>
#include "../xrCore/mt_tasks.h"

extern int ps_mt_panel_lang; // quark_panel_lang: 0 English, 1 Russian

namespace xr_imgui
{
namespace quark_mt
{
constexpr ImVec4 rgb(u32 hex, float alpha = 1.f)
{
	return ImVec4(float((hex >> 16) & 255) / 255.f, float((hex >> 8) & 255) / 255.f, float(hex & 255) / 255.f, alpha);
}
// Neutral graphite, soft white text and one light grey accent for what is chosen or active; no pure black or white,
// so the contrast stays easy on the eyes
constexpr ImVec4 paper = rgb(0x161616);    // the window
constexpr ImVec4 surface = rgb(0x222222);  // section strips, table headers, plates
constexpr ImVec4 stroke = rgb(0x363636);   // frames and dividers
constexpr ImVec4 muted = rgb(0x8A8A8A);    // secondary text
constexpr ImVec4 ink = rgb(0xD2D2D2);      // text
constexpr ImVec4 accent = rgb(0xA0A0A0); // a mid grey: a column of chosen blocks does not glare
constexpr ImVec4 accent_hover = rgb(0xB0B0B0);
constexpr ImVec4 accent_press = rgb(0x888888);
constexpr ImVec4 on_accent = paper;        // text on the accent
constexpr float content_width = 860.f;

inline const char* L(const char* en, const char* ru) { return ps_mt_panel_lang ? ru : en; }

ImFont* big_font();
// A light block when selected, an outlined one otherwise.
bool block_button(const char* label, bool selected, const ImVec2& size = ImVec2(0.f, 0.f));
// A horizontal bar filled to fraction (0..1).
void bar(float fraction, float width, float height);
// A table row outside the visible part of the tab keeps its height and builds nothing: the window is rebuilt every
// frame, most of a long list is scrolled away.
inline bool row_hidden(float height)
{
	ImGui::TableSetColumnIndex(0);
	if (ImGui::IsRectVisible(ImVec2(1.f, height)))
		return false;
	ImGui::Dummy(ImVec2(1.f, height));
	return true;
}
// Name of a task in the language of the window.
const char* task_name(mt_tasks::task task);

// Every frame: open starts the sampler and takes the counters of the job pool and of the frame.
void threads_frame(bool open);
void threads_tab(float fps, float frame_ms);
// Of the last second: what the whole process spent, in cores (cycles over the TSC rate).
float process_cores();
} // namespace quark_mt
} // namespace xr_imgui
