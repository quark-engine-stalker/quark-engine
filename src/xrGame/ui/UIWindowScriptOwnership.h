#pragma once

#include <luabind/detail/policy.hpp>
#include <luabind/detail/object_rep.hpp>

// A window constructed by a script is owned by its Lua object. A parent also
// destroys its AutoDelete children, so such a child must stop being Lua-owned,
// otherwise the Lua finalizer and the parent delete it twice (GAMMA's options
// keybind page crashed this way when the script VM was closed on save load).
// Anomaly binds AttachChild with luabind::adopt; here ownership moves only when
// the parent really deletes the window. Engine-created windows are not owned by
// Lua and are accepted unchanged, and non-AutoDelete children stay Lua-owned:
// they detach themselves from the parent when Lua destroys them.
namespace ui_script_ownership
{
inline void release_lua_ownership(lua_State* L, int index)
{
	using luabind::detail::object_rep;
	object_rep* rep = luabind::detail::is_class_object(L, index);
	if (rep && (rep->flags() & object_rep::owner))
		rep->remove_ownership();
}

// Child argument of AttachChild.
struct attach_child_converter : luabind::detail::pointer_converter<luabind::detail::Direction::lua_to_cpp>
{
	template <class T>
	T* apply(lua_State* L, luabind::detail::by_pointer<T> type, int index)
	{
		T* window = pointer_converter::apply(L, type, index);
		if (window && window->IsAutoDelete())
			release_lua_ownership(L, index);
		return window;
	}
};

// Self argument of SetAutoDelete: the new flag is the next Lua argument.
struct set_auto_delete_converter : luabind::detail::pointer_converter<luabind::detail::Direction::lua_to_cpp>
{
	template <class T>
	T* apply(lua_State* L, luabind::detail::by_pointer<T> type, int index)
	{
		T* window = pointer_converter::apply(L, type, index);
		if (window && window->GetParent() && lua_toboolean(L, index + 1))
			release_lua_ownership(L, index);
		return window;
	}
};

// Bound as a free function: luabind converts the self argument of a member
// function without consulting policies.
inline void set_auto_delete(CUIWindow* window, bool value)
{
	if (window)
		window->SetAutoDelete(value);
}

template <size_t N, class Converter>
struct policy : luabind::conversion_policy<N>
{
	static void precall(lua_State*, const luabind::index_map&) {}
	static void postcall(lua_State*, const luabind::index_map&) {}

	template <class T, luabind::detail::Direction Dir>
	struct generate_converter
	{
		using type = Converter;
	};
};

inline luabind::detail::policy_cons<policy<2, attach_child_converter>> attach_child_policy() { return {}; }
inline luabind::detail::policy_cons<policy<1, set_auto_delete_converter>> set_auto_delete_policy() { return {}; }
}
