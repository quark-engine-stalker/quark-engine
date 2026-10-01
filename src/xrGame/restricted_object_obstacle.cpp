////////////////////////////////////////////////////////////////////////////
//	Module 		: restricted_object_obstacle.cpp
//	Created 	: 18.08.2004
//  Modified 	: 23.08.2004
//	Author		: Dmitriy Iassenev
//	Description : Restricted object with obstacles' avoidance
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "restricted_object_obstacle.h"
#include "ai_space.h"
#include "level_graph.h"
#include "obstacles_query.h"

typedef obstacles_query::AREA AREA;

CRestrictedObjectObstacle::CRestrictedObjectObstacle(CCustomMonster* object, const obstacles_query& static_query,
                                                     const obstacles_query& dynamic_query) :
	inherited(object),
	m_static_query(static_query),
	m_dynamic_query(dynamic_query)
{
}

void CRestrictedObjectObstacle::apply(const obstacles_query& query, const u32& start_vertex_id) const
{
	CLevelGraph& graph = ai().level_graph();
	AREA::const_iterator I = query.area().begin();
	AREA::const_iterator E = query.area().end();
	for (; I != E; ++I)
	{
		if (*I == start_vertex_id)
			continue;

		graph.set_mask_no_check(*I);
	}
}

void CRestrictedObjectObstacle::add_border(u32 start_vertex_id, float radius) const
{
	inherited::add_border(start_vertex_id, radius);

	apply(m_static_query, start_vertex_id);
	apply(m_dynamic_query, start_vertex_id);
}

void CRestrictedObjectObstacle::apply(const obstacles_query& query, const Fvector& start_position,
                                      const Fvector& dest_position) const
{
	CLevelGraph& graph = ai().level_graph();
	AREA::const_iterator I = query.area().begin();
	AREA::const_iterator E = query.area().end();
	for (; I != E; ++I)
	{
		if (graph.inside(graph.vertex(*I), start_position))
			continue;

		if (graph.inside(graph.vertex(*I), dest_position))
			continue;

		graph.set_mask_no_check(*I);
	}
}

void CRestrictedObjectObstacle::add_border(const Fvector& start_position, const Fvector& dest_position) const
{
	inherited::add_border(start_position, dest_position);

	apply(m_static_query, start_position, dest_position);
	apply(m_dynamic_query, start_position, dest_position);
}

void CRestrictedObjectObstacle::apply(const obstacles_query& query, const u32& start_vertex_id,
                                      const u32& dest_vertex_id) const
{
	CLevelGraph& graph = ai().level_graph();
	AREA::const_iterator I = query.area().begin();
	AREA::const_iterator E = query.area().end();
	for (; I != E; ++I)
	{
		if (*I == start_vertex_id)
			continue;

		if (*I == dest_vertex_id)
			continue;

		graph.set_mask_no_check(*I);
	}
}

void CRestrictedObjectObstacle::add_border(u32 start_vertex_id, u32 dest_vertex_id) const
{
	inherited::add_border(start_vertex_id, dest_vertex_id);

	apply(m_static_query, start_vertex_id, dest_vertex_id);
	apply(m_dynamic_query, start_vertex_id, dest_vertex_id);
}

void CRestrictedObjectObstacle::remove_border() const
{
	inherited::remove_border();

	ai().level_graph().clear_mask_no_check(m_static_query.area());
	ai().level_graph().clear_mask_no_check(m_dynamic_query.area());
}

void CRestrictedObjectObstacle::collect_path_border(u32 start_vertex_id, u32 dest_vertex_id,
	                                                  xr_vector<u32>& result) const
{
	inherited::collect_path_border(start_vertex_id, dest_vertex_id, result);

	const obstacles_query* queries[] = {&m_static_query, &m_dynamic_query};
	for (u32 query_index = 0; query_index < sizeof(queries) / sizeof(queries[0]); ++query_index)
	{
		const AREA& area = queries[query_index]->area();
		for (AREA::const_iterator I = area.begin(), E = area.end(); I != E; ++I)
		{
			if (*I != start_vertex_id && *I != dest_vertex_id)
				result.push_back(*I);
		}
	}
}
