#include "stdafx.h"
#pragma hdrstop

#include <new>

using namespace std;

namespace
{
constexpr size_t shared_memory_header_size = offsetof(smem_value, value);
static_assert(sizeof(xr_atomic_u32) == sizeof(u32), "shared memory reference counter must remain 32-bit");
}

XRCORE_API smem_container* g_pSharedMemoryContainer = NULL;

smem_value* smem_container::dock(u32 dwCRC, u32 dwLength, void* ptr)
{
	VERIFY(dwCRC && dwLength && ptr);

	cs.Enter();
	smem_value* result = 0;

	// search a place to insert
	struct search_key
	{
		u32 crc;
		u32 length;
	};

	const search_key key{dwCRC, dwLength};
	cdb::iterator it = std::lower_bound(container.begin(), container.end(), key,
		[](const smem_value* value, const search_key& search)
		{
			if (value->dwCRC != search.crc)
				return value->dwCRC < search.crc;
			return value->dwLength < search.length;
		});
	cdb::iterator saved_place = it;
	if (container.end() != it)
	{
		// supposedly found
		for (;; it++)
		{
			if (it == container.end()) break;
			if ((*it)->dwCRC != dwCRC) break;
			if ((*it)->dwLength != dwLength) break;
			if (0 == memcmp((*it)->value, ptr, dwLength))
			{
				// really found
				result = *it;
				break;
			}
		}
	}

	// if not found - create new entry
	if (0 == result)
	{
		result = (smem_value*)Memory.mem_alloc_uninitialized(shared_memory_header_size + dwLength
#ifdef DEBUG_MEMORY_NAME
                                               , "storage: smem"
#endif // DEBUG_MEMORY_NAME
		);
		R_ASSERT2(result, "shared memory allocation failed");
		new (&result->dwReference) xr_atomic_u32(1);
		result->dwCRC = dwCRC;
		result->dwLength = dwLength;
		result->_align_16 = 0;
		CopyMemory(result->value, ptr, dwLength);
		container.insert(saved_place, result);
	}
	else
	{
		result->dwReference.fetch_add(1, std::memory_order_relaxed);
	}

	// exit
	cs.Leave();
	return result;
}

void smem_container::clean()
{
	cs.Enter();
	for (cdb::iterator it = container.begin(); it != container.end();)
	{
		if ((*it)->dwReference.load(std::memory_order_acquire) == 0)
		{
			xr_free(*it);
			it = container.erase(it);
		}
		else
		{
			++it;
		}
	}

	// clean() is an explicit compaction boundary. If a loading spike left a
	// much larger pointer array behind, return that high-water capacity too.
	const size_t live_count = static_cast<size_t>(container.size());
	const size_t capacity = container.capacity();
	if (capacity > 256 && capacity > ((live_count > 128 ? live_count : size_t(128)) * 2))
	{
		cdb compact;
		compact.reserve(live_count);
		compact.insert(compact.end(), container.begin(), container.end());
		container.swap(compact);
	}
	cs.Leave();
}

void smem_container::dump()
{
	cs.Enter();
	cdb::iterator it = container.begin();
	cdb::iterator end = container.end();
	FILE* F = fopen("x:\\$smem_dump$.txt", "w");
	for (; it != end; it++)
		fprintf(F, "%4u : crc[%6x], %u bytes\n", (*it)->dwReference.load(std::memory_order_relaxed), (*it)->dwCRC, (*it)->dwLength);
	fclose(F);
	cs.Leave();
}

u32 smem_container::stat_economy()
{
	cs.Enter();
	cdb::iterator it = container.begin();
	cdb::iterator end = container.end();
	s64 counter = 0;
	counter -= sizeof(*this);
	counter -= sizeof(cdb::allocator_type);
	const int node_size = 20;
	for (; it != end; it++)
	{
		counter -= 16;
		counter -= node_size;
		const u32 references = (*it)->dwReference.load(std::memory_order_relaxed);
		if (references > 1)
			counter += s64(references - 1) * s64((*it)->dwLength);
	}
	cs.Leave();

	return u32(s64(counter) / s64(1024));
}

smem_container::~smem_container()
{
	clean();
}
