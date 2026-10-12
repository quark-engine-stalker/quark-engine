#include "stdafx.h"
#include "../xrRender/DetailManager.h"

#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"

#include "../xrRenderDX10/dx10BufferUtils.h"
#include <xmmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>

const int c_hdr = 10;
const int c_size = 4;

static D3DVERTEXELEMENT9 dwDecl[] =
{
	{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, // pos
	{0, 12, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, // uv
	D3DDECL_END()
};

#pragma pack(push,1)
struct vertHW
{
	float x, y, z;
	short u, v, t, mid;
};
#pragma pack(pop)

short QC(float v);
//{
//	int t=iFloor(v*float(quant)); clamp(t,-32768,32767);
//	return short(t&0xffff);
//}

namespace
{
static_assert(sizeof(CDetailManager::SlotItem) == 80, "Keep detail instances compact and cache friendly");
static_assert(alignof(CDetailManager::SlotItem) == 16, "Detail instances must support aligned SIMD loads");
static_assert(offsetof(CDetailManager::SlotItem, xform) % 16 == 0,
	"Detail transforms must stay 16-byte aligned");
static_assert(offsetof(CDetailManager::SlotItem, alpha) ==
	offsetof(CDetailManager::SlotItem, normal) + sizeof(Fvector),
	"Normal and alpha must stay contiguous for the SIMD upload");

struct DetailConstantNames
{
	shared_str consts;
	shared_str wave;
	shared_str wind;
	shared_str array;
	shared_str xform;
	shared_str prev_wave;
	shared_str prev_wind;
	shared_str prev_benders;
	shared_str benders;
	shared_str grass_setup;
	shared_str extra_data;
	shared_str grass_align;
	shared_str base_sampler;

	DetailConstantNames()
		: consts("consts"), wave("wave"), wind("dir2D"), array("array"), xform("xform"),
		  prev_wave("wave_prev"), prev_wind("dir2D_prev"), prev_benders("benders_prevpos"),
		  benders("benders_pos"), grass_setup("benders_setup"), extra_data("exdata"),
		  grass_align("grass_align"), base_sampler("s_base")
	{
	}
};

struct DetailPassConstants
{
	R_constant_table* table;
	R_constant* consts;
	R_constant* wave;
	R_constant* wind;
	R_constant* array;
	R_constant* xform;
	R_constant* prev_wave;
	R_constant* prev_wind;
	R_constant* prev_benders;
	R_constant* benders;
	R_constant* grass_setup;
	R_constant* extra_data;
	R_constant* grass_align;
	R_constant* base_sampler;
};

	// Constant tables are shared by most detail materials. Keep the name lookups
	// out of the large per-instance function and only resolve them on a table miss.
__declspec(noinline) void resolve_detail_constants(DetailPassConstants& cache,
	R_constant_table* table, DetailConstantNames& names)
{
	cache.table = table;
	cache.consts = table ? table->get(names.consts) : nullptr;
	cache.wave = table ? table->get(names.wave) : nullptr;
	cache.wind = table ? table->get(names.wind) : nullptr;
	cache.array = table ? table->get(names.array) : nullptr;
	cache.xform = table ? table->get(names.xform) : nullptr;
	cache.prev_wave = table ? table->get(names.prev_wave) : nullptr;
	cache.prev_wind = table ? table->get(names.prev_wind) : nullptr;
	cache.prev_benders = table ? table->get(names.prev_benders) : nullptr;
	cache.benders = table ? table->get(names.benders) : nullptr;
	cache.grass_setup = table ? table->get(names.grass_setup) : nullptr;
	cache.extra_data = table ? table->get(names.extra_data) : nullptr;
	cache.grass_align = table ? table->get(names.grass_align) : nullptr;
	cache.base_sampler = table ? table->get(names.base_sampler) : nullptr;
}

ICN void render_detail_batch(const CDetail& object, u32 batch_count, u32 vertex_offset, u32 index_offset)
{
	if (g_bEnableStatGather)
		Device.Statistic->RenderDUMP_DT_Count += batch_count;
	const u32 vertex_count = batch_count * object.number_vertices;
	const u32 primitive_count = (batch_count * object.number_indices) / 3;
	RCache.Render(D3DPT_TRIANGLELIST, vertex_offset, 0, vertex_count, index_offset, primitive_count);
	RCache.stat.r.s_details.add(vertex_count);
}

// Packing is independent of shaders, cascade matrices and bender state. Keep
// the original operations and order, so cached rows are bit-for-bit identical.
ICF void pack_detail_instance(const CDetailManager::SlotItem& instance, float scale,
	Fvector4* rows, Fvector4* extra)
{
	const __m128 scale_xyz = _mm_blend_ps(_mm_set1_ps(scale), _mm_set1_ps(1.f), 0x8);
	_mm_store_ps(&rows[0].x, _mm_mul_ps(_mm_load_ps(&instance.xform[0].x), scale_xyz));
	_mm_store_ps(&rows[1].x, _mm_mul_ps(_mm_load_ps(&instance.xform[1].x), scale_xyz));
	_mm_store_ps(&rows[2].x, _mm_mul_ps(_mm_load_ps(&instance.xform[2].x), scale_xyz));
	const __m128 color = _mm_blend_ps(_mm_set1_ps(instance.c_sun),
		_mm_set1_ps(instance.c_hemi), 0x8);
	_mm_store_ps(&rows[3].x, color);
	if (extra)
		_mm_store_ps(&extra->x, _mm_load_ps(&instance.normal.x));
}

ICF void copy_detail_rows(Fvector4* destination, const Fvector4* source, u32 rows)
{
	u32 i = 0;
#if defined(__AVX2__) || defined(_M_AVX2)
	for (; i + 2 <= rows; i += 2)
		_mm256_storeu_ps(&destination[i].x, _mm256_loadu_ps(&source[i].x));
#endif
	for (; i < rows; ++i)
		_mm_store_ps(&destination[i].x, _mm_load_ps(&source[i].x));
}
}


__declspec(noinline) CDetailManager::PackedDetailKey* CDetailManager::prepare_upload_keys(
	u32 variant, u32 object, u32 visible_count, u32& key_count)
{
	const u32 frame = RDEVICE.dwFrame;
	const bool new_frame = m_detail_upload_frame != frame;
	if (new_frame)
	{
		m_detail_upload_frame = frame;
		++m_detail_upload_generation;
		m_detail_upload_key_count = 0;
		m_detail_upload_build_count = 0;
	}
	const u32 fp_control = _mm_getcsr() & ~0x3fu;
	if (m_detail_upload_fp_control != fp_control ||
		(new_frame && m_detail_upload_reset_pending && frame - m_detail_upload_last_reset_frame >= 120))
	{
		m_detail_upload_slots.clear_not_free();
		m_detail_upload_sources.clear();
		m_detail_upload_count = 0;
		m_detail_upload_reset_pending = false;
		m_detail_upload_last_reset_frame = frame;
		m_detail_upload_fp_control = fp_control;
		++m_detail_upload_generation;
		m_detail_upload_key_count = 0;
	}
	xr_vector<PackedDetailGroup>& groups = m_detail_upload_groups[variant];
	if (groups.size() != objects.size())
		groups.resize(objects.size());
	PackedDetailGroup& group = groups[object];
	if (group.generation != m_detail_upload_generation)
	{
		group.generation = m_detail_upload_generation;
		group.offset = m_detail_upload_key_count;
		group.count = _min(visible_count, detail_upload_slot_limit - group.offset);
		const u32 requested = group.offset + group.count;
		u32 capacity = 32;
		while (capacity < requested)
			capacity *= 2;
		if (m_detail_upload_keys.capacity() < capacity)
			m_detail_upload_keys.reserve(capacity);
		if (m_detail_upload_keys.size() < requested)
			m_detail_upload_keys.resize(requested);
		for (u32 i = group.offset; i < requested; ++i)
			m_detail_upload_keys[i] = PackedDetailKey{};
		m_detail_upload_key_count = requested;
	}
	key_count = group.count;
	return group.count ? m_detail_upload_keys.data() + group.offset : nullptr;
}

__declspec(noinline) bool CDetailManager::cached_slot_upload(const VisibleSlotItems& visible,
	PackedDetailKey& frame_key, u32 variant, float slot_scale,
	const Fvector4*& transforms, const Fvector4*& extra, u32& count)
{
	if ((_mm_getcsr() & ~0x3fu) != m_detail_upload_fp_control)
		return false;
	const Slot& slot = *visible.slot;
	const SlotPart* part = nullptr;
	for (const SlotPart& candidate : slot.G)
		if (&candidate.r_items[variant] == visible.items)
		{
			part = &candidate;
			break;
		}
	// Content generation covers list changes and every alpha step. Fading
	// variants still use the original packing path.
	if (!part || (part->alpha_fading_mask & (1u << variant)))
		return false;
	const SlotItemVec& items = *visible.items;
	if (items.size() < 8 || items.size() > detail_upload_instance_limit)
		return false;
	const u32 frame = RDEVICE.dwFrame;
	for (u32 i = 0; i < 2; ++i)
	{
		const u32 index = frame_key.entries[i];
		if (index >= m_detail_upload_slots.size())
			continue;
		PackedDetailSlot& entry = m_detail_upload_slots[index];
		if (entry.source == &items && entry.source_generation == slot.grass_upload_source_generation &&
			memcmp(&entry.scale, &slot_scale, sizeof(float)) == 0)
		{
			entry.last_used_frame = frame;
			transforms = m_detail_upload_xforms.data() + entry.offset * 4;
			extra = m_detail_upload_extra.data() + entry.offset;
			count = entry.count;
			return true;
		}
	}
	PackedDetailKey* source_key = nullptr;
	u32 replace_index = u32(-1), free_key = u32(-1);
	const auto existing = m_detail_upload_sources.find(&items);
	if (existing != m_detail_upload_sources.end())
	{
		source_key = &existing->second;
		frame_key = *source_key;
		for (u32 i = 0; i < 2; ++i)
		{
			const u32 index = source_key->entries[i];
			if (index == u32(-1))
			{
				free_key = i;
				continue;
			}
			VERIFY(index < m_detail_upload_slots.size());
			PackedDetailSlot& entry = m_detail_upload_slots[index];
			const bool same_scale = memcmp(&entry.scale, &slot_scale, sizeof(float)) == 0;
			if (entry.source_generation == slot.grass_upload_source_generation &&
				same_scale)
			{
				entry.last_used_frame = frame;
				transforms = m_detail_upload_xforms.data() + entry.offset * 4;
				extra = m_detail_upload_extra.data() + entry.offset;
				count = entry.count;
				return true;
			}
			if (same_scale || entry.source_generation != slot.grass_upload_source_generation)
				replace_index = index;
		}
		if (replace_index == u32(-1) && free_key == u32(-1))
		{
			const u32 left = source_key->entries[0], right = source_key->entries[1];
			replace_index = m_detail_upload_slots[left].last_used_frame <=
				m_detail_upload_slots[right].last_used_frame ? left : right;
		}
	}
	u32 capacity = 8;
	while (capacity < items.size())
		capacity *= 2;
	// Limit cold-cache preparation and simultaneous list refreshes. A declined
	// request uses the original draw loop, so a large scene cannot force one
	// frame to populate the entire arena.
	if (items.size() > detail_upload_build_limit - m_detail_upload_build_count)
		return false;
	const bool reuse_range = replace_index != u32(-1) &&
		m_detail_upload_slots[replace_index].capacity >= items.size();
	if ((!reuse_range && capacity > detail_upload_instance_limit - m_detail_upload_count) ||
		(replace_index == u32(-1) && m_detail_upload_slots.size() >= detail_upload_slot_limit))
	{
		m_detail_upload_reset_pending = true;
		return false;
	}
	u32 offset;
	if (reuse_range)
	{
		offset = m_detail_upload_slots[replace_index].offset;
		capacity = m_detail_upload_slots[replace_index].capacity;
	}
	else
	{
		offset = m_detail_upload_count;
		const u32 requested = offset + capacity;
		u32 arena_capacity = 64;
		while (arena_capacity < requested)
			arena_capacity *= 2;
		if (m_detail_upload_extra.capacity() < arena_capacity)
			m_detail_upload_extra.reserve(arena_capacity);
		if (m_detail_upload_xforms.capacity() < arena_capacity * 4)
			m_detail_upload_xforms.reserve(arena_capacity * 4);
		if (m_detail_upload_extra.size() < requested)
			m_detail_upload_extra.resize(requested);
		if (m_detail_upload_xforms.size() < requested * 4)
			m_detail_upload_xforms.resize(requested * 4);
		m_detail_upload_count = requested;
	}
	u32 packed_count = 0;
	m_detail_upload_build_count += static_cast<u32>(items.size());
	for (const SlotItem* item : items)
	{
		VERIFY(item->alpha_target != 0);
		if (item->alpha <= 0.f)
			continue;
		const float scale = item->scale * slot_scale;
		if (scale <= 0.f)
			continue;
		pack_detail_instance(*item, scale,
			m_detail_upload_xforms.data() + (offset + packed_count) * 4,
			m_detail_upload_extra.data() + offset + packed_count);
		++packed_count;
	}
	const PackedDetailSlot entry = {&items, slot.grass_upload_source_generation,
		slot_scale, offset, capacity, packed_count, frame};
	if (replace_index != u32(-1))
		m_detail_upload_slots[replace_index] = entry;
	else
	{
		u32 slot_capacity = 32;
		while (slot_capacity <= m_detail_upload_slots.size())
			slot_capacity *= 2;
		if (m_detail_upload_slots.capacity() < slot_capacity)
			m_detail_upload_slots.reserve(slot_capacity);
		const u32 index = static_cast<u32>(m_detail_upload_slots.size());
		m_detail_upload_slots.push_back(entry);
		if (!source_key)
		{
			source_key = &m_detail_upload_sources.emplace(&items, PackedDetailKey{}).first->second;
			free_key = 0;
		}
		source_key->entries[free_key] = index;
	}
	frame_key = *source_key;
	transforms = m_detail_upload_xforms.data() + offset * 4;
	extra = m_detail_upload_extra.data() + offset;
	count = packed_count;
	return true;
}


void CDetailManager::hw_Load_Shaders()
{
	// Create shader to access constant storage
	ref_shader S;
	S.create("details\\set");
	R_constant_table& T0 = *(S->E[0]->passes[0]->constants);
	R_constant_table& T1 = *(S->E[1]->passes[0]->constants);
	hwc_consts = T0.get("consts");
	hwc_wave = T0.get("wave");
	hwc_wind = T0.get("dir2D");
	hwc_array = T0.get("array");
	hwc_s_consts = T1.get("consts");
	hwc_s_xform = T1.get("xform");
	hwc_s_array = T1.get("array");
}

void CDetailManager::hw_Render()
{
	// All frame-global animation and bender data is prepared by Render once.
	// Keep this function pass-only: one Details::Render can issue three shader
	// variants without rebuilding the same CPU state.
	RCache.set_Geometry(hw_Geom);
	const DetailFrameState& state = m_frame_state;
	hw_Render_dump(state.animated_consts, state.wave[0], state.wind[0],
		state.previous_wave[0], state.previous_wind[0], 1, 0,
		state.current_benders, state.previous_benders, state.bender_count);
	hw_Render_dump(state.animated_consts, state.wave[1], state.wind[1],
		state.previous_wave[1], state.previous_wind[1], 2, 0,
		state.current_benders, state.previous_benders, state.bender_count);
	hw_Render_dump(state.still_consts, state.wave[1], state.wind[1],
		state.previous_wave[1], state.previous_wind[1], 0, 1,
		state.current_benders, state.previous_benders, state.bender_count);
}


void CDetailManager::hw_Render_dump(const Fvector4& consts, const Fvector4& wave, const Fvector4& wind,
	const Fvector4& prev_wave, const Fvector4& prev_wind, u32 var_id, u32 lod_id,
	const Fvector4* current_benders, const Fvector4* previous_benders, u32 bender_count)
{
	const bool reuse_upload = fade_distance > -1.f;
	if (reuse_upload)
		hw_Render_dump_impl<true>(consts, wave, wind, prev_wave, prev_wind, var_id, lod_id,
			current_benders, previous_benders, bender_count);
	else
		hw_Render_dump_impl<false>(consts, wave, wind, prev_wave, prev_wind, var_id, lod_id,
			current_benders, previous_benders, bender_count);
}

template <bool packed_upload>
void CDetailManager::hw_Render_dump_impl(const Fvector4& consts, const Fvector4& wave, const Fvector4& wind,
	const Fvector4& prev_wave, const Fvector4& prev_wind, u32 var_id, u32 lod_id,
	const Fvector4* current_benders, const Fvector4* previous_benders, u32 bender_count)
{
	// One guarded initialization instead of thirteen TLS guard checks per call.
	static DetailConstantNames constant_names;
	DetailPassConstants pass_constants;
	bool pass_constants_resolved = false;

	Device.Statistic->RenderDUMP_DT_Count = 0;

	// Matrices and offsets
	u32 vOffset = 0;
	u32 iOffset = 0;

	vis_list& list = m_visibles[var_id];
	const float render_fade_distance = fade_distance;
	const Fvector render_light_position = light_position;
	const bool use_light_fade = render_fade_distance <= -1.f;
	const u32 spot_candidate_generation = m_grass_spot_candidate_generation;
	// The existing DX11 detail path already batches up to hw_BatchSize instances
	// into one draw through the shader constant array. Keep that shader-compatible
	// pseudo-instancing and only remove instances that cannot contribute to the
	// current sun cascade before they consume batch slots / constant uploads.
	const bool use_sun_cascade_filter = !use_light_fade &&
		RImplementation.phase == RImplementation.PHASE_SMAP &&
		m_grass_sun_cascade_index < m_grass_sun_cascade_count;
	const u32 sun_cascade_bit = use_sun_cascade_filter ? (1u << m_grass_sun_cascade_index) : 0;
	const u64 render_fade_generation = m_grass_render_fade_generation;
	const bool use_packed_upload = packed_upload && !use_light_fade;
	const u32 batch_size = hw_BatchSize;
	const u32 array_bytes = batch_size * sizeof(Fvector4) * 4;
	const u32 extra_data_bytes = batch_size * sizeof(Fvector4);
	const u32 object_count = objects.size();

	// Iterate
	for (u32 O = 0; O < object_count; O++)
	{
		CDetail& Object = *objects[O];
		xr_vector<VisibleSlotItems>& vis = list[O];
		const bool model_in_sun_cascade = !use_sun_cascade_filter ||
			(O < m_grass_sun_object_masks[var_id].size() &&
			 (m_grass_sun_object_masks[var_id][O] & sun_cascade_bit));
		if (!vis.empty() && model_in_sun_cascade)
		{
			// Spot-shadow visibility is broad-phase data shared with the main pass. It
			// can contain only slots outside the light candidate region; avoid entering
			// the render-state/constant setup when no slot can survive this pass.
			bool has_spot_candidate = true;
			if (use_light_fade)
			{
				has_spot_candidate = false;
				for (const VisibleSlotItems& visible : vis)
				{
					if (visible.slot->grass_spot_candidate_generation == spot_candidate_generation)
					{
						has_spot_candidate = true;
						break;
					}
				}
			}

			if (has_spot_candidate)
			{
				ref_selement& detail_element = Object.shader->E[lod_id];
				for (u32 iPass = 0; iPass < detail_element->passes.size(); ++iPass)
				{
					SPass& detail_pass = *detail_element->passes[iPass];
					// Setup matrices + colors (and flush it as necessary)
					RCache.set_Element(detail_element, iPass);

					R_constant_table* const detail_table = detail_pass.constants._get();
					const bool bind_pass_constants =
						!pass_constants_resolved || pass_constants.table != detail_table;
					if (bind_pass_constants)
					{
						resolve_detail_constants(pass_constants, detail_table, constant_names);
						pass_constants_resolved = true;
					}
					RImplementation.apply_lmaterial(pass_constants.base_sampler);

					R_constant* const c_consts = pass_constants.consts;
					R_constant* const c_wave = pass_constants.wave;
					R_constant* const c_wind = pass_constants.wind;
					R_constant* const c_xform = pass_constants.xform;
					R_constant* const c_grass_align = pass_constants.grass_align;
					R_constant* const c_prev_wave = pass_constants.prev_wave;
					R_constant* const c_prev_wind = pass_constants.prev_wind;
					R_constant* const c_grass_setup = pass_constants.grass_setup;
					R_constant* const c_benders = pass_constants.benders;
					R_constant* const c_prev_benders = pass_constants.prev_benders;
					R_constant* const c_extra_data = pass_constants.extra_data;
					R_constant* const c_array = pass_constants.array;

					//	This could be cached in the corresponding consatant buffer
					//	as it is done for DX9
					if (bind_pass_constants)
					{
						RCache.set_c(c_consts, consts);
						RCache.set_c(c_wave, wave);
						RCache.set_c(c_wind, wind);
						RCache.set_c(c_xform, Device.mFullTransform);
						RCache.set_c(c_grass_align, ps_ssfx_terrain_grass_align);

						RCache.set_c(c_prev_wave, prev_wave);
						RCache.set_c(c_prev_wind, prev_wind);

						if (bender_count)
						{
							RCache.set_c(c_grass_setup, ps_ssfx_int_grass_params_1);
							const u32 bender_bytes = bender_count * sizeof(Fvector4);
							constexpr u32 direction_offset = 16u * sizeof(Fvector4);

							// Benders are pass-wide, but this loop visits every detail model. Direct
							// access marked both constant buffers dirty for every model and forced
							// redundant driver uploads. The shadow-buffer path compares each range
							// and marks it only when the frame data or the bound table really changed.
							RCache.set_ConstantData(c_benders, 0, current_benders, bender_bytes);
							RCache.set_ConstantData(c_benders, direction_offset,
								current_benders + 16, bender_bytes);
							RCache.set_ConstantData(c_prev_benders, 0, previous_benders, bender_bytes);
							RCache.set_ConstantData(c_prev_benders, direction_offset,
								previous_benders + 16, bender_bytes);
						}
					}

					//ref_constant constArray = RCache.get_c(strArray);
					//VERIFY(constArray);

					//u32			c_base				= x_array->vs.index;
					//Fvector4*	c_storage			= RCache.get_ConstantCache_Vertex().get_array_f().access(c_base);
					// Map the per-batch constant buffers only after an instance survives all
					// CPU-side rejection. AccessDirect marks the shadow buffer dirty even when
					// no bytes are written, so doing this eagerly costs an unnecessary upload
					// for fully faded/spot-filtered visible lists.
					Fvector4* c_ExData = 0;
					Fvector4* c_storage = 0;
					bool batch_storage_needs_reacquire = true;

					u32 dwBatch = 0;
					u32 upload_key_count = 0;
					PackedDetailKey* upload_keys = use_packed_upload ? prepare_upload_keys(
						var_id, O, static_cast<u32>(vis.size()), upload_key_count) : nullptr;

					xr_vector<VisibleSlotItems>::iterator _vI = vis.begin();
					xr_vector<VisibleSlotItems>::iterator _vE = vis.end();
					for (; _vI != _vE; _vI++)
					{
						SlotItemVec& items = *_vI->items;
						Slot& slot = *_vI->slot;
						if (use_light_fade && slot.grass_spot_candidate_generation != spot_candidate_generation)
							continue;
						if (use_sun_cascade_filter && !(sun_shadow_mask_for_slot(slot) & sun_cascade_bit))
							continue;

						// Distance and light fading are slot-wide values. The same slot can
						// contribute several model parts and all three detail variants, so
						// calculate the coefficient once per render pass and reuse it.
						float slot_fade;
						if (slot.grass_render_fade_generation == render_fade_generation)
						{
							slot_fade = slot.grass_render_fade;
						}
						else
						{
							slot_fade = 1.f;
							if (use_light_fade)
								slot_fade -= slot.vis.sphere.P.distance_to_xz_sqr(render_light_position) * 0.005f;
							else if (slot.distance > render_fade_distance)
								slot_fade -= (slot.distance - render_fade_distance) * 0.005f;

							slot.grass_render_fade = slot_fade;
							slot.grass_render_fade_generation = render_fade_generation;
						}
						const float slot_scale = slot.visibility_scale * slot_fade;
					if (slot_scale <= 0.f)
						continue;

					const size_t item_count = items.size();
					if (!item_count)
						continue;


					if (upload_keys && static_cast<u32>(_vI - vis.begin()) < upload_key_count)
					{
						const Fvector4* cached_transforms;
						const Fvector4* cached_extra;
						u32 cached_count;
						if (cached_slot_upload(*_vI, upload_keys[_vI - vis.begin()], var_id, slot_scale,
							cached_transforms, cached_extra, cached_count))
						{
							u32 copied = 0;
							while (copied < cached_count)
							{
								if (batch_storage_needs_reacquire)
								{
									void* extra_data;
									RCache.get_ConstantDirect(c_extra_data, extra_data_bytes, &extra_data, 0, 0);
									c_ExData = static_cast<Fvector4*>(extra_data);
									VERIFY(c_ExData);
									void* array_data;
									RCache.get_ConstantDirect(c_array, array_bytes, &array_data, 0, 0);
									c_storage = static_cast<Fvector4*>(array_data);
									VERIFY(c_storage);
									batch_storage_needs_reacquire = false;
								}
								const u32 take = _min(cached_count - copied, batch_size - dwBatch);
								copy_detail_rows(c_storage + dwBatch * 4,
									cached_transforms + copied * 4, take * 4);
								if (c_ExData)
									copy_detail_rows(c_ExData + dwBatch, cached_extra + copied, take);
								copied += take;
								dwBatch += take;
								if (dwBatch == batch_size)
								{
									render_detail_batch(Object, dwBatch, vOffset, iOffset);
									dwBatch = 0;
									batch_storage_needs_reacquire = true;
								}
							}
							continue;
						}
					}

					SlotItem* const* item = items.data();
					SlotItem* const* const item_end = item + item_count;
					constexpr size_t prefetch_distance = 8;

					for (; item != item_end; ++item)
					{
						// Visible detail lists contain pointers into many slots. Pull both cache
						// lines of a future instance in while this one is transformed; eight
						// iterations leave enough independent work to hide the pointer-chase.
						if (static_cast<size_t>(item_end - item) > prefetch_distance)
						{
							const SlotItem* const future_instance = item[prefetch_distance];
							_mm_prefetch(reinterpret_cast<const char*>(future_instance), _MM_HINT_T0);
							_mm_prefetch(reinterpret_cast<const char*>(future_instance) + 64, _MM_HINT_T0);
						}

						const SlotItem& Instance = **item;
						u32 base = dwBatch * 4;

						// Fade is advanced once per frame by UpdateVisibleM. Keeping that
						// write out of every render pass makes this hot loop read-only.
						VERIFY(Instance.alpha_target != 0);

						// Alpha lives with the transform/normal data. Reject fully faded
						// instances before touching scale on the following cache line.
						if (Instance.alpha <= 0.f)
							continue;

						const float scale = Instance.scale * slot_scale;
						if (scale <= 0.f)
							continue;

						// A full batch clears the constant-buffer dirty bit during Draw. Delay
						// marking the next batch until an instance really survives culling;
						// otherwise an exact final batch caused a redundant GPU upload later.
						if (batch_storage_needs_reacquire)
						{
							void* extra_data;
							RCache.get_ConstantDirect(c_extra_data, extra_data_bytes, &extra_data, 0, 0);
							c_ExData = static_cast<Fvector4*>(extra_data);
							VERIFY(c_ExData);
							void* array_data;
							RCache.get_ConstantDirect(c_array, array_bytes, &array_data, 0, 0);
							c_storage = static_cast<Fvector4*>(array_data);
							VERIFY(c_storage);
							batch_storage_needs_reacquire = false;
						}

						// The transform is already stored in the layout consumed by the
						// shader. SIMD scales xyz while preserving translation in w.
						const __m128 scale_xyz = _mm_blend_ps(_mm_set1_ps(scale), _mm_set1_ps(1.f), 0x8);
						_mm_store_ps(&c_storage[base + 0].x,
							_mm_mul_ps(_mm_load_ps(&Instance.xform[0].x), scale_xyz));
						_mm_store_ps(&c_storage[base + 1].x,
							_mm_mul_ps(_mm_load_ps(&Instance.xform[1].x), scale_xyz));
						_mm_store_ps(&c_storage[base + 2].x,
							_mm_mul_ps(_mm_load_ps(&Instance.xform[2].x), scale_xyz));
						//RCache.set_ca(&*constArray, base+0, M._11*scale,	M._21*scale,	M._31*scale,	M._41	);
						//RCache.set_ca(&*constArray, base+1, M._12*scale,	M._22*scale,	M._32*scale,	M._42	);
						//RCache.set_ca(&*constArray, base+2, M._13*scale,	M._23*scale,	M._33*scale,	M._43	);

						// Build color
						// R2 only needs hemisphere
						const __m128 color = _mm_blend_ps(_mm_set1_ps(Instance.c_sun),
							_mm_set1_ps(Instance.c_hemi), 0x8);
						_mm_store_ps(&c_storage[base + 3].x, color);

						if (c_ExData)
							_mm_store_ps(&c_ExData[dwBatch].x, _mm_load_ps(&Instance.normal.x));

						//RCache.set_ca(&*constArray, base+3, s,				s,				s,				h		);
						dwBatch ++;
						if (dwBatch == batch_size)
						{
							render_detail_batch(Object, dwBatch, vOffset, iOffset);

							// restart
							dwBatch = 0;
							batch_storage_needs_reacquire = true;
						}
					}
					}
					// flush if nessecary
					if (dwBatch)
					{
						render_detail_batch(Object, dwBatch, vOffset, iOffset);
					}
				}
			}
			// Clean up
			// KD: we must not clear vis on r2 since we want details shadows
			if (ps_ssfx_grass_shadows.x <= 0)
			{
				if (!psDeviceFlags2.test(rsGrassShadow) || ((ps_r2_ls_flags.test(R2FLAG_SUN_DETAILS) && (RImplementation.PHASE_SMAP ==
					RImplementation.phase)) // phase smap with shadows
					|| (ps_r2_ls_flags.test(R2FLAG_SUN_DETAILS) && (RImplementation.PHASE_NORMAL == RImplementation.phase)
						&& (!RImplementation.is_sun())) // phase normal with shadows without sun
					|| (!ps_r2_ls_flags.test(R2FLAG_SUN_DETAILS) && (RImplementation.PHASE_NORMAL == RImplementation.phase))
					)) // phase normal without shadows
					vis.clear_not_free();
			}
		}
		vOffset += batch_size * Object.number_vertices;
		iOffset += batch_size * Object.number_indices;
	}
}
