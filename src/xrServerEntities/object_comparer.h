////////////////////////////////////////////////////////////////////////////
//	Module 		: object_comparer.h
//	Created 	: 13.07.2004
//  Modified 	: 13.07.2004
//	Author		: Dmitriy Iassenev
//	Description : Object equality checker
////////////////////////////////////////////////////////////////////////////

#pragma once

template <typename P>
struct CComparer
{
	IC static bool compare(LPCSTR first, LPCSTR second, const P& p)
	{
		return p(first, second);
	}

	IC static bool compare(LPSTR first, LPSTR second, const P& p)
	{
		return p(first, second);
	}

	IC static bool compare(const shared_str& first, const shared_str& second, const P& p)
	{
		return p(first, second);
	}

	template <typename T1, typename T2>
	IC static bool compare(const std::pair<T1, T2>& first, const std::pair<T1, T2>& second, const P& p)
	{
		return compare(first.first, second.first, p) && compare(first.second, second.second, p);
	}

	template <typename T, int size>
	IC static bool compare(const svector<T, size>& first, const svector<T, size>& second, const P& p)
	{
		if (first.size() != second.size())
			return p();

		auto I = first.begin();
		auto J = second.begin();
		const auto E = first.end();
		for (; I != E; ++I, ++J)
			if (!compare(*I, *J, p))
				return false;
		return true;
	}

	template <typename T1, typename T2>
	IC static bool compare(const std::queue<T1, T2>& source_first, const std::queue<T1, T2>& source_second,
		const P& p)
	{
		std::queue<T1, T2> first = source_first;
		std::queue<T1, T2> second = source_second;
		if (first.size() != second.size())
			return p();

		for (; !first.empty(); first.pop(), second.pop())
			if (!compare(first.front(), second.front(), p))
				return false;
		return true;
	}

	template <template <typename, typename> class T1, typename T2, typename T3>
	IC static bool compare(const T1<T2, T3>& source_first, const T1<T2, T3>& source_second, const P& p, bool)
	{
		T1<T2, T3> first = source_first;
		T1<T2, T3> second = source_second;
		if (first.size() != second.size())
			return p();

		for (; !first.empty(); first.pop(), second.pop())
			if (!compare(first.top(), second.top(), p))
				return false;
		return true;
	}

	template <template <typename, typename, typename> class T1, typename T2, typename T3, typename T4>
	IC static bool compare(const T1<T2, T3, T4>& source_first, const T1<T2, T3, T4>& source_second,
		const P& p, bool)
	{
		T1<T2, T3, T4> first = source_first;
		T1<T2, T3, T4> second = source_second;
		if (first.size() != second.size())
			return p();

		for (; !first.empty(); first.pop(), second.pop())
			if (!compare(first.top(), second.top(), p))
				return false;
		return true;
	}

	template <typename T1, typename T2>
	IC static bool compare(const xr_stack<T1, T2>& first, const xr_stack<T1, T2>& second, const P& p)
	{
		return compare(first, second, p, true);
	}

	template <typename T1, typename T2, typename T3>
	IC static bool compare(const std::priority_queue<T1, T2, T3>& first,
		const std::priority_queue<T1, T2, T3>& second, const P& p)
	{
		return compare(first, second, p, true);
	}

	template <typename T>
	IC static bool compare(const T& first, const T& second, const P& p)
	{
		if constexpr (object_type_traits::is_stl_container<T>::value)
		{
			if (first.size() != second.size())
				return p();

			auto I = first.begin();
			auto J = second.begin();
			const auto E = first.end();
			for (; I != E; ++I, ++J)
				if (!CComparer<P>::compare(*I, *J, p))
					return false;
			return true;
		}
		else if constexpr (std::is_pointer_v<T>)
		{
			return CComparer<P>::compare(*first, *second, p);
		}
		else
		{
			return p(first, second);
		}
	}
};

template <typename P>
IC bool compare(LPCSTR p0, LPSTR p1, const P& p)
{
	return (p(p0, p1));
}

template <typename P>
IC bool compare(LPSTR p0, LPCSTR p1, const P& p)
{
	return (p(p0, p1));
}

template <typename T, typename P>
IC bool compare(const T& p0, const T& p1, const P& p)
{
	return (CComparer<P>::compare(p0, p1, p));
}

namespace object_comparer
{
	namespace detail
	{
		template <template <typename _1> class P>
		struct comparer
		{
			template <typename T>
			IC bool operator()(const T& _1, const T& _2) const { return (P<T>()(_1, _2)); }

			IC bool operator()() const { return (P<bool>()(false, true)); }
			IC bool operator()(LPCSTR _1, LPCSTR _2) const { return (P<int>()(xr_strcmp(_1, _2), 0)); }
			IC bool operator()(LPSTR _1, LPSTR _2) const { return (P<int>()(xr_strcmp(_1, _2), 0)); }
			IC bool operator()(LPCSTR _1, LPSTR _2) const { return (P<int>()(xr_strcmp(_1, _2), 0)); }
			IC bool operator()(LPSTR _1, LPCSTR _2) const { return (P<int>()(xr_strcmp(_1, _2), 0)); }
		};
	};
};

#define declare_comparer(a,b) \
	template <typename T1, typename T2>\
	IC	bool a(const T1 &p0, const T2 &p1)\
	{\
		return			(compare(p0,p1,object_comparer::detail::comparer<b>()));\
	}

declare_comparer(equal, std::equal_to);
declare_comparer(greater_equal, std::greater_equal);
declare_comparer(greater, std::greater);
declare_comparer(less_equal, std::less_equal);
declare_comparer(less, std::less);
declare_comparer(not_equal, std::not_equal_to);
declare_comparer(logical_and, std::logical_and);
declare_comparer(logical_or, std::logical_or);
