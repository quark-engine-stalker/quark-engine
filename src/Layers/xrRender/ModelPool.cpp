#include "stdafx.h"
#pragma hdrstop

#include "ModelPool.h"
#include "../../xrCore/adaptive_memory_policy.h"

#include "../../xrCore/job_system.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#ifndef _EDITOR
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/fmesh.h"
#include "fhierrarhyvisual.h"
#include "SkeletonAnimated.h"
#include "fvisual.h"
#include "fprogressive.h"
#include "fskinned.h"
#include "flod.h"
#include "ftreevisual.h"
#include "ParticleGroup.h"
#include "ParticleEffect.h"
#else
    #include "fmesh.h"
    #include "fvisual.h"
    #include "fprogressive.h"
    #include "ParticleEffect.h"
    #include "ParticleGroup.h"
	#include "fskinned.h"
    #include "fhierrarhyvisual.h"
    #include "SkeletonAnimated.h"
	#include "IGame_Persistent.h"
#endif

int psModelAsyncPrepare = 1;
int psModelAsyncTextureWarmup = 1;
int psModelFinalizeBudgetMs = 2;
int psModelPrefetchManifest = 1;
int psModelPrefetchManifestMax = 4096;
int psModelPrefetchInFlight = 64;

namespace
{
thread_local u32 model_load_depth = 0;

class model_load_scope : xray::noncopyable
{
public:
	model_load_scope() { ++model_load_depth; }
	~model_load_scope()
	{
		VERIFY(model_load_depth > 0);
		--model_load_depth;
	}
};

}

namespace
{
constexpr u32 model_manifest_magic = 0x33464D50; // PMF3
constexpr u32 model_manifest_version = 1;
constexpr u32 model_manifest_max_file_size = 4u * 1024u * 1024u;
constexpr u32 prepared_ogf_max_file_size = 256u * 1024u * 1024u;
constexpr u32 prepared_omf_max_file_size = 512u * 1024u * 1024u;

enum class prepared_model_stage : u32
{
	queued,
	preparing,
	ready,
	finalizing,
	finalized,
	failed
};

enum class prepared_motion_stage : u32
{
	preparing,
	ready,
	failed
};

class prepared_byte_buffer
{
private:
	u8* m_data = nullptr;
	size_t m_size = 0;

public:
	prepared_byte_buffer() = default;
	prepared_byte_buffer(const prepared_byte_buffer&) = delete;
	prepared_byte_buffer& operator=(const prepared_byte_buffer&) = delete;

	prepared_byte_buffer(prepared_byte_buffer&& other) noexcept
		: m_data(other.m_data), m_size(other.m_size)
	{
		other.m_data = nullptr;
		other.m_size = 0;
	}

	prepared_byte_buffer& operator=(prepared_byte_buffer&& other) noexcept
	{
		if (this == &other)
			return *this;
		reset();
		m_data = other.m_data;
		m_size = other.m_size;
		other.m_data = nullptr;
		other.m_size = 0;
		return *this;
	}

	~prepared_byte_buffer()
	{
		reset();
	}

	bool allocate_for_overwrite(size_t size)
	{
		reset();
		if (!size)
			return true;

		m_data = static_cast<u8*>(xr_malloc_uninitialized(size));
		if (!m_data)
			return false;
		m_size = size;
		return true;
	}

	void reset()
	{
		if (m_data)
			xr_free(m_data);
		m_size = 0;
	}

	bool empty() const { return m_size == 0; }
	size_t size() const { return m_size; }
	u8* data() { return m_data; }
	const u8* data() const { return m_data; }
};

struct prepared_motion_blob
{
	shared_str name;
	prepared_byte_buffer bytes;
	u32 motion_count = 0;
	std::atomic<prepared_motion_stage> stage{prepared_motion_stage::preparing};
	std::mutex wait_mutex;
	std::condition_variable wait_condition;
	xr_string error;
};

using prepared_motion_ptr = std::shared_ptr<prepared_motion_blob>;

bool copy_reader_bytes(IReader* reader, prepared_byte_buffer& destination, u32 maximum_size)
{
	if (!reader || reader->length() <= 0 || static_cast<u32>(reader->length()) > maximum_size)
		return false;

	const size_t length = static_cast<size_t>(reader->length());
	if (!destination.allocate_for_overwrite(length))
		return false;
	CopyMemory(destination.data(), reader->pointer(), length);
	return true;
}

bool read_string_z_safe(IReader& reader, xr_string& destination, size_t max_length)
{
	const int remaining = reader.elapsed();
	if (remaining <= 0)
		return false;

	const char* begin = static_cast<const char*>(reader.pointer());
	const char* terminator = static_cast<const char*>(std::memchr(begin, 0, static_cast<size_t>(remaining)));
	if (!terminator)
		return false;

	const size_t length = static_cast<size_t>(terminator - begin);
	if (length >= max_length)
		return false;

	destination.assign(begin, length);
	reader.advance(static_cast<int>(length + 1));
	return true;
}

bool read_string_z_safe(IReader& reader, char* destination, size_t capacity)
{
	if (!destination || !capacity)
		return false;

	const int remaining = reader.elapsed();
	if (remaining <= 0)
		return false;

	const char* begin = static_cast<const char*>(reader.pointer());
	const char* terminator = static_cast<const char*>(std::memchr(begin, 0, static_cast<size_t>(remaining)));
	if (!terminator)
		return false;

	const size_t length = static_cast<size_t>(terminator - begin);
	if (length >= capacity)
		return false;

	if (length)
		CopyMemory(destination, begin, length);
	destination[length] = 0;
	reader.advance(static_cast<int>(length + 1));
	return true;
}

bool read_line_safe(IReader& reader, xr_string& destination, size_t max_length)
{
	const int remaining = reader.elapsed();
	if (remaining <= 0)
		return false;

	const char* begin = static_cast<const char*>(reader.pointer());
	size_t length = 0;
	while (length < static_cast<size_t>(remaining) && begin[length] != '\r' &&
		begin[length] != '\n' && begin[length] != 0)
		++length;
	if (length >= max_length)
		return false;

	destination.assign(begin, length);
	return true;
}

bool is_prefetch_model_name_safe(LPCSTR name)
{
	if (!name || !name[0] || xr_strlen(name) >= sizeof(string_path) ||
		name[0] == '\\' || name[0] == '/' || strchr(name, ':') || strchr(name, '#') || strstr(name, ".."))
		return false;
	return true;
}

void normalize_model_name(LPCSTR source, string_path& destination)
{
	xr_strcpy(destination, source ? source : "");
	xr_strlwr(destination);
	if (strext(destination))
		*strext(destination) = 0;
}

bool open_model_bytes(LPCSTR requested_name, prepared_byte_buffer& bytes, xr_string& resolved_name)
{
	string_path with_extension;
	if (strext(requested_name))
		xr_strcpy(with_extension, requested_name);
	else
		strconcat(sizeof(with_extension), with_extension, requested_name, ".ogf");

	IReader* reader = FS.r_open(requested_name);
	if (reader)
	{
		resolved_name = requested_name;
	}
	else if ((reader = FS.r_open("$level$", with_extension)) != nullptr)
	{
		string_path full;
		FS.update_path(full, "$level$", with_extension);
		resolved_name = full;
	}
	else if ((reader = FS.r_open("$game_meshes$", with_extension)) != nullptr)
	{
		string_path full;
		FS.update_path(full, "$game_meshes$", with_extension);
		resolved_name = full;
	}
	else
	{
		return false;
	}

	const bool copied = copy_reader_bytes(reader, bytes, prepared_ogf_max_file_size);
	FS.r_close(reader);
	return copied;
}

bool open_motion_bytes(LPCSTR motion_name, prepared_byte_buffer& bytes)
{
	IReader* reader = FS.r_open("$level$", motion_name);
	if (!reader)
		reader = FS.r_open("$game_meshes$", motion_name);
	if (!reader)
		return false;

	const bool copied = copy_reader_bytes(reader, bytes, prepared_omf_max_file_size);
	FS.r_close(reader);
	return copied;
}

bool inspect_motion_blob(prepared_motion_blob& blob, xr_string& error)
{
	if (blob.bytes.empty())
	{
		error = "empty OMF";
		return false;
	}

	IReader reader(blob.bytes.data(), static_cast<int>(blob.bytes.size()));
	IReader* params = reader.open_chunk(OGF_S_SMPARAMS);
	if (!params)
	{
		error = "OMF has no OGF_S_SMPARAMS chunk";
		return false;
	}

	if (params->length() < static_cast<int>(sizeof(u16) * 2))
	{
		params->close();
		error = "truncated OMF parameters";
		return false;
	}

	const u16 version = params->r_u16();
	params->close();
	if (version > xrOGF_SMParamsVersion)
	{
		error = "unsupported OMF version";
		return false;
	}

	IReader* motions = reader.open_chunk(OGF_S_MOTIONS);
	if (!motions)
	{
		error = "OMF has no OGF_S_MOTIONS chunk";
		return false;
	}

	u32 count = 0;
	const bool count_ok = motions->r_chunk_safe(0, &count, sizeof(count)) != FALSE;
	motions->close();
	if (!count_ok || count >= 0x3FFF)
	{
		error = "invalid OMF motion count";
		return false;
	}

	blob.motion_count = count;
	return true;
}

void append_motion_name(xr_vector<shared_str>& names, xr_unordered_set<shared_str>& unique, LPCSTR name)
{
	if (!name || !name[0])
		return;

	string_path normalized;
	xr_strcpy(normalized, name);
	xr_strlwr(normalized);
	const shared_str key = normalized;
	if (unique.emplace(key).second)
		names.push_back(key);
}

void expand_motion_reference(LPCSTR reference, xr_vector<shared_str>& names,
	xr_unordered_set<shared_str>& unique)
{
	if (strstr(reference, "\\*.omf"))
	{
		FS_FileSet files;
		FS.file_list(files, "$game_meshes$", FS_ListFiles, reference);
		FS.file_list(files, "$level$", FS_ListFiles, reference);
		for (const FS_File& file : files)
			append_motion_name(names, unique, file.name.c_str());
		return;
	}

	string_path motion_name;
	xr_strcpy(motion_name, reference);
	if (!strext(motion_name))
		xr_strcat(motion_name, sizeof(motion_name), ".omf");
	append_motion_name(names, unique, motion_name);

	if (strstr(motion_name, "stalker_animation.omf"))
	{
		FS_FileSet extra;
		FS.file_list(extra, "$game_meshes$", FS_ListFiles,
			"actors\\modded_stalker_animations\\*.omf");
		for (const FS_File& file : extra)
			append_motion_name(names, unique, file.name.c_str());
	}
}

void append_model_dependency(xr_vector<shared_str>& dependencies, LPCSTR name)
{
	if (!name || !name[0])
		return;
	string_path normalized;
	normalize_model_name(name, normalized);
	if (is_prefetch_model_name_safe(normalized))
		dependencies.emplace_back(normalized);
}

bool inspect_ogf_reader(IReader& reader, ogf_header& header,
	xr_vector<shared_str>& motion_names, xr_vector<shared_str>& dependencies,
	xr_vector<shared_str>& texture_names, xr_string& error)
{
	if (reader.length() < static_cast<int>(sizeof(u32) * 2 + sizeof(ogf_header)))
	{
		error = "truncated OGF";
		return false;
	}

	if (!reader.r_chunk_safe(OGF_HEADER, &header, sizeof(header)))
	{
		error = "OGF has no valid header";
		return false;
	}
	if (header.format_version != xrOGF_FormatVersion)
	{
		error = "unsupported OGF version";
		return false;
	}
	if (header.type > MT_TREE_PM)
	{
		error = "unsupported OGF visual type";
		return false;
	}

	// Parse strings and embedded child headers on the worker. Runtime objects,
	// shaders and D3D resources are deliberately not created here.
	if (reader.find_chunk(OGF_TEXTURE))
	{
		string256 texture_name;
		string256 shader_name;
		if (!read_string_z_safe(reader, texture_name, sizeof(texture_name)) ||
			!read_string_z_safe(reader, shader_name, sizeof(shader_name)))
		{
			error = "invalid OGF texture/shader strings";
			return false;
		}
		if (texture_name[0] && xr_strcmp(texture_name, "null") && !strstr(texture_name, "$user$"))
			texture_names.emplace_back(texture_name);
	}

	xr_unordered_set<shared_str> unique_motion_names;
	unique_motion_names.reserve(16);
	if (reader.find_chunk(OGF_S_MOTION_REFS))
	{
		string_path references;
		if (!read_string_z_safe(reader, references, sizeof(references)))
		{
			error = "invalid OGF motion references";
			return false;
		}
		const u32 count = _GetItemCount(references);
		string_path item;
		for (u32 index = 0; index < count; ++index)
		{
			_GetItem(references, index, item);
			expand_motion_reference(item, motion_names, unique_motion_names);
		}
	}
	else if (reader.find_chunk(OGF_S_MOTION_REFS2))
	{
		if (reader.elapsed() < static_cast<int>(sizeof(u32)))
		{
			error = "truncated OGF motion reference count";
			return false;
		}
		const u32 count = reader.r_u32();
		if (count >= MAX_ANIM_SLOT)
		{
			error = "too many OMF references";
			return false;
		}

		string_path item;
		for (u32 index = 0; index < count; ++index)
		{
			if (!read_string_z_safe(reader, item, sizeof(item)))
			{
				error = "invalid OGF motion reference";
				return false;
			}
			expand_motion_reference(item, motion_names, unique_motion_names);
		}
	}

	IReader* lod = reader.open_chunk(OGF_S_LODS);
	if (lod)
	{
		xr_string lod_name;
		const bool valid_lod = read_line_safe(*lod, lod_name, sizeof(string_path));
		lod->close();
		if (!valid_lod || lod_name.empty())
		{
			error = "invalid OGF LOD reference";
			return false;
		}
		append_model_dependency(dependencies, lod_name.c_str());
	}

	IReader* children = reader.open_chunk(OGF_CHILDREN);
	if (children)
	{
		u32 child_id = 0;
		for (IReader* child = children->open_chunk_iterator(child_id); child;
			child = children->open_chunk_iterator(child_id, child))
		{
			if (child->length() <= 0 || static_cast<u32>(child->length()) > prepared_ogf_max_file_size)
			{
				child->close();
				children->close();
				error = "invalid embedded OGF child";
				return false;
			}

			ogf_header child_header{};
			if (!inspect_ogf_reader(*child, child_header, motion_names, dependencies, texture_names, error))
			{
				child->close();
				children->close();
				const xr_string details = error;
				error = make_string("invalid embedded OGF child: %s", details.c_str());
				return false;
			}
		}
		children->close();
	}

	return true;
}

bool inspect_ogf(const prepared_byte_buffer& bytes, ogf_header& header,
	xr_vector<shared_str>& motion_names, xr_vector<shared_str>& dependencies,
	xr_vector<shared_str>& texture_names, xr_string& error)
{
	if (bytes.empty() || bytes.size() > prepared_ogf_max_file_size)
	{
		error = "truncated OGF";
		return false;
	}

	IReader reader(const_cast<u8*>(bytes.data()), static_cast<int>(bytes.size()));
	return inspect_ogf_reader(reader, header, motion_names, dependencies, texture_names, error);
}

void sanitize_manifest_component(LPCSTR level_name, string_path& result)
{
	u32 out = 0;
	for (u32 index = 0; level_name && level_name[index] && out + 1 < sizeof(string_path); ++index)
	{
		const char value = level_name[index];
		if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
			(value >= '0' && value <= '9') || value == '_' || value == '-')
			result[out++] = static_cast<char>(tolower(static_cast<unsigned char>(value)));
		else
			result[out++] = '_';
	}
	result[out] = 0;
	if (!out)
		xr_strcpy(result, "unknown_level");
}

void build_manifest_path(LPCSTR level_name, string_path& result)
{
	string_path component;
	sanitize_manifest_component(level_name, component);
	string_path relative;
	xr_sprintf(relative, "cache\\model_prefetch\\%s.pmf", component);
	FS.update_path(result, "$app_data_root$", relative);
}
}

struct CModelPool::AsyncStorage
{
	struct PreparedModel
	{
		shared_str key;
		shared_str requested_name;
		xr_string resolved_name;
		prepared_byte_buffer ogf_bytes;
		ogf_header header{};
		xr_vector<prepared_motion_ptr> motion_blobs;
		xr_vector<std::weak_ptr<PreparedModel>> dependencies;
		std::atomic<prepared_model_stage> stage{prepared_model_stage::queued};
		std::mutex wait_mutex;
		std::condition_variable wait_condition;
		xr_string error;
		bool assert_on_missing = false;
		std::atomic<bool> from_manifest{false};
		bool manifest_counted = false;
		bool manifest_completed = false;
	};

	using state_ptr = std::shared_ptr<PreparedModel>;
	struct JobContext
	{
		CModelPool* owner;
		state_ptr state;
	};

	xrCriticalSection guard;
	xr_unordered_map<shared_str, state_ptr> in_flight;
	xr_unordered_map<shared_str, std::weak_ptr<prepared_motion_blob>> motion_in_flight;
	// Raw OMF blobs are only needed until the render thread installs their
	// parsed motions in g_pMotionsContainer. Remember those names so later NPC
	// models do not copy the same large animation packs again on a worker.
	xr_unordered_set<shared_str> resident_motions;
	xr_vector<state_ptr> ready_to_finalize;
	xr_vector<state_ptr> manifest_states;
	xr_vector<shared_str> manifest_pending;
	size_t manifest_cursor = 0;
	u32 manifest_inflight = 0;
	xr_unordered_set<shared_str> observed_models;
	xr_vector<shared_str> observed_order;
	xr_unordered_set<shared_str> failed_models;
	shared_str active_level;
	xr_jobs::task_scope job_scope;
	xr_jobs::task_group job_group;
	DWORD finalization_thread_id = 0;
	bool level_recording = false;

	AsyncStorage()
	{
		// Keep only the hot async lane warm at construction time. Manifest and
		// recording storage is level-scoped and is reserved lazily in
		// BeginLevelPrefetch(), otherwise every renderer instance permanently pays
		// for thousands of entries even before a manifest is used.
		in_flight.reserve(128);
		motion_in_flight.reserve(32);
		resident_motions.reserve(128);
		ready_to_finalize.reserve(64);
		finalization_thread_id = GetCurrentThreadId();
	}
};

namespace
{
void erase_in_flight(CModelPool* owner, const CModelPool::AsyncStorage::state_ptr& state);
CModelPool::AsyncStorage::state_ptr queue_model_preparation(CModelPool* owner,
	LPCSTR normalized_name, LPCSTR requested_name, bool assert_on_missing, bool from_manifest);
void pump_manifest_preparations(CModelPool* owner);

void mark_manifest_complete_locked(CModelPool::AsyncStorage* storage,
	const CModelPool::AsyncStorage::state_ptr& state)
{
	if (!state->manifest_counted || state->manifest_completed)
		return;
	state->manifest_completed = true;
	VERIFY(storage->manifest_inflight > 0);
	--storage->manifest_inflight;
}

void release_prepared_payload(const CModelPool::AsyncStorage::state_ptr& state)
{
	state->ogf_bytes.reset();
	xr_vector<prepared_motion_ptr>().swap(state->motion_blobs);
	xr_vector<std::weak_ptr<CModelPool::AsyncStorage::PreparedModel>>().swap(state->dependencies);
}

void publish_resident_motions(CModelPool* owner,
	const CModelPool::AsyncStorage::state_ptr& state)
{
	xrCriticalSectionGuard guard(owner->AsyncStorageData()->guard);
	for (const prepared_motion_ptr& blob : state->motion_blobs)
		owner->AsyncStorageData()->resident_motions.emplace(blob->name);
}

void finish_preparation(CModelPool* owner, const CModelPool::AsyncStorage::state_ptr& state,
	prepared_model_stage stage, LPCSTR error = nullptr)
{
	if (error && error != state->error.c_str())
		state->error = error;
	if (stage == prepared_model_stage::failed)
	{
		release_prepared_payload(state);
	}
	state->stage.store(stage, std::memory_order_release);
	if (stage == prepared_model_stage::ready)
	{
		xrCriticalSectionGuard guard(owner->AsyncStorageData()->guard);
		owner->AsyncStorageData()->ready_to_finalize.push_back(state);
	}
	state->wait_condition.notify_all();
	if (stage == prepared_model_stage::failed)
	{
		{
			CModelPool::AsyncStorage* storage = owner->AsyncStorageData();
			xrCriticalSectionGuard guard(storage->guard);
			mark_manifest_complete_locked(storage, state);
			if (storage->level_recording)
			{
				storage->failed_models.emplace(state->key);
				storage->observed_models.erase(state->key);
			}
		}
		erase_in_flight(owner, state);
	}
}

prepared_motion_ptr acquire_prepared_motion(CModelPool* owner, LPCSTR motion_name, xr_string& error)
{
	CModelPool::AsyncStorage* storage = owner->AsyncStorageData();
	const shared_str key = motion_name;
	prepared_motion_ptr blob;
	bool producer = false;
	{
		xrCriticalSectionGuard guard(storage->guard);
		const auto found = storage->motion_in_flight.find(key);
		if (found != storage->motion_in_flight.end())
			blob = found->second.lock();

		if (!blob)
		{
			blob = std::make_shared<prepared_motion_blob>();
			blob->name = key;
			storage->motion_in_flight[key] = blob;
			producer = true;
		}
	}

	if (producer)
	{
		if (!open_motion_bytes(motion_name, blob->bytes))
		{
			blob->error = make_string("motion file not found: %s", motion_name);
			blob->stage.store(prepared_motion_stage::failed, std::memory_order_release);
		}
		else if (!inspect_motion_blob(*blob, blob->error))
		{
			const xr_string details = blob->error;
			blob->error = make_string("invalid motion file %s: %s", motion_name, details.c_str());
			blob->bytes.reset();
			blob->stage.store(prepared_motion_stage::failed, std::memory_order_release);
		}
		else
		{
			blob->stage.store(prepared_motion_stage::ready, std::memory_order_release);
		}
		blob->wait_condition.notify_all();
	}
	else
	{
		std::unique_lock<std::mutex> lock(blob->wait_mutex);
		blob->wait_condition.wait(lock, [&blob]()
		{
			return blob->stage.load(std::memory_order_acquire) != prepared_motion_stage::preparing;
		});
	}

	if (blob->stage.load(std::memory_order_acquire) == prepared_motion_stage::failed)
	{
		error = blob->error;
		xrCriticalSectionGuard guard(storage->guard);
		const auto found = storage->motion_in_flight.find(key);
		if (found != storage->motion_in_flight.end() && found->second.lock() == blob)
			storage->motion_in_flight.erase(found);
		return {};
	}

	return blob;
}

void prepare_model_state(CModelPool* owner, const CModelPool::AsyncStorage::state_ptr& state)
{
	prepared_model_stage expected = prepared_model_stage::queued;
	if (!state->stage.compare_exchange_strong(expected, prepared_model_stage::preparing,
		std::memory_order_acq_rel, std::memory_order_acquire))
		return;

	if (!open_model_bytes(state->requested_name.c_str(), state->ogf_bytes, state->resolved_name))
	{
		finish_preparation(owner, state, prepared_model_stage::failed, "model file not found");
		return;
	}

	xr_vector<shared_str> motion_names;
	xr_vector<shared_str> dependency_names;
	xr_vector<shared_str> texture_names;
	motion_names.reserve(8);
	dependency_names.reserve(2);
	texture_names.reserve(8);
	if (!inspect_ogf(state->ogf_bytes, state->header, motion_names, dependency_names, texture_names, state->error))
	{
		finish_preparation(owner, state, prepared_model_stage::failed, state->error.c_str());
		return;
	}

	// Texture objects created from an OGF shader are one of the largest cold-spawn
	// stalls. The DX11 resource manager deliberately performs texture IO/staging
	// outside its registry lock, so warm the model's DDS set while this worker is
	// already preparing the OGF. The renderer expands the base texture to its THM
	// bump/bump# companions. Unsupported/user/video textures stay on the normal
	// render-thread compatibility path.
	if (psModelAsyncTextureWarmup && Device.b_is_Ready && Device.m_pRender)
	{
		xr_unordered_set<shared_str> unique_textures;
		unique_textures.reserve(texture_names.size());
		for (const shared_str& texture_name : texture_names)
		{
			if (!texture_name.size() || !unique_textures.emplace(texture_name).second)
				continue;
			Device.m_pRender->ResourcesPrefetchCreateModelTexture(texture_name.c_str());
		}
	}

	// Embedded children may repeat the same OMF/LOD reference. Compact before
	// touching the shared blob and model registries.
	xr_unordered_set<shared_str> unique_motions;
	xr_vector<shared_str> compact_motions;
	unique_motions.reserve(motion_names.size());
	compact_motions.reserve(motion_names.size());
	for (const shared_str& motion_name : motion_names)
	{
		if (unique_motions.emplace(motion_name).second)
			compact_motions.push_back(motion_name);
	}

	xr_vector<shared_str> motions_to_prepare;
	motions_to_prepare.reserve(compact_motions.size());
	{
		xrCriticalSectionGuard guard(owner->AsyncStorageData()->guard);
		for (const shared_str& motion_name : compact_motions)
		{
			if (owner->AsyncStorageData()->resident_motions.find(motion_name) ==
				owner->AsyncStorageData()->resident_motions.end())
				motions_to_prepare.push_back(motion_name);
		}
	}

	state->motion_blobs.reserve(motions_to_prepare.size());
	for (const shared_str& motion_name : motions_to_prepare)
	{
		prepared_motion_ptr blob = acquire_prepared_motion(owner, motion_name.c_str(), state->error);
		if (!blob)
		{
			finish_preparation(owner, state, prepared_model_stage::failed);
			return;
		}
		state->motion_blobs.push_back(std::move(blob));
	}

	xr_unordered_set<shared_str> unique_dependencies;
	unique_dependencies.reserve(dependency_names.size());
	state->dependencies.reserve(dependency_names.size());
	for (const shared_str& dependency : dependency_names)
	{
		if (dependency == state->key || !unique_dependencies.emplace(dependency).second)
			continue;
		state->dependencies.push_back(queue_model_preparation(owner, dependency.c_str(),
			dependency.c_str(), false, state->from_manifest.load(std::memory_order_acquire)));
	}

	finish_preparation(owner, state, prepared_model_stage::ready);
}

void prepare_model_job(void* raw_context)
{
	auto* context = static_cast<CModelPool::AsyncStorage::JobContext*>(raw_context);
	CModelPool* owner = context->owner;
	const CModelPool::AsyncStorage::state_ptr state = context->state;
	xr_delete(context);
	prepare_model_state(owner, state);
}

CModelPool::AsyncStorage::state_ptr queue_model_preparation(CModelPool* owner,
	LPCSTR normalized_name, LPCSTR requested_name, bool assert_on_missing, bool from_manifest)
{
	CModelPool::AsyncStorage* storage = owner->AsyncStorageData();
	const shared_str key = normalized_name;
	CModelPool::AsyncStorage::state_ptr state;
	{
		xrCriticalSectionGuard guard(storage->guard);
		const auto existing = storage->in_flight.find(key);
		if (existing != storage->in_flight.end())
		{
			existing->second->assert_on_missing = existing->second->assert_on_missing || assert_on_missing;
			if (from_manifest && !existing->second->manifest_counted)
			{
				const prepared_model_stage existing_stage =
					existing->second->stage.load(std::memory_order_acquire);
				if (existing_stage != prepared_model_stage::finalized &&
					existing_stage != prepared_model_stage::failed)
				{
					existing->second->from_manifest.store(true, std::memory_order_release);
					existing->second->manifest_counted = true;
					storage->manifest_states.push_back(existing->second);
					++storage->manifest_inflight;
				}
			}
			return existing->second;
		}

		state = std::make_shared<CModelPool::AsyncStorage::PreparedModel>();
		state->key = key;
		state->requested_name = requested_name;
		state->assert_on_missing = assert_on_missing;
		state->from_manifest.store(from_manifest, std::memory_order_relaxed);
		storage->in_flight.emplace(key, state);
		if (from_manifest)
		{
			state->manifest_counted = true;
			storage->manifest_states.push_back(state);
			++storage->manifest_inflight;
		}
	}

	auto* context = xr_new<CModelPool::AsyncStorage::JobContext>();
	context->owner = owner;
	context->state = state;

	// Work-first scheduling: a pool worker never submits nested model preparation
	// back to the same pool. This avoids worker saturation/priority inversion and
	// also keeps dependency preparation on the worker that already owns the cold
	// model miss. Render-thread finalization remains separate.
	if (xr_jobs::is_worker_thread())
	{
		prepare_model_job(context);
		return state;
	}

	// Manifest warming is speculative and must yield to the frame. A runtime
	// miss, however, is followed immediately by wait_for_preparation() on the
	// render thread, so leaving it in the background lane directly creates a
	// frame-time spike behind unrelated queued work.
	const xr_jobs::priority preparation_priority = from_manifest ?
		xr_jobs::priority::background : xr_jobs::priority::high;
	if (!xr_jobs::submit_scoped(&prepare_model_job, context, storage->job_scope,
		&storage->job_group, preparation_priority))
	{
		xr_delete(context);
		finish_preparation(owner, state, prepared_model_stage::failed,
			"model preparation job rejected");
	}
	return state;
}

void pump_manifest_preparations(CModelPool* owner)
{
	CModelPool::AsyncStorage* storage = owner->AsyncStorageData();
	if (GetCurrentThreadId() != storage->finalization_thread_id)
		return;

	const u32 inflight_limit = static_cast<u32>(_max(8, psModelPrefetchInFlight));
	for (;;)
	{
		shared_str name;
		{
			xrCriticalSectionGuard guard(storage->guard);
			if (storage->manifest_cursor >= storage->manifest_pending.size() ||
				storage->manifest_inflight >= inflight_limit)
				break;
			name = storage->manifest_pending[storage->manifest_cursor++];
		}

		queue_model_preparation(owner, name.c_str(), name.c_str(), false, true);
	}
}

void wait_for_preparation(const CModelPool::AsyncStorage::state_ptr& state)
{
	std::unique_lock<std::mutex> lock(state->wait_mutex);
	state->wait_condition.wait(lock, [&state]()
	{
		const prepared_model_stage stage = state->stage.load(std::memory_order_acquire);
		return stage == prepared_model_stage::ready || stage == prepared_model_stage::failed ||
			stage == prepared_model_stage::finalized;
	});
}

void erase_in_flight(CModelPool* owner, const CModelPool::AsyncStorage::state_ptr& state)
{
	xrCriticalSectionGuard guard(owner->AsyncStorageData()->guard);
	const auto found = owner->AsyncStorageData()->in_flight.find(state->key);
	if (found != owner->AsyncStorageData()->in_flight.end() && found->second == state)
		owner->AsyncStorageData()->in_flight.erase(found);
}

bool finalize_prepared_model(CModelPool* owner, const CModelPool::AsyncStorage::state_ptr& state)
{
	prepared_model_stage expected = prepared_model_stage::ready;
	if (!state->stage.compare_exchange_strong(expected, prepared_model_stage::finalizing,
		std::memory_order_acq_rel))
		return expected == prepared_model_stage::finalized;

	// External skeleton LODs are finalized first. This prevents the existing
	// CKinematics loader from falling back to synchronous disk IO for a child.
	for (const std::weak_ptr<CModelPool::AsyncStorage::PreparedModel>& dependency_ref : state->dependencies)
	{
		const CModelPool::AsyncStorage::state_ptr dependency = dependency_ref.lock();
		if (!dependency || dependency.get() == state.get())
			continue;

		prepared_model_stage dependency_stage = dependency->stage.load(std::memory_order_acquire);
		if (dependency_stage == prepared_model_stage::finalizing)
		{
			finish_preparation(owner, state, prepared_model_stage::failed,
				"cyclic prepared-model dependency");
			return false;
		}
		if (dependency_stage == prepared_model_stage::queued ||
			dependency_stage == prepared_model_stage::preparing)
		{
			wait_for_preparation(dependency);
			dependency_stage = dependency->stage.load(std::memory_order_acquire);
		}
		if (dependency_stage == prepared_model_stage::ready)
			finalize_prepared_model(owner, dependency);
	}

	// Another path may have loaded the model while worker preparation was active.
	if (owner->Instance_Find(state->key.c_str()))
	{
		publish_resident_motions(owner, state);
		release_prepared_payload(state);
		state->stage.store(prepared_model_stage::finalized, std::memory_order_release);
		state->wait_condition.notify_all();
		{
			CModelPool::AsyncStorage* storage = owner->AsyncStorageData();
			xrCriticalSectionGuard guard(storage->guard);
			mark_manifest_complete_locked(storage, state);
		}
		erase_in_flight(owner, state);
		return true;
	}

	xr_vector<prepared_motion_source> motion_sources;
	motion_sources.reserve(state->motion_blobs.size());
	for (const prepared_motion_ptr& blob : state->motion_blobs)
		motion_sources.push_back({blob->name, blob->bytes.data(), static_cast<u32>(blob->bytes.size())});

	IReader reader(state->ogf_bytes.data(), static_cast<int>(state->ogf_bytes.size()));
	dxRender_Visual* base = nullptr;
	{
		prepared_motion_source_scope motion_scope(&motion_sources);
		xrCriticalSectionGuard load_guard(owner->ModelLoadGuard());
		model_load_scope loading;
		base = owner->Instance_Load(state->key.c_str(), &reader, TRUE);
		if (base)
			g_pGamePersistent->RegisterModel(base);
	}

	if (!base)
	{
		finish_preparation(owner, state, prepared_model_stage::failed,
			"render-thread model finalization failed");
		return false;
	}

	// A successful visual load guarantees that every supplied OMF has been
	// installed in the shared motion container. Publish that fact before raw
	// blobs are released, closing the window in which a later spawn could start
	// another copy of the same animation pack.
	publish_resident_motions(owner, state);
	release_prepared_payload(state);
	state->stage.store(prepared_model_stage::finalized, std::memory_order_release);
	state->wait_condition.notify_all();
	{
		CModelPool::AsyncStorage* storage = owner->AsyncStorageData();
		xrCriticalSectionGuard guard(storage->guard);
		mark_manifest_complete_locked(storage, state);
	}
	erase_in_flight(owner, state);
	return true;
}
}

dxRender_Visual* CModelPool::Instance_Create(u32 type)
{
	dxRender_Visual* V = NULL;

	// Check types
	switch (type)
	{
	case MT_NORMAL: // our base visual
		V = xr_new<Fvisual>();
		break;
	case MT_HIERRARHY:
		V = xr_new<FHierrarhyVisual>();
		break;
	case MT_PROGRESSIVE: // dynamic-resolution visual
		V = xr_new<FProgressive>();
		break;
	case MT_SKELETON_ANIM:
		V = xr_new<CKinematicsAnimated>();
		break;
	case MT_SKELETON_RIGID:
		V = xr_new<CKinematics>();
		break;
	case MT_SKELETON_GEOMDEF_PM:
		V = xr_new<CSkeletonX_PM>();
		break;
	case MT_SKELETON_GEOMDEF_ST:
		V = xr_new<CSkeletonX_ST>();
		break;
	case MT_PARTICLE_EFFECT:
		V = xr_new<PS::CParticleEffect>();
		break;
	case MT_PARTICLE_GROUP:
		V = xr_new<PS::CParticleGroup>();
		break;
#ifndef _EDITOR
	case MT_LOD:
		V = xr_new<FLOD>();
		break;
	case MT_TREE_ST:
		V = xr_new<FTreeVisual_ST>();
		break;
	case MT_TREE_PM:
		V = xr_new<FTreeVisual_PM>();
		break;
#endif
	default:
		FATAL("Unknown visual type");
		break;
	}
	R_ASSERT(V);
	V->Type = type;
	return V;
}

dxRender_Visual* CModelPool::Instance_Duplicate(dxRender_Visual* V)
{
	xrCriticalSectionGuard guard(modelPoolGuard);
	R_ASSERT(V);
	dxRender_Visual* N = Instance_Create(V->Type);
	N->Copy(V);
	N->Spawn();

	const MODEL_INDEX_BY_POINTER::const_iterator index = ModelIndexByPointer.find(V);
	if (index != ModelIndexByPointer.end())
	{
		VERIFY(index->second < Models.size());
		Models[index->second].refs++;
	}
	return N;
}

dxRender_Visual* CModelPool::Instance_Load(const char* N, BOOL allow_register, bool assert)
{
	dxRender_Visual* V;
	string_path fn;
	string_path name;

	// Add default ext if no ext at all
	if (0 == strext(N)) strconcat(sizeof(name), name, N, ".ogf");
	else xr_strcpy(name, sizeof(name), N);

	// Open directly instead of performing FS.exist() followed by the same lookup
	// again in FS.r_open(). Model creation is one of the hottest save-load paths.
	IReader* data = FS.r_open(N);
	if (data)
	{
		xr_strcpy(fn, N);
	}
	else if ((data = FS.r_open("$level$", name)) != nullptr)
	{
		FS.update_path(fn, "$level$", name);
	}
	else if ((data = FS.r_open("$game_meshes$", name)) != nullptr)
	{
		FS.update_path(fn, "$game_meshes$", name);
	}
	else
	{
#ifdef _EDITOR
		Msg("!Can't find model file '%s'.", name);
		return nullptr;
#else
		if (assert)
			Debug.fatal(DEBUG_INFO, "Can't find model file '%s'.", name);
		return nullptr;
#endif
	}

	// Actual loading
#ifdef DEBUG
	if (bLogging)		Msg		("- Uncached model loading: %s",fn);
#endif // DEBUG

	ogf_header H;
	data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
	V = Instance_Create(H.type);
	V->Load(N, data, 0);
	FS.r_close(data);
	g_pGamePersistent->RegisterModel(V);

	// Registration
	if (allow_register) Instance_Register(N, V);

	return V;
}

dxRender_Visual* CModelPool::Instance_Load(LPCSTR name, IReader* data, BOOL allow_register)
{
	dxRender_Visual* V;

	ogf_header H;
	data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
	V = Instance_Create(H.type);
	V->Load(name, data, 0);

	// Registration
	if (allow_register) Instance_Register(name, V);
	return V;
}

void CModelPool::Instance_Register(LPCSTR N, dxRender_Visual* V)
{
	xrCriticalSectionGuard guard(modelPoolGuard);
	ModelDef M;
	M.name = N;
	M.model = V;
	Models.push_back(M);

	const u32 index = static_cast<u32>(Models.size() - 1);
	VERIFY(ModelByName.find(M.name) == ModelByName.end());
	VERIFY(ModelIndexByPointer.find(V) == ModelIndexByPointer.end());
	ModelByName.emplace(M.name, V);
	ModelIndexByPointer.emplace(V, index);
}

void CModelPool::EraseModelIndex(u32 index)
{
	VERIFY(index < Models.size());
	ModelByName.erase(Models[index].name);
	ModelIndexByPointer.erase(Models[index].model);
	Models.erase(Models.begin() + index);

	for (u32 i = index; i < Models.size(); ++i)
		ModelIndexByPointer[Models[i].model] = i;
}

dxRender_Visual* CModelPool::ExtractUnreferencedBaseForChild(const shared_str& name)
{
	const MODEL_BY_NAME::const_iterator found = ModelByName.find(name);
	if (found == ModelByName.end())
		return nullptr;

	const MODEL_INDEX_BY_POINTER::const_iterator index = ModelIndexByPointer.find(found->second);
	VERIFY(index != ModelIndexByPointer.end());
	if (index == ModelIndexByPointer.end())
		return nullptr;
	VERIFY(index->second < Models.size());
	if (index->second >= Models.size())
		return nullptr;
	if (Models[index->second].refs != 0)
		return nullptr;

	// CreateChild transfers ownership to the base visual currently being loaded.
	// Async preparation can register an external LOD before its parent asks for
	// it. Detach that otherwise-unreferenced base so the parent becomes its only
	// owner instead of leaving the same pointer in Models for a second deletion.
	dxRender_Visual* model = found->second;
	EraseModelIndex(index->second);
	return model;
}

void CModelPool::Destroy()
{
	// Destruction releases shared motions and renderer resources, so it must not
	// overlap an uncached model load. Keep the global lock order load -> pool.
	xrCriticalSectionGuard load_guard(modelLoadGuard);
	xrCriticalSectionGuard pool_guard(modelPoolGuard);
	// Pool
	Pool.clear();

	// Registry
	while (!Registry.empty())
	{
		REGISTRY_IT it = Registry.begin();
		dxRender_Visual* V = (dxRender_Visual*)it->first;
#ifdef _DEBUG
		Msg				("ModelPool: Destroy object: '%s'",*V->dbg_name);
#endif
		DeleteInternal(V,TRUE);
	}

	// Base/Reference
	xr_vector<ModelDef>::iterator I = Models.begin();
	xr_vector<ModelDef>::iterator E = Models.end();
	for (; I != E; I++)
	{
		I->model->Release();
		xr_delete(I->model);
	}

	Models.clear();
	ModelByName.clear();
	ModelIndexByPointer.clear();

	// cleanup motions container
	g_pMotionsContainer->clean(false);
}

CModelPool::CModelPool()
{
	asyncStorage = xr_new<AsyncStorage>();
	Models.reserve(4096);
	ModelByName.reserve(4096);
	ModelIndexByPointer.reserve(4096);
	Registry.reserve(8192);
	ModelsToDelete.reserve(256);

	bLogging = TRUE;
	bForceDiscard = FALSE;
	g_pMotionsContainer = xr_new<motions_container>();
}

CModelPool::~CModelPool()
{
	// Let accepted jobs release their heap contexts before closing the scope.
	// task_scope cancellation intentionally skips callbacks, so cancelling first
	// would leak the per-job preparation context.
	xr_jobs::wait(asyncStorage->job_group);
	asyncStorage->job_scope.cancel_and_wait();
	Destroy();
	xr_delete(g_pMotionsContainer);
	xr_delete(asyncStorage);
}

dxRender_Visual* CModelPool::Instance_Find(LPCSTR N)
{
	xrCriticalSectionGuard guard(modelPoolGuard);
	const shared_str key = N;
	const MODEL_BY_NAME::const_iterator found = ModelByName.find(key);
	return found != ModelByName.end() ? found->second : nullptr;
}

dxRender_Visual* CModelPool::CreateFromPrepared(LPCSTR normalized_name, LPCSTR requested_name, bool assert,
	u64* wait_ticks, u64* finalize_ticks)
{
	if (!psModelAsyncPrepare || xr_jobs::worker_count() == 0 || xr_jobs::is_worker_thread() ||
		GetCurrentThreadId() != asyncStorage->finalization_thread_id)
		return nullptr;

	const AsyncStorage::state_ptr state = queue_model_preparation(
		this, normalized_name, requested_name, assert, false);
	const u64 wait_started = CPU::QPC();
	wait_for_preparation(state);
	const u64 wait_finished = CPU::QPC();
	if (wait_ticks)
		*wait_ticks = wait_finished - wait_started;

	prepared_model_stage stage = state->stage.load(std::memory_order_acquire);
	if (stage == prepared_model_stage::ready)
	{
		const u64 finalize_started = CPU::QPC();
		finalize_prepared_model(this, state);
		const u64 finalize_finished = CPU::QPC();
		if (finalize_ticks)
			*finalize_ticks = finalize_finished - finalize_started;
		stage = state->stage.load(std::memory_order_acquire);
	}

	if (stage == prepared_model_stage::failed)
	{
		Msg("! Prepared model fallback for '%s': %s", requested_name, state->error.c_str());
		erase_in_flight(this, state);
		return nullptr;
	}

	return Instance_Find(normalized_name);
}

void CModelPool::RecordLevelModel(LPCSTR normalized_name)
{
	if (!psModelPrefetchManifest || !is_prefetch_model_name_safe(normalized_name))
		return;

	xrCriticalSectionGuard guard(asyncStorage->guard);
	if (!asyncStorage->level_recording ||
		asyncStorage->failed_models.find(normalized_name) != asyncStorage->failed_models.end() ||
		asyncStorage->observed_models.size() >= static_cast<size_t>(_max(1, psModelPrefetchManifestMax)))
		return;
	const shared_str name = normalized_name;
	if (asyncStorage->observed_models.emplace(name).second)
		asyncStorage->observed_order.push_back(name);
}

dxRender_Visual* CModelPool::Create(const char* name, IReader* data, bool assert)
{
#ifdef _EDITOR
	if (!name||!name[0])	return 0;
#endif
	string_path low_name;
	VERIFY(xr_strlen(name)<sizeof(low_name));
	xr_strcpy(low_name, name);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;
	const shared_str key = low_name;
	if (!data)
		RecordLevelModel(low_name);

	// The common cached path only needs the short registry lock. In particular,
	// it no longer waits for an unrelated background OGF load to finish IO,
	// parsing, shader lookup and resource creation.
	{
		xrCriticalSectionGuard pool_guard(modelPoolGuard);
		POOL_IT it = Pool.find(key);
		if (it != Pool.end())
		{
			dxRender_Visual* Model = it->second;
			Model->Spawn();
			Pool.erase(it);
			return Model;
		}

		const MODEL_BY_NAME::const_iterator found = ModelByName.find(key);
		if (found != ModelByName.end())
		{
			dxRender_Visual* Model = Instance_Duplicate(found->second);
			Registry.emplace(Model, key);
			return Model;
		}
	}

	// Use an already queued manifest/spawn preparation or start a worker-side
	// OGF/OMF parse now. Only final visual creation is serialized on this thread.
	if (!data && !strchr(low_name, '#') && psModelAsyncPrepare && !xr_jobs::is_worker_thread() &&
		GetCurrentThreadId() == asyncStorage->finalization_thread_id)
	{
		u64 prepared_wait_ticks = 0;
		u64 prepared_finalize_ticks = 0;
		dxRender_Visual* Base = CreateFromPrepared(low_name, name, assert,
			&prepared_wait_ticks, &prepared_finalize_ticks);

		if (Base)
		{
			xrCriticalSectionGuard pool_guard(modelPoolGuard);
			dxRender_Visual* Model = Instance_Duplicate(Base);
			Registry.emplace(Model, key);
			return Model;
		}
	}

	// Compatibility fallback for worker-thread callers, external IReader children
	// and any model rejected by the conservative preparation parser.
	const u64 wait_started = CPU::QPC();
	xrCriticalSectionGuard load_guard(modelLoadGuard);
	const u64 load_lock_acquired = CPU::QPC();
	dxRender_Visual* Model = nullptr;
	{
		xrCriticalSectionGuard pool_guard(modelPoolGuard);
		POOL_IT it = Pool.find(key);
		if (it != Pool.end())
		{
			Model = it->second;
			Model->Spawn();
			Pool.erase(it);
		}
		else
		{
			const MODEL_BY_NAME::const_iterator found = ModelByName.find(key);
			if (found != ModelByName.end())
			{
				Model = Instance_Duplicate(found->second);
				Registry.emplace(Model, key);
			}
		}
	}
	if (Model)
	{
		const u64 finished = CPU::QPC();
		return Model;
	}

	dxRender_Visual* Base = nullptr;
	const u64 load_started = CPU::QPC();
	{
		model_load_scope loading;
		if (data)
			Base = Instance_Load(low_name, data, TRUE);
		else
			Base = Instance_Load(low_name, TRUE, assert);
	}
	const u64 load_finished = CPU::QPC();
	if (!Base)
	{
		return nullptr;
	}

	{
		xrCriticalSectionGuard pool_guard(modelPoolGuard);
		Model = Instance_Duplicate(Base);
		Registry.emplace(Model, key);
	}

	const u64 finished = CPU::QPC();
	return Model;
}

dxRender_Visual* CModelPool::CreateChild(LPCSTR name, IReader* data)
{
	string256 low_name;
	VERIFY(xr_strlen(name)<256);
	xr_strcpy(low_name, name);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;
	const shared_str key = low_name;

	// Children loaded while constructing a base visual belong to that base and
	// must not be cloned. This was a process-global flag that could leak its state
	// after an early load failure; make it local to the active loader instead.
	const bool duplicate = model_load_depth == 0;
	{
		xrCriticalSectionGuard pool_guard(modelPoolGuard);
		const MODEL_BY_NAME::const_iterator found = ModelByName.find(key);
		if (found != ModelByName.end())
		{
			if (duplicate)
				return Instance_Duplicate(found->second);
			if (dxRender_Visual* child = ExtractUnreferencedBaseForChild(key))
				return child;
		}
	}

	const u64 wait_started = CPU::QPC();
	xrCriticalSectionGuard load_guard(modelLoadGuard);
	const u64 load_lock_acquired = CPU::QPC();
	{
		xrCriticalSectionGuard pool_guard(modelPoolGuard);
		const MODEL_BY_NAME::const_iterator found = ModelByName.find(key);
		if (found != ModelByName.end())
		{
			dxRender_Visual* Model = duplicate ? Instance_Duplicate(found->second) :
				ExtractUnreferencedBaseForChild(key);
			if (Model)
			{
				const u64 finished = CPU::QPC();
				return Model;
			}
		}
	}

	dxRender_Visual* Base = nullptr;
	const u64 load_started = CPU::QPC();
	{
		model_load_scope loading;
		if (data)
			Base = Instance_Load(low_name, data, FALSE);
		else
			Base = Instance_Load(low_name, FALSE);
	}
	const u64 load_finished = CPU::QPC();

	if (!Base || !duplicate)
		return Base;

	xrCriticalSectionGuard pool_guard(modelPoolGuard);
	return Instance_Duplicate(Base);
}

extern BOOL ENGINE_API g_bRendering;

void CModelPool::DeleteInternal(dxRender_Visual* & V, BOOL bDiscard)
{
	VERIFY(!g_bRendering);
	if (!V) return;
	V->Depart();
	if (bDiscard || bForceDiscard)
	{
		Discard(V, TRUE);
	}
	else
	{
		//
		REGISTRY_IT it = Registry.find(V);
		if (it != Registry.end())
		{
			// Registry entry found - move it to pool and reset changed shader/texture if necessary
			xr_vector<IRenderVisual*>* children = V->get_children();
			if (children)
				for (auto* child : *children)
					child->ResetShaderTexture();
			else
				V->ResetShaderTexture();

			Pool.insert(mk_pair(it->second, V));
		}
		else
		{
			// Registry entry not-found - just special type of visual / particles / etc.
			xr_delete(V);
		}
	}
	V = NULL;
}

void CModelPool::Delete(dxRender_Visual* & V, BOOL bDiscard)
{
	if (bDiscard)
	{
		// Complete discard can release shared motions and child resources. Order it
		// against uncached loads while leaving ordinary pool returns concurrent.
		xrCriticalSectionGuard load_guard(modelLoadGuard);
		xrCriticalSectionGuard pool_guard(modelPoolGuard);
		if (NULL == V) return;
		if (g_bRendering)
		{
			VERIFY(!bDiscard);
			ModelsToDelete.push_back(V);
		}
		else
		{
			DeleteInternal(V, bDiscard);
		}
		V = NULL;
		return;
	}

	xrCriticalSectionGuard pool_guard(modelPoolGuard);
	if (NULL == V) return;
	if (g_bRendering)
	{
		VERIFY(!bDiscard);
		ModelsToDelete.push_back(V);
	}
	else
	{
		DeleteInternal(V, bDiscard);
	}
	V = NULL;
}

void CModelPool::DeleteQueue()
{
	xrCriticalSectionGuard guard(modelPoolGuard);
	for (u32 it = 0; it < ModelsToDelete.size(); it++)
		DeleteInternal(ModelsToDelete[it]);
	ModelsToDelete.clear();
}

void CModelPool::Discard(dxRender_Visual* & V, BOOL b_complete)
{
	xrCriticalSectionGuard load_guard(modelLoadGuard);
	xrCriticalSectionGuard pool_guard(modelPoolGuard);
	//
	REGISTRY_IT it = Registry.find(V);
	if (it != Registry.end())
	{
		// Resolve the base model through the O(1) name and pointer indexes.
		const shared_str& name = it->second;
		const MODEL_BY_NAME::iterator base_by_name = ModelByName.find(name);
		if (base_by_name != ModelByName.end())
		{
			const MODEL_INDEX_BY_POINTER::iterator base_index = ModelIndexByPointer.find(base_by_name->second);
			VERIFY(base_index != ModelIndexByPointer.end());
			VERIFY(base_index->second < Models.size());

			const u32 index = base_index->second;
			ModelDef& model = Models[index];
			if (b_complete || strchr(*name, '#'))
			{
				VERIFY(model.refs > 0);
				model.refs--;
				if (model.refs == 0)
				{
					dxRender_Visual* base = model.model;
					EraseModelIndex(index);
					bForceDiscard = TRUE;
					base->Release();
					xr_delete(base);
					bForceDiscard = FALSE;
				}
			}
			else if (model.refs > 0)
			{
				model.refs--;
			}
		}
		// Registry
		xr_delete(V);
		Registry.erase(it);
	}
	else
	{
		// Registry entry not-found - just special type of visual / particles / etc.
		xr_delete(V);
	}
	V = NULL;
}

void CModelPool::ProcessAsyncFinalization(u32 budget_ms, bool wait_for_all)
{
	if (GetCurrentThreadId() != asyncStorage->finalization_thread_id ||
		(!wait_for_all && (!psModelAsyncPrepare || budget_ms == 0)))
		return;

	const auto drain_ready = [this](u64 started, u64 budget_ticks, bool drain_all)
	{
		for (;;)
		{
			AsyncStorage::state_ptr state;
			{
				xrCriticalSectionGuard guard(asyncStorage->guard);
				if (asyncStorage->ready_to_finalize.empty())
					break;
				state = asyncStorage->ready_to_finalize.back();
				asyncStorage->ready_to_finalize.pop_back();
			}

			finalize_prepared_model(this, state);
			if (!drain_all && budget_ticks && CPU::QPC() - started >= budget_ticks)
				break;
		}
	};

	if (wait_for_all)
	{
		for (;;)
		{
			pump_manifest_preparations(this);
			xr_jobs::wait(asyncStorage->job_group);
			drain_ready(0, 0, true);
			pump_manifest_preparations(this);

			bool complete = false;
			{
				xrCriticalSectionGuard guard(asyncStorage->guard);
				complete = asyncStorage->manifest_cursor >= asyncStorage->manifest_pending.size() &&
					asyncStorage->manifest_inflight == 0 && asyncStorage->ready_to_finalize.empty();
			}
			if (complete && asyncStorage->job_group.empty())
				break;
		}
		return;
	}

	pump_manifest_preparations(this);
	const u64 started = CPU::QPC();
	const u64 budget_ticks = CPU::qpc_freq
		? CPU::qpc_freq * static_cast<u64>(budget_ms) / 1000u
		: 0;
	drain_ready(started, budget_ticks, false);
	pump_manifest_preparations(this);
}

void CModelPool::BeginLevelPrefetch(LPCSTR level_name)
{
	static bool adaptive_memory_initialized = false;
	if (!adaptive_memory_initialized && !strstr(Core.Params, "-no_adaptive_memory"))
	{
		adaptive_memory_initialized = true;
		const xr_memory_policy::snapshot memory_policy = xr_memory_policy::query();
		// Only retune stock defaults. Explicit console/config values remain
		// authoritative for mod packs that already budget these caches themselves.
		if (psModelPrefetchManifestMax == 4096)
			psModelPrefetchManifestMax = static_cast<int>(xr_memory_policy::model_manifest_max(memory_policy));
		if (psModelPrefetchInFlight == 64)
			psModelPrefetchInFlight = static_cast<int>(xr_memory_policy::model_prefetch_inflight(memory_policy));
		Msg("* [memory-policy] model manifest=%d in_flight=%d (%s)",
			psModelPrefetchManifestMax, psModelPrefetchInFlight, xr_memory_policy::tier_name(memory_policy.mode));
	}

	// A renderer can be recreated without a clean level transition during video
	// reset. Complete any previous manifest jobs before changing the recording key,
	// even when the feature was disabled through the console between levels.
	FinishLevelPrefetch();
	if (!psModelPrefetchManifest || !psModelAsyncPrepare || xr_jobs::worker_count() == 0 ||
		!level_name || !level_name[0])
		return;

	{
		xrCriticalSectionGuard guard(asyncStorage->guard);
		asyncStorage->active_level = level_name;
		asyncStorage->observed_models.clear();
		asyncStorage->observed_order.clear();
		asyncStorage->failed_models.clear();
		asyncStorage->manifest_states.clear();
		asyncStorage->manifest_pending.clear();

		// Reserve a bounded working set during the loading boundary. The vectors
		// can still grow to psModelPrefetchManifestMax, but normal levels no longer
		// retain a 4096-entry baseline for the whole process lifetime.
		const size_t manifest_limit = static_cast<size_t>(_max(1, psModelPrefetchManifestMax));
		const size_t warm_entries = _min(manifest_limit, size_t(1024));
		if (asyncStorage->observed_order.capacity() < warm_entries)
			asyncStorage->observed_order.reserve(warm_entries);
		if (asyncStorage->manifest_states.capacity() < warm_entries)
			asyncStorage->manifest_states.reserve(warm_entries);
		asyncStorage->observed_models.reserve(warm_entries);
		asyncStorage->failed_models.reserve(_min(warm_entries, size_t(128)));

		asyncStorage->manifest_cursor = 0;
		asyncStorage->manifest_inflight = 0;
		asyncStorage->level_recording = true;
	}

	string_path manifest_path;
	build_manifest_path(level_name, manifest_path);
	IReader* manifest = FS.r_open(manifest_path);
	if (!manifest)
		return;

	if (manifest->length() < static_cast<int>(sizeof(u32) * 3) ||
		static_cast<u32>(manifest->length()) > model_manifest_max_file_size)
	{
		FS.r_close(manifest);
		return;
	}

	const u32 magic = manifest->r_u32();
	const u32 version = manifest->r_u32();
	const u32 count = manifest->r_u32();
	if (magic != model_manifest_magic || version != model_manifest_version ||
		count > static_cast<u32>(_max(1, psModelPrefetchManifestMax)))
	{
		FS.r_close(manifest);
		return;
	}

	xr_vector<shared_str> names;
	xr_unordered_set<shared_str> unique_names;
	names.reserve(count);
	unique_names.reserve(count);
	for (u32 index = 0; index < count; ++index)
	{
		xr_string value;
		if (!read_string_z_safe(*manifest, value, sizeof(string_path)))
		{
			names.clear();
			break;
		}

		string_path normalized;
		normalize_model_name(value.c_str(), normalized);
		if (!is_prefetch_model_name_safe(normalized))
			continue;
		const shared_str name = normalized;
		if (unique_names.emplace(name).second)
			names.push_back(name);
	}
	FS.r_close(manifest);

	if (!names.empty())
	{
		{
			xrCriticalSectionGuard guard(asyncStorage->guard);
			/* Keep the learned set cumulative across visits. Previously the loaded
			** manifest warmed this session, but SaveLevelPrefetchManifest replaced
			** it with only the route observed since the latest level load. Modded
			** levels with several distant populations would therefore forget cold
			** NPC/outfit models on alternating routes and pay the same synchronous
			** spawn miss again. The existing manifest limit remains authoritative. */
			for (const shared_str& name : names)
			{
				if (asyncStorage->observed_models.emplace(name).second)
					asyncStorage->observed_order.push_back(name);
			}
			asyncStorage->manifest_pending = std::move(names);
			asyncStorage->manifest_cursor = 0;
		}
		pump_manifest_preparations(this);
		Msg("* [model-prefetch] loaded %u manifest entries for level '%s' (window=%d)",
			static_cast<u32>(unique_names.size()), level_name, _max(8, psModelPrefetchInFlight));
	}
}

void CModelPool::FinishLevelPrefetch()
{
	if (GetCurrentThreadId() != asyncStorage->finalization_thread_id)
		return;

	ProcessAsyncFinalization(0, true);
	{
		xrCriticalSectionGuard guard(asyncStorage->guard);
		// Manifest payload is level-scoped. Return its high-water backing storage
		// after all preparation jobs are drained instead of carrying the largest
		// level's allocation through menus and subsequent smaller levels.
		asyncStorage->manifest_states.clear_and_free();
		asyncStorage->manifest_pending.clear_and_free();
		asyncStorage->manifest_cursor = 0;
		asyncStorage->manifest_inflight = 0;
		asyncStorage->motion_in_flight.clear();
		asyncStorage->motion_in_flight.rehash(0);
		if (asyncStorage->in_flight.empty())
			asyncStorage->in_flight.rehash(0);
	}
}

void CModelPool::SaveLevelPrefetchManifest()
{
	if (!psModelPrefetchManifest)
	{
		xrCriticalSectionGuard guard(asyncStorage->guard);
		asyncStorage->level_recording = false;
		asyncStorage->active_level = nullptr;
		asyncStorage->observed_models.clear();
		asyncStorage->observed_models.rehash(0);
		asyncStorage->observed_order.clear_and_free();
		asyncStorage->failed_models.clear();
		asyncStorage->failed_models.rehash(0);
		return;
	}

	shared_str level_name;
	xr_vector<shared_str> names;
	{
		xrCriticalSectionGuard guard(asyncStorage->guard);
		if (!asyncStorage->level_recording || !asyncStorage->active_level.size())
			return;

		level_name = asyncStorage->active_level;
		names.reserve(asyncStorage->observed_order.size());
		for (const shared_str& name : asyncStorage->observed_order)
		{
			if (asyncStorage->observed_models.find(name) != asyncStorage->observed_models.end() &&
				asyncStorage->failed_models.find(name) == asyncStorage->failed_models.end())
				names.push_back(name);
		}
		asyncStorage->level_recording = false;
	}

	if (names.size() > static_cast<size_t>(_max(1, psModelPrefetchManifestMax)))
		names.resize(static_cast<size_t>(_max(1, psModelPrefetchManifestMax)));

	string_path manifest_path;
	build_manifest_path(level_name.c_str(), manifest_path);
	VerifyPath(manifest_path);
	IWriter* writer = FS.w_open(manifest_path);
	if (!writer)
	{
		// Recording was already closed above, so these sets are dead even when
		// the manifest cannot be written. Do not pin their high-water capacity.
		xrCriticalSectionGuard guard(asyncStorage->guard);
		asyncStorage->active_level = nullptr;
		asyncStorage->observed_models.clear();
		asyncStorage->observed_models.rehash(0);
		asyncStorage->observed_order.clear_and_free();
		asyncStorage->failed_models.clear();
		asyncStorage->failed_models.rehash(0);
		return;
	}

	writer->w_u32(model_manifest_magic);
	writer->w_u32(model_manifest_version);
	writer->w_u32(static_cast<u32>(names.size()));
	for (const shared_str& name : names)
		writer->w_stringZ(name);
	FS.w_close(writer);
	Msg("* [model-prefetch] saved %u models for level '%s'", static_cast<u32>(names.size()), level_name.c_str());

	// The recording set is no longer consulted after the manifest is written.
	// Compact it here, outside gameplay, so a content-heavy level cannot pin its
	// peak hash/vector capacity for the rest of the process.
	{
		xrCriticalSectionGuard guard(asyncStorage->guard);
		asyncStorage->active_level = nullptr;
		asyncStorage->observed_models.clear();
		asyncStorage->observed_models.rehash(0);
		asyncStorage->observed_order.clear_and_free();
		asyncStorage->failed_models.clear();
		asyncStorage->failed_models.rehash(0);
	}
}

void CModelPool::Prefetch()
{
	Logging(FALSE);
	string256 section;
	strconcat(sizeof(section), section, "prefetch_visuals_", g_pGamePersistent->m_game_params.m_game_type);
	CInifile::Sect& sect = pSettings->r_section(section);

	if (psModelAsyncPrepare && xr_jobs::worker_count() > 0 &&
		GetCurrentThreadId() == asyncStorage->finalization_thread_id)
	{
		for (CInifile::SectCIt iterator = sect.Data.begin(); iterator != sect.Data.end(); ++iterator)
		{
			string_path normalized;
			normalize_model_name(iterator->first.c_str(), normalized);
			queue_model_preparation(this, normalized, iterator->first.c_str(), true, false);
		}
		ProcessAsyncFinalization(0, true);

		// Preserve the legacy prefetch contract: missing required entries still
		// take the asserting fallback, and a reusable duplicate is placed in Pool.
		for (CInifile::SectCIt iterator = sect.Data.begin(); iterator != sect.Data.end(); ++iterator)
		{
			dxRender_Visual* visual = Create(iterator->first.c_str());
			Delete(visual, FALSE);
		}
	}
	else
	{
		for (CInifile::SectCIt iterator = sect.Data.begin(); iterator != sect.Data.end(); ++iterator)
		{
			dxRender_Visual* visual = Create(iterator->first.c_str());
			Delete(visual, FALSE);
		}
	}
	Logging(TRUE);
}

void CModelPool::Prefetch_One(LPCSTR name, bool assert)
{
	if (psModelAsyncPrepare && xr_jobs::worker_count() > 0 && xr_jobs::is_worker_thread())
	{
		string_path normalized;
		normalize_model_name(name, normalized);
		RecordLevelModel(normalized);

		// SPAWN_ANTIFREEZE releases its postponed M_SPAWN immediately after this
		// call returns. Merely queueing preparation allowed the main thread to race
		// the worker and synchronously wait/load the same cold model. Do not finalize
		// on this worker; only make CPU-side model/motion/texture preparation a real
		// barrier before the spawn event becomes visible again.
		const AsyncStorage::state_ptr state = queue_model_preparation(
			this, normalized, name, assert, false);

		// Prefetch_One already runs on a job worker. Claim queued preparation here
		// instead of blocking that worker on work submitted back to the same pool.
		// The queued callback is race-safe: prepare_model_state() is single-owner via
		// CAS and becomes a cheap no-op if this worker won the claim.
		prepare_model_state(this, state);
		wait_for_preparation(state);
		return;
	}

	dxRender_Visual* visual = Create(name, nullptr, assert);
	if (visual)
		Delete(visual, FALSE);
}

bool CModelPool::Exists(LPCSTR N)
{
	string_path low_name;
	VERIFY(xr_strlen(N) < sizeof(low_name));
	xr_strcpy(low_name, N);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;

	const shared_str key = low_name;
	{
		xrCriticalSectionGuard pool_guard(modelPoolGuard);
		if (Pool.find(key) != Pool.end() || ModelByName.find(key) != ModelByName.end())
			return true;
	}

	// Prefetch model
	dxRender_Visual* V = Create(N, 0, false);
	if (V) 
	{
		Delete(V, FALSE);
		return true;
	}

	return false;
}

dxRender_Visual* CModelPool::CreatePE(PS::CPEDef* source)
{
	PS::CParticleEffect* V = (PS::CParticleEffect*)Instance_Create(MT_PARTICLE_EFFECT);
	V->Compile(source);
	return V;
}

dxRender_Visual* CModelPool::CreatePG(PS::CPGDef* source)
{
	PS::CParticleGroup* V = (PS::CParticleGroup*)Instance_Create(MT_PARTICLE_GROUP);
	V->Compile(source);
	return V;
}

void CModelPool::ClearPool(BOOL b_complete)
{
	xrCriticalSectionGuard load_guard(modelLoadGuard);
	xrCriticalSectionGuard pool_guard(modelPoolGuard);
	POOL_IT _I = Pool.begin();
	POOL_IT _E = Pool.end();
	for (; _I != _E; _I++)
	{
		Discard(_I->second, b_complete);
	}
	Pool.clear();
}

void CModelPool::dump()
{
	xrCriticalSectionGuard guard(modelPoolGuard);
	Log("--- model pool --- begin:");
	u32 sz = 0;
	u32 k = 0;
	for (xr_vector<ModelDef>::iterator I = Models.begin(); I != Models.end(); I++)
	{
		CKinematics* K = PCKinematics(I->model);
		if (K)
		{
			u32 cur = K->mem_usage(false);
			sz += cur;
			Msg("#%3d: [%3d/%5d Kb] - %s", k++, I->refs, cur / 1024, I->name.c_str());
		}
	}
	Msg("--- models: %d, mem usage: %d Kb ", k, sz / 1024);
	sz = 0;
	k = 0;
	int free_cnt = 0;
	for (REGISTRY_IT it = Registry.begin(); it != Registry.end(); it++)
	{
		CKinematics* K = PCKinematics((dxRender_Visual*)it->first);
		VERIFY(K);
		if (K)
		{
			u32 cur = K->mem_usage(true);
			sz += cur;
			bool b_free = (Pool.find(it->second) != Pool.end());
			if (b_free) ++free_cnt;
			Msg("#%3d: [%s] [%5d Kb] - %s", k++, (b_free) ? "free" : "used", cur / 1024, it->second.c_str());
		}
	}
	Msg("--- instances: %d, free %d, mem usage: %d Kb ", k, free_cnt, sz / 1024);
	Log("--- model pool --- end.");
}

void CModelPool::memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system)
{
	xrCriticalSectionGuard guard(modelPoolGuard);
	vb_mem_video = 0;
	vb_mem_system = 0;
	ib_mem_video = 0;
	ib_mem_system = 0;

	xr_vector<ModelDef>::iterator it = Models.begin();
	xr_vector<ModelDef>::const_iterator en = Models.end();

	for (; it != en; ++it)
	{
		dxRender_Visual* ptr = it->model;
		Fvisual* vis_ptr = fast_dynamic_cast<Fvisual*>(ptr);

		if (vis_ptr == NULL)
			continue;
		D3D_BUFFER_DESC IB_desc;
		D3D_BUFFER_DESC VB_desc;

		vis_ptr->m_fast->p_rm_Indices->GetDesc(&IB_desc);

		ib_mem_video += IB_desc.ByteWidth;
		ib_mem_system += IB_desc.ByteWidth;

		vis_ptr->m_fast->p_rm_Vertices->GetDesc(&VB_desc);

		vb_mem_video += IB_desc.ByteWidth;
		vb_mem_system += IB_desc.ByteWidth;

	}
}

#ifdef _EDITOR
IC bool	_IsBoxVisible(dxRender_Visual* visual, const Fmatrix& transform)
{
    Fbox 		bb; 
    bb.xform	(visual->vis.box,transform);
    return 		::Render->occ_visible(bb);
}
IC bool	_IsValidShader(dxRender_Visual* visual, u32 priority, bool strictB2F)
{
	if (visual->shader)
        return (priority==visual->shader->E[0]->flags.iPriority)&&(strictB2F==visual->shader->E[0]->flags.bStrictB2F);
    return false;
}

void 	CModelPool::Render(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD)
{
    // render visual
    xr_vector<dxRender_Visual*>::iterator I,E;
    switch (m_pVisual->Type){
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:{
        if (_IsBoxVisible(m_pVisual,mTransform)){
            CKinematics* pV		= fast_dynamic_cast<CKinematics*>(m_pVisual); VERIFY(pV);
            if (fis_zero(m_fLOD,EPS)&&pV->m_lod){
		        if (_IsValidShader(pV->m_lod,priority,strictB2F)){
	                RCache.set_Shader		(pV->m_lod->shader?pV->m_lod->shader:EDevice.m_WireShader);
    	            RCache.set_xform_world	(mTransform);
        	        pV->m_lod->Render		(1.f);
                }
            }else{
                I = pV->children.begin		();
                E = pV->children.end		();
                for (; I!=E; I++){
                    if (_IsValidShader(*I,priority,strictB2F)){
                        RCache.set_Shader		((*I)->shader?(*I)->shader:EDevice.m_WireShader);
                        RCache.set_xform_world	(mTransform);
                        (*I)->Render		 	(m_fLOD);
                    }
                }
            }
        }
    }break;
    case MT_HIERRARHY:{
        if (_IsBoxVisible(m_pVisual,mTransform)){
            FHierrarhyVisual* pV		= fast_dynamic_cast<FHierrarhyVisual*>(m_pVisual); VERIFY(pV);
            I = pV->children.begin		();
            E = pV->children.end		();
            for (; I!=E; I++){
		        if (_IsValidShader(*I,priority,strictB2F)){
	                RCache.set_Shader		((*I)->shader?(*I)->shader:EDevice.m_WireShader);
    	            RCache.set_xform_world	(mTransform);
        	        (*I)->Render		 	(m_fLOD);
                }
            }
        }
    }break;
    case MT_PARTICLE_GROUP:{
        PS::CParticleGroup* pG			= fast_dynamic_cast<PS::CParticleGroup*>(m_pVisual); VERIFY(pG);
//		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            RCache.set_xform_world	  		(mTransform);
            for (PS::CParticleGroup::SItemVecIt i_it=pG->items.begin(); i_it!=pG->items.end(); i_it++){
                xr_vector<dxRender_Visual*>	visuals;
                i_it->GetVisuals			(visuals);
                for (xr_vector<dxRender_Visual*>::iterator it=visuals.begin(); it!=visuals.end(); it++)
                    Render					(*it,Fidentity,priority,strictB2F,m_fLOD);
            }
        }
    }break;
    case MT_PARTICLE_EFFECT:{
//		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            if (_IsValidShader(m_pVisual,priority,strictB2F)){
                RCache.set_Shader			(m_pVisual->shader?m_pVisual->shader:EDevice.m_WireShader);
                RCache.set_xform_world		(mTransform);
                m_pVisual->Render		 	(m_fLOD);
            }
        }
    }break;
    default:
        if (_IsBoxVisible(m_pVisual,mTransform)){
            if (_IsValidShader(m_pVisual,priority,strictB2F)){
                RCache.set_Shader			(m_pVisual->shader?m_pVisual->shader:EDevice.m_WireShader);
                RCache.set_xform_world		(mTransform);
                m_pVisual->Render		 	(m_fLOD);
            }
        }
        break;
    }
}

void 	CModelPool::RenderSingle(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD)
{
	for (int p=0; p<4; p++){
    	Render(m_pVisual,mTransform,p,false,m_fLOD);
    	Render(m_pVisual,mTransform,p,true,m_fLOD);
    }
}
void CModelPool::OnDeviceDestroy()
{
	Destroy();
}
#endif
