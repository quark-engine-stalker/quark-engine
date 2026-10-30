////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_data_storage.h
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation data storage
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "../xrEngine/SkeletonMotions.h"

class CStalkerAnimationData;
class IKinematicsAnimated;

class CStalkerAnimationDataStorage
{
public:
	// Keyed by the motion slots, not by the first stalker's skeleton: that visual can go back to
	// the model pool and be freed mid-level (VRAM pressure trim clears the pool).
	typedef xr_vector<shared_motions> MOTIONS;
	typedef std::pair<MOTIONS, CStalkerAnimationData*> OBJECT;
	typedef xr_vector<OBJECT> OBJECTS;

private:
	OBJECTS m_objects;

public:
	virtual ~CStalkerAnimationDataStorage();
	const CStalkerAnimationData* object(IKinematicsAnimated* skeleton_animated);
	void clear();
};

IC CStalkerAnimationDataStorage& stalker_animation_data_storage();

extern CStalkerAnimationDataStorage* g_stalker_animation_data_storage;

#include "stalker_animation_data_storage_inline.h"
