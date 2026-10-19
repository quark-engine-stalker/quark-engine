////////////////////////////////////////////////////////////////////////////
//	Module 		: game_graph_script.cpp
//	Created 	: 02.11.2005
//  Modified 	: 02.11.2005
//	Author		: Dmitriy Iassenev
//	Description : Game graph class script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "game_graph.h"
#include "ai_space.h"
#include "script_engine.h"

using namespace luabind;

const CGameGraph* get_game_graph()
{
	return (&ai().game_graph());
}

const CGameGraph::CHeader* get_header(const CGameGraph* self)
{
	return (&self->header());
}

bool get_accessible1(const CGameGraph* self, const u32& vertex_id)
{
	return (self->accessible(vertex_id));
}

void get_accessible2(const CGameGraph* self, const u32& vertex_id, bool value)
{
	self->accessible(vertex_id, value);
}

Fvector CVertex__level_point(const CGameGraph::CVertex* vertex)
{
	THROW(vertex);
	return (vertex->level_point());
}

Fvector CVertex__game_point(const CGameGraph::CVertex* vertex)
{
	THROW(vertex);
	return (vertex->game_point());
}

GameGraph::LEVEL_MAP const& get_levels(CGameGraph const* graph)
{
	THROW(graph);
	return graph->header().levels();
}

// Game vertex id of level_name's vertex whose level point is nearest to position; nil when the graph has no such
// level (or the level has no vertices).
::luabind::object get_nearest_vertex(lua_State* L, CGameGraph const* graph, LPCSTR level_name, const Fvector& position)
{
	THROW(graph);
	u32 vertex_id;
	float distance;
	const GameGraph::SLevel* level = graph->header().level(level_name, true);
	if (level && graph->nearest_vertex(level->id(), position, vertex_id, distance))
		return ::luabind::object(L, vertex_id);

	::luabind::object none(L);
	lua_pushnil(L);
	none.set();
	return none;
}

#pragma optimize("s",on)
void CGameGraph::script_register(lua_State* L)
{
	module(L)
	[
		class_<GameGraph::LEVEL_MAP::value_type>("GameGraph__LEVEL_MAP__value_type")
		.def_readonly("id", &GameGraph::LEVEL_MAP::value_type::first)
		.def_readonly("level", &GameGraph::LEVEL_MAP::value_type::second),

		def("game_graph", &get_game_graph),

		class_<CGameGraph>("CGameGraph")
		.def("accessible", &get_accessible1)
		.def("accessible", &get_accessible2)
		.def("valid_vertex_id", &CGameGraph::valid_vertex_id)
		.def("vertex", &CGameGraph::vertex)
		.def("vertex_id", &CGameGraph::vertex_id)
		.def("levels", &get_levels, return_stl_iterator)
		.def("nearest_vertex", &get_nearest_vertex, raw<1>()),

		class_<CVertex>("GameGraph__CVertex")
		.def("level_point", &CVertex__level_point)
		.def("game_point", &CVertex__game_point)
		.def("level_id", &CVertex::level_id)
		.def("level_vertex_id", &CVertex::level_vertex_id)
	];
}
