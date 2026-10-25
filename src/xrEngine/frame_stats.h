#pragma once

// Lightweight frame-time accounting for the `frame_stats` console command.
//
// All stage timers are main-thread only and cost two QueryPerformanceCounter reads per
// scope (well below a microsecond per frame in total). The optional "deep" mode also
// times every UpdateCL() call and groups it by class id; it is off by default because
// it adds two counter reads per active object.

#include <typeinfo>
#include "../xrCore/clsid.h"

enum EFrameStage : u32
{
	fsFrame = 0,    // whole frame body of CRenderDevice::on_idle()
	fsUpdate,       //   FrameMove(): everything registered in seqFrame
	fsUpdEvents,    //     level: network receive, game events, spawn events
	fsUpdObjects,   //     CObjectList::Update(): UpdateCL() of all active objects
	fsUpdHud,       //     HUD OnFrame
	fsUpdScheduler, //     scheduler phases that run on the main thread
	fsUpdScript,    //     level script processor
	fsUpdPhysics,   //     physics commander
	fsRenderPrep,   //   render-preparation batch (skeleton jobs)
	fsLuaGcPre,     //   Lua GC slice before rendering
	fsJobsSubmit,   //   frame-parallel jobs submit
	fsRenderBegin,  //   device Begin()
	fsRenderScene,  //   seqRender: scene render submission
	fsRendCalc,     //     Render->Calculate(): visibility, light culling
	fsRendMain,     //     HOM + render_main (portal traversal, sorting) incl. z-fill
	fsRendGeom,     //     geometry pass: HUD, graph, lods, details
	fsRendLightVis, //     occlusion tests of light sources
	fsRendSun,      //     sun cascades (shadow maps) + direct light accumulate
	fsSunGather,    //       cascade preparation + single caster gather
	fsSunSubmit,    //       casters pushed into the dsgraph, per cascade
	fsSunDraw,      //       shadow-map draw submission (graph, grass, sorted)
	fsSunAccum,     //       min/max SM + cascade accumulation
	fsRendLights,   //     point/spot lights incl. their shadow maps
	fsLightSetup,   //       visibility, sort, atlas packing before the first shadow
	fsLightTraverse,//       portal traversal + culling per shadow face (dsgraph build)
	fsLightSmapDraw,//       shadow-map draw submission (graph, grass, sorted)
	fsLightAccum,   //       accumulation of shadowed/unshadowed lights, GI, volumetrics
	fsRendCombine,  //     combine + post-processing
	fsRenderPresent,//   device End(): present, may block on vsync / GPU
	fsJobsWait,     //   waiting for frame-parallel jobs (AI vision, scheduler deferred...)
	fsLuaGcPost,    //   Lua GC deferred phase after rendering
	fsFrameWait,    //   eco-render sleep: pause, main menu or r__framelimit
	fsStageCount
};

// Per-frame counters of the renderer (averaged and peaked in the report).
enum EFrameCounter : u32
{
	fcLightFacesIn = 0,  // shadowed light entries handed to render_lights (6 per point light)
	fcLightFacesVisible, // ... that survived visibility / view culling
	fcLightAtlasPages,   // shadow atlas pages used
	fcLightMaps,         // shadow maps that actually had casters
	fcLightEmpty,        // shadow maps dropped because no caster was left
	fcLightAccumShadow,  // shadowed spot/point faces accumulated
	fcLightAccumPoint,   // unshadowed point lights accumulated
	fcLightAccumSpot,    // unshadowed spot lights accumulated
	fcLightSmapCalls,    // draw calls submitted into light shadow maps
	fcLightSmapPolys,    // polygons submitted into light shadow maps
	fcLightCacheReused,  // shadow faces whose static casters came from the shadow cache page
	fcLightCacheRendered,// shadow faces whose static casters were (re)rendered into it
	fcLightCacheMovedPoint, // faces drawn without the cache: point light moved or resized
	fcLightCacheMovedSpot,  // ... spot light moved or resized
	fcLightCacheVolatile,   // ... static casters change by themselves or are translucent
	fcLightCacheNoSlot,     // ... HUD light or no free cache slot
	fcSunCascades,       // sun cascades rendered
	fcSunCalls,          // draw calls submitted into sun shadow maps
	fcSunPolys,          // polygons submitted into sun shadow maps
	fcCBufferUploads,    // constant buffer uploads (Map + copy), all passes
	fcCBufferBytes,      // bytes copied by those uploads
	fcCounterCount
};

struct SFrameStageInfo
{
	const char* name;
	u32 depth;
	u32 parent;
};

struct SFrameClientStat
{
	const void* type;
	const char* name;
	u64 ticks;
	u32 calls;
	u32 max_ticks;
};

struct SFrameClassStat
{
	CLASS_ID clsid;
	u64 ticks;
	u32 calls;
	u32 max_ticks;
};

// Report output: every line goes to the console and, when a file is open, to the file.
struct ENGINE_API SFrameReportSink
{
	FILE* file;

	explicit SFrameReportSink(FILE* f = nullptr) : file(f) {}
	void line(const char* format, ...);
};

struct ENGINE_API SFrameStats
{
	static constexpr u32 class_slots = 128;
	static constexpr u32 client_slots = 48;

	bool enabled;
	bool deep;

	u64 cur[fsStageCount];
	u64 sum[fsStageCount];
	u64 peak[fsStageCount];
	u64 worst[fsStageCount];

	u64 counter_cur[fcCounterCount];
	u64 counter_sum[fcCounterCount];
	u64 counter_peak[fcCounterCount];
	u64 worst_frame_ticks;
	u64 frames;
	u64 deep_frames;
	u64 interval_sum;
	u64 interval_max;
	u64 last_commit;
	u64 reset_time;

	SFrameClassStat updatecl[class_slots];
	u32 updatecl_overflow;
	// Skeleton poses built on the main thread (lazy renderer path, UpdateCL...) instead
	// of the render-prep workers, by class of the owner object (0 = no object).
	SFrameClassStat bones_main[class_slots];
	u32 bones_main_overflow;

	// Time of every seqFrame client (sound, environment, level, game persistent...).
	SFrameClientStat clients[client_slots];
	u32 client_count;
	u32 client_overflow;

	SFrameStats();

	void reset();
	// End of a rendered frame: folds the per-frame accumulators into the totals.
	void commit();
	void count(const u32 counter, const u64 amount)
	{
		if (enabled)
			counter_cur[counter] += amount;
	}
	void note_updatecl(CLASS_ID clsid, u64 ticks);
	void note_bones_main(CLASS_ID clsid, u64 ticks);
	void note_client(const std::type_info& type, u64 ticks);
	// Writes the report to the console and to quark_frame_stats.txt on the desktop.
	// `top` limits the per-class tables.
	void write_report(u32 top) const;
	void report(u32 top, SFrameReportSink& sink) const;
};

extern ENGINE_API SFrameStats g_frame_stats;

// `frame_ab`: A/B of two console states within one session. Segments of 1.5 s alternate
// A B B A A B B A..., so a steady drift of the scene cancels inside every block of four.
// The first 0.3 s of a segment are not counted (caches refill after a switch), neither
// are frames of pause, main menu or frame limiter sleep. Fed by SFrameStats::commit().
struct ENGINE_API SFrameAB
{
	bool active = false;
	string256 command[2];
	u32 segment = 0;
	u32 segment_count = 0;
	double segment_elapsed = 0.0;
	double segment_counted_ms = 0.0;
	u32 segment_frames = 0;
	xr_vector<double> segment_mean; // frame ms of every finished segment
	xr_vector<float> frame_ms[2];
	u64 stage_sum[2][fsStageCount];

	void start(double seconds, const char* a, const char* b);
	// Restores state A and writes the report (console + quark_frame_ab.txt on the desktop).
	void stop();
	void on_frame(const u64* stage_ticks, u64 interval_ticks);
};

extern ENGINE_API SFrameAB g_frame_ab;

// Optional extra report section supplied by the game module (Lua profiler tables).
extern ENGINE_API void (*g_frame_stats_extra)(u32 top, SFrameReportSink& sink);

// Adds the lifetime of the scope to a stage of the current frame. Nested scopes are
// inclusive: a child's time is also part of its parent's time.
struct SFrameScope
{
	u64 started;
	u32 stage;

	explicit SFrameScope(u32 stage_) : started(0), stage(stage_)
	{
		if (g_frame_stats.enabled)
			started = CPU::QPC();
	}
	~SFrameScope()
	{
		stop();
	}
	// Ends the scope early (the destructor then does nothing).
	void stop()
	{
		if (started)
		{
			g_frame_stats.cur[stage] += CPU::QPC() - started;
			started = 0;
		}
	}

	SFrameScope(const SFrameScope&) = delete;
	SFrameScope& operator=(const SFrameScope&) = delete;
};

// Frame body guard: put it right after the early returns of on_idle(). It times the
// whole body as stage fsFrame and folds the frame into the totals when it ends.
struct SFrameBody
{
	u64 started;

	SFrameBody() : started(0)
	{
		if (g_frame_stats.enabled)
			started = CPU::QPC();
	}
	~SFrameBody()
	{
		if (started)
			g_frame_stats.cur[fsFrame] += CPU::QPC() - started;
		g_frame_stats.commit();
	}

	SFrameBody(const SFrameBody&) = delete;
	SFrameBody& operator=(const SFrameBody&) = delete;
};
