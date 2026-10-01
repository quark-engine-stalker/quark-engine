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
#include "luabind_api.h"
#include <luabind/detail/pcall.hpp>
#include <luabind/error.hpp>
#include <luabind/lua_include.hpp>

namespace luabind { namespace detail
{
	namespace
	{
		pcall_profile_begin_callback g_profile_begin = nullptr;
		pcall_profile_end_callback g_profile_end = nullptr;
		lua_to_cpp_profile_begin_callback g_lua_to_cpp_profile_begin = nullptr;
		lua_to_cpp_profile_end_callback g_lua_to_cpp_profile_end = nullptr;

		// lua_pushcfunction() allocates a fresh LuaJIT GC closure. The old pcall
		// path did that for every protected call, creating continuous allocator/GC
		// pressure in callback-heavy gameplay and eventually making even the error
		// handler itself fail to allocate. Keep one closure per Lua state instead.
		char g_pcall_handler_registry_key;

		void push_pcall_handler(lua_State* L, pcall_callback_fun callback)
		{
			lua_pushlightuserdata(L, &g_pcall_handler_registry_key);
			lua_rawget(L, LUA_REGISTRYINDEX);
			if (lua_iscfunction(L, -1) && lua_tocfunction(L, -1) == callback)
				return;

			lua_pop(L, 1);
			lua_pushcfunction(L, callback);

			// Registry keys are lightuserdata, so refreshing the cache does not intern
			// a string. Comparing lua_tocfunction above also makes callback changes
			// (e.g. debugger attach/detach) self-invalidating.
			lua_pushlightuserdata(L, &g_pcall_handler_registry_key);
			lua_pushvalue(L, -2);
			lua_rawset(L, LUA_REGISTRYINDEX);
		}
	}

	thread_local unsigned int g_lua_to_cpp_profile_scope_depth = 0;

	void set_pcall_profile_callbacks(
		pcall_profile_begin_callback begin_callback,
		pcall_profile_end_callback end_callback)
	{
		g_profile_begin = begin_callback;
		g_profile_end = end_callback;
	}

	void set_lua_to_cpp_profile_callbacks(
		lua_to_cpp_profile_begin_callback begin_callback,
		lua_to_cpp_profile_end_callback end_callback)
	{
		g_lua_to_cpp_profile_begin = begin_callback;
		g_lua_to_cpp_profile_end = end_callback;
	}

	void begin_lua_to_cpp_profile_scope()
	{
		++g_lua_to_cpp_profile_scope_depth;
	}

	void end_lua_to_cpp_profile_scope()
	{
		if (g_lua_to_cpp_profile_scope_depth)
			--g_lua_to_cpp_profile_scope_depth;
	}

	void* begin_lua_to_cpp_profile(lua_State* L, const char* owner_name, const char* function_name)
	{
		return g_lua_to_cpp_profile_begin && g_lua_to_cpp_profile_end ?
			g_lua_to_cpp_profile_begin(L, owner_name, function_name) : nullptr;
	}

	void end_lua_to_cpp_profile(void* context)
	{
		if (context && g_lua_to_cpp_profile_end)
			g_lua_to_cpp_profile_end(context);
	}

	int pcall(lua_State *L, int nargs, int nresults)
	{
		void* profile_context = nullptr;
		pcall_profile_end_callback profile_end = nullptr;
		if (g_profile_begin && g_profile_end)
		{
			const int function_index = lua_gettop(L) - nargs;
			profile_context = g_profile_begin(L, function_index);
			profile_end = g_profile_end;
		}

		pcall_callback_fun e = get_pcall_callback();
		int en = 0;
		if ( e )
		{
			int base = lua_gettop(L) - nargs;
			push_pcall_handler(L, e);
			lua_insert(L, base);  // push pcall_callback under chunk and args
			en = base;
  		}
		int result = lua_pcall(L, nargs, nresults, en);
		if ( en )
			lua_remove(L, en);  // remove pcall_callback
		if (profile_end)
			profile_end(profile_context, result);
		return result;
	}

	int resume_impl(lua_State *L, int nargs, int)
	{
		void* profile_context = nullptr;
		pcall_profile_end_callback profile_end = nullptr;
		if (g_profile_begin && g_profile_end)
		{
			profile_context = g_profile_begin(L, 0);
			profile_end = g_profile_end;
		}

		const int result = lua_resume(L, nargs);
		if (profile_end)
			profile_end(profile_context, result);
		return result;
	}

}}
