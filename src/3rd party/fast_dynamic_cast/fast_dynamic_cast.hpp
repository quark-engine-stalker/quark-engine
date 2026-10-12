/**
 * Fast Dynamic Cast version 1.6
 * 
 * Copyright (c) 2016 tobspr <tobias.springer1@googlemail.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef FAST_DYNAMIC_CAST_H
#define FAST_DYNAMIC_CAST_H

// This determines whether the fast dynamic cast is used. You can disable this to
// check if the fast dynamic cast causes errors, or to see how slow the regular
// dynamic cast is.
#define FAST_DYNAMIC_CAST_ENABLED 1

// Whether the fast dynamic cast will be used from multiple threads. If so,
// set this to 1, otherwise disable it. With the multithreaded dcast, the
// performance is marginally slower.
#define DCAST_MULTITHREADED 1

// Magic value used to indicate that there is no cache entry for this particular
// conversion yet. Should be a fairly high value to avoid collisions.
#if _WIN64
  #define DCAST_NO_OFFSET 0x7FFFFFFFFFFFFFFFLL
#else 
  #define DCAST_NO_OFFSET 0x7FFFFFFLL
#endif

// Declare variables as thread local when using multithreaded dcast
#if DCAST_MULTITHREADED
  #define DCAST_THREADLOCAL __declspec(thread)
#else
  #define DCAST_THREADLOCAL
#endif

// Only works with Visual Studio 2013 and upwards (Could work in MSVC2010, but yet untested)
#if FAST_DYNAMIC_CAST_ENABLED && defined(_MSC_VER) && (_MSC_VER >= 1800)

// Include memory header for std::dynamic_pointer_cast. You can replace this
// with your own memory include.
#include <memory>

namespace fast_dcast
{
  // Pointer to complete object
  using class_obj_ptr = const uintptr_t*;

  // Pointer to __vftable
  using v_table_ptr = const uintptr_t*;

  template < typename _Ty >
  __forceinline v_table_ptr get_vtable(const _Ty* ptr)
  {
    // __vftable is at [ptr + 0]
    return reinterpret_cast<v_table_ptr>(*reinterpret_cast<class_obj_ptr>(ptr));
  }

  // Converts T&, const T&, volatile const T& etc to T, used for enable_if
  // Could also abuse std::decay for this.
  template < typename _Ty >
  using clean_type_t = typename std::remove_cv_t<std::remove_reference_t<std::remove_pointer_t< _Ty >>>;

  // fast dynamic cast for T*
  // Should be nothrow but this decreases performance. In practice this is not
  // an issue.
  template < typename _To, typename _From >
  __forceinline _To fast_dynamic_cast(_From* ptr)
  {
    // Not required, dynamic_cast already produces a compile-time error in this case
    // static_assert(std::is_polymorphic<_From>::value, "Type is not polymorphic!");

    if (!ptr)
      return nullptr;

    // Cache both successful and failed type checks. Nearby-object queries often
    // mix NPCs, inventory items and non-target objects; one positive entry kept
    // rerunning RTTI for every rejected object. Eight thread-owned entries retain
    // that small working set without object pointers or heap allocations.
    struct cache_entry
    {
      v_table_ptr vtable;
      ptrdiff_t offset; // DCAST_NO_OFFSET denotes a known failed conversion.
    };
    DCAST_THREADLOCAL static cache_entry cache[8] = {};
    DCAST_THREADLOCAL static unsigned next_entry = 0;
    DCAST_THREADLOCAL static v_table_ptr recent_vtable = nullptr;
    DCAST_THREADLOCAL static ptrdiff_t recent_offset = DCAST_NO_OFFSET;

    const v_table_ptr this_vtable = get_vtable(ptr);
    if (recent_offset != DCAST_NO_OFFSET && recent_vtable == this_vtable)
    {
      return reinterpret_cast<_To>(reinterpret_cast<char*>(ptr) + recent_offset);
    }
    if (recent_vtable == this_vtable)
      return nullptr;
    for (const cache_entry& entry : cache)
    {
      if (entry.vtable == this_vtable)
      {
        recent_vtable = this_vtable;
        recent_offset = entry.offset;
        if (entry.offset == DCAST_NO_OFFSET)
          return nullptr;
        return reinterpret_cast<_To>(reinterpret_cast<char*>(ptr) + entry.offset);
      }
    }

    // RTTI stays authoritative on misses, including cross-casts and ambiguity.
    auto result = dynamic_cast<_To>(ptr);
    cache_entry& entry = cache[next_entry];
    next_entry = (next_entry + 1u) & 7u;
    entry.vtable = this_vtable;
    entry.offset = result ? reinterpret_cast<const char*>(result) -
      reinterpret_cast<const char*>(ptr) : DCAST_NO_OFFSET;
    recent_vtable = this_vtable;
    recent_offset = entry.offset;
    return result;
  };

  // const T*
  template < typename _To, typename _From >
  __forceinline const _To fast_dynamic_cast(const _From* ptr)
  {
    _From* nonconst_ptr = const_cast<_From*>(ptr);
    _To casted_ptr = fast_dynamic_cast<_To>(nonconst_ptr);
    return const_cast<const _To>(casted_ptr);
  };

  // T&
  template < typename _To, typename _From, typename = std::enable_if_t<!std::is_same<clean_type_t<_To>, clean_type_t<_From&>>::value>>
  __forceinline _To fast_dynamic_cast(_From& ref)
  {
    using _ToPtr = std::add_pointer_t<std::remove_reference_t<_To>>;
    auto casted_ptr = fast_dynamic_cast<_ToPtr>(&ref);
    if (!casted_ptr)
      throw std::bad_cast {};
    return *casted_ptr;
  };

  // const T&
  template < typename _To, typename _From, typename = std::enable_if_t<!std::is_same<clean_type_t<_To>, clean_type_t<_From&>>::value>>
  __forceinline _To fast_dynamic_cast(const _From& ref)
  {
    using _ToPtr = std::add_pointer_t<std::remove_reference_t<_To>>;
    auto casted_ptr = fast_dynamic_cast<_ToPtr>(const_cast<_From*>(&ref));
    if (!casted_ptr)
      throw std::bad_cast {};
    return *casted_ptr;
  };

  // std::dynamic_pointer_cast
  template < typename _To, typename _From >
  __forceinline std::shared_ptr<_To> fast_dynamic_pointer_cast(const std::shared_ptr<_From>& ptr)
  {
    // Do not use std::move, since this might actually prevent RVO on MSVC 2015 and upwards
    return std::shared_ptr<_To>(ptr, fast_dynamic_cast<_To*>(ptr.get()));
  }

  // T -> T
  template < typename _To >
  __forceinline _To fast_dynamic_cast(_To ptr) { return ptr; }
}

using fast_dcast::fast_dynamic_cast;
using fast_dcast::fast_dynamic_pointer_cast;

#else

// When no fast dynamic cast is available, fall back to regular dynamic cast
#define fast_dynamic_cast dynamic_cast
#define fast_dynamic_pointer_cast std::dynamic_pointer_cast

#endif // FAST_DYNAMIC_CAST_ENABLED

#undef FAST_DYNAMIC_CAST_ENABLED
#undef DCAST_NO_OFFSET
#undef DCAST_CALLING_CONVENTION
#undef DCAST_MULTITHREADED
#undef DCAST_THREADLOCAL

#endif // FAST_DYNAMIC_CAST_H
