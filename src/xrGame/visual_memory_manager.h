////////////////////////////////////////////////////////////////////////////
//	Module 		: visual_memory_manager.h
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Visual memory manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "visual_memory_params.h"
#include "memory_space.h"

class CCustomMonster;
class CAI_Stalker;
class CAgentMemoryManager;
class vision_client;

namespace luabind
{
	template <class T> class functor;
}

class CVisualMemoryManager
{
#ifdef DEBUG
	friend class CAI_Stalker;
#endif
public:
	typedef MemorySpace::CVisibleObject CVisibleObject;
	typedef MemorySpace::CNotYetVisibleObject CNotYetVisibleObject;
	typedef xr_vector<CVisibleObject> VISIBLES;
	typedef xr_vector<CObject*> RAW_VISIBLES;
	typedef xr_vector<CNotYetVisibleObject> NOT_YET_VISIBLES;

private:
	struct CDelayedVisibleObject
	{
		ALife::_OBJECT_ID m_object_id;
		CVisibleObject m_visible_object;
	};

private:
	typedef xr_vector<CDelayedVisibleObject> DELAYED_VISIBLE_OBJECTS;

private:
	CCustomMonster* m_object;
	CAI_Stalker* m_stalker;
	vision_client* m_client;

private:
	mutable RAW_VISIBLES m_visible_objects;
	VISIBLES* m_objects;
	// Non-owning pointer to the index owned by this group's agent manager.
	// Group registration detaches it before that manager is destroyed.
	CAgentMemoryManager* m_squad_merge_index;
	NOT_YET_VISIBLES m_not_yet_visible_objects;

	struct SVisionObserverContext
	{
		Fvector eye_position;
		Fvector eye_direction;
		float object_range;
		float half_fov;
		float cos_half_fov;
	};

	typedef xr_unordered_flat_map<u16, u32> OBJECT_INDEX_CACHE;
	mutable OBJECT_INDEX_CACHE m_visible_object_indices;
	mutable OBJECT_INDEX_CACHE m_not_yet_visible_object_indices;

private:
	DELAYED_VISIBLE_OBJECTS m_delayed_objects;

private:
	CVisionParameters m_free;
	CVisionParameters m_danger;

private:
	u32 m_max_object_count;
	bool m_enabled;
	u32 m_last_update_time;
	float m_vision_speed = 1.0f; // per-NPC vision-speed factor; scales get_visible_value (1.0 = vanilla). Deliberately not reset in reinit()/reload(): persists while online, re-set from script on spawn.

public:
	void add_visible_object(const CObject* object, float time_delta, bool fictitious = false);
	u32 add_fictitious_visible_mask(const CObject* object, squad_mask_type knowledge_mask);

private:
	void build_observer_context(SVisionObserverContext& context) const;
	float object_visible_distance(const CGameObject* game_object, float& object_distance,
	                              const SVisionObserverContext& context, const CVisionParameters& parameters) const;
	float object_luminocity(const CGameObject* game_object, const CVisionParameters& parameters) const;
	float get_visible_value(const CGameObject* game_object, float distance, float object_distance, float time_delta,
	                        float object_velocity, float luminocity, const CVisionParameters& parameters,
	                        const ::luabind::functor<float>* callback) const;
	bool visible(const CGameObject* game_object, float time_delta, const SVisionObserverContext& context,
	             const ::luabind::functor<float>* callback);
	void add_visible_object(const CObject* object, float time_delta, bool fictitious,
	                        const SVisionObserverContext& context, const ::luabind::functor<float>* callback);

	VISIBLES::iterator find_visible_object(u16 id);
	VISIBLES::const_iterator find_visible_object(u16 id) const;
	NOT_YET_VISIBLES::iterator find_not_yet_visible_object(u16 id);

protected:
	IC void fill_object(CVisibleObject& visible_object, const CGameObject* game_object);
	bool should_ignore_object(CObject const* object) const;
	void add_visible_object(const CVisibleObject visible_object);

public:
	float object_visible_distance(const CGameObject* game_object, float& object_distance) const;
	float object_luminocity(const CGameObject* game_object) const;
	float get_visible_value(const CGameObject* game_object, float distance, float object_distance, float time_delta,
	                        float object_velocity, float luminocity) const;
	float get_object_velocity(const CGameObject* game_object, const CNotYetVisibleObject& not_yet_visible_object) const;
	u32 get_prev_time(const CGameObject* game_object) const;

public:
	u32 visible_object_time_last_seen(const CObject* object) const;

protected:
	void add_not_yet_visible_object(const CNotYetVisibleObject& not_yet_visible_object);
	CNotYetVisibleObject* not_yet_visible_object(const CGameObject* game_object);

private:
	void initialize();
	void refresh_visible_objects() const;

public:
	CVisualMemoryManager(CCustomMonster* object);
	CVisualMemoryManager(CAI_Stalker* stalker);
	CVisualMemoryManager(vision_client* client);
	virtual ~CVisualMemoryManager();
	virtual void reinit();
	virtual void reload(LPCSTR section);
	virtual void update(float time_delta);
	virtual float feel_vision_mtl_transp(CObject* O, u32 element);
	void remove_links(CObject* object);

public:
	bool visible(const CGameObject* game_object, float time_delta);
	bool visible(u32 level_vertex_id, float yaw, float eye_fov) const;

public:
	void set_squad_objects(xr_vector<CVisibleObject>* squad_objects);
	void set_squad_objects(xr_vector<CVisibleObject>* squad_objects, CAgentMemoryManager* merge_index);
	CVisibleObject* visible_object(const CGameObject* game_object);
	const CVisibleObject* object_by_id(u16 id) const;

public:
	// this function returns true if and only if 
	// specified object is visible now
	bool visible_right_now(const CGameObject* game_object) const;
	// if current_params.m_still_visible_time == 0
	// this function returns true if and only if 
	// specified object is visible now
	// if current_params.m_still_visible_time > 0
	// this function returns true if and only if 
	// specified object is visible now or 
	// some time ago <= current_params.m_still_visible_time
	bool visible_now(const CGameObject* game_object) const;

public:
	void enable(const CObject* object, bool enable);

public:
	IC float visibility_threshold() const;
	IC float transparency_threshold() const;

public:
	IC bool enabled() const;
	IC void enable(bool value);
	void set_vision_speed(float value) { m_vision_speed = (value < 0.f) ? 0.f : value; }
	float vision_speed() const { return (m_vision_speed); }

public:
	IC const VISIBLES& objects() const;
	IC const RAW_VISIBLES& raw_objects() const;
	IC const NOT_YET_VISIBLES& not_yet_visible_objects() const;
	IC const CVisionParameters& current_state() const;
	IC squad_mask_type mask() const;

public:
#ifdef DEBUG
			void					check_visibles				() const;
#endif

public:
	void save(NET_Packet& packet) const;
	void load(IReader& packet);
	void on_requested_spawn(CObject* object);

private:
	void clear_delayed_objects();
};

#include "visual_memory_manager_inline.h"
