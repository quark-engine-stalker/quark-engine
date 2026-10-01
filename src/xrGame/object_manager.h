////////////////////////////////////////////////////////////////////////////
//	Module 		: object_manager.h
//	Created 	: 30.12.2003
//  Modified 	: 30.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Object manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <functional>

template <typename T>
class CObjectManager
{
public:
	typedef xr_vector<T*> OBJECTS;
	typedef xr_unordered_flat_map<T*, u32> OBJECT_LOOKUP;
protected:
	OBJECTS m_objects;
	OBJECT_LOOKUP m_object_lookup;
	u32 m_lookup_generation;
	T* m_selected;

protected:
	IC bool contains(T* object) const;
	bool remove_object(T* object);
	void rebuild_object_lookup();

public:
	CObjectManager();
	virtual ~CObjectManager();
	virtual void Load(LPCSTR section);
	virtual void reinit();
	virtual void reload(LPCSTR section);
	virtual void update();
	bool add(T* object);
	virtual bool is_useful(T* object) const;
	virtual float do_evaluate(T* object) const;
	virtual void reset();

public:
	IC T* selected() const;
	IC const OBJECTS& objects() const;
};

#include "object_manager_inline.h"
