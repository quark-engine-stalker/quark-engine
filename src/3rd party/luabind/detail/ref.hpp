// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

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

#pragma once

#include <cassert>
#include <algorithm>

#include <luabind/config.hpp>
#include <luabind/lua_include.hpp>

struct lua_State;

namespace luabind { namespace detail
{

	int LUABIND_API ref(lua_State *L);
	void LUABIND_API unref(lua_State *L, int ref);

	struct lua_vm_token
	{
		lua_vm_token()
			: count(1)
			, alive(true)
		{
		}

		int count;
		bool alive;
	};

	lua_vm_token* LUABIND_API acquire_lua_vm_token(lua_State* L);
	void LUABIND_API retain_lua_vm_token(lua_vm_token* token);
	void LUABIND_API release_lua_vm_token(lua_vm_token* token);
	void LUABIND_API invalidate_lua_vm(lua_State* L);

	inline void getref(lua_State* L, int r)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, r);
	}

	struct lua_reference
	{
		lua_reference(lua_State* L_ = 0)
			: L(L_)
			, m_ref(LUA_NOREF)
			, m_vm_token(nullptr)
		{}
		lua_reference(lua_reference const& r)
			: L(r.L)
			, m_ref(LUA_NOREF)
			, m_vm_token(nullptr)
		{
			if (!r.is_valid()) return;
			m_vm_token = r.m_vm_token;
			retain_lua_vm_token(m_vm_token);
			r.get(L);
			m_ref = ref(L);
		}
		~lua_reference() { reset(); }

		lua_State* state() const { return L; }

		void operator=(lua_reference const& r)
		{
			if (this == &r) return;
			reset();
			L = r.L;
			if (!r.is_valid()) return;
			m_vm_token = r.m_vm_token;
			retain_lua_vm_token(m_vm_token);
			r.get(L);
			m_ref = ref(L);
		}

		bool is_valid() const
		{ return m_ref != LUA_NOREF && m_vm_token && m_vm_token->alive; }

		void set(lua_State* L_)
		{
			reset();
			L = L_;
			m_vm_token = acquire_lua_vm_token(L);
			m_ref = ref(L);
		}

		void replace(lua_State* L_)
		{
			assert(is_valid());
			lua_rawseti(L_, LUA_REGISTRYINDEX, m_ref);
		}

		// L may not be the same pointer as
		// was used when creating this reference
		// since it may be a thread that shares
		// the same globals table.
		void get(lua_State* L_) const
		{
			assert(is_valid());
			assert(L_);
			if (!is_valid() || !L_)
				return;
			getref(L_, m_ref);
		}

		void reset()
		{
			if (m_vm_token)
			{
				if (m_vm_token->alive && L && m_ref != LUA_NOREF)
					unref(L, m_ref);
				release_lua_vm_token(m_vm_token);
				m_vm_token = nullptr;
			}
			m_ref = LUA_NOREF;
		}

		void swap(lua_reference& r)
		{
			assert(r.L == L);
			std::swap(r.m_ref, m_ref);
			std::swap(r.m_vm_token, m_vm_token);
		}

	private:
		lua_State* L;
		int m_ref;
		lua_vm_token* m_vm_token;
	};

}}
