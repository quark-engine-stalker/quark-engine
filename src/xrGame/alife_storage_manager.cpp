////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_storage_manager.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "alife_registry_container.h"
#include "xrserver.h"
#include "level.h"
#include "../xrEngine/x_ray.h"
#include "saved_game_wrapper.h"
#include "string_table.h"
#include "../xrEngine/igame_persistent.h"
#include "../xrCore/job_system.h"
#include "autosave_manager.h"
//Alundaio
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
#include "pch_script.h"
#include "../../xrServerEntities/script_engine.h"
#endif
//-Alundaio

extern XRCORE_API string_path g_bug_report_file;

using namespace ALife;
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
 //Alundaio
#endif

extern string_path g_last_saved_game;

namespace
{
bool copy_save_name(LPSTR destination, u32 destination_size, LPCSTR source, LPCSTR game_saves_path)
{
	destination[0] = 0;
	if (!source || !source[0])
		return true;

	const u32 reserved_length = 5u + xr_strlen(SAVE_EXTENSION) + xr_strlen(game_saves_path);
	if (reserved_length >= destination_size || xr_strlen(source) > destination_size - reserved_length)
		return false;

	xr_strcpy(destination, destination_size, source);
	return true;
}

struct prepared_save_state
{
	string_path file_name{};
	IReader* stream = nullptr;
	IReader* companion_stream = nullptr;
	void* source_data = nullptr;
	const void* compressed_data = nullptr;
	const void* companion_data = nullptr;
	u32 source_count = 0;
	u32 compressed_count = 0;
	u32 result_size = 0;
	u32 companion_size = 0;
	bool succeeded = false;
	bool submitted = false;
	bool companion_submitted = false;
	xr_jobs::task_group group;
	xr_jobs::task_group companion_group;
};

prepared_save_state* g_prepared_save = nullptr;
u32 g_last_serialized_save_size = 0;

bool prepare_script_save_companion(prepared_save_state& state)
{
	// G.A.M.M.A keeps its marshalled Lua state next to the engine save as
	// <name>.scoc. Open the mapping on the main thread because CLocatorAPI's file
	// index is not a worker-thread API. The worker only touches this stable mapping.
	constexpr u32 maximum_companion_size = 16u * 1024u * 1024u;
	if (Core.Params && strstr(Core.Params, "-no_save_companion_prefetch"))
		return false;

	string_path companion_name;
	xr_strcpy(companion_name, state.file_name);
	LPSTR extension = strext(companion_name);
	if (!extension || _stricmp(extension, SAVE_EXTENSION))
		return false;

	xr_strcpy(extension, sizeof(companion_name) - static_cast<u32>(extension - companion_name), ".scoc");
	state.companion_stream = FS.r_open(companion_name);
	if (!state.companion_stream)
		return false;

	const int signed_size = state.companion_stream->length();
	if (signed_size <= 0 || static_cast<u32>(signed_size) > maximum_companion_size)
	{
		FS.r_close(state.companion_stream);
		return false;
	}

	state.companion_data = state.companion_stream->pointer();
	state.companion_size = static_cast<u32>(signed_size);
	return state.companion_data != nullptr;
}

void prefetch_script_save_companion(void* parameter)
{
	prepared_save_state& state = *static_cast<prepared_save_state*>(parameter);
	if (!state.companion_data || !state.companion_size)
		return;

	// Fault only the exact, bounded companion into the Windows file cache while
	// the main thread rebuilds ALife registries. Clean Anomaly saves never submit
	// this job because they have no .scoc file.
	SYSTEM_INFO system_info{};
	GetSystemInfo(&system_info);
	const size_t page_size = _max<size_t>(system_info.dwPageSize, 4096u);
	const size_t companion_size = static_cast<size_t>(state.companion_size);
	const volatile u8* bytes = static_cast<const volatile u8*>(state.companion_data);
	volatile u8 page_probe = 0;
	for (size_t offset = 0; offset < companion_size; offset += page_size)
		page_probe = static_cast<u8>(page_probe ^ bytes[offset]);
	page_probe = static_cast<u8>(page_probe ^ bytes[companion_size - 1]);
	(void)page_probe;
}

void decompress_save(void* parameter)
{
	prepared_save_state& state = *static_cast<prepared_save_state*>(parameter);
	state.succeeded = rtc_decompress_safe(
		state.source_data, state.source_count, state.compressed_data, state.compressed_count, &state.result_size);
}

bool complete_decompressed_save(prepared_save_state& state)
{
	if (state.submitted)
	{
		xr_jobs::wait(state.group);
		state.submitted = false;
	}

	if (state.stream)
	{
		FS.r_close(state.stream);
		state.stream = nullptr;
		state.compressed_data = nullptr;
	}

	return state.source_data && state.succeeded && state.result_size == state.source_count;
}

void complete_companion_prefetch(prepared_save_state& state)
{
	if (state.companion_submitted)
	{
		xr_jobs::wait(state.companion_group);
		state.companion_submitted = false;
	}

	if (state.companion_stream)
	{
		FS.r_close(state.companion_stream);
		state.companion_data = nullptr;
	}
}

bool complete_prepared_save(prepared_save_state& state)
{
	const bool result = complete_decompressed_save(state);
	complete_companion_prefetch(state);
	return result;
}

void destroy_prepared_save(prepared_save_state*& state)
{
	if (!state)
		return;

	if (state->submitted)
		xr_jobs::wait(state->group);
	if (state->companion_submitted)
		xr_jobs::wait(state->companion_group);

	if (state->stream)
		FS.r_close(state->stream);
	if (state->companion_stream)
		FS.r_close(state->companion_stream);
	xr_free(state->source_data);
	xr_delete(state);
}

prepared_save_state* create_prepared_save(LPCSTR file_name, bool require_worker)
{
	if (require_worker && !xr_jobs::worker_count())
		return nullptr;

	prepared_save_state* state = xr_new<prepared_save_state>();
	xr_strcpy(state->file_name, file_name);
	state->stream = FS.r_open(file_name);
	if (!state->stream || !CSavedGameWrapper::valid_saved_game(*state->stream))
	{
		destroy_prepared_save(state);
		return nullptr;
	}

	state->stream->seek(2 * sizeof(u32));
	state->source_count = state->stream->r_u32();
	state->compressed_count = u32(state->stream->length() - 3 * sizeof(u32));
	state->compressed_data = state->stream->pointer();
	state->source_data = xr_malloc_uninitialized(state->source_count);
	if (!state->source_data)
	{
		destroy_prepared_save(state);
		return nullptr;
	}

	const bool use_worker = xr_jobs::worker_count() &&
		!(Core.Params && strstr(Core.Params, "-no_mt_save_decompress"));
	if (use_worker)
	{
		state->submitted = xr_jobs::submit(
			decompress_save, state, &state->group, xr_jobs::priority::normal);
		if (!state->submitted)
			decompress_save(state);
	}
	else if (require_worker)
	{
		destroy_prepared_save(state);
		return nullptr;
	}
	else
	{
		decompress_save(state);
	}

	// Submit this independently from decompression. The metadata reader only joins
	// the .scop task; the .scoc warm-up can continue across level teardown and is
	// joined immediately before the Lua load callback can consume the companion.
	if (xr_jobs::worker_count() && prepare_script_save_companion(*state))
	{
		state->companion_submitted = xr_jobs::submit(
			prefetch_script_save_companion, state, &state->companion_group, xr_jobs::priority::normal);
	}

	return state;
}

prepared_save_state* take_prepared_save(LPCSTR file_name)
{
	if (!g_prepared_save)
		return nullptr;

	if (_stricmp(g_prepared_save->file_name, file_name) != 0)
	{
		destroy_prepared_save(g_prepared_save);
		return nullptr;
	}

	prepared_save_state* result = g_prepared_save;
	g_prepared_save = nullptr;
	return result;
}
}

CALifeStorageManager::~CALifeStorageManager()
{
	*g_last_saved_game = 0;
}

void CALifeStorageManager::prepare_load_async(LPCSTR save_name)
{
	if (!save_name || !save_name[0] || !xr_jobs::worker_count() ||
		(Core.Params && strstr(Core.Params, "-no_mt_save_preload")))
	{
		return;
	}

	string_path file_name;
	CSavedGameWrapper::saved_game_full_name(save_name, file_name);
	if (g_prepared_save && _stricmp(g_prepared_save->file_name, file_name) == 0)
		return;

	destroy_prepared_save(g_prepared_save);
	g_prepared_save = create_prepared_save(file_name, true);
}

bool CALifeStorageManager::get_prepared_load_data(LPCSTR save_name, const void*& data, u32& data_size)
{
	data = nullptr;
	data_size = 0;
	if (!save_name || !save_name[0] || !g_prepared_save)
		return false;

	string_path file_name;
	CSavedGameWrapper::saved_game_full_name(save_name, file_name);
	if (_stricmp(g_prepared_save->file_name, file_name))
		return false;
	if (!complete_decompressed_save(*g_prepared_save))
	{
		// Do not let a failed speculative result shadow the ordinary synchronous
		// loader, which remains the compatibility fallback.
		destroy_prepared_save(g_prepared_save);
		return false;
	}

	data = g_prepared_save->source_data;
	data_size = g_prepared_save->source_count;
	if (IsLoadDiagnosticsEnabled())
	{
		Msg("* Save preload reused: payload=%u KB, script companion=%u KB",
			(data_size + 1023u) / 1024u,
			(g_prepared_save->companion_size + 1023u) / 1024u);
	}
	return true;
}

void CALifeStorageManager::discard_prepared_load()
{
	destroy_prepared_save(g_prepared_save);
}

bool CALifeStorageManager::save(LPCSTR save_name_no_check, bool update_name)
{
	PROF_EVENT();
	m_last_save_succeeded = false;
	// A cancelled load may have left a speculative mapping alive. Never allow an
	// overwrite of the same slot to reuse bytes from the previous generation.
	destroy_prepared_save(g_prepared_save);

	LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

	string_path save_name;
	if (!copy_save_name(save_name, sizeof(save_name), save_name_no_check, game_saves_path))
	{
		Msg("! Save failed: file name is too long");
		return false;
	}

	string_path previous_save_name;
	xr_strcpy(previous_save_name, m_save_name);

	if (save_name[0])
	{
		strconcat(sizeof(m_save_name), m_save_name, save_name, SAVE_EXTENSION);
	}
	else if (!m_save_name[0])
	{
		Log("There is no file name specified!");
		return false;
	}

	string_path committed_save_name;
	if (save_name[0])
	{
		xr_strcpy(committed_save_name, save_name);
	}
	else
	{
		xr_strcpy(committed_save_name, m_save_name);
		const u32 extension_length = xr_strlen(SAVE_EXTENSION);
		const u32 name_length = xr_strlen(committed_save_name);
		if (name_length >= extension_length &&
			!xr_strcmp(committed_save_name + name_length - extension_length, SAVE_EXTENSION))
		{
			committed_save_name[name_length - extension_length] = 0;
		}
	}

#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
	::luabind::functor<void> funct1;
	if (ai().script_engine().functor("alife_storage_manager.CALifeStorageManager_before_save", funct1))
		funct1((LPCSTR)m_save_name);
#endif

	constexpr u32 max_saved_game_payload_size = 1024u * 1024u * 1024u;
	string_path file_name;
	FS.update_path(file_name, "$game_saves$", m_save_name);

	u32 reserve_hint = g_last_serialized_save_size;
	if (!reserve_hint)
	{
		IReader* previous_save = FS.r_open(file_name);
		if (previous_save)
		{
			if (CSavedGameWrapper::valid_saved_game(*previous_save))
			{
				previous_save->seek(2 * sizeof(u32));
				reserve_hint = previous_save->r_u32();
			}
			FS.r_close(previous_save);
		}
	}

	u32 source_count = 0;
	u32 dest_count = 0;
	void* dest_data = nullptr;
	{
		CMemoryWriter stream;
		if (reserve_hint && reserve_hint <= max_saved_game_payload_size)
		{
			const u64 suggested_capacity = u64(reserve_hint) + u64(reserve_hint) / 16u + 64u * 1024u;
			stream.reserve(u32(suggested_capacity < max_saved_game_payload_size ? suggested_capacity : max_saved_game_payload_size));
		}

		header().save(stream);
		time_manager().save(stream);
		spawns().save(stream);
		objects().save(stream);
		registry().save(stream);

		source_count = stream.tell();
		if (!source_count || source_count > max_saved_game_payload_size)
		{
			Msg("! Save failed: invalid serialized size %u bytes", source_count);
			xr_strcpy(m_save_name, previous_save_name);
			return false;
		}

		g_last_serialized_save_size = source_count;
		const u32 dest_capacity = rtc_csize(source_count);
		dest_data = xr_malloc_uninitialized(dest_capacity);
		if (!dest_data)
		{
			Msg("! Save failed: cannot allocate %u bytes for compression", dest_capacity);
			xr_strcpy(m_save_name, previous_save_name);
			return false;
		}

		dest_count = rtc_compress(dest_data, dest_capacity, stream.pointer(), source_count);
		if (!dest_count || dest_count > dest_capacity)
		{
			Msg("! Save failed: compression returned invalid size %u", dest_count);
			xr_free(dest_data);
			xr_strcpy(m_save_name, previous_save_name);
			return false;
		}
	}

	const u32 file_header[] = {u32(-1), ALIFE_VERSION, source_count};
	const bool committed = FS.write_file_atomic(
		file_name, file_header, sizeof(file_header), dest_data, dest_count);
	xr_free(dest_data);

	if (!committed)
	{
		Msg("! Game save failed for file '%s'; previous save was left intact", file_name);
		xr_strcpy(m_save_name, previous_save_name);
		return false;
	}

	xr_strcpy(g_last_saved_game, committed_save_name);
	m_last_save_succeeded = true;
#ifdef DEBUG
	Msg("* Game %s is successfully saved to file '%s' (%u bytes compressed to %u)",
		m_save_name, file_name, source_count, dest_count + sizeof(file_header));
#else
	Msg("* Game %s is successfully saved to file '%s'", m_save_name, file_name);
#endif

#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
	::luabind::functor<void> funct2;
	if (ai().script_engine().functor("alife_storage_manager.CALifeStorageManager_save", funct2))
		funct2((LPCSTR)m_save_name);
#endif

	if (!update_name)
		xr_strcpy(m_save_name, previous_save_name);

	return true;
}

bool CALifeStorageManager::load(void* buffer, const u32& buffer_size, LPCSTR file_name)
{
	//Alundaio: So we can get the fname to make our own custom save states
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
	::luabind::functor<void> funct;
	if (ai().script_engine().functor("alife_storage_manager.CALifeStorageManager_load", funct))
		funct(file_name);
#endif
	//-Alundaio

	IReader source(buffer, buffer_size);
	header().load(source);
	time_manager().load(source);
	spawns().load(source, file_name);
	graph().on_load();
	if (!objects().load(source))
		return false;

	VERIFY(can_register_objects());
	can_register_objects(false);
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator B = objects().objects().begin();
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator E = objects().objects().end();
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator I;
	for (I = B; I != E; ++I)
	{
		ALife::_OBJECT_ID id = (*I).second->ID;
		(*I).second->ID = server().PerformIDgen(id);
		VERIFY(id == (*I).second->ID);
		register_object((*I).second, false);
	}

	registry().load(source);

	can_register_objects(true);

	for (I = B; I != E; ++I)
		(*I).second->on_register();

	if (g_pGameLevel)
		Level().autosave_manager().on_game_loaded();

	return true;
}

bool CALifeStorageManager::load(LPCSTR save_name_no_check)
{
	LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

	string_path save_name;
	if (!copy_save_name(save_name, sizeof(save_name), save_name_no_check, game_saves_path))
	{
		Msg("! Cannot load saved game: file name is too long");
		return false;
	}

	CTimer timer;
	timer.Start();

	string_path previous_save_name;
	xr_strcpy(previous_save_name, m_save_name);
	if (save_name[0])
	{
		strconcat(sizeof(m_save_name), m_save_name, save_name, SAVE_EXTENSION);
	}
	else if (!m_save_name[0])
	{
		Msg("! Cannot load saved game: no file name specified");
		return false;
	}

	string_path loaded_save_name;
	if (save_name[0])
	{
		xr_strcpy(loaded_save_name, save_name);
	}
	else
	{
		xr_strcpy(loaded_save_name, m_save_name);
		const u32 extension_length = xr_strlen(SAVE_EXTENSION);
		const u32 name_length = xr_strlen(loaded_save_name);
		if (name_length >= extension_length &&
			!xr_strcmp(loaded_save_name + name_length - extension_length, SAVE_EXTENSION))
		{
			loaded_save_name[name_length - extension_length] = 0;
		}
	}

	string_path file_name;
	FS.update_path(file_name, "$game_saves$", m_save_name);

	prepared_save_state* prepared = take_prepared_save(file_name);
	if (!prepared)
		prepared = create_prepared_save(file_name, false);

	if (!prepared)
	{
		Msg("! Cannot open or validate saved game %s", file_name);
		xr_strcpy(m_save_name, previous_save_name);
		return false;
	}

	xr_strcpy(g_bug_report_file, file_name);
	g_pGamePersistent->LoadTitle();

	// Save decompression may already be running from the menu command. Rebuild the
	// ALife registries concurrently, then join only when serialized data is needed.
	unload();
	reload(m_section);

	// complete_prepared_save also finishes the bounded .scoc cache warm-up when
	// one exists. It never changes the companion file or the save format.
	const bool decompressed = complete_prepared_save(*prepared);
	const u32 source_count = prepared->source_count;
	void* source_data = prepared->source_data;
	prepared->source_data = nullptr;
	destroy_prepared_save(prepared);

	if (!decompressed)
	{
		Msg("! Cannot safely decompress saved game '%s'", file_name);
		xr_free(source_data);
		xr_strcpy(m_save_name, previous_save_name);
		return false;
	}

	g_last_serialized_save_size = source_count;
	if (!load(source_data, source_count, file_name))
	{
		Msg("! Cannot deserialize saved game '%s'", file_name);
		xr_free(source_data);
		unload();
		reload(m_section);
		xr_strcpy(m_save_name, previous_save_name);
		return false;
	}
	xr_free(source_data);

	groups().on_after_game_load();

	if (!graph().actor())
	{
		Msg("! Saved game '%s' does not contain a valid actor", file_name);
		unload();
		reload(m_section);
		xr_strcpy(m_save_name, previous_save_name);
		return false;
	}

	xr_strcpy(g_last_saved_game, loaded_save_name);
	Msg("* Game %s is successfully loaded from file '%s' (%.3fs)",
		loaded_save_name, file_name, timer.GetElapsed_sec());

	return true;
}

bool CALifeStorageManager::save(NET_Packet& net_packet)
{
	PROF_EVENT();
	m_last_save_succeeded = false;
	prepare_objects_for_save();

	shared_str game_name;
	net_packet.r_stringZ(game_name);
	return save(*game_name, !!net_packet.r_u8());
}

void CALifeStorageManager::prepare_objects_for_save()
{
	PROF_EVENT();
	Level().ClientSend();
	Level().ClientSave();
}
