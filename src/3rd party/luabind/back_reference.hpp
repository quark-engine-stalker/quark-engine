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

#pragma once

#include <luabind/lua_include.hpp>
#include <luabind/wrapper_base.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace luabind {
    template<class T>
    T* get_pointer(T& ref)
    {
        return &ref;
    }

    namespace detail {

    // Opt-in for polymorphic T without virtual bases anywhere in the hierarchies of
    // its dynamic types: then T's vfptr is at offset 0 and the T* -> wrap_base* offset
    // depends only on that vfptr, so dynamic_cast is done once per vtable.
    template<class T>
    struct cache_wrap_base_cast : std::false_type {};

    template<class T>
    struct extract_wrap_base
    {
       static wrap_base const* extract(T const* ptr)
        {
            return extract(const_cast<T*>(ptr));
        }

        static wrap_base* extract(T* ptr)
        {
            if constexpr (!cache_wrap_base_cast<T>::value)
                return dynamic_cast<wrap_base*>(get_pointer(*ptr));
            else
            {
                // Lua thread only, like every push to Lua. A collision just recomputes.
                struct slot { const void* vtable; std::ptrdiff_t offset; };
                static constexpr std::ptrdiff_t not_wrapped = PTRDIFF_MIN;
                static slot slots[256];

                const void* vtable = *reinterpret_cast<const void* const*>(ptr);
                slot& s = slots[(reinterpret_cast<std::uintptr_t>(vtable) >> 4) & 255];
                if (s.vtable != vtable)
                {
                    wrap_base* wrapped = dynamic_cast<wrap_base*>(ptr);
                    s.offset = wrapped ? reinterpret_cast<char*>(wrapped) - reinterpret_cast<char*>(ptr) : not_wrapped;
                    s.vtable = vtable;
                }
                return s.offset == not_wrapped ? nullptr : reinterpret_cast<wrap_base*>(reinterpret_cast<char*>(ptr) + s.offset);
            }
        }
    };

    struct default_back_reference {};

    template<class T>
    struct back_reference_impl : default_back_reference
    {
        static bool extract(lua_State* L, T const* ptr)
        {
            if (!has_wrapper) return false;

            if (wrap_base const* p = extract_wrap_base<T>::extract(ptr))
            {
                wrap_access::ref(*p).get(L);
                return true;
            }

            return false;
        }

        static bool move(lua_State* L, T* ptr)
        {
            if (!has_wrapper) return false;

            if (wrap_base* p = extract_wrap_base<T>::extract(ptr))
            {
                assert(wrap_access::ref(*p).m_strong_ref.is_valid());
                wrap_access::ref(*p).get(L);
                wrap_access::ref(*p).m_strong_ref.reset();
                return true;
            }

            return false;
        }

        static bool has_wrapper;
    };

    template<class T>
    bool back_reference_impl<T>::has_wrapper = false;

    template<class T>
    struct back_reference_do_nothing
    {
        static bool extract(lua_State*, T const*)
        {
            return false;
        }

        static bool move(lua_State*, T*)
        {
            return false;
        }
    };

    } // namespace detail

#ifndef LUABIND_NO_RTTI

    template<class T>
    struct back_reference
        : std::conditional_t<std::is_polymorphic_v<T>,
            detail::back_reference_impl<T>,
            detail::back_reference_do_nothing<T>
        >
    {
    };

#else

    template<class T>
    struct back_reference
        : detail::back_reference_do_nothing<T>
    {
    };

#endif

} // namespace luabind
