////////////////////////////////////////////////////////////////////////////
//	Module 		: object_cloner.h
//	Created 	: 13.07.2004
//  Modified 	: 13.07.2004
//	Author		: Dmitriy Iassenev
//	Description : Object cloner
////////////////////////////////////////////////////////////////////////////

#pragma once

struct CCloner
{
private:
	template <typename Container, typename Value, typename = void>
	struct has_push_back : std::false_type
	{
	};

	template <typename Container, typename Value>
	struct has_push_back<Container, Value,
		std::void_t<decltype(std::declval<Container&>().push_back(std::declval<Value>()))>> : std::true_type
	{
	};

	template <typename Container, typename Value>
	IC static void add(Container& data, Value& value)
	{
		if constexpr (has_push_back<Container, Value>::value)
			data.push_back(value);
		else
			data.insert(value);
	}

public:
	IC static void clone(LPCSTR source, LPCSTR& destination)
	{
		destination = source;
	}

	IC static void clone(LPSTR source, LPSTR& destination)
	{
		destination = xr_strdup(source);
	}

	IC static void clone(const shared_str& source, shared_str& destination)
	{
		destination = source;
	}

	template <typename T1, typename T2>
	IC static void clone(const std::pair<T1, T2>& source, std::pair<T1, T2>& destination)
	{
		using first_type = typename object_type_traits::remove_const<T1>::type;
		clone(const_cast<first_type&>(source.first), const_cast<first_type&>(destination.first));
		clone(source.second, destination.second);
	}

	template <typename T, int size>
	IC static void clone(const svector<T, size>& source, svector<T, size>& destination)
	{
		destination.resize(source.size());
		auto J = destination.begin();
		for (auto I = source.begin(), E = source.end(); I != E; ++I, ++J)
			clone(*I, *J);
	}

	template <typename T1, typename T2>
	IC static void clone(const std::queue<T1, T2>& source, std::queue<T1, T2>& destination)
	{
		std::queue<T1, T2> input = source;
		std::queue<T1, T2> ordered;
		for (; !input.empty(); input.pop())
			ordered.push(input.front());

		while (!destination.empty())
			destination.pop();

		for (; !ordered.empty(); ordered.pop())
		{
			typename std::queue<T1, T2>::value_type value{};
			CCloner::clone(ordered.front(), value);
			destination.push(value);
		}
	}

	template <template <typename, typename> class T1, typename T2, typename T3>
	IC static void clone(const T1<T2, T3>& source, T1<T2, T3>& destination, bool)
	{
		T1<T2, T3> input = source;
		T1<T2, T3> ordered;
		for (; !input.empty(); input.pop())
			ordered.push(input.top());

		while (!destination.empty())
			destination.pop();

		for (; !ordered.empty(); ordered.pop())
		{
			typename T1<T2, T3>::value_type value{};
			CCloner::clone(ordered.top(), value);
			destination.push(value);
		}
	}

	template <template <typename, typename, typename> class T1, typename T2, typename T3, typename T4>
	IC static void clone(const T1<T2, T3, T4>& source, T1<T2, T3, T4>& destination, bool)
	{
		T1<T2, T3, T4> input = source;
		T1<T2, T3, T4> ordered;
		for (; !input.empty(); input.pop())
			ordered.push(input.top());

		while (!destination.empty())
			destination.pop();

		for (; !ordered.empty(); ordered.pop())
		{
			typename T1<T2, T3, T4>::value_type value{};
			CCloner::clone(ordered.top(), value);
			destination.push(value);
		}
	}

	template <typename T1, typename T2>
	IC static void clone(const xr_stack<T1, T2>& source, xr_stack<T1, T2>& destination)
	{
		clone(source, destination, true);
	}

	template <typename T1, typename T2, typename T3>
	IC static void clone(const std::priority_queue<T1, T2, T3>& source,
		std::priority_queue<T1, T2, T3>& destination)
	{
		clone(source, destination, true);
	}

	template <typename T>
	IC static void clone(const T& source, T& destination)
	{
		if constexpr (object_type_traits::is_stl_container<T>::value)
		{
			destination.clear();
			for (auto I = source.begin(), E = source.end(); I != E; ++I)
			{
				typename T::value_type value{};
				CCloner::clone(*I, value);
				add(destination, value);
			}
		}
		else if constexpr (std::is_pointer_v<T>)
		{
			using pointee_type = typename object_type_traits::remove_pointer<T>::type;
			destination = xr_new<pointee_type>(*source);
			CCloner::clone(*source, *destination);
		}
		else
		{
			destination = source;
		}
	}
};

IC void clone(LPCSTR p0, LPSTR& p1)
{
	p1 = xr_strdup(p0);
}

template <typename T>
IC void clone(const T& p0, T& p1)
{
	CCloner::clone(p0, p1);
}
