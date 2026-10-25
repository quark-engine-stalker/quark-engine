#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "../xrCore/ScopeLock.hpp"
#include "../xrCore/job_system.h"

CSoundRender_Source* CSoundRender_Core::i_create_source(LPCSTR name)
{
	// Search
	string256 id;
	xr_strcpy(id, name);
	strlwr(id);
	if (strext(id)) *strext(id) = 0;
	auto it = s_sources.find(id);
	if (it != s_sources.end())
	{
		return it->second;
	}

	// Load a _new one
	CSoundRender_Source* S = xr_new<CSoundRender_Source>();
	S->load(id);
	s_sources.insert({id, S});
	return S;
}

void CSoundRender_Core::i_destroy_source(CSoundRender_Source* S)
{
	// No actual destroy at all
}

namespace
{
constexpr u32 source_cache_magic = 0x48534351; // QCSH
constexpr u32 source_cache_version = 1;

// The ogg a cached header was read from: archive entries differ by vfs/crc/ptr, loose files by size/modif
struct source_cache_entry
{
	u32 vfs, crc, ptr, size, modif;
	u32 silent; // rejected by LoadWave, header is the one of $no_sound
	CSoundRender_Source::header header;

	bool same_file(const source_cache_entry& other) const
	{
		return vfs == other.vfs && crc == other.crc && ptr == other.ptr && size == other.size && modif == other.modif;
	}
};

using source_cache = xr_unordered_map<xr_string, source_cache_entry>;

void build_source_cache_path(string_path& result)
{
	FS.update_path(result, "$app_data_root$", "cache\\sound_sources.bin");
}

void load_source_cache(source_cache& cache)
{
	string_path path;
	build_source_cache_path(path);
	IReader* reader = FS.r_open(path);
	if (!reader)
		return;

	if (reader->length() >= static_cast<int>(sizeof(u32) * 3) && reader->r_u32() == source_cache_magic &&
		reader->r_u32() == source_cache_version)
	{
		const u32 count = reader->r_u32();
		cache.reserve(count);
		for (u32 i = 0; i < count && reader->elapsed() > static_cast<int>(sizeof(source_cache_entry)); ++i)
		{
			const char* name = static_cast<const char*>(reader->pointer());
			const size_t length = strnlen(name, static_cast<size_t>(reader->elapsed() - sizeof(source_cache_entry)));
			if (length + 1 + sizeof(source_cache_entry) > static_cast<size_t>(reader->elapsed()))
				break; // truncated file
			reader->advance(static_cast<int>(length + 1));
			source_cache_entry entry;
			reader->r(&entry, static_cast<int>(sizeof(entry)));
			cache.emplace(xr_string(name, length), entry);
		}
	}
	FS.r_close(reader);
}

void save_source_cache(const xr_vector<const FS_File*>& files, const xr_vector<source_cache_entry>& entries)
{
	string_path path;
	build_source_cache_path(path);
	VerifyPath(path);
	IWriter* writer = FS.w_open(path);
	if (!writer)
		return;

	u32 count = 0;
	for (const source_cache_entry& entry : entries)
		count += entry.size ? 1 : 0;

	writer->w_u32(source_cache_magic);
	writer->w_u32(source_cache_version);
	writer->w_u32(count);
	for (size_t i = 0; i < files.size(); ++i)
	{
		if (!entries[i].size)
			continue;
		string256 id;
		xr_strcpy(id, files[i]->name.c_str());
		if (strext(id))
			*strext(id) = 0;
		writer->w_stringZ(id);
		writer->w(&entries[i], static_cast<u32>(sizeof(source_cache_entry)));
	}
	FS.w_close(writer);
}
}

void CSoundRender_Core::i_create_all_sources(const u64 minimum_available_mb)
{
	PROF_EVENT();
	CTimer T;
	T.Start();

	FS_FileSet flist;
	FS.file_list(flist, "$game_sounds$", FS_ListFiles, "*.ogg");
	const size_t sizeBefore = s_sources.size();
	// This path runs at a loading boundary. Forced prewarm reserves the final table;
	// adaptive prewarm keeps the initial reservation bounded because it may stop
	// early when the physical-RAM safety margin is reached.
	constexpr size_t adaptive_source_reserve_limit = 8192;
	const size_t source_count = static_cast<size_t>(flist.size());
	const size_t source_reserve =
		(minimum_available_mb && source_count > adaptive_source_reserve_limit) ? adaptive_source_reserve_limit : source_count;
	s_sources.reserve(s_sources.size() + source_reserve);

	xr_vector<const FS_File*> files;
	files.reserve(flist.size());
	for (const FS_File& file : flist)
		files.push_back(&file);

	// Opening every ogg (through usvfs under MO2) is what makes the prewarm slow; headers of unchanged files come
	// from the cache. entries[i].size == 0 marks a file not to cache (missing or level override).
	source_cache cache;
	load_source_cache(cache);
	xr_vector<source_cache_entry> entries(files.size(), source_cache_entry{});

	Lock lock;
	struct source_load_context
	{
		CSoundRender_Core* owner;
		const xr_vector<const FS_File*>* files;
		const source_cache* cache;
		xr_vector<source_cache_entry>* entries;
		Lock* lock;
		u64 minimum_available_mb;
		__declspec(align(64)) volatile LONG next_file;
		volatile LONG stop_requested;
		volatile LONG cache_hits;
	};

	source_load_context context = {this, &files, &cache, &entries, &lock, minimum_available_mb, 0, 0, 0};
	const auto process_batch = [](void* raw_context)
	{
		source_load_context& context = *static_cast<source_load_context*>(raw_context);
		for (;;)
		{
			if (_InterlockedCompareExchange(&context.stop_requested, FALSE, FALSE) != FALSE)
				return;

			const LONG file_index = InterlockedIncrement(&context.next_file) - 1;
			if (file_index < 0 || static_cast<size_t>(file_index) >= context.files->size())
				return;

			// Adaptive prewarm must always leave a large physical-RAM safety margin.
			// Check only every 64 completed sources so the query itself is negligible.
			if (context.minimum_available_mb && (file_index & 63) == 0)
			{
				MEMORYSTATUSEX status = {};
				status.dwLength = sizeof(status);
				if (GlobalMemoryStatusEx(&status) &&
					status.ullAvailPhys / (1024ull * 1024ull) < context.minimum_available_mb)
				{
					_InterlockedExchange(&context.stop_requested, TRUE);
					return;
				}
			}

			const FS_File& file = *(*context.files)[static_cast<size_t>(file_index)];
			string256 id;
			xr_strcpy(id, file.name.c_str());

			xr_strlwr(id);
			if (strext(id))
				*strext(id) = 0;

			{
				ScopeLock scope(context.lock);
				const auto it = context.owner->s_sources.find(id);
				if (it != context.owner->s_sources.end())
					continue;
				UNUSED(scope);
			}

			CSoundRender_Source* S = new CSoundRender_Source();
			// Resolve like CSoundRender_Source::load: it strips the extension once more ("x.ogg.ogg" loads "x.ogg"),
			// a $level$ copy wins, otherwise $game_sounds$
			string256 name;
			xr_strcpy(name, id);
			if (strext(name))
				*strext(name) = 0;
			string_path path, silence;
			const CLocatorAPI::file* desc =
				FS.exist(path, "$level$", name, ".ogg") ? nullptr : FS.exist(path, "$game_sounds$", name, ".ogg");
			FS.update_path(silence, "$game_sounds$", "$no_sound.ogg");
			source_cache_entry& entry = (*context.entries)[static_cast<size_t>(file_index)];
			if (desc)
				entry = {desc->vfs, desc->crc, desc->ptr, desc->size_real, desc->modif};

			const auto cached = desc ? context.cache->find(id) : context.cache->end();
			if (cached != context.cache->end() && cached->second.same_file(entry))
			{
				entry = cached->second;
				if (entry.silent)
					Msg("! Warning: Invalid source rate (need 44100), playing silence: %s", path);
				S->load(name, entry.silent ? silence : path, entry.header);
				InterlockedIncrement(&context.cache_hits);
			}
			else
			{
				S->load(id);
				// LoadWave replaces a file with a rate other than 44100 by $no_sound; parsing such a file
				// (often a big 48/96 kHz track) is the slowest part, so that outcome is cached as well
				entry.silent = !xr_strcmp(S->pname.c_str(), silence);
				if (desc && (entry.silent || !xr_strcmp(S->pname.c_str(), path)))
					entry.header = S->get_header();
				else
					entry.size = 0; // not resolved here
			}

			context.lock->Enter();
			context.owner->s_sources.insert({id, S});
			context.lock->Leave();
		}
	};

	// Source parsing is coarse enough to scale, but spawning a separate TBB arena
	// competes with the engine pool during loading. Reuse the warm pool and keep at
	// least 32 files behind every lane so small sound packs remain serial.
	constexpr u32 min_files_per_lane = 32;
	const u32 physical_cores = _max(1u, CPU::ID.n_cores);
	const u32 lane_capacity = _min(physical_cores, xr_jobs::available_thread_count());
	const u32 useful_lanes = static_cast<u32>((files.size() + min_files_per_lane - 1) /
		min_files_per_lane);
	const u32 lane_count = _max(1u, _min(lane_capacity, useful_lanes));

	xr_jobs::task_group group;
	xr_jobs::submit_many(process_batch, &context, lane_count - 1,
		&group, xr_jobs::priority::normal);
	process_batch(&context);
	if (lane_count > 1)
		xr_jobs::wait(group);

	const bool stopped_for_memory = _InterlockedCompareExchange(&context.stop_requested, FALSE, FALSE) != FALSE;
	const u32 cache_hits = static_cast<u32>(context.cache_hits);
	u32 cacheable = 0;
	for (const source_cache_entry& entry : entries)
		cacheable += entry.size ? 1 : 0;
	// A stopped prewarm did not visit every file; keep the old cache instead of dropping their entries
	if (!stopped_for_memory && (cache_hits != cacheable || cache.size() != cacheable))
		save_source_cache(files, entries);

	Msg("Finished creating %d sound sources (%u headers from cache). Duration: %d ms%s", s_sources.size() - sizeBefore,
		cache_hits, T.GetElapsed_ms(), stopped_for_memory ? " (adaptive RAM reserve reached)" : "");
}
