////////////////////////////////////////////////////////////////////////////
//	Module 		: xrServer_Factory.cpp
//	Created 	: 19.09.2002
//  Modified 	: 04.06.2003
//	Author		: Oles Shyshkovtsov, Alexander Maksimchuk, Victor Reutskiy and Dmitriy Iassenev
//	Description : Server objects factory
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "object_factory.h"

CSE_Abstract* F_entity_Create(LPCSTR section)
{
	// Save loading creates many entities from the same sections. Cache the
	// immutable section-to-class mapping instead of repeating INI lookups and
	// TEXT2CLSID conversion for every object.
	using class_cache_type = xr_unordered_map<shared_str, CLASS_ID>;
	static xrSRWLock cache_lock;
	static class_cache_type* class_cache = []
	{
		class_cache_type* cache = xr_new<class_cache_type>();
		cache->reserve(2048);
		return cache;
	}();

	const shared_str key = section;
	CLASS_ID class_id = CLASS_ID(0);
	bool resolved = false;
	{
		xrSRWLockGuard guard(cache_lock, true);
		const auto found = class_cache->find(key);
		if (found != class_cache->end())
		{
			class_id = found->second;
			resolved = true;
		}
	}

	if (!resolved)
	{
		// Resolve misses under the exclusive lock. This keeps pSettings access
		// and cache publication single-threaded, while object construction itself
		// remains outside the cache lock.
		xrSRWLockGuard guard(cache_lock);
		const auto found = class_cache->find(key);
		if (found != class_cache->end())
		{
			class_id = found->second;
		}
		else
		{
			if (!pSettings->section_exist(section))
				return nullptr;

			class_id = pSettings->r_clsid(section, "class");
			class_cache->emplace(key, class_id);
		}
	}

	return object_factory().server_object(class_id, section);
}
