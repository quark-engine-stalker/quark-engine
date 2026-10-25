#include "stdafx.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include "frame_stats.h"
#include "Engine.h"
#include "xrSheduler.h"
#include "device.h"
#include "XR_IOConsole.h"

extern int psShedulerMaxBudgetMs;

ENGINE_API SFrameStats g_frame_stats;
ENGINE_API void (*g_frame_stats_extra)(u32 top, SFrameReportSink& sink) = nullptr;

namespace
{
const SFrameStageInfo stage_info[fsStageCount] =
{
	{"frame body", 0, fsFrame},
	{"update (seqFrame)", 1, fsFrame},
	{"events/net/spawn", 2, fsUpdate},
	{"objects UpdateCL", 2, fsUpdate},
	{"hud", 2, fsUpdate},
	{"scheduler (main thr)", 2, fsUpdate},
	{"level scripts", 2, fsUpdate},
	{"physics commander", 2, fsUpdate},
	{"render prep (skeleton)", 1, fsFrame},
	{"lua gc (pre)", 1, fsFrame},
	{"jobs submit", 1, fsFrame},
	{"render begin", 1, fsFrame},
	{"render scene", 1, fsFrame},
	{"calculate (cull)", 2, fsRenderScene},
	{"main calc (HOM)", 2, fsRenderScene},
	{"geometry pass", 2, fsRenderScene},
	{"light visibility", 2, fsRenderScene},
	{"sun cascades", 2, fsRenderScene},
	{"gather+prepare", 3, fsRendSun},
	{"submit casters", 3, fsRendSun},
	{"shadow draw", 3, fsRendSun},
	{"accumulate", 3, fsRendSun},
	{"lights+shadowmaps", 2, fsRenderScene},
	{"setup/sort/pack", 3, fsRendLights},
	{"portal traversal", 3, fsRendLights},
	{"shadow draw", 3, fsRendLights},
	{"accumulate", 3, fsRendLights},
	{"combine/post", 2, fsRenderScene},
	{"present (vsync/gpu)", 1, fsFrame},
	{"jobs wait", 1, fsFrame},
	{"lua gc (post)", 1, fsFrame},
	{"pause/menu/limit sleep", 1, fsFrame},
};

double ticks_to_ms(const u64 ticks)
{
	return CPU::qpc_freq ? 1000.0 * double(ticks) / double(CPU::qpc_freq) : 0.0;
}

void note_class(SFrameClassStat* table, u32& overflow, const CLASS_ID clsid, const u64 ticks)
{
	// Open addressing over a small fixed table: a game has a few dozen object classes.
	u32 slot = u32((clsid * 0x9E3779B97F4A7C15ull) >> 57); // 7 bits -> 128 slots
	for (u32 probe = 0; probe < SFrameStats::class_slots; ++probe, slot = (slot + 1) & (SFrameStats::class_slots - 1))
	{
		SFrameClassStat& entry = table[slot];
		if (entry.calls == 0)
			entry.clsid = clsid;
		if (entry.clsid != clsid)
			continue;
		entry.ticks += ticks;
		++entry.calls;
		const u32 clamped = ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : u32(ticks);
		if (clamped > entry.max_ticks)
			entry.max_ticks = clamped;
		return;
	}
	++overflow;
}

// Copies the used entries of a class table, most expensive first; returns their count.
u32 sort_classes(const SFrameClassStat* table, SFrameClassStat* sorted)
{
	u32 count = 0;
	for (u32 slot = 0; slot < SFrameStats::class_slots; ++slot)
		if (table[slot].calls)
			sorted[count++] = table[slot];
	std::sort(sorted, sorted + count,
		[](const SFrameClassStat& a, const SFrameClassStat& b) { return a.ticks > b.ticks; });
	return count;
}
}

void SFrameReportSink::line(const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	buffer[sizeof(buffer) - 1] = 0;
	va_end(args);

	Msg("%s", buffer);
	if (file)
	{
		fputs(buffer, file);
		fputc('\n', file);
	}
}

SFrameStats::SFrameStats()
{
	enabled = true;
	deep = false;
	reset();
}

void SFrameStats::reset()
{
	memset(cur, 0, sizeof(cur));
	memset(sum, 0, sizeof(sum));
	memset(peak, 0, sizeof(peak));
	memset(worst, 0, sizeof(worst));
	memset(counter_cur, 0, sizeof(counter_cur));
	memset(counter_sum, 0, sizeof(counter_sum));
	memset(counter_peak, 0, sizeof(counter_peak));
	memset(updatecl, 0, sizeof(updatecl));
	updatecl_overflow = 0;
	memset(bones_main, 0, sizeof(bones_main));
	bones_main_overflow = 0;
	memset(clients, 0, sizeof(clients));
	client_count = 0;
	client_overflow = 0;
	worst_frame_ticks = 0;
	frames = 0;
	deep_frames = 0;
	interval_sum = 0;
	interval_max = 0;
	last_commit = 0;
	reset_time = CPU::QPC();
}

void SFrameStats::commit()
{
	if (!enabled)
	{
		memset(cur, 0, sizeof(cur));
		memset(counter_cur, 0, sizeof(counter_cur));
		return;
	}

	const u64 now = CPU::QPC();
	if (last_commit)
	{
		const u64 interval = now - last_commit;
		interval_sum += interval;
		if (interval > interval_max)
			interval_max = interval;
		if (g_frame_ab.active)
			g_frame_ab.on_frame(cur, interval);
	}
	last_commit = now;

	++frames;
	if (deep)
		++deep_frames;

	for (u32 index = 0; index < fsStageCount; ++index)
	{
		sum[index] += cur[index];
		if (cur[index] > peak[index])
			peak[index] = cur[index];
	}
	for (u32 index = 0; index < fcCounterCount; ++index)
	{
		counter_sum[index] += counter_cur[index];
		if (counter_cur[index] > counter_peak[index])
			counter_peak[index] = counter_cur[index];
	}
	memset(counter_cur, 0, sizeof(counter_cur));
	if (cur[fsFrame] > worst_frame_ticks)
	{
		worst_frame_ticks = cur[fsFrame];
		memcpy(worst, cur, sizeof(worst));
	}
	memset(cur, 0, sizeof(cur));
}

void SFrameStats::note_updatecl(const CLASS_ID clsid, const u64 ticks)
{
	note_class(updatecl, updatecl_overflow, clsid, ticks);
}

void SFrameStats::note_bones_main(const CLASS_ID clsid, const u64 ticks)
{
	note_class(bones_main, bones_main_overflow, clsid, ticks);
}

void SFrameStats::note_client(const std::type_info& type, const u64 ticks)
{
	const void* key = &type;
	for (u32 index = 0; index < client_count; ++index)
	{
		SFrameClientStat& entry = clients[index];
		if (entry.type != key)
			continue;
		entry.ticks += ticks;
		++entry.calls;
		const u32 clamped = ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : u32(ticks);
		if (clamped > entry.max_ticks)
			entry.max_ticks = clamped;
		return;
	}
	if (client_count >= client_slots)
	{
		++client_overflow;
		return;
	}
	SFrameClientStat& entry = clients[client_count++];
	entry.type = key;
	entry.name = type.name();
	entry.ticks = ticks;
	entry.calls = 1;
	entry.max_ticks = ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : u32(ticks);
}

void SFrameStats::report(const u32 top, SFrameReportSink& sink) const
{
	if (!frames)
	{
		sink.line("[frame_stats] no frames recorded yet (enabled=%d)", enabled ? 1 : 0);
		return;
	}

	const double frame_count = double(frames);
	const double body_avg = ticks_to_ms(sum[fsFrame]) / frame_count;
	const double wall_avg = frames > 1 ? ticks_to_ms(interval_sum) / double(frames - 1) : 0.0;
	const double seconds = CPU::qpc_freq ? double(CPU::QPC() - reset_time) / double(CPU::qpc_freq) : 0.0;

	sink.line("[frame_stats] frames=%llu over %.1fs, deep=%d", frames, seconds, deep ? 1 : 0);
	sink.line("[frame_stats] frame time avg=%.2f ms (%.1f fps) max=%.2f ms | engine body avg=%.2f ms, outside body avg=%.2f ms",
		wall_avg, wall_avg > 0.0 ? 1000.0 / wall_avg : 0.0, ticks_to_ms(interval_max), body_avg,
		wall_avg > body_avg ? wall_avg - body_avg : 0.0);
	sink.line("[frame_stats] %-26s %8s %8s %6s", "stage (inclusive)", "avg ms", "max ms", "% body");

	for (u32 index = 0; index < fsStageCount; ++index)
	{
		const double avg = ticks_to_ms(sum[index]) / frame_count;
		char label[64];
		xr_sprintf(label, "%*s%s", int(stage_info[index].depth * 2), "", stage_info[index].name);
		sink.line("[frame_stats] %-26s %8.3f %8.3f %5.1f%%", label, avg, ticks_to_ms(peak[index]),
			body_avg > 0.0 ? 100.0 * avg / body_avg : 0.0);

	}

	// Remainders ("other") are printed explicitly so that the list adds up to 100%.
	{
		u64 update_children = 0;
		for (u32 child = fsUpdEvents; child <= fsUpdPhysics; ++child)
			update_children += sum[child];
		const double update_other = ticks_to_ms(sum[fsUpdate] > update_children ? sum[fsUpdate] - update_children : 0) / frame_count;

		u64 frame_children = 0;
		for (u32 child = 0; child < fsStageCount; ++child)
			if (stage_info[child].depth == 1)
				frame_children += sum[child];
		const double frame_other = ticks_to_ms(sum[fsFrame] > frame_children ? sum[fsFrame] - frame_children : 0) / frame_count;

		u64 render_children = 0;
		for (u32 child = fsRendCalc; child <= fsRendCombine; ++child)
			if (stage_info[child].depth == 2)
				render_children += sum[child];
		const double render_other = ticks_to_ms(sum[fsRenderScene] > render_children ? sum[fsRenderScene] - render_children : 0) / frame_count;

		const auto remainder = [&](const u32 parent)
		{
			u64 children = 0;
			for (u32 child = parent + 1; child < fsStageCount; ++child)
				if (stage_info[child].parent == parent)
					children += sum[child];
			return ticks_to_ms(sum[parent] > children ? sum[parent] - children : 0) / frame_count;
		};
		sink.line("[frame_stats] not covered inside sun cascades=%.3f ms, inside lights+shadowmaps=%.3f ms (HUD-light fixup, layer switches, misc)",
			remainder(fsRendSun), remainder(fsRendLights));

		sink.line("[frame_stats] not covered: update other=%.3f ms (sound, environment, UI, input...), render scene other=%.3f ms (wallmarks, rain, SSS, emissive, forward, HUD/UI draw), frame other=%.3f ms",
			update_other, render_other, frame_other);
	}

	// Renderer work counters: they explain *why* the shadow stages cost what they cost.
	{
		static const char* const counter_names[fcCounterCount] =
		{
			"shadow faces submitted (6 per point light)",
			"  ... visible after culling",
			"shadow atlas pages",
			"shadow maps with casters",
			"shadow maps empty (dropped)",
			"shadowed faces accumulated",
			"unshadowed point lights",
			"unshadowed spot lights",
			"light shadow draw calls",
			"light shadow polygons",
			"shadow cache: static reused",
			"shadow cache: static rendered",
			"shadow cache miss: point light moved/resized",
			"shadow cache miss: spot light moved/resized",
			"shadow cache miss: particles/skinned/translucent",
			"shadow cache miss: HUD light or no free slot",
			"sun cascades rendered",
			"sun shadow draw calls",
			"sun shadow polygons",
			"constant buffer uploads",
			"constant buffer bytes uploaded",
		};
		sink.line("[frame_stats] renderer counters (per frame): %-44s %10s %10s", "counter", "avg", "max");
		for (u32 index = 0; index < fcCounterCount; ++index)
			sink.line("[frame_stats]   %-56s %10.2f %10llu", counter_names[index],
				double(counter_sum[index]) / frame_count, (unsigned long long)counter_peak[index]);

		const double light_calls = double(counter_sum[fcLightSmapCalls]);
		const double light_maps = double(counter_sum[fcLightMaps]);
		const double sun_calls = double(counter_sum[fcSunCalls]);
		if (light_maps > 0.0)
			sink.line("[frame_stats]   per light shadow map: %.1f draw calls, %.0f polygons, %.1f us traversal + %.1f us draw",
				light_calls / light_maps, double(counter_sum[fcLightSmapPolys]) / light_maps,
				1000.0 * ticks_to_ms(sum[fsLightTraverse]) / light_maps, 1000.0 * ticks_to_ms(sum[fsLightSmapDraw]) / light_maps);
		if (light_calls > 0.0)
			sink.line("[frame_stats]   light shadow draw submission: %.2f us per draw call", 1000.0 * ticks_to_ms(sum[fsLightSmapDraw]) / light_calls);
		if (sun_calls > 0.0)
			sink.line("[frame_stats]   sun shadow draw submission: %.2f us per draw call", 1000.0 * ticks_to_ms(sum[fsSunDraw]) / sun_calls);
	}

	{
		SFrameClassStat sorted[class_slots];
		const u32 count = sort_classes(bones_main, sorted);
		sink.line("[frame_stats] skeleton poses built on the main thread, not by workers (per frame): %-10s %8s %8s %9s",
			"class", "poses", "ms", "avg us");
		for (u32 index = 0; index < count && index < top; ++index)
		{
			char name[16] = "(no object)";
			if (sorted[index].clsid)
				CLSID2TEXT(sorted[index].clsid, name);
			const double calls = double(sorted[index].calls);
			sink.line("[frame_stats]   %-11s %8.2f %8.3f %9.1f", name, calls / frame_count,
				ticks_to_ms(sorted[index].ticks) / frame_count, ticks_to_ms(sorted[index].ticks) * 1000.0 / calls);
		}
		if (bones_main_overflow)
			sink.line("[frame_stats]   (%u poses did not fit the class table)", bones_main_overflow);
	}

	if (worst_frame_ticks)
	{
		sink.line("[frame_stats] slowest frame body=%.2f ms: update=%.2f (objects=%.2f sched=%.2f events=%.2f) prep=%.2f scene=%.2f present=%.2f wait=%.2f",
			ticks_to_ms(worst[fsFrame]), ticks_to_ms(worst[fsUpdate]), ticks_to_ms(worst[fsUpdObjects]),
			ticks_to_ms(worst[fsUpdScheduler]), ticks_to_ms(worst[fsUpdEvents]), ticks_to_ms(worst[fsRenderPrep]),
			ticks_to_ms(worst[fsRenderScene]), ticks_to_ms(worst[fsRenderPresent]), ticks_to_ms(worst[fsJobsWait]));
	}

	if (client_count)
	{
		SFrameClientStat sorted_clients[client_slots];
		for (u32 index = 0; index < client_count; ++index)
			sorted_clients[index] = clients[index];
		std::sort(sorted_clients, sorted_clients + client_count,
			[](const SFrameClientStat& a, const SFrameClientStat& b) { return a.ticks > b.ticks; });

		sink.line("[frame_stats] seqFrame clients (inside update): %-34s %8s %8s %9s", "class", "ms/frame", "calls", "max ms");
		for (u32 index = 0; index < client_count && index < top; ++index)
		{
			const char* name = sorted_clients[index].name ? sorted_clients[index].name : "?";
			if (!strncmp(name, "class ", 6))
				name += 6;
			else if (!strncmp(name, "struct ", 7))
				name += 7;
			sink.line("[frame_stats]   %-34s %8.3f %8.2f %9.3f", name,
				ticks_to_ms(sorted_clients[index].ticks) / frame_count,
				double(sorted_clients[index].calls) / frame_count,
				ticks_to_ms(sorted_clients[index].max_ticks));
		}
		if (client_overflow)
			sink.line("[frame_stats]   (%u client samples did not fit the table)", client_overflow);
	}

	if (deep && deep_frames)
	{
		SFrameClassStat sorted[class_slots];
		const u32 count = sort_classes(updatecl, sorted);

		const double deep_count = double(deep_frames);
		sink.line("[frame_stats] UpdateCL by class (per frame): %-10s %8s %8s %9s %9s", "class", "ms", "calls", "avg us", "max us");
		for (u32 index = 0; index < count && index < top; ++index)
		{
			char name[16];
			CLSID2TEXT(sorted[index].clsid, name);
			const double calls = double(sorted[index].calls);
			sink.line("[frame_stats]   %-10s %8.3f %8.1f %9.1f %9.1f", name,
				ticks_to_ms(sorted[index].ticks) / deep_count, calls / deep_count,
				ticks_to_ms(sorted[index].ticks) * 1000.0 / calls,
				ticks_to_ms(sorted[index].max_ticks) * 1000.0);
		}
		if (updatecl_overflow)
			sink.line("[frame_stats]   (%u UpdateCL samples did not fit the class table)", updatecl_overflow);
	}
	else if (!deep)
	{
		sink.line("[frame_stats] `frame_stats deep` adds UpdateCL time per object class");
	}

	Engine.Sheduler.DumpClassCosts(top, seconds, sink);
}

// Desktop first (also when it is redirected to OneDrive); the game folder is the
// fallback so that a report is never lost.
static FILE* open_report_file(const wchar_t* name, wchar_t (&path)[MAX_PATH * 2])
{
	wchar_t folder[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, folder)) && folder[0])
		swprintf(path, MAX_PATH * 2, L"%ls\\%ls", folder, name);
	else
		swprintf(path, MAX_PATH * 2, L"%ls", name);
	return _wfopen(path, L"wb");
}

void SFrameStats::write_report(const u32 top) const
{
	wchar_t path[MAX_PATH * 2] = {};
	FILE* file = open_report_file(L"quark_frame_stats.txt", path);
	SFrameReportSink sink(file);

	SYSTEMTIME now;
	GetLocalTime(&now);
	sink.line("QUARK frame_stats report %04u-%02u-%02u %02u:%02u:%02u, frame %u", now.wYear, now.wMonth, now.wDay,
		now.wHour, now.wMinute, now.wSecond, Device.dwFrame);
	sink.line("settings: mt_scheduler=%d scheduler_adaptive=%d main_budget=%d deferred_budget=%d max_budget=%d batch=%d",
		int(mt_Scheduler), int(psShedulerAdaptive), psShedulerMainBudgetMs, psShedulerDeferredBudgetMs,
		psShedulerMaxBudgetMs, psShedulerBatchSize);
	const auto& skeleton = Device.SkeletonJobStats;
	sink.line("skeleton jobs (since start or mt_skeleton_stats reset): queued=%u executed=%ld already=%ld busy=%ld "
		"batches=%u discarded=%u post_callbacks=%u skip_hidden=%u skip_unsafe=%u", skeleton.queued,
		long(skeleton.executed), long(skeleton.already), long(skeleton.busy), skeleton.batches, skeleton.discarded,
		skeleton.post_callbacks, skeleton.skip_hidden, skeleton.skip_unsafe);

	report(top, sink);
	if (g_frame_stats_extra)
		g_frame_stats_extra(top, sink);

	if (file)
	{
		fclose(file);
		Msg("[frame_stats] report saved: %ls", path);
	}
	else
	{
		Msg("[frame_stats] could not write %ls", path);
	}
}

ENGINE_API SFrameAB g_frame_ab;

extern int ps_framelimiter;

namespace
{
constexpr double ab_segment_ms = 1500.0;
constexpr double ab_warmup_ms = 300.0;

// A B B A: a linear drift adds the same amount to both halves of a block.
u32 ab_state(const u32 segment)
{
	const u32 phase = segment % 4;
	return phase == 0 || phase == 3 ? 0 : 1;
}

// Two-sided 95% Student t quantile.
double t95(const u32 dof)
{
	static const double table[] = {0.0, 12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23,
		2.20, 2.18, 2.16, 2.14, 2.13, 2.12, 2.11, 2.10, 2.09, 2.09};
	if (dof < sizeof(table) / sizeof(table[0]))
		return table[dof];
	return dof < 30 ? 2.06 : 2.00;
}

double percentile(xr_vector<float> values, const double fraction)
{
	std::sort(values.begin(), values.end());
	const size_t index = size_t(fraction * double(values.size() - 1) + 0.5);
	return values[index < values.size() ? index : values.size() - 1];
}

double mean(const xr_vector<float>& values)
{
	double total = 0.0;
	for (const float value : values)
		total += value;
	return total / double(values.size());
}
}

void SFrameAB::start(const double seconds, const char* a, const char* b)
{
	xr_strcpy(command[0], a);
	xr_strcpy(command[1], b);
	segment = 0;
	segment_count = 4 * _max(2u, u32(seconds * 1000.0 / (4.0 * ab_segment_ms) + 0.5));
	segment_elapsed = -3000.0; // not counted: time to close the console
	segment_counted_ms = 0.0;
	segment_frames = 0;
	segment_mean.clear();
	frame_ms[0].clear();
	frame_ms[1].clear();
	memset(stage_sum, 0, sizeof(stage_sum));
	g_frame_stats.enabled = true;
	active = true;
	Console->Execute(command[0]);
	Msg("[frame_ab] starts in 3 s: %u segments of %.1f s, A='%s', B='%s'. Close the console, do not pause; `frame_ab stop` ends early",
		segment_count, ab_segment_ms / 1000.0, command[0], command[1]);
}

void SFrameAB::on_frame(const u64* stage_ticks, const u64 interval_ticks)
{
	if (stage_ticks[fsFrameWait])
		return;

	const double ms = ticks_to_ms(interval_ticks);
	const u32 state = ab_state(segment);
	if (segment_elapsed >= ab_warmup_ms)
	{
		frame_ms[state].push_back(float(ms));
		for (u32 index = 0; index < fsStageCount; ++index)
			stage_sum[state][index] += stage_ticks[index];
		segment_counted_ms += ms;
		++segment_frames;
	}
	segment_elapsed += ms;
	if (segment_elapsed < ab_segment_ms)
		return;

	segment_mean.push_back(segment_frames ? segment_counted_ms / segment_frames : 0.0);
	segment_elapsed = 0.0;
	segment_counted_ms = 0.0;
	segment_frames = 0;
	if (++segment == segment_count)
	{
		stop();
		return;
	}
	if (ab_state(segment) != state)
		Console->Execute(command[ab_state(segment)]);
}

void SFrameAB::stop()
{
	if (!active)
		return;
	active = false;
	Console->Execute(command[0]);

	wchar_t path[MAX_PATH * 2] = {};
	FILE* file = open_report_file(L"quark_frame_ab.txt", path);
	SFrameReportSink sink(file);

	SYSTEMTIME now;
	GetLocalTime(&now);
	sink.line("QUARK frame_ab report %04u-%02u-%02u %02u:%02u:%02u, frame %u", now.wYear, now.wMonth, now.wDay,
		now.wHour, now.wMinute, now.wSecond, Device.dwFrame);
	sink.line("[frame_ab] A: %s", command[0]);
	sink.line("[frame_ab] B: %s", command[1]);
	sink.line("[frame_ab] %u of %u segments of %.1f s (first %.1f s of each not counted), order A B B A; vsync=%d framelimit=%d; left in state A",
		u32(segment_mean.size()), segment_count, ab_segment_ms / 1000.0, ab_warmup_ms / 1000.0,
		psDeviceFlags.test(rsVSync) ? 1 : 0, ps_framelimiter);

	if (frame_ms[0].empty() || frame_ms[1].empty())
		sink.line("[frame_ab] not enough frames in both states");
	else
	{
		const double mean_a = mean(frame_ms[0]);
		const double mean_b = mean(frame_ms[1]);
		sink.line("[frame_ab] %-24s %10s %10s %10s", "", "A", "B", "B-A");
		sink.line("[frame_ab] %-24s %10u %10u", "frames", u32(frame_ms[0].size()), u32(frame_ms[1].size()));
		sink.line("[frame_ab] %-24s %10.3f %10.3f %+10.3f", "frame time mean, ms", mean_a, mean_b, mean_b - mean_a);
		sink.line("[frame_ab] %-24s %10.1f %10.1f %+10.1f", "fps (1000 / mean)", 1000.0 / mean_a, 1000.0 / mean_b,
			1000.0 / mean_b - 1000.0 / mean_a);
		const double fractions[] = {0.5, 0.95, 0.99};
		const char* const fraction_names[] = {"frame time median, ms", "frame time p95, ms", "frame time p99, ms"};
		for (u32 index = 0; index < 3; ++index)
		{
			const double value_a = percentile(frame_ms[0], fractions[index]);
			const double value_b = percentile(frame_ms[1], fractions[index]);
			sink.line("[frame_ab] %-24s %10.3f %10.3f %+10.3f", fraction_names[index], value_a, value_b, value_b - value_a);
		}

		struct { u32 stage; const char* name; } const stages[] =
		{
			{fsFrame, "frame body"},
			{fsUpdate, "  update"},
			{fsUpdObjects, "    objects UpdateCL"},
			{fsUpdScheduler, "    scheduler"},
			{fsRenderPrep, "  render prep"},
			{fsRenderScene, "  render scene"},
			{fsRendMain, "    main calc (HOM)"},
			{fsRendGeom, "    geometry pass"},
			{fsRendSun, "    sun cascades"},
			{fsSunGather, "      sun gather+prepare"},
			{fsSunSubmit, "      sun submit casters"},
			{fsSunDraw, "      sun shadow draw"},
			{fsRendLights, "    lights"},
			{fsLightTraverse, "      light traversal"},
			{fsLightSmapDraw, "      light shadow draw"},
			{fsRendCombine, "    combine/post"},
			{fsRenderPresent, "  present"},
			{fsJobsWait, "  jobs wait"},
		};
		sink.line("[frame_ab] stage means, ms:");
		for (const auto& row : stages)
		{
			const double value_a = ticks_to_ms(stage_sum[0][row.stage]) / double(frame_ms[0].size());
			const double value_b = ticks_to_ms(stage_sum[1][row.stage]) / double(frame_ms[1].size());
			sink.line("[frame_ab] %-24s %10.3f %10.3f %+10.3f", row.name, value_a, value_b, value_b - value_a);
		}

		// Each A B B A block gives one difference; their spread gives the interval.
		const u32 blocks = u32(segment_mean.size() / 4);
		if (blocks >= 2)
		{
			double total = 0.0;
			xr_vector<double> diffs;
			for (u32 block = 0; block < blocks; ++block)
			{
				const double* m = &segment_mean[block * 4];
				diffs.push_back(0.5 * (m[1] + m[2]) - 0.5 * (m[0] + m[3]));
				total += diffs.back();
			}
			const double diff = total / blocks;
			double variance = 0.0;
			for (const double value : diffs)
				variance += (value - diff) * (value - diff);
			variance /= double(blocks - 1);
			const double half = t95(blocks - 1) * sqrt(variance / blocks);
			const char* verdict = diff + half < 0.0 ? "B is faster" :
				diff - half > 0.0 ? "B is slower" : "no difference detected (run longer to narrow the interval)";
			sink.line("[frame_ab] B-A frame time %+.3f ms (%+.1f%%), 95%% interval [%+.3f, %+.3f] over %u A-B-B-A blocks: %s",
				diff, 100.0 * diff / mean_a, diff - half, diff + half, blocks, verdict);
		}
		else
			sink.line("[frame_ab] fewer than 2 complete A-B-B-A blocks: no interval");
	}

	if (file)
	{
		fclose(file);
		Msg("[frame_ab] report saved: %ls", path);
	}
	else
		Msg("[frame_ab] could not write %ls", path);
}
