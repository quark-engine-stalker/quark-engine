////////////////////////////////////////////////////////////////////////////
//	Module 		: script_engine.h
//	Created 	: 01.04.2004
//  Modified 	: 01.04.2004
//	Author		: Dmitriy Iassenev
//	Description : XRay Script Engine
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "script_storage.h"
#include "script_export_space.h"
#include "script_space_forward.h"
#include "associative_vector.h"

//AVO: lua re-org
#include "lua.hpp"
/*extern "C" {
#include <lua/lua.h>
}*/
//-AVO

//#define DBG_DISABLE_SCRIPTS

#include "script_engine_space.h"
#include <luabind/functor.hpp>

class CScriptProcess;
class CScriptThread;
struct lua_State;
struct lua_Debug;
struct SLuaHotspotProfilerState;
struct SLuaRecordState;

class CScriptEngine;

class ICachedScriptFunctor
{
public:
	virtual ~ICachedScriptFunctor() = default;
	virtual void invalidate_script_functor() = 0;
	virtual void detach_script_engine(CScriptEngine* engine) = 0;
};

#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
		class CScriptDebugger;
#	else // #ifndef USE_LUA_STUDIO
		namespace cs {
			namespace lua_studio {
				struct world;
			} // namespace lua_studio
		} // namespace cs

		class lua_studio_engine;
#	endif // #ifndef USE_LUA_STUDIO
#endif

class CScriptEngine : public CScriptStorage
{
public:
	typedef CScriptStorage inherited;
	typedef ScriptEngine::EScriptProcessors EScriptProcessors;
	typedef associative_vector<EScriptProcessors, CScriptProcess*> CScriptProcessStorage;

private:
	template <typename TResult>
	friend class cached_script_functor;

	bool m_reload_modules;
	u32 m_script_generation;
	u32 m_lua_profile_epoch;
	xr_vector<ICachedScriptFunctor*> m_cached_functors;
	SLuaHotspotProfilerState* m_lua_hotspot_profiler;
	SLuaRecordState* m_lua_record_state;

protected:
	CScriptProcessStorage m_script_processes;
	int m_stack_level;
	shared_str m_class_registrators;

protected:
#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
		CScriptDebugger			*m_scriptDebugger;
#	else // #ifndef USE_LUA_STUDIO
		cs::lua_studio::world*	m_lua_studio_world;
		lua_studio_engine*		m_lua_studio_engine;
#	endif // #ifndef USE_LUA_STUDIO
#endif // #ifdef USE_DEBUGGER

private:
	string128 m_last_no_file;
	u32 m_last_no_file_length;

	bool no_file_exists(LPCSTR file_name, u32 string_length);
	void add_no_file(LPCSTR file_name, u32 string_length);
	void register_cached_functor(ICachedScriptFunctor* functor);
	void unregister_cached_functor(ICachedScriptFunctor* functor);
	void advance_script_generation();
	void invalidate_script_functors();
	void release_script_functors();
	void register_profiled_function(LPCSTR function_name, const void* function_identity);
	static void lua_record_hook(lua_State* state, lua_Debug* debug_info);
	void process_lua_record_hook(lua_State* state, lua_Debug* debug_info);

public:
	CScriptEngine();
	virtual ~CScriptEngine();
	void init();
	virtual void unload();
	static int lua_panic(lua_State* L);
	static void lua_error(lua_State* L);
	static void lua_error_not_crash(lua_State* L);
	static int lua_pcall_failed(lua_State* L);
#ifdef DEBUG
	static	void				lua_hook_call				(lua_State *L, lua_Debug *dbg);
#endif // #ifdef DEBUG
	void setup_callbacks();
	void load_common_scripts();
	bool load_file(LPCSTR caScriptName, LPCSTR namespace_name);
	IC CScriptProcess* script_process(const EScriptProcessors& process_id) const;
	IC void add_script_process(const EScriptProcessors& process_id, CScriptProcess* script_process);
	void remove_script_process(const EScriptProcessors& process_id);
	void setup_auto_load();
	void process_file_if_exists(LPCSTR file_name, bool warn_if_not_exist);
	void process_file(LPCSTR file_name);
	void process_file(LPCSTR file_name, bool reload_modules);
	bool function_object(LPCSTR function_to_call, ::luabind::object& object, int type = LUA_TFUNCTION);
	u32 script_generation() const { return m_script_generation; }
	u32 lua_profile_epoch() const { return m_lua_profile_epoch; }
	bool lua_hotspot_profiler_enabled() const;
	void set_lua_hotspot_profiler(bool enabled, u32 threshold_us);
	void dump_lua_hotspots(u32 limit) const;
	void reset_lua_hotspots();
	bool start_lua_record(u32 threshold_us);
	bool stop_lua_record();
	bool lua_recording() const;
	u32 lua_record_threshold_us() const;
	LPCSTR lua_record_file_name() const;
	void record_native_profile(LPCSTR domain, LPCSTR phase, LPCSTR object_name,
		u32 object_id, u64 elapsed_ticks, u32 detail = 0);
	void* begin_lua_profile_call(lua_State* state, int function_index, LPCSTR explicit_name = nullptr);
	void end_lua_profile_call(void* context, int result);
	void* begin_lua_to_cpp_profile_call(lua_State* state, LPCSTR owner_name, LPCSTR function_name);
	void end_lua_to_cpp_profile_call(void* context);
	void register_script_classes();
	IC void parse_script_namespace(LPCSTR function_to_call, LPSTR name_space, u32 const namespace_size, LPSTR function,
	                               u32 const function_size);

	template <typename _result_type>
	IC bool functor(LPCSTR function_to_call, ::luabind::functor<_result_type>& lua_function);

	template <typename _result_type>
	void register_profiled_functor(LPCSTR function_name, const ::luabind::functor<_result_type>& lua_function);

#ifdef USE_DEBUGGER
#	ifndef USE_LUA_STUDIO
			void				stopDebugger				();
			void				restartDebugger				();
			CScriptDebugger		*debugger					();
#	else // ifndef USE_LUA_STUDIO
			void				try_connect_to_debugger		();
			void				disconnect_from_debugger	();
	inline cs::lua_studio::world* debugger					() const { return m_lua_studio_world; }
#	endif // ifndef USE_LUA_STUDIO
#endif
	virtual void on_error(lua_State* state);
	void collect_all_garbage();

DECLARE_SCRIPT_REGISTER_FUNCTION
};

class CScopedLuaRecordPhase : xray::noncopyable
{
public:
	CScopedLuaRecordPhase(CScriptEngine& engine, LPCSTR domain, LPCSTR phase,
		LPCSTR object_name, u32 object_id, u32 detail = 0, bool enabled = true) :
		m_engine(&engine),
		m_domain(domain),
		m_phase(phase),
		m_object_name(object_name),
		m_object_id(object_id),
		m_detail(detail),
		m_started(enabled && engine.lua_recording() && CPU::qpc_freq ? CPU::QPC() : 0)
	{
	}

	~CScopedLuaRecordPhase()
	{
		if (m_started)
			m_engine->record_native_profile(m_domain, m_phase, m_object_name,
				m_object_id, CPU::QPC() - m_started, m_detail);
	}

private:
	CScriptEngine* m_engine;
	LPCSTR m_domain;
	LPCSTR m_phase;
	LPCSTR m_object_name;
	u32 m_object_id;
	u32 m_detail;
	u64 m_started;
};

template <typename TResult>
class cached_script_functor final : public ICachedScriptFunctor
{
public:
	explicit cached_script_functor(LPCSTR function_name) :
		m_function_name(function_name),
		m_engine(nullptr),
		m_generation(u32(-1)),
		m_profile_epoch(u32(-1)),
		m_resolved(false),
		m_exists(false)
	{
	}

	~cached_script_functor() override
	{
		if (m_engine)
			m_engine->unregister_cached_functor(this);
		m_functor.reset();
	}

	const ::luabind::functor<TResult>* get(CScriptEngine& engine)
	{
		if (m_engine != &engine)
		{
			if (m_engine)
				m_engine->unregister_cached_functor(this);
			m_engine = &engine;
			engine.register_cached_functor(this);
			m_generation = u32(-1);
			m_profile_epoch = u32(-1);
		}

		if (m_generation != engine.script_generation())
			invalidate_script_functor();

		if (!m_resolved)
		{
			m_exists = engine.functor(*m_function_name, m_functor);
			m_resolved = true;
			m_generation = engine.script_generation();
		}

		if (!m_exists)
			return nullptr;

		if (m_profile_epoch != engine.lua_profile_epoch())
		{
			engine.register_profiled_functor(*m_function_name, m_functor);
			m_profile_epoch = engine.lua_profile_epoch();
		}

		return &m_functor;
	}

	void invalidate_script_functor() override
	{
		m_functor.reset();
		m_generation = u32(-1);
		m_profile_epoch = u32(-1);
		m_resolved = false;
		m_exists = false;
	}

	void detach_script_engine(CScriptEngine* engine) override
	{
		if (m_engine == engine)
			m_engine = nullptr;
	}

private:
	shared_str m_function_name;
	CScriptEngine* m_engine;
	u32 m_generation;
	u32 m_profile_epoch;
	bool m_resolved;
	bool m_exists;
	::luabind::functor<TResult> m_functor;
};

#include "script_engine_inline.h"
