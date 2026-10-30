////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_data_storage.cpp
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation data storage
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stalker_animation_data_storage.h"
#include "stalker_animation_data.h"
#include "object_broker.h"
#include "../Include/xrRender/Kinematics.h"

CStalkerAnimationDataStorage* g_stalker_animation_data_storage = 0;

class data_predicate
{
private:
	IKinematicsAnimated* m_object;

public:
	IC data_predicate(IKinematicsAnimated* skeleton_animated)
	{
		VERIFY(skeleton_animated);
		m_object = skeleton_animated;
	}

	IC bool operator()(const CStalkerAnimationDataStorage::OBJECT& object) const
	{
		if (m_object->LL_MotionsSlotCount() != object.first.size())
			return (false);

		for (u16 i = 0, n = m_object->LL_MotionsSlotCount(); i < n; ++i)
			if (!(m_object->LL_MotionsSlot(i) == object.first[i]))
				return (false);

		return (true);
	}
};

CStalkerAnimationDataStorage::~CStalkerAnimationDataStorage()
{
	clear();
}

void CStalkerAnimationDataStorage::clear()
{
	while (!m_objects.empty())
	{
		xr_delete(m_objects.back().second);
		m_objects.pop_back();
	}
}

const CStalkerAnimationData* CStalkerAnimationDataStorage::object(IKinematicsAnimated* skeleton_animated)
{
	OBJECTS::const_iterator I = std::find_if(m_objects.begin(), m_objects.end(), data_predicate(skeleton_animated));
	if (I != m_objects.end())
		return ((*I).second);

	// The copies hold references, so the motions cannot be freed and reused at the same address
	MOTIONS motions;
	for (u16 i = 0, n = skeleton_animated->LL_MotionsSlotCount(); i < n; ++i)
		motions.push_back(skeleton_animated->LL_MotionsSlot(i));
	m_objects.push_back(std::make_pair(motions, xr_new<CStalkerAnimationData>(skeleton_animated)));
	return (m_objects.back().second);
}
