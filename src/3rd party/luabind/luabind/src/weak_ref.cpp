#include "luabind_api.h"
// Copyright (c) 2004 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.
#include <algorithm>

#include <luabind/lua_include.hpp>

#include <luabind/config.hpp>
#include <luabind/detail/ref.hpp>
#include <luabind/weak_ref.hpp>
#include <cassert>

namespace luabind {

// allocation code from lauxlib.c
/******************************************************************************
* Copyright (C) 1994-2003 Tecgraf, PUC-Rio.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/
    
    namespace {

        enum
        {
            freelist_ref = 1, count_ref = 2
        };
        
        void get_weak_table(lua_State* L)
        {
            lua_pushliteral(L, "__luabind_weak_refs");
            lua_gettable(L, LUA_REGISTRYINDEX);

            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                lua_newtable(L);
                // metatable
                lua_newtable(L);
                lua_pushliteral(L, "__mode");
                lua_pushliteral(L, "v");
                lua_rawset(L, -3);
                // set metatable
                lua_setmetatable(L, -2);
                lua_pushnumber(L, 0);
                lua_rawseti(L, -2, freelist_ref);
                lua_pushnumber(L, 2);
                lua_rawseti(L, -2, count_ref);

                lua_pushliteral(L, "__luabind_weak_refs");
                lua_pushvalue(L, -2);
                lua_settable(L, LUA_REGISTRYINDEX);
            }
        }

    } // namespace unnamed

    struct weak_ref::impl
    {
        impl(lua_State* s, int index)
            : count(0)
            , state(s)
            , ref(0)
            , vm_token(nullptr)
            , registry_identity(nullptr)
        {
            lua_pushvalue(s, index);
            get_weak_table(s);
            vm_token = detail::acquire_lua_vm_token(s);
            registry_identity = lua_topointer(s, -1);

            lua_rawgeti(s, -1, freelist_ref);
            ref = (int)lua_tonumber(s, -1);
            lua_pop(s, 1);

            if (ref == 0)
            {
                lua_rawgeti(s, -1, count_ref);
                ref = (int)lua_tonumber(s, -1) + 1;
                lua_pop(s, 1);
                lua_pushnumber(s, (lua_Number)ref);
                lua_rawseti(s, -2, count_ref);
            }
            else
            {
                lua_rawgeti(s, -1, ref);
                lua_rawseti(s, -2, freelist_ref);
            }

            lua_pushvalue(s, -2); // duplicate value
            lua_rawseti(s, -2, ref);
            lua_pop(s, 2); // pop weakref table and value
        }

        ~impl()
        {
            // invalidate() is called before the owning Lua VM is closed. The
            // C++ weak_ref may live longer than the VM, in which case there is
            // no Lua slot left to recycle.
            if (vm_token->alive)
            {
                get_weak_table(state);
                if (lua_topointer(state, -1) == registry_identity)
                {
                    lua_rawgeti(state, -1, freelist_ref);
                    lua_rawseti(state, -2, ref);
                    lua_pushnumber(state, (lua_Number)ref);
                    lua_rawseti(state, -2, freelist_ref);
                }
                lua_pop(state, 1);
            }

            detail::release_lua_vm_token(vm_token);
        }

        int count;
        lua_State* state;
        int ref;
        detail::lua_vm_token* vm_token;
        const void* registry_identity;
    };

    weak_ref::weak_ref()
        : m_impl(0)
    {
    }
    
    weak_ref::weak_ref(lua_State* L, int index)
        : m_impl(luabind_new<impl>(L, index))
    {
        m_impl->count = 1;
    }

    weak_ref::weak_ref(weak_ref const& other)
        : m_impl(other.m_impl)
    {
        if (m_impl) ++m_impl->count;
    }

    weak_ref::~weak_ref()
    {
        if (m_impl && --m_impl->count == 0)
        {
            luabind_delete	(m_impl);
        }
    }

    weak_ref& weak_ref::operator=(weak_ref const& other)
    {
        weak_ref(other).swap(*this);
        return *this;
    }

    void weak_ref::swap(weak_ref& other)
    {
        std::swap(m_impl, other.m_impl);
    }

    bool weak_ref::empty() const
    {
        return !m_impl || !m_impl->vm_token->alive;
    }

    void weak_ref::invalidate(lua_State* L)
    {
        assert(L);
        detail::invalidate_lua_vm(L);
    }

    int weak_ref::id() const
    {
		assert(m_impl && m_impl->state);
		return m_impl->ref;
    }

	// L may not be the same pointer as
	// was used when creating this reference
	// since it may be a thread that shares
	// the same globals table.
    void weak_ref::get(lua_State* L) const
    {
		assert(L);

        if (!m_impl || !m_impl->vm_token->alive)
        {
            lua_pushnil(L);
            return;
        }

        get_weak_table(L);
		if (lua_topointer(L, -1) != m_impl->registry_identity)
		{
			lua_pop(L, 1);
			lua_pushnil(L);
			return;
		}

        lua_rawgeti(L, -1, m_impl->ref);
        lua_remove(L, -2);
    }

    lua_State* weak_ref::state() const
    {
		assert(m_impl && m_impl->state);
        return m_impl->state;
    }
    
} // namespace luabind
