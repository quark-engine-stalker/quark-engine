////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.h
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_simulator_base.h"

class NET_Packet;

class CALifeStorageManager : public virtual CALifeSimulatorBase
{
	friend class CALifeUpdatePredicate;
protected:
	typedef CALifeSimulatorBase inherited;

protected:
	string_path m_save_name;
	LPCSTR m_section;
	bool m_last_save_succeeded;

private:
	void prepare_objects_for_save();
	bool load(void* buffer, const u32& buffer_size, LPCSTR file_name);

public:
	IC CALifeStorageManager(xrServer* server, LPCSTR section);
	virtual ~CALifeStorageManager();
	bool load(LPCSTR save_name = 0);
	bool save(LPCSTR save_name = 0, bool update_name = true);
	bool save(NET_Packet& net_packet);
	static void prepare_load_async(LPCSTR save_name);
	// Returns a borrowed, decompressed payload prepared for the selected save.
	// The pointer stays valid until that load is consumed/discarded or another
	// save is prepared.
	static bool get_prepared_load_data(LPCSTR save_name, const void*& data, u32& data_size);
	static void discard_prepared_load();
	IC bool last_save_succeeded() const { return m_last_save_succeeded; }
};

#include "alife_storage_manager_inline.h"
