#include "stdafx.h"
#pragma hdrstop



#include "SoundRender_Cache.h"

CSoundRender_Cache::CSoundRender_Cache()
{
	data = NULL;
	c_storage = NULL;
	c_begin = NULL;
	c_end = NULL;
	_total = 0;
	_line = 0;
	_count = 0;
}

CSoundRender_Cache::~CSoundRender_Cache()
{
}


void CSoundRender_Cache::move2top(cache_line* line)
{
	VERIFY(line);
	if (line == c_begin) return; // already at top

	// track end
	if (line == c_end) c_end = c_end->prev;

	// cut 
	cache_line* prev = line->prev;
	cache_line* next = line->next;
	if (prev) prev->next = next;
	if (next) next->prev = prev;

	// register at top
	line->prev = NULL;
	line->next = c_begin;

	// track begin
	c_begin->prev = line;
	c_begin = line;

	// internal verify
	VERIFY(c_begin->prev == NULL);
	VERIFY(c_end->next == NULL);
}

BOOL CSoundRender_Cache::request(cache_cat& cat, u32 id)
{
	// 1. check if cached version available
	id %= cat.size;
	//.	R_ASSERT		(id<cat.size);
	u16& cptr = cat.table[id];
	if (CAT_FREE != cptr)
	{
		// cache line exists - change it's priority and return
		_stat_hit ++;
		cache_line* L = c_storage + cptr;
		move2top(L);
		return FALSE;
	}

	// 2. purge oldest item + move it to top
	_stat_miss ++;
	move2top(c_end);
	if (c_begin->loopback)
	{
		*c_begin->loopback = CAT_FREE;
		c_begin->loopback = NULL;
	}

	commit_line(c_begin);
	// 3. associate
	cptr = c_begin->id;
	c_begin->loopback = &cptr;

	// 4. fill with data
	return TRUE;
}

void CSoundRender_Cache::commit_line(cache_line* line)
{
    if (!virtual_storage)
        return;
    const size_t begin = (static_cast<size_t>(line->id) * _line / page_size) * page_size;
    if (begin >= committed_begin)
        return;
    // Unused LRU slots are consumed from the end. Reused lines remain inside
    // this committed suffix, so subsequent misses need no system call.
    void* committed = VirtualAlloc(data + begin, committed_begin - begin, MEM_COMMIT, PAGE_READWRITE);
    R_ASSERT2(committed, "Sound cache page commit failed");
    committed_begin = begin;
}

void CSoundRender_Cache::initialize(u32 total_kb, u32 bytes_per_line)
{
    R_ASSERT(bytes_per_line);
    _line = bytes_per_line;
    // Preserve the existing console/config capacity multiplier. u16 CAT entries
    // cannot represent more slots, including on high-RAM automatic presets.
    const u64 requested = static_cast<u64>(total_kb) * 2048ull / bytes_per_line + 1;
    _count = static_cast<u32>(_min(requested, static_cast<u64>(CAT_FREE - 1)));
    R_ASSERT(static_cast<u64>(_count) * _line <= 0xffffffffull);
    _total = _count * _line;
    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    // Commit at least 64 KiB per cold miss to amortize the OS call, while
    // unused PCM capacity remains only an address-space reservation.
    page_size = _max(static_cast<size_t>(info.dwPageSize), size_t(64 * 1024));
    reserved_bytes = (static_cast<size_t>(_total) + page_size - 1) / page_size * page_size;
    data = static_cast<u8*>(VirtualAlloc(nullptr, reserved_bytes, MEM_RESERVE, PAGE_READWRITE));
    virtual_storage = data != nullptr;
    if (!virtual_storage)
        data = xr_alloc<u8>(_total);
    committed_begin = virtual_storage ? reserved_bytes : 0;
    c_storage = xr_alloc<cache_line>(_count);
    Msg("* sound : cache capacity: %d kb, %d lines, %d bpl, lazy=%u", _total / 1024, _count, _line, virtual_storage);
    format();
}

void CSoundRender_Cache::disconnect()
{
	// disconnect from CATs
	for (u32 it = 0; it < _count; it++)
	{
		cache_line* L = c_storage + it;
		if (L->loopback)
		{
			*L->loopback = CAT_FREE;
			L->loopback = NULL;
		}
	}
}

void CSoundRender_Cache::format()
{
	// format structs
	for (u32 it = 0; it < _count; it++)
	{
		cache_line* L = c_storage + it;
		L->prev = (0 == it) ? NULL : c_storage + it - 1;
		L->next = ((_count - 1) == it) ? NULL : c_storage + it + 1;
		L->data = data + it * _line;
		L->loopback = NULL;
		L->id = u16(it);
	}

	// start-end
	c_begin = c_storage + 0;
	c_end = c_storage + _count - 1;
}

void CSoundRender_Cache::purge()
{
	disconnect(); // disconnect from CATs
	format(); // format
    if (virtual_storage)
    {
        const BOOL decommitted = VirtualFree(data, reserved_bytes, MEM_DECOMMIT);
        R_ASSERT2(decommitted, "Sound cache page decommit failed");
        committed_begin = reserved_bytes;
    }
}

void CSoundRender_Cache::destroy()
{
	disconnect();
	if (virtual_storage)
    {
        const BOOL released = VirtualFree(data, 0, MEM_RELEASE);
        R_ASSERT2(released, "Sound cache reservation release failed");
        data = nullptr;
    }
    else
        xr_free(data);
    virtual_storage = false;
    reserved_bytes = committed_begin = page_size = 0;
	xr_free(c_storage);
	c_begin = NULL;
	c_end = NULL;
	_total = 0;
	_line = 0;
	_count = 0;
}

void CSoundRender_Cache::cat_create(cache_cat& cat, u32 bytes)
{
	cat.size = bytes / _line;
	if (bytes % _line) cat.size += 1;
	u32 allocsize = (cat.size & 1) ? cat.size + 1 : cat.size;
	cat.table = xr_alloc<u16>(allocsize);
	Memory.mem_fill32(cat.table, 0xffffffff, allocsize / 2);
}

void CSoundRender_Cache::cat_destroy(cache_cat& cat)
{
	xr_free(cat.table);
	cat.size = 0;
}
