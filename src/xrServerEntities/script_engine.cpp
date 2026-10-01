////////////////////////////////////////////////////////////////////////////
//	Module 		: script_engine.cpp
//	Created 	: 01.04.2004
//  Modified 	: [1/14/2015 Andrey]
//	Author		: Dmitriy Iassenev
//	Description : XRay Script Engine
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_engine.h"
#include "ai_space.h"
#include "object_factory.h"
#include "script_process.h"
#include "../build_config_defines.h"
#include "script_storage.h"
#include "../xrCore/engine_error_logger.h"
#include <luabind/detail/pcall.hpp>
#include <unordered_map>
#include <set>
#include <atomic>

#ifdef _WIN64
extern "C"
{
#include "../3rd party/luajit-2/src/xr_alloc.h"
}
#endif

struct SLuaHotspotProfileEntry
{
	u64 calls = 0;
	u64 total_ticks = 0;
	u64 max_ticks = 0;
	u64 alloc_calls = 0;
	u64 realloc_calls = 0;
	u64 free_calls = 0;
	u64 allocated_bytes = 0;
	u64 freed_bytes = 0;
	u64 errors = 0;
};

struct SLuaHotspotProfilerState
{
	bool enabled = false;
	bool callbacks_enabled = false;
	u32 threshold_us = 0;
	xr_map<const void*, shared_str> function_names;
	xr_map<shared_str, SLuaHotspotProfileEntry> hotspots;
};

struct SLuaRecordFunctionEntry
{
	u64 calls = 0;
	u64 total_ticks = 0;
	u64 self_ticks = 0;
	u64 max_ticks = 0;
	u64 max_self_ticks = 0;
};

struct SLuaRecordFunctionIdentity
{
	shared_str name;
	shared_str source;
	int line = -1;
};

struct SLuaRecordDirectCEntry
{
	shared_str callback;
	shared_str function;
	u64 calls = 0;
	u64 total_ticks = 0;
	u64 max_ticks = 0;
};

struct SLuaRecordSlowCallback
{
	u64 relative_ticks = 0;
	shared_str callback;
	shared_str hottest_inclusive_function;
	shared_str hottest_self_function;
	shared_str hottest_direct_c_function;
	u64 elapsed_ticks = 0;
	u64 hottest_inclusive_ticks = 0;
	u64 hottest_self_ticks = 0;
	u64 hottest_direct_c_ticks = 0;
	u64 alloc_calls = 0;
	u64 realloc_calls = 0;
	u64 free_calls = 0;
	u64 allocated_bytes = 0;
	u64 freed_bytes = 0;
	u64 gc_bytes_begin = 0;
	u64 gc_bytes_end = 0;
	int result = 0;
};

struct SLuaRecordNativeEntry
{
	shared_str domain;
	shared_str phase;
	u64 calls = 0;
	u64 total_ticks = 0;
	u64 max_ticks = 0;
	u64 detail_total = 0;
	u32 detail_max = 0;
};

struct SLuaRecordNativeEvent
{
	u64 relative_ticks = 0;
	shared_str domain;
	shared_str phase;
	shared_str object_name;
	u32 object_id = 0;
	u32 detail = 0;
	u64 elapsed_ticks = 0;
};

struct SLuaRecordState
{
	std::atomic_bool active{false};
	xrCriticalSection native_lock;
	u32 threshold_us = 500;
	u64 started_ticks = 0;
	HANDLE file = INVALID_HANDLE_VALUE;
	string_path file_name = {};
	xr_map<const void*, SLuaRecordFunctionIdentity> function_names;
	xr_map<shared_str, SLuaRecordFunctionEntry> functions;
	xr_map<shared_str, SLuaRecordDirectCEntry> direct_c_functions;
	xr_map<shared_str, SLuaRecordNativeEntry> native_profiles;
	xr_vector<SLuaRecordSlowCallback> slow_callbacks;
	xr_vector<SLuaRecordNativeEvent> native_events;
	u64 dropped_slow_callbacks = 0;
	u64 dropped_native_events = 0;
	u64 hook_overflows = 0;
	u64 hook_call_events = 0;
	u64 hook_return_events = 0;
	u64 hook_tail_return_events = 0;
	u64 hook_inferred_tail_calls = 0;
	u64 hook_resync_events = 0;
	u64 hook_resync_frames = 0;
	u64 hook_unmatched_returns = 0;
	u64 hook_ignored_c_calls = 0;
	u64 direct_c_call_events = 0;
	u64 direct_c_completed_events = 0;
	u64 direct_c_profile_overflows = 0;
	u32 hook_max_depth = 0;
};

namespace
{
constexpr u32 lua_profile_max_nested_calls = 128;
constexpr u32 lua_record_max_function_depth = 4096;
constexpr u32 lua_record_max_slow_events = 8192;
constexpr u32 lua_record_max_native_events = 8192;

struct SLuaRecordFunctionFrame
{
	lua_State* state = nullptr;
	int activation_id = 0;
	const void* function_identity = nullptr;
	shared_str function_name;
	u64 started_ticks = 0;
	u64 child_ticks = 0;
};

struct SLuaProfileCallContext
{
	CScriptEngine* engine = nullptr;
	lua_State* state = nullptr;
	bool profiling = false;
	bool record_hook_installed = false;
	bool record_direct_c_calls = false;
	bool direct_c_dispatch_scope_started = false;
	u64 start_ticks = 0;
	u64 gc_bytes_begin = 0;
	SLuaAllocationStats allocation_begin;
	shared_str function_name;
	shared_str hottest_inclusive_function;
	shared_str hottest_self_function;
	shared_str hottest_direct_c_function;
	u64 hottest_inclusive_ticks = 0;
	u64 hottest_self_ticks = 0;
	u64 hottest_direct_c_ticks = 0;
	lua_Hook previous_hook = nullptr;
	int previous_hook_mask = 0;
	int previous_hook_count = 0;
	xr_vector<SLuaRecordFunctionFrame> record_frames;
	u32 record_overflow_depth = 0;
};

constexpr u32 lua_to_cpp_profile_max_nested_calls = 128;

struct SLuaToCppProfileCallContext
{
	CScriptEngine* engine = nullptr;
	SLuaProfileCallContext* callback_context = nullptr;
	shared_str profile_key;
	shared_str function_name;
	u64 started_ticks = 0;
};

thread_local SLuaProfileCallContext lua_profile_contexts[lua_profile_max_nested_calls];
thread_local u32 lua_profile_context_depth = 0;
thread_local SLuaToCppProfileCallContext lua_to_cpp_profile_contexts[lua_to_cpp_profile_max_nested_calls];
thread_local u32 lua_to_cpp_profile_context_depth = 0;

u64 lua_profile_counter_delta(const u64 current, const u64 previous)
{
	return current >= previous ? current - previous : current;
}

u32 lua_profile_ticks_to_us(const u64 ticks)
{
	if (!CPU::qpc_freq)
		return 0;
	return static_cast<u32>(_min<u64>(
		(ticks * 1000000ull) / CPU::qpc_freq, u32(-1)));
}

double lua_record_ticks_to_ms(const u64 ticks)
{
	return CPU::qpc_freq ?
		static_cast<double>(ticks) * 1000.0 / static_cast<double>(CPU::qpc_freq) : 0.0;
}

u64 lua_record_gc_bytes(lua_State* state)
{
	if (!state)
		return 0;

	const int kilobytes = lua_gc(state, LUA_GCCOUNT, 0);
	const int bytes = lua_gc(state, LUA_GCCOUNTB, 0);
	return (kilobytes > 0 ? static_cast<u64>(kilobytes) * 1024ull : 0ull) +
		(bytes > 0 ? static_cast<u64>(bytes) : 0ull);
}

struct SLuaSlowCallbackMinHeap
{
	bool operator()(const SLuaRecordSlowCallback& left, const SLuaRecordSlowCallback& right) const
	{
		return left.elapsed_ticks > right.elapsed_ticks;
	}
};

struct SLuaNativeEventMinHeap
{
	bool operator()(const SLuaRecordNativeEvent& left, const SLuaRecordNativeEvent& right) const
	{
		return left.elapsed_ticks > right.elapsed_ticks;
	}
};

void lua_record_keep_slowest_callback(SLuaRecordState* state, const SLuaRecordSlowCallback& event)
{
	if (!state)
		return;

	if (state->slow_callbacks.size() < lua_record_max_slow_events)
	{
		state->slow_callbacks.push_back(event);
		std::push_heap(state->slow_callbacks.begin(), state->slow_callbacks.end(), SLuaSlowCallbackMinHeap{});
		return;
	}

	++state->dropped_slow_callbacks;
	if (state->slow_callbacks.empty() || event.elapsed_ticks <= state->slow_callbacks.front().elapsed_ticks)
		return;

	std::pop_heap(state->slow_callbacks.begin(), state->slow_callbacks.end(), SLuaSlowCallbackMinHeap{});
	state->slow_callbacks.back() = event;
	std::push_heap(state->slow_callbacks.begin(), state->slow_callbacks.end(), SLuaSlowCallbackMinHeap{});
}

void lua_record_keep_slowest_native_event(SLuaRecordState* state, const SLuaRecordNativeEvent& event)
{
	if (!state)
		return;

	if (state->native_events.size() < lua_record_max_native_events)
	{
		state->native_events.push_back(event);
		std::push_heap(state->native_events.begin(), state->native_events.end(), SLuaNativeEventMinHeap{});
		return;
	}

	++state->dropped_native_events;
	if (state->native_events.empty() || event.elapsed_ticks <= state->native_events.front().elapsed_ticks)
		return;

	std::pop_heap(state->native_events.begin(), state->native_events.end(), SLuaNativeEventMinHeap{});
	state->native_events.back() = event;
	std::push_heap(state->native_events.begin(), state->native_events.end(), SLuaNativeEventMinHeap{});
}

void lua_record_write(HANDLE file, LPCSTR format, ...)
{
	if (file == INVALID_HANDLE_VALUE || !format)
		return;

	char buffer[8192];
	va_list args;
	va_start(args, format);
	const int written = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
	va_end(args);

	const DWORD bytes = written >= 0 ? static_cast<DWORD>(written) : static_cast<DWORD>(xr_strlen(buffer));
	if (!bytes)
		return;

	DWORD actual = 0;
	WriteFile(file, buffer, bytes, &actual, nullptr);
}

bool lua_record_ensure_directory(LPSTR result, const size_t result_size)
{
	if (!result || !result_size)
		return false;
	*result = 0;

	char app_data[MAX_PATH] = {};
	const DWORD length = GetEnvironmentVariableA("APPDATA", app_data, static_cast<DWORD>(sizeof(app_data)));
	if (!length || length >= sizeof(app_data))
	{
		LPCSTR error_directory = EngineErrorLogger::GetDirectory();
		if (!error_directory || !error_directory[0] || !xr_strcmp(error_directory, "<unavailable>"))
			return false;

		xr_strcpy(app_data, error_directory);
		LPSTR separator = strrchr(app_data, '\\');
		if (!separator)
			return false;
		*separator = 0;
	}
	else
	{
		char engine_directory[MAX_PATH] = {};
		if (_snprintf_s(engine_directory, sizeof(engine_directory), _TRUNCATE,
			"%s\\QUARK ENGINE", app_data) < 0)
			return false;
		if (!CreateDirectoryA(engine_directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
			return false;
		xr_strcpy(app_data, engine_directory);
	}

	if (_snprintf_s(result, result_size, _TRUNCATE, "%s\\LUA", app_data) < 0)
		return false;
	if (!CreateDirectoryA(result, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		*result = 0;
		return false;
	}
	return true;
}

int lua_record_event_mask(const int event)
{
	switch (event)
	{
	case LUA_HOOKCALL: return LUA_MASKCALL;
	case LUA_HOOKRET:
	case LUA_HOOKTAILRET: return LUA_MASKRET;
	case LUA_HOOKLINE: return LUA_MASKLINE;
	case LUA_HOOKCOUNT: return LUA_MASKCOUNT;
	default: return 0;
	}
}

bool lua_record_contains_no_case(LPCSTR text, LPCSTR needle)
{
	if (!text || !needle || !needle[0])
		return false;

	const size_t needle_length = xr_strlen(needle);
	for (LPCSTR cursor = text; *cursor; ++cursor)
	{
		if (!_strnicmp(cursor, needle, needle_length))
			return true;
	}
	return false;
}

SLuaProfileCallContext* lua_record_context_for_state(lua_State* state)
{
	for (u32 index = lua_profile_context_depth; index > 0; --index)
	{
		SLuaProfileCallContext& context = lua_profile_contexts[index - 1];
		if (context.engine && context.state == state)
			return &context;
	}
	return nullptr;
}

SLuaProfileCallContext* lua_record_hook_owner_for_state(lua_State* state)
{
	for (u32 index = lua_profile_context_depth; index > 0; --index)
	{
		SLuaProfileCallContext& context = lua_profile_contexts[index - 1];
		if (context.record_hook_installed && context.state == state)
			return &context;
	}
	return nullptr;
}

bool lua_record_hook_already_installed(lua_State* state, lua_Hook record_hook)
{
	return state && record_hook && lua_gethook(state) == record_hook &&
		lua_record_hook_owner_for_state(state) != nullptr;
}

struct SLuaResolvedHookFunction
{
	bool info_available = false;
	bool is_lua = false;
	int activation_id = 0;
	const void* identity = nullptr;
	shared_str name;
};

bool lua_record_hook_call_is_lua(lua_State* state, lua_Debug* debug_info)
{
	if (!state || !debug_info)
		return true;
	lua_Debug source_info = *debug_info;
	if (!lua_getinfo(state, "S", &source_info))
		return true;
	return !source_info.what || xr_strcmp(source_info.what, "C") != 0;
}

SLuaResolvedHookFunction lua_record_resolve_hook_function(
	SLuaRecordState* record_state, lua_State* state, lua_Debug* debug_info)
{
	SLuaResolvedHookFunction result;
	if (!record_state || !state || !debug_info)
		return result;

	result.activation_id = debug_info->i_ci;
	const int previous_top = lua_gettop(state);
	lua_Debug function_info = *debug_info;
	result.info_available = lua_getinfo(state, "nSf", &function_info) != 0;
	result.identity = lua_gettop(state) > previous_top ? lua_topointer(state, -1) : nullptr;
	result.is_lua = !result.info_available || !function_info.what || xr_strcmp(function_info.what, "C") != 0;

	if (result.is_lua)
	{
		LPCSTR source = result.info_available && function_info.short_src[0] ? function_info.short_src : "<Lua>";
		const int definition_line = result.info_available ? function_info.linedefined : -1;

		if (result.identity)
		{
			auto found = record_state->function_names.find(result.identity);
			if (found != record_state->function_names.end() &&
				found->second.line == definition_line &&
				!xr_strcmp(found->second.source.c_str(), source))
			{
				result.name = found->second.name;
			}
		}

		if (!result.name.size())
		{
			string1024 resolved_name;
			if (result.info_available && function_info.name && function_info.name[0])
				xr_sprintf(resolved_name, "%s [%s:%d]", function_info.name, source, definition_line);
			else
				xr_sprintf(resolved_name, "<anonymous> [%s:%d]", source, definition_line);

			result.name = resolved_name;
			if (result.identity)
			{
				SLuaRecordFunctionIdentity& identity = record_state->function_names[result.identity];
				identity.name = result.name;
				identity.source = source;
				identity.line = definition_line;
			}
		}
	}
	else
	{
		string512 resolved_name;
		if (result.info_available && function_info.name && function_info.name[0])
			xr_sprintf(resolved_name, "%s [[C]]", function_info.name);
		else
			xr_strcpy(resolved_name, "<C function> [[C]]");
		result.name = resolved_name;
	}

	lua_settop(state, previous_top);
	return result;
}

bool lua_record_close_top_frame(SLuaRecordState* record_state, SLuaProfileCallContext* owner,
	lua_State* state, const u64 ended_ticks)
{
	if (!record_state || !owner || !state || owner->record_frames.empty())
		return false;

	SLuaRecordFunctionFrame frame = owner->record_frames.back();
	if (frame.state != state)
		return false;

	owner->record_frames.pop_back();
	const u64 elapsed_ticks = ended_ticks >= frame.started_ticks ? ended_ticks - frame.started_ticks : 0;
	const u64 self_ticks = elapsed_ticks >= frame.child_ticks ? elapsed_ticks - frame.child_ticks : 0;

	if (frame.function_name.size())
	{
		SLuaRecordFunctionEntry& entry = record_state->functions[frame.function_name];
		++entry.calls;
		entry.total_ticks += elapsed_ticks;
		entry.self_ticks += self_ticks;
		entry.max_ticks = _max(entry.max_ticks, elapsed_ticks);
		entry.max_self_ticks = _max(entry.max_self_ticks, self_ticks);
	}

	SLuaProfileCallContext* context = lua_record_context_for_state(state);
	if (context)
	{
		if (elapsed_ticks > context->hottest_inclusive_ticks)
		{
			context->hottest_inclusive_ticks = elapsed_ticks;
			context->hottest_inclusive_function = frame.function_name;
		}
		if (self_ticks > context->hottest_self_ticks)
		{
			context->hottest_self_ticks = self_ticks;
			context->hottest_self_function = frame.function_name;
		}
	}

	if (!owner->record_frames.empty())
	{
		SLuaRecordFunctionFrame& parent = owner->record_frames.back();
		if (parent.state == state)
			parent.child_ticks += elapsed_ticks;
	}
	return true;
}

bool lua_record_close_matching_activation(SLuaRecordState* record_state, SLuaProfileCallContext* owner,
	lua_State* state, const int activation_id, const u64 ended_ticks, const bool count_resync = true)
{
	if (!record_state || !owner || !activation_id || owner->record_frames.empty())
		return false;

	s32 match_index = -1;
	for (s32 index = static_cast<s32>(owner->record_frames.size()) - 1; index >= 0; --index)
	{
		const SLuaRecordFunctionFrame& frame = owner->record_frames[static_cast<u32>(index)];
		if (frame.state == state && frame.activation_id == activation_id)
		{
			match_index = index;
			break;
		}
	}

	if (match_index < 0)
		return false;

	const u32 frames_to_close = static_cast<u32>(owner->record_frames.size()) - static_cast<u32>(match_index);
	if (count_resync && frames_to_close > 1)
	{
		++record_state->hook_resync_events;
		record_state->hook_resync_frames += frames_to_close - 1;
	}

	for (u32 index = 0; index < frames_to_close; ++index)
	{
		if (!lua_record_close_top_frame(record_state, owner, state, ended_ticks))
			return false;
	}
	return true;
}

bool lua_record_close_matching_identity(SLuaRecordState* record_state, SLuaProfileCallContext* owner,
	lua_State* state, const void* function_identity, const u64 ended_ticks)
{
	if (!record_state || !owner || !function_identity || owner->record_frames.empty())
		return false;

	s32 match_index = -1;
	for (s32 index = static_cast<s32>(owner->record_frames.size()) - 1; index >= 0; --index)
	{
		const SLuaRecordFunctionFrame& frame = owner->record_frames[static_cast<u32>(index)];
		if (frame.state == state && frame.function_identity == function_identity)
		{
			match_index = index;
			break;
		}
	}

	if (match_index < 0)
		return false;

	const u32 frames_to_close = static_cast<u32>(owner->record_frames.size()) - static_cast<u32>(match_index);
	if (frames_to_close > 1)
	{
		++record_state->hook_resync_events;
		record_state->hook_resync_frames += frames_to_close - 1;
	}

	for (u32 index = 0; index < frames_to_close; ++index)
	{
		if (!lua_record_close_top_frame(record_state, owner, state, ended_ticks))
			return false;
	}
	return true;
}

void* luabind_profile_begin(lua_State* state, int function_index)
{
	return ai().script_engine().begin_lua_profile_call(state, function_index);
}

void luabind_profile_end(void* context, int result)
{
	if (!context)
		return;
	static_cast<SLuaProfileCallContext*>(context)->engine->end_lua_profile_call(context, result);
}

void* luabind_lua_to_cpp_profile_begin(lua_State* state, const char* owner_name, const char* function_name)
{
	return ai().script_engine().begin_lua_to_cpp_profile_call(state, owner_name, function_name);
}

void luabind_lua_to_cpp_profile_end(void* context)
{
	if (!context)
		return;
	static_cast<SLuaToCppProfileCallContext*>(context)->engine->end_lua_to_cpp_profile_call(context);
}
}

#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
#		include "script_debugger.h"
#	else //USE_LUA_STUDIO
#		include "lua_studio.h"
typedef cs::lua_studio::create_world_function_type			create_world_function_type;
typedef cs::lua_studio::destroy_world_function_type			destroy_world_function_type;

static create_world_function_type	s_create_world				= 0;
static destroy_world_function_type	s_destroy_world				= 0;
static HMODULE						s_script_debugger_handle	= 0;
static LogCallback					s_old_log_callback			= 0;
#	endif //!USE_LUA_STUDIO
#endif

#ifndef XRSE_FACTORY_EXPORTS
#	ifdef DEBUG
#		include "ai_debug.h"
extern Flags32 psAI_Flags;
#	endif //-DEBUG
#endif //!XRSE_FACTORY_EXPORTS
#include "lua.hpp"

#ifdef USE_LUAJIT_ONE
void jit_command(lua_State*, LPCSTR);
#endif

#if defined(USE_DEBUGGER) && defined(USE_LUA_STUDIO)
static void log_callback			(LPCSTR message)
{
    if (s_old_log_callback)
        s_old_log_callback			(message);

    if (!ai().script_engine().debugger())
        return;

    ai().script_engine().debugger()->add_log_line	(message);
}

static void initialize_lua_studio	( lua_State* state, cs::lua_studio::world*& world, lua_studio_engine*& engine)
{
    engine							= 0;
    world							= 0;

    u32 const old_error_mode		= SetErrorMode(SEM_FAILCRITICALERRORS);
    s_script_debugger_handle		= LoadLibrary(CS_LUA_STUDIO_BACKEND_FILE_NAME);
    SetErrorMode					(old_error_mode);
    if (!s_script_debugger_handle) {
        Msg							("! cannot load %s dynamic library", CS_LUA_STUDIO_BACKEND_FILE_NAME);
        return;
    }

    R_ASSERT2						(s_script_debugger_handle, "can't load script debugger library");

    s_create_world					= (create_world_function_type)
        GetProcAddress(
        s_script_debugger_handle,
        "_cs_lua_studio_backend_create_world@12"
        );
    R_ASSERT2						(s_create_world, "can't find function \"cs_lua_studio_backend_create_world\"");

    s_destroy_world					= (destroy_world_function_type)
        GetProcAddress(
        s_script_debugger_handle,
        "_cs_lua_studio_backend_destroy_world@4"
        );
    R_ASSERT2						(s_destroy_world, "can't find function \"cs_lua_studio_backend_destroy_world\" in the library");

    engine							= xr_new<lua_studio_engine>();
    world							= s_create_world( *engine, false, false );
    VERIFY							(world);

    s_old_log_callback				= SetLogCB(&log_callback);

#ifdef USE_LUAJIT_ONE
    jit_command						(state, "debug=2");
    jit_command						(state, "off");
#else
	luaJIT_setmode(state, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
#endif

    world->add						(state);
}

static void finalize_lua_studio		( lua_State* state, cs::lua_studio::world*& world, lua_studio_engine*& engine)
{
    world->remove					(state);

    VERIFY							(world);
    s_destroy_world					(world);
    world							= 0;

    VERIFY							(engine);
    xr_delete						(engine);

    FreeLibrary						(s_script_debugger_handle);
    s_script_debugger_handle		= 0;

    SetLogCB						(s_old_log_callback);
}

void CScriptEngine::try_connect_to_debugger		()
{
    if (m_lua_studio_world)
        return;

    initialize_lua_studio			( lua(), m_lua_studio_world, m_lua_studio_engine );
}

void CScriptEngine::disconnect_from_debugger	()
{
    if (!m_lua_studio_world)
        return;

    finalize_lua_studio				( lua(), m_lua_studio_world, m_lua_studio_engine );
}
#endif //-(USE_DEBUGGER) && defined(USE_LUA_STUDIO)

CScriptEngine::CScriptEngine()
{
	m_stack_level = 0;
	m_reload_modules = false;
	m_script_generation = 1;
	m_lua_profile_epoch = 1;
	m_lua_hotspot_profiler = xr_new<SLuaHotspotProfilerState>();
	m_lua_record_state = xr_new<SLuaRecordState>();
	m_last_no_file_length = 0;
	*m_last_no_file = 0;

#ifdef DEBUG
	::luabind::detail::set_pcall_profile_callbacks(&luabind_profile_begin, &luabind_profile_end);
#endif
	::luabind::detail::set_lua_to_cpp_profile_callbacks(
		&luabind_lua_to_cpp_profile_begin, &luabind_lua_to_cpp_profile_end);

#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
	m_scriptDebugger = NULL;
	restartDebugger();
#	else //USE_LUA_STUDIO
	m_lua_studio_world = 0;
#	endif //!USE_LUA_STUDIO
#endif
}

void CScriptEngine::register_cached_functor(ICachedScriptFunctor* functor)
{
	if (!functor)
		return;

	if (std::find(m_cached_functors.begin(), m_cached_functors.end(), functor) == m_cached_functors.end())
		m_cached_functors.push_back(functor);
}

void CScriptEngine::unregister_cached_functor(ICachedScriptFunctor* functor)
{
	auto found = std::find(m_cached_functors.begin(), m_cached_functors.end(), functor);
	if (found != m_cached_functors.end())
		m_cached_functors.erase(found);
}

void CScriptEngine::advance_script_generation()
{
	++m_script_generation;
	if (!m_script_generation)
		m_script_generation = 1;

	++m_lua_profile_epoch;
	if (!m_lua_profile_epoch)
		m_lua_profile_epoch = 1;

	if (m_lua_hotspot_profiler)
		m_lua_hotspot_profiler->function_names.clear();
	if (m_lua_record_state)
		m_lua_record_state->function_names.clear();
}

void CScriptEngine::invalidate_script_functors()
{
	for (ICachedScriptFunctor* cached_functor : m_cached_functors)
	{
		if (cached_functor)
			cached_functor->invalidate_script_functor();
	}
	advance_script_generation();
}

void CScriptEngine::release_script_functors()
{
	for (ICachedScriptFunctor* cached_functor : m_cached_functors)
	{
		if (!cached_functor)
			continue;
		cached_functor->invalidate_script_functor();
		cached_functor->detach_script_engine(this);
	}
	m_cached_functors.clear();
}

bool CScriptEngine::lua_hotspot_profiler_enabled() const
{
	return m_lua_hotspot_profiler && m_lua_hotspot_profiler->enabled;
}

void CScriptEngine::set_lua_hotspot_profiler(const bool enabled, const u32 threshold_us)
{
	if (!m_lua_hotspot_profiler)
		return;

	const bool record_enabled = lua_recording();
	const bool effective_enabled = enabled || record_enabled;
	const bool callbacks_enabled = effective_enabled;
	const u32 effective_threshold = record_enabled ? lua_record_threshold_us() : threshold_us;
	const bool changed = m_lua_hotspot_profiler->enabled != effective_enabled ||
		m_lua_hotspot_profiler->callbacks_enabled != callbacks_enabled;
	m_lua_hotspot_profiler->enabled = effective_enabled;
	m_lua_hotspot_profiler->callbacks_enabled = callbacks_enabled;
	m_lua_hotspot_profiler->threshold_us = effective_threshold;

	if (changed)
	{
		++m_lua_profile_epoch;
		if (!m_lua_profile_epoch)
			m_lua_profile_epoch = 1;
	}

#ifdef DEBUG
	::luabind::detail::set_pcall_profile_callbacks(&luabind_profile_begin, &luabind_profile_end);
#else
	::luabind::detail::set_pcall_profile_callbacks(
		callbacks_enabled ? &luabind_profile_begin : nullptr,
		callbacks_enabled ? &luabind_profile_end : nullptr);
#endif
}

void CScriptEngine::register_profiled_function(LPCSTR function_name, const void* function_identity)
{
	if (!lua_hotspot_profiler_enabled() || !function_name || !function_name[0] || !function_identity)
		return;
	m_lua_hotspot_profiler->function_names[function_identity] = function_name;
}

void CScriptEngine::lua_record_hook(lua_State* state, lua_Debug* debug_info)
{
	lua_Debug record_info{};
	if (debug_info)
		record_info = *debug_info;

	SLuaProfileCallContext* owner = lua_record_hook_owner_for_state(state);
	if (owner && owner->previous_hook && owner->previous_hook != &CScriptEngine::lua_record_hook &&
		(owner->previous_hook_mask & lua_record_event_mask(debug_info ? debug_info->event : -1)))
	{
		owner->previous_hook(state, debug_info);
	}

	ai().script_engine().process_lua_record_hook(state, debug_info ? &record_info : nullptr);
}

void CScriptEngine::process_lua_record_hook(lua_State* state, lua_Debug* debug_info)
{
	if (!m_lua_record_state || !m_lua_record_state->active.load(std::memory_order_acquire) || !state || !debug_info)
		return;

	SLuaProfileCallContext* owner = lua_record_hook_owner_for_state(state);
	if (!owner)
		return;

	if (debug_info->event == LUA_HOOKCALL)
	{
		++m_lua_record_state->hook_call_events;
		if (!lua_record_hook_call_is_lua(state, debug_info))
		{
			++m_lua_record_state->hook_ignored_c_calls;
			return;
		}
		SLuaResolvedHookFunction function = lua_record_resolve_hook_function(m_lua_record_state, state, debug_info);

		const u64 started_ticks = CPU::QPC();
		if (function.activation_id && !owner->record_frames.empty())
		{
			s32 existing_index = -1;
			for (s32 index = static_cast<s32>(owner->record_frames.size()) - 1; index >= 0; --index)
			{
				const SLuaRecordFunctionFrame& frame = owner->record_frames[static_cast<u32>(index)];
				if (frame.state == state && frame.activation_id == function.activation_id)
				{
					existing_index = index;
					break;
				}
			}

			if (existing_index >= 0)
			{
				++m_lua_record_state->hook_inferred_tail_calls;
				const u32 frames_to_close = static_cast<u32>(owner->record_frames.size()) -
					static_cast<u32>(existing_index);
				if (frames_to_close > 1)
				{
					++m_lua_record_state->hook_resync_events;
					m_lua_record_state->hook_resync_frames += frames_to_close - 1;
				}
				for (u32 index = 0; index < frames_to_close; ++index)
					lua_record_close_top_frame(m_lua_record_state, owner, state, started_ticks);
			}
		}

		if (owner->record_overflow_depth || owner->record_frames.size() >= lua_record_max_function_depth)
		{
			++owner->record_overflow_depth;
			++m_lua_record_state->hook_overflows;
			return;
		}

		SLuaRecordFunctionFrame frame;
		frame.state = state;
		frame.activation_id = function.activation_id;
		frame.function_identity = function.identity;
		frame.function_name = function.name;
		frame.started_ticks = started_ticks;
		frame.child_ticks = 0;
		owner->record_frames.push_back(frame);
		m_lua_record_state->hook_max_depth = _max(
			m_lua_record_state->hook_max_depth, static_cast<u32>(owner->record_frames.size()));
		return;
	}

	if (debug_info->event == LUA_HOOKTAILRET)
	{
		++m_lua_record_state->hook_tail_return_events;
		if (owner->record_overflow_depth)
		{
			--owner->record_overflow_depth;
			return;
		}

		const u64 ended_ticks = CPU::QPC();
		if (!lua_record_close_matching_activation(
			m_lua_record_state, owner, state, debug_info->i_ci, ended_ticks))
		{
			if (!lua_record_close_top_frame(m_lua_record_state, owner, state, ended_ticks))
				++m_lua_record_state->hook_unmatched_returns;
		}
		return;
	}

	if (debug_info->event != LUA_HOOKRET)
		return;

	++m_lua_record_state->hook_return_events;
	const u64 ended_ticks = CPU::QPC();

	if (owner->record_overflow_depth)
	{
		--owner->record_overflow_depth;
		return;
	}

	if (lua_record_close_matching_activation(
		m_lua_record_state, owner, state, debug_info->i_ci, ended_ticks))
	{
		return;
	}

	// Fallback for non-LuaJIT/invalid activation records. Function identity is used only
	// to recover naming/stack state; normal synchronization is activation-frame based.
	SLuaResolvedHookFunction function = lua_record_resolve_hook_function(m_lua_record_state, state, debug_info);
	if (function.identity && lua_record_close_matching_identity(
		m_lua_record_state, owner, state, function.identity, ended_ticks))
	{
		return;
	}

	++m_lua_record_state->hook_unmatched_returns;
	if (!owner->record_frames.empty())
	{
		++m_lua_record_state->hook_resync_events;
		++m_lua_record_state->hook_resync_frames;
		lua_record_close_top_frame(m_lua_record_state, owner, state, ended_ticks);
	}
}

void* CScriptEngine::begin_lua_profile_call(
	lua_State* state, const int function_index, LPCSTR explicit_name)
{
#ifndef DEBUG
	if (!lua_hotspot_profiler_enabled() && !lua_recording())
		return nullptr;
#endif

#ifdef DEBUG
	debug_lua_enter(explicit_name ? explicit_name : "luabind::pcall");
#endif

	if (lua_profile_context_depth >= lua_profile_max_nested_calls)
	{
#ifdef DEBUG
		debug_lua_leave();
#endif
		// Profiling must never turn a deeply nested but otherwise valid mod call
		// chain into a crash. Keep gameplay running when the diagnostic stack is full.

		return nullptr;
	}

	SLuaProfileCallContext& context = lua_profile_contexts[lua_profile_context_depth++];
	context.engine = this;
	context.state = state;
	context.profiling = lua_hotspot_profiler_enabled();
	context.record_hook_installed = false;
	context.start_ticks = 0;
	context.gc_bytes_begin = 0;
	context.allocation_begin = SLuaAllocationStats{};
	context.function_name = nullptr;
	context.hottest_inclusive_function = nullptr;
	context.hottest_self_function = nullptr;
	context.hottest_direct_c_function = nullptr;
	context.hottest_inclusive_ticks = 0;
	context.hottest_self_ticks = 0;
	context.hottest_direct_c_ticks = 0;
	context.previous_hook = nullptr;
	context.previous_hook_mask = 0;
	context.previous_hook_count = 0;
	context.record_frames.clear();
	context.record_direct_c_calls = false;
	context.direct_c_dispatch_scope_started = false;
	context.record_overflow_depth = 0;

	if (explicit_name && explicit_name[0])
	{
		context.function_name = explicit_name;
	}
	else if (context.profiling)
	{
		const void* function_identity = nullptr;
		if (state && function_index && lua_isfunction(state, function_index))
			function_identity = lua_topointer(state, function_index);

		if (function_identity)
		{
			auto found = m_lua_hotspot_profiler->function_names.find(function_identity);
			if (found != m_lua_hotspot_profiler->function_names.end())
				context.function_name = found->second;
		}

		if (!context.function_name.size())
		{
			string512 resolved_name;
			xr_strcpy(resolved_name, "<unknown Lua function>");

			if (state && function_index && lua_isfunction(state, function_index))
			{
				lua_Debug debug_info{};
				lua_pushvalue(state, function_index);
				if (lua_getinfo(state, ">nS", &debug_info))
				{
					LPCSTR source = debug_info.short_src[0] ? debug_info.short_src : "<Lua>";
					if (debug_info.name && debug_info.name[0])
						xr_sprintf(resolved_name, "%s [%s:%d]", debug_info.name, source, debug_info.linedefined);
					else if (debug_info.linedefined > 0)
						xr_sprintf(resolved_name, "%s:%d", source, debug_info.linedefined);
					else if (debug_info.what && !xr_strcmp(debug_info.what, "C"))
						xr_sprintf(resolved_name, "<C function> [%s]", source);
					else
						xr_sprintf(resolved_name, "%s", source);
				}
			}

			context.function_name = resolved_name;
			if (function_identity)
				m_lua_hotspot_profiler->function_names[function_identity] = context.function_name;
		}
	}

	if (!context.function_name.size())
		context.function_name = explicit_name && explicit_name[0] ? explicit_name : "<Lua callback>";

	context.record_direct_c_calls = lua_recording() &&
		lua_record_contains_no_case(context.function_name.c_str(), "se_stalker.script:75");
	if (context.record_direct_c_calls)
	{
		::luabind::detail::begin_lua_to_cpp_profile_scope();
		context.direct_c_dispatch_scope_started = true;
	}

	if (context.profiling)
		lua_allocation_stats(context.allocation_begin);
	if (lua_recording())
		context.gc_bytes_begin = lua_record_gc_bytes(state);

	if (lua_recording() && state && !lua_record_hook_already_installed(state, &CScriptEngine::lua_record_hook))
	{
		context.previous_hook = lua_gethook(state);
		context.previous_hook_mask = lua_gethookmask(state);
		context.previous_hook_count = lua_gethookcount(state);
		if (context.record_frames.capacity() < lua_record_max_function_depth)
			context.record_frames.reserve(lua_record_max_function_depth);
		context.record_hook_installed = true;
		lua_sethook(state, &CScriptEngine::lua_record_hook,
			context.previous_hook_mask | LUA_MASKCALL | LUA_MASKRET,
			context.previous_hook_count);
	}
	context.start_ticks = CPU::QPC();

	return &context;
}

void CScriptEngine::end_lua_profile_call(void* opaque_context, const int result)
{
	if (!opaque_context)
		return;

	SLuaProfileCallContext* context = static_cast<SLuaProfileCallContext*>(opaque_context);
	R_ASSERT2(lua_profile_context_depth > 0 &&
		context == &lua_profile_contexts[lua_profile_context_depth - 1],
		"Lua hotspot profiler call scopes must be closed in LIFO order");

	const u64 ended_ticks = CPU::QPC();
	if (context->record_hook_installed && context->state)
	{
		if (!context->record_frames.empty() && m_lua_record_state &&
			m_lua_record_state->active.load(std::memory_order_acquire))
		{
			++m_lua_record_state->hook_resync_events;
			m_lua_record_state->hook_resync_frames += context->record_frames.size();
			while (!context->record_frames.empty())
			{
				if (!lua_record_close_top_frame(m_lua_record_state, context, context->state, ended_ticks))
				{
					context->record_frames.clear();
					break;
				}
			}
		}
		context->record_overflow_depth = 0;
		if (lua_gethook(context->state) == &CScriptEngine::lua_record_hook)
		{
			lua_sethook(context->state, context->previous_hook,
				context->previous_hook_mask, context->previous_hook_count);
		}
	}

	const u64 elapsed_ticks = ended_ticks - context->start_ticks;
	const u32 elapsed_us = lua_profile_ticks_to_us(elapsed_ticks);
	const bool hotspot_event = context->profiling && m_lua_hotspot_profiler &&
		elapsed_us >= m_lua_hotspot_profiler->threshold_us;

	if (hotspot_event)
	{
		SLuaAllocationStats allocation_end;
		lua_allocation_stats(allocation_end);
		const bool record_active = m_lua_record_state &&
			m_lua_record_state->active.load(std::memory_order_acquire);
		const u64 gc_bytes_end = record_active ? lua_record_gc_bytes(context->state) : 0;

		const u64 alloc_calls = lua_profile_counter_delta(
			allocation_end.alloc_calls, context->allocation_begin.alloc_calls);
		const u64 realloc_calls = lua_profile_counter_delta(
			allocation_end.realloc_calls, context->allocation_begin.realloc_calls);
		const u64 free_calls = lua_profile_counter_delta(
			allocation_end.free_calls, context->allocation_begin.free_calls);
		const u64 allocated_bytes = lua_profile_counter_delta(
			allocation_end.allocated_bytes, context->allocation_begin.allocated_bytes);
		const u64 freed_bytes = lua_profile_counter_delta(
			allocation_end.freed_bytes, context->allocation_begin.freed_bytes);

		if (hotspot_event)
		{
			SLuaHotspotProfileEntry& entry = m_lua_hotspot_profiler->hotspots[context->function_name];
			++entry.calls;
			entry.total_ticks += elapsed_ticks;
			entry.max_ticks = _max(entry.max_ticks, elapsed_ticks);
			entry.alloc_calls += alloc_calls;
			entry.realloc_calls += realloc_calls;
			entry.free_calls += free_calls;
			entry.allocated_bytes += allocated_bytes;
			entry.freed_bytes += freed_bytes;
			entry.errors += result == 0 || result == LUA_YIELD ? 0u : 1u;

			if (record_active)
			{
				SLuaRecordSlowCallback event;
				event.relative_ticks = ended_ticks - m_lua_record_state->started_ticks;
				event.callback = context->function_name;
				event.hottest_inclusive_function = context->hottest_inclusive_function.size() ?
					context->hottest_inclusive_function : context->function_name;
				event.hottest_self_function = context->hottest_self_function.size() ?
					context->hottest_self_function : context->function_name;
				event.hottest_direct_c_function = context->hottest_direct_c_function;
				event.elapsed_ticks = elapsed_ticks;
				event.hottest_inclusive_ticks = context->hottest_inclusive_ticks;
				event.hottest_self_ticks = context->hottest_self_ticks;
				event.hottest_direct_c_ticks = context->hottest_direct_c_ticks;
				event.alloc_calls = alloc_calls;
				event.realloc_calls = realloc_calls;
				event.free_calls = free_calls;
				event.allocated_bytes = allocated_bytes;
				event.freed_bytes = freed_bytes;
				event.gc_bytes_begin = context->gc_bytes_begin;
				event.gc_bytes_end = gc_bytes_end;
				event.result = result;
				lua_record_keep_slowest_callback(m_lua_record_state, event);
			}
		}

	}

	while (lua_to_cpp_profile_context_depth &&
		lua_to_cpp_profile_contexts[lua_to_cpp_profile_context_depth - 1].callback_context == context)
	{
		SLuaToCppProfileCallContext& stale = lua_to_cpp_profile_contexts[lua_to_cpp_profile_context_depth - 1];
		stale.engine = nullptr;
		stale.callback_context = nullptr;
		stale.profile_key = nullptr;
		stale.function_name = nullptr;
		stale.started_ticks = 0;
		--lua_to_cpp_profile_context_depth;
		if (m_lua_record_state)
			++m_lua_record_state->direct_c_profile_overflows;
	}

	if (context->direct_c_dispatch_scope_started)
	{
		::luabind::detail::end_lua_to_cpp_profile_scope();
		context->direct_c_dispatch_scope_started = false;
	}

	context->function_name = nullptr;
	context->hottest_inclusive_function = nullptr;
	context->hottest_self_function = nullptr;
	context->hottest_direct_c_function = nullptr;
	context->engine = nullptr;
	context->state = nullptr;
	context->profiling = false;
	context->record_hook_installed = false;
	context->gc_bytes_begin = 0;
	context->record_direct_c_calls = false;
	context->record_overflow_depth = 0;
	context->record_frames.clear();
	--lua_profile_context_depth;
#ifdef DEBUG
	debug_lua_leave();
#endif
}

void* CScriptEngine::begin_lua_to_cpp_profile_call(lua_State* state, LPCSTR owner_name, LPCSTR function_name)
{
	if (!m_lua_record_state || !m_lua_record_state->active.load(std::memory_order_acquire) ||
		!state || !function_name || !function_name[0] || !CPU::qpc_freq)
	{
		return nullptr;
	}

	SLuaProfileCallContext* callback_context = lua_record_context_for_state(state);
	if (!callback_context || !callback_context->record_direct_c_calls)
		return nullptr;

	if (lua_to_cpp_profile_context_depth >= lua_to_cpp_profile_max_nested_calls)
	{
		++m_lua_record_state->direct_c_profile_overflows;
		return nullptr;
	}

	SLuaToCppProfileCallContext& context = lua_to_cpp_profile_contexts[lua_to_cpp_profile_context_depth++];
	context.engine = this;
	context.callback_context = callback_context;

	string1024 resolved_name;
	if (owner_name && owner_name[0])
		_snprintf_s(resolved_name, sizeof(resolved_name), _TRUNCATE, "%s::%s", owner_name, function_name);
	else
		xr_strcpy(resolved_name, function_name);
	context.function_name = resolved_name;

	string2048 profile_key;
	_snprintf_s(profile_key, sizeof(profile_key), _TRUNCATE, "%s -> %s",
		callback_context->function_name.c_str(), resolved_name);
	context.profile_key = profile_key;
	context.started_ticks = CPU::QPC();
	++m_lua_record_state->direct_c_call_events;
	return &context;
}

void CScriptEngine::end_lua_to_cpp_profile_call(void* raw_context)
{
	if (!raw_context || !lua_to_cpp_profile_context_depth)
		return;

	SLuaToCppProfileCallContext* context = static_cast<SLuaToCppProfileCallContext*>(raw_context);
	SLuaToCppProfileCallContext* expected = &lua_to_cpp_profile_contexts[lua_to_cpp_profile_context_depth - 1];
	if (context != expected)
	{
		if (m_lua_record_state)
			++m_lua_record_state->direct_c_profile_overflows;
		return;
	}

	const u64 ended_ticks = CPU::QPC();
	const u64 elapsed_ticks = ended_ticks >= context->started_ticks ? ended_ticks - context->started_ticks : 0;
	if (m_lua_record_state)
	{
		SLuaRecordDirectCEntry& entry = m_lua_record_state->direct_c_functions[context->profile_key];
		if (!entry.calls)
		{
			entry.callback = context->callback_context ? context->callback_context->function_name : "<Lua callback>";
			entry.function = context->function_name;
		}
		++entry.calls;
		entry.total_ticks += elapsed_ticks;
		entry.max_ticks = _max(entry.max_ticks, elapsed_ticks);
		++m_lua_record_state->direct_c_completed_events;
	}

	if (context->callback_context && elapsed_ticks > context->callback_context->hottest_direct_c_ticks)
	{
		context->callback_context->hottest_direct_c_ticks = elapsed_ticks;
		context->callback_context->hottest_direct_c_function = context->function_name;
	}

	context->engine = nullptr;
	context->callback_context = nullptr;
	context->profile_key = nullptr;
	context->function_name = nullptr;
	context->started_ticks = 0;
	--lua_to_cpp_profile_context_depth;
}

void CScriptEngine::dump_lua_hotspots(const u32 limit) const
{
	if (!m_lua_hotspot_profiler)
		return;

	struct SProfileRow
	{
		shared_str name;
		SLuaHotspotProfileEntry entry;
	};

	xr_vector<SProfileRow> rows;
	rows.reserve(m_lua_hotspot_profiler->hotspots.size());
	for (const auto& item : m_lua_hotspot_profiler->hotspots)
		rows.push_back(SProfileRow{item.first, item.second});

	std::sort(rows.begin(), rows.end(), [](const SProfileRow& left, const SProfileRow& right)
	{
		return left.entry.total_ticks > right.entry.total_ticks;
	});

	const u32 row_count = _min(limit, static_cast<u32>(rows.size()));
	Msg("[Lua hotspots] enabled=%s threshold=%u us functions=%u showing=%u",
		m_lua_hotspot_profiler->enabled ? "yes" : "no",
		m_lua_hotspot_profiler->threshold_us,
		static_cast<u32>(rows.size()),
		row_count);

	for (u32 index = 0; index < row_count; ++index)
	{
		const SProfileRow& row = rows[index];
		const double total_ms = CPU::qpc_freq ?
			static_cast<double>(row.entry.total_ticks) * 1000.0 / static_cast<double>(CPU::qpc_freq) : 0.0;
		const double max_ms = CPU::qpc_freq ?
			static_cast<double>(row.entry.max_ticks) * 1000.0 / static_cast<double>(CPU::qpc_freq) : 0.0;
		const double average_us = row.entry.calls && CPU::qpc_freq ?
			static_cast<double>(row.entry.total_ticks) * 1000000.0 /
			(static_cast<double>(CPU::qpc_freq) * static_cast<double>(row.entry.calls)) : 0.0;

		Msg("[Lua hotspots] %2u. %-64s calls=%llu total=%.3f ms avg=%.2f us max=%.3f ms "
			"errors=%llu alloc=%llu realloc=%llu free=%llu allocated=%.2f KB freed=%.2f KB",
			index + 1,
			row.name.c_str(),
			static_cast<unsigned long long>(row.entry.calls),
			total_ms,
			average_us,
			max_ms,
			static_cast<unsigned long long>(row.entry.errors),
			static_cast<unsigned long long>(row.entry.alloc_calls),
			static_cast<unsigned long long>(row.entry.realloc_calls),
			static_cast<unsigned long long>(row.entry.free_calls),
			static_cast<double>(row.entry.allocated_bytes) / 1024.0,
			static_cast<double>(row.entry.freed_bytes) / 1024.0);
	}
}

void CScriptEngine::reset_lua_hotspots()
{
	if (!m_lua_hotspot_profiler)
		return;
	m_lua_hotspot_profiler->hotspots.clear();
	m_lua_hotspot_profiler->function_names.clear();
	++m_lua_profile_epoch;
	if (!m_lua_profile_epoch)
		m_lua_profile_epoch = 1;
}

bool CScriptEngine::lua_recording() const
{
	return m_lua_record_state && m_lua_record_state->active.load(std::memory_order_acquire);
}

u32 CScriptEngine::lua_record_threshold_us() const
{
	return m_lua_record_state ? m_lua_record_state->threshold_us : 0;
}

LPCSTR CScriptEngine::lua_record_file_name() const
{
	return m_lua_record_state && m_lua_record_state->file_name[0] ?
		m_lua_record_state->file_name : "";
}

bool CScriptEngine::start_lua_record(const u32 threshold_us)
{
	if (!m_lua_record_state || m_lua_record_state->active.load(std::memory_order_acquire) || !CPU::qpc_freq)
		return false;

	string_path directory;
	if (!lua_record_ensure_directory(directory, sizeof(directory)))
		return false;

	SYSTEMTIME local_time{};
	GetLocalTime(&local_time);

	HANDLE file = INVALID_HANDLE_VALUE;
	for (u32 attempt = 0; attempt < 100 && file == INVALID_HANDLE_VALUE; ++attempt)
	{
		if (!attempt)
		{
			_snprintf_s(m_lua_record_state->file_name, sizeof(m_lua_record_state->file_name), _TRUNCATE,
				"%s\\LUA-RECORD-%04u%02u%02u-%02u%02u%02u-%03u.log",
				directory,
				local_time.wYear, local_time.wMonth, local_time.wDay,
				local_time.wHour, local_time.wMinute, local_time.wSecond, local_time.wMilliseconds);
		}
		else
		{
			_snprintf_s(m_lua_record_state->file_name, sizeof(m_lua_record_state->file_name), _TRUNCATE,
				"%s\\LUA-RECORD-%04u%02u%02u-%02u%02u%02u-%03u-%u.log",
				directory,
				local_time.wYear, local_time.wMonth, local_time.wDay,
				local_time.wHour, local_time.wMinute, local_time.wSecond, local_time.wMilliseconds,
				attempt);
		}

		file = CreateFileA(m_lua_record_state->file_name, GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	if (file == INVALID_HANDLE_VALUE)
	{
		*m_lua_record_state->file_name = 0;
		return false;
	}

	m_lua_record_state->file = file;
	m_lua_record_state->threshold_us = _max(threshold_us, 1u);
	m_lua_record_state->started_ticks = CPU::QPC();
	m_lua_record_state->function_names.clear();
	m_lua_record_state->functions.clear();
	m_lua_record_state->direct_c_functions.clear();
	m_lua_record_state->native_profiles.clear();
	m_lua_record_state->slow_callbacks.clear();
	m_lua_record_state->native_events.clear();
	m_lua_record_state->slow_callbacks.reserve(lua_record_max_slow_events);
	m_lua_record_state->native_events.reserve(lua_record_max_native_events);
	m_lua_record_state->dropped_slow_callbacks = 0;
	m_lua_record_state->dropped_native_events = 0;
	m_lua_record_state->hook_overflows = 0;
	m_lua_record_state->hook_call_events = 0;
	m_lua_record_state->hook_return_events = 0;
	m_lua_record_state->hook_tail_return_events = 0;
	m_lua_record_state->hook_inferred_tail_calls = 0;
	m_lua_record_state->hook_resync_events = 0;
	m_lua_record_state->hook_resync_frames = 0;
	m_lua_record_state->hook_unmatched_returns = 0;
	m_lua_record_state->hook_ignored_c_calls = 0;
	m_lua_record_state->direct_c_call_events = 0;
	m_lua_record_state->direct_c_completed_events = 0;
	m_lua_record_state->direct_c_profile_overflows = 0;
	m_lua_record_state->hook_max_depth = 0;

	reset_lua_hotspots();
	reset_lua_allocation_stats();
	set_lua_allocation_tracking(true);
	set_lua_hotspot_profiler(true, m_lua_record_state->threshold_us);
	m_lua_record_state->active.store(true, std::memory_order_release);

	lua_record_write(file,
		"QUARK ENGINE LUA + AI TAIL RECORD\r\n"
		"started_local=%04u-%02u-%02u %02u:%02u:%02u.%03u\r\n"
		"process_id=%lu\r\n"
		"slow_callback_threshold_us=%u\r\n"
		"lua_function_mode=LuaJIT activation-frame (i_ci) CALL/RETURN resync inclusive+self time\r\n"
		"lua_tail_mode=reused activation frames treated as inferred tail-call replacement\r\n"
		"lua_self_time=excludes nested Lua calls; includes direct C-function time\r\n"
		"direct_c_mode=exact luabind Lua-to-C dispatch timing for se_stalker.script:75 callbacks\r\n"
		"native_detail=solve.search visited nodes; solve evaluator phases use condition ID/name with call counts and max timing; solution_rebuild solution size; memory.merge object counts; ai_visual detail is observer ID; alife_register detail is parent depth/result flag; alife_specific_batch detail is loaded character count; otherwise phase-specific\r\n"
		"goap_phase_accounting=solve.search is inclusive; evaluator/operator/rebuild timings are nested diagnostics and are not additive\r\n"
		"slow_callback_tail=hottest inclusive and hottest self-time Lua function\r\n"
		"slow_event_retention=top elapsed-time events, chronological output\r\n"
		"jup_b19_mode=dedicated source+line profile sorted by self time\r\n"
		"allocation_mode=deltas and Lua GC memory emitted only for callbacks at or above the slow threshold\r\n"
		"game_logic_changes=none\r\n\r\n",
		local_time.wYear, local_time.wMonth, local_time.wDay,
		local_time.wHour, local_time.wMinute, local_time.wSecond, local_time.wMilliseconds,
		GetCurrentProcessId(), m_lua_record_state->threshold_us);
	FlushFileBuffers(file);
	return true;
}

void CScriptEngine::record_native_profile(LPCSTR domain, LPCSTR phase, LPCSTR object_name,
	const u32 object_id, const u64 elapsed_ticks, const u32 detail)
{
	if (!m_lua_record_state || !m_lua_record_state->active.load(std::memory_order_acquire) || !domain || !phase)
		return;

	xrCriticalSectionGuard guard(m_lua_record_state->native_lock);
	if (!m_lua_record_state->active.load(std::memory_order_relaxed))
		return;

	string1024 key;
	xr_sprintf(key, "%s|%s", domain, phase);
	SLuaRecordNativeEntry& entry = m_lua_record_state->native_profiles[key];
	if (!entry.calls)
	{
		entry.domain = domain;
		entry.phase = phase;
	}
	++entry.calls;
	entry.total_ticks += elapsed_ticks;
	entry.max_ticks = _max(entry.max_ticks, elapsed_ticks);
	entry.detail_total += detail;
	entry.detail_max = _max(entry.detail_max, detail);

	if (lua_profile_ticks_to_us(elapsed_ticks) < m_lua_record_state->threshold_us)
		return;

	SLuaRecordNativeEvent event;
	const u64 now = CPU::QPC();
	event.relative_ticks = now >= m_lua_record_state->started_ticks ?
		now - m_lua_record_state->started_ticks : 0;
	event.domain = domain;
	event.phase = phase;
	event.object_name = object_name && object_name[0] ? object_name : "<none>";
	event.object_id = object_id;
	event.detail = detail;
	event.elapsed_ticks = elapsed_ticks;
	lua_record_keep_slowest_native_event(m_lua_record_state, event);
}

bool CScriptEngine::stop_lua_record()
{
	if (!m_lua_record_state || !m_lua_record_state->active.load(std::memory_order_acquire))
		return false;

	const u64 ended_ticks = CPU::QPC();
	m_lua_record_state->active.store(false, std::memory_order_release);

	xr_vector<SLuaRecordNativeEntry> native_profile_snapshot;
	xr_vector<SLuaRecordNativeEvent> native_event_snapshot;
	xr_vector<SLuaRecordSlowCallback> slow_callback_snapshot = m_lua_record_state->slow_callbacks;
	u64 dropped_native_events_snapshot = 0;
	{
		xrCriticalSectionGuard guard(m_lua_record_state->native_lock);
		native_profile_snapshot.reserve(m_lua_record_state->native_profiles.size());
		for (const auto& item : m_lua_record_state->native_profiles)
			native_profile_snapshot.push_back(item.second);
		native_event_snapshot = m_lua_record_state->native_events;
		dropped_native_events_snapshot = m_lua_record_state->dropped_native_events;
	}
	std::sort(slow_callback_snapshot.begin(), slow_callback_snapshot.end(),
		[](const SLuaRecordSlowCallback& left, const SLuaRecordSlowCallback& right)
		{
			return left.relative_ticks < right.relative_ticks;
		});
	std::sort(native_event_snapshot.begin(), native_event_snapshot.end(),
		[](const SLuaRecordNativeEvent& left, const SLuaRecordNativeEvent& right)
		{
			return left.relative_ticks < right.relative_ticks;
		});

	HANDLE file = m_lua_record_state->file;

	lua_record_write(file, "[SUMMARY]\r\n");
	lua_record_write(file, "duration_ms=%.3f\r\n", lua_record_ticks_to_ms(ended_ticks - m_lua_record_state->started_ticks));
	lua_record_write(file, "lua_functions=%u\r\n", static_cast<u32>(m_lua_record_state->functions.size()));
	lua_record_write(file, "slow_lua_callbacks=%u\r\n", static_cast<u32>(slow_callback_snapshot.size()));
	lua_record_write(file, "native_profiles=%u\r\n", static_cast<u32>(native_profile_snapshot.size()));
	lua_record_write(file, "slow_native_events=%u\r\n", static_cast<u32>(native_event_snapshot.size()));
	lua_record_write(file, "slow_lua_callbacks_seen=%llu\r\n",
		static_cast<unsigned long long>(slow_callback_snapshot.size() + m_lua_record_state->dropped_slow_callbacks));
	lua_record_write(file, "slow_native_events_seen=%llu\r\n",
		static_cast<unsigned long long>(native_event_snapshot.size() + dropped_native_events_snapshot));
	lua_record_write(file, "dropped_slow_lua_callbacks=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->dropped_slow_callbacks));
	lua_record_write(file, "dropped_slow_native_events=%llu\r\n",
		static_cast<unsigned long long>(dropped_native_events_snapshot));
	lua_record_write(file, "lua_hook_call_events=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_call_events));
	lua_record_write(file, "lua_hook_return_events=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_return_events));
	lua_record_write(file, "lua_hook_tail_return_events=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_tail_return_events));
	lua_record_write(file, "lua_hook_inferred_tail_calls=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_inferred_tail_calls));
	lua_record_write(file, "lua_hook_ignored_c_calls=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_ignored_c_calls));
	lua_record_write(file, "lua_hook_resync_events=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_resync_events));
	lua_record_write(file, "lua_hook_resync_frames=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_resync_frames));
	lua_record_write(file, "lua_hook_unmatched_returns=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_unmatched_returns));
	lua_record_write(file, "direct_c_call_events=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->direct_c_call_events));
	lua_record_write(file, "direct_c_completed_events=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->direct_c_completed_events));
	lua_record_write(file, "direct_c_profile_overflows=%llu\r\n",
		static_cast<unsigned long long>(m_lua_record_state->direct_c_profile_overflows));
	lua_record_write(file, "lua_hook_max_depth=%u\r\n", m_lua_record_state->hook_max_depth);
	lua_record_write(file, "lua_hook_depth_overflows=%llu\r\n\r\n",
		static_cast<unsigned long long>(m_lua_record_state->hook_overflows));

	lua_record_write(file,
		"[SLOW LUA CALLBACK EVENTS]\r\n"
		"time_ms\telapsed_ms\thottest_inclusive_ms\thottest_self_ms\thottest_direct_c_ms\tresult\talloc\trealloc\tfree\tallocated_kb\tfreed_kb\tgc_before_kb\tgc_after_kb\tgc_delta_kb\tcallback\thottest_inclusive_function\thottest_self_function\thottest_direct_c_function\r\n");
	for (const SLuaRecordSlowCallback& event : slow_callback_snapshot)
	{
		lua_record_write(file, "%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%llu\t%llu\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%s\t%s\t%s\t%s\r\n",
			lua_record_ticks_to_ms(event.relative_ticks),
			lua_record_ticks_to_ms(event.elapsed_ticks),
			lua_record_ticks_to_ms(event.hottest_inclusive_ticks),
			lua_record_ticks_to_ms(event.hottest_self_ticks),
			lua_record_ticks_to_ms(event.hottest_direct_c_ticks),
			event.result,
			static_cast<unsigned long long>(event.alloc_calls),
			static_cast<unsigned long long>(event.realloc_calls),
			static_cast<unsigned long long>(event.free_calls),
			static_cast<double>(event.allocated_bytes) / 1024.0,
			static_cast<double>(event.freed_bytes) / 1024.0,
			static_cast<double>(event.gc_bytes_begin) / 1024.0,
			static_cast<double>(event.gc_bytes_end) / 1024.0,
			(static_cast<double>(event.gc_bytes_end) - static_cast<double>(event.gc_bytes_begin)) / 1024.0,
			event.callback.c_str(), event.hottest_inclusive_function.c_str(),
			event.hottest_self_function.c_str(),
			event.hottest_direct_c_function.size() ? event.hottest_direct_c_function.c_str() : "<none>");
	}
	lua_record_write(file, "\r\n");

	struct SFunctionRow
	{
		shared_str name;
		SLuaRecordFunctionEntry entry;
	};
	xr_vector<SFunctionRow> function_rows;
	function_rows.reserve(m_lua_record_state->functions.size());
	for (const auto& item : m_lua_record_state->functions)
		function_rows.push_back(SFunctionRow{item.first, item.second});
	std::sort(function_rows.begin(), function_rows.end(), [](const SFunctionRow& left, const SFunctionRow& right)
	{
		return left.entry.total_ticks > right.entry.total_ticks;
	});

	lua_record_write(file,
		"[LUA FUNCTION PROFILE]\r\n"
		"rank\tcalls\ttotal_ms\tself_ms\tavg_us\tmax_ms\tmax_self_ms\tfunction_source_line\r\n");
	for (u32 index = 0; index < function_rows.size(); ++index)
	{
		const SFunctionRow& row = function_rows[index];
		const double average_us = row.entry.calls ?
			lua_record_ticks_to_ms(row.entry.total_ticks) * 1000.0 / static_cast<double>(row.entry.calls) : 0.0;
		lua_record_write(file, "%u\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%s\r\n",
			index + 1,
			static_cast<unsigned long long>(row.entry.calls),
			lua_record_ticks_to_ms(row.entry.total_ticks),
			lua_record_ticks_to_ms(row.entry.self_ticks),
			average_us,
			lua_record_ticks_to_ms(row.entry.max_ticks),
			lua_record_ticks_to_ms(row.entry.max_self_ticks),
			row.name.c_str());
	}
	lua_record_write(file, "\r\n");

	struct SDirectCRow
	{
		SLuaRecordDirectCEntry entry;
	};
	xr_vector<SDirectCRow> direct_c_rows;
	direct_c_rows.reserve(m_lua_record_state->direct_c_functions.size());
	for (const auto& item : m_lua_record_state->direct_c_functions)
		direct_c_rows.push_back(SDirectCRow{item.second});
	std::sort(direct_c_rows.begin(), direct_c_rows.end(), [](const SDirectCRow& left, const SDirectCRow& right)
	{
		return left.entry.total_ticks > right.entry.total_ticks;
	});

	lua_record_write(file,
		"[SE_STALKER DIRECT LUA-TO-C PROFILE]\r\n"
		"rank\tcalls\ttotal_ms\tavg_us\tmax_ms\tcallback\tc_function\r\n");
	for (u32 index = 0; index < direct_c_rows.size(); ++index)
	{
		const SLuaRecordDirectCEntry& row = direct_c_rows[index].entry;
		const double average_us = row.calls ?
			lua_record_ticks_to_ms(row.total_ticks) * 1000.0 / static_cast<double>(row.calls) : 0.0;
		lua_record_write(file, "%u\t%llu\t%.3f\t%.3f\t%.3f\t%s\t%s\r\n",
			index + 1, static_cast<unsigned long long>(row.calls),
			lua_record_ticks_to_ms(row.total_ticks), average_us, lua_record_ticks_to_ms(row.max_ticks),
			row.callback.c_str(), row.function.c_str());
	}
	if (direct_c_rows.empty())
		lua_record_write(file, "<no se_stalker direct C call observed>\r\n");
	lua_record_write(file, "\r\n");

	xr_vector<SFunctionRow> jup_b19_rows;
	for (const SFunctionRow& row : function_rows)
	{
		if (lua_record_contains_no_case(row.name.c_str(), "jup_b19"))
			jup_b19_rows.push_back(row);
	}
	std::sort(jup_b19_rows.begin(), jup_b19_rows.end(), [](const SFunctionRow& left, const SFunctionRow& right)
	{
		return left.entry.self_ticks > right.entry.self_ticks;
	});

	lua_record_write(file,
		"[JUP_B19 LUA FUNCTION PROFILE]\r\n"
		"rank\tcalls\ttotal_ms\tself_ms\tmax_ms\tmax_self_ms\tfunction_source_line\r\n");
	for (u32 index = 0; index < jup_b19_rows.size(); ++index)
	{
		const SFunctionRow& row = jup_b19_rows[index];
		lua_record_write(file, "%u\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%s\r\n",
			index + 1,
			static_cast<unsigned long long>(row.entry.calls),
			lua_record_ticks_to_ms(row.entry.total_ticks),
			lua_record_ticks_to_ms(row.entry.self_ticks),
			lua_record_ticks_to_ms(row.entry.max_ticks),
			lua_record_ticks_to_ms(row.entry.max_self_ticks),
			row.name.c_str());
	}
	if (jup_b19_rows.empty())
		lua_record_write(file, "<no jup_b19 Lua function observed>\r\n");
	lua_record_write(file, "\r\n");

	lua_record_write(file,
		"[JUP_B19 SLOW CALLBACK EVENTS]\r\n"
		"time_ms\telapsed_ms\thottest_inclusive_ms\thottest_self_ms\tresult\talloc\trealloc\tfree\tallocated_kb\tfreed_kb\tgc_before_kb\tgc_after_kb\tgc_delta_kb\tcallback\thottest_inclusive_function\thottest_self_function\r\n");
	u32 jup_b19_event_count = 0;
	for (const SLuaRecordSlowCallback& event : slow_callback_snapshot)
	{
		if (!lua_record_contains_no_case(event.callback.c_str(), "jup_b19") &&
			!lua_record_contains_no_case(event.hottest_inclusive_function.c_str(), "jup_b19") &&
			!lua_record_contains_no_case(event.hottest_self_function.c_str(), "jup_b19"))
		{
			continue;
		}
		++jup_b19_event_count;
		lua_record_write(file, "%.3f\t%.3f\t%.3f\t%.3f\t%d\t%llu\t%llu\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%s\t%s\t%s\r\n",
			lua_record_ticks_to_ms(event.relative_ticks),
			lua_record_ticks_to_ms(event.elapsed_ticks),
			lua_record_ticks_to_ms(event.hottest_inclusive_ticks),
			lua_record_ticks_to_ms(event.hottest_self_ticks),
			event.result,
			static_cast<unsigned long long>(event.alloc_calls),
			static_cast<unsigned long long>(event.realloc_calls),
			static_cast<unsigned long long>(event.free_calls),
			static_cast<double>(event.allocated_bytes) / 1024.0,
			static_cast<double>(event.freed_bytes) / 1024.0,
			static_cast<double>(event.gc_bytes_begin) / 1024.0,
			static_cast<double>(event.gc_bytes_end) / 1024.0,
			(static_cast<double>(event.gc_bytes_end) - static_cast<double>(event.gc_bytes_begin)) / 1024.0,
			event.callback.c_str(), event.hottest_inclusive_function.c_str(),
			event.hottest_self_function.c_str());
	}
	if (!jup_b19_event_count)
		lua_record_write(file, "<no slow jup_b19 callback observed>\r\n");
	lua_record_write(file, "\r\n");

	struct SHotspotRow
	{
		shared_str name;
		SLuaHotspotProfileEntry entry;
	};
	xr_vector<SHotspotRow> hotspot_rows;
	if (m_lua_hotspot_profiler)
	{
		hotspot_rows.reserve(m_lua_hotspot_profiler->hotspots.size());
		for (const auto& item : m_lua_hotspot_profiler->hotspots)
			hotspot_rows.push_back(SHotspotRow{item.first, item.second});
	}
	std::sort(hotspot_rows.begin(), hotspot_rows.end(), [](const SHotspotRow& left, const SHotspotRow& right)
	{
		return left.entry.total_ticks > right.entry.total_ticks;
	});

	lua_record_write(file,
		"[SLOW LUA CALLBACK AGGREGATES]\r\n"
		"rank\tslow_calls\ttotal_ms\tavg_us\tmax_ms\terrors\talloc\trealloc\tfree\tallocated_kb\tfreed_kb\tcallback\r\n");
	for (u32 index = 0; index < hotspot_rows.size(); ++index)
	{
		const SHotspotRow& row = hotspot_rows[index];
		const double average_us = row.entry.calls ?
			lua_record_ticks_to_ms(row.entry.total_ticks) * 1000.0 / static_cast<double>(row.entry.calls) : 0.0;
		lua_record_write(file, "%u\t%llu\t%.3f\t%.3f\t%.3f\t%llu\t%llu\t%llu\t%llu\t%.3f\t%.3f\t%s\r\n",
			index + 1,
			static_cast<unsigned long long>(row.entry.calls),
			lua_record_ticks_to_ms(row.entry.total_ticks), average_us,
			lua_record_ticks_to_ms(row.entry.max_ticks),
			static_cast<unsigned long long>(row.entry.errors),
			static_cast<unsigned long long>(row.entry.alloc_calls),
			static_cast<unsigned long long>(row.entry.realloc_calls),
			static_cast<unsigned long long>(row.entry.free_calls),
			static_cast<double>(row.entry.allocated_bytes) / 1024.0,
			static_cast<double>(row.entry.freed_bytes) / 1024.0,
			row.name.c_str());
	}
	lua_record_write(file, "\r\n");

	struct SNativeRow
	{
		SLuaRecordNativeEntry entry;
	};
	xr_vector<SNativeRow> native_rows;
	native_rows.reserve(native_profile_snapshot.size());
	for (const SLuaRecordNativeEntry& item : native_profile_snapshot)
		native_rows.push_back(SNativeRow{item});
	std::sort(native_rows.begin(), native_rows.end(), [](const SNativeRow& left, const SNativeRow& right)
	{
		return left.entry.total_ticks > right.entry.total_ticks;
	});

	lua_record_write(file,
		"[ACTOR/BINDER LUA + AI SUBPHASE PROFILE]\r\n"
		"rank\tdomain\tphase\tcalls\ttotal_ms\tavg_us\tmax_ms\tdetail_total\tdetail_avg\tdetail_max\r\n");
	for (u32 index = 0; index < native_rows.size(); ++index)
	{
		const SLuaRecordNativeEntry& row = native_rows[index].entry;
		const double average_us = row.calls ?
			lua_record_ticks_to_ms(row.total_ticks) * 1000.0 / static_cast<double>(row.calls) : 0.0;
		const double detail_average = row.calls ?
			static_cast<double>(row.detail_total) / static_cast<double>(row.calls) : 0.0;
		lua_record_write(file, "%u\t%s\t%s\t%llu\t%.3f\t%.3f\t%.3f\t%llu\t%.3f\t%u\r\n",
			index + 1, row.domain.c_str(), row.phase.c_str(),
			static_cast<unsigned long long>(row.calls),
			lua_record_ticks_to_ms(row.total_ticks), average_us,
			lua_record_ticks_to_ms(row.max_ticks),
			static_cast<unsigned long long>(row.detail_total), detail_average, row.detail_max);
	}
	lua_record_write(file, "\r\n");

	lua_record_write(file,
		"[SLOW ACTOR/BINDER LUA + AI EVENTS]\r\n"
		"time_ms\tdomain\tphase\tobject\tobject_id\telapsed_ms\tdetail\r\n");
	for (const SLuaRecordNativeEvent& event : native_event_snapshot)
	{
		lua_record_write(file, "%.3f\t%s\t%s\t%s\t%u\t%.3f\t%u\r\n",
			lua_record_ticks_to_ms(event.relative_ticks),
			event.domain.c_str(), event.phase.c_str(), event.object_name.c_str(),
			event.object_id, lua_record_ticks_to_ms(event.elapsed_ticks), event.detail);
	}
	luaJIT_JITStats jit_stats = {};
	luaJIT_getjitstats(lua(), &jit_stats);
	const double jit_reserved_mib = static_cast<double>(jit_stats.mcode_reserved_bytes) / (1024.0 * 1024.0);
	const double jit_trace_mib = static_cast<double>(jit_stats.trace_mcode_bytes) / (1024.0 * 1024.0);
	const double jit_limit_mib = static_cast<double>(jit_stats.mcode_limit_bytes) / (1024.0 * 1024.0);
	lua_record_write(file,
		"[LUAJIT CACHE TELEMETRY]\r\n"
		"traces=%u/%u trace_mcode=%.3fMiB reserved_mcode=%.3f/%.3fMiB area=%uKiB\r\n"
		"starts=%llu root_starts=%llu side_starts=%llu compiled=%llu aborts=%llu "
		"root_aborts=%llu side_aborts=%llu blacklisted=%llu side_nyi_disabled=%llu "
		"root_nyi_backoffs=%llu root_nyi_suppressed=%llu\r\n"
		"flushes=%llu maxtrace_flushes=%llu mcode_retries=%llu mcode_limit_hits=%llu mcode_alloc_failures=%llu\r\n"
		"abort_mcode=%llu abort_nyi=%llu abort_blacklist=%llu\r\n\r\n",
		jit_stats.active_traces, jit_stats.max_traces, jit_trace_mib, jit_reserved_mib, jit_limit_mib,
		jit_stats.mcode_area_kb,
		jit_stats.trace_starts, jit_stats.root_trace_starts, jit_stats.side_trace_starts,
		jit_stats.trace_compiled, jit_stats.trace_aborts,
		jit_stats.root_trace_aborts, jit_stats.side_trace_aborts,
		jit_stats.blacklisted_sites, jit_stats.side_nyi_disabled,
		jit_stats.root_nyi_backoffs, jit_stats.root_nyi_suppressed,
		jit_stats.trace_flushes, jit_stats.maxtrace_flushes, jit_stats.mcode_retries,
		jit_stats.mcode_limit_hits, jit_stats.mcode_alloc_failures,
		jit_stats.abort_mcode, jit_stats.abort_nyi, jit_stats.abort_blacklist);

	luaJIT_JITAbortSite jit_abort_sites[32] = {};
	const unsigned int jit_abort_site_count = luaJIT_getjitabortsites(lua(), jit_abort_sites, 32);
	std::sort(jit_abort_sites, jit_abort_sites + jit_abort_site_count,
		[](const luaJIT_JITAbortSite& left, const luaJIT_JITAbortSite& right)
		{
			return left.count > right.count;
		});
	lua_record_write(file,
		"[LUAJIT NYI ABORT SITES]\r\n"
		"rank\tcount\ttrace_kind\treason\tsource\tline\r\n");
	for (unsigned int index = 0; index < jit_abort_site_count; ++index)
	{
		const luaJIT_JITAbortSite& site = jit_abort_sites[index];
		lua_record_write(file, "%u\t%llu\t%s\t%u\t%s\t%u\r\n",
			index + 1, site.count, site.side_trace ? "side" : "root",
			site.reason, site.source[0] ? site.source : "<unknown>", site.line);
	}
	lua_record_write(file, "\r\nEND OF RECORD\r\n");

	if (file != INVALID_HANDLE_VALUE)
	{
		FlushFileBuffers(file);
		CloseHandle(file);
	}
	m_lua_record_state->file = INVALID_HANDLE_VALUE;

	set_lua_hotspot_profiler(false, 0);
	set_lua_allocation_tracking(false);
	return true;
}

CScriptEngine::~CScriptEngine()
{
	stop_lua_record();
	::luabind::detail::set_pcall_profile_callbacks(nullptr, nullptr);
	release_script_functors();

	while (!m_script_processes.empty())
		remove_script_process(m_script_processes.begin()->first);

#ifdef LUA_DEBUG_PRINT
    flush_log();
#endif //-LUA_DEBUG_PRINT

#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
    xr_delete				(m_scriptDebugger);
#	else // #ifndef USE_LUA_STUDIO
    disconnect_from_debugger();
#	endif // #ifndef USE_LUA_STUDIO
#endif

	xr_delete(m_lua_hotspot_profiler);
	xr_delete(m_lua_record_state);
}

void CScriptEngine::unload()
{
	invalidate_script_functors();
	lua_settop(lua(), m_stack_level);
	m_last_no_file_length = 0;
	*m_last_no_file = 0;
}

int CScriptEngine::lua_panic(lua_State* L)
{
	ai().script_engine().print_stack();
	print_output(L, "PANIC", LUA_ERRRUN);
	return (0);
}

// demonized: get lua stack in array
xr_vector<xr_string> get_lua_stack(lua_State* L)
{
	xr_vector<xr_string> res;
	lua_Debug l_tDebugInfo;
	for (int i = 0; lua_getstack(L, i, &l_tDebugInfo); ++i)
	{
		lua_getinfo(L, "nSlu", &l_tDebugInfo);
		if (!l_tDebugInfo.name)
		{
			res.push_back(make_string("%2d : [%s] %s(%d) : %s", i, l_tDebugInfo.what, l_tDebugInfo.short_src, l_tDebugInfo.currentline, "").c_str());
		} else
		{
			if (!xr_strcmp(l_tDebugInfo.what, "C"))
			{
				res.push_back(make_string("%2d : [C  ] %s", i, l_tDebugInfo.name).c_str());
			} else
			{
				res.push_back(make_string("%2d : [%s] %s(%d) : %s", i, l_tDebugInfo.what, l_tDebugInfo.short_src, l_tDebugInfo.currentline, l_tDebugInfo.name).c_str());
			}
		}
	}
	return res;
}

namespace
{
bool lua_error_is_out_of_memory(lua_State* L)
{
	if (!L || lua_type(L, -1) != LUA_TSTRING)
		return false;

	LPCSTR message = lua_tostring(L, -1);
	return message && xr_strcmp(message, "not enough memory") == 0;
}

void log_lua_oom_state(lua_State* L)
{
	if (!L)
		return;

	luaJIT_GCStateInfo gc = {};
	luaJIT_getgcstate(L, &gc);

#ifdef _WIN64
	xr_luajit_pool_stats pool = {};
	XR_GET_POOL_STATS(&pool);
	Msg("! [Lua OOM] heap=%llu KB threshold=%llu KB debt=%llu KB state=%d active=%d "
		"pool_committed=%llu/%llu KB pool_fallback=%llu KB fallback_allocs=%llu failures=%llu",
		static_cast<u64>(gc.total >> 10),
		static_cast<u64>(gc.threshold >> 10),
		static_cast<u64>(gc.debt >> 10), gc.state, gc.active,
		static_cast<u64>(pool.committed_bytes >> 10),
		static_cast<u64>(pool.reserved_bytes >> 10),
		static_cast<u64>(pool.fallback_bytes >> 10),
		static_cast<u64>(pool.fallback_active_allocations),
		static_cast<u64>(pool.allocation_failures));
#else
	Msg("! [Lua OOM] heap=%llu KB threshold=%llu KB debt=%llu KB state=%d active=%d",
		static_cast<u64>(gc.total >> 10),
		static_cast<u64>(gc.threshold >> 10),
		static_cast<u64>(gc.debt >> 10), gc.state, gc.active);
#endif
}

void report_lua_oom_fatal(lua_State* L)
{
	log_lua_oom_state(L);
#if !XRAY_EXCEPTIONS
	Debug.fatal(DEBUG_INFO, "\n\nLUA error: not enough memory\n\nLuaJIT low-address memory exhausted; check log for allocator/GC state");
#else
	throw "not enough memory";
#endif
}
}

void CScriptEngine::lua_error(lua_State* L)
{
	if (lua_error_is_out_of_memory(L))
	{
		// Stack walking, debugger hooks and traceback construction may allocate.
		// Under LuaJIT OOM keep the error path allocation-free on the Lua side.
		report_lua_oom_fatal(L);
		return;
	}
	ai().script_engine().print_stack();
	print_output(L, "", LUA_ERRRUN);
	ai().script_engine().on_error(L);

	// demonized: print first line with lua error
	auto stack = get_lua_stack(L);
	xr_string lua_error_line = "";
	for (auto const& s : stack) {
		if (s.find("[Lua]") != xr_string::npos) {
			lua_error_line = s;
			break;
		}
	}

	auto error_str = make_string("\n%s\n\nLUA error: %s\n\nCheck log for details", lua_error_line.c_str(), lua_tostring(L, -1));
	LPCSTR error_msg = error_str.c_str();

#if !XRAY_EXCEPTIONS
	Debug.fatal(DEBUG_INFO, error_msg);
#else
    throw					lua_tostring(L,-1);
#endif
}

extern BOOL lua_busy_hands_debug;
void CScriptEngine::lua_error_not_crash(lua_State* L)
{
    if (!lua_busy_hands_debug)
        return;

    ai().script_engine().script_log(ScriptStorage::eLuaMessageTypeError, "[BusyHandsDebug] Runtime Error");
    auto stack = get_lua_stack(ai().script_engine().lua());

    xr_string lua_error_line = "";
    for (auto const& s : stack)
    {
        if (s.find("[Lua]") != xr_string::npos)
        {
            lua_error_line = s;
            break;
        }
    }

    ::luabind::functor<void> funct;
    if (ai().script_engine().functor("_G.COnLuaBindFatalError", funct))
        funct(lua_error_line.c_str());
}

void printLuaStack()
{
	ai().script_engine().print_stack();
}

int CScriptEngine::lua_pcall_failed(lua_State* L)
{
	if (lua_error_is_out_of_memory(L))
	{
		// This function is Lua's message handler. Do not try to build a traceback
		// after the VM has already failed an allocation. Keep the original error
		// object on the stack so the handler still obeys the Lua C API contract.
		report_lua_oom_fatal(L);
		return 1;
	}

	ai().script_engine().print_stack();
	print_output(L, "", LUA_ERRRUN);
	ai().script_engine().on_error(L);

	// demonized: print first line with lua error
	auto stack = get_lua_stack(L);
	xr_string lua_error_line = "";
	for (auto const& s : stack) {
		if (s.find("[Lua]") != xr_string::npos) {
			lua_error_line = s;
			break;
		}
	}

	auto error_str = make_string("\n%s\n\nLUA error: %s\n\nCheck log for details", lua_error_line.c_str(), lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
	LPCSTR error_msg = error_str.c_str();

#if !XRAY_EXCEPTIONS
	Debug.fatal(DEBUG_INFO, error_msg);
#endif
	// A lua_pcall message handler returns the number of replacement error
	// objects. The old code popped the only error and returned LUA_ERRRUN (2),
	// which is an error code, not a result count. Preserve one error object.
	return 1;
}

void lua_cast_failed(lua_State* L, LUABIND_TYPE_INFO info)
{
	CScriptEngine::print_output(L, "", LUA_ERRRUN);

	Debug.fatal(DEBUG_INFO, "LUA error: cannot cast lua value to %s", info->name());
}

void CScriptEngine::setup_callbacks()
{
#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
    if( debugger() )
        debugger()->PrepareLuaBind	();
#	endif // #ifndef USE_LUA_STUDIO
#endif

#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
    if (!debugger() || !debugger()->Active() )
#	endif // #ifndef USE_LUA_STUDIO
#endif
	{
#if !XRAY_EXCEPTIONS
		::luabind::set_error_callback(CScriptEngine::lua_error);
        ::luabind::set_error_callback_not_crash(CScriptEngine::lua_error_not_crash);
#endif

		::luabind::set_pcall_callback(CScriptEngine::lua_pcall_failed);
	}

#if !XRAY_EXCEPTIONS
	::luabind::set_cast_failed_callback(lua_cast_failed);
#endif
	lua_atpanic(lua(), CScriptEngine::lua_panic);
}

#ifdef DEBUG
#	include "script_thread.h"
void CScriptEngine::lua_hook_call		(lua_State *L, lua_Debug *dbg)
{
    if (ai().script_engine().current_thread())
        ai().script_engine().current_thread()->script_hook(L,dbg);
    else
        ai().script_engine().m_stack_is_ready	= true;
}
#endif

int auto_load(lua_State* L)
{
	if ((lua_gettop(L) < 2) || !lua_istable(L, 1) || !lua_isstring(L, 2))
	{
		lua_pushnil(L);
		return (1);
	}

	ai().script_engine().process_file_if_exists(lua_tostring(L, 2), false);
	lua_rawget(L, 1);
	return (1);
}

void CScriptEngine::setup_auto_load()
{
	luaL_newmetatable(lua(), "XRAY_AutoLoadMetaTable");
	lua_pushstring(lua(), "__index");
	lua_pushcfunction(lua(), auto_load);
	lua_settable(lua(), -3);
	lua_pushstring(lua(), "_G");
	lua_gettable(lua(), LUA_GLOBALSINDEX);
	luaL_getmetatable(lua(), "XRAY_AutoLoadMetaTable");
	lua_setmetatable(lua(), -2);
	//. ??????????
	// lua_settop							(lua(),-0);
}

extern void export_classes(lua_State* L);
extern xr_unordered_map<std::string, std::set<std::string>> unlocalizers;
extern bool unlocalizerPassed;

namespace
{
constexpr LPCSTR kQuarkAlifeObjectFallbackRegistryKey = "QUARK_ENGINE.alife_object_fallback";

void install_quark_hot_global_fast_paths(lua_State* L)
{
	const int stack_top = lua_gettop(L);

	// _G.script owns the public compatibility wrapper. Preserve it in the Lua
	// registry so the native fast path can delegate every non-hot/invalid case
	// back to the exact script implementation.
	lua_getglobal(L, "alife_object");
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, stack_top);
		return;
	}
	lua_setfield(L, LUA_REGISTRYINDEX, kQuarkAlifeObjectFallbackRegistryKey);

	lua_getglobal(L, "__quark_alife_object_fast");
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, stack_top);
		return;
	}
	lua_setglobal(L, "alife_object");

	// The helper remains referenced by alife_object itself; do not leave an
	// additional public global that scripts could start depending on.
	lua_pushnil(L);
	lua_setglobal(L, "__quark_alife_object_fast");

	// _G.script wraps device():time_global() in Lua. Restore the already
	// registered direct C bridge after _G has replaced the early export.
	lua_getglobal(L, "__quark_time_global_fast");
	if (lua_isfunction(L, -1))
		lua_setglobal(L, "time_global");
	else
		lua_pop(L, 1);

	lua_pushnil(L);
	lua_setglobal(L, "__quark_time_global_fast");
	lua_settop(L, stack_top);
}
}

void CScriptEngine::init()
{
	invalidate_script_functors();
#ifdef USE_LUA_STUDIO
    bool lua_studio_connected = !!m_lua_studio_world;
    if (lua_studio_connected)
        m_lua_studio_world->remove		(lua());
#endif // #ifdef USE_LUA_STUDIO

	CScriptStorage::reinit();

#ifdef USE_LUA_STUDIO
    if (m_lua_studio_world || strstr(Core.Params, "-lua_studio")) {
        if (!lua_studio_connected)
            try_connect_to_debugger		();
        else {
#ifdef USE_LUAJIT_ONE
            jit_command					(lua(), "debug=2");
            jit_command					(lua(), "off");
#else
            luaJIT_setmode(lua(), 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
#endif
            m_lua_studio_world->add		(lua());
        }
    }
#endif // #ifdef USE_LUA_STUDIO

	::luabind::open(lua());
	setup_callbacks();
	export_classes(lua());
	setup_auto_load();

#ifdef DEBUG
    m_stack_is_ready					= true;
#endif

#ifndef USE_LUA_STUDIO
#	ifdef DEBUG
#		if defined(USE_DEBUGGER) && !defined(USE_LUA_STUDIO)
    if( !debugger() || !debugger()->Active()  )
#		endif // #if defined(USE_DEBUGGER) && !defined(USE_LUA_STUDIO)
        lua_sethook					(lua(),lua_hook_call,	LUA_MASKLINE|LUA_MASKCALL|LUA_MASKRET,	0);
#	endif // #ifdef DEBUG
#endif // #ifndef USE_LUA_STUDIO
	//	lua_sethook							(lua(), lua_hook_call,	LUA_MASKLINE|LUA_MASKCALL|LUA_MASKRET,	0);

	unlocalizers.clear();
	unlocalizers.rehash(0);
	unlocalizerPassed = false;
	bool save = m_reload_modules;
	m_reload_modules = true;
	process_file_if_exists("_G", false);
	m_reload_modules = save;

#ifdef XRGAME_EXPORTS
	install_quark_hot_global_fast_paths(lua());
#endif

	register_script_classes();
	object_factory().register_script();

#ifdef XRGAME_EXPORTS
	load_common_scripts();
#endif
	m_stack_level = lua_gettop(lua());

    if (strstr(Core.Params, "-ldbg")) {
        CScriptStorage::DebuggerAttach();
    }
}

void CScriptEngine::remove_script_process(const EScriptProcessors& process_id)
{
	CScriptProcessStorage::iterator I = m_script_processes.find(process_id);
	if (I != m_script_processes.end())
	{
		xr_delete((*I).second);
		m_script_processes.erase(I);
	}
}

void CScriptEngine::load_common_scripts()
{
#ifdef DBG_DISABLE_SCRIPTS
    return;
#endif
	string_path S;
	FS.update_path(S, "$game_config$", "script.ltx");
	CInifile* l_tpIniFile = xr_new<CInifile>(S);
	R_ASSERT(l_tpIniFile);
	if (!l_tpIniFile->section_exist("common"))
	{
		xr_delete(l_tpIniFile);
		return;
	}

	if (l_tpIniFile->line_exist("common", "script"))
	{
		LPCSTR caScriptString = l_tpIniFile->r_string("common", "script");
		u32 n = _GetItemCount(caScriptString);
		string256 I;
		for (u32 i = 0; i < n; ++i)
		{
			process_file(_GetItem(caScriptString, i, I));
			xr_strcat(I, "_initialize");
			if (object("_G", I, LUA_TFUNCTION))
			{
				//				lua_dostring			(lua(),xr_strcat(I,"()"));
				::luabind::functor<void> f;
				R_ASSERT(functor(I, f));
				f();
			}
		}
	}

	xr_delete(l_tpIniFile);
}

void CScriptEngine::process_file_if_exists(LPCSTR file_name, bool warn_if_not_exist)
{
	u32 string_length = xr_strlen(file_name);
	if (!warn_if_not_exist && no_file_exists(file_name, string_length))
		return;

	string_path S, S1;
	if (m_reload_modules || (*file_name && !namespace_loaded(file_name)))
	{
		FS.update_path(S, "$game_scripts$", strconcat(sizeof(S1), S1, file_name, ".script"));
		if (!warn_if_not_exist && !FS.exist(S))
		{
#ifdef DEBUG
#	ifndef XRSE_FACTORY_EXPORTS
            if (psAI_Flags.test(aiNilObjectAccess))
#	endif
            {
                print_stack			();
                Msg					("* trying to access variable %s, which doesn't exist, or to load script %s, which doesn't exist too",file_name,S);
                m_stack_is_ready	= true;
            }
#endif
			add_no_file(file_name, string_length);
			return;
		}
		//#ifndef MASTER_GOLD
		if (strstr(Core.Params, "-dbg"))
			Msg("* loading script %s", S1);
		//#endif // MASTER_GOLD
		m_reload_modules = false;
		if (load_file_into_namespace(S, *file_name ? file_name : "_G"))
			advance_script_generation();
	}
}

void CScriptEngine::process_file(LPCSTR file_name)
{
	process_file_if_exists(file_name, true);
}

void CScriptEngine::process_file(LPCSTR file_name, bool reload_modules)
{
	m_reload_modules = reload_modules;
	process_file_if_exists(file_name, true);
	m_reload_modules = false;
}

void CScriptEngine::register_script_classes()
{
#ifdef DBG_DISABLE_SCRIPTS
    return;
#endif
	string_path S;
	FS.update_path(S, "$game_config$", "script.ltx");
	CInifile* l_tpIniFile = xr_new<CInifile>(S);
	R_ASSERT(l_tpIniFile);

	if (!l_tpIniFile->section_exist("common"))
	{
		xr_delete(l_tpIniFile);
		return;
	}

	m_class_registrators = READ_IF_EXISTS(l_tpIniFile, r_string, "common", "class_registrators", "");
	xr_delete(l_tpIniFile);

	u32 n = _GetItemCount(*m_class_registrators);
	string256 I;
	for (u32 i = 0; i < n; ++i)
	{
		_GetItem(*m_class_registrators, i, I);
		::luabind::functor<void> result;
		if (!functor(I, result))
		{
			script_log(eLuaMessageTypeError, "Cannot load class registrator %s!", I);
			continue;
		}
		result(const_cast<CObjectFactory*>(&object_factory()));
	}
}

bool CScriptEngine::function_object(LPCSTR function_to_call, ::luabind::object& object, int type)
{
	if (!xr_strlen(function_to_call))
		return (false);

	string256 name_space, function;

	parse_script_namespace(function_to_call, name_space, sizeof(name_space), function, sizeof(function));
	if (xr_strcmp(name_space, "_G"))
	{
		LPSTR file_name = strchr(name_space, '.');
		if (!file_name)
			process_file(name_space);
		else
		{
			*file_name = 0;
			process_file(name_space);
			*file_name = '.';
		}
	}

	if (!this->object(name_space, function, type))
		return (false);

	::luabind::object lua_namespace = this->name_space(name_space);
	object = lua_namespace[function];
	return (true);
}

#if defined(USE_DEBUGGER) && !defined(USE_LUA_STUDIO)
void CScriptEngine::stopDebugger				()
{
    if (debugger()){
        xr_delete	(m_scriptDebugger);
        Msg			("Script debugger succesfully stoped.");
    }
    else
        Msg			("Script debugger not present.");
}

void CScriptEngine::restartDebugger				()
{
    if(debugger())
        stopDebugger();

    m_scriptDebugger = xr_new<CScriptDebugger>();
    debugger()->PrepareLuaBind();
    Msg				("Script debugger succesfully restarted.");
}
#endif // #if defined(USE_DEBUGGER) && !defined(USE_LUA_STUDIO)

bool CScriptEngine::no_file_exists(LPCSTR file_name, u32 string_length)
{
	if (m_last_no_file_length != string_length)
		return (false);

	return (!memcmp(m_last_no_file, file_name, string_length * sizeof(char)));
}

void CScriptEngine::add_no_file(LPCSTR file_name, u32 string_length)
{
	m_last_no_file_length = string_length;
	CopyMemory(m_last_no_file, file_name, (string_length + 1)*sizeof(char));
}

void CScriptEngine::collect_all_garbage()
{
	CScriptLuaStateGuard lua_guard(*this, "collect_all_garbage");
	lua_gc(lua(), LUA_GCCOLLECT, 0);
	lua_gc(lua(), LUA_GCCOLLECT, 0);
}

void CScriptEngine::on_error(lua_State* state)
{
#if defined(USE_DEBUGGER) && defined(USE_LUA_STUDIO)
    if (!debugger())
        return;

    debugger()->on_error	( state );
#endif // #if defined(USE_DEBUGGER) && defined(USE_LUA_STUDIO)
}
