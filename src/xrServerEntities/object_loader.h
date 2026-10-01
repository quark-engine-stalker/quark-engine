////////////////////////////////////////////////////////////////////////////
//	Module 		: object_loader.h
//	Created 	: 21.01.2003
//  Modified 	: 09.07.2004
//	Author		: Dmitriy Iassenev
//	Description : Object loader
////////////////////////////////////////////////////////////////////////////

#pragma once

template <class M, typename P>
struct CLoader
{
private:
	template <typename Container, typename = void>
	struct has_reserve : std::false_type
	{
	};

	template <typename Container>
	struct has_reserve<Container,
		std::void_t<decltype(std::declval<Container&>().reserve(std::declval<typename Container::size_type>()))>>
		: std::true_type
	{
	};

	template <typename Container, typename Value, typename = void>
	struct has_hint_insert : std::false_type
	{
	};

	template <typename Container, typename Value>
	struct has_hint_insert<Container, Value,
		std::void_t<decltype(std::declval<Container&>().insert(
			std::declval<typename Container::const_iterator>(), std::declval<Value>()))>> : std::true_type
	{
	};

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
		else if constexpr (has_hint_insert<Container, Value>::value)
			data.insert(data.end(), value);
		else
			data.insert(value);
	}

public:
	IC static void load_data(LPCSTR&, M&, const P&)
	{
		NODEFAULT;
	}

	IC static void load_data(LPSTR& data, M& stream, const P&)
	{
		shared_str value;
		stream.r_stringZ(value);
		data = xr_strdup(*value);
	}

	IC static void load_data(shared_str& data, M& stream, const P&)
	{
		stream.r_stringZ(data);
	}

	IC static void load_data(xr_string& data, M& stream, const P&)
	{
		shared_str value;
		stream.r_stringZ(value);
		data = *value;
	}

	template <typename T1, typename T2>
	IC static void load_data(std::pair<T1, T2>& data, M& stream, const P& p)
	{
		using first_type = typename object_type_traits::remove_const<T1>::type;
		first_type& first = const_cast<first_type&>(data.first);
		if (p(data, first, true))
		{
			constexpr bool is_string_pointer = object_type_traits::is_same<T1, LPCSTR>::value;
			VERIFY(!is_string_pointer);
			load_data(first, stream, p);
		}
		if (p(data, data.second, false))
			load_data(data.second, stream, p);
		p.after_load(data, stream);
	}

	IC static void load_data(xr_vector<bool>& data, M& stream, const P& p)
	{
		if (p.can_clear())
			data.clear();
		const u32 previous_count = data.size();
		data.resize(previous_count + stream.r_u32());
		auto I = data.begin() + previous_count;
		const auto E = data.end();
		u32 mask = 0;
		for (int j = 32; I != E; ++I, ++j)
		{
			if (j >= 32)
			{
				mask = stream.r_u32();
				j = 0;
			}
			*I = !!(mask & (u32(1) << j));
		}
	}

	template <typename T, int size>
	IC static void load_data(svector<T, size>& data, M& stream, const P& p)
	{
		if (p.can_clear())
			data.clear();
		const u32 count = stream.r_u32();
		for (u32 i = 0; i < count; ++i)
		{
			typename svector<T, size>::value_type temp{};
			CLoader<M, P>::load_data(temp, stream, p);
			if (p(data, temp))
				data.push_back(temp);
		}
	}

	template <typename T1, typename T2>
	IC static void load_data(std::queue<T1, T2>& data, M& stream, const P& p)
	{
		if (p.can_clear())
			while (!data.empty())
				data.pop();

		std::queue<T1, T2> temp;
		const u32 count = stream.r_u32();
		for (u32 i = 0; i < count; ++i)
		{
			typename std::queue<T1, T2>::value_type value{};
			CLoader<M, P>::load_data(value, stream, p);
			if (p(temp, value))
				temp.push(value);
		}
		for (; !temp.empty(); temp.pop())
			data.push(temp.front());
	}

	template <template <typename, typename> class T1, typename T2, typename T3>
	IC static void load_data(T1<T2, T3>& data, M& stream, const P& p, bool)
	{
		if (p.can_clear())
			while (!data.empty())
				data.pop();

		T1<T2, T3> temp;
		const u32 count = stream.r_u32();
		for (u32 i = 0; i < count; ++i)
		{
			typename T1<T2, T3>::value_type value{};
			CLoader<M, P>::load_data(value, stream, p);
			if (p(temp, value))
				temp.push(value);
		}
		for (; !temp.empty(); temp.pop())
			data.push(temp.top());
	}

	template <template <typename, typename, typename> class T1, typename T2, typename T3, typename T4>
	IC static void load_data(T1<T2, T3, T4>& data, M& stream, const P& p, bool)
	{
		if (p.can_clear())
			while (!data.empty())
				data.pop();

		T1<T2, T3, T4> temp;
		const u32 count = stream.r_u32();
		for (u32 i = 0; i < count; ++i)
		{
			typename T1<T2, T3, T4>::value_type value{};
			CLoader<M, P>::load_data(value, stream, p);
			if (p(temp, value))
				temp.push(value);
		}
		for (; !temp.empty(); temp.pop())
			data.push(temp.top());
	}

	template <typename T1, typename T2>
	IC static void load_data(xr_stack<T1, T2>& data, M& stream, const P& p)
	{
		load_data(data, stream, p, true);
	}

	template <typename T1, typename T2, typename T3>
	IC static void load_data(std::priority_queue<T1, T2, T3>& data, M& stream, const P& p)
	{
		load_data(data, stream, p, true);
	}

	template <typename T>
	IC static void load_data(T& data, M& stream, const P& p)
	{
		if constexpr (object_type_traits::is_stl_container<T>::value)
		{
			if (p.can_clear())
				data.clear();
			const u32 count = stream.r_u32();
			if constexpr (has_reserve<T>::value)
				data.reserve(data.size() + count);
			for (u32 i = 0; i < count; ++i)
			{
				typename T::value_type temp{};
				CLoader<M, P>::load_data(temp, stream, p);
				if (p(data, temp))
					add(data, temp);
			}
		}
		else if constexpr (std::is_pointer_v<T>)
		{
			using pointee_type = typename object_type_traits::remove_pointer<T>::type;
			data = xr_new<pointee_type>();
			CLoader<M, P>::load_data(*data, stream, p);
		}
		else if constexpr (std::is_base_of_v<IPureLoadableObject<M>, T>)
		{
			data.load(stream);
		}
		else
		{
			static_assert(!std::is_polymorphic_v<T>, "Cannot load polymorphic classes as binary data");
			stream.r(&data, sizeof(T));
		}
	}
};

namespace object_loader
{
	namespace detail
	{
		struct CEmptyPredicate
		{
			template <typename T1, typename T2>
			IC void after_load(T1&, T2&) const
			{
			}

			template <typename T1, typename T2>
			IC bool operator()(T1&, const T2&) const { return (true); }

			template <typename T1, typename T2>
			IC bool operator()(T1&, const T2&, bool) const { return (true); }

			IC bool can_clear() const { return (true); }
			IC bool can_add() const { return (true); }
		};
	};
};

template <typename T, typename M, typename P>
IC void load_data(const T& data, M& stream, const P& p)
{
	T* temp = const_cast<T*>(&data);
	CLoader<M, P>::load_data(*temp, stream, p);
}

template <typename T, typename M>
IC void load_data(const T& data, M& stream)
{
	load_data(data, stream, object_loader::detail::CEmptyPredicate());
}
