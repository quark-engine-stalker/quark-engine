////////////////////////////////////////////////////////////////////////////
//	Module 		: object_saver.h
//	Created 	: 21.01.2003
//  Modified 	: 09.07.2004
//	Author		: Dmitriy Iassenev
//	Description : Object saver
////////////////////////////////////////////////////////////////////////////

#pragma once

template <class M, typename P>
struct CSaver
{
	IC static void save_data(LPSTR data, M& stream, const P&)
	{
		stream.w_stringZ(data);
	}

	IC static void save_data(LPCSTR data, M& stream, const P&)
	{
		stream.w_stringZ(data);
	}

	IC static void save_data(const shared_str& data, M& stream, const P&)
	{
		stream.w_stringZ(data);
	}

	IC static void save_data(const xr_string& data, M& stream, const P&)
	{
		stream.w_stringZ(data.c_str());
	}

	template <typename T1, typename T2>
	IC static void save_data(const std::pair<T1, T2>& data, M& stream, const P& p)
	{
		if (p(data, data.first, true))
			CSaver<M, P>::save_data(data.first, stream, p);
		if (p(data, data.second, false))
			CSaver<M, P>::save_data(data.second, stream, p);
	}

	IC static void save_data(const xr_vector<bool>& data, M& stream, const P&)
	{
		stream.w_u32(static_cast<u32>(data.size()));
		auto I = data.begin();
		const auto E = data.end();
		u32 mask = 0;
		if (I != E)
		{
			for (int j = 0; I != E; ++I, ++j)
			{
				if (j >= 32)
				{
					stream.w_u32(mask);
					mask = 0;
					j = 0;
				}
				if (*I)
					mask |= u32(1) << j;
			}
			stream.w_u32(mask);
		}
	}

	template <typename T, int size>
	IC static void save_data(const svector<T, size>& data, M& stream, const P& p)
	{
		stream.w_u32(static_cast<u32>(data.size()));
		for (auto I = data.begin(), E = data.end(); I != E; ++I)
			if (p(data, *I))
				CSaver<M, P>::save_data(*I, stream, p);
	}

	template <typename T1, typename T2>
	IC static void save_data(const std::queue<T1, T2>& data, M& stream, const P& p)
	{
		std::queue<T1, T2> temp = data;
		stream.w_u32(static_cast<u32>(data.size()));
		for (; !temp.empty(); temp.pop())
			if (p(temp, temp.front()))
				CSaver<M, P>::save_data(temp.front(), stream, p);
	}

	template <template <typename, typename> class T1, typename T2, typename T3>
	IC static void save_data(const T1<T2, T3>& data, M& stream, const P& p, bool)
	{
		T1<T2, T3> temp = data;
		stream.w_u32(static_cast<u32>(data.size()));
		for (; !temp.empty(); temp.pop())
			if (p(temp, temp.top()))
				CSaver<M, P>::save_data(temp.top(), stream, p);
	}

	template <template <typename, typename, typename> class T1, typename T2, typename T3, typename T4>
	IC static void save_data(const T1<T2, T3, T4>& data, M& stream, const P& p, bool)
	{
		T1<T2, T3, T4> temp = data;
		stream.w_u32(static_cast<u32>(data.size()));
		for (; !temp.empty(); temp.pop())
			if (p(temp, temp.top()))
				CSaver<M, P>::save_data(temp.top(), stream, p);
	}

	template <typename T1, typename T2>
	IC static void save_data(const xr_stack<T1, T2>& data, M& stream, const P& p)
	{
		save_data(data, stream, p, true);
	}

	template <typename T1, typename T2, typename T3>
	IC static void save_data(const std::priority_queue<T1, T2, T3>& data, M& stream, const P& p)
	{
		save_data(data, stream, p, true);
	}

	template <typename T>
	IC static void save_data(const T& data, M& stream, const P& p)
	{
		if constexpr (object_type_traits::is_stl_container<T>::value)
		{
			stream.w_u32(static_cast<u32>(data.size()));
			for (auto I = data.begin(), E = data.end(); I != E; ++I)
				if (p(data, *I))
					CSaver<M, P>::save_data(*I, stream, p);
		}
		else if constexpr (std::is_pointer_v<T>)
		{
			CSaver<M, P>::save_data(*data, stream, p);
		}
		else if constexpr (std::is_base_of_v<IPureSavableObject<M>, T>)
		{
			const_cast<T&>(data).save(stream);
		}
		else
		{
			static_assert(!std::is_polymorphic_v<T>, "Cannot save polymorphic classes as binary data");
			stream.w(&data, sizeof(T));
		}
	}
};

namespace object_saver
{
	namespace detail
	{
		struct CEmptyPredicate
		{
			template <typename T1, typename T2>
			IC bool operator()(const T1&, const T2&) const { return (true); }

			template <typename T1, typename T2>
			IC bool operator()(const T1&, const T2&, bool) const { return (true); }
		};
	};
};

template <typename T, typename M, typename P>
IC void save_data(const T& data, M& stream, const P& p)
{
	CSaver<M, P>::save_data(data, stream, p);
}

template <typename T, typename M>
IC void save_data(const T& data, M& stream)
{
	save_data(data, stream, object_saver::detail::CEmptyPredicate());
}
