#include "stdafx.h"
#include "feel_vision.h"
#include "render.h"
#include "xr_object.h"
#include "xr_collide_form.h"
#include "igame_level.h"
#include "cl_intersect.h"
#include "GameMtlLib.h"
#include "../xrCore/job_system.h"
#include "AIAsyncPrepare.h"

namespace Feel
{
	ENGINE_API int psAI_VisionAdaptiveBudget = 1;
	ENGINE_API int psAI_VisionBudgetUS = 2000;
	ENGINE_API int psAI_VisionMaxDeferredMS = 350;
	ENGINE_API int psAI_VisionHardMaxDeferredMS = 700;
	ENGINE_API int psAI_VisionBackgroundMaxDeferredMS = 1200;
	ENGINE_API int psAI_VisionBackgroundMaxRays = 12;
	// Dense combat used to trace every visible candidate in one scheduler callback.
	// Keep the selected enemy current while spreading secondary candidates across
	// callbacks, bounding a single NPC's contribution to the main-thread frame time.
	ENGINE_API int psAI_VisionActiveMaxRays = 8;
	ENGINE_API int psAI_VisionMaxForcedPerFrame = 1;
	ENGINE_API float psAI_VisionNearDistance = 35.f;
	ENGINE_API int psAI_VisionFrameStagger = 1;
	ENGINE_API int psAI_VisionTemporalVisibleMS = 75;
	ENGINE_API int psAI_VisionTemporalBlockedMS = 400;
	ENGINE_API float psAI_VisionTemporalMoveEpsilon = 0.075f;
	// Small per-observer batches lost more to snapshot/fork-join overhead than the
	// static ray work recovered in the runtime capture. Retain explicit tuning but
	// use the previously proven threshold by default.
	ENGINE_API int psAI_VisionParallelMinRays = 12;
	ENGINE_API int psAIAsyncPrepare = 1;
	ENGINE_API int psAIAsyncPrepareMinRays = 4;
	// Active client-updated observers build their immutable snapshot shortly
	// before the scheduler is expected to consume it. The worker still has more
	// than a frame at normal frame rates, without carrying target/bone state for
	// the full 100-625 ms scheduler interval.
	// A worker batch took 0.124 ms on average and never missed readiness in the
	// latest active-world capture. Keep more than one 60 Hz frame of slack, but
	// submit closer to consumption so dynamic snapshots carry less stale state.
	ENGINE_API int psAIAsyncPrepareLeadMS = 24;
	ENGINE_API int psAI_VisionDebug = 0;

	namespace
	{
		constexpr u32 vision_initial_candidate_reserve = 32;
		constexpr u32 vision_initial_dynamic_reserve = 64;
		constexpr size_t vision_trim_threshold = 128;
		constexpr size_t vision_dynamic_trim_threshold = 256;

		template <typename T>
		void clear_and_trim_vision_vector(xr_vector<T>& values, size_t retain_count, size_t trim_threshold)
		{
			values.clear_not_free();
			if (values.capacity() <= trim_threshold)
				return;

			values.clear_and_free();
			if (retain_count)
				values.reserve(retain_count);
		}

		struct vision_budget_state
		{
			u32 frame;
			u32 requests;
			u32 granted;
			u32 skipped;
			u32 forced;
			u32 stride;
			u64 used_ticks;
			float frame_ema_us;
			float candidate_cost_ema_us;
			bool initialized;
			bool cost_initialized;

			vision_budget_state() : frame(u32(-1)), requests(0), granted(0), skipped(0), forced(0), stride(1),
				used_ticks(0), frame_ema_us(0.f), candidate_cost_ema_us(12.f), initialized(false),
				cost_initialized(false)
			{}
		};

		vision_budget_state g_vision_budget;
		SRWLOCK g_vision_async_state_lock = SRWLOCK_INIT;
		xr_unordered_flat_map<const Vision*, void*> g_vision_async_states;

		IC u32 vision_budget_hash(u16 owner_id)
		{
			u32 value = u32(owner_id) + 0x9e3779b9u;
			value ^= value >> 16;
			value *= 0x7feb352du;
			value ^= value >> 15;
			value *= 0x846ca68bu;
			return value ^ (value >> 16);
		}

		IC float vision_ticks_to_us(u64 ticks)
		{
			return CPU::qpc_freq ? float(double(ticks) * 1000000.0 / double(CPU::qpc_freq)) : 0.f;
		}
	}

	struct Vision::async_prepare_state
	{

		AIAsyncPrepare::Task task;
		xr_vector<trace_prepass_result> results;
		xr_vector<u32> selection;
		xr_vector<const CObject*> candidate_order;
		xr_vector<ISpatial*> dynamic_spatial;
		xr_vector<trace_dynamic_candidate_snapshot> dynamic_candidates;
		xr_unordered_flat_map<const CObject*, u32> dynamic_object_indices;
		xr_unordered_flat_map<const CObject*, u32> result_indices;
		const xr_vector<float>* material_transparency;
		const CDB::MODEL* static_model;
		const CDB::TRI* static_triangles;
		const Fvector* static_vertices;
		Fvector owner_position;
		float visibility_threshold;
		u32 trace_time;
		u32 pending_count;
		u32 deferred_prepare_time;
		s32 deferred_prepare_adjustment_ms;
		u32 observed_commit_age_ms;
		bool discarded;
		bool deferred_prepare;

		async_prepare_state() : material_transparency(nullptr), static_model(nullptr), static_triangles(nullptr),
			static_vertices(nullptr), visibility_threshold(0.f), trace_time(0),
			pending_count(0), deferred_prepare_time(0), deferred_prepare_adjustment_ms(0),
			observed_commit_age_ms(0), discarded(false),
			deferred_prepare(false)
		{
			results.reserve(vision_initial_candidate_reserve);
			selection.reserve(vision_initial_candidate_reserve);
			candidate_order.reserve(vision_initial_candidate_reserve);
			dynamic_spatial.reserve(vision_initial_dynamic_reserve);
			dynamic_candidates.reserve(vision_initial_dynamic_reserve);
			dynamic_object_indices.reserve(vision_initial_dynamic_reserve);
			result_indices.reserve(vision_initial_candidate_reserve);
			owner_position.set(0.f, 0.f, 0.f);
		}
	};

	Vision::async_prepare_state* Vision::async_state(const Vision* vision)
	{
		AcquireSRWLockShared(&g_vision_async_state_lock);
		const auto found = g_vision_async_states.find(vision);
		async_prepare_state* state = found != g_vision_async_states.end() ?
			static_cast<async_prepare_state*>(found->second) : nullptr;
		ReleaseSRWLockShared(&g_vision_async_state_lock);
		return state;
	}

	void Vision::register_async_state()
	{
		async_prepare_state* state = xr_new<async_prepare_state>();
		AcquireSRWLockExclusive(&g_vision_async_state_lock);
		const bool inserted = g_vision_async_states.emplace(this, state).second;
		ReleaseSRWLockExclusive(&g_vision_async_state_lock);
		R_ASSERT(inserted);
	}

	void Vision::unregister_async_state()
	{
		async_prepare_state* state = nullptr;
		AcquireSRWLockExclusive(&g_vision_async_state_lock);
		const auto found = g_vision_async_states.find(this);
		if (found != g_vision_async_states.end())
		{
			state = static_cast<async_prepare_state*>(found->second);
			g_vision_async_states.erase(found);
		}
		ReleaseSRWLockExclusive(&g_vision_async_state_lock);

		if (state)
		{
			state->task.wait();
			xr_delete(state);
		}
	}

	ENGINE_API void vision_async_prepare_wait_all()
	{
		AcquireSRWLockShared(&g_vision_async_state_lock);
		for (const auto& entry : g_vision_async_states)
			static_cast<Vision::async_prepare_state*>(entry.second)->task.wait();
		ReleaseSRWLockShared(&g_vision_async_state_lock);
	}

	ENGINE_API void vision_budget_begin_frame()
	{
		VERIFY(!xr_jobs::is_worker_thread());
		vision_budget_state& state = g_vision_budget;
		state.frame = Device.dwFrame;
		state.requests = 0;
		state.granted = 0;
		state.skipped = 0;
		state.forced = 0;
		state.used_ticks = 0;

		if (!psAI_VisionAdaptiveBudget || psAI_VisionBudgetUS <= 0 || !state.initialized)
		{
			state.stride = 1;
			return;
		}

		const float pressure = state.frame_ema_us / float(_max(psAI_VisionBudgetUS, 1));
		if (pressure <= .9f)
			state.stride = 1;
		else if (pressure <= 1.35f)
			state.stride = 2;
		else if (pressure <= 1.9f)
			state.stride = 3;
		else
			state.stride = 4;
	}

	ENGINE_API bool vision_budget_allow(u16 owner_id, u32 candidate_count, u32 age_ms, int priority,
		const bool ready_commit)
	{
		// Priority is supplied by the gameplay owner: distant/idle work may be
		// deferred more aggressively, while combat work keeps the shortest latency.
		vision_budget_state& state = g_vision_budget;
		if (state.frame != Device.dwFrame)
			vision_budget_begin_frame();

		++state.requests;

		if (!psAI_VisionAdaptiveBudget || psAI_VisionBudgetUS <= 0 || Device.dwPrecacheFrame != 0)
		{
			++state.granted;

			return true;
		}

		priority = clampr(priority, int(VisionPriorityBackground), int(VisionPriorityUrgent));
		const bool urgent = priority == VisionPriorityUrgent;
		const bool background = priority == VisionPriorityBackground;
		const u32 owner_hash = vision_budget_hash(owner_id);

		int max_defer_ms = psAI_VisionMaxDeferredMS;
		if (urgent && max_defer_ms > 0)
			max_defer_ms = _max(75, max_defer_ms / 2);
		else if (background && psAI_VisionBackgroundMaxDeferredMS > 0)
			max_defer_ms = _max(max_defer_ms, psAI_VisionBackgroundMaxDeferredMS);

		u32 force_after_ms = max_defer_ms > 0 ? u32(max_defer_ms) : 0u;
		if (force_after_ms > 4u)
		{
			const u32 jitter_window = _max(1u, force_after_ms / 4u);
			force_after_ms -= owner_hash % (jitter_window + 1u);
		}
		const bool deadline_reached = max_defer_ms <= 0 || age_ms >= force_after_ms;

		int configured_hard_defer_ms = psAI_VisionHardMaxDeferredMS;
		if (urgent && configured_hard_defer_ms > 0)
			configured_hard_defer_ms = _max(100, configured_hard_defer_ms / 2);
		else if (background && psAI_VisionBackgroundMaxDeferredMS > 0)
			configured_hard_defer_ms = _max(configured_hard_defer_ms, psAI_VisionBackgroundMaxDeferredMS);

		u32 hard_force_after_ms = configured_hard_defer_ms > 0 ?
			_max(force_after_ms, static_cast<u32>(configured_hard_defer_ms)) : force_after_ms;
		if (hard_force_after_ms > force_after_ms + 4u)
		{
			const u32 hard_jitter_window = _max(1u, (hard_force_after_ms - force_after_ms) / 2u);
			hard_force_after_ms -= owner_hash % (hard_jitter_window + 1u);
			hard_force_after_ms = _max(hard_force_after_ms, force_after_ms);
		}
		const bool hard_forced = configured_hard_defer_ms > 0 ?
			age_ms >= hard_force_after_ms : deadline_reached;
		const bool forced_quota_available = psAI_VisionMaxForcedPerFrame <= 0 ||
			state.forced < static_cast<u32>(psAI_VisionMaxForcedPerFrame);
		// The quota is an absolute per-frame cap. The previous hard-deadline bypass
		// let every overdue observer run in the same frame, which recreated the exact
		// 10-20 ms visibility bursts this budget is meant to prevent. Overdue owners
		// remain due and retry on their next scheduler callback; the scheduler's
		// per-object timing and jitter provide fair progress without a thundering herd.
		// A ready snapshot gets scheduling priority, not an unlimited CPU bypass:
		// it skips deadline/phase gates below but still has to fit the measured
		// per-frame microsecond budget because validation may require serial rays.
		const bool forced = !ready_commit && (max_defer_ms <= 0 ||
			((deadline_reached || hard_forced) && forced_quota_available));
		if (deadline_reached && !forced && !ready_commit)
		{
			++state.skipped;
			return false;
		}

		// Combat vision is latency-sensitive and bypasses phase staggering. Distant
		// idle vision gets a wider deterministic stride under sustained pressure.
		if (!forced && !urgent && psAI_VisionFrameStagger && state.stride > 1)
		{
			u32 effective_stride = state.stride;
			if (background)
				effective_stride = _min(effective_stride * 2u, 8u);
			const u32 phase = (owner_hash + Device.dwFrame) % effective_stride;
			if (phase != 0)
			{
				if (!(ready_commit))
				{
					++state.skipped;
					return false;
				}
			}
		}

		const float used_us = vision_ticks_to_us(state.used_ticks);
		const float estimate_us = 35.f + state.candidate_cost_ema_us * float(_max(candidate_count, 1u));
		const float oversubscription = urgent ? 1.35f : (background ? .70f : 1.f);
		const float allowed_us = float(psAI_VisionBudgetUS) * oversubscription;
		if (!forced && state.granted != 0 && used_us + estimate_us > allowed_us)
		{
			++state.skipped;

			return false;
		}

		++state.granted;
		if (forced)
			++state.forced;

		return true;
	}

	ENGINE_API void vision_budget_account(u32 candidate_count, u64 elapsed_ticks)
	{
		vision_budget_state& state = g_vision_budget;
		state.used_ticks += elapsed_ticks;

		const float elapsed_us = vision_ticks_to_us(elapsed_ticks);
		const float per_candidate = clampr((elapsed_us - 20.f) / float(_max(candidate_count, 1u)), 1.f, 250.f);
		state.candidate_cost_ema_us = state.cost_initialized ?
			state.candidate_cost_ema_us * .85f + per_candidate * .15f : per_candidate;
		state.cost_initialized = true;
	}

	ENGINE_API void vision_budget_end_frame()
	{
		vision_budget_state& state = g_vision_budget;
		const float frame_us = vision_ticks_to_us(state.used_ticks);
		if (state.requests)
		{
			state.frame_ema_us = state.initialized ? state.frame_ema_us * .85f + frame_us * .15f : frame_us;
			state.initialized = true;
		}

		if (psAI_VisionDebug && state.requests && (Device.dwFrame % 120u) == 0)
		{
			Msg("* AI vision budget: %.1f us (ema %.1f / target %d), requests=%u granted=%u skipped=%u forced=%u/%d stride=%u cost=%.2f us/candidate hard_defer=%d",
				frame_us, state.frame_ema_us, psAI_VisionBudgetUS, state.requests, state.granted, state.skipped,
				state.forced, psAI_VisionMaxForcedPerFrame, state.stride, state.candidate_cost_ema_us,
				psAI_VisionHardMaxDeferredMS);
		}
	}
	Vision::Vision(CObject const* owner) :
		pure_relcase(this, &Vision::feel_vision_relcase),
		m_static_material_transparency(nullptr),
		m_owner(owner),
		m_trace_fallback_ema(0.f),
		m_trace_prepass_cooldown(0),
		m_trace_history_valid(false),
		m_trace_dynamic_snapshot_valid(false)
	{
		seen.reserve(vision_initial_candidate_reserve);
		query.reserve(vision_initial_candidate_reserve);
		diff.reserve(vision_initial_candidate_reserve);
		removed.reserve(vision_initial_candidate_reserve);
		feel_visible.reserve(vision_initial_candidate_reserve);
		m_feel_visible_indices.reserve(vision_initial_candidate_reserve);
		r_spatial.reserve(vision_initial_candidate_reserve);
		RQR.r_results().reserve(16);
		m_trace_prepass_results.reserve(vision_initial_candidate_reserve);
		m_trace_prepass_jobs.reserve(vision_initial_candidate_reserve);
		m_trace_dynamic_spatial.reserve(vision_initial_dynamic_reserve);
		m_trace_dynamic_candidates.reserve(vision_initial_dynamic_reserve);
		m_trace_dynamic_object_indices.reserve(vision_initial_dynamic_reserve);
		register_async_state();
	}

	Vision::~Vision()
	{
		unregister_async_state();
	}

	struct SFeelParam
	{
		Vision* parent;
		Vision::feel_visible_Item* item;
		const xr_vector<float>* static_material_transparency;
		CObject* dynamic_blocker;
		float vis;
		float vis_threshold;

		SFeelParam(Vision* _parent, Vision::feel_visible_Item* _item, float _vis_threshold,
		           const xr_vector<float>* _static_material_transparency) : parent(_parent), item(_item),
			static_material_transparency(_static_material_transparency), dynamic_blocker(nullptr), vis(1.f),
			vis_threshold(_vis_threshold)
		{}
	};

	IC BOOL feel_vision_callback(collide::rq_result& result, LPVOID params)
	{
		SFeelParam* fp = (SFeelParam*)params;
		// The serial path historically scans r_spatial after the query to cache
		// the first visible-for-AI dynamic blocker. RayQueryCandidates has already
		// produced the exact collision result, so retain that identity here and
		// let the caller skip the duplicate broadphase/model query. Ignore the
		// target itself: it is queried intentionally and is not a third-party blocker.
		if (result.O && result.O != fp->item->O &&
			(result.O->spatial.type & STYPE_VISIBLEFORAI) && !fp->dynamic_blocker)
			fp->dynamic_blocker = result.O;

		float vis;
		if (!result.O && fp->static_material_transparency)
		{
			const CDB::TRI* triangle = g_pGameLevel->ObjectSpace.GetStaticTris() + result.element;
			const u16 material = triangle->material;
			vis = material < fp->static_material_transparency->size() ?
				(*fp->static_material_transparency)[material] :
				fp->parent->feel_vision_mtl_transp(nullptr, result.element);
		}
		else
		{
			vis = fp->parent->feel_vision_mtl_transp(result.O, result.element);
		}
		fp->vis *= vis;
		if (NULL == result.O && fis_zero(vis))
		{
			CDB::TRI* T = g_pGameLevel->ObjectSpace.GetStaticTris() + result.element;
			Fvector* V = g_pGameLevel->ObjectSpace.GetStaticVerts();
			fp->item->Cache.verts[0].set(V[T->verts[0]]);
			fp->item->Cache.verts[1].set(V[T->verts[1]]);
			fp->item->Cache.verts[2].set(V[T->verts[2]]);
		}
		return (fp->vis > fp->vis_threshold);
	}

#ifdef SPATIAL_CHANGE
	IC BOOL feel_vision_test_callback(const collide::ray_defs &rd, CObject *object, LPVOID user_data)
	{
		/* Return FALSE to see through object. */
		if (object->spatial.type & STYPE_FEELVISIONIGNORE)
		{
			return FALSE;
		}
		return TRUE;
	}
#endif

	namespace
	{
		class vision_spatial_read_guard
		{
		public:
			explicit vision_spatial_read_guard(ISpatial_DB* database) : m_database(database)
			{
				if (m_database)
					m_database->acquire_read();
			}

			~vision_spatial_read_guard()
			{
				if (m_database)
					m_database->release_read();
			}

			vision_spatial_read_guard(const vision_spatial_read_guard&) = delete;
			vision_spatial_read_guard& operator=(const vision_spatial_read_guard&) = delete;

		private:
			ISpatial_DB* m_database;
		};

		IC bool vision_ray_intersects_spatial(const ISpatial& spatial, const collide::ray_defs& ray)
		{
			return !!spatial.spatial.sphere.intersects_segment(ray.start, ray.dir, ray.range);
		}

		IC bool vision_ray_intersects_sphere(const Fsphere& sphere, const collide::ray_defs& ray)
		{
			return !!sphere.intersects_segment(ray.start, ray.dir, ray.range);
		}
	}

	void Vision::refresh_static_material_transparency()
	{
		const xr_vector<float>& transparency = GMLib.MaterialVisTransparency();
		m_static_material_transparency = transparency.size() == GMLib.CountMaterial() ? &transparency : nullptr;
	}

	float Vision::static_material_transparency(u32 triangle_id) const
	{
		const CDB::TRI* triangle = g_pGameLevel->ObjectSpace.GetStaticTris() + triangle_id;
		const u16 material = triangle->material;
		if (m_static_material_transparency &&
			material < m_static_material_transparency->size())
		{
			return (*m_static_material_transparency)[material];
		}

		return const_cast<Vision*>(this)->feel_vision_mtl_transp(nullptr, triangle_id);
	}

	void Vision::o_new(CObject* O)
	{
		feel_visible.push_back(feel_visible_Item());
		feel_visible_Item& I = feel_visible.back();
		I.O = O;
		I.last_dynamic_blocker = 0;
		I.last_dynamic_blocker_id = u16(-1);
		I.Cache_vis = 1.f;
		I.StaticCache_vis = 1.f;
		I.Cache_valid = FALSE;
		I.StaticCache_valid = FALSE;
		I.StaticCache_has_opaque_triangle = FALSE;
		I.temporal_owner_position.set(0.f, 0.f, 0.f);
		I.temporal_target_position.set(0.f, 0.f, 0.f);
		I.temporal_visibility = 0.f;
		I.temporal_time = 0;
		I.temporal_valid = FALSE;
		I.temporal_static_opaque = FALSE;
		I.Cache.verts[0].set(0, 0, 0);
		I.Cache.verts[1].set(0, 0, 0);
		I.Cache.verts[2].set(0, 0, 0);
		I.StaticCache.verts[0].set(0, 0, 0);
		I.StaticCache.verts[1].set(0, 0, 0);
		I.StaticCache.verts[2].set(0, 0, 0);
		I.fuzzy = -EPS_S;
		I.cp_LP = O->get_new_local_point_on_mesh(I.bone_id);
		I.cp_LAST = O->get_last_local_point_on_mesh(I.cp_LP, I.bone_id);
		m_feel_visible_indices[O->ID()] = feel_visible.size() - 1;
	}

	void Vision::rebuild_feel_visible_indices()
	{
		m_feel_visible_indices.clear();
		for (u32 index = 0; index < feel_visible.size(); ++index)
		{
			CObject* object = feel_visible[index].O;
			if (object)
				m_feel_visible_indices[object->ID()] = index;
		}
	}

	Vision::VISIBLE_ITEMS::iterator Vision::find_feel_visible(CObject* object)
	{
		if (!object)
			return feel_visible.end();

		const u16 id = object->ID();
		object_index_cache::iterator cached = m_feel_visible_indices.find(id);
		if (cached != m_feel_visible_indices.end())
		{
			const u32 index = cached->second;
			if (index < feel_visible.size() && feel_visible[index].O == object)
				return feel_visible.begin() + index;

			m_feel_visible_indices.erase(id);
		}

		VISIBLE_ITEMS::iterator found = std::find_if(
			feel_visible.begin(), feel_visible.end(),
			[object](const feel_visible_Item& item) { return item.O == object; });
		if (found != feel_visible.end())
			m_feel_visible_indices[id] = u32(found - feel_visible.begin());

		return found;
	}

	void Vision::o_delete(CObject* object)
	{
		VISIBLE_ITEMS::iterator item = find_feel_visible(object);
		if (item == feel_visible.end())
			return;

		feel_visible.erase(item);
		rebuild_feel_visible_indices();
	}

	void Vision::clear_dynamic_blocker(feel_visible_Item& item)
	{
		item.last_dynamic_blocker = 0;
		item.last_dynamic_blocker_id = u16(-1);
	}

	bool Vision::test_cached_dynamic_blocker(feel_visible_Item& item, const collide::ray_defs& ray)
	{
		CObject* blocker = item.last_dynamic_blocker;
		if (!blocker)
			return false;

		if (blocker == m_owner || blocker == item.O || blocker->ID() != item.last_dynamic_blocker_id ||
			!(blocker->spatial.type & STYPE_VISIBLEFORAI) || blocker->spatial.space != g_SpatialSpace ||
			!blocker->spatial.node_ptr || !blocker->collidable.model)
		{
			clear_dynamic_blocker(item);
			return false;
		}

#ifdef SPATIAL_CHANGE
		if (blocker->spatial.type & STYPE_FEELVISIONIGNORE)
		{
			clear_dynamic_blocker(item);
			return false;
		}
#endif

		if (!blocker->spatial.sphere.intersects_segment(ray.start, ray.dir, ray.range))
		{
			clear_dynamic_blocker(item);
			return false;
		}

		RQR.r_clear();
		if (blocker->collidable.model->_RayQuery(ray, RQR))
			return true;

		clear_dynamic_blocker(item);
		return false;
	}

	bool Vision::has_cached_dynamic_candidate_locked(const trace_prepass_result& result) const
	{
		CObject* blocker = result.cached_dynamic_blocker;
		if (!blocker || blocker == m_owner || blocker == result.target ||
			blocker->ID() != result.cached_dynamic_blocker_id ||
			!(blocker->spatial.type & STYPE_VISIBLEFORAI) || blocker->spatial.space != g_SpatialSpace ||
			!blocker->spatial.node_ptr || !blocker->collidable.model)
			return false;

#ifdef SPATIAL_CHANGE
		if (blocker->spatial.type & STYPE_FEELVISIONIGNORE)
			return false;
#endif

		collide::ray_defs ray(result.start, result.direction, result.range, CDB::OPT_ONLYFIRST,
			collide::rq_target(collide::rqtObject | collide::rqtObstacle));
		return vision_ray_intersects_spatial(*blocker, ray);
	}

	bool Vision::has_cached_dynamic_candidate_snapshot(const trace_prepass_result& result) const
	{
		if (!m_trace_dynamic_snapshot_valid || !result.cached_dynamic_blocker)
			return false;

		const auto cached = m_trace_dynamic_object_indices.find(result.cached_dynamic_blocker);
		if (cached == m_trace_dynamic_object_indices.end() || cached->second >= m_trace_dynamic_candidates.size())
			return false;

		collide::ray_defs ray(result.start, result.direction, result.range, CDB::OPT_ONLYFIRST,
			collide::rq_target(collide::rqtObject | collide::rqtObstacle));
		const ISpatial* target_spatial = result.target_spatial_identity;
		const trace_dynamic_candidate_snapshot& candidate = m_trace_dynamic_candidates[cached->second];
		if (candidate.spatial_identity != target_spatial &&
			candidate.object_identity == result.cached_dynamic_blocker &&
			candidate.object_id == result.cached_dynamic_blocker_id &&
			candidate.visible_for_ai && candidate.has_collision_form)
			return vision_ray_intersects_sphere(candidate.sphere, ray);

		// Preserve the exact legacy result if one object is represented by more than one spatial entry.
		for (u32 index = 0; index < m_trace_dynamic_candidates.size(); ++index)
		{
			if (index == cached->second)
				continue;

			const trace_dynamic_candidate_snapshot& duplicate = m_trace_dynamic_candidates[index];
			if (duplicate.spatial_identity == target_spatial ||
				duplicate.object_identity != result.cached_dynamic_blocker ||
				duplicate.object_id != result.cached_dynamic_blocker_id ||
				!duplicate.visible_for_ai || !duplicate.has_collision_form)
				continue;

			return vision_ray_intersects_sphere(duplicate.sphere, ray);
		}

		return false;
	}

	bool Vision::build_trace_dynamic_snapshot(const Fvector& position, u32 job_count)
	{
		m_trace_dynamic_snapshot_valid = false;
		m_trace_dynamic_spatial.clear_not_free();
		m_trace_dynamic_candidates.clear_not_free();
		m_trace_dynamic_object_indices.clear();
		const u32 minimum_jobs = static_cast<u32>(_max(psAI_VisionParallelMinRays, 2));
		if (!g_SpatialSpace || job_count < minimum_jobs)
			return false;

		Fbox bounds;
		bounds.invalidate();
		bounds.modify(position);
		for (u32 job = 0; job < job_count; ++job)
		{
			const trace_prepass_result& result = m_trace_prepass_results[m_trace_prepass_jobs[job]];
			Fvector endpoint;
			endpoint.mad(result.start, result.direction, result.range);
			bounds.modify(endpoint);
		}
		bounds.grow(EPS_L);

		Fvector center;
		Fvector half_size;
		bounds.get_CD(center, half_size);
		half_size.x = _max(half_size.x, EPS_L);
		half_size.y = _max(half_size.y, EPS_L);
		half_size.z = _max(half_size.z, EPS_L);

		const u32 combined_mask = STYPE_COLLIDEABLE | STYPE_OBSTACLE | STYPE_VISIBLEFORAI;
		{
			vision_spatial_read_guard spatial_guard(g_SpatialSpace);
			g_SpatialSpace->q_box_unlocked(m_trace_dynamic_spatial, 0, combined_mask, center, half_size);

			const ISpatial* owner_spatial = static_cast<const ISpatial*>(m_owner);
			const u32 primary_dynamic_flags = STYPE_COLLIDEABLE | STYPE_OBSTACLE;
			for (xr_vector<ISpatial*>::const_iterator spatial = m_trace_dynamic_spatial.begin();
				spatial != m_trace_dynamic_spatial.end(); ++spatial)
			{
				ISpatial* entry = *spatial;
				if (!entry || entry == owner_spatial)
					continue;

				CObject* object = entry->dcast_CObject();
				bool ignored = false;
#ifdef SPATIAL_CHANGE
				ignored = object && (object->spatial.type & STYPE_FEELVISIONIGNORE);
#endif
				const bool visible_for_ai = !ignored && (entry->spatial.type & STYPE_VISIBLEFORAI);
				const bool has_collision_form = object && object->collidable.model;
				const bool primary_dynamic = !ignored && has_collision_form &&
					primary_dynamic_flags == (entry->spatial.type & primary_dynamic_flags) &&
					object->collidable.model->Type() == cftObject;
				if (!visible_for_ai && !primary_dynamic)
					continue;

				trace_dynamic_candidate_snapshot snapshot;
				snapshot.spatial_identity = entry;
				snapshot.object_identity = object;
				snapshot.sphere = entry->spatial.sphere;
				snapshot.object_id = object ? object->ID() : u16(-1);
				snapshot.primary_dynamic = primary_dynamic ? TRUE : FALSE;
				snapshot.visible_for_ai = visible_for_ai ? TRUE : FALSE;
				snapshot.has_collision_form = has_collision_form ? TRUE : FALSE;
				m_trace_dynamic_candidates.push_back(snapshot);
				if (object)
					m_trace_dynamic_object_indices.emplace(object, m_trace_dynamic_candidates.size() - 1);
			}
		}

		// A single observer-level broadphase is beneficial while the immutable
		// candidate list remains compact. Fall back to the exact per-ray octree
		// traversal when a very large box would turn the prepass into an O(N*M) scan.
		const u64 pair_tests = u64(m_trace_dynamic_candidates.size()) * u64(job_count);
		if (m_trace_dynamic_candidates.size() > 512 || pair_tests > 32768)
		{
			m_trace_dynamic_spatial.clear_not_free();
			m_trace_dynamic_candidates.clear_not_free();
			m_trace_dynamic_object_indices.clear();
			return false;
		}

		m_trace_dynamic_spatial.clear_not_free();
		m_trace_dynamic_snapshot_valid = true;
		return true;
	}

	bool Vision::has_potential_dynamic_snapshot(const trace_prepass_result& result) const
	{
		if (!m_trace_dynamic_snapshot_valid)
			return false;

		collide::ray_defs ray(result.start, result.direction, result.range, CDB::OPT_ONLYFIRST,
			collide::rq_target(collide::rqtObject | collide::rqtObstacle));
		const ISpatial* target_spatial = result.target_spatial_identity;
		for (xr_vector<trace_dynamic_candidate_snapshot>::const_iterator candidate =
			m_trace_dynamic_candidates.begin(); candidate != m_trace_dynamic_candidates.end(); ++candidate)
		{
			if (candidate->spatial_identity == target_spatial)
				continue;
			if (!candidate->visible_for_ai && !candidate->primary_dynamic)
				continue;
			if (vision_ray_intersects_sphere(candidate->sphere, ray))
				return true;
		}

		return false;
	}

	void Vision::feel_vision_clear()
	{
		clear_and_trim_vision_vector(seen, vision_initial_candidate_reserve, vision_trim_threshold);
		clear_and_trim_vision_vector(query, vision_initial_candidate_reserve, vision_trim_threshold);
		clear_and_trim_vision_vector(diff, vision_initial_candidate_reserve, vision_trim_threshold);
		clear_and_trim_vision_vector(removed, vision_initial_candidate_reserve, vision_trim_threshold);
		clear_and_trim_vision_vector(feel_visible, vision_initial_candidate_reserve, vision_trim_threshold);

		object_index_cache compact_visible_indices;
		compact_visible_indices.reserve(vision_initial_candidate_reserve);
		m_feel_visible_indices.swap(compact_visible_indices);

		clear_and_trim_vision_vector(r_spatial, vision_initial_candidate_reserve, vision_trim_threshold);
		RQR.r_results().clear_and_free();
		RQR.r_results().reserve(16);
		clear_and_trim_vision_vector(m_trace_prepass_results, vision_initial_candidate_reserve,
			vision_trim_threshold);
		clear_and_trim_vision_vector(m_trace_prepass_jobs, vision_initial_candidate_reserve,
			vision_trim_threshold);
		clear_and_trim_vision_vector(m_trace_dynamic_spatial, vision_initial_dynamic_reserve,
			vision_dynamic_trim_threshold);
		clear_and_trim_vision_vector(m_trace_dynamic_candidates, vision_initial_dynamic_reserve,
			vision_dynamic_trim_threshold);

		decltype(m_trace_dynamic_object_indices) compact_dynamic_indices;
		compact_dynamic_indices.reserve(vision_initial_dynamic_reserve);
		m_trace_dynamic_object_indices.swap(compact_dynamic_indices);

		m_static_material_transparency = nullptr;
		m_trace_fallback_ema = 0.f;
		m_trace_prepass_cooldown = 0;
		m_trace_history_valid = false;
		m_trace_dynamic_snapshot_valid = false;
	}

	void Vision::feel_vision_relcase(CObject* object)
	{
		seen.erase(std::remove(seen.begin(), seen.end(), object), seen.end());
		query.erase(std::remove(query.begin(), query.end(), object), query.end());
		diff.erase(std::remove(diff.begin(), diff.end(), object), diff.end());
		removed.erase(std::remove(removed.begin(), removed.end(), object), removed.end());

		for (VISIBLE_ITEMS::iterator item = feel_visible.begin(); item != feel_visible.end(); ++item)
		{
			if (item->last_dynamic_blocker == object)
				clear_dynamic_blocker(*item);
		}

		o_delete(object);
	}

	void Vision::feel_vision_query(Fmatrix& mFull, Fvector& P)
	{
		CFrustum Frustum;
		Frustum.CreateFromMatrix(mFull, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

		// Traverse object database
		r_spatial.clear_not_free();
		g_SpatialSpace->q_frustum
		(
			r_spatial,
			0,
			STYPE_VISIBLEFORAI,
			Frustum
		);

		// Determine visibility for dynamic part of scene
		seen.clear_and_reserve();
		for (u32 o_it = 0; o_it < r_spatial.size(); o_it++)
		{
			ISpatial* spatial = r_spatial[o_it];
			CObject* object = spatial->dcast_CObject();
			if (object && feel_vision_isRelevant(object)) seen.push_back(object);
		}
		if (seen.size() > 1)
		{
			std::sort(seen.begin(), seen.end(), std::less<CObject*>());
			xr_vector<CObject*>::iterator end = std::unique(seen.begin(), seen.end());
			if (end != seen.end()) seen.erase(end, seen.end());
		}
	}

	bool Vision::build_async_trace_snapshot(async_prepare_state& state, const Fvector& position,
		const float vis_threshold, const u32 max_trace_items, const CObject* priority_target)
	{
		const u32 item_count = feel_visible.size();
		if (!item_count || !g_pGameLevel || !g_SpatialSpace)
			return false;

		refresh_static_material_transparency();
		if (!m_static_material_transparency)
			return false;

		state.results.clear_not_free();
		state.selection.clear_not_free();
		state.candidate_order.clear_not_free();
		state.dynamic_spatial.clear_not_free();
		state.dynamic_candidates.clear_not_free();
		state.dynamic_object_indices.clear();
		state.result_indices.clear();
		state.owner_position = position;
		state.visibility_threshold = vis_threshold;
		state.trace_time = Device.dwTimeGlobal;
		state.pending_count = 0;
		state.discarded = false;

		// Stage 0 has already published the new frustum query in `seen`, but the
		// legacy pipeline deliberately applies that membership change only at the
		// start of stage 1. Predict the exact post-merge order here without mutating
		// feel_visible: surviving items keep their order and new objects are appended
		// in the sorted `seen - query` order used by feel_vision_update(). This keeps
		// gameplay timing unchanged while making the worker prepare the slice that
		// stage 1 will actually consume.
		const std::less<CObject*> object_less;
		for (u32 index = 0; index < item_count; ++index)
		{
			CObject* const object = feel_visible[index].O;
			if (object && object != m_owner &&
				std::binary_search(seen.begin(), seen.end(), object, object_less))
			{
				state.candidate_order.push_back(object);
			}
		}
		for (CObject* const object : seen)
		{
			if (!object || object == m_owner)
				continue;
			if (!std::binary_search(query.begin(), query.end(), object, object_less))
				state.candidate_order.push_back(object);
		}

		const u32 future_item_count = static_cast<u32>(state.candidate_order.size());
		if (!future_item_count)
			return false;
		const u32 trace_count = max_trace_items ? _min(max_trace_items, future_item_count) : future_item_count;

		auto select_target = [&](const CObject* const target)
		{
			if (!target)
				return;
			const object_index_cache::const_iterator current = m_feel_visible_indices.find(target->ID());
			if (current != m_feel_visible_indices.end() && current->second < item_count &&
				feel_visible[current->second].O == target)
			{
				state.selection.push_back(current->second);
				return;
			}

			// A newly queried object has no feel_visible_Item yet: o_new() must retain
			// its original main-thread RNG/bone initialization point at stage 1.
		};

		if (trace_count < future_item_count)
		{
			const u32 first = m_trace_prepass_jobs.empty() ? 0 :
				m_trace_prepass_jobs[0] % future_item_count;
			u32 visited = 0;
			bool priority_added = false;
			if (priority_target && std::find(state.candidate_order.begin(),
				state.candidate_order.end(), priority_target) != state.candidate_order.end())
			{
				select_target(priority_target);
				priority_added = true;
			}

			// A stage-0 selected enemy can cease to be urgent before commit. Keep one
			// complete regular slice in addition to that speculative priority entry so
			// either stage-1 selection is a subset of the same immutable snapshot.
			u32 regular_selected = 0;
			while (regular_selected < trace_count && visited < future_item_count)
			{
				const CObject* const target = state.candidate_order[(first + visited) % future_item_count];
				++visited;
				if (priority_added && target == priority_target)
					continue;
				select_target(target);
				++regular_selected;
			}
		}
		else
		{
			for (const CObject* const target : state.candidate_order)
				select_target(target);
		}

		state.results.resize(state.selection.size());
		state.result_indices.reserve(state.selection.size());
		auto predict_target_offset = [&](const CObject& target, Fvector& offset)
		{
			offset.set(0.f, 0.f, 0.f);
			const s32 configured_lead_ms = _max(psAIAsyncPrepareLeadMS, 0);
			const s32 adjusted_lead_ms = _max(configured_lead_ms - state.deferred_prepare_adjustment_ms, 0);
			const u32 prediction_ms = _min(
				state.observed_commit_age_ms ? state.observed_commit_age_ms :
				static_cast<u32>(adjusted_lead_ms), 50u);
			const u32 position_count = target.ps_Size();
			if (!prediction_ms || position_count < 2)
				return false;

			const CObject::SavedPosition latest = target.ps_Element(position_count - 1);
			CObject::SavedPosition previous = target.ps_Element(position_count - 2);
			for (u32 distance = 3; latest.dwTime == previous.dwTime && distance <= position_count; ++distance)
				previous = target.ps_Element(position_count - distance);

			if (latest.dwTime <= previous.dwTime || state.trace_time - previous.dwTime > 300u)
				return false;
			const u32 sample_ms = latest.dwTime - previous.dwTime;
			if (sample_ms < 4u || sample_ms > 300u)
				return false;

			offset.sub(latest.vPosition, previous.vPosition);
			const float velocity_scale = static_cast<float>(prediction_ms) / static_cast<float>(sample_ms);
			offset.mul(velocity_scale);
			// Ignore teleport/correction history. A wrong prediction is validation-safe,
			// but it would waste the worker ray and lower useful result coverage.
			const float predicted_distance_sqr = offset.square_magnitude();
			return predicted_distance_sqr > EPS_S * EPS_S && predicted_distance_sqr <= .5f * .5f;
		};
		for (u32 snapshot_index = 0; snapshot_index < state.selection.size(); ++snapshot_index)
		{
			feel_visible_Item& item = feel_visible[state.selection[snapshot_index]];
			trace_prepass_result& result = state.results[snapshot_index];
			result.target = item.O;
			result.target_spatial_identity = item.O ? static_cast<const ISpatial*>(item.O) : nullptr;
			result.target_cform = item.O ? item.O->CFORM() : nullptr;
			result.target_id = item.O ? item.O->ID() : u16(-1);
			if (item.O)
				state.result_indices.emplace(item.O, snapshot_index);
			result.bone_id = item.bone_id;
			result.target_position.set(0.f, 0.f, 0.f);
			result.target_point.set(0.f, 0.f, 0.f);
			result.local_point = item.cp_LP;
			result.cached_dynamic_blocker = item.last_dynamic_blocker;
			result.cached_dynamic_blocker_id = item.last_dynamic_blocker_id;
			result.start = position;
			result.direction.set(0.f, 0.f, 0.f);
			result.range = 0.f;
			result.visibility = 1.f;
			result.status = trace_prepass_status::unused;
			result.full_query_cache = FALSE;
			result.static_cache_reused = FALSE;
			result.static_query_performed = FALSE;
			result.fast_dynamic_fallback = FALSE;
			result.had_static_hit = FALSE;
			result.has_opaque_triangle = FALSE;
			result.opaque_triangle[0].set(0.f, 0.f, 0.f);
			result.opaque_triangle[1].set(0.f, 0.f, 0.f);
			result.opaque_triangle[2].set(0.f, 0.f, 0.f);

			if (!item.O || !result.target_cform)
			{
				result.status = trace_prepass_status::no_collision_form;
				continue;
			}

			result.target_position = item.O->Position();
			if (can_reuse_temporal(item, position, result.target_position, state.trace_time, vis_threshold))
			{
				result.visibility = item.temporal_visibility;
				result.status = trace_prepass_status::temporal_reuse;
				continue;
			}

			result.target_point = item.O->get_last_local_point_on_mesh(item.cp_LP, item.bone_id);
			Fvector predicted_offset;
			if (predict_target_offset(*item.O, predicted_offset))
			{
				result.target_position.add(predicted_offset);
				result.target_point.add(predicted_offset);
			}
			result.direction.sub(result.target_point, position);
			const float ray_distance = result.direction.magnitude();
			if (fis_zero(ray_distance))
			{
				result.status = trace_prepass_status::coincident_visible;
				continue;
			}

			result.range = ray_distance + .2f;
			if (result.range <= fuzzy_guaranteed)
			{
				result.status = trace_prepass_status::near_visible;
				continue;
			}

			result.direction.div(result.range);
			if (item.Cache_valid && item.Cache.similar(position, result.direction, result.range))
			{
				result.visibility = item.Cache_vis;
				if (result.visibility < vis_threshold)
				{
					float cached_u, cached_v, cached_range;
					if (CDB::TestRayTri(position, result.direction, item.Cache.verts,
						cached_u, cached_v, cached_range, false) && cached_range > 0.f && cached_range < result.range)
					{
						result.has_opaque_triangle = TRUE;
						result.opaque_triangle[0] = item.Cache.verts[0];
						result.opaque_triangle[1] = item.Cache.verts[1];
						result.opaque_triangle[2] = item.Cache.verts[2];
					}
					result.status = trace_prepass_status::cached_blocked;
					continue;
				}

				if (item.Cache.result)
					result.full_query_cache = TRUE;
				else
				{
					result.static_cache_reused = TRUE;
					result.had_static_hit = FALSE;
				}
				result.status = trace_prepass_status::pending;
				++state.pending_count;
				continue;
			}

			if (item.StaticCache_valid && item.StaticCache.similar(position, result.direction, result.range))
			{
				result.visibility = item.StaticCache_vis;
				result.static_cache_reused = TRUE;
				result.had_static_hit = item.StaticCache.result;
				result.has_opaque_triangle = item.StaticCache_has_opaque_triangle;
				if (result.has_opaque_triangle)
				{
					result.opaque_triangle[0] = item.StaticCache.verts[0];
					result.opaque_triangle[1] = item.StaticCache.verts[1];
					result.opaque_triangle[2] = item.StaticCache.verts[2];
				}
				if (result.visibility < vis_threshold)
				{
					result.status = trace_prepass_status::cached_blocked;
					continue;
				}
				if (result.visibility == vis_threshold)
				{
					result.status = trace_prepass_status::serial_fallback;
					continue;
				}
				result.status = trace_prepass_status::pending;
				++state.pending_count;
				continue;
			}

			float u, v, cached_range;
			if (CDB::TestRayTri(position, result.direction, item.Cache.verts, u, v, cached_range, false) && cached_range > 0.f && cached_range < result.range)
			{
				result.visibility = 0.f;
				result.has_opaque_triangle = TRUE;
				result.opaque_triangle[0] = item.Cache.verts[0];
				result.opaque_triangle[1] = item.Cache.verts[1];
				result.opaque_triangle[2] = item.Cache.verts[2];
				result.status = trace_prepass_status::cached_blocked;
				continue;
			}

			result.status = trace_prepass_status::pending;
			++state.pending_count;
		}

		const u32 minimum_rays = static_cast<u32>(_max(psAIAsyncPrepareMinRays, 2));
		if (state.pending_count < minimum_rays)
			return false;

		Fbox bounds;
		bounds.invalidate();
		bounds.modify(position);
		for (const trace_prepass_result& result : state.results)
		{
			if (result.status != trace_prepass_status::pending)
				continue;
			Fvector endpoint;
			endpoint.mad(result.start, result.direction, result.range);
			bounds.modify(endpoint);
		}
		bounds.grow(EPS_L);

		Fvector center;
		Fvector half_size;
		bounds.get_CD(center, half_size);
		half_size.x = _max(half_size.x, EPS_L);
		half_size.y = _max(half_size.y, EPS_L);
		half_size.z = _max(half_size.z, EPS_L);

		const u32 combined_mask = STYPE_COLLIDEABLE | STYPE_OBSTACLE | STYPE_VISIBLEFORAI;
		{
			vision_spatial_read_guard spatial_guard(g_SpatialSpace);
			g_SpatialSpace->q_box_unlocked(state.dynamic_spatial, 0, combined_mask, center, half_size);
			const ISpatial* owner_spatial = static_cast<const ISpatial*>(m_owner);
			const u32 primary_dynamic_flags = STYPE_COLLIDEABLE | STYPE_OBSTACLE;
			for (ISpatial* entry : state.dynamic_spatial)
			{
				if (!entry || entry == owner_spatial)
					continue;
				CObject* object = entry->dcast_CObject();
				bool ignored = false;
#ifdef SPATIAL_CHANGE
				ignored = object && (object->spatial.type & STYPE_FEELVISIONIGNORE);
#endif
				const bool visible_for_ai = !ignored && (entry->spatial.type & STYPE_VISIBLEFORAI);
				const bool has_collision_form = object && object->collidable.model;
				const bool primary_dynamic = !ignored && has_collision_form &&
					primary_dynamic_flags == (entry->spatial.type & primary_dynamic_flags) &&
					object->collidable.model->Type() == cftObject;
				if (!visible_for_ai && !primary_dynamic)
					continue;

				trace_dynamic_candidate_snapshot snapshot;
				snapshot.spatial_identity = entry;
				snapshot.object_identity = object;
				snapshot.sphere = entry->spatial.sphere;
				snapshot.object_id = object ? object->ID() : u16(-1);
				snapshot.primary_dynamic = primary_dynamic ? TRUE : FALSE;
				snapshot.visible_for_ai = visible_for_ai ? TRUE : FALSE;
				snapshot.has_collision_form = has_collision_form ? TRUE : FALSE;
				state.dynamic_candidates.push_back(snapshot);
				if (object)
					state.dynamic_object_indices.emplace(object, state.dynamic_candidates.size() - 1);
			}
		}

		state.dynamic_spatial.clear_not_free();
		const u64 pair_tests = u64(state.dynamic_candidates.size()) * u64(state.pending_count);
		if (state.dynamic_candidates.size() > 512 || pair_tests > 32768)
			return false;

		state.static_model = g_pGameLevel->ObjectSpace.GetStaticModel();
		if (!state.static_model)
			return false;
		// Resolve a possible asynchronous level-CDB build on the main thread. In
		// normal gameplay this is only an acquire-load; the worker then performs a
		// strictly read-only query over an already published immutable model.
		state.static_model->syncronize();
		state.material_transparency = m_static_material_transparency;
		state.static_triangles = g_pGameLevel->ObjectSpace.GetStaticTris();
		state.static_vertices = g_pGameLevel->ObjectSpace.GetStaticVerts();
		return state.static_triangles && state.static_vertices;
	}

	void Vision::feel_vision_prepare_async(Fvector& position, const float vis_threshold, const bool allow_parallel,
		const u32 max_trace_items, const CObject* priority_target)
	{
#ifdef DEBUG
		return;
#else
		VERIFY(!xr_jobs::is_worker_thread());
		async_prepare_state* state = async_state(this);
		if (!state)
			return;

		state->deferred_prepare = false;
		if (state->task.ready())
		{
			state->task.retire_ready();
		}
		if (!psAIAsyncPrepare || !allow_parallel || xr_jobs::worker_count() == 0 ||
			state->task.status() != AIAsyncPrepare::TaskStatus::Idle)
			return;

		if (!build_async_trace_snapshot(*state, position, vis_threshold, max_trace_items, priority_target))
			return;

		if (!state->task.submit(&Vision::async_trace_worker, state))
			state->discarded = true;
#endif
	}

	void Vision::feel_vision_defer_async(const u32 delay_ms)
	{
#ifndef DEBUG
		VERIFY(!xr_jobs::is_worker_thread());
		async_prepare_state* state = async_state(this);
		if (!state || !psAIAsyncPrepare)
			return;
		u32 adjusted_delay_ms = delay_ms;
		if (state->deferred_prepare_adjustment_ms > 0)
			adjusted_delay_ms += static_cast<u32>(state->deferred_prepare_adjustment_ms);
		else
			adjusted_delay_ms -= _min(adjusted_delay_ms,
				static_cast<u32>(-state->deferred_prepare_adjustment_ms));
		state->deferred_prepare_time = Device.dwTimeGlobal + adjusted_delay_ms;
		state->deferred_prepare = true;
#endif
	}

	bool Vision::feel_vision_async_ready(const u32 max_age_ms) const
	{
#ifdef DEBUG
		return false;
#else
		VERIFY(!xr_jobs::is_worker_thread());
		async_prepare_state* state = async_state(this);
		if (!psAIAsyncPrepare || !state || state->discarded || !state->trace_time || !state->task.ready())
			return false;
		return !max_age_ms || Device.dwTimeGlobal - state->trace_time <= max_age_ms;
#endif
	}

	bool Vision::feel_vision_discard_stale_async(const u32 max_age_ms)
	{
#ifdef DEBUG
		return false;
#else
		VERIFY(!xr_jobs::is_worker_thread());
		async_prepare_state* state = async_state(this);
		if (!psAIAsyncPrepare || !state || state->discarded || !state->trace_time || !state->task.ready())
			return false;

		const u32 age_ms = Device.dwTimeGlobal - state->trace_time;
		if (!max_age_ms || age_ms <= max_age_ms)
			return false;

		// A completed snapshot older than the consume window is never allowed back
		// into validation. Retire it on the owner thread, account all completed rays
		// as unused, and let the unchanged serial path remain authoritative.
		state->task.retire_ready();
		state->deferred_prepare = false;
		return true;
#endif
	}

	void Vision::feel_vision_prepare_async_if_due(Fvector& position, const float vis_threshold,
		const bool allow_parallel, const u32 max_trace_items, const CObject* priority_target)
	{
#ifndef DEBUG
		VERIFY(!xr_jobs::is_worker_thread());
		async_prepare_state* state = async_state(this);
		if (!state || !state->deferred_prepare || Device.dwTimeGlobal < state->deferred_prepare_time)
			return;
		state->deferred_prepare = false;
		feel_vision_prepare_async(position, vis_threshold, allow_parallel, max_trace_items, priority_target);
#endif
	}

	void Vision::feel_vision_update(CObject* parent, Fvector& P, float dt, float vis_threshold,
	                               bool allow_parallel)
	{
		feel_vision_update(parent, P, dt, vis_threshold, allow_parallel, 0);
	}

	void Vision::feel_vision_update(CObject* parent, Fvector& P, float dt, float vis_threshold,
	                               bool allow_parallel, u32 max_trace_items)
	{
		feel_vision_update(parent, P, dt, vis_threshold, allow_parallel, max_trace_items, nullptr);
	}

	void Vision::feel_vision_update(CObject* parent, Fvector& P, float dt, float vis_threshold,
	                               bool allow_parallel, u32 max_trace_items, const CObject* priority_target)
	{
		if (!seen.empty())
		{
			xr_vector<CObject*>::iterator end = std::remove(seen.begin(), seen.end(), parent);
			seen.resize(end - seen.begin());
		}

		// Synchronize the two sorted candidate sets in one merge pass. New objects are
		// appended after the surviving old objects, exactly as in the legacy add-then-delete path.
		diff.clear_not_free();
		removed.clear_not_free();

		xr_vector<CObject*>::const_iterator seen_it = seen.begin();
		xr_vector<CObject*>::const_iterator seen_end = seen.end();
		xr_vector<CObject*>::const_iterator query_it = query.begin();
		xr_vector<CObject*>::const_iterator query_end = query.end();

		const std::less<CObject*> object_less;
		while (seen_it != seen_end && query_it != query_end)
		{
			if (object_less(*seen_it, *query_it))
				diff.push_back(*seen_it++);
			else if (object_less(*query_it, *seen_it))
				removed.push_back(*query_it++);
			else
			{
				++seen_it;
				++query_it;
			}
		}

		for (; seen_it != seen_end; ++seen_it)
			diff.push_back(*seen_it);
		for (; query_it != query_end; ++query_it)
			removed.push_back(*query_it);

		if (!removed.empty())
		{
			u32 write_index = 0;
			for (u32 read_index = 0; read_index < feel_visible.size(); ++read_index)
			{
				const feel_visible_Item& source = feel_visible[read_index];
				if (std::binary_search(removed.begin(), removed.end(), source.O, object_less))
					continue;

				if (write_index != read_index)
					feel_visible[write_index] = source;
				++write_index;
			}

			feel_visible.resize(write_index);
			rebuild_feel_visible_indices();
		}

		for (u32 index = 0; index < diff.size(); ++index)
			o_new(diff[index]);

		query = seen;
		o_trace(P, dt, vis_threshold, allow_parallel, max_trace_items, priority_target);
	}

	void Vision::apply_trace_visibility(feel_visible_Item& item, float visibility, float dt, float vis_threshold)
	{
		if (visibility < vis_threshold)
		{
			item.fuzzy -= fuzzy_update_novis * dt;
			clamp(item.fuzzy, -.5f, 1.f);
			item.cp_LP = item.O->get_new_local_point_on_mesh(item.bone_id);
			return;
		}

		item.fuzzy += fuzzy_update_vis * dt;
		clamp(item.fuzzy, -.5f, 1.f);
	}

	bool Vision::can_reuse_temporal(const feel_visible_Item& item, const Fvector& owner_position,
		const Fvector& target_position, u32 now, float vis_threshold) const
	{
		if (!item.temporal_valid || item.last_dynamic_blocker)
			return false;

		const bool blocked = item.temporal_visibility < vis_threshold;
		const int ttl_ms = blocked ? psAI_VisionTemporalBlockedMS : psAI_VisionTemporalVisibleMS;
		if (ttl_ms <= 0 || now - item.temporal_time > u32(ttl_ms))
			return false;

		if (blocked && !item.temporal_static_opaque)
			return false;

		const float epsilon = _max(psAI_VisionTemporalMoveEpsilon, 0.f);
		const float epsilon_sqr = epsilon * epsilon;
		return owner_position.distance_to_sqr(item.temporal_owner_position) <= epsilon_sqr &&
			target_position.distance_to_sqr(item.temporal_target_position) <= epsilon_sqr;
	}

	void Vision::record_temporal_result(feel_visible_Item& item, const Fvector& owner_position,
		const Fvector& target_position, float visibility, bool static_opaque_proof)
	{
		item.temporal_owner_position.set(owner_position);
		item.temporal_target_position.set(target_position);
		item.temporal_visibility = visibility;
		item.temporal_time = Device.dwTimeGlobal;
		item.temporal_valid = TRUE;
		item.temporal_static_opaque = static_opaque_proof ? TRUE : FALSE;
	}

	void Vision::trace_item_serial(feel_visible_Item& item, const Fvector& position, float dt, float vis_threshold)
	{
		if (!item.O || !item.O->CFORM())
		{
			item.fuzzy = -1.f;
			item.Cache_valid = FALSE;
			item.temporal_valid = FALSE;
			clear_dynamic_blocker(item);
			return;
		}

		item.cp_LR_dst = item.O->Position();
		item.cp_LR_src = position;
		if (can_reuse_temporal(item, position, item.cp_LR_dst, Device.dwTimeGlobal, vis_threshold))
		{
			apply_trace_visibility(item, item.temporal_visibility, dt, vis_threshold);
			return;
		}
		item.cp_LAST = item.O->get_last_local_point_on_mesh(item.cp_LP, item.bone_id);

		Fvector direction;
		direction.sub(item.cp_LAST, position);
		const float ray_distance = direction.magnitude();
		if (fis_zero(ray_distance))
		{
			item.fuzzy = 1.f;
			clear_dynamic_blocker(item);
			record_temporal_result(item, position, item.cp_LR_dst, 1.f);
			return;
		}

		const float range = ray_distance + .2f;
		if (range <= fuzzy_guaranteed)
		{
			clear_dynamic_blocker(item);
			apply_trace_visibility(item, 1.f, dt, vis_threshold);
			record_temporal_result(item, position, item.cp_LR_dst, 1.f);
			return;
		}

		direction.div(range);
		collide::ray_defs ray(position, direction, range, CDB::OPT_CULL,
			collide::rq_target(collide::rqtStatic | collide::rqtObject | collide::rqtObstacle));
		SFeelParam feel_params(this, &item, vis_threshold, m_static_material_transparency);
		bool candidates_ready = false;
		const u32 candidate_mask = STYPE_COLLIDEABLE | STYPE_OBSTACLE | STYPE_VISIBLEFORAI;
		auto prepare_candidates = [&]()
		{
			if (candidates_ready)
				return;
			r_spatial.clear_not_free();
			g_SpatialSpace->q_ray_or(r_spatial, 0, candidate_mask, position, direction, range);
			candidates_ready = true;
		};

		const bool full_cache_similar = item.Cache_valid && item.Cache.similar(position, direction, range);
		if (full_cache_similar && item.Cache.result)
		{
			feel_params.vis = item.Cache_vis;
		}
		else if (full_cache_similar)
		{
			// A valid no-hit result proves that the immutable static corridor was clear.
			// Recheck dynamic broadphase and the target CFORM without repeating static CDB.
			feel_params.vis = item.Cache_vis;
			prepare_candidates();

			bool third_party_primary_dynamic = false;
			const u32 primary_dynamic_flags = STYPE_COLLIDEABLE | STYPE_OBSTACLE;
			for (xr_vector<ISpatial*>::const_iterator spatial = r_spatial.begin(); spatial != r_spatial.end(); ++spatial)
			{
				ISpatial* entry = *spatial;
				if (!entry || primary_dynamic_flags != (entry->spatial.type & primary_dynamic_flags) ||
					entry == m_owner || entry == item.O)
					continue;

				CObject* object = entry->dcast_CObject();
#ifdef SPATIAL_CHANGE
				if (object && (object->spatial.type & STYPE_FEELVISIONIGNORE))
					continue;
#endif
				if (object && object->collidable.model && object->collidable.model->Type() == cftObject)
				{
					third_party_primary_dynamic = true;
					break;
				}
			}

			if (!third_party_primary_dynamic)
			{
				const bool target_hit = query_target_materials(item, ray, feel_params.vis, vis_threshold);
				item.Cache_vis = feel_params.vis;
				item.Cache.set(position, direction, range, target_hit);
			}
			else
			{
#ifdef SPATIAL_CHANGE
				const BOOL hit = g_pGameLevel->ObjectSpace.RayQueryCandidates(RQR, ray, r_spatial,
					feel_vision_callback, &feel_params, feel_vision_test_callback, const_cast<CObject*>(m_owner));
#else
				const BOOL hit = g_pGameLevel->ObjectSpace.RayQueryCandidates(RQR, ray, r_spatial,
					feel_vision_callback, &feel_params, nullptr, const_cast<CObject*>(m_owner));
#endif
				item.Cache_vis = feel_params.vis;
				item.Cache.set(position, direction, range, hit);
			}
		}
		else
		{
			float u, v, cached_range;
			if (CDB::TestRayTri(position, direction, item.Cache.verts, u, v, cached_range, false) && cached_range > 0.f && cached_range < range)
			{
				feel_params.vis = 0.f;
			}
			else
			{
				VERIFY(!fis_zero(ray.dir.magnitude()));
				prepare_candidates();
#ifdef SPATIAL_CHANGE
				const BOOL hit = g_pGameLevel->ObjectSpace.RayQueryCandidates(RQR, ray, r_spatial,
					feel_vision_callback, &feel_params, feel_vision_test_callback, const_cast<CObject*>(m_owner));
#else
				const BOOL hit = g_pGameLevel->ObjectSpace.RayQueryCandidates(RQR, ray, r_spatial,
					feel_vision_callback, &feel_params, nullptr, const_cast<CObject*>(m_owner));
#endif
				item.Cache_vis = feel_params.vis;
				item.Cache.set(position, direction, range, hit);
			}
		}
		item.Cache_valid = TRUE;

		ray.flags = CDB::OPT_ONLYFIRST;
		bool collision_found = false;
		if (feel_params.dynamic_blocker)
		{
			// This is the same class of blocker selected by the legacy r_spatial
			// fallback below, but its collision was already produced by the query.
			item.last_dynamic_blocker = feel_params.dynamic_blocker;
			item.last_dynamic_blocker_id = feel_params.dynamic_blocker->ID();
			collision_found = true;
		}
		else
			collision_found = test_cached_dynamic_blocker(item, ray);
		if (!collision_found)
		{
			prepare_candidates();

			for (xr_vector<ISpatial*>::const_iterator spatial = r_spatial.begin(); spatial != r_spatial.end(); ++spatial)
			{
				if (!*spatial || 0 == ((*spatial)->spatial.type & STYPE_VISIBLEFORAI))
					continue;
				if (*spatial == m_owner || *spatial == item.O)
					continue;

				CObject* object = (*spatial)->dcast_CObject();
#ifdef SPATIAL_CHANGE
				if (object && (object->spatial.type & STYPE_FEELVISIONIGNORE))
					continue;
#endif
				if (object && object->collidable.model)
				{
					RQR.r_clear();
					if (!object->collidable.model->_RayQuery(ray, RQR))
						continue;

					item.last_dynamic_blocker = object;
					item.last_dynamic_blocker_id = object->ID();
				}
				else
				{
					clear_dynamic_blocker(item);
				}

				collision_found = true;
				break;
			}

			if (!collision_found)
				clear_dynamic_blocker(item);
		}

		if (collision_found)
			feel_params.vis = 0.f;

		apply_trace_visibility(item, feel_params.vis, dt, vis_threshold);
		record_temporal_result(item, position, item.cp_LR_dst, feel_params.vis);
	}

	bool Vision::query_target_materials(feel_visible_Item& item, const collide::ray_defs& ray, float& visibility,
		float vis_threshold)
	{
		CObject* target = item.O;
		if (!target || !target->collidable.model || target->spatial.space != g_SpatialSpace ||
			!target->spatial.node_ptr)
			return false;

		const u32 dynamic_flags = STYPE_COLLIDEABLE | STYPE_OBSTACLE;
		if (dynamic_flags != (target->spatial.type & dynamic_flags))
			return false;
#ifdef SPATIAL_CHANGE
		if (target->spatial.type & STYPE_FEELVISIONIGNORE)
			return false;
#endif
		if (target->collidable.model->Type() != cftObject || !vision_ray_intersects_spatial(*target, ray))
			return false;

		RQR.r_clear();
		if (!target->collidable.model->_RayQuery(ray, RQR))
			return false;

		if (RQR.r_count() > 1)
			RQR.r_sort();
		SFeelParam feel_params(this, &item, vis_threshold, m_static_material_transparency);
		feel_params.vis = visibility;
		for (collide::rq_result* hit = RQR.r_begin(); hit != RQR.r_end(); ++hit)
		{
			if (!feel_vision_callback(*hit, &feel_params))
				break;
		}
		visibility = feel_params.vis;
		return true;
	}

	bool Vision::can_use_parallel_trace_prepass(bool allow_parallel, u32 job_count) const
	{
#ifdef DEBUG
		return false;
#else
		// The deferred scheduler itself normally occupies one pool worker. The job
		// system's wait() is cooperative and explicitly supports nested fork/join,
		// so that worker can safely fan independent static LOS rays out to the rest
		// of the pool and help execute them while waiting.
		if (!allow_parallel || xr_jobs::available_thread_count() < 2)
			return false;

		// Keep the workload threshold explicit and runtime-tunable. Captures showed
		// that forcing 4-10-ray observer batches through snapshot/fork-join can cost
		// more than their static CDB work, so the shipping default remains 12.
		const u32 minimum_jobs = static_cast<u32>(_max(psAI_VisionParallelMinRays, 2));
		return job_count >= minimum_jobs;
#endif
	}

	BOOL Vision::trace_spatial_filter(ISpatial* spatial, LPVOID context)
	{
		if (!spatial || !context)
			return FALSE;

		trace_spatial_filter_data& data = *static_cast<trace_spatial_filter_data*>(context);
		const ISpatial* owner_spatial = static_cast<const ISpatial*>(data.vision->m_owner);
		const ISpatial* target_spatial = static_cast<const ISpatial*>(data.target);
		if (spatial == owner_spatial || spatial == target_spatial)
			return FALSE;

		CObject* object = spatial->dcast_CObject();
#ifdef SPATIAL_CHANGE
		if (object && (object->spatial.type & STYPE_FEELVISIONIGNORE))
			return FALSE;
#endif

		if (spatial->spatial.type & STYPE_VISIBLEFORAI)
		{
			// The legacy secondary visibility pass treats any accepted non-owner/non-target
			// spatial entry as potentially blocking, including malformed entries without CFORM.
			return TRUE;
		}

		if (!data.include_primary_dynamic ||
			data.primary_dynamic_flags != (spatial->spatial.type & data.primary_dynamic_flags))
			return FALSE;

		if (!object || !object->collidable.model)
			return FALSE;

		return object->collidable.model->Type() == cftObject ? TRUE : FALSE;
	}

	bool Vision::has_potential_dynamic_locked(const trace_prepass_result& result) const
	{
		if (!g_SpatialSpace)
			return false;

		trace_spatial_filter_data filter_data;
		filter_data.vision = this;
		filter_data.target = result.target;
		filter_data.primary_dynamic_flags = STYPE_COLLIDEABLE | STYPE_OBSTACLE;
		// Even a cached full first-pass result must be invalidated by a newly entered
		// third-party dynamic object. The target itself remains excluded by the filter.
		filter_data.include_primary_dynamic = TRUE;

		const u32 combined_mask = filter_data.primary_dynamic_flags | STYPE_VISIBLEFORAI;
		return !!g_SpatialSpace->q_ray_any_filtered_or_unlocked(combined_mask, result.start, result.direction,
			result.range, &Vision::trace_spatial_filter, &filter_data);
	}

	bool Vision::has_potential_dynamic(const trace_prepass_result& result) const
	{
		return m_trace_dynamic_snapshot_valid ?
			has_potential_dynamic_snapshot(result) : has_potential_dynamic_locked(result);
	}

	void Vision::process_trace_prepass(trace_prepass_result& result, float vis_threshold)
	{
		const bool cached_dynamic_candidate = m_trace_dynamic_snapshot_valid ?
			has_cached_dynamic_candidate_snapshot(result) : has_cached_dynamic_candidate_locked(result);
		if (result.visibility >= vis_threshold && cached_dynamic_candidate)
		{
			result.fast_dynamic_fallback = TRUE;
			result.status = trace_prepass_status::serial_fallback;
			return;
		}

		if (result.full_query_cache)
		{
			if (result.visibility >= vis_threshold && has_potential_dynamic(result))
				result.status = trace_prepass_status::serial_fallback;
			else
				result.status = trace_prepass_status::ready;
			return;
		}

		if (result.static_cache_reused)
		{
			if (result.visibility >= vis_threshold && has_potential_dynamic(result))
				result.status = trace_prepass_status::serial_fallback;
			else
				result.status = trace_prepass_status::ready;
			return;
		}

		// Dynamic broadphase is cheaper than a full static CDB traversal and prevents
		// duplicate static work when this target must use the unchanged serial path.
		if (has_potential_dynamic(result))
		{
			result.status = trace_prepass_status::serial_fallback;
			return;
		}

		struct static_query_workspace
		{
			CDB::COLLIDER collider;
			xr_vector<const CDB::RESULT*> ordered_hits;

			static_query_workspace()
			{
				collider.r_reserve(32);
				ordered_hits.reserve(32);
			}
		};
		thread_local static_query_workspace workspace;

		workspace.collider.ray_options(CDB::OPT_CULL);
		workspace.collider.ray_query(g_pGameLevel->ObjectSpace.GetStaticModel(), result.start, result.direction,
			result.range);
		result.static_query_performed = TRUE;

		workspace.ordered_hits.clear_not_free();
		const u32 static_hit_count = workspace.collider.r_count();
		if (static_hit_count)
		{
			CDB::RESULT* static_hits = workspace.collider.r_begin();
			for (u32 hit = 0; hit < static_hit_count; ++hit)
				workspace.ordered_hits.push_back(static_hits + hit);
		}
		if (workspace.ordered_hits.size() > 1)
		{
			std::sort(workspace.ordered_hits.begin(), workspace.ordered_hits.end(),
				[](const CDB::RESULT* left, const CDB::RESULT* right) { return left->range < right->range; });
		}

		result.had_static_hit = static_hit_count ? TRUE : FALSE;
		result.visibility = 1.f;
		for (xr_vector<const CDB::RESULT*>::const_iterator hit = workspace.ordered_hits.begin();
			hit != workspace.ordered_hits.end(); ++hit)
		{
			const float transparency = static_material_transparency((*hit)->id);
			result.visibility *= transparency;
			if (!result.has_opaque_triangle && fis_zero(transparency))
			{
				result.has_opaque_triangle = TRUE;
				const CDB::TRI* triangle = g_pGameLevel->ObjectSpace.GetStaticTris() + (*hit)->id;
				const Fvector* vertices = g_pGameLevel->ObjectSpace.GetStaticVerts();
				result.opaque_triangle[0].set(vertices[triangle->verts[0]]);
				result.opaque_triangle[1].set(vertices[triangle->verts[1]]);
				result.opaque_triangle[2].set(vertices[triangle->verts[2]]);
			}

			if (result.visibility <= vis_threshold)
				break;
		}

		// Equality is visible in the fuzzy commit but stops the legacy material callback.
		// Preserve that edge case through the exact serial ordering of static/target hits.
		result.status = result.visibility == vis_threshold ? trace_prepass_status::serial_fallback :
			trace_prepass_status::ready;
	}

	void Vision::trace_prepass_worker(void* context)
	{
		trace_prepass_workload& workload = *static_cast<trace_prepass_workload*>(context);
		for (;;)
		{
			const LONG claimed = InterlockedExchangeAdd(&workload.next_job, static_cast<LONG>(workload.chunk_size));
			if (claimed < 0 || static_cast<u32>(claimed) >= workload.job_count)
				return;

			const u32 begin = static_cast<u32>(claimed);
			const u32 end = _min(begin + workload.chunk_size, workload.job_count);
			if (workload.use_dynamic_snapshot)
			{
				for (u32 job = begin; job < end; ++job)
				{
					const u32 result_index = (*workload.jobs)[job];
					workload.vision->process_trace_prepass((*workload.results)[result_index],
						workload.visibility_threshold);
				}
			}
			else
			{
				vision_spatial_read_guard spatial_guard(g_SpatialSpace);
				for (u32 job = begin; job < end; ++job)
				{
					const u32 result_index = (*workload.jobs)[job];
					workload.vision->process_trace_prepass((*workload.results)[result_index],
						workload.visibility_threshold);
				}
			}
		}
	}

	void Vision::async_trace_worker(void* context)
	{
		async_prepare_state& state = *static_cast<async_prepare_state*>(context);

		auto intersects = [](const Fsphere& sphere, const trace_prepass_result& result)
		{
			collide::ray_defs ray(result.start, result.direction, result.range, CDB::OPT_ONLYFIRST,
				collide::rq_target(collide::rqtObject | collide::rqtObstacle));
			return vision_ray_intersects_sphere(sphere, ray);
		};

		auto has_cached_dynamic = [&](const trace_prepass_result& result)
		{
			if (!result.cached_dynamic_blocker)
				return false;
			const auto cached = state.dynamic_object_indices.find(result.cached_dynamic_blocker);
			if (cached == state.dynamic_object_indices.end() || cached->second >= state.dynamic_candidates.size())
				return false;

			const ISpatial* target_spatial = result.target_spatial_identity;
			const trace_dynamic_candidate_snapshot& candidate = state.dynamic_candidates[cached->second];
			if (candidate.spatial_identity != target_spatial &&
				candidate.object_identity == result.cached_dynamic_blocker &&
				candidate.object_id == result.cached_dynamic_blocker_id && candidate.visible_for_ai &&
				candidate.has_collision_form && intersects(candidate.sphere, result))
				return true;

			for (u32 index = 0; index < state.dynamic_candidates.size(); ++index)
			{
				if (index == cached->second)
					continue;
				const trace_dynamic_candidate_snapshot& duplicate = state.dynamic_candidates[index];
				if (duplicate.spatial_identity == target_spatial ||
					duplicate.object_identity != result.cached_dynamic_blocker ||
					duplicate.object_id != result.cached_dynamic_blocker_id || !duplicate.visible_for_ai ||
					!duplicate.has_collision_form)
					continue;
				if (intersects(duplicate.sphere, result))
					return true;
			}
			return false;
		};

		auto has_potential_dynamic = [&](const trace_prepass_result& result)
		{
			const ISpatial* target_spatial = result.target_spatial_identity;
			for (const trace_dynamic_candidate_snapshot& candidate : state.dynamic_candidates)
			{
				if (candidate.spatial_identity == target_spatial)
					continue;
				if (!candidate.visible_for_ai && !candidate.primary_dynamic)
					continue;
				if (intersects(candidate.sphere, result))
					return true;
			}
			return false;
		};

		struct static_query_workspace
		{
			CDB::COLLIDER collider;
			xr_vector<const CDB::RESULT*> ordered_hits;

			static_query_workspace()
			{
				collider.r_reserve(32);
				ordered_hits.reserve(32);
			}
		};
		thread_local static_query_workspace workspace;

		for (trace_prepass_result& result : state.results)
		{
			if (result.status != trace_prepass_status::pending)
				continue;

			if (result.visibility >= state.visibility_threshold && has_cached_dynamic(result))
			{
				result.fast_dynamic_fallback = TRUE;
				result.status = trace_prepass_status::serial_fallback;
				continue;
			}

			if (result.full_query_cache || result.static_cache_reused)
			{
				result.status = result.visibility >= state.visibility_threshold && has_potential_dynamic(result) ?
					trace_prepass_status::serial_fallback : trace_prepass_status::ready;
				continue;
			}

			if (has_potential_dynamic(result))
			{
				result.status = trace_prepass_status::serial_fallback;
				continue;
			}

			workspace.collider.ray_options(CDB::OPT_CULL);
			workspace.collider.ray_query(state.static_model, result.start, result.direction, result.range);
			result.static_query_performed = TRUE;
			workspace.ordered_hits.clear_not_free();
			const u32 static_hit_count = workspace.collider.r_count();
			if (static_hit_count)
			{
				CDB::RESULT* static_hits = workspace.collider.r_begin();
				for (u32 hit = 0; hit < static_hit_count; ++hit)
					workspace.ordered_hits.push_back(static_hits + hit);
			}
			if (workspace.ordered_hits.size() > 1)
			{
				std::sort(workspace.ordered_hits.begin(), workspace.ordered_hits.end(),
					[](const CDB::RESULT* left, const CDB::RESULT* right) { return left->range < right->range; });
			}

			result.had_static_hit = static_hit_count ? TRUE : FALSE;
			result.visibility = 1.f;
			bool material_snapshot_valid = true;
			for (const CDB::RESULT* hit : workspace.ordered_hits)
			{
				const CDB::TRI& triangle = state.static_triangles[hit->id];
				if (!state.material_transparency || triangle.material >= state.material_transparency->size())
				{
					material_snapshot_valid = false;
					break;
				}

				const float transparency = (*state.material_transparency)[triangle.material];
				result.visibility *= transparency;
				if (!result.has_opaque_triangle && fis_zero(transparency))
				{
					result.has_opaque_triangle = TRUE;
					result.opaque_triangle[0] = state.static_vertices[triangle.verts[0]];
					result.opaque_triangle[1] = state.static_vertices[triangle.verts[1]];
					result.opaque_triangle[2] = state.static_vertices[triangle.verts[2]];
				}
				if (result.visibility <= state.visibility_threshold)
					break;
			}

			if (!material_snapshot_valid || result.visibility == state.visibility_threshold)
				result.status = trace_prepass_status::serial_fallback;
			else
				result.status = trace_prepass_status::ready;
		}
	}

	void Vision::commit_trace_result(feel_visible_Item& item, trace_prepass_result& result,
		const Fvector& position, const float dt, const float vis_threshold)
	{
		if (!item.O || item.O != result.target || item.O->ID() != result.target_id || !item.O->CFORM())
		{
			item.fuzzy = -1.f;
			item.Cache_valid = FALSE;
			item.StaticCache_valid = FALSE;
			item.temporal_valid = FALSE;
			clear_dynamic_blocker(item);
			return;
		}

		const float snapshot_epsilon = _max(psAI_VisionTemporalMoveEpsilon * 2.f, .01f);
		if (result.status != trace_prepass_status::no_collision_form &&
			result.status != trace_prepass_status::temporal_reuse &&
			item.O->Position().distance_to_sqr(result.target_position) > snapshot_epsilon * snapshot_epsilon)
		{
			trace_item_serial(item, position, dt, vis_threshold);
			return;
		}

		if (result.static_query_performed)
		{
			item.StaticCache_vis = result.visibility;
			item.StaticCache_valid = TRUE;
			item.StaticCache_has_opaque_triangle = result.has_opaque_triangle;
			item.StaticCache.set(result.start, result.direction, result.range, result.had_static_hit);
			if (result.has_opaque_triangle)
			{
				item.StaticCache.verts[0].set(result.opaque_triangle[0]);
				item.StaticCache.verts[1].set(result.opaque_triangle[1]);
				item.StaticCache.verts[2].set(result.opaque_triangle[2]);
			}
		}

		switch (result.status)
		{
		case trace_prepass_status::no_collision_form:
			item.fuzzy = -1.f;
			item.Cache_valid = FALSE;
			item.temporal_valid = FALSE;
			clear_dynamic_blocker(item);
			break;
		case trace_prepass_status::coincident_visible:
			item.fuzzy = 1.f;
			clear_dynamic_blocker(item);
			record_temporal_result(item, position, result.target_position, 1.f);
			break;
		case trace_prepass_status::near_visible:
			clear_dynamic_blocker(item);
			apply_trace_visibility(item, 1.f, dt, vis_threshold);
			record_temporal_result(item, position, result.target_position, 1.f);
			break;
		case trace_prepass_status::temporal_reuse:
			apply_trace_visibility(item, result.visibility, dt, vis_threshold);
			break;
		case trace_prepass_status::cached_blocked:
			apply_trace_visibility(item, 0.f, dt, vis_threshold);
			record_temporal_result(item, position, result.target_position, 0.f, result.has_opaque_triangle);
			break;
		case trace_prepass_status::ready:
			{
				float visibility = result.visibility;
				if (!result.full_query_cache)
				{
					collide::ray_defs ray(result.start, result.direction, result.range, CDB::OPT_CULL,
						collide::rq_target(collide::rqtStatic | collide::rqtObject | collide::rqtObstacle));
					const bool target_hit = visibility > vis_threshold &&
						query_target_materials(item, ray, visibility, vis_threshold);
					if (result.has_opaque_triangle)
					{
						item.Cache.verts[0].set(result.opaque_triangle[0]);
						item.Cache.verts[1].set(result.opaque_triangle[1]);
						item.Cache.verts[2].set(result.opaque_triangle[2]);
					}
					const BOOL had_first_pass_hit = result.had_static_hit || target_hit;
					item.Cache_vis = visibility;
					item.Cache_valid = TRUE;
					item.Cache.set(result.start, result.direction, result.range, had_first_pass_hit);
				}
				clear_dynamic_blocker(item);
				apply_trace_visibility(item, visibility, dt, vis_threshold);
				record_temporal_result(item, position, result.target_position, visibility,
					result.has_opaque_triangle);
				break;
			}
		case trace_prepass_status::serial_fallback:
		default:
			trace_item_serial(item, position, dt, vis_threshold);
			break;
		}
	}

	void Vision::commit_trace_results(const Fvector& position, const float dt, const float vis_threshold)
	{
		// Both the legacy fork/join path and async consumption commit in the
		// observer's original scheduler callback and deterministic item order.
		const u32 item_count = _min(u32(feel_visible.size()), u32(m_trace_prepass_results.size()));
		for (u32 index = 0; index < item_count; ++index)
			commit_trace_result(feel_visible[index], m_trace_prepass_results[index], position, dt, vis_threshold);
	}

	bool Vision::try_consume_async_trace(const Fvector& position, const float dt, const float vis_threshold,
		const u32 max_trace_items, const CObject* priority_target)
	{
		async_prepare_state* state = async_state(this);
		if (!state || state->task.status() == AIAsyncPrepare::TaskStatus::Idle)
			return false;

		const u32 item_count = feel_visible.size();
		const u32 trace_count = max_trace_items ? _min(max_trace_items, item_count) : item_count;
		if (!state->task.ready())
		{
			state->discarded = true;
			return false;
		}

		if (state->discarded)
		{
			state->task.retire_ready();
			return false;
		}

		const u32 legacy_cursor = m_trace_prepass_jobs.empty() || !item_count ? 0 :
			m_trace_prepass_jobs[0] % item_count;
		m_trace_prepass_jobs.clear_not_free();
		u32 next_cursor = 0;
		const bool limited = trace_count < item_count;
		if (limited)
		{
			const u32 cursor = legacy_cursor;
			u32 visited = 0;
			if (priority_target)
			{
				const object_index_cache::const_iterator priority = m_feel_visible_indices.find(priority_target->ID());
				if (priority != m_feel_visible_indices.end() && priority->second < item_count)
					m_trace_prepass_jobs.push_back(priority->second);
			}
			while (m_trace_prepass_jobs.size() < trace_count && visited < item_count)
			{
				const u32 index = (cursor + visited) % item_count;
				++visited;
				if (feel_visible[index].O == priority_target)
					continue;
				m_trace_prepass_jobs.push_back(index);
			}
			next_cursor = (cursor + visited) % item_count;
		}
		else
		{
			m_trace_prepass_jobs.resize(item_count);
			for (u32 index = 0; index < item_count; ++index)
				m_trace_prepass_jobs[index] = index;
		}

		const u32 snapshot_age_ms = state->trace_time ? Device.dwTimeGlobal - state->trace_time : 0;
		const s32 desired_lead_ms = _max(psAIAsyncPrepareLeadMS, 0);
		if (snapshot_age_ms && snapshot_age_ms <= 250)
		{
			state->observed_commit_age_ms = state->observed_commit_age_ms ?
				(state->observed_commit_age_ms * 3u + snapshot_age_ms + 2u) / 4u : snapshot_age_ms;
		}
		if (desired_lead_ms > 0 && snapshot_age_ms && snapshot_age_ms <= 250)
		{
			// The scheduler's distance scaling, jitter and budget deferrals make its
			// nominal interval only an estimate. Learn the observed stage-0 -> stage-1
			// error per observer and submit later when snapshots consistently arrive
			// stale. Retain at least four milliseconds of predicted worker slack.
			const s32 latest_safe_adjustment = _max(desired_lead_ms - 4, 0);
			const s32 timing_error = clampr(static_cast<s32>(snapshot_age_ms) - desired_lead_ms,
				-desired_lead_ms, latest_safe_adjustment);
			// timing_error is the remaining error after the current adjustment has
			// already affected this snapshot. Apply a quarter of that residual;
			// averaging the absolute adjustment with the residual would converge at
			// only half of the required correction.
			state->deferred_prepare_adjustment_ms = clampr(
				state->deferred_prepare_adjustment_ms + timing_error / 4,
				-desired_lead_ms, latest_safe_adjustment);
		}
		const float snapshot_epsilon = _max(psAI_VisionTemporalMoveEpsilon * 2.f, .01f);
		const float snapshot_epsilon_sqr = snapshot_epsilon * snapshot_epsilon;
		bool batch_failure = false;
		if (_abs(state->visibility_threshold - vis_threshold) > EPS_S)
			batch_failure = true;
		else if (position.distance_to_sqr(state->owner_position) > snapshot_epsilon_sqr)
			batch_failure = true;
		for (const u32 item_index : m_trace_prepass_jobs)
		{
			feel_visible_Item& item = feel_visible[item_index];
			trace_prepass_result* prepared = nullptr;
			bool failure = batch_failure;
			if (!item.O && !failure)
				failure = true;
			if (item.O)
			{
				const auto prepared_index = state->result_indices.find(item.O);
				if (prepared_index != state->result_indices.end() &&
					prepared_index->second < state->results.size())
				{
					trace_prepass_result& candidate = state->results[prepared_index->second];
					if (candidate.target == item.O && candidate.target_id == item.O->ID())
						prepared = &candidate;
				}
				if (!prepared && !failure)
				{
					failure = true;

				}
			}

			Fvector current_target_point;
			bool have_current_target_point = false;
			if (!failure)
			{
				const ICollisionForm* current_cform = item.O->CFORM();
				if (prepared->status == trace_prepass_status::no_collision_form)
				{
					if (current_cform != prepared->target_cform)
						failure = true;
				}
				else if (!current_cform || current_cform != prepared->target_cform)
					failure = true;
				else if (item.bone_id != prepared->bone_id)
					failure = true;
				else if (!item.cp_LP.similar(prepared->local_point, EPS_S))
					failure = true;
				else if (item.O->Position().distance_to_sqr(prepared->target_position) > snapshot_epsilon_sqr)
					failure = true;
			}

			if (!failure && prepared->status == trace_prepass_status::temporal_reuse)
			{
				if (!can_reuse_temporal(item, position, item.O->Position(), Device.dwTimeGlobal, vis_threshold))
					failure = true;
			}
			else if (prepared && prepared->status != trace_prepass_status::no_collision_form && item.O && item.O->CFORM())
			{
				current_target_point = item.O->get_last_local_point_on_mesh(item.cp_LP, item.bone_id);
				have_current_target_point = true;
				if (!failure &&
					current_target_point.distance_to_sqr(prepared->target_point) > snapshot_epsilon_sqr)
					failure = true;
			}

			// A stale free ray cannot prove that the current ray is still free. An
			// opaque static triangle is different: re-testing that exact triangle on
			// the current main-thread ray is the same conservative shortcut used by
			// the legacy OpenXRay visibility cache. Only that blocked case is rebased.
			if (failure && prepared && item.O && item.O->CFORM() &&
				prepared->visibility < vis_threshold && prepared->has_opaque_triangle &&
				have_current_target_point && vis_threshold > 0.f)
			{
				Fvector current_direction;
				current_direction.sub(current_target_point, position);
				const float current_distance = current_direction.magnitude();
				const float current_range = current_distance + .2f;
				if (!fis_zero(current_distance) && current_range > fuzzy_guaranteed)
				{
					current_direction.div(current_range);
					float triangle_u, triangle_v, triangle_range;
					if (CDB::TestRayTri(position, current_direction, prepared->opaque_triangle,
						triangle_u, triangle_v, triangle_range, false) && triangle_range > 0.f && triangle_range < current_range)
					{
						prepared->target_cform = item.O->CFORM();
						prepared->bone_id = item.bone_id;
						prepared->local_point = item.cp_LP;
						prepared->target_position = item.O->Position();
						prepared->target_point = current_target_point;
						prepared->start = position;
						prepared->direction = current_direction;
						prepared->range = current_range;
						prepared->visibility = 0.f;
						prepared->had_static_hit = TRUE;
						prepared->full_query_cache = FALSE;
						prepared->static_cache_reused = FALSE;
						prepared->status = trace_prepass_status::cached_blocked;
						failure = false;
					}
				}
			}

			if (!failure && prepared->status == trace_prepass_status::ready &&
				prepared->visibility >= vis_threshold)
			{
				trace_prepass_result current_ray = *prepared;
				if (have_current_target_point)
				{
					current_ray.start = position;
					current_ray.direction.sub(current_target_point, position);
					const float current_distance = current_ray.direction.magnitude();
					current_ray.range = current_distance + .2f;
					if (!fis_zero(current_distance))
						current_ray.direction.div(current_ray.range);
				}
				vision_spatial_read_guard spatial_guard(g_SpatialSpace);
				if (has_potential_dynamic_locked(current_ray))
					failure = true;
			}

			if (failure || prepared->status == trace_prepass_status::serial_fallback ||
				prepared->status == trace_prepass_status::pending || prepared->status == trace_prepass_status::unused)
			{
				trace_item_serial(item, position, dt, vis_threshold);
				continue;
			}

			// Validation has just proved that the prepared ray endpoint is within the
			// existing tolerance. Publish the authoritative current target position to
			// temporal memory instead of carrying the older/predicted snapshot forward.
			prepared->target_position = item.O->Position();
			item.cp_LR_src = position;
			item.cp_LR_dst = prepared->target_position;
			if (prepared->status != trace_prepass_status::no_collision_form &&
				prepared->status != trace_prepass_status::temporal_reuse)
				item.cp_LAST = current_target_point;
			commit_trace_result(item, *prepared, position, dt, vis_threshold);
		}

		state->task.retire_ready();

		m_trace_prepass_jobs.clear_not_free();
		if (limited && item_count)
		{
			m_trace_prepass_jobs.resize(1);
			m_trace_prepass_jobs[0] = next_cursor;
		}
		return true;
	}

	void Vision::o_trace(Fvector& position, float dt, float vis_threshold, bool allow_parallel)
	{
		o_trace(position, dt, vis_threshold, allow_parallel, 0);
	}

	void Vision::o_trace(Fvector& position, float dt, float vis_threshold, bool allow_parallel,
		u32 max_trace_items)
	{
		o_trace(position, dt, vis_threshold, allow_parallel, max_trace_items, nullptr);
	}

	void Vision::o_trace(Fvector& position, float dt, float vis_threshold, bool allow_parallel,
		u32 max_trace_items, const CObject* priority_target)
	{
		refresh_static_material_transparency();
		RQR.r_clear();
		m_trace_dynamic_snapshot_valid = false;
		m_trace_dynamic_spatial.clear_not_free();
		m_trace_dynamic_candidates.clear_not_free();
		m_trace_dynamic_object_indices.clear();
		const u32 item_count = feel_visible.size();
		if (!item_count)
		{
			m_trace_prepass_jobs.clear_not_free();
			return;
		}

		// Bounded observers keep their complete candidate set and all visibility
		// state, but advance it in round-robin slices. Active combat supplies its
		// selected enemy as priority_target below; distant observers simply rotate.
		// This caps one scheduler callback without deleting NPC simulation.
		const u32 trace_count = max_trace_items ? _min(max_trace_items, item_count) : item_count;
		async_prepare_state* prepared_state = async_state(this);
		const bool had_async_task = prepared_state &&
			prepared_state->task.status() != AIAsyncPrepare::TaskStatus::Idle;
		if (had_async_task && try_consume_async_trace(position, dt, vis_threshold, max_trace_items, priority_target))
			return;

		if (trace_count < item_count)
		{
			// Reuse the otherwise idle prepass scratch vector to retain the cursor. This
			// avoids growing the exported Vision object and keeps its binary layout stable
			// for native plugins built against the previous engine headers.
			const u32 first = m_trace_prepass_jobs.empty() ? 0 : m_trace_prepass_jobs[0] % item_count;
			u32 traced = 0;
			u32 visited = 0;

			// The currently selected enemy drives immediate combat decisions. Trace it on
			// every active slice, then spend the remaining quota on a fair round-robin of
			// secondary candidates. No object is removed and all legacy visibility state
			// remains authoritative between slices.
			if (priority_target)
			{
				const object_index_cache::const_iterator priority = m_feel_visible_indices.find(
					priority_target->ID());
				if (priority != m_feel_visible_indices.end() && priority->second < item_count)
				{
					trace_item_serial(feel_visible[priority->second], position, dt, vis_threshold);
					++traced;
				}
			}

			while (traced < trace_count && visited < item_count)
			{
				feel_visible_Item& item = feel_visible[(first + visited) % item_count];
				++visited;
				if (item.O == priority_target)
					continue;
				trace_item_serial(item, position, dt, vis_threshold);
				++traced;
			}
			m_trace_prepass_jobs.resize(1);
			m_trace_prepass_jobs[0] = (first + visited) % item_count;
			return;
		}
		m_trace_prepass_jobs.clear_not_free();

#ifdef DEBUG
		const bool parallel_environment_available = false;
#else
		const bool cooldown_active = m_trace_prepass_cooldown != 0;
		if (cooldown_active)
			--m_trace_prepass_cooldown;
		// Do not pay snapshot/result-buffer setup for small observer lists. The
		// actual uncached-ray count is checked again after the cheap cache pass.
		// With the asynchronous pipeline enabled, every miss/not-ready/stale path is
		// an exact main-thread serial fallback. The older immediate fork/join path is
		// retained only as the compatibility fallback when async prepare is disabled.
		const bool parallel_environment_available = !psAIAsyncPrepare && !cooldown_active &&
			can_use_parallel_trace_prepass(allow_parallel, item_count);
#endif
		if (!parallel_environment_available)
		{
			for (u32 index = 0; index < item_count; ++index)
				trace_item_serial(feel_visible[index], position, dt, vis_threshold);
			return;
		}

		m_trace_prepass_results.resize(item_count);
		m_trace_prepass_jobs.clear_not_free();
		const u32 trace_time = Device.dwTimeGlobal;

		for (u32 index = 0; index < item_count; ++index)
		{
			feel_visible_Item& item = feel_visible[index];
			trace_prepass_result& result = m_trace_prepass_results[index];
			result.target = item.O;
			result.target_spatial_identity = item.O ? static_cast<const ISpatial*>(item.O) : nullptr;
			result.target_cform = item.O ? item.O->CFORM() : nullptr;
			result.target_id = item.O ? item.O->ID() : u16(-1);
			result.bone_id = item.bone_id;
			result.target_position.set(0.f, 0.f, 0.f);
			result.target_point.set(0.f, 0.f, 0.f);
			result.local_point = item.cp_LP;
			result.cached_dynamic_blocker = item.last_dynamic_blocker;
			result.cached_dynamic_blocker_id = item.last_dynamic_blocker_id;
			result.start = position;
			result.direction.set(0.f, 0.f, 0.f);
			result.range = 0.f;
			result.visibility = 1.f;
			result.status = trace_prepass_status::unused;
			result.full_query_cache = FALSE;
			result.static_cache_reused = FALSE;
			result.static_query_performed = FALSE;
			result.fast_dynamic_fallback = FALSE;
			result.had_static_hit = FALSE;
			result.has_opaque_triangle = FALSE;
			result.opaque_triangle[0].set(0.f, 0.f, 0.f);
			result.opaque_triangle[1].set(0.f, 0.f, 0.f);
			result.opaque_triangle[2].set(0.f, 0.f, 0.f);

			if (!item.O || !item.O->CFORM())
			{
				result.status = trace_prepass_status::no_collision_form;
				continue;
			}

			result.target_position.set(item.O->Position());
			item.cp_LR_dst.set(result.target_position);
			item.cp_LR_src = position;
			if (can_reuse_temporal(item, position, result.target_position, trace_time, vis_threshold))
			{
				result.visibility = item.temporal_visibility;
				result.status = trace_prepass_status::temporal_reuse;
				continue;
			}
			item.cp_LAST = item.O->get_last_local_point_on_mesh(item.cp_LP, item.bone_id);
			result.target_point = item.cp_LAST;

			result.direction.sub(item.cp_LAST, position);
			const float ray_distance = result.direction.magnitude();
			if (fis_zero(ray_distance))
			{
				result.status = trace_prepass_status::coincident_visible;
				continue;
			}

			result.range = ray_distance + .2f;
			if (result.range <= fuzzy_guaranteed)
			{
				result.status = trace_prepass_status::near_visible;
				continue;
			}

			result.direction.div(result.range);
			if (item.Cache_valid && item.Cache.similar(position, result.direction, result.range))
			{
				result.visibility = item.Cache_vis;
				if (result.visibility < vis_threshold)
				{
					result.status = trace_prepass_status::cached_blocked;
					continue;
				}

				if (item.Cache.result)
				{
					result.full_query_cache = TRUE;
				}
				else
				{
					// A valid no-hit first-pass cache proves that the static corridor was clear.
					// The target collision form is still checked on the serial commit thread.
					result.static_cache_reused = TRUE;
					result.had_static_hit = FALSE;
				}

				result.status = trace_prepass_status::pending;
				m_trace_prepass_jobs.push_back(index);
				continue;
			}

			if (item.StaticCache_valid && item.StaticCache.similar(position, result.direction, result.range))
			{
				result.visibility = item.StaticCache_vis;
				result.static_cache_reused = TRUE;
				result.had_static_hit = item.StaticCache.result;
				result.has_opaque_triangle = item.StaticCache_has_opaque_triangle;
				if (result.has_opaque_triangle)
				{
					result.opaque_triangle[0].set(item.StaticCache.verts[0]);
					result.opaque_triangle[1].set(item.StaticCache.verts[1]);
					result.opaque_triangle[2].set(item.StaticCache.verts[2]);
				}

				if (result.visibility < vis_threshold)
				{
					result.status = trace_prepass_status::cached_blocked;
					continue;
				}
				if (result.visibility == vis_threshold)
				{
					result.status = trace_prepass_status::serial_fallback;
					continue;
				}

				result.status = trace_prepass_status::pending;
				m_trace_prepass_jobs.push_back(index);
				continue;
			}

			float u, v, cached_range;
			if (CDB::TestRayTri(position, result.direction, item.Cache.verts, u, v, cached_range, false) && cached_range > 0.f && cached_range < result.range)
			{
				result.visibility = 0.f;
				result.status = trace_prepass_status::cached_blocked;
				continue;
			}

			result.status = trace_prepass_status::pending;
			m_trace_prepass_jobs.push_back(index);
		}

		const u32 job_count = m_trace_prepass_jobs.size();
		const bool use_dynamic_snapshot = build_trace_dynamic_snapshot(position, job_count);
		const bool parallel_prepass = use_dynamic_snapshot &&
			can_use_parallel_trace_prepass(allow_parallel, job_count);
		if (parallel_prepass)
		{
			trace_prepass_workload workload;
			workload.vision = this;
			workload.results = &m_trace_prepass_results;
			workload.jobs = &m_trace_prepass_jobs;
			workload.next_job = 0;
			workload.job_count = job_count;
			workload.visibility_threshold = vis_threshold;
			workload.use_dynamic_snapshot = use_dynamic_snapshot ? TRUE : FALSE;

			const u32 lane_capacity = xr_jobs::available_thread_count();
			const u32 target_chunks = _max(1u, lane_capacity * 3u);
			const u32 estimated_chunk = (job_count + target_chunks - 1) / target_chunks;
			workload.chunk_size = _min(8u, _max(2u, estimated_chunk));

			const u32 requested_lanes = (job_count + workload.chunk_size - 1) / workload.chunk_size;
			const u32 lane_count = _min(lane_capacity, requested_lanes);
			xr_jobs::task_group group;
			xr_jobs::submit_many(&Vision::trace_prepass_worker, &workload,
				lane_count - 1, &group, xr_jobs::priority::high);

			trace_prepass_worker(&workload);
			xr_jobs::wait(group);

			u32 fallback_count = 0;
			for (u32 job = 0; job < job_count; ++job)
			{
				const trace_prepass_result& result = m_trace_prepass_results[m_trace_prepass_jobs[job]];
				if (result.status == trace_prepass_status::serial_fallback && !result.fast_dynamic_fallback)
					++fallback_count;
			}

			const float fallback_ratio = job_count ? float(fallback_count) / float(job_count) : 0.f;
			if (m_trace_history_valid)
				m_trace_fallback_ema = m_trace_fallback_ema * .75f + fallback_ratio * .25f;
			else
			{
				m_trace_fallback_ema = fallback_ratio;
				m_trace_history_valid = true;
			}

			if (fallback_ratio >= .75f)
				m_trace_prepass_cooldown = 3;
			else if (fallback_ratio >= .5f)
				m_trace_prepass_cooldown = 1;
		}
		else if (job_count)
		{
			if (use_dynamic_snapshot)
			{
				for (u32 job = 0; job < job_count; ++job)
					process_trace_prepass(m_trace_prepass_results[m_trace_prepass_jobs[job]], vis_threshold);
			}
			else
			{
				vision_spatial_read_guard spatial_guard(g_SpatialSpace);
				for (u32 job = 0; job < job_count; ++job)
					process_trace_prepass(m_trace_prepass_results[m_trace_prepass_jobs[job]], vis_threshold);
			}
		}

		commit_trace_results(position, dt, vis_threshold);
		m_trace_prepass_jobs.clear_not_free();
	}

};
