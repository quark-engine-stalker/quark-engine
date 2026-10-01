////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry_inline.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC void CALifeObjectRegistry::add(CSE_ALifeDynamicObject* object)
{
	auto position = m_objects.lower_bound(object->ID);
	if (position != m_objects.end() && position->first == object->ID)
	{
		THROW2(position->second == object,
		       "The specified object is already presented in the Object Registry!");
		THROW2(position->second != object,
		       "Object with the specified ID is already presented in the Object Registry!");
	}

	m_objects.insert(position, std::make_pair(object->ID, object));
	m_object_by_id[object->ID] = object;
}

IC void CALifeObjectRegistry::remove(const ALife::_OBJECT_ID& id, bool no_assert)
{
	OBJECT_REGISTRY::iterator I = m_objects.find(id);
	if (I == m_objects.end())
	{
		THROW2(no_assert, "The specified object hasn't been found in the Object Registry!");
		return;
	}

	m_object_by_id[id] = nullptr;
	m_objects.erase(I);
}

IC CSE_ALifeDynamicObject* CALifeObjectRegistry::object(const ALife::_OBJECT_ID& id, bool no_assert) const
{
	START_PROFILE("ALife/objects::object")
		CSE_ALifeDynamicObject* const result = m_object_by_id[id];

		if (!result)
		{
#ifdef DEBUG
			if (!no_assert)
				Msg("There is no object with id %d!", id);
#endif
			THROW2(no_assert, "Specified object hasn't been found in the object registry!");
			return (0);
		}

		return result;
	STOP_PROFILE
}

IC const CALifeObjectRegistry::OBJECT_REGISTRY& CALifeObjectRegistry::objects() const
{
	return (m_objects);
}

IC CALifeObjectRegistry::OBJECT_REGISTRY& CALifeObjectRegistry::objects()
{
	return (m_objects);
}
