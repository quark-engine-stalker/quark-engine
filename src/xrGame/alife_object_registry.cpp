////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_object_registry.h"
#include "ai_debug.h"

namespace
{
bool read_saved_packet(IReader& file_stream, NET_Packet& packet, LPCSTR packet_name)
{
	if (file_stream.elapsed() < int(sizeof(u16)))
	{
		Msg("! Corrupted saved game: missing %s packet size", packet_name);
		return false;
	}

	const u16 packet_size = file_stream.r_u16();
	if (packet_size < sizeof(u16) || packet_size > NET_PacketSizeLimit || int(packet_size) > file_stream.elapsed())
	{
		Msg("! Corrupted saved game: invalid %s packet size %u (remaining %d)",
			packet_name, packet_size, file_stream.elapsed());
		return false;
	}

	packet.B.count = packet_size;
	file_stream.r(packet.B.data, packet_size);
	return true;
}
}

CALifeObjectRegistry::CALifeObjectRegistry(LPCSTR section)
{
}

CALifeObjectRegistry::~CALifeObjectRegistry()
{
	OBJECT_REGISTRY::iterator const B = m_objects.begin();
	OBJECT_REGISTRY::iterator I = B;
	OBJECT_REGISTRY::iterator const E = m_objects.end();
	for (; I != E; ++I)
		(*I).second->on_unregister();

	for (I = B; I != E; ++I)
		xr_delete((*I).second);
}

void CALifeObjectRegistry::save(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count)
{
	++object_count;

	NET_Packet tNetPacket;
	// Spawn
	object->Spawn_Write(tNetPacket,TRUE);
	memory_stream.w_u16(u16(tNetPacket.B.count));
	memory_stream.w(tNetPacket.B.data, tNetPacket.B.count);

	// Update
	tNetPacket.w_begin(M_UPDATE);
	object->UPDATE_Write(tNetPacket);

	memory_stream.w_u16(u16(tNetPacket.B.count));
	memory_stream.w(tNetPacket.B.data, tNetPacket.B.count);

	ALife::OBJECT_VECTOR::const_iterator I = object->children.begin();
	ALife::OBJECT_VECTOR::const_iterator E = object->children.end();
	for (; I != E; ++I)
	{
		CSE_ALifeDynamicObject* child = this->object(*I, true);
		if (!child)
			continue;

		if (!child->can_save())
			continue;

		save(memory_stream, child, object_count);
	}
}

void CALifeObjectRegistry::save(IWriter& memory_stream)
{
	Msg("* Saving objects...");
	memory_stream.open_chunk(OBJECT_CHUNK_DATA);

	u32 position = memory_stream.tell();
	memory_stream.w_u32(u32(-1));

	u32 object_count = 0;
	OBJECT_REGISTRY::iterator I = m_objects.begin();
	OBJECT_REGISTRY::iterator E = m_objects.end();
	for (; I != E; ++I)
	{
		if (!(*I).second->can_save())
			continue;

		if ((*I).second->redundant())
			continue;

		if ((*I).second->ID_Parent != 0xffff)
			continue;

		save(memory_stream, (*I).second, object_count);
	}

	u32 last_position = memory_stream.tell();
	memory_stream.seek(position);
	memory_stream.w_u32(object_count);
	memory_stream.seek(last_position);

	memory_stream.close_chunk();

	Msg("* %d objects are successfully saved", object_count);
}

CSE_ALifeDynamicObject* CALifeObjectRegistry::get_object(IReader& file_stream, bool* corrupted)
{
	if (corrupted)
		*corrupted = false;

	NET_Packet tNetPacket;
	u16 u_id;
	// Spawn
	if (!read_saved_packet(file_stream, tNetPacket, "spawn"))
	{
		if (corrupted)
			*corrupted = true;
		return nullptr;
	}

	tNetPacket.r_begin(u_id);
	if (M_SPAWN != u_id)
	{
		Msg("! Corrupted saved game: invalid spawn packet ID %u", u_id);
		if (corrupted)
			*corrupted = true;
		return nullptr;
	}

	string64 s_name;
	const u8* section_begin = tNetPacket.B.data + tNetPacket.r_pos;
	const void* section_end = memchr(section_begin, 0, tNetPacket.r_elapsed());
	if (!tNetPacket.r_elapsed() || !section_end ||
		u32(static_cast<const u8*>(section_end) - section_begin) >= sizeof(s_name))
	{
		Msg("! Corrupted saved game: unterminated entity section name");
		if (corrupted)
			*corrupted = true;
		return nullptr;
	}
	tNetPacket.r_stringZ_s(s_name);
#ifdef DEBUG
	if (psAI_Flags.test(aiALife)) {
		Msg					("Loading object %s [%d]b", s_name, tNetPacket.B.count);
	}
#endif
	// create entity
	CSE_Abstract* tpSE_Abstract = F_entity_Create(s_name);
	if (!tpSE_Abstract)
	{
		Msg("! Can't create entity '%s'", s_name);
		if (!read_saved_packet(file_stream, tNetPacket, "update"))
		{
			if (corrupted)
				*corrupted = true;
		}
		return nullptr;
	}
	CSE_ALifeDynamicObject* tpALifeDynamicObject = smart_cast<CSE_ALifeDynamicObject*>(tpSE_Abstract);
	if (!tpALifeDynamicObject)
	{
		Msg("! Corrupted saved game: non-ALife object '%s'", s_name);
		F_entity_Destroy(tpSE_Abstract);
		if (corrupted)
			*corrupted = true;
		return nullptr;
	}
	tpALifeDynamicObject->Spawn_Read(tNetPacket);

	// Update
	if (!read_saved_packet(file_stream, tNetPacket, "update"))
	{
		F_entity_Destroy(tpSE_Abstract);
		if (corrupted)
			*corrupted = true;
		return nullptr;
	}

	tNetPacket.r_begin(u_id);
	if (M_UPDATE != u_id)
	{
		Msg("! Corrupted saved game: invalid update packet ID %u", u_id);
		F_entity_Destroy(tpSE_Abstract);
		if (corrupted)
			*corrupted = true;
		return nullptr;
	}
	tpALifeDynamicObject->UPDATE_Read(tNetPacket);

	return (tpALifeDynamicObject);
}

bool CALifeObjectRegistry::load(IReader& file_stream)
{
	Msg("* Loading objects...");
	if (!file_stream.find_chunk(OBJECT_CHUNK_DATA) || file_stream.elapsed() < int(sizeof(u32)))
	{
		Msg("! Corrupted saved game: cannot find valid OBJECT_CHUNK_DATA");
		return false;
	}

	m_objects.clear();
	m_object_by_id.fill(nullptr);

	u32 count = file_stream.r_u32();
	constexpr u32 minimum_serialized_object_size = 2u * sizeof(u16) + 2u * sizeof(u16);
	if (count > u32(file_stream.elapsed()) / minimum_serialized_object_size)
	{
		Msg("! Corrupted saved game: invalid object count %u", count);
		return false;
	}

	for (u32 I = 0; I < count; ++I)
	{
		bool corrupted = false;
		CSE_ALifeDynamicObject* tpSE_Abstract = get_object(file_stream, &corrupted);
		if (corrupted)
			return false;

		if (!tpSE_Abstract)
			continue;

		add(tpSE_Abstract);
	}

	Msg("* %d objects are successfully loaded", m_objects.size());
	return true;
}
