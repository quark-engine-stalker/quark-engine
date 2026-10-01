// Optional identity cache for non-owning C++ objects exposed to Lua.
#pragma once

struct lua_State;

namespace luabind
{
    // The default keeps historical luabind behaviour. Engine object types may
    // specialize this trait when they own a lifetime-safe weak back-reference
    // to their userdata wrapper.
    template <class T>
    struct identity_back_reference
    {
        static bool extract(lua_State*, const T*) { return false; }
        static void store(lua_State*, const T*, int) {}
    };
}
