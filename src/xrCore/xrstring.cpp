#include "stdafx.h"
#pragma hdrstop

#include "xrstring.h"

#include "FS_impl.h"

#include <new>

XRCORE_API str_container* g_pStringContainer = nullptr;

namespace
{
constexpr size_t string_header_size = offsetof(str_value, value);
static_assert(sizeof(xr_atomic_u32) == sizeof(u32), "shared_str reference counter must remain 32-bit");
}

struct str_container_impl
{
	static constexpr u32 buffer_size = 1024 * 256;
	static constexpr u32 lock_count = 64;
	static_assert((buffer_size & (buffer_size - 1)) == 0, "buffer_size must be a power of two");
	static_assert((lock_count & (lock_count - 1)) == 0, "lock_count must be a power of two");

	struct lock_shard
	{
		xrSRWLock lock;
		u8 padding[64 - sizeof(xrSRWLock)];
	};
	static_assert(sizeof(lock_shard) == 64, "shared_str lock shard must occupy one cache line");

	str_value* buffer[buffer_size];
	u8 lock_storage[sizeof(lock_shard) * lock_count + 63];
	lock_shard* locks;

	str_container_impl()
	{
		ZeroMemory(buffer, sizeof(buffer));
		const size_t raw_address = reinterpret_cast<size_t>(lock_storage);
		const size_t aligned_address = (raw_address + 63u) & ~size_t(63u);
		locks = reinterpret_cast<lock_shard*>(aligned_address);
		for (u32 i = 0; i < lock_count; ++i)
			new (&locks[i]) lock_shard();
		VERIFY((reinterpret_cast<size_t>(locks) & 63u) == 0);
	}

	~str_container_impl()
	{
		for (u32 i = 0; i < lock_count; ++i)
			locks[i].~lock_shard();
	}

	static u32 bucket_index(u32 crc)
	{
		return crc & (buffer_size - 1);
	}

	static u32 lock_index(u32 crc)
	{
		return bucket_index(crc) & (lock_count - 1);
	}

	xrSRWLock& lock_for(u32 crc)
	{
		return locks[lock_index(crc)].lock;
	}

	void lock_all()
	{
		for (u32 i = 0; i < lock_count; ++i)
			locks[i].lock.AcquireExclusive();
	}

	void unlock_all()
	{
		for (u32 i = lock_count; i > 0; --i)
			locks[i - 1].lock.ReleaseExclusive();
	}

	str_value* find(u32 crc, u32 length, const char* str) const
	{
		str_value* candidate = buffer[bucket_index(crc)];
		while (candidate)
		{
			if (candidate->dwCRC == crc && candidate->dwLength == length &&
				!memcmp(candidate->value, str, length))
			{
				return candidate;
			}

			candidate = candidate->next;
		}

		return nullptr;
	}

	void insert(str_value* value)
	{
		str_value** element = &buffer[bucket_index(value->dwCRC)];
		value->next = *element;
		*element = value;
	}

	void clean()
	{
		for (u32 i = 0; i < buffer_size; ++i)
		{
			str_value** current = &buffer[i];
			while (*current)
			{
				str_value* value = *current;
				if (value->dwReference.load(std::memory_order_acquire) == 0)
				{
					*current = value->next;
					xr_free(value);
				}
				else
				{
					current = &value->next;
				}
			}
		}
	}

	void verify() const
	{
		Msg("strings verify started");
		for (u32 i = 0; i < buffer_size; ++i)
		{
			str_value* value = buffer[i];
			while (value)
			{
				u32 crc = crc32(value->value, value->dwLength);
				string32 crc_str;
				R_ASSERT3(crc == value->dwCRC, "CorePanic: read-only memory corruption (shared_strings)",
					itoa(value->dwCRC, crc_str, 16));
				R_ASSERT3(value->dwLength == xr_strlen(value->value),
					"CorePanic: read-only memory corruption (shared_strings, internal structures)", value->value);
				value = value->next;
			}
		}
		Msg("strings verify completed");
	}

	void dump(FILE* file) const
	{
		for (u32 i = 0; i < buffer_size; ++i)
		{
			str_value* value = buffer[i];
			while (value)
			{
				fprintf(file, "ref[%4u]-len[%3u]-crc[%8X] : %s\n",
					value->dwReference.load(std::memory_order_relaxed), value->dwLength, value->dwCRC, value->value);
				value = value->next;
			}
		}
	}

	void dump(IWriter* writer) const
	{
		for (u32 i = 0; i < buffer_size; ++i)
		{
			str_value* value = buffer[i];
			string4096 temp;
			while (value)
			{
				xr_sprintf(temp, sizeof(temp), "ref[%4u]-len[%3u]-crc[%8X] : %s\n",
					value->dwReference.load(std::memory_order_relaxed), value->dwLength, value->dwCRC, value->value);
				writer->w_string(temp);
				value = value->next;
			}
		}
	}

	s64 stat_economy(u32& count) const
	{
		s64 counter = 0;
		for (u32 i = 0; i < buffer_size; ++i)
		{
			str_value* value = buffer[i];
			while (value)
			{
				const u32 references = value->dwReference.load(std::memory_order_relaxed);
				++count;
				counter -= static_cast<s64>(string_header_size);
				if (references > 1)
					counter += static_cast<s64>(references - 1) * static_cast<s64>(value->dwLength + 1);
				value = value->next;
			}
		}
		return counter;
	}
};

namespace
{
class all_string_shards_guard : xray::noncopyable
{
public:
	explicit all_string_shards_guard(str_container_impl& container) : container_(container)
	{
		container_.lock_all();
	}

	~all_string_shards_guard()
	{
		container_.unlock_all();
	}

private:
	str_container_impl& container_;
};
}

str_container::str_container()
{
	impl = xr_new<str_container_impl>();
}

str_value* str_container::dock(str_c value)
{
	if (!value)
		return nullptr;

#ifdef DEBUG_MEMORY_MANAGER
	Memory.stat_strdock++;
#endif

	const u32 length = static_cast<u32>(xr_strlen(value));
	const u32 length_with_zero = length + 1;
	VERIFY(string_header_size + length_with_zero < 4096);
	const u32 crc = crc32(value, length);

	xrSRWLockGuard guard(impl->lock_for(crc));
	str_value* result = impl->find(crc, length, value);

#ifdef DEBUG
	const bool is_leaked_string = !xr_strcmp(value, "enter leaked string here");
#else
	constexpr bool is_leaked_string = false;
#endif

	if (!result || is_leaked_string)
	{
		result = static_cast<str_value*>(Memory.mem_alloc_uninitialized(string_header_size + length_with_zero
#ifdef DEBUG_MEMORY_NAME
			, "storage: sstring"
#endif
		));
		R_ASSERT2(result, "shared_str allocation failed");

#ifdef DEBUG
		if (is_leaked_string)
		{
			static int leaked_string_count = 0;
			Msg("leaked_string: %d 0x%p", ++leaked_string_count, result);
		}
#endif

		new (&result->dwReference) xr_atomic_u32(1);
		result->dwLength = length;
		result->dwCRC = crc;
		CopyMemory(result->value, value, length_with_zero);
		impl->insert(result);
	}
	else
	{
		result->dwReference.fetch_add(1, std::memory_order_relaxed);
	}

	return result;
}

void str_container::clean()
{
	all_string_shards_guard guard(*impl);
	impl->clean();
}

void str_container::verify()
{
	all_string_shards_guard guard(*impl);
	impl->verify();
}

void str_container::dump()
{
	all_string_shards_guard guard(*impl);
	FILE* file = fopen("d:\\$str_dump$.txt", "w");
	if (!file)
		return;
	impl->dump(file);
	fclose(file);
}

void str_container::dump(IWriter* writer)
{
	if (!writer)
		return;
	all_string_shards_guard guard(*impl);
	impl->dump(writer);
}

u32 str_container::stat_economy(u32& count)
{
	all_string_shards_guard guard(*impl);
	s64 counter = -static_cast<s64>(sizeof(*this));
	counter += impl->stat_economy(count);
	return static_cast<u32>(counter);
}

str_container::~str_container()
{
	clean();
	xr_delete(impl);
}
