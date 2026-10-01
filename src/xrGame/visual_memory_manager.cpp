////////////////////////////////////////////////////////////////////////////
//	Module 		: visual_memory_manager.cpp
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Visual memory manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "visual_memory_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "memory_space_impl.h"
#include "../Include/xrRender/Kinematics.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "stalker_movement_manager_smart_cover.h"
#include "../xrEngine/gamemtllib.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "agent_memory_manager.h"
#include "ai_space.h"
#include "profiler.h"
#include "actor.h"
#include "../xrEngine/camerabase.h"
#include "gamepersistent.h"
#include "actor_memory.h"
#include "client_spawn_manager.h"
#include "client_spawn_manager.h"
#include "memory_manager.h"
#include "ai/monsters/basemonster/base_monster.h"

float g_ai_vision_speed_boost = 1.0f;

#ifndef MASTER_GOLD
#	include "actor.h"
#	include "ai_debug.h"
#endif // MASTER_GOLD

//Alundaio
#include "../../xrServerEntities/script_engine.h"
#include "script_game_object.h"
 //Alundaio
//-Alundaio

namespace
{
constexpr u32 visual_memory_initial_reserve = 32;
constexpr u32 visual_memory_trim_threshold = 256;

template <typename T>
void clear_and_trim_visual_vector(T& values)
{
	values.clear_not_free();
	if (values.capacity() > visual_memory_trim_threshold)
	{
		values.clear_and_free();
		values.reserve(visual_memory_initial_reserve);
	}
}

template <typename T>
void reset_visual_index_cache(T& cache)
{
	T compact;
	compact.reserve(visual_memory_initial_reserve);
	cache.swap(compact);
}

const ::luabind::functor<float>* visible_value_script_callback()
{
	static cached_script_functor<float> callback_cache("visual_memory_manager.get_visible_value");
	return callback_cache.get(ai().script_engine());
}

float call_visible_value_script_callback(const ::luabind::functor<float>& callback,
	CScriptGameObject* observer, CScriptGameObject* target, const float time_delta,
	const float time_quant, const float luminocity, const float velocity_factor,
	const float object_velocity, const float distance, const float object_distance,
	const float always_visible_distance)
{
	lua_State* const lua = callback.lua_state();
#ifndef LUABIND_NO_ERROR_CHECKING
	if (!lua)
	{
#ifndef LUABIND_NO_EXCEPTIONS
		throw ::luabind::error(lua);
#else
		::luabind::error_callback_fun e = ::luabind::get_error_callback();
		if (e) e(lua);
		assert(0 && "tried to call uninitialized functor object");
#endif
	}
#endif

	::luabind::detail::default_policy::generate_converter<float,
		::luabind::detail::Direction::lua_to_cpp>::type converter;
	::luabind::detail::stack_pop result_pop(lua, 1);

	callback.pushvalue();
	::luabind::detail::convert_to_lua(lua, observer);
	::luabind::detail::convert_to_lua(lua, target);
	lua_pushnumber(lua, time_delta);
	lua_pushnumber(lua, time_quant);
	lua_pushnumber(lua, luminocity);
	lua_pushnumber(lua, velocity_factor);
	lua_pushnumber(lua, object_velocity);
	lua_pushnumber(lua, distance);
	lua_pushnumber(lua, object_distance);
	lua_pushnumber(lua, always_visible_distance);

	if (::luabind::detail::pcall(lua, 10, 1))
	{
#ifndef LUABIND_NO_EXCEPTIONS
		throw ::luabind::error(lua);
#else
		::luabind::error_callback_fun e = ::luabind::get_error_callback();
		if (e) e(lua);
		assert(0 && "the lua function threw an error and exceptions are disabled");
#endif
	}

#ifndef LUABIND_NO_ERROR_CHECKING
	if (converter.match(lua, LUABIND_DECORATE_TYPE(float), -1) < 0)
	{
#ifndef LUABIND_NO_EXCEPTIONS
		throw ::luabind::cast_failed(lua, LUABIND_TYPEID(float));
#else
		::luabind::cast_failed_callback_fun e = ::luabind::get_cast_failed_callback();
		if (e) e(lua, LUABIND_TYPEID(float));
		assert(0 && "the lua function's return value could not be converted");
#endif
	}
#endif
	return converter.apply(lua, LUABIND_DECORATE_TYPE(float), -1);
}
}

void SetActorVisibility(u16 who, float value);

struct SRemoveOfflinePredicate
{
	bool operator()(const CVisibleObject& object) const
	{
		VERIFY(object.m_object);
		return (!!object.m_object->getDestroy() || object.m_object->H_Parent());
	}

	bool operator()(const CNotYetVisibleObject& object) const
	{
		VERIFY(object.m_object);
		return (!!object.m_object->getDestroy() || object.m_object->H_Parent());
	}
};

CVisualMemoryManager::CVisualMemoryManager(CCustomMonster* object)
{
	m_object = object;
	m_stalker = 0;
	m_client = 0;
	initialize();
}

CVisualMemoryManager::CVisualMemoryManager(CAI_Stalker* stalker)
{
	m_object = stalker;
	m_stalker = stalker;
	m_client = 0;
	initialize();
}

CVisualMemoryManager::CVisualMemoryManager(vision_client* client)
{
	m_object = 0;
	m_stalker = 0;
	m_client = client;
	initialize();

	m_objects = xr_new<VISIBLES>();
	m_objects->reserve(visual_memory_initial_reserve);
}

void CVisualMemoryManager::initialize()
{
	m_max_object_count = 128;
	m_enabled = true;
	m_objects = 0;
	m_squad_merge_index = 0;

	m_visible_objects.reserve(visual_memory_initial_reserve);
	m_not_yet_visible_objects.reserve(visual_memory_initial_reserve);
	m_visible_object_indices.reserve(visual_memory_initial_reserve);
	m_not_yet_visible_object_indices.reserve(visual_memory_initial_reserve);
}

void CVisualMemoryManager::set_squad_objects(VISIBLES* squad_objects)
{
	set_squad_objects(squad_objects, 0);
}

void CVisualMemoryManager::set_squad_objects(VISIBLES* squad_objects, CAgentMemoryManager* merge_index)
{
	if (m_squad_merge_index)
		m_squad_merge_index->invalidate_visual_merge_candidates();

	m_objects = squad_objects;
	m_squad_merge_index = merge_index;
	m_visible_object_indices.clear();
	if (m_squad_merge_index)
		m_squad_merge_index->invalidate_visual_merge_candidates();

	if (!m_objects)
	{
		m_not_yet_visible_objects.clear_not_free();
		m_not_yet_visible_object_indices.clear();
	}
}

void CVisualMemoryManager::refresh_visible_objects() const
{
	m_visible_objects.clear_not_free();
	if (m_object)
		m_object->feel_vision_get(m_visible_objects);
	else
	{
		VERIFY(m_client);
		m_client->feel_vision_get(m_visible_objects);
	}
}

CVisualMemoryManager::~CVisualMemoryManager()
{
	clear_delayed_objects();

	if (!m_client)
		return;

	xr_delete(m_objects);
}

void CVisualMemoryManager::reinit()
{
	if (!m_client)
	{
		if (m_squad_merge_index)
			m_squad_merge_index->invalidate_visual_merge_candidates();
		m_objects = 0;
		m_squad_merge_index = 0;
	}
	else
	{
		VERIFY(m_objects);
		m_objects->clear();
	}

	clear_and_trim_visual_vector(m_visible_objects);
	clear_and_trim_visual_vector(m_not_yet_visible_objects);
	reset_visual_index_cache(m_visible_object_indices);
	reset_visual_index_cache(m_not_yet_visible_object_indices);

	if (m_client && m_objects && m_objects->capacity() > visual_memory_trim_threshold)
	{
		m_objects->clear_and_free();
		m_objects->reserve(visual_memory_initial_reserve);
	}

	if (m_object)
		m_object->feel_vision_clear();

	m_last_update_time = u32(-1);
}

void CVisualMemoryManager::reload(LPCSTR section)
{
	//	m_max_object_count			= READ_IF_EXISTS(pSettings,r_s32,section,"DynamicObjectsCount",1);

	if (m_stalker)
	{
		m_free.Load(pSettings->r_string(section, "vision_free_section"), true);
		m_danger.Load(pSettings->r_string(section, "vision_danger_section"), true);
	}
	else if (m_object)
	{
		m_free.Load(pSettings->r_string(section, "vision_free_section"), !!m_client);
		m_danger.Load(pSettings->r_string(section, "vision_danger_section"), !!m_client);
	}
	else
	{
		m_free.Load(section, !!m_client);
	}
}

IC const CVisionParameters& CVisualMemoryManager::current_state() const
{
	if (m_stalker)
	{
		return (m_stalker->movement().mental_state() == eMentalStateDanger) ? m_danger : m_free;
	}
	else if (m_object)
	{
		return m_object->is_base_monster_with_enemy() ? m_danger : m_free;
	}
	else
	{
		return m_free;
	}
}

CVisualMemoryManager::VISIBLES::iterator CVisualMemoryManager::find_visible_object(u16 id)
{
	VERIFY(m_objects);

	OBJECT_INDEX_CACHE::iterator cached = m_visible_object_indices.find(id);
	if (cached != m_visible_object_indices.end())
	{
		const u32 index = cached->second;
		if (index < m_objects->size())
		{
			CVisibleObject& object = (*m_objects)[index];
			if (object.m_object && object.m_object->ID() == id)
				return m_objects->begin() + index;
		}
		m_visible_object_indices.erase(id);
	}

	VISIBLES::iterator found = std::find(m_objects->begin(), m_objects->end(), id);
	if (found == m_objects->end())
		return found;

	if (m_visible_object_indices.size() >= m_max_object_count * 2)
		m_visible_object_indices.clear();
	m_visible_object_indices[id] = u32(found - m_objects->begin());
	return found;
}

CVisualMemoryManager::VISIBLES::const_iterator CVisualMemoryManager::find_visible_object(u16 id) const
{
	VERIFY(m_objects);
	const VISIBLES& values = *m_objects;

	OBJECT_INDEX_CACHE::iterator cached = m_visible_object_indices.find(id);
	if (cached != m_visible_object_indices.end())
	{
		const u32 index = cached->second;
		if (index < values.size())
		{
			const CVisibleObject& object = values[index];
			if (object.m_object && object.m_object->ID() == id)
				return values.begin() + index;
		}
		m_visible_object_indices.erase(id);
	}

	VISIBLES::const_iterator found = std::find(values.begin(), values.end(), id);
	if (found == values.end())
		return found;

	if (m_visible_object_indices.size() >= m_max_object_count * 2)
		m_visible_object_indices.clear();
	m_visible_object_indices[id] = u32(found - values.begin());
	return found;
}

CVisualMemoryManager::NOT_YET_VISIBLES::iterator CVisualMemoryManager::find_not_yet_visible_object(u16 id)
{
	OBJECT_INDEX_CACHE::iterator cached = m_not_yet_visible_object_indices.find(id);
	if (cached != m_not_yet_visible_object_indices.end())
	{
		const u32 index = cached->second;
		if (index < m_not_yet_visible_objects.size())
		{
			CNotYetVisibleObject& object = m_not_yet_visible_objects[index];
			if (object.m_object && object.m_object->ID() == id)
				return m_not_yet_visible_objects.begin() + index;
		}
		m_not_yet_visible_object_indices.erase(id);
	}

	NOT_YET_VISIBLES::iterator found = std::find_if(
		m_not_yet_visible_objects.begin(),
		m_not_yet_visible_objects.end(),
		[id](const CNotYetVisibleObject& object)
		{
			return object.m_object && object.m_object->ID() == id;
		});
	if (found == m_not_yet_visible_objects.end())
		return found;

	if (m_not_yet_visible_object_indices.size() >= m_max_object_count * 2)
		m_not_yet_visible_object_indices.clear();
	m_not_yet_visible_object_indices[id] = u32(found - m_not_yet_visible_objects.begin());
	return found;
}

const CVisualMemoryManager::CVisibleObject* CVisualMemoryManager::object_by_id(u16 id) const
{
	if (!m_objects)
		return nullptr;
	VISIBLES::const_iterator found = find_visible_object(id);
	return found != m_objects->end() ? &*found : nullptr;
}

u32 CVisualMemoryManager::visible_object_time_last_seen(const CObject* object) const
{
	if (!m_objects || !object)
		return u32(-1);
	VISIBLES::const_iterator found = find_visible_object(object->ID());
	return found != m_objects->end() ? found->m_level_time : u32(-1);
}

bool CVisualMemoryManager::visible_right_now(const CGameObject* game_object) const
{
	if (!m_objects)
	{
		// --> owner is dead
		return false;
	}

	if (should_ignore_object(game_object))
	{
		return false;
	}

	VISIBLES::const_iterator I = find_visible_object(game_object->ID());
	if (objects().end() == I)
		return false;

	if (!I->visible(mask()))
		return (false);

	if (I->m_level_time < m_last_update_time)
		return false;

	return true;
}

bool CVisualMemoryManager::visible_now(const CGameObject* game_object) const
{
	if (!m_objects)
	{
		// --> owner is dead
		return false;
	}

	if (should_ignore_object(game_object))
	{
		return false;
	}

	VISIBLES::const_iterator I = find_visible_object(game_object->ID());
	return objects().end() != I && I->visible(mask());
}

void CVisualMemoryManager::enable(const CObject* object, bool enable)
{
	if (!m_objects || !object)
		return;
	VISIBLES::iterator J = find_visible_object(object->ID());
	if (J == m_objects->end())
		return;
	const bool old_enabled = !!J->m_enabled;
	if (old_enabled == enable)
		return;
	const squad_mask_type old_mask = J->m_squad_mask.get();
	const u32 index = static_cast<u32>(J - m_objects->begin());
	J->m_enabled = enable;
	if (m_squad_merge_index)
		m_squad_merge_index->visual_object_filter_changed(index, old_mask, old_enabled);
}

void CVisualMemoryManager::build_observer_context(SVisionObserverContext& context) const
{
	context.eye_position.set(0.f, 0.f, 0.f);
	context.eye_direction.set(0.f, 0.f, 1.f);
	context.object_range = flt_max;
	float object_fov = flt_max;

	if (m_object)
	{
		IKinematics* kinematics = smart_cast<IKinematics*>(m_object->Visual());
		VERIFY(kinematics);

		const Fmatrix& eye_matrix = kinematics->LL_GetTransform(u16(m_object->eye_bone));
		Fvector local_eye_position;
		eye_matrix.transform_tiny(local_eye_position, context.eye_position);
		m_object->XFORM().transform_tiny(context.eye_position, local_eye_position);

		if (m_stalker)
		{
			context.eye_direction.setHP(-m_stalker->movement().m_head.current.yaw,
			                            -m_stalker->movement().m_head.current.pitch);
#ifdef HOLDERCUSTOM_NEW
			if (m_stalker->Holder())
				context.eye_direction.set(m_stalker->Direction());
#endif
		}
		else
		{
			const MonsterSpace::SBoneRotation& head_orient = m_object->head_orientation();
			context.eye_direction.setHP(-head_orient.current.yaw, -head_orient.current.pitch);
		}

		m_object->update_range_fov(context.object_range, object_fov, m_object->eye_range,
		                           deg2rad(m_object->eye_fov));
	}
	else
	{
		Fvector dummy;
		float zero0, zero1;
		m_client->camera(context.eye_position, context.eye_direction, dummy, object_fov, zero0, zero1,
		                 context.object_range);
	}

	context.half_fov = object_fov * .5f;
	context.cos_half_fov = _cos(context.half_fov);
}

float CVisualMemoryManager::object_visible_distance(const CGameObject* game_object, float& object_distance,
                                                     const SVisionObserverContext& context,
                                                     const CVisionParameters& parameters) const
{
	Fvector object_direction;
	game_object->Center(object_direction);
	object_direction.sub(context.eye_position);

	const float distance_sqr = object_direction.square_magnitude();
	object_distance = _sqrt(distance_sqr);
	if (object_distance > EPS_S)
		object_direction.div(object_distance);
	else
		object_direction.set(0.f, 0.f, 0.f);

	float cos_alpha = context.eye_direction.dotproduct(object_direction);
	clamp(cos_alpha, -.99999f, .99999f);

	float alpha;
	if (context.half_fov > EPS_S && context.half_fov < PI && cos_alpha <= context.cos_half_fov)
	{
		// Outside the FOV the original code clamps acos() to half_fov.
		alpha = context.half_fov;
	}
	else
	{
		alpha = acosf(cos_alpha);
		clamp(alpha, 0.f, context.half_fov);
	}

	const float max_view_distance = context.object_range * parameters.m_max_view_distance;
	const float min_view_distance = context.object_range * parameters.m_min_view_distance;
	if (context.half_fov <= EPS_S)
		return min_view_distance;

	return (1.f - alpha / context.half_fov) * (max_view_distance - min_view_distance) + min_view_distance;
}

float CVisualMemoryManager::object_visible_distance(const CGameObject* game_object, float& object_distance) const
{
	SVisionObserverContext context;
	build_observer_context(context);
	const CVisionParameters& parameters = current_state();
	return object_visible_distance(game_object, object_distance, context, parameters);
}

float CVisualMemoryManager::object_luminocity(const CGameObject* game_object,
                                              const CVisionParameters& parameters) const
{
	if (!const_cast<CGameObject*>(game_object)->cast_entity_alive())
		return 1.f;

	const float luminocity = const_cast<CGameObject*>(game_object)->ROS()->get_luminocity();
	const float power = log(luminocity > .001f ? luminocity : .001f) * parameters.m_luminocity_factor;
	return exp(power);
}

float CVisualMemoryManager::object_luminocity(const CGameObject* game_object) const
{
	return object_luminocity(game_object, current_state());
}

float CVisualMemoryManager::get_object_velocity(const CGameObject* game_object,
                                                const CNotYetVisibleObject& not_yet_visible_object) const
{
	if (!const_cast<CGameObject*>(game_object)->cast_entity_alive())
		return 0.f;

	if ((game_object->ps_Size() < 2) ||
	    (not_yet_visible_object.m_prev_time == game_object->ps_Element(game_object->ps_Size() - 2).dwTime))
		return 0.f;

	const CObject::SavedPosition pos0 = game_object->ps_Element(game_object->ps_Size() - 2);
	const CObject::SavedPosition pos1 = game_object->ps_Element(game_object->ps_Size() - 1);
	return pos1.vPosition.distance_to(pos0.vPosition) /
	       (float(pos1.dwTime) / 1000.f - float(pos0.dwTime) / 1000.f);
}

float CVisualMemoryManager::get_visible_value(const CGameObject* game_object, float distance, float object_distance,
                                              float time_delta, float object_velocity, float luminocity,
                                              const CVisionParameters& parameters,
                                              const ::luabind::functor<float>* callback) const
{
	const float always_visible_distance = parameters.m_always_visible_distance;
	if (distance <= always_visible_distance + EPS_L)
		return parameters.m_visibility_threshold;

	if (callback)
	{
		const bool record = m_stalker && ai().script_engine().lua_recording() && CPU::qpc_freq;
		const u64 started = record ? CPU::QPC() : 0;
		const float result = call_visible_value_script_callback(*callback,
		                   m_object ? m_object->lua_game_object() : 0,
		                   game_object ? game_object->lua_game_object() : 0,
		                   time_delta, parameters.m_time_quant, luminocity, parameters.m_velocity_factor,
		                   object_velocity, distance, object_distance, always_visible_distance) *
		       g_ai_vision_speed_boost * m_vision_speed;
		if (started)
		{
			const u64 elapsed = CPU::QPC() - started;
			const u64 threshold_ticks = (static_cast<u64>(ai().script_engine().lua_record_threshold_us()) *
				CPU::qpc_freq) / 1000000ull;
			if (elapsed >= threshold_ticks)
				ai().script_engine().record_native_profile("ai_visual", "visual.callback.get_visible_value",
					game_object ? *game_object->cName() : "<visual_target>",
					game_object ? static_cast<u32>(game_object->ID()) : 0u, elapsed,
					m_object ? static_cast<u32>(m_object->ID()) : 0u);
		}
		return result;
	}

	return (time_delta / parameters.m_time_quant * luminocity *
	        (1.f + parameters.m_velocity_factor * object_velocity) *
	        (distance - object_distance) / (distance - always_visible_distance)) *
	       g_ai_vision_speed_boost * m_vision_speed;
}

float CVisualMemoryManager::get_visible_value(const CGameObject* game_object, float distance, float object_distance,
                                              float time_delta, float object_velocity, float luminocity) const
{
	const ::luabind::functor<float>* callback_ptr = visible_value_script_callback();
	return get_visible_value(game_object, distance, object_distance, time_delta, object_velocity, luminocity,
	                         current_state(), callback_ptr);
}

CNotYetVisibleObject* CVisualMemoryManager::not_yet_visible_object(const CGameObject* game_object)
{
	START_PROFILE("Memory Manager/visuals/not_yet_visible_object")
		NOT_YET_VISIBLES::iterator found = find_not_yet_visible_object(game_object->ID());
		if (found == m_not_yet_visible_objects.end())
			return 0;
		return &*found;
	STOP_PROFILE
}

void CVisualMemoryManager::add_not_yet_visible_object(const CNotYetVisibleObject& not_yet_visible_object)
{
	m_not_yet_visible_objects.push_back(not_yet_visible_object);
	if (not_yet_visible_object.m_object)
	{
		if (m_not_yet_visible_object_indices.size() >= m_max_object_count * 2)
			m_not_yet_visible_object_indices.clear();
		m_not_yet_visible_object_indices[not_yet_visible_object.m_object->ID()] =
			m_not_yet_visible_objects.size() - 1;
	}
}

u32 CVisualMemoryManager::get_prev_time(const CGameObject* game_object) const
{
	if (!game_object->ps_Size())
		return (0);
	if (game_object->ps_Size() == 1)
		return (game_object->ps_Element(0).dwTime);
	return (game_object->ps_Element(game_object->ps_Size() - 2).dwTime);
}

bool CVisualMemoryManager::visible(const CGameObject* game_object, float time_delta,
                                   const SVisionObserverContext& context,
                                   const ::luabind::functor<float>* callback)
{
	VERIFY(game_object);

	if (should_ignore_object(game_object) || game_object->getDestroy())
		return false;

#ifndef USE_STALKER_VISION_FOR_MONSTERS
	if (!m_stalker && !m_client)
		return true;
#endif

	const CVisionParameters& parameters = current_state();
	float object_distance = 0.f;
	const float distance = object_visible_distance(game_object, object_distance, context, parameters);
	CNotYetVisibleObject* object = not_yet_visible_object(game_object);

	if (distance < object_distance)
	{
		if (!object)
			return false;

		object->m_value -= parameters.m_decrease_value;
		if (object->m_value < 0.f)
			object->m_value = 0.f;
		else
			object->m_update_time = Device.dwTimeGlobal;
		return object->m_value >= parameters.m_visibility_threshold;
	}

	if (!object)
	{
		CNotYetVisibleObject new_object;
		new_object.m_object = game_object;
		new_object.m_prev_time = 0;
		new_object.m_value = get_visible_value(game_object, distance, object_distance, time_delta,
		                                         get_object_velocity(game_object, new_object),
		                                         object_luminocity(game_object, parameters), parameters, callback);
		const CVisionParameters& result_parameters = current_state();
		clamp(new_object.m_value, 0.f, result_parameters.m_visibility_threshold + EPS_L);
		new_object.m_update_time = Device.dwTimeGlobal;
		new_object.m_prev_time = get_prev_time(game_object);
		add_not_yet_visible_object(new_object);
		return new_object.m_value >= result_parameters.m_visibility_threshold;
	}

	object->m_update_time = Device.dwTimeGlobal;
	object->m_value += get_visible_value(game_object, distance, object_distance, time_delta,
	                                     get_object_velocity(game_object, *object),
	                                     object_luminocity(game_object, parameters), parameters, callback);
	const CVisionParameters& result_parameters = current_state();
	clamp(object->m_value, 0.f, result_parameters.m_visibility_threshold + EPS_L);
	object->m_prev_time = get_prev_time(game_object);
	return object->m_value >= result_parameters.m_visibility_threshold;
}

bool CVisualMemoryManager::visible(const CGameObject* game_object, float time_delta)
{
	SVisionObserverContext context;
	build_observer_context(context);
	const ::luabind::functor<float>* callback_ptr = visible_value_script_callback();
	return visible(game_object, time_delta, context, callback_ptr);
}

bool CVisualMemoryManager::should_ignore_object(CObject const* object) const
{
	if (!object)
	{
		return true;
	}

#ifndef MASTER_GOLD
	if ( smart_cast<CActor const*>(object) && psAI_Flags.test(aiIgnoreActor) )
	{
		return	true;
	}
	else
#endif // MASTER_GOLD

	if (CBaseMonster const* const monster = smart_cast<CBaseMonster const*>(object))
	{
		if (!monster->can_be_seen())
		{
			return true;
		}
	}

	return false;
}

void CVisualMemoryManager::add_visible_object(const CObject* object, float time_delta, bool fictitious)
{
	SVisionObserverContext context;
	if (fictitious)
	{
		context.eye_position.set(0.f, 0.f, 0.f);
		context.eye_direction.set(0.f, 0.f, 1.f);
		context.object_range = 0.f;
		context.half_fov = 0.f;
		context.cos_half_fov = 1.f;
		add_visible_object(object, time_delta, true, context, 0);
		return;
	}

	build_observer_context(context);
	const ::luabind::functor<float>* callback_ptr = visible_value_script_callback();
	add_visible_object(object, time_delta, false, context, callback_ptr);
}

u32 CVisualMemoryManager::add_fictitious_visible_mask(const CObject* object, squad_mask_type knowledge_mask)
{
	if (!object || !knowledge_mask || !m_objects)
		return u32(-1);

	const CGameObject* game_object = smart_cast<const CGameObject*>(object);
	if (!game_object)
		return u32(-1);

	VISIBLES::iterator found = find_visible_object(game_object->ID());
	if (m_objects->end() == found)
	{
		// Keep the legacy fictitious-insert path authoritative for timestamps,
		// replacement policy, index caches and merge-index notifications.
		const squad_mask_type observer_mask = mask();
		add_visible_object(object, .001f, true);

		found = find_visible_object(game_object->ID());
		if (m_objects->end() == found)
			return u32(-1);

		// make_object_visible_somewhen() historically restored the observer's
		// visibility bit immediately after the fictitious insertion.
		found->visible(observer_mask, false);
	}

	const u32 index = static_cast<u32>(found - m_objects->begin());
	const squad_mask_type old_mask = found->m_squad_mask.get();
	const bool old_enabled = !!found->m_enabled;
	const squad_mask_type new_mask = old_mask | knowledge_mask;

	if ((new_mask != old_mask) || !old_enabled)
	{
		found->m_squad_mask.assign(new_mask);
		found->m_enabled = true;
		if (m_squad_merge_index)
			m_squad_merge_index->visual_object_filter_changed(index, old_mask, old_enabled);
	}

	return index;
}

void CVisualMemoryManager::add_visible_object(const CObject* object, float time_delta, bool fictitious,
                                              const SVisionObserverContext& context,
                                              const ::luabind::functor<float>* callback)
{
	if (!fictitious && should_ignore_object(object))
		return;

	const CGameObject* game_object = smart_cast<const CGameObject*>(object);
	if (!game_object)
		return;
	if (!fictitious)
	{
		const bool record = m_stalker && ai().script_engine().lua_recording() && CPU::qpc_freq;
		const u64 started = record ? CPU::QPC() : 0;
		const bool is_visible = visible(game_object, time_delta, context, callback);
		if (started)
		{
			const u64 elapsed = CPU::QPC() - started;
			const u64 threshold_ticks = (static_cast<u64>(ai().script_engine().lua_record_threshold_us()) *
				CPU::qpc_freq) / 1000000ull;
			if (elapsed >= threshold_ticks)
				ai().script_engine().record_native_profile("ai_visual", "visual.add_visible_object.visible_check",
					*game_object->cName(), static_cast<u32>(game_object->ID()), elapsed,
					m_object ? static_cast<u32>(m_object->ID()) : 0u);
		}
		if (!is_visible)
			return;
	}

	const CGameObject* self = m_object;
	VISIBLES::iterator found = find_visible_object(game_object->ID());
	if (m_objects->end() == found)
	{
		CVisibleObject visible_object;
		visible_object.fill(game_object, self, mask(), mask());
#ifdef USE_FIRST_GAME_TIME
		visible_object.m_first_game_time = Level().GetGameTime();
#endif
#ifdef USE_FIRST_LEVEL_TIME
		visible_object.m_first_level_time = Device.dwTimeGlobal;
#endif

		if (m_max_object_count <= m_objects->size())
		{
			VISIBLES::iterator oldest = std::min_element(m_objects->begin(), m_objects->end(),
			                                             SLevelTimePredicate<CGameObject>());
			VERIFY(m_objects->end() != oldest);
			const squad_mask_type old_mask = oldest->m_squad_mask.get();
			const bool old_enabled = !!oldest->m_enabled;
			const u32 index = static_cast<u32>(oldest - m_objects->begin());
			*oldest = visible_object;
			m_visible_object_indices.clear();
			if (m_squad_merge_index)
				m_squad_merge_index->visual_object_filter_changed(index, old_mask, old_enabled);
		}
		else
		{
			m_objects->push_back(visible_object);
			const u32 index = static_cast<u32>(m_objects->size() - 1);
			if (m_visible_object_indices.size() >= m_max_object_count * 2)
				m_visible_object_indices.clear();
			m_visible_object_indices[game_object->ID()] = index;
			if (m_squad_merge_index)
				m_squad_merge_index->visual_object_appended(index);
		}
	}
	else
	{
		const squad_mask_type old_mask = found->m_squad_mask.get();
		const bool old_enabled = !!found->m_enabled;
		const u32 index = static_cast<u32>(found - m_objects->begin());
		if (!fictitious)
			found->fill(game_object, self, found->m_squad_mask.get() | mask(), found->m_visible.get() | mask());
		else
		{
			found->m_visible.assign(found->m_visible.get() | mask());
			found->m_squad_mask.assign(found->m_squad_mask.get() | mask());
			found->m_enabled = true;
		}
		if (m_squad_merge_index &&
			(old_mask != found->m_squad_mask.get() || old_enabled != !!found->m_enabled))
		{
			m_squad_merge_index->visual_object_filter_changed(index, old_mask, old_enabled);
		}
	}
}

void CVisualMemoryManager::add_visible_object(const CVisibleObject visible_object)
{
	if (should_ignore_object(visible_object.m_object))
	{
		return;
	}

	VERIFY(m_objects);
	VISIBLES::iterator found = find_visible_object(visible_object.m_object->ID());
	if (m_objects->end() != found)
	{
		const squad_mask_type old_mask = found->m_squad_mask.get();
		const bool old_enabled = !!found->m_enabled;
		const u32 index = static_cast<u32>(found - m_objects->begin());
		*found = visible_object;
		if (m_squad_merge_index)
			m_squad_merge_index->visual_object_filter_changed(index, old_mask, old_enabled);
	}
	else if (m_max_object_count <= m_objects->size())
	{
		VISIBLES::iterator oldest = std::min_element(m_objects->begin(), m_objects->end(),
		                                             SLevelTimePredicate<CGameObject>());
		VERIFY(m_objects->end() != oldest);
		const squad_mask_type old_mask = oldest->m_squad_mask.get();
		const bool old_enabled = !!oldest->m_enabled;
		const u32 index = static_cast<u32>(oldest - m_objects->begin());
		*oldest = visible_object;
		m_visible_object_indices.clear();
		if (m_squad_merge_index)
			m_squad_merge_index->visual_object_filter_changed(index, old_mask, old_enabled);
	}
	else
	{
		m_objects->push_back(visible_object);
		const u32 index = static_cast<u32>(m_objects->size() - 1);
		if (m_visible_object_indices.size() >= m_max_object_count * 2)
			m_visible_object_indices.clear();
		m_visible_object_indices[visible_object.m_object->ID()] = index;
		if (m_squad_merge_index)
			m_squad_merge_index->visual_object_appended(index);
	}
}

#ifdef DEBUG
void CVisualMemoryManager::check_visibles	() const
{
	squad_mask_type						mask = this->mask();
	xr_vector<CVisibleObject>::iterator	I = m_objects->begin();
	xr_vector<CVisibleObject>::iterator	E = m_objects->end();
	for ( ; I != E; ++I) {
		if (!(*I).visible(mask))
			continue;

		xr_vector<Feel::Vision::feel_visible_Item>::iterator	i = m_object->feel_visible.begin();
		xr_vector<Feel::Vision::feel_visible_Item>::iterator	e = m_object->feel_visible.end();
		for (; i!=e; ++i)
			if (i->O->ID() == (*I).m_object->ID()) {
				VERIFY						(i->fuzzy > 0.f);
				break;
			}
	}
}
#endif

bool CVisualMemoryManager::visible(u32 _level_vertex_id, float yaw, float eye_fov) const
{
	Fvector direction;
	direction.sub(ai().level_graph().vertex_position(_level_vertex_id), m_object->Position());
	direction.normalize_safe();
	float y, p;
	direction.getHP(y, p);
	if (angle_difference(yaw, y) <= eye_fov * PI / 180.f / 2.f)
		return (ai().level_graph().check_vertex_in_direction(m_object->ai_location().level_vertex_id(),
		                                                     m_object->Position(), _level_vertex_id));
	else
		return (false);
}

float CVisualMemoryManager::feel_vision_mtl_transp(CObject* O, u32 element)
{
	float vis = 1.f;
	if (O)
	{
		IKinematics* V = smart_cast<IKinematics*>(O->Visual());
		if (0 != V)
		{
			CBoneData& B = V->LL_GetData((u16)element);
			vis = GMLib.GetMaterialByIdx(B.game_mtl_idx)->fVisTransparencyFactor;
		}
	}
	else
	{
		CDB::TRI* T = Level().ObjectSpace.GetStaticTris() + element;
		vis = GMLib.GetMaterialByIdx(T->material)->fVisTransparencyFactor;
	}
	return vis;
}

void CVisualMemoryManager::remove_links(CObject* object)
{
	VERIFY(m_objects);
	if (!object)
	{
		const u32 visible_size = m_objects->size();
		m_objects->erase(
			std::remove_if(m_objects->begin(), m_objects->end(),
			               [](const CVisibleObject& value) { return !value.m_object; }),
			m_objects->end());
		if (visible_size != m_objects->size())
		{
			m_visible_object_indices.clear();
			if (m_squad_merge_index)
				m_squad_merge_index->invalidate_visual_merge_candidates();
		}

		const u32 pending_size = m_not_yet_visible_objects.size();
		m_not_yet_visible_objects.erase(
			std::remove_if(m_not_yet_visible_objects.begin(), m_not_yet_visible_objects.end(),
			               [](const CNotYetVisibleObject& value) { return !value.m_object; }),
			m_not_yet_visible_objects.end());
		if (pending_size != m_not_yet_visible_objects.size())
			m_not_yet_visible_object_indices.clear();
		return;
	}

	VISIBLES::iterator visible = find_visible_object(object->ID());
	if (visible != m_objects->end())
	{
		m_objects->erase(visible);
		m_visible_object_indices.clear();
		if (m_squad_merge_index)
			m_squad_merge_index->invalidate_visual_merge_candidates();
	}

	NOT_YET_VISIBLES::iterator pending = find_not_yet_visible_object(object->ID());
	if (pending != m_not_yet_visible_objects.end())
	{
		m_not_yet_visible_objects.erase(pending);
		m_not_yet_visible_object_indices.clear();
	}
}

CVisibleObject* CVisualMemoryManager::visible_object(const CGameObject* game_object)
{
	if (!m_objects || !game_object)
		return 0;
	VISIBLES::iterator found = find_visible_object(game_object->ID());
	return found == m_objects->end() ? 0 : &*found;
}

IC squad_mask_type CVisualMemoryManager::mask() const
{
	if (!m_stalker)
		return (squad_mask_type(-1));

	return (m_stalker->agent_manager().member().mask(m_stalker));
}

void CVisualMemoryManager::update(float time_delta)
{
	START_PROFILE("Memory Manager/visuals/update")
		clear_delayed_objects();

		if (!enabled())
			return;

		m_last_update_time = Device.dwTimeGlobal;

		squad_mask_type mask = this->mask();
		VERIFY(m_objects);

		const Feel::Vision::VISIBLE_ITEMS& visible_items =
			m_object ? m_object->feel_vision_items() : m_client->feel_vision_items();
		bool has_visible_items = false;
		for (Feel::Vision::VISIBLE_ITEMS::const_iterator item = visible_items.begin(); item != visible_items.end(); ++item)
		{
			if (positive(item->fuzzy))
			{
				has_visible_items = true;
				break;
			}
		}

		bool needs_observer_context = !!m_stalker || !!m_client;
#ifdef USE_STALKER_VISION_FOR_MONSTERS
		needs_observer_context = true;
#endif

		const ::luabind::functor<float>* visible_value_callback_ptr =
			needs_observer_context && has_visible_items ? visible_value_script_callback() : nullptr;

		SVisionObserverContext observer_context;
		if (needs_observer_context && !visible_value_callback_ptr && has_visible_items)
			build_observer_context(observer_context);
		else
		{
			observer_context.eye_position.set(0.f, 0.f, 0.f);
			observer_context.eye_direction.set(0.f, 0.f, 1.f);
			observer_context.object_range = 0.f;
			observer_context.half_fov = 0.f;
			observer_context.cos_half_fov = 1.f;
		}
		const CVisionParameters& update_parameters = current_state();

		START_PROFILE("Memory Manager/visuals/update/make_invisible")
			{

				xr_vector<CVisibleObject>::iterator I = m_objects->begin();
				xr_vector<CVisibleObject>::iterator E = m_objects->end();
				for (; I != E; ++I)
					if (I->m_level_time + update_parameters.m_still_visible_time < Device.dwTimeGlobal)
						I->visible(mask, false);
			}
		STOP_PROFILE

		START_PROFILE("Memory Manager/visuals/update/add_visibles")
			{

				if (visible_value_callback_ptr)
				{
					// Preserve the legacy pointer snapshot when Lua is involved: a mod callback
					// can mutate object state and must not invalidate the source vision vector.
					refresh_visible_objects();
					for (RAW_VISIBLES::const_iterator object = m_visible_objects.begin();
						 object != m_visible_objects.end(); ++object)
					{
						SVisionObserverContext live_context;
						build_observer_context(live_context);
						add_visible_object(*object, time_delta, false, live_context, visible_value_callback_ptr);
					}
				}
				else
				{
					for (Feel::Vision::VISIBLE_ITEMS::const_iterator item = visible_items.begin();
						 item != visible_items.end(); ++item)
					{
						if (positive(item->fuzzy))
							add_visible_object(item->O, time_delta, false, observer_context, 0);
					}
				}
			}
		STOP_PROFILE

		START_PROFILE("Memory Manager/visuals/update/make_not_yet_visible")
			{

				xr_vector<CNotYetVisibleObject>::iterator I = m_not_yet_visible_objects.begin();
				xr_vector<CNotYetVisibleObject>::iterator E = m_not_yet_visible_objects.end();
				for (; I != E; ++I)
					if ((*I).m_update_time < Device.dwTimeGlobal)
						(*I).m_value = 0.f;
			}
		STOP_PROFILE

		START_PROFILE("Memory Manager/visuals/update/removing_offline")

			// verifying if object is online
			{
				const u32 old_size = m_objects->size();
				m_objects->erase(
					std::remove_if(
						m_objects->begin(),
						m_objects->end(),
						SRemoveOfflinePredicate()
					),
					m_objects->end()
				);
				if (old_size != m_objects->size())
				{
					m_visible_object_indices.clear();
					if (m_squad_merge_index)
						m_squad_merge_index->invalidate_visual_merge_candidates();
				}
			}

			// verifying if object is online
			{
				const u32 old_size = m_not_yet_visible_objects.size();
				m_not_yet_visible_objects.erase(
					std::remove_if(
						m_not_yet_visible_objects.begin(),
						m_not_yet_visible_objects.end(),
						SRemoveOfflinePredicate()
					),
					m_not_yet_visible_objects.end()
				);
				if (old_size != m_not_yet_visible_objects.size())
					m_not_yet_visible_object_indices.clear();
			}
		STOP_PROFILE

#if 0//def DEBUG
	if (m_stalker) {
		CAgentMemberManager::MEMBER_STORAGE::const_iterator	I = m_stalker->agent_manager().member().members().begin();
		CAgentMemberManager::MEMBER_STORAGE::const_iterator	E = m_stalker->agent_manager().member().members().end();
		for ( ; I != E; ++I)
			(*I)->object().memory().visual().check_visibles();
	}
#endif

		if (m_object && g_actor && m_object->is_relation_enemy(Actor()))
		{

			NOT_YET_VISIBLES::iterator I = find_not_yet_visible_object(Actor()->ID());
			if (I != m_not_yet_visible_objects.end())
			{
				SetActorVisibility(
					m_object->ID(),
					clampr(
						(*I).m_value / visibility_threshold(),
						0.f,
						1.f
					)
				);
			}
			else
				SetActorVisibility(m_object->ID(), 0.f);
		}

	STOP_PROFILE
}

static inline bool is_object_valuable_to_save(CCustomMonster const* const self,
                                              MemorySpace::CVisibleObject const& object)
{
	CEntityAlive const* const entity_alive = smart_cast<CEntityAlive const*>(object.m_object);
	if (!entity_alive)
		return false;

	if (!entity_alive->g_Alive())
		return true;

	return self->is_relation_enemy(entity_alive);
}

void CVisualMemoryManager::save(NET_Packet& packet) const
{
	if (m_client)
		return;

	if (!m_object->g_Alive())
		return;

	//	Msg("before saving object %s[%d]", m_object->cName().c_str(), packet.w_tell() );
	u32 count = 0;
	VISIBLES::const_iterator I = objects().begin();
	VISIBLES::const_iterator const E = objects().end();
	for (; I != E; ++I)
	{
		if (is_object_valuable_to_save(m_object, *I))
			++count;
	}

	packet.w_u8((u8)count);

	if (!count)
		return;

	for (I = objects().begin(); I != E; ++I)
	{
		if (!is_object_valuable_to_save(m_object, *I))
			continue;

		VERIFY((*I).m_object);
		packet.w_u16((*I).m_object->ID());
		// object params
		packet.w_u32((*I).m_object_params.m_level_vertex_id);
		packet.w_vec3((*I).m_object_params.m_position);
#ifdef USE_ORIENTATION
		packet.w_float			((*I).m_object_params.m_orientation.yaw);
		packet.w_float			((*I).m_object_params.m_orientation.pitch);
		packet.w_float			((*I).m_object_params.m_orientation.roll);
#endif // USE_ORIENTATION
		// self params
		packet.w_u32((*I).m_self_params.m_level_vertex_id);
		packet.w_vec3((*I).m_self_params.m_position);
#ifdef USE_ORIENTATION
		packet.w_float			((*I).m_self_params.m_orientation.yaw);
		packet.w_float			((*I).m_self_params.m_orientation.pitch);
		packet.w_float			((*I).m_self_params.m_orientation.roll);
#endif // USE_ORIENTATION
#ifdef USE_LEVEL_TIME
		packet.w_u32((Device.dwTimeGlobal > (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_level_time) : 0);
#endif // USE_LAST_LEVEL_TIME
#ifdef USE_LEVEL_TIME
		packet.w_u32((Device.dwTimeGlobal > (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_last_level_time) : 0);
#endif // USE_LAST_LEVEL_TIME
#ifdef USE_FIRST_LEVEL_TIME
		packet.w_u32			((Device.dwTimeGlobal >= (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_first_level_time) : 0);
#endif // USE_FIRST_LEVEL_TIME
		packet.w_u64((*I).m_visible.flags);
	}

	//	Msg("after saving object %s[%d]", m_object->cName().c_str(), packet.w_tell() );
}

void CVisualMemoryManager::load(IReader& packet)
{
	if (m_client)
		return;

	if (!m_object->g_Alive())
		return;

	typedef CClientSpawnManager::CALLBACK_TYPE CALLBACK_TYPE;
	CALLBACK_TYPE callback;
	callback.bind(&m_object->memory(), &CMemoryManager::on_requested_spawn);

	int count = packet.r_u8();
	for (int i = 0; i < count; ++i)
	{
		CDelayedVisibleObject delayed_object;
		delayed_object.m_object_id = packet.r_u16();

		CVisibleObject& object = delayed_object.m_visible_object;
		object.m_object = smart_cast<CGameObject*>(Level().Objects.net_Find(delayed_object.m_object_id));
		// object params
		object.m_object_params.m_level_vertex_id = packet.r_u32();
		packet.r_fvector3(object.m_object_params.m_position);
#ifdef USE_ORIENTATION
		packet.r_float				(object.m_object_params.m_orientation.yaw);
		packet.r_float				(object.m_object_params.m_orientation.pitch);
		packet.r_float				(object.m_object_params.m_orientation.roll);
#endif
		// self params
		object.m_self_params.m_level_vertex_id = packet.r_u32();
		packet.r_fvector3(object.m_self_params.m_position);
#ifdef USE_ORIENTATION
		packet.r_float				(object.m_self_params.m_orientation.yaw);
		packet.r_float				(object.m_self_params.m_orientation.pitch);
		packet.r_float				(object.m_self_params.m_orientation.roll);
#endif
#ifdef USE_LEVEL_TIME
		VERIFY(Device.dwTimeGlobal >= object.m_level_time);
		object.m_level_time = Device.dwTimeGlobal - packet.r_u32();
		if (object.m_level_time > Device.dwTimeGlobal)
			object.m_level_time = Device.dwTimeGlobal;
#endif // USE_LEVEL_TIME
#ifdef USE_LAST_LEVEL_TIME
		VERIFY(Device.dwTimeGlobal >= object.m_last_level_time);
		object.m_last_level_time = Device.dwTimeGlobal - packet.r_u32();
		if (object.m_last_level_time > Device.dwTimeGlobal)
			object.m_last_level_time = Device.dwTimeGlobal;
#endif // USE_LAST_LEVEL_TIME
#ifdef USE_FIRST_LEVEL_TIME
		VERIFY						(Device.dwTimeGlobal >= (*I).m_first_level_time);
		object.m_first_level_time	= packet.r_u32();
		object.m_first_level_time	+= Device.dwTimeGlobal;
#endif // USE_FIRST_LEVEL_TIME
		object.m_visible.assign(packet.r_u64());

		if (object.m_object)
		{
			add_visible_object(object);
			continue;
		}

		m_delayed_objects.push_back(delayed_object);

		const CClientSpawnManager::CSpawnCallback* spawn_callback = Level().client_spawn_manager().callback(
			delayed_object.m_object_id,
			m_object->ID());
		if (!spawn_callback || !spawn_callback->m_object_callback)
			if (!g_dedicated_server)
				Level().client_spawn_manager().add(delayed_object.m_object_id, m_object->ID(), callback);
#ifdef DEBUG
		else {
			if (spawn_callback && spawn_callback->m_object_callback) {
				VERIFY				(spawn_callback->m_object_callback == callback);
			}
		}
#endif // DEBUG
	}
}

void CVisualMemoryManager::clear_delayed_objects()
{
	if (m_client)
		return;

	if (m_delayed_objects.empty())
		return;

	CClientSpawnManager& manager = Level().client_spawn_manager();
	DELAYED_VISIBLE_OBJECTS::const_iterator I = m_delayed_objects.begin();
	DELAYED_VISIBLE_OBJECTS::const_iterator E = m_delayed_objects.end();
	for (; I != E; ++I)
		manager.remove((*I).m_object_id, m_object->ID());

	m_delayed_objects.clear();
}

void CVisualMemoryManager::on_requested_spawn(CObject* object)
{
	DELAYED_VISIBLE_OBJECTS::iterator I = m_delayed_objects.begin();
	DELAYED_VISIBLE_OBJECTS::iterator E = m_delayed_objects.end();
	for (; I != E; ++I)
	{
		if ((*I).m_object_id != object->ID())
			continue;

		if (m_object->g_Alive())
		{
			(*I).m_visible_object.m_object = smart_cast<CGameObject*>(object);
			VERIFY((*I).m_visible_object.m_object);
			add_visible_object((*I).m_visible_object);
		}

		m_delayed_objects.erase(I);
		return;
	}
}
