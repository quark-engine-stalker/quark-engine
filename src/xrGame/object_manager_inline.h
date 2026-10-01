////////////////////////////////////////////////////////////////////////////
//	Module 		: object_manager.h
//	Created 	: 30.12.2003
//  Modified 	: 30.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Object manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#define TEMPLATE_SPECIALIZATION template <\
	typename T\
>

#define CAbstractObjectManager CObjectManager<T>

TEMPLATE_SPECIALIZATION
CAbstractObjectManager::CObjectManager()
{
	// These managers are rebuilt frequently from visual/sound/hit memory. Keep
	// a small high-water allocation so the common case performs no allocator work.
	m_objects.reserve(32);
	m_object_lookup.reserve(64);
	m_lookup_generation = 1;
	m_selected = 0;
}

TEMPLATE_SPECIALIZATION
CAbstractObjectManager::~CObjectManager()
{
}

TEMPLATE_SPECIALIZATION
void CAbstractObjectManager::Load(LPCSTR section)
{
}

TEMPLATE_SPECIALIZATION
void CAbstractObjectManager::reinit()
{
	m_objects.clear();
	m_object_lookup.clear();
	m_lookup_generation = 1;
	m_selected = 0;
}

TEMPLATE_SPECIALIZATION
void CAbstractObjectManager::reload(LPCSTR section)
{
}

TEMPLATE_SPECIALIZATION
void CAbstractObjectManager::update()
{
	float result = flt_max;
	m_selected = 0;
	OBJECTS::const_iterator I = m_objects.begin();
	OBJECTS::const_iterator E = m_objects.end();
	for (; I != E; ++I)
	{
		float value = do_evaluate(*I);
		if (result > value)
		{
			result = value;
			m_selected = *I;
		}
	}
}

TEMPLATE_SPECIALIZATION
float CAbstractObjectManager::do_evaluate(T* object) const
{
	return (0.f);
}

TEMPLATE_SPECIALIZATION
bool CAbstractObjectManager::is_useful(T* object) const
{
	const ISpatial* self = (const ISpatial*)(object);
	if (!self)
		return (false);

	if ((object->spatial.type & STYPE_VISIBLEFORAI) != STYPE_VISIBLEFORAI)
		return (false);

	return (true);
}

TEMPLATE_SPECIALIZATION
IC bool CAbstractObjectManager::contains(T* object) const
{
	typename OBJECT_LOOKUP::const_iterator found = m_object_lookup.find(object);
	return found != m_object_lookup.end() && found->second == m_lookup_generation;
}

TEMPLATE_SPECIALIZATION
bool CAbstractObjectManager::remove_object(T* object)
{
	typename OBJECTS::iterator I = std::find(m_objects.begin(), m_objects.end(), object);
	if (I == m_objects.end())
		return (false);

	m_objects.erase(I);

	m_object_lookup.erase(object);

	return (true);
}

TEMPLATE_SPECIALIZATION
void CAbstractObjectManager::rebuild_object_lookup()
{
	if (++m_lookup_generation == 0)
	{
		m_object_lookup.clear();
		m_lookup_generation = 1;
	}
	size_t reserve_count = m_objects.size() * 2 + 32;
	if (reserve_count < m_object_lookup.size())
		reserve_count = m_object_lookup.size();
	m_object_lookup.reserve(reserve_count);
	for (typename OBJECTS::const_iterator I = m_objects.begin(); I != m_objects.end(); ++I)
		m_object_lookup[*I] = m_lookup_generation;
}

TEMPLATE_SPECIALIZATION
bool CAbstractObjectManager::add(T* object)
{
	if (!object)
		return (false);

	// Visual, sound and hit memory can report the same object during one
	// update. Avoid running expensive usefulness/Lua predicates again.
	if (contains(object))
		return (true);

	if (!is_useful(object))
		return (false);

	m_objects.push_back(object);
	m_object_lookup[object] = m_lookup_generation;
	return (true);
}

TEMPLATE_SPECIALIZATION
IC T*CAbstractObjectManager::selected() const
{
	return (m_selected);
}

TEMPLATE_SPECIALIZATION
void CAbstractObjectManager::reset()
{
	m_objects.clear();

	// Keep the hash table and its buckets alive between AI updates. A generation
	// stamp makes old entries invisible without clearing/rebuilding the table on
	// every scheduled update. This trades a small amount of retained RAM for less
	// main-thread allocator/hash maintenance while preserving encounter order.
	if (++m_lookup_generation == 0)
	{
		m_object_lookup.clear();
		m_lookup_generation = 1;
	}

	// Bound stale keys collected over a long level session. Physical compaction is
	// deliberately rare so the hot path remains generation-only.
	if (m_object_lookup.size() > 4096)
	{
		m_object_lookup.clear();
		m_object_lookup.reserve(64);
	}
}

TEMPLATE_SPECIALIZATION
IC const typename CAbstractObjectManager::OBJECTS&CAbstractObjectManager::objects() const
{
	return (m_objects);
}

#undef TEMPLATE_SPECIALIZATION
#undef CAbstractObjectManager
