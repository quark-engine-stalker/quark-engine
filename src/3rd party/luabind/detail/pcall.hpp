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

	// Called before every protected call into Lua.
	LUABIND_API void set_pcall_gate(void (*gate)(lua_State* L));
	// Called inside every catch(...) of a C++ function bound to Lua: with /EHa it also catches access
	// violations, which become a Lua error ("Busy Hands"). where names the bound function.
	LUABIND_API void set_caught_exception_callback(void (*callback)(const char* where));
	LUABIND_API void report_caught_exception(const char* where);

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
	// QUARK: told of every call the host makes into Lua (pcall, resume_impl and the host's own lua_resume through
	// observed_lua_call), on the calling thread, enter before and leave after; set it null for nothing to be called.
	struct lua_call_observer
	{
		void (*enter)(lua_State* L);
		void (*leave)(lua_State* L);
	};
	extern LUABIND_API const lua_call_observer* g_lua_call_observer;
	LUABIND_API void set_lua_call_observer(const lua_call_observer* observer);

	class observed_lua_call
	{
	public:
		explicit observed_lua_call(lua_State* L) : m_observer(g_lua_call_observer), m_L(L)
		{
			if (m_observer)
				m_observer->enter(m_L);
		}

		~observed_lua_call()
		{
			if (m_observer)
				m_observer->leave(m_L);
		}

		observed_lua_call(const observed_lua_call&) = delete;
		observed_lua_call& operator=(const observed_lua_call&) = delete;

	private:
		const lua_call_observer* const m_observer; // the one told of the enter is told of the leave
		lua_State* const m_L;
	};

	LUABIND_API int pcall(lua_State *L, int nargs, int nresults);
	LUABIND_API int resume_impl(lua_State *L, int nargs, int nresults);
	// The calling thread runs Lua now (inside a pcall or resume): error records may walk the VM's stack.
	LUABIND_API bool lua_running_on_this_thread();
}}
