////////////////////////////////////////////////////////////////////////////
//	Module 		: path_manager_params.h
//	Created 	: 21.03.2002
//  Modified 	: 03.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Base path manager parameters
////////////////////////////////////////////////////////////////////////////

#pragma once

template <
	typename _dist_type,
	typename _index_type,
	typename _iteration_type
>
struct SBaseParameters
{
	_dist_type max_range;
	_iteration_type max_iteration_count;
	u32 max_visited_node_count;
	// Optional per-search mask. Level-path jobs use this instead of mutating
	// CLevelGraph::m_access_mask, which is shared by every NPC. A flat hash set
	// keeps the hot neighbour check O(1); binary-searching a border vector here
	// noticeably increased the total cost of long A* searches.
	const xr_unordered_flat_set<_index_type>* blocked_vertices;

	IC SBaseParameters(
		_dist_type max_range = type_max(_dist_type),
		_iteration_type max_iteration_count = _iteration_type(-1),
#ifndef AI_COMPILER
		u32 max_visited_node_count = 65500
#else
			u32				max_visited_node_count	= u32(-1)
#endif
	) :
		max_range(max_range),
		max_iteration_count(max_iteration_count),
		max_visited_node_count(max_visited_node_count),
		blocked_vertices(nullptr)
	{
	}

	IC bool actual() const
	{
		return (true);
	}
};
