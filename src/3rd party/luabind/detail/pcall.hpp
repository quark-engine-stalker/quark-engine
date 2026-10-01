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

#include <luabind/config.hpp>

struct lua_State;

namespace luabind { namespace detail
{
	using pcall_profile_begin_callback = void* (*)(lua_State* L, int function_index);
	using pcall_profile_end_callback = void (*)(void* context, int result);
	using lua_to_cpp_profile_begin_callback = void* (*)(lua_State* L, const char* owner_name, const char* function_name);
	using lua_to_cpp_profile_end_callback = void (*)(void* context);

	LUABIND_API void set_pcall_profile_callbacks(
		pcall_profile_begin_callback begin_callback,
		pcall_profile_end_callback end_callback);
	LUABIND_API void set_lua_to_cpp_profile_callbacks(
		lua_to_cpp_profile_begin_callback begin_callback,
		lua_to_cpp_profile_end_callback end_callback);
	LUABIND_API void begin_lua_to_cpp_profile_scope();
	LUABIND_API void end_lua_to_cpp_profile_scope();
	LUABIND_API void* begin_lua_to_cpp_profile(lua_State* L, const char* owner_name, const char* function_name);
	LUABIND_API void end_lua_to_cpp_profile(void* context);
	extern LUABIND_API thread_local unsigned int g_lua_to_cpp_profile_scope_depth;

	class lua_to_cpp_profile_scope
	{
	public:
		lua_to_cpp_profile_scope(lua_State* L, const char* owner_name, const char* function_name) :
			m_context(g_lua_to_cpp_profile_scope_depth ?
				begin_lua_to_cpp_profile(L, owner_name, function_name) : nullptr)
		{
		}

		~lua_to_cpp_profile_scope()
		{
			if (m_context)
				end_lua_to_cpp_profile(m_context);
		}

		lua_to_cpp_profile_scope(const lua_to_cpp_profile_scope&) = delete;
		lua_to_cpp_profile_scope& operator=(const lua_to_cpp_profile_scope&) = delete;

	private:
		void* m_context;
	};
	LUABIND_API int pcall(lua_State *L, int nargs, int nresults);
	LUABIND_API int resume_impl(lua_State *L, int nargs, int nresults);
}}
