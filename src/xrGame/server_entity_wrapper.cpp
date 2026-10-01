////////////////////////////////////////////////////////////////////////////
//	Module 		: server_entity_wrapper.cpp
//	Created 	: 16.10.2004
//  Modified 	: 16.10.2004
//	Author		: Dmitriy Iassenev
//	Description : Server entity wrapper
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "server_entity_wrapper.h"
#include "xrServer_Objects.h"
#include "xrmessages.h"

#ifdef AI_COMPILER
#	include "factory_api.h"
#endif

struct ISE_Abstract;

CServerEntityWrapper::~CServerEntityWrapper()
{
	F_entity_Destroy(m_object);
}

void CServerEntityWrapper::save(IWriter& stream)
{
	NET_Packet net_packet;

	// Spawn
	stream.open_chunk(0);

	m_object->Spawn_Write(net_packet,TRUE);
	stream.w_u16(u16(net_packet.B.count));
	stream.w(net_packet.B.data, net_packet.B.count);

	stream.close_chunk();

	// Update
	stream.open_chunk(1);

	net_packet.w_begin(M_UPDATE);
	m_object->UPDATE_Write(net_packet);
	stream.w_u16(u16(net_packet.B.count));
	stream.w(net_packet.B.data, net_packet.B.count);

	//	u16						ID;
	//	net_packet.r_begin		(ID);
	//	VERIFY					(ID==M_UPDATE);
	//	m_object->UPDATE_Read	(net_packet);

	stream.close_chunk();
}

void CServerEntityWrapper::load(IReader& stream)
{
	NET_Packet net_packet;
	u16 ID;
	IReader* chunk;

	chunk = stream.open_chunk(0);
	if (!chunk)
	{
		R_ASSERT2(false, "Cannot find server entity spawn chunk");
		return;
	}
	if (chunk->elapsed() < int(sizeof(u16)))
	{
		chunk->close();
		R_ASSERT2(false, "Corrupted server entity spawn chunk");
		return;
	}

	const u16 spawn_packet_size = chunk->r_u16();
	if (spawn_packet_size < sizeof(u16) || spawn_packet_size > NET_PacketSizeLimit ||
		int(spawn_packet_size) > chunk->elapsed())
	{
		chunk->close();
		R_ASSERT2(false, "Invalid or truncated server entity spawn packet");
		return;
	}
	net_packet.B.count = spawn_packet_size;
	chunk->r(net_packet.B.data, net_packet.B.count);

	chunk->close();

	net_packet.r_begin(ID);
	if (M_SPAWN != ID)
	{
		R_ASSERT2(false, "Invalid packet ID (!= M_SPAWN)!");
		return;
	}

	string64 s_name;
	const u8* section_begin = net_packet.B.data + net_packet.r_pos;
	const void* section_end = memchr(section_begin, 0, net_packet.r_elapsed());
	if (!net_packet.r_elapsed() || !section_end ||
		u32(static_cast<const u8*>(section_end) - section_begin) >= sizeof(s_name))
	{
		R_ASSERT2(false, "Unterminated server entity section name");
		return;
	}
	net_packet.r_stringZ_s(s_name);

	m_object = F_entity_Create(s_name);

	if (!m_object)
	{
		R_ASSERT3(false, "Can't create entity.", s_name);
		return;
	}
	m_object->Spawn_Read(net_packet);

	chunk = stream.open_chunk(1);
	if (!chunk)
	{
		F_entity_Destroy(m_object);
		m_object = nullptr;
		R_ASSERT2(false, "Cannot find server entity update chunk");
		return;
	}
	if (chunk->elapsed() < int(sizeof(u16)))
	{
		chunk->close();
		F_entity_Destroy(m_object);
		m_object = nullptr;
		R_ASSERT2(false, "Corrupted server entity update chunk");
		return;
	}

	const u16 update_packet_size = chunk->r_u16();
	if (update_packet_size < sizeof(u16) || update_packet_size > NET_PacketSizeLimit ||
		int(update_packet_size) > chunk->elapsed())
	{
		chunk->close();
		F_entity_Destroy(m_object);
		m_object = nullptr;
		R_ASSERT2(false, "Invalid or truncated server entity update packet");
		return;
	}
	net_packet.B.count = update_packet_size;
	chunk->r(net_packet.B.data, net_packet.B.count);

	chunk->close();

	net_packet.r_begin(ID);
	if (M_UPDATE != ID)
	{
		F_entity_Destroy(m_object);
		m_object = nullptr;
		R_ASSERT2(false, "Invalid packet ID (!= M_UPDATE)!");
		return;
	}
	m_object->UPDATE_Read(net_packet);
}

void CServerEntityWrapper::save_update(IWriter& stream)
{
	//	NET_Packet				net_packet;
	//	net_packet.w_begin		(M_UPDATE);
	//	m_object->save_update	(net_packet);
	//	stream.w_u16			(u16(net_packet.B.count));
	//	stream.w				(net_packet.B.data,net_packet.B.count);
}

void CServerEntityWrapper::load_update(IReader& stream)
{
	//	NET_Packet				net_packet;
	//	u16						ID;
	//
	//	net_packet.B.count		= stream.r_u16();
	//	stream.r				(net_packet.B.data,net_packet.B.count);
	//
	//	net_packet.r_begin		(ID);
	//	R_ASSERT2				(M_UPDATE == ID,"Invalid packet ID (!= M_UPDATE)!");
	//	m_object->load_update	(net_packet);
}
