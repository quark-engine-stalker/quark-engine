////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder_object_wrapper.cpp
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script object binder wrapper
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_binder_object_wrapper.h"
#include "script_game_object.h"
#include "xrServer_Objects_ALife.h"
#include "ai_space.h"
#include "script_engine.h"
#include "actor.h"

BOOL psLua_Profile = FALSE;
int psLua_ProfileThresholdUS = 0;

namespace
{
struct lua_callback_profile_entry
{
	u64 calls = 0;
	u64 total_ticks = 0;
	u64 max_ticks = 0;
	u64 alloc_calls = 0;
	u64 realloc_calls = 0;
	u64 free_calls = 0;
	u64 allocated_bytes = 0;
	u64 freed_bytes = 0;
};

xr_map<shared_str, lua_callback_profile_entry> lua_callback_profile;

u64 profile_counter_delta(const u64 current, const u64 previous)
{
	return current >= previous ? current - previous : current;
}

class lua_callback_profile_scope
{
public:
	explicit lua_callback_profile_scope(CScriptBinderObjectWrapper* owner)
	{
		LPCSTR section = owner->m_object ? owner->m_object->Section() : nullptr;
		section_ = section ? section : "<unbound>";
		CScriptEngine& script_engine = ai().script_engine();
		threshold_us_ = script_engine.lua_recording() ? script_engine.lua_record_threshold_us() :
			static_cast<u32>(_max(psLua_ProfileThresholdUS, 0));
		script_engine.lua_allocation_stats(allocation_begin_);
		start_ticks_ = CPU::QPC();
	}

	~lua_callback_profile_scope()
	{
		const u64 elapsed_ticks = CPU::QPC() - start_ticks_;
		lua_callback_profile_entry& entry = lua_callback_profile[section_];
		++entry.calls;
		entry.total_ticks += elapsed_ticks;
		entry.max_ticks = _max(entry.max_ticks, elapsed_ticks);

		const u32 elapsed_us = CPU::qpc_freq ? static_cast<u32>(_min<u64>(
			(elapsed_ticks * 1000000ull) / CPU::qpc_freq, u32(-1))) : 0;
		if (elapsed_us < threshold_us_)
			return;

		SLuaAllocationStats allocation_end;
		ai().script_engine().lua_allocation_stats(allocation_end);
		entry.alloc_calls += profile_counter_delta(allocation_end.alloc_calls, allocation_begin_.alloc_calls);
		entry.realloc_calls += profile_counter_delta(allocation_end.realloc_calls, allocation_begin_.realloc_calls);
		entry.free_calls += profile_counter_delta(allocation_end.free_calls, allocation_begin_.free_calls);
		entry.allocated_bytes += profile_counter_delta(allocation_end.allocated_bytes, allocation_begin_.allocated_bytes);
		entry.freed_bytes += profile_counter_delta(allocation_end.freed_bytes, allocation_begin_.freed_bytes);
	}

private:
	shared_str section_;
	u64 start_ticks_ = 0;
	u32 threshold_us_ = 0;
	SLuaAllocationStats allocation_begin_;
};
}

void lua_profile_dump_callbacks(const u32 limit)
{
	struct profile_row
	{
		shared_str section;
		lua_callback_profile_entry stats;
	};

	xr_vector<profile_row> rows;
	rows.reserve(lua_callback_profile.size());
	for (const auto& item : lua_callback_profile)
		rows.push_back(profile_row{item.first, item.second});

	std::sort(rows.begin(), rows.end(), [](const profile_row& left, const profile_row& right)
	{
		return left.stats.total_ticks > right.stats.total_ticks;
	});

	Msg("[Lua profile] binder update sections=%u, showing=%u",
		static_cast<u32>(rows.size()), _min(limit, static_cast<u32>(rows.size())));

	const u32 row_count = _min(limit, static_cast<u32>(rows.size()));
	for (u32 index = 0; index < row_count; ++index)
	{
		const profile_row& row = rows[index];
		const double total_ms = CPU::qpc_freq ?
			static_cast<double>(row.stats.total_ticks) * 1000.0 / static_cast<double>(CPU::qpc_freq) : 0.0;
		const double max_ms = CPU::qpc_freq ?
			static_cast<double>(row.stats.max_ticks) * 1000.0 / static_cast<double>(CPU::qpc_freq) : 0.0;
		const double average_us = row.stats.calls && CPU::qpc_freq ?
			static_cast<double>(row.stats.total_ticks) * 1000000.0 /
			(static_cast<double>(CPU::qpc_freq) * static_cast<double>(row.stats.calls)) : 0.0;

		Msg("[Lua profile] %2u. %-40s calls=%llu total=%.3f ms avg=%.2f us max=%.3f ms alloc=%llu realloc=%llu free=%llu allocated=%.2f KB freed=%.2f KB",
			index + 1,
			row.section.c_str(),
			static_cast<unsigned long long>(row.stats.calls),
			total_ms,
			average_us,
			max_ms,
			static_cast<unsigned long long>(row.stats.alloc_calls),
			static_cast<unsigned long long>(row.stats.realloc_calls),
			static_cast<unsigned long long>(row.stats.free_calls),
			static_cast<double>(row.stats.allocated_bytes) / 1024.0,
			static_cast<double>(row.stats.freed_bytes) / 1024.0);
	}
}

void lua_profile_reset_callbacks()
{
	lua_callback_profile.clear();
}

CScriptBinderObjectWrapper::CScriptBinderObjectWrapper(CScriptGameObject* object) :
	CScriptBinderObject(object)
{}

CScriptBinderObjectWrapper::~CScriptBinderObjectWrapper()
{}

void CScriptBinderObjectWrapper::reinit()
{
	::luabind::call_member<void>(this, "reinit");
}

void CScriptBinderObjectWrapper::reinit_static(CScriptBinderObject* script_binder_object)
{
	script_binder_object->CScriptBinderObject::reinit();
}

void CScriptBinderObjectWrapper::reload(LPCSTR section)
{
	::luabind::call_member<void>(this, "reload", section);
}

void CScriptBinderObjectWrapper::reload_static(CScriptBinderObject* script_binder_object, LPCSTR section)
{
	script_binder_object->CScriptBinderObject::reload(section);
}

bool CScriptBinderObjectWrapper::net_Spawn(SpawnType DC)
{
	return (::luabind::call_member<bool>(this, "net_spawn", DC));
}

bool CScriptBinderObjectWrapper::net_Spawn_static(CScriptBinderObject* script_binder_object, SpawnType DC)
{
	return (script_binder_object->CScriptBinderObject::net_Spawn(DC));
}

void CScriptBinderObjectWrapper::net_Destroy()
{
	::luabind::call_member<void>(this, "net_destroy");
}

void CScriptBinderObjectWrapper::net_Destroy_static(CScriptBinderObject* script_binder_object)
{
	script_binder_object->CScriptBinderObject::net_Destroy();
}

void CScriptBinderObjectWrapper::net_Import(NET_Packet* net_packet)
{
	::luabind::call_member<void>(this, "net_import", net_packet);
}

void CScriptBinderObjectWrapper::net_Import_static(CScriptBinderObject* script_binder_object, NET_Packet* net_packet)
{
	script_binder_object->CScriptBinderObject::net_Import(net_packet);
}

void CScriptBinderObjectWrapper::net_Export(NET_Packet* net_packet)
{
	::luabind::call_member<void>(this, "net_export", net_packet);
}

void CScriptBinderObjectWrapper::net_Export_static(CScriptBinderObject* script_binder_object, NET_Packet* net_packet)
{
	script_binder_object->CScriptBinderObject::net_Export(net_packet);
}

void CScriptBinderObjectWrapper::shedule_Update(u32 time_delta)
{
	CScriptEngine& script_engine = ai().script_engine();
	const bool profile = !!psLua_Profile || script_engine.lua_recording();

	if (!profile)
	{
		this->call_void_unsigned("update", time_delta);
		return;
	}

	LPCSTR record_section = m_object ? m_object->Section() : nullptr;
	LPCSTR record_object = m_object ? m_object->Name() : nullptr;
	const u32 record_id = m_object ? static_cast<u32>(m_object->ID()) : u32(-1);
	const bool record_actor = script_engine.lua_recording() && m_object && smart_cast<CActor*>(m_object);
	CScopedLuaRecordPhase record_scope(script_engine, record_actor ? "actor_lua" : "binder_lua",
		record_section ? record_section : "<unbound>",
		record_object ? record_object : "<unbound>", record_id, time_delta);

	if (profile)
	{
		lua_callback_profile_scope profile_scope(this);
		this->call_void_unsigned("update", time_delta);
	}
	else
		this->call_void_unsigned("update", time_delta);

}

void CScriptBinderObjectWrapper::shedule_Update_static(CScriptBinderObject* script_binder_object, u32 time_delta)
{
	script_binder_object->CScriptBinderObject::shedule_Update(time_delta);
}

void CScriptBinderObjectWrapper::save(NET_Packet* output_packet)
{
	::luabind::call_member<void>(this, "save", output_packet);
}

void CScriptBinderObjectWrapper::save_static(CScriptBinderObject* script_binder_object, NET_Packet* output_packet)
{
	script_binder_object->CScriptBinderObject::save(output_packet);
}

void CScriptBinderObjectWrapper::load(IReader* input_packet)
{
	::luabind::call_member<void>(this, "load", input_packet);
}

void CScriptBinderObjectWrapper::load_static(CScriptBinderObject* script_binder_object, IReader* input_packet)
{
	script_binder_object->CScriptBinderObject::load(input_packet);
}

bool CScriptBinderObjectWrapper::net_SaveRelevant()
{
	return (::luabind::call_member<bool>(this, "net_save_relevant"));
}

bool CScriptBinderObjectWrapper::net_SaveRelevant_static(CScriptBinderObject* script_binder_object)
{
	return (script_binder_object->CScriptBinderObject::net_SaveRelevant());
}

void CScriptBinderObjectWrapper::net_Relcase(CScriptGameObject* object)
{
	::luabind::call_member<void>(this, "net_Relcase", object);
}

void CScriptBinderObjectWrapper::net_Relcase_static(CScriptBinderObject* script_binder_object,
                                                    CScriptGameObject* object)
{
	script_binder_object->CScriptBinderObject::net_Relcase(object);
}
