// DetailManager.h: interface for the CDetailManager class.
//
//////////////////////////////////////////////////////////////////////

#ifndef DetailManagerH
#define DetailManagerH
#pragma once

#include "../../xrCore/xrpool.h"
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
		// Reused by the three hardware detail variants and by all model parts
		// belonging to this slot during one Render() call. This is transient
		// renderer state and is not part of the on-disk detail format.
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
		const Fvector4* current_benders, const Fvector4* previous_benders, u32 bender_count);

public:
	// get unpacked slot
	DetailSlot& QueryDB(int sx, int sz);

	void cache_Initialize();
	void cache_Update(int sx, int sz, Fvector& view, int limit);
	void cache_Task(int gx, int gz, Slot* D);
	Slot* cache_Query(int sx, int sz);
	void cache_Decompress(Slot* D);
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
	void PrepareSunShadowCascades(const CFrustum* const* frustums, u32 count);
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

	DetailFrameState m_frame_state;
	float m_previous_time = 0.f;
	Fvector4 m_previous_dir1 = { 0.f, 0.f, 0.f, 0.f };
	Fvector4 m_previous_dir2 = { 0.f, 0.f, 0.f, 0.f };
	u32 m_grass_spot_candidate_generation = 0;
	static constexpr u32 grass_sun_cascade_capacity = 3;
	const CFrustum* m_grass_sun_cascade_frustums[grass_sun_cascade_capacity] = {0, 0, 0};
	u32 m_grass_sun_cascade_count = 0;
	u32 m_grass_sun_cascade_index = u32(-1);
	u32 m_grass_sun_cascade_generation = 0;
	// Per detail variant/model OR of all visible slot cascade masks. This lets a
	// shadow pass skip shader/material setup for a model that contributes nothing
	// to the current cascade while retaining vector capacity across frames.
	xr_vector<u8> m_grass_sun_object_masks[3];
	u64 m_grass_render_fade_generation = 1;

	void prepare_frame_state();
	void prepare_spot_candidates();
	u32 sun_shadow_mask_for_slot(Slot& slot);
};

#endif //DetailManagerH
