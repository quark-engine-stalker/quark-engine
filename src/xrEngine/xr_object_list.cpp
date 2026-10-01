#include "stdafx.h"
#include "igame_level.h"
#include "igame_persistent.h"

#include "xrSheduler.h"
#include "xr_object_list.h"
#include "std_classes.h"

#include "xr_object.h"
#include "../xrCore/net_utils.h"
#include "../xrCore/profiler.h"

#include "CustomHUD.h"

class fClassEQ
{
	CLASS_ID cls;
public:
	fClassEQ(CLASS_ID C) : cls(C)
	{};
	IC bool operator()(CObject* O) { return cls == O->CLS_ID; }
};
#ifdef DEBUG
BOOL debug_destroy = TRUE;
#endif

CObjectList::CObjectList() :
	m_owner_thread_id(GetCurrentThreadId())
{
	ZeroMemory(map_NETID, 0xffff * sizeof(CObject*));
}

CObjectList::~CObjectList()
{
	R_ASSERT(objects_active.empty());
	R_ASSERT(objects_sleeping.empty());
	R_ASSERT(destroy_queue.empty());
	//. R_ASSERT ( map_NETID.empty() );
}

CObject* CObjectList::FindObjectByName(shared_str name)
{
	for (Objects::iterator I = objects_active.begin(); I != objects_active.end(); I++)
		if ((*I)->cName().equal(name)) return (*I);
	for (Objects::iterator I = objects_sleeping.begin(); I != objects_sleeping.end(); I++)
		if ((*I)->cName().equal(name)) return (*I);
	return NULL;
}

CObject* CObjectList::FindObjectByName(LPCSTR name)
{
	return FindObjectByName(shared_str(name));
}

CObject* CObjectList::FindObjectByCLS_ID(CLASS_ID cls)
{
	{
		Objects::iterator O = std::find_if(objects_active.begin(), objects_active.end(), fClassEQ(cls));
		if (O != objects_active.end()) return *O;
	}
	{
		Objects::iterator O = std::find_if(objects_sleeping.begin(), objects_sleeping.end(), fClassEQ(cls));
		if (O != objects_sleeping.end()) return *O;
	}

	return NULL;
}

void CObjectList::o_remove(Objects& v, CObject* O)
{
	//. if(O->ID()==1026)
	//. {
	//. Log("ahtung");
	//. }
	Objects::iterator _i = std::find(v.begin(), v.end(), O);
	VERIFY(_i != v.end());
	v.erase(_i);
	//. Msg("---o_remove[%s][%d]", O->cName().c_str(), O->ID() );
}

void CObjectList::o_activate(CObject* O)
{
	VERIFY(O && O->processing_enabled());
	o_remove(objects_sleeping, O);
	objects_active.push_back(O);
	O->MakeMeCrow();
}

void CObjectList::o_sleep(CObject* O)
{
	VERIFY(O && !O->processing_enabled());
	o_remove(objects_active, O);
	objects_sleeping.push_back(O);
	O->MakeMeCrow();
}

void CObjectList::SingleUpdate(CObject* O)
{
	if (Device.dwFrame == O->dwFrame_UpdateCL)
	{
#ifdef DEBUG
		// if (O->getDestroy())
		// Msg ("- !!!processing_enabled ->destroy_queue.push_back %s[%d] frame [%d]",O->cName().c_str(), O->ID(), Device.dwFrame);
#endif // #ifdef DEBUG

		return;
	}

	if (!O->processing_enabled())
	{
#ifdef DEBUG
		// if (O->getDestroy())
		// Msg ("- !!!processing_enabled ->destroy_queue.push_back %s[%d] frame [%d]",O->cName().c_str(), O->ID(), Device.dwFrame);
#endif // #ifdef DEBUG

		return;
	}

	if (O->H_Parent())
		SingleUpdate(O->H_Parent());

	if (g_bEnableStatGather)
		Device.Statistic->UpdateClient_updated++;
	O->dwFrame_UpdateCL = Device.dwFrame;

	// Msg ("[%d][0x%08x]IAmNotACrowAnyMore (CObjectList::SingleUpdate)", Device.dwFrame, fast_dynamic_cast<void*>(O));

	O->UpdateCL();
#ifdef DEBUG
	VERIFY3(O->dbg_update_cl == Device.dwFrame, "Broken sequence of calls to 'UpdateCL'", *O->cName());
#endif
#if 0//ndef DEBUG
    __try
    {
#endif
	if (O->H_Parent() && (O->H_Parent()->getDestroy() || O->H_Root()->getDestroy()))
	{
		// Push to destroy-queue if it isn't here already
		Msg("! ERROR: incorrect destroy sequence for object[%d:%s], section[%s], parent[%d:%s]", O->ID(), *O->cName(),
		    *O->cNameSect(), O->H_Parent()->ID(), *O->H_Parent()->cName());
	}
#if 0//ndef DEBUG
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CObject* parent_obj = O->H_Parent();
        CObject* root_obj = O->H_Root();
        Msg ("! ERROR: going to crush: [%d:%s], section[%s], parent_obj_addr[0x%08x], root_obj_addr[0x%08x]",O->ID(),*O->cName(),*O->cNameSect(), *((u32*)&parent_obj), *((u32*)&root_obj));
        if (parent_obj)
        {
            __try
            {
                Msg("! Parent object: [%d:%s], section[%s]",
                    parent_obj->ID(),
                    parent_obj->cName().c_str(),
                    parent_obj->cNameSect().c_str());

            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Msg("! Failed to get parent object info.");
            }
        }
        if (root_obj)
        {
            __try
            {
                Msg("! Root object: [%d:%s], section[%s]",
                    root_obj->ID(),
                    root_obj->cName().c_str(),
                    root_obj->cNameSect().c_str());
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Msg("! Failed to get root object info.");
            }
        }
        R_ASSERT(false);
    } //end of __except
#endif

#ifdef DEBUG
	// if (O->getDestroy())
	// Msg ("- !!!processing_enabled ->destroy_queue.push_back %s[%d] frame [%d]",O->cName().c_str(), O->ID(), Device.dwFrame);
#endif // #ifdef DEBUG
}

void CObjectList::clear_crow_vec(Objects& o)
{
	for (u32 _it = 0; _it < o.size(); _it++)
	{
		// Msg ("[%d][0x%08x]IAmNotACrowAnyMore (clear_crow_vec)", Device.dwFrame, fast_dynamic_cast<void*>(o[_it]));
		o[_it]->IAmNotACrowAnyMore();
	}
	o.clear_not_free();
}

void CObjectList::Update(bool bForce)
{
	if (!Device.Paused() || bForce)
	{
		// Clients
		if (Device.fTimeDelta > EPS_S || bForce)
		{

			// Select Crow-Mode
			Device.Statistic->UpdateClient_updated = 0;

			Objects& crows = m_crows[0];
			Objects* workload = nullptr;
			{

				Objects& crows1 = m_crows[1];
				crows.insert(crows.end(), crows1.begin(), crows1.end());
				crows1.clear_not_free();

#if 0
            std::sort (crows.begin(), crows.end());
            crows.erase (
                std::unique(
                    crows.begin(),
                    crows.end()
                ),
                crows.end()
            );
#else
# ifdef DEBUG
            std::sort(crows.begin(), crows.end());
            VERIFY(
                std::unique(
                    crows.begin(),
                    crows.end()
                ) == crows.end()
            );
# endif // ifdef DEBUG
#endif

				Device.Statistic->UpdateClient_crows = crows.size();
				if (!psDeviceFlags.test(rsDisableObjectsAsCrows))
					workload = &crows;
				else
				{
					workload = &objects_active;
					clear_crow_vec(crows);
				}

				Device.Statistic->UpdateClient.Begin();
				Device.Statistic->UpdateClient_active = objects_active.size();
				Device.Statistic->UpdateClient_total = objects_active.size() + objects_sleeping.size();

				START_PROFILE("CObjectList/Copy update workload");
				m_update_workload.clear_and_reserve();
				m_update_workload.insert(
					m_update_workload.end(), workload->begin(), workload->end());
				STOP_PROFILE;

				crows.clear_not_free();

				for (Objects::iterator i = m_update_workload.begin(); i != m_update_workload.end(); ++i)
				{
					(*i)->IAmNotACrowAnyMore();
					(*i)->dwFrame_AsCrow = u32(-1);
				}
			}

			{

				START_PROFILE("CObjectList/SingleUpdate");
				for (Objects::iterator i = m_update_workload.begin(); i != m_update_workload.end(); ++i)
					SingleUpdate(*i);
				STOP_PROFILE;
			}

			Device.Statistic->UpdateClient.End();
		}
	}

	// Destroy
	if (!destroy_queue.empty())
	{
		// Large destruction bursts are pathological in heavily modded games: the
		// legacy O((active+sleeping)*destroy_count) relcase fan-out can monopolize
		// the main thread for 100+ ms. Keep the exact legacy path for normal bursts
		// and amortize only a large backlog. Work is taken from the tail so actual
		// net_Destroy order remains the historical reverse registration order.
		constexpr u32 destroy_batch_threshold = 32;
		constexpr u32 destroy_max_per_frame = 24;
		const u32 total_destroy_count = static_cast<u32>(destroy_queue.size());
		// A level teardown is not a frame-budgeted operation. More importantly, its
		// callers expect one Update() to finish the pending reverse-order destruction
		// before another object-update pass starts. Leaving a partially destroyed
		// world alive between remove_objects() iterations invalidates actor/HUD and
		// restriction-manager lifetime assumptions.
		const bool amortize_destroy = g_pGameLevel && g_pGameLevel->bReady &&
			total_destroy_count > destroy_batch_threshold;
		const u32 destroy_count = amortize_destroy ?
			_min(total_destroy_count, destroy_max_per_frame) : total_destroy_count;
		const u32 destroy_begin = total_destroy_count - destroy_count;

		// A relcase/net_Destroy callback may append to destroy_queue and reallocate
		// its storage. Snapshot every batch before invoking callbacks; keep the
		// common small batch on the stack to avoid an allocation.
		CObject* small_destroy_batch[destroy_batch_threshold];
		Objects destroy_batch;
		CObject* const* destroy_objects = small_destroy_batch;
		if (destroy_count <= destroy_batch_threshold)
		{
			for (u32 index = 0; index < destroy_count; ++index)
				small_destroy_batch[index] = destroy_queue[destroy_begin + index];
		}
		else
		{
			destroy_batch.reserve(destroy_count);
			for (u32 index = destroy_begin; index < total_destroy_count; ++index)
				destroy_batch.push_back(destroy_queue[index]);
			destroy_objects = destroy_batch.data();
		}

		{

			for (CObject* object : objects_active)
				for (u32 index = destroy_count; index-- > 0;)
				{
					CObject* const destroyed = destroy_objects[index];
					object->net_Relcase(destroyed);
				}
		}
		{

			for (CObject* object : objects_sleeping)
				for (u32 index = destroy_count; index-- > 0;)
				{
					CObject* const destroyed = destroy_objects[index];
					object->net_Relcase(destroyed);
				}
		}

		{

			if (Sound)
				for (u32 index = destroy_count; index-- > 0;)
					Sound->object_relcase(destroy_objects[index]);
		}

		{

			RELCASE_CALLBACK_VEC::iterator It = m_relcase_callbacks.begin();
			RELCASE_CALLBACK_VEC::iterator Ite = m_relcase_callbacks.end();
			for (; It != Ite; ++It)
			{
				VERIFY(*(*It).m_ID == (It - m_relcase_callbacks.begin()));
				for (u32 index = 0; index < destroy_count; ++index)
					(*It).m_Callback(destroy_objects[index]);
			}
		}

		// HUD is a single relcase consumer, not one consumer per registered
		// callback. The old nesting invoked it callback_count times per object.
		{

			if (g_hud)
				for (u32 index = 0; index < destroy_count; ++index)
					g_hud->net_Relcase(destroy_objects[index]);
		}

		// Destroy. Keep the historical reverse queue order exactly.
		{

			for (u32 index = destroy_count; index-- > 0;)
			{
				CObject* O = destroy_objects[index];
#ifdef DEBUG
				if (debug_destroy)
					Msg("Destroying object[%x][%x] [%d][%s] frame[%d]", fast_dynamic_cast<void*>(O), O, O->ID(), *O->cName(), Device.dwFrame);
#endif // DEBUG
				O->net_Destroy();
				Destroy(O);
			}
		}
		// Remove only the snapshotted range. Callbacks may have appended new
		// destroy requests, which must remain queued for the next update.
		destroy_queue.erase(destroy_queue.begin() + destroy_begin,
			destroy_queue.begin() + total_destroy_count);
	}

}

void CObjectList::net_Register(CObject* O)
{
	R_ASSERT(O);
	R_ASSERT(O->ID() < 0xffff);

	map_NETID[O->ID()] = O;

	//. map_NETID.insert(mk_pair(O->ID(),O));
	//Msg ("-------------------------------- Register: %s",O->cName());
}

void CObjectList::net_Unregister(CObject* O)
{
	//R_ASSERT (O->ID() < 0xffff);
	if (O->ID() < 0xffff) //demo_spectator can have 0xffff
		map_NETID[O->ID()] = NULL;
	/*
	 xr_map<u32,CObject*>::iterator it = map_NETID.find(O->ID());
	 if ((it!=map_NETID.end()) && (it->second == O)) {
	 // Msg ("-------------------------------- Unregster: %s",O->cName());
	 map_NETID.erase(it);
	 }
	 */
}

int g_Dump_Export_Obj = 0;

u32 CObjectList::net_Export(NET_Packet* _Packet, u32 start, u32 max_object_size)
{
	if (g_Dump_Export_Obj) Msg("---- net_export --- ");

	NET_Packet& Packet = *_Packet;
	u32 position;
	for (; start < objects_active.size() + objects_sleeping.size(); start++)
	{
		CObject* P = (start < objects_active.size())
			             ? objects_active[start]
			             : objects_sleeping[start - objects_active.size()];
		if (P->net_Relevant() && !P->getDestroy())
		{
			Packet.w_u16(u16(P->ID()));
			Packet.w_chunk_open8(position);
			//Msg ("cl_export: %d '%s'",P->ID(),*P->cName());
			P->net_Export(Packet);

#ifdef DEBUG
            u32 size = u32(Packet.w_tell() - position) - sizeof(u8);
            if (size >= 256)
            {
                Debug.fatal(DEBUG_INFO, "Object [%s][%d] exceed network-data limit\n size=%d, Pend=%d, Pstart=%d",
                            *P->cName(), P->ID(), size, Packet.w_tell(), position);
            }
#endif
			if (g_Dump_Export_Obj)
			{
				u32 size = u32(Packet.w_tell() - position) - sizeof(u8);
				Msg("* %s : %d", *(P->cNameSect()), size);
			}
			Packet.w_chunk_close8(position);
			// if (0==(--count))
			// break;
			if (max_object_size >= (NET_PacketSizeLimit - Packet.w_tell()))
				break;
		}
	}
	if (g_Dump_Export_Obj) Msg("------------------- ");
	return start + 1;
}

int g_Dump_Import_Obj = 0;

void CObjectList::net_Import(NET_Packet* Packet)
{
	if (g_Dump_Import_Obj) Msg("---- net_import --- ");

	while (!Packet->r_eof())
	{
		u16 ID;
		Packet->r_u16(ID);
		u8 size;
		Packet->r_u8(size);
		CObject* P = net_Find(ID);
		if (P)
		{
			u32 rsize = Packet->r_tell();

			P->net_Import(*Packet);

			if (g_Dump_Import_Obj) Msg("* %s : %d - %d", *(P->cNameSect()), size, Packet->r_tell() - rsize);
		}
		else Packet->r_advance(size);
	}

	if (g_Dump_Import_Obj) Msg("------------------- ");
}

/*
CObject* CObjectList::net_Find(u16 ID)
{

xr_map<u32,CObject*>::iterator it = map_NETID.find(ID);
return (it==map_NETID.end())?0:it->second;
}
*/
void CObjectList::Load()
{
	R_ASSERT(/*map_NETID.empty() &&*/ objects_active.empty() && destroy_queue.empty() && objects_sleeping.empty());
}

void CObjectList::Unload()
{
	if (objects_sleeping.size() || objects_active.size())
		Msg("! objects-leaked: %d", objects_sleeping.size() + objects_active.size());

	// Destroy objects
	while (objects_sleeping.size())
	{
		CObject* O = objects_sleeping.back();
		Msg("! [%x] s[%4d]-[%s]-[%s]", O, O->ID(), *O->cNameSect(), *O->cName());
		O->setDestroy(true);

#ifdef DEBUG
        if (debug_destroy)
            Msg("Destroying object [%d][%s]", O->ID(), *O->cName());
#endif
		O->net_Destroy();
		Destroy(O);
	}
	while (objects_active.size())
	{
		CObject* O = objects_active.back();
		Msg("! [%x] a[%4d]-[%s]-[%s]", O, O->ID(), *O->cNameSect(), *O->cName());
		O->setDestroy(true);

#ifdef DEBUG
        if (debug_destroy)
            Msg("Destroying object [%d][%s]", O->ID(), *O->cName());
#endif
		O->net_Destroy();
		Destroy(O);
	}

	// These arrays are level-scoped. Return mass-spawn/high-water capacity at the
	// unload boundary instead of carrying it through the menu and into the next level.
	destroy_queue.clear_and_free();
	objects_active.clear_and_free();
	objects_sleeping.clear_and_free();
	m_crows[0].clear_and_free();
	m_crows[1].clear_and_free();
	m_update_workload.clear_and_free();
}

CObject* CObjectList::Create(LPCSTR name)
{
	CObject* O = g_pGamePersistent->ObjectPool.create(name);
	// Msg("CObjectList::Create [%x]%s", O, name);
	objects_sleeping.push_back(O);
	return O;
}

void CObjectList::Destroy(CObject* O)
{
	if (0 == O) return;
	net_Unregister(O);

	if (!Device.Paused())
	{
		if (!m_crows[1].empty())
		{
			Msg("assertion !m_crows[1].empty() failed: %d", m_crows[1].size());

			Objects::const_iterator i = m_crows[1].begin();
			Objects::const_iterator const e = m_crows[1].end();
			for (u32 j = 0; i != e; ++i, ++j)
				Msg("%d %s", j, (*i)->cName().c_str());
			VERIFY(Device.Paused() || m_crows[1].empty());
			m_crows[1].clear_not_free();
		}
	}
	else
	{
		Objects& crows = m_crows[1];
		Objects::iterator const i = std::find(crows.begin(), crows.end(), O);
		if (i != crows.end())
		{
			crows.erase(i);
			VERIFY(std::find(crows.begin(), crows.end(), O) == crows.end());
		}
	}

	Objects& crows = m_crows[0];
	Objects::iterator _i0 = std::find(crows.begin(), crows.end(), O);
	if (_i0 != crows.end())
	{
		crows.erase(_i0);
		VERIFY(std::find(crows.begin(), crows.end(), O) == crows.end());
	}

	// active/inactive. processing_enabled() normally identifies the owning list,
	// so search that list first. Keep the opposite-list fallback for exact legacy
	// behavior during transitional states.
	Objects* primary = O->processing_enabled() ? &objects_active : &objects_sleeping;
	Objects* secondary = O->processing_enabled() ? &objects_sleeping : &objects_active;
	Objects::iterator found = std::find(primary->begin(), primary->end(), O);
	if (found != primary->end())
	{
		primary->erase(found);
		VERIFY(std::find(primary->begin(), primary->end(), O) == primary->end());
		VERIFY(std::find(secondary->begin(), secondary->end(), O) == secondary->end());
	}
	else
	{
		found = std::find(secondary->begin(), secondary->end(), O);
		if (found != secondary->end())
		{
			secondary->erase(found);
			VERIFY(std::find(secondary->begin(), secondary->end(), O) == secondary->end());
		}
		else
			FATAL("! Unregistered object being destroyed");
	}

	g_pGamePersistent->ObjectPool.destroy(O);
}

void CObjectList::relcase_register(RELCASE_CALLBACK cb, int* ID)
{
#ifdef DEBUG
    RELCASE_CALLBACK_VEC::iterator It = std::find(m_relcase_callbacks.begin(),
                                        m_relcase_callbacks.end(),
                                        cb);
    VERIFY(It == m_relcase_callbacks.end());
#endif
	*ID = m_relcase_callbacks.size();
	m_relcase_callbacks.push_back(SRelcasePair(ID, cb));
}

void CObjectList::relcase_unregister(int* ID)
{
	VERIFY(m_relcase_callbacks[*ID].m_ID == ID);
	m_relcase_callbacks[*ID] = m_relcase_callbacks.back();
	*m_relcase_callbacks.back().m_ID = *ID;
	m_relcase_callbacks.pop_back();
}

void CObjectList::dump_list(Objects& v, LPCSTR reason)
{
	Objects::iterator it = v.begin();
	Objects::iterator it_e = v.end();
#ifdef DEBUG
    Msg("----------------dump_list [%s]", reason);
    for (; it != it_e; ++it)
        Msg("%x - name [%s] ID[%d] parent[%s] getDestroy()=[%s]",
            (*it),
            (*it)->cName().c_str(),
            (*it)->ID(),
            ((*it)->H_Parent()) ? (*it)->H_Parent()->cName().c_str() : "",
            ((*it)->getDestroy()) ? "yes" : "no");
#endif // #ifdef DEBUG
}

bool CObjectList::dump_all_objects()
{
	dump_list(destroy_queue, "destroy_queue");
	dump_list(objects_active, "objects_active");
	dump_list(objects_sleeping, "objects_sleeping");
	dump_list(m_crows[0], "m_crows[0]");
	dump_list(m_crows[1], "m_crows[1]");
	return false;
}

void CObjectList::register_object_to_destroy(CObject* object_to_destroy)
{
#ifdef DEBUG
	VERIFY(!registered_object_to_destroy(object_to_destroy));
#endif
	destroy_queue.push_back(object_to_destroy);

	Objects::iterator it = objects_active.begin();
	Objects::iterator it_e = objects_active.end();
	for (; it != it_e; ++it)
	{
		CObject* O = *it;
		if (!O->getDestroy() && O->H_Parent() == object_to_destroy)
		{
			Msg("setDestroy called, but not-destroyed child found parent[%d] child[%d]", object_to_destroy->ID(),
			    O->ID(), Device.dwFrame);
			O->setDestroy(TRUE);
		}
	}

	it = objects_sleeping.begin();
	it_e = objects_sleeping.end();
	for (; it != it_e; ++it)
	{
		CObject* O = *it;
		if (!O->getDestroy() && O->H_Parent() == object_to_destroy)
		{
			Msg("setDestroy called, but not-destroyed child found parent[%d] child[%d]", object_to_destroy->ID(),
			    O->ID(), Device.dwFrame);
			O->setDestroy(TRUE);
		}
	}
}

#ifdef DEBUG
bool CObjectList::registered_object_to_destroy(const CObject* object_to_destroy) const
{
    return (
               std::find(
                   destroy_queue.begin(),
                   destroy_queue.end(),
                   object_to_destroy
               ) !=
               destroy_queue.end()
           );
}
#endif // DEBUG
