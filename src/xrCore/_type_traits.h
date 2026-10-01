#ifndef _STL_EXT_type_traits
#define _STL_EXT_type_traits
#pragma once

#include <type_traits>

template <typename T>
struct is_class
{
    enum { result = std::is_class_v<T> };
};

template <typename T>
struct is_pm_class
{
    enum { result = std::is_polymorphic_v<T> };
};

template <bool is_class_type>
struct is_pm_classify
{
    template <typename T>
    struct _detail
    {
        enum { result = is_pm_class<T>::result };
    };
};

template <>
struct is_pm_classify<false>
{
    template <typename T>
    struct _detail
    {
        enum { result = false };
    };
};

template <typename T>
struct is_polymorphic
{
    enum { result = std::is_polymorphic_v<T> };
};

#endif
