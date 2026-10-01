////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper.cpp
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "saved_game_wrapper.h"
#include "alife_time_manager.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"
#include "game_graph.h"
#include "alife_simulator_header.h"
#include "alife_simulator.h"
#include "alife_spawn_registry.h"

extern LPCSTR alife_section;

namespace
{
constexpr u32 saved_game_header_size = 3u * sizeof(u32);
constexpr u32 max_saved_game_payload_size = 1024u * 1024u * 1024u;
constexpr u32 max_saved_game_compressed_size = max_saved_game_payload_size + max_saved_game_payload_size / 64u + 19u;

class scoped_fs_reader
{
private:
	IReader* m_reader;

public:
	explicit scoped_fs_reader(IReader* reader = nullptr) : m_reader(reader) {}
	~scoped_fs_reader()
	{
		if (m_reader)
			FS.r_close(m_reader);
	}

	scoped_fs_reader(const scoped_fs_reader&) = delete;
	scoped_fs_reader& operator=(const scoped_fs_reader&) = delete;

	IReader* get() const { return m_reader; }
	IReader* operator->() const { return m_reader; }
	operator bool() const { return m_reader != nullptr; }

	void reset(IReader* reader = nullptr)
	{
		if (m_reader)
			FS.r_close(m_reader);
		m_reader = reader;
	}
};

class scoped_chunk_reader
{
private:
	IReader* m_reader;

public:
	explicit scoped_chunk_reader(IReader* reader = nullptr) : m_reader(reader) {}
	~scoped_chunk_reader()
	{
		if (m_reader)
			m_reader->close();
	}

	scoped_chunk_reader(const scoped_chunk_reader&) = delete;
	scoped_chunk_reader& operator=(const scoped_chunk_reader&) = delete;

	IReader* get() const { return m_reader; }
	IReader* operator->() const { return m_reader; }
	operator bool() const { return m_reader != nullptr; }
};

class scoped_xr_buffer
{
private:
	void* m_data;

public:
	explicit scoped_xr_buffer(void* data = nullptr) : m_data(data) {}
	~scoped_xr_buffer() { xr_free(m_data); }

	scoped_xr_buffer(const scoped_xr_buffer&) = delete;
	scoped_xr_buffer& operator=(const scoped_xr_buffer&) = delete;

	void* get() const { return m_data; }
	operator bool() const { return m_data != nullptr; }
};

class scoped_server_entity
{
private:
	CSE_Abstract* m_object;

public:
	explicit scoped_server_entity(CSE_Abstract* object = nullptr) : m_object(object) {}
	~scoped_server_entity()
	{
		if (m_object)
			F_entity_Destroy(m_object);
	}

	scoped_server_entity(const scoped_server_entity&) = delete;
	scoped_server_entity& operator=(const scoped_server_entity&) = delete;
};
}

LPCSTR CSavedGameWrapper::saved_game_full_name(LPCSTR saved_game_name, string_path& result)
{
	string_path temp;
	strconcat(sizeof(temp), temp, saved_game_name, SAVE_EXTENSION);
	FS.update_path(result, "$game_saves$", temp);
	return result;
}

bool CSavedGameWrapper::saved_game_exist(LPCSTR saved_game_name)
{
	string_path file_name;
	return !!FS.exist(saved_game_full_name(saved_game_name, file_name));
}

bool CSavedGameWrapper::valid_saved_game(IReader& stream)
{
	const int original_position = stream.tell();
	if (stream.length() < int(saved_game_header_size))
		return false;

	stream.seek(0);
	const u32 magic = stream.r_u32();
	const u32 version = stream.r_u32();
	const u32 source_count = stream.r_u32();
	const u32 compressed_count = u32(stream.length() - saved_game_header_size);
	stream.seek(original_position);

	if (magic != u32(-1))
		return false;

	if (version != ALIFE_VERSION)
		return false;

	if (!source_count || source_count > max_saved_game_payload_size)
		return false;

	if (!compressed_count || compressed_count > max_saved_game_compressed_size)
		return false;

	if (compressed_count > rtc_csize(source_count))
		return false;

	return true;
}

bool CSavedGameWrapper::valid_saved_game(LPCSTR saved_game_name)
{
	string_path file_name;
	scoped_fs_reader stream(FS.r_open(saved_game_full_name(saved_game_name, file_name)));
	return stream && valid_saved_game(*stream.get());
}

void CSavedGameWrapper::initialize()
{
	CALifeTimeManager default_time_manager(alife_section);
	m_game_time = default_time_manager.game_time();
	m_actor_health = 1.f;
	m_valid = false;
	m_level_id = _LEVEL_ID(-1);
	m_level_name = "";
}

CSavedGameWrapper::CSavedGameWrapper(LPCSTR saved_game_name)
{
	initialize();
	load_from_file(saved_game_name);
}

CSavedGameWrapper::CSavedGameWrapper(
	LPCSTR saved_game_name, const void* prepared_payload, u32 prepared_payload_size)
{
	initialize();
	if (prepared_payload && prepared_payload_size)
		load_from_payload(saved_game_name, prepared_payload, prepared_payload_size);
	else
		load_from_file(saved_game_name);
}

void CSavedGameWrapper::load_from_file(LPCSTR saved_game_name)
{
	string_path file_name;
	saved_game_full_name(saved_game_name, file_name);
	scoped_fs_reader stream(FS.r_open(file_name));
	if (!stream)
	{
		Msg("! There is no saved game '%s'", file_name);
		return;
	}

	if (!valid_saved_game(*stream.get()))
		return;

	stream->seek(2 * sizeof(u32));
	const u32 source_count = stream->r_u32();
	const u32 compressed_count = u32(stream->length() - saved_game_header_size);
	scoped_xr_buffer source_data(xr_malloc_uninitialized(source_count));
	if (!source_data)
		return;

	u32 decompressed_count = 0;
	if (!rtc_decompress_safe(
			source_data.get(), source_count, stream->pointer(), compressed_count, &decompressed_count) ||
		decompressed_count != source_count)
	{
		Msg("! Cannot decompress saved game '%s'", file_name);
		return;
	}
	stream.reset();
	load_from_payload(saved_game_name, source_data.get(), source_count);
}

void CSavedGameWrapper::load_from_payload(LPCSTR saved_game_name, const void* payload, u32 payload_size)
{
	(void)saved_game_name;
	if (!payload || !payload_size || payload_size > max_saved_game_payload_size)
		return;

	IReader reader(const_cast<void*>(payload), payload_size);
	_TIME_ID loaded_game_time;

	{
		CALifeTimeManager time_manager(alife_section);
		time_manager.load(reader);
		loaded_game_time = time_manager.game_time();
	}

	if (!reader.find_chunk(OBJECT_CHUNK_DATA) || reader.elapsed() < int(sizeof(u32)))
		return;

	const u32 count = reader.r_u32();
	if (!count)
		return;

	bool corrupted_object = false;
	CSE_ALifeDynamicObject* object = CALifeObjectRegistry::get_object(reader, &corrupted_object);
	scoped_server_entity object_guard(object);
	if (corrupted_object || !object || object->ID != 0)
		return;

	CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(object);
	if (!actor)
		return;

	const float loaded_actor_health = actor->get_health();

	scoped_chunk_reader spawn_chunk(reader.open_chunk(SPAWN_CHUNK_DATA));
	if (!spawn_chunk)
		return;

	string_path spawn_file_name;
	{
		scoped_chunk_reader sub_chunk(spawn_chunk->open_chunk(0));
		if (!sub_chunk)
			return;
		sub_chunk->r_stringZ(spawn_file_name, sizeof(spawn_file_name));
	}

	string_path spawn_file_path;
	if (!FS.exist(spawn_file_path, "$game_spawn$", spawn_file_name, ".spawn"))
		return;

	_LEVEL_ID loaded_level_id = _LEVEL_ID(-1);
	shared_str loaded_level_name;
	const auto resolve_level = [&](const CGameGraph& graph)
	{
		const CGameGraph::CVertex* graph_vertex = graph.vertex(object->m_tGraphID);
		if (!graph_vertex)
			return false;

		loaded_level_id = graph_vertex->level_id();
		loaded_level_name = graph.header().level(loaded_level_id).name();
		return true;
	};

	const bool current_spawn_matches = ai().get_alife() && ai().get_game_graph() &&
		ai().alife().spawns().get_spawn_name() == spawn_file_name;
	if (current_spawn_matches)
	{
		// Quick-load and an in-game save browser already have this exact graph in
		// memory. Reconstructing CGameGraph from all.spawn for every metadata check
		// only repeats allocations and parsing.
		if (!resolve_level(ai().game_graph()))
			return;
	}
	else
	{
		scoped_fs_reader spawn(FS.r_open(spawn_file_path));
		if (!spawn)
			return;

		scoped_chunk_reader graph_chunk(spawn->open_chunk(4));
		if (!graph_chunk)
			return;

		CGameGraph graph(*graph_chunk.get());
		if (!resolve_level(graph))
			return;
	}

	m_game_time = loaded_game_time;
	m_actor_health = loaded_actor_health;
	m_level_id = loaded_level_id;
	m_level_name = loaded_level_name;
	m_valid = true;
}
