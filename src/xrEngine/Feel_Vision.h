#pragma once

#include "../xrcdb/xr_collide_defs.h"
#include "render.h"
#include "pure_relcase.h"

class IRender_Sector;
class CObject;
class ISpatial;
class ICollisionForm;

namespace Feel
{
	extern ENGINE_API int psAI_VisionAdaptiveBudget;
	extern ENGINE_API int psAI_VisionBudgetUS;
	extern ENGINE_API int psAI_VisionMaxDeferredMS;
	extern ENGINE_API int psAI_VisionHardMaxDeferredMS;
	extern ENGINE_API int psAI_VisionBackgroundMaxDeferredMS;
	extern ENGINE_API int psAI_VisionBackgroundMaxRays;
	extern ENGINE_API int psAI_VisionActiveMaxRays;
	extern ENGINE_API int psAI_VisionMaxForcedPerFrame;
	extern ENGINE_API float psAI_VisionNearDistance;
	extern ENGINE_API int psAI_VisionFrameStagger;
	extern ENGINE_API int psAI_VisionTemporalVisibleMS;
	extern ENGINE_API int psAI_VisionTemporalBlockedMS;
	extern ENGINE_API float psAI_VisionTemporalMoveEpsilon;
	extern ENGINE_API int psAI_VisionParallelMinRays;
	extern ENGINE_API int psAIAsyncPrepare;
	extern ENGINE_API int psAIAsyncPrepareMinRays;
	extern ENGINE_API int psAIAsyncPrepareLeadMS;
	extern ENGINE_API int psAI_VisionDebug;

	ENGINE_API void vision_budget_begin_frame();
	ENGINE_API void vision_budget_end_frame();
	ENGINE_API void vision_async_prepare_wait_all();
	enum EVisionBudgetPriority
	{
		VisionPriorityBackground = 0,
		VisionPriorityNormal = 1,
		VisionPriorityUrgent = 2
	};

	ENGINE_API bool vision_budget_allow(u16 owner_id, u32 candidate_count, u32 age_ms, int priority,
		bool ready_commit = false);
	ENGINE_API void vision_budget_account(u32 candidate_count, u64 elapsed_ticks);

	const float fuzzy_update_vis = 1000.f; // speed of fuzzy-logic desisions
	const float fuzzy_update_novis = 1000.f; // speed of fuzzy-logic desisions
	const float fuzzy_guaranteed = 0.001f; // distance which is supposed 100% visible
	const float lr_granularity = 0.1f; // assume similar positions

	class ENGINE_API Vision : private pure_relcase
	{
		friend void vision_async_prepare_wait_all();
	public:
		struct feel_visible_Item
		{
			collide::ray_cache Cache;
			collide::ray_cache StaticCache;
			Fvector cp_LP;
			Fvector cp_LR_src;
			Fvector cp_LR_dst;
			Fvector cp_LAST; // last point found to be visible
			CObject* O;
			CObject* last_dynamic_blocker;
			float fuzzy; // note range: (-1[no]..1[yes])
			float Cache_vis;
			float StaticCache_vis;
			u16 bone_id;
			u16 last_dynamic_blocker_id;
			BOOL Cache_valid;
			BOOL StaticCache_valid;
			BOOL StaticCache_has_opaque_triangle;
			Fvector temporal_owner_position;
			Fvector temporal_target_position;
			float temporal_visibility;
			u32 temporal_time;
			BOOL temporal_valid;
			BOOL temporal_static_opaque;
		};

		typedef xr_vector<feel_visible_Item> VISIBLE_ITEMS;

	private:
		enum class trace_prepass_status : u8
		{
			unused,
			no_collision_form,
			coincident_visible,
			near_visible,
			temporal_reuse,
			cached_blocked,
			pending,
			ready,
			serial_fallback
		};

		struct trace_prepass_result
		{
			CObject* target;
			const ISpatial* target_spatial_identity;
			const ICollisionForm* target_cform;
			u16 target_id;
			u16 bone_id;
			Fvector target_position;
			Fvector target_point;
			Fvector local_point;
			CObject* cached_dynamic_blocker;
			u16 cached_dynamic_blocker_id;
			Fvector start;
			Fvector direction;
			float range;
			float visibility;
			trace_prepass_status status;
			BOOL full_query_cache;
			BOOL static_cache_reused;
			BOOL static_query_performed;
			BOOL fast_dynamic_fallback;
			BOOL had_static_hit;
			BOOL has_opaque_triangle;
			Fvector opaque_triangle[3];
		};

		struct trace_prepass_workload
		{
			Vision* vision;
			xr_vector<trace_prepass_result>* results;
			xr_vector<u32>* jobs;
			u32 job_count;
			u32 chunk_size;
			float visibility_threshold;
			BOOL use_dynamic_snapshot;
			__declspec(align(64)) volatile LONG next_job;
		};

		struct trace_dynamic_candidate_snapshot
		{
			const ISpatial* spatial_identity;
			CObject* object_identity;
			Fsphere sphere;
			u16 object_id;
			BOOL primary_dynamic;
			BOOL visible_for_ai;
			BOOL has_collision_form;
		};

		struct trace_spatial_filter_data
		{
			const Vision* vision;
			const CObject* target;
			u32 primary_dynamic_flags;
			BOOL include_primary_dynamic;
		};

		xr_vector<CObject*> seen;
		xr_vector<CObject*> query;
		xr_vector<CObject*> diff;
		xr_vector<CObject*> removed;
		collide::rq_results RQR;
		xr_vector<ISpatial*> r_spatial;
		xr_vector<trace_prepass_result> m_trace_prepass_results;
		xr_vector<u32> m_trace_prepass_jobs;
		xr_vector<ISpatial*> m_trace_dynamic_spatial;
		xr_vector<trace_dynamic_candidate_snapshot> m_trace_dynamic_candidates;
		xr_unordered_flat_map<const CObject*, u32> m_trace_dynamic_object_indices;
		const xr_vector<float>* m_static_material_transparency;
		CObject const* m_owner;
		float m_trace_fallback_ema;
		u8 m_trace_prepass_cooldown;
		bool m_trace_history_valid;
		bool m_trace_dynamic_snapshot_valid;

		typedef xr_unordered_flat_map<u16, u32> object_index_cache;
		object_index_cache m_feel_visible_indices;
		struct async_prepare_state;

		void o_new(CObject* E);
		void o_delete(CObject* E);
		void rebuild_feel_visible_indices();
		VISIBLE_ITEMS::iterator find_feel_visible(CObject* object);
		bool test_cached_dynamic_blocker(feel_visible_Item& item, const collide::ray_defs& ray);
		bool has_cached_dynamic_candidate_locked(const trace_prepass_result& result) const;
		bool has_cached_dynamic_candidate_snapshot(const trace_prepass_result& result) const;
		void clear_dynamic_blocker(feel_visible_Item& item);
		void refresh_static_material_transparency();
		float static_material_transparency(u32 triangle_id) const;
		void apply_trace_visibility(feel_visible_Item& item, float visibility, float dt, float vis_threshold);
		bool can_reuse_temporal(const feel_visible_Item& item, const Fvector& owner_position,
			const Fvector& target_position, u32 now, float vis_threshold) const;
		void record_temporal_result(feel_visible_Item& item, const Fvector& owner_position,
			const Fvector& target_position, float visibility, bool static_opaque_proof = false);
		void commit_trace_result(feel_visible_Item& item, trace_prepass_result& result,
			const Fvector& position, float dt, float vis_threshold);
		void commit_trace_results(const Fvector& position, float dt, float vis_threshold);
		void trace_item_serial(feel_visible_Item& item, const Fvector& position, float dt, float vis_threshold);
		bool query_target_materials(feel_visible_Item& item, const collide::ray_defs& ray, float& visibility,
		                            float vis_threshold);
		bool can_use_parallel_trace_prepass(bool allow_parallel, u32 job_count) const;
		void process_trace_prepass(trace_prepass_result& result, float vis_threshold);
		bool build_trace_dynamic_snapshot(const Fvector& position, u32 job_count);
		bool has_potential_dynamic_snapshot(const trace_prepass_result& result) const;
		bool has_potential_dynamic_locked(const trace_prepass_result& result) const;
		bool has_potential_dynamic(const trace_prepass_result& result) const;
		static BOOL trace_spatial_filter(ISpatial* spatial, LPVOID context);
		static void trace_prepass_worker(void* context);
		static void async_trace_worker(void* context);
		static async_prepare_state* async_state(const Vision* vision);
		void register_async_state();
		void unregister_async_state();
		bool build_async_trace_snapshot(async_prepare_state& state, const Fvector& position,
			float vis_threshold, u32 max_trace_items, const CObject* priority_target);
		bool try_consume_async_trace(const Fvector& position, float dt, float vis_threshold,
			u32 max_trace_items, const CObject* priority_target);
		void o_trace(Fvector& P, float dt, float vis_threshold, bool allow_parallel);
		void o_trace(Fvector& P, float dt, float vis_threshold, bool allow_parallel, u32 max_trace_items);
		void o_trace(Fvector& P, float dt, float vis_threshold, bool allow_parallel, u32 max_trace_items,
		             const CObject* priority_target);

	public:
		Vision(CObject const* owner);
		virtual ~Vision();

		VISIBLE_ITEMS feel_visible;

	public:
		void feel_vision_clear();
		void feel_vision_query(Fmatrix& mFull, Fvector& P);
		void feel_vision_prepare_async(Fvector& P, float vis_threshold, bool allow_parallel,
			u32 max_trace_items = 0, const CObject* priority_target = nullptr);
		void feel_vision_defer_async(u32 delay_ms);
		bool feel_vision_async_ready(u32 max_age_ms) const;
		bool feel_vision_discard_stale_async(u32 max_age_ms);
		void feel_vision_prepare_async_if_due(Fvector& P, float vis_threshold, bool allow_parallel,
			u32 max_trace_items = 0, const CObject* priority_target = nullptr);
		void feel_vision_update(CObject* parent, Fvector& P, float dt, float vis_threshold,
		                       bool allow_parallel = false);
		void feel_vision_update(CObject* parent, Fvector& P, float dt, float vis_threshold,
		                       bool allow_parallel, u32 max_trace_items);
		void feel_vision_update(CObject* parent, Fvector& P, float dt, float vis_threshold,
		                       bool allow_parallel, u32 max_trace_items, const CObject* priority_target);
		void __stdcall feel_vision_relcase(CObject* object);

		IC const VISIBLE_ITEMS& feel_vision_items() const
		{
			return feel_visible;
		}

		void feel_vision_get(xr_vector<CObject*>& R)
		{
			R.clear();
			VISIBLE_ITEMS::iterator I = feel_visible.begin(), E = feel_visible.end();
			for (; I != E; ++I)
				if (positive(I->fuzzy))
					R.push_back(I->O);
		}

		Fvector feel_vision_get_vispoint(CObject* object)
		{
			VISIBLE_ITEMS::iterator item = find_feel_visible(object);
			if (item != feel_visible.end())
			{
				VERIFY(positive(item->fuzzy));
				return item->cp_LAST;
			}

			VERIFY2(0, "There is no such object in the potentially visible list");
			return Fvector().set(flt_max, flt_max, flt_max);
		}

		virtual bool feel_vision_isRelevant(CObject* O) = 0;
		virtual float feel_vision_mtl_transp(CObject* O, u32 element) = 0;
	};
};
