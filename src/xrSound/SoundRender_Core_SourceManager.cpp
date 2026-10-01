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

	Lock lock;
	struct source_load_context
	{
		CSoundRender_Core* owner;
		const xr_vector<const FS_File*>* files;
		Lock* lock;
		u64 minimum_available_mb;
		__declspec(align(64)) volatile LONG next_file;
		volatile LONG stop_requested;
	};

	source_load_context context = {this, &files, &lock, minimum_available_mb, 0, 0};
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
			S->load(id);

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
	Msg("Finished creating %d sound sources. Duration: %d ms%s", s_sources.size() - sizeBefore, T.GetElapsed_ms(),
		stopped_for_memory ? " (adaptive RAM reserve reached)" : "");
}
