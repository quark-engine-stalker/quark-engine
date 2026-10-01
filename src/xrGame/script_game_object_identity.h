#pragma once

#include <luabind/identity_back_reference.hpp>

class CScriptGameObject;

// Kept in a lightweight header included by the xrGame PCH, so every template
// instantiation which pushes CScriptGameObject* sees the same specialization.
namespace luabind
{
	template <>
	struct identity_back_reference<CScriptGameObject>
	{
		static bool extract(lua_State* L, const CScriptGameObject* object);
		static void store(lua_State* L, const CScriptGameObject* object, int index);
	};
}
