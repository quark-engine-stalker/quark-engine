////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrServer_Objects_ALife.h"
#include "profiler.h"
#include <array>

#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#pragma warning(pop)

class CALifeObjectRegistry
{
public:
	typedef xr_map<ALife::_OBJECT_ID, CSE_ALifeDynamicObject*> OBJECT_REGISTRY;
	ALife::_OBJECT_ID max_id = ALife::_OBJECT_ID(-1);

protected:
	OBJECT_REGISTRY m_objects;
	std::array<CSE_ALifeDynamicObject*, 65536> m_object_by_id{};

private:
	void save(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count);

public:
	static CSE_ALifeDynamicObject* get_object(IReader& file_stream, bool* corrupted = nullptr);

public:
	CALifeObjectRegistry(LPCSTR section);
	virtual ~CALifeObjectRegistry();
	virtual void save(IWriter& memory_stream);
	bool load(IReader& file_stream);
	IC void add(CSE_ALifeDynamicObject* object);
	IC void remove(const ALife::_OBJECT_ID& id, bool no_assert = false);
	IC CSE_ALifeDynamicObject* object(const ALife::_OBJECT_ID& id, bool no_assert = false) const;
	IC const OBJECT_REGISTRY& objects() const;
	IC OBJECT_REGISTRY& objects();
};

#include "alife_object_registry_inline.h"
