// DetailManager.h: interface for the CDetailManager class.
//
//////////////////////////////////////////////////////////////////////

#ifndef DetailManagerH
#define DetailManagerH
#pragma once

#include "../../xrCore/xrpool.h"
#include "../../xrCore/job_system.h"
#include "detailformat.h"
#include "detailmodel.h"

class CFrustum;

#ifdef _EDITOR
//.	#include	"ESceneClassList.h"
	const int	dm_max_decompress	= 14;
	class CCustomObject;
	typedef u32	ObjClassID;

    typedef xr_list<CCustomObject*> 		ObjectList;
    typedef ObjectList::iterator 			ObjectIt;
    typedef xr_map<ObjClassID,ObjectList> 	ObjectMap;
    typedef ObjectMap::iterator 			ObjectPairIt;

#else
const int dm_max_decompress = 7;
#endif
//const int		dm_size				= 24;								//!
const int dm_cache1_count = 4; // 
//const int 		dm_cache1_line		= dm_size*2/dm_cache1_count;		//! dm_size*2 must be div dm_cache1_count
const int dm_max_objects = 16383; // v4 14-bit id range (0x3FFF reserved for empty)
const int dm_obj_in_slot = 4;
//const int		dm_cache_line		= dm_size+1+dm_size;
//const int		dm_cache_size		= dm_cache_line*dm_cache_line;
//const float		dm_fade				= float(2*dm_size)-.5f;
const float dm_slot_size = DETAIL_SLOT_SIZE;

//AVO: detail radius
#include "../../build_config_defines.h"
#ifdef DETAIL_RADIUS
const u32 dm_max_cache_size = 62001 * 2; // assuming max dm_size = 124
extern u32 dm_size;
extern u32 dm_cache1_line;
extern u32 dm_cache_line;
extern u32 dm_cache_size;
extern float dm_fade;
extern u32 dm_current_size; //				= iFloor((float)ps_r__detail_radius/4)*2;				//!
extern u32 dm_current_cache1_line;
//		= dm_current_size*2/dm_cache1_count;		//! dm_current_size*2 must be div dm_cache1_count
extern u32 dm_current_cache_line; //		= dm_current_size+1+dm_current_size;
extern u32 dm_current_cache_size; //		= dm_current_cache_line*dm_current_cache_line;
extern float dm_current_fade; //				= float(2*dm_current_size)-.5f;
extern float ps_current_detail_density;
extern float ps_current_detail_height;
#else
const int		dm_size = 24;								//!
const int 		dm_cache1_line = dm_size * 2 / dm_cache1_count;		//! dm_size*2 must be div dm_cache1_count
const int		dm_cache_line = dm_size + 1 + dm_size;
const int		dm_cache_size = dm_cache_line * dm_cache_line;
const float		dm_fade = float(2 * dm_size) - .5f;
#endif

class ECORE_API CDetailManager
{
public:

	float fade_distance = 99999;
	Fvector light_position;

	void details_clear();

	struct alignas(16) SlotItem
	{
		// один кустик
		// Keep the data copied every draw in aligned, contiguous SIMD blocks.
		// The immutable transform is already stored in the GPU-ready layout.
		Fvector4 xform[3];
		// normal + alpha form the fourth aligned SIMD block used by exdata.
		Fvector normal;
		float alpha;
		float scale;
		float c_hemi;
		float c_sun;
		u8 vis_ID; // индекс в visibility списке: still, wave 1, wave 2
		u8 alpha_target; // boolean fade target stored compactly
		u16 lod_rank; // detail_lod_hash of the instance in 16 bits: a thinned sun shadow pass keeps the lowest ranks
	};

	DEFINE_VECTOR(SlotItem, SlotItemDataVec, SlotItemDataVecIt);
	DEFINE_VECTOR(SlotItem*, SlotItemVec, SlotItemVecIt);

	struct SlotPart
	{
		// 
		u32 id; // ID модельки
		u8 alpha_fading_mask = 0; // r_items lists that still contain a fading instance
		// Instance bodies stay contiguous per model. Render lists keep stable
		// pointers into this vector after decompression has finished.
		SlotItemDataVec items; // список кустиков
		SlotItemVec r_items[3]; // список кустиков for render
	};

	enum SlotType
	{
		stReady = 0,
		// Ready to use
		stPending,
		// Pending for decompression

		stFORCEDWORD = 0xffffffff
	};

	struct Slot
	{
		// распакованый слот размером DETAIL_SLOT_SIZE
		struct
		{
			u32 empty :1;
			u32 type :1;
			u32 frame :30;
		};

		int sx, sz; // координаты слота X x Y
		vis_data vis; // 
		float distance;
		float visibility_scale;
		SlotPart G[dm_obj_in_slot]; // 
		bool hidden;
		u32 grass_spot_candidate_generation;
		// Sun-shadow visibility is classified once against all prepared cascades.
		// The mask is transient and generation-tagged so cache-slot reuse cannot
		// leak visibility from an older sun pass.
		u32 grass_sun_cascade_generation;
		u32 grass_sun_cascade_mask;
		u32 grass_sun_keep; // the lod_rank limit of the sun shadow passes this frame (65536: every instance)
		// Of the decompressed instances: sum of their (scale * model radius)^2 and of their ground normals
		float grass_cover;
		Fvector grass_ground;
		// Reused by the three hardware detail variants and by all model parts
		// belonging to this slot during one Render() call. This is transient
		// renderer state and is not part of the on-disk detail format.
		// Invalidated by physical reuse and actual visible-list/alpha changes,
		// independently of the scheduled visibility refresh frame.
		u64 grass_upload_source_generation = 0;
		u64 grass_visibility_parameters_generation = 0;
		u64 grass_render_fade_generation;
		float grass_render_fade;

		Slot()
		{
			frame = 0;
			empty = 1;
			type = stReady;
			sx = sz = 0;
			distance = 0.f;
			visibility_scale = 1.f;
			hidden = true;
			grass_spot_candidate_generation = 0;
			grass_sun_cascade_generation = 0;
			grass_sun_cascade_mask = 0;
			grass_sun_keep = 65536;
			grass_cover = 0.f;
			grass_ground.set(0.f, 0.f, 0.f);
			grass_render_fade_generation = 0;
			grass_render_fade = 1.f;
			vis.clear();
		}
	};

	struct CacheSlot1
	{
		u32 empty;
		vis_data vis;
		Slot** slots[dm_cache1_count * dm_cache1_count];

		CacheSlot1()
		{
			empty = 1;
			vis.clear();
		}
	};

	struct VisibleSlotItems
	{
		SlotItemVec* items;
		Slot* slot;
		// Per-frame packed CPU data; two fade coefficients cover main/sun.
	};

	typedef xr_vector<xr_vector<VisibleSlotItems>> vis_list;
	typedef xr_vector<CDetail*> DetailVec; // dynamic; 14-bit id range enforced at Load()
	typedef DetailVec::iterator DetailIt;
	typedef poolSS<SlotItem, 4096> PSS;
public:
	int dither [16][16];
public:
	// swing values
	struct SSwingValue
	{
		float rot1;
		float rot2;
		float amp1;
		float amp2;
		float speed;
		void lerp(const SSwingValue& v1, const SSwingValue& v2, float factor);
	};

	SSwingValue swing_desc[2];
	SSwingValue swing_current;
	float m_time_rot_1;
	float m_time_rot_2;
	float m_time_pos;
	float m_global_time_old;
public:
	IReader* dtFS;
	DetailHeader dtH;
	DetailSlot* dtSlots; // note: pointer into VFS
	DetailSlot DS_empty;

public:
	DetailVec objects;
	vis_list m_visibles [3]; // 0=still, 1=Wave1, 2=Wave2

#ifndef _EDITOR
	xrXRC xrc;
#endif
	//AVO: detail draw raius
	//CacheSlot1 					cache_level1[dm_cache1_line][dm_cache1_line];
	//Slot*							cache		[dm_cache_line][dm_cache_line];	// grid-cache itself
	//svector<Slot*,dm_cache_size>	cache_task;									// non-unpacked slots
	//Slot							cache_pool	[dm_cache_size];				// just memory for slots

#ifdef DETAIL_RADIUS
	CacheSlot1** cache_level1;
	Slot*** cache; // grid-cache itself
	svector<Slot*, dm_max_cache_size> cache_task; // non-unpacked slots
	Slot* cache_pool; // just memory for slots
#else
    CacheSlot1 						cache_level1[dm_cache1_line][dm_cache1_line];
    Slot*							cache[dm_cache_line][dm_cache_line];	// grid-cache itself
    svector<Slot*, dm_cache_size>	cache_task;									// non-unpacked slots
    Slot							cache_pool[dm_cache_size];				// just memory for slots*/
#endif

	int cache_cx;
	int cache_cz;

	PSS poolSI; // pool из которого выделяются SlotItem

	void UpdateVisibleM();
	void UpdateVisibleS();

	// The frame's inputs of the visibility walk, shared by its lanes
	struct VisibilityPass
	{
		Fvector eye;
		CFrustum view;
		float fade_limit, fade_start, fade_range, ssa_cheap, lod_layers, alpha_step;
		float ssa_min; // smaller grass is not drawn (r__detail_min_size)
		bool no_scale;
		bool refresh_all; // ssa_min fell (zoom): every visible slot rebuilds its lists now
	};
	// What one row of the level-1 cache adds to m_visibles, in the order the serial walk adds it
	struct VisibleEntry
	{
		VisibleSlotItems item;
		u32 variant;
		u32 object;
	};
	struct VisibleRowsWork
	{
		CDetailManager* self;
		const VisibilityPass* pass;
		volatile LONG next_row;
	};
	xr_vector<xr_vector<VisibleEntry>> m_visible_rows;
	template <typename Push> void UpdateVisibleRow(const VisibilityPass& pass, u32 row, Push& push);
	static void VisibleRowsLane(void* work);
	static void VisibleRowsHelper(void* work);

	// The constant rows of the visible instances, packed by lanes on the workers once MT_CALC has the visible lists,
	// in the order the render loop writes them, so the main pass copies contiguous rows instead of chasing scattered
	// instances. A chunk is up to main_pack_chunk entries of one model's visible list; the chunks are numbered in the
	// pass's draw order. The lanes take them from the last one backwards and the pass draws from the first one: it
	// copies the chunks already packed and writes the others itself, it never waits for them. Chunks, not whole
	// models: the first models of the order hold most of the grass.
	struct MainPassPack
	{
		xr_vector<Fvector4> rows; // 4 per instance
		xr_vector<Fvector4> extra; // 1 per instance
		u32 count = 0;
		u32 variant = 0, object = 0, begin = 0, end = 0; // the visible list entries it covers
		float farthest = 0.f; // farthest packed slot, squared like Slot::distance
		volatile LONG ready = -1; // the frame it was packed for, set after the rows
	};
	static constexpr u32 main_pack_lanes = 4;
	static constexpr u32 main_pack_chunk = 64;
	xr_vector<MainPassPack> m_main_pack; // grows only, the first m_main_pack_chunks are the frame's
	u32 m_main_pack_chunks = 0;
	xr_vector<u32> m_main_pack_first; // [band - near_bands][variant][object]: the first chunk of the model in the band
	u32 m_main_pack_frame = u32(-1);
	u32 m_main_pack_fp_control = 0;
	volatile LONG m_main_pack_next = 0; // the next chunk a lane takes, counting down
	volatile LONG m_main_pack_lane = 0; // lane numbers handed out for the frame
	volatile LONG m_main_pack_claim = -1; // the pass's chunk being drawn: the lanes stop there
	volatile LONG m_main_pack_current[main_pack_lanes] = {-1, -1, -1, -1}; // the chunk each lane reads
	u32 m_main_pack_lane_count = 1;
	u32 m_main_slow_instances = 0; // the last main pass wrote these itself from the scattered instances
	xr_jobs::task_group m_main_pack_group;
	static void pack_main_lane(void* manager);
	void stop_main_pack();

	// The main pass draws front to back across models: alpha-tested grass writes depth only after its pixel shader, so
	// just the grass already drawn rejects the grass behind it, and every model went from near to far on its own. The
	// wave instances (variants 1 and 2) of the slots nearer than near_distance go one by one, by their own distance
	// in near_bands bands (2 sqrt(d)); the farther slots go in far_bands bands of slot distance, each 1.4 times the
	// last. In each band every model of variant 1, then of variant 2. Variant 0 (small far clumps, still models) and
	// the shadow passes draw the whole lists, model by model.
	static constexpr float near_distance = 4.f;
	static constexpr u32 near_bands = 5;
	static constexpr u32 far_bands = 7;
	static constexpr u32 all_bands = near_bands + far_bands; // the band of a whole list
	static const float far_band_edges[far_bands + 1];
	struct NearInstance
	{
		float distance_sq;
		Slot* slot;
		const SlotItem* item;
	};
	xr_vector<NearInstance> m_near; // by run, each run by distance
	xr_vector<u32> m_near_runs; // [band][variant][object] (a run): its first entry of m_near, one more at the end
	xr_vector<u32> m_near_fill;
	void sort_near_instances(const Fvector& eye);
	u32 band_pack_index(u32 band, u32 variant, u32 object) const
	{
		return ((band - near_bands) * 3 + variant) * static_cast<u32>(objects.size()) + object;
	}
	void band_range(const xr_vector<VisibleSlotItems>& vis, u32 band, u32& begin, u32& end) const;

	// Far grass behind the terrain: variant 0 (the small far clumps) is a pure vertex cost and in a hilly or wooded level
	// most of it is hidden, as is the far band of the wave variants. The main pass draws those lists tile by tile, each
	// tile under a GPU predicate: its box drawn against the depth of the frame so far. The GPU skips the draws of a tile
	// whose box shows no sample; nothing waits for a result, so nothing shows up late.
	struct GrassTile
	{
		u32 key;
		Fbox box; // of its tiled slots
		u32 indices; // of its tiled instances: a predicate stalls the GPU's front end, a small tile is cheaper drawn
		ID3D11Predicate* predicate; // null: drawn without one (seen last time, small, at the camera, past the budget)
	};
	// A tile's query of the last pass, read without waiting: a tile the GPU saw is drawn without its predicate (no
	// stall, its batches merge with the neighbours'); one hidden or new is drawn under it. Only the choice of the
	// predicate depends on the history, never what is drawn.
	struct GrassTileQuery
	{
		ID3D11Predicate* predicate;
		bool visible;
	};
	static constexpr u32 grass_tile_shift = 3; // 8 x 8 slots, 16 m: 8 m tiles split more batches than they cull more
	u32 m_grass_tile_shift = 0; // this frame's; 0 = no tiles (no vertex shader path)
	xr_vector<GrassTile> m_grass_tiles;
	xr_unordered_map<u32, u32> m_grass_tile_map; // key -> m_grass_tiles
	xr_vector<ID3D11Predicate*> m_grass_tile_predicates[2]; // pools of alternate passes, grow only
	u32 m_grass_tile_pool = 0;
	xr_unordered_map<u32, GrassTileQuery> m_grass_tile_queries[2]; // key -> the query of the last pass, this one
	ref_geom m_grass_tile_geom;
	u32 grass_tile_key(const Slot& slot) const
	{
		return (u32(slot.sx >> m_grass_tile_shift) & 0xffffu) << 16 | (u32(slot.sz >> m_grass_tile_shift) & 0xffffu);
	}
	void build_grass_tiles();
	void query_grass_tiles();
	ID3D11Predicate* grass_tile_predicate(u32 key) const
	{
		const auto it = m_grass_tile_map.find(key);
		return it != m_grass_tile_map.end() ? m_grass_tiles[it->second].predicate : nullptr;
	}
public:
#ifdef _EDITOR
	virtual ObjectList* 			GetSnapList		()=0;
#endif

	IC bool UseVS() { return HW.Caps.geometry_major >= 1; }

	// Software processor
	ref_geom soft_Geom;
	void soft_Load();
	void soft_Unload();
	void soft_Render();

	// Hardware processor
	ref_geom hw_Geom;
	u32 hw_BatchSize;
	ID3DVertexBuffer* hw_VB;
	ID3DIndexBuffer* hw_IB;
	ref_constant hwc_consts;
	ref_constant hwc_wave;
	ref_constant hwc_wind;
	ref_constant hwc_array;
	ref_constant hwc_s_consts;
	ref_constant hwc_s_xform;
	ref_constant hwc_s_array;
	void hw_Load();
	void hw_Load_Geom();
	void hw_Load_Shaders();
	void hw_Unload();
	void hw_Render();
	void hw_Render_dump(const Fvector4& consts, const Fvector4& wave, const Fvector4& wind,
		const Fvector4& prev_wave, const Fvector4& prev_wind, u32 var_id, u32 lod_id,
		const Fvector4* current_benders, const Fvector4* previous_benders, u32 bender_count, u32 band);

public:
	// get unpacked slot
	DetailSlot& QueryDB(int sx, int sz);

	void cache_Initialize();
	void cache_Update(int sx, int sz, Fvector& view, int limit);
	void cache_Task(int gx, int gz, Slot* D);
	Slot* cache_Query(int sx, int sz);
	// Slots are independent: several threads decompress at once, each with its own collider
	void cache_Decompress(Slot* D, xrXRC& collider);
	BOOL cache_Validate();
	// cache grid to world
	int cg2w_X(int x) { return cache_cx - dm_size + x; }
	int cg2w_Z(int z) { return cache_cz - dm_size + (dm_cache_line - 1 - z); }
	// world to cache grid 
	int w2cg_X(int x) { return x - cache_cx + dm_size; }
	int w2cg_Z(int z) { return cache_cz - dm_size + (dm_cache_line - 1 - z); }

	void Load();
	void Unload();
	void Render();
	void ResetFrameState();
	// The renderer supplies the already prepared sun caster frustums. Detail
	// geometry keeps its existing shaders/batching; these methods only provide a
	// conservative per-slot cascade filter for the sun shadow passes.
	void PrepareSunShadowCascades(const CFrustum* const* frustums, u32 count, const Fvector& sun_direction);
	void SetSunShadowCascade(u32 cascade_index);
	void ClearSunShadowCascades();

	/// MT stuff
	xrCriticalSection MT;
	volatile LONG m_frame_calc;
	volatile LONG m_frame_rendered;

	void __stdcall MT_CALC();
	ICF void MT_SYNC()
	{
		const u32 calculated_frame = static_cast<u32>(InterlockedCompareExchange(&m_frame_calc, 0, 0));
		if (calculated_frame == RDEVICE.dwFrame)
			return;

		MT_CALC();
	}

	CDetailManager();
	virtual ~CDetailManager();

private:
	struct DetailFrameState
	{
		u32 frame = u32(-1);
		Fvector4 animated_consts;
		Fvector4 still_consts;
		Fvector4 wave[2];
		Fvector4 previous_wave[2];
		Fvector4 wind[2];
		Fvector4 previous_wind[2];
		u32 bender_count = 0;
		const Fvector4* current_benders = 0;
		const Fvector4* previous_benders = 0;
	};

	struct PackedDetailSlot
	{
		const SlotItemVec* source;
		u64 source_generation;
		float scale;
		u32 keep_rank; // the instances below this lod_rank were packed (65536: all)
		u32 offset;
		u32 capacity;
		u32 count;
		u32 last_used_frame;
	};
	struct PackedDetailKey
	{
		u32 entries[2] = {u32(-1), u32(-1)};
	};
	struct PackedDetailGroup
	{
		u64 generation = 0;
		u32 offset = 0;
		u32 count = 0;
	};
	// At most 20 MiB of CPU-only packed instance data. Stable slot contents
	// survive frame changes; appearance/fading and point lights use the old path.
	static constexpr u32 detail_upload_instance_limit = 262144;
	static constexpr u32 detail_upload_slot_limit = 16384;
	static constexpr u32 detail_upload_build_limit = 32768;
	u32 m_detail_upload_build_count = 0;
	u32 m_detail_upload_count = 0;
	u32 m_detail_upload_frame = u32(-1);
	u64 m_detail_upload_generation = 1;
	u32 m_detail_upload_last_reset_frame = 0;
	u32 m_detail_upload_fp_control = 0;
	bool m_detail_upload_reset_pending = false;
	xr_vector<PackedDetailSlot> m_detail_upload_slots;
	xr_unordered_map<const SlotItemVec*, PackedDetailKey> m_detail_upload_sources;
	xr_vector<PackedDetailKey> m_detail_upload_keys;
	xr_vector<PackedDetailGroup> m_detail_upload_groups[3];
	u32 m_detail_upload_key_count = 0;
	xr_vector<Fvector4> m_detail_upload_xforms;
	xr_vector<Fvector4> m_detail_upload_extra;
	template <bool packed_upload>
	void hw_Render_dump_impl(const Fvector4& consts, const Fvector4& wave, const Fvector4& wind,
		const Fvector4& prev_wave, const Fvector4& prev_wind, u32 var_id, u32 lod_id,
		const Fvector4* current_benders, const Fvector4* previous_benders, u32 bender_count, u32 band);
	PackedDetailKey* prepare_upload_keys(u32 variant, u32 object, u32 visible_count, u32& key_count);
	bool cached_slot_upload(const VisibleSlotItems& visible, PackedDetailKey& key, u32 variant, float slot_scale,
		u32 keep_rank, const Fvector4*& transforms, const Fvector4*& extra, u32& count);

	// Classification settings are checked once per UpdateVisibleM; slots keep
	// their original staggered refresh cadence when a console setting changes.
	u64 m_grass_visibility_parameters_generation = 1;
	float m_grass_visibility_ssa_min = 0.f;
	float m_grass_visibility_lod = 0.f;
	u32 m_grass_visibility_fp_control = 0;
	bool m_grass_visibility_no_scale = false;
	DetailFrameState m_frame_state;
	float m_previous_time = 0.f;
	Fvector4 m_previous_dir1 = { 0.f, 0.f, 0.f, 0.f };
	Fvector4 m_previous_dir2 = { 0.f, 0.f, 0.f, 0.f };
	u32 m_grass_spot_candidate_generation = 0;
	static constexpr u32 grass_sun_cascade_capacity = 3;
	const CFrustum* m_grass_sun_cascade_frustums[grass_sun_cascade_capacity] = {0, 0, 0};
	Fvector m_grass_sun_direction = {0.f, -1.f, 0.f};
	u32 m_grass_sun_cascade_count = 0;
	u32 m_grass_sun_cascade_index = u32(-1);
	u32 m_grass_sun_cascade_generation = 0;
	// Per detail variant/model OR of all visible slot cascade masks. This lets a
	// shadow pass skip shader/material setup for a model that contributes nothing
	// to the current cascade while retaining vector capacity across frames.
	xr_vector<u8> m_grass_sun_object_masks[3];
	// [cascade][variant][model]: positions in m_visibles of the slots the cascade sees, in their order there. A cascade
	// pass walks these instead of every visible slot.
	xr_vector<xr_vector<u32>> m_grass_sun_slots[grass_sun_cascade_capacity][3];
	u64 m_grass_render_fade_generation = 1;

	void prepare_frame_state();
	void prepare_spot_candidates();
	u32 sun_shadow_mask_for_slot(Slot& slot);
};

// A fixed value in [0, 1) per instance of a slot: the same instances stay every frame and after a reload
IC float detail_lod_hash(int sx, int sz, u32 part, u32 item)
{
	u32 h = u32(sx) * 0x8da6b343u ^ u32(sz) * 0xd8163841u ^ ((part << 16) + item) * 0xcb1ab31fu;
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	return float(h >> 8) * (1.f / 16777216.f);
}

#endif //DetailManagerH
