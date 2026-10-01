////////////////////////////////////////////////////////////////////////////
//	Module 		: item_manager.cpp
//	Created 	: 27.12.2003
//  Modified 	: 27.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Item manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "item_manager.h"
#include "inventory_item.h"
#include "custommonster.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "restricted_object.h"
#include "movement_manager.h"
#include "ai_space.h"
#include "profiler.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager_smart_cover.h"
#include "restricted_object.h"

CItemManager::CItemManager(CCustomMonster* object)
{
	VERIFY(object);
	m_object = object;

	m_stalker = smart_cast<CAI_Stalker*>(m_object);
}

bool CItemManager::is_useful(const CGameObject* object) const
{
	return (m_object->useful(this, object));
}

bool CItemManager::useful(const CGameObject* object) const
{
	if (!object || object->getDestroy())
		return (false);

	if (!object->getEnabled())
		return (false);

	CGameObject* game_object = const_cast<CGameObject*>(object);
	if (!game_object->UsedAI_Locations())
		return (false);

	if (!inherited::is_useful(object))
		return (false);

	if (m_object->getDestroy())
		return (false);

	// Attached items are owned or otherwise unavailable for pickup.
	if (object->H_Parent())
		return (false);

	const CInventoryItem* inventory_item = const_cast<CGameObject*>(object)->cast_inventory_item();
	if (!inventory_item || !inventory_item->useful_for_NPC())
		return (false);

	const Fvector& position = object->Position();
	const u32 level_vertex_id = object->ai_location().level_vertex_id();
	const CRestrictedObject& restrictions = m_object->movement().restrictions();

	if (!restrictions.accessible(position))
		return (false);

	if (!restrictions.accessible(level_vertex_id))
		return (false);

	if (m_stalker && !m_stalker->can_take(inventory_item))
		return (false);

	if (!ai().get_level_graph())
		return (false);

	if (!ai().level_graph().valid_vertex_id(level_vertex_id))
		return (false);

	if (!ai().level_graph().inside(level_vertex_id, position))
		return (false);

	return (true);
}

float CItemManager::do_evaluate(const CGameObject* object) const
{
	VERIFY3(
		m_object->movement().restrictions().accessible(
			object->ai_location().level_vertex_id()
		),
		*m_object->cName(),
		*object->cName()
	);
	return (m_object->evaluate(this, object));
}

float CItemManager::evaluate(const CGameObject* object) const
{
	const CInventoryItem* inventory_item = const_cast<CGameObject*>(object)->cast_inventory_item();
	VERIFY(inventory_item);
	VERIFY(inventory_item->useful_for_NPC());
	return (1000000.f - (float)inventory_item->Cost());
}

void CItemManager::update()
{
	START_PROFILE("Memory Manager/items::update")
#ifdef DEBUG
	OBJECTS::const_iterator	I = m_objects.begin();
	OBJECTS::const_iterator	E = m_objects.end();
	for ( ; I != E; ++I)
		VERIFY3				(
			m_object->movement().restrictions().accessible(
				(*I)->ai_location().level_vertex_id()
			),
			*m_object->cName(),
			*(*I)->cName()
		);
#endif // DEBUG

		inherited::update();

		VERIFY3(
			!selected() ||
			m_object->movement().restrictions().accessible(selected()->ai_location().level_vertex_id()),
			*m_object->cName(),
			selected() ? *selected()->cName() : "<no selected item>"
		);

	STOP_PROFILE
}

void CItemManager::remove_links(CObject* object)
{
	// Search uses pointer identity only; keep the ordered lookup synchronized.
	inherited::remove_object((const CGameObject*)object);

	if (m_selected && (m_selected->ID() == object->ID()))
		m_selected = 0;
}

void CItemManager::on_restrictions_change()
{
	if (!m_selected)
		return;

	if (!m_object->movement().restrictions().accessible(m_selected->ai_location().level_vertex_id()))
	{
		m_selected = 0;
		return;
	}

	if (m_object->movement().restrictions().accessible(m_selected->Position()))
		return;

	m_selected = 0;
}
