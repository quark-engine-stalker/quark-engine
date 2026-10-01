#ifndef _STL_EXT_internal
#define _STL_EXT_internal

using std::swap;

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "_type_traits.h"

#ifdef __BORLANDC__
#define M_NOSTDCONTAINERS_EXT
#endif
#ifdef _M_AMD64
#define M_DONTDEFERCLEAR_EXT
#endif

#define M_DONTDEFERCLEAR_EXT //. for mem-debug only

//--------
#ifdef M_NOSTDCONTAINERS_EXT

#define xr_list std::list
#define xr_deque std::deque
#define xr_stack std::stack
#define xr_set std::set
#define xr_multiset std::multiset
#define xr_map std::map
#define xr_hash_map std::unordered_map
#define xr_multimap std::multimap
#define xr_string std::string

template <class T>
class xr_vector : public std::vector<T>
{
private:
	using inherited = std::vector<T>;

public:
	using size_type = typename inherited::size_type;
	using reference = typename inherited::reference;
	using const_reference = typename inherited::const_reference;
	using allocator_type = typename inherited::allocator_type;
	using inherited::inherited;

	xr_vector() = default;
	void clear() { inherited::clear(); }
	void clear_and_free()
	{
		inherited empty(this->get_allocator());
		inherited::swap(empty);
	}
	void clear_not_free() { inherited::clear(); }
	ICF const_reference operator[](size_type position) const
	{
		VERIFY(position < inherited::size());
		return inherited::operator[](position);
	}
	ICF reference operator[](size_type position)
	{
		VERIFY(position < inherited::size());
		return inherited::operator[](position);
	}
};

template <>
class xr_vector<bool> : public std::vector<bool>
{
private:
	using inherited = std::vector<bool>;

public:
	using size_type = typename inherited::size_type;
	using reference = typename inherited::reference;
	using const_reference = typename inherited::const_reference;
	using allocator_type = typename inherited::allocator_type;
	using inherited::inherited;

	xr_vector() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
	void clear() { inherited::clear(); }
};

#else

template <class T>
class xalloc
{
public:
	typedef size_t size_type;
	typedef ptrdiff_t difference_type;
	typedef T* pointer;
	typedef const T* const_pointer;
	typedef T& reference;
	typedef const T& const_reference;
	typedef T value_type;

public:
	template <class _Other>
	struct rebind
	{
		typedef xalloc<_Other> other;
	};

public:
	pointer address(reference _Val) const { return (&_Val); }
	const_pointer address(const_reference _Val) const { return (&_Val); }

	xalloc()
	{
	}

	xalloc(const xalloc<T>&)
	{
	}

	template <class _Other>
	xalloc(const xalloc<_Other>&)
	{
	}

	template <class _Other>
	xalloc<T>& operator=(const xalloc<_Other>&) { return (*this); }

	// Preserve the engine allocator's zero-filled storage contract. A number of
	// legacy and mod-facing paths historically rely on it for reserved slots.
	pointer allocate(size_type n, const void* = nullptr) const { return xr_alloc<T>((u32)n); }
	char* _charalloc(size_type n) { return (char*)allocate(n); }
	void deallocate(pointer p, size_type) const { xr_free(p); }
	void deallocate(void* p, size_type) const { xr_free(p); }
	void construct(pointer p, const T& _Val) { ::new((void*)p) T(_Val); }
	void destroy(pointer p) { p->~value_type(); }

	size_type max_size() const
	{
		size_type _Count = (size_type)(-1) / sizeof(T);
		return (0 < _Count ? _Count : 1);
	}
};

struct xr_allocator
{
	template <typename T>
	struct helper
	{
		typedef xalloc<T> result;
	};

	static void* alloc(const u32& n) { return xr_malloc((u32)n); }

	template <typename T>
	static void dealloc(T*& p) { xr_free(p); }
};

template <class _Ty, class _Other>
inline bool operator==(const xalloc<_Ty>&, const xalloc<_Other>&) { return (true); }

template <class _Ty, class _Other>
inline bool operator!=(const xalloc<_Ty>&, const xalloc<_Other>&) { return (false); }

// string(char)
typedef std::basic_string<char, std::char_traits<char>, xalloc<char>> xr_string;

// vector
template <typename T, typename allocator = xalloc<T>>
class xr_vector : public std::vector<T, allocator>
{
private:
	using inherited = std::vector<T, allocator>;

public:
	using allocator_type = allocator;
	using size_type = typename inherited::size_type;
	using reference = typename inherited::reference;
	using const_reference = typename inherited::const_reference;
	using inherited::inherited;

	xr_vector() = default;

	u32 size() const { return static_cast<u32>(inherited::size()); }

	void clear_and_free()
	{
		inherited empty(this->get_allocator());
		inherited::swap(empty);
	}
	void clear_not_free() { inherited::clear(); }

	void clear_and_reserve()
	{
		// Hot frame/update containers use their previous high-water capacity as a
		// RAM-backed cache. Shrinking here makes a temporary quiet frame pay again
		// when the workload returns and puts allocator traffic on the main thread.
		// Explicit unload/compaction sites still use clear_and_free().
		clear_not_free();
	}

	// Normal clear keeps capacity for hot-path reuse. Call clear_and_free()
	// explicitly at unload/compaction boundaries when the backing allocation
	// should be returned to the allocator.
	void clear() { clear_not_free(); }

	const_reference operator[](size_type position) const
	{
		VERIFY2(position < inherited::size(),
			make_string("index is out of range: index requested[%zu], size of container[%zu]",
				static_cast<size_t>(position), static_cast<size_t>(inherited::size())).c_str());
		return inherited::operator[](position);
	}

	reference operator[](size_type position)
	{
		VERIFY2(position < inherited::size(),
			make_string("index is out of range: index requested[%zu], size of container[%zu]",
				static_cast<size_t>(position), static_cast<size_t>(inherited::size())).c_str());
		return inherited::operator[](position);
	}
};

// vector<bool>
template <>
class xr_vector<bool, xalloc<bool>> : public std::vector<bool, xalloc<bool>>
{
private:
	using inherited = std::vector<bool, xalloc<bool>>;

public:
	using allocator_type = xalloc<bool>;
	using size_type = typename inherited::size_type;
	using reference = typename inherited::reference;
	using const_reference = typename inherited::const_reference;
	using inherited::inherited;

	xr_vector() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
	void clear() { inherited::clear(); }
};

template <typename allocator>
class xr_vector<bool, allocator> : public std::vector<bool, allocator>
{
private:
	using inherited = std::vector<bool, allocator>;

public:
	using allocator_type = allocator;
	using size_type = typename inherited::size_type;
	using reference = typename inherited::reference;
	using const_reference = typename inherited::const_reference;
	using inherited::inherited;

	xr_vector() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
	void clear() { inherited::clear(); }
};

// deque
template <typename T, typename allocator = xalloc<T>>
class xr_deque : public std::deque<T, allocator>
{
private:
	using inherited = std::deque<T, allocator>;

public:
	using allocator_type = allocator;
	using value_type = typename inherited::value_type;
	using size_type = typename inherited::size_type;
	using inherited::inherited;

	xr_deque() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
};

// stack
template <typename _Ty, class _C = xr_vector<_Ty>>
class xr_stack
{
public:
	typedef typename _C::allocator_type allocator_type;
	typedef typename allocator_type::value_type value_type;
	typedef typename allocator_type::size_type size_type;

	//explicit stack(const allocator_type& _Al = allocator_type()) : c(_Al) {}
	allocator_type get_allocator() const { return (c.get_allocator()); }
	bool empty() const { return (c.empty()); }
	u32 size() const { return c.size(); }
	value_type& top() { return (c.back()); }
	const value_type& top() const { return (c.back()); }
	void push(const value_type& _X) { c.push_back(_X); }
	void pop() { c.pop_back(); }
	bool operator==(const xr_stack<_Ty, _C>& _X) const { return (c == _X.c); }
	bool operator!=(const xr_stack<_Ty, _C>& _X) const { return (!(*this == _X)); }
	bool operator<(const xr_stack<_Ty, _C>& _X) const { return (c < _X.c); }
	bool operator>(const xr_stack<_Ty, _C>& _X) const { return (_X < *this); }
	bool operator<=(const xr_stack<_Ty, _C>& _X) const { return (!(_X < *this)); }
	bool operator>=(const xr_stack<_Ty, _C>& _X) const { return (!(*this < _X)); }

protected:
	_C c;
};

#define USE_ROBINHOOD

#ifdef USE_ROBINHOOD

#include <robin_hood/robin_hood.h>
template <class T>
using xr_hash = robin_hood::hash<T>;

template <typename K, class V>
using xr_pair = robin_hood::pair<K, V>;

template <typename K, class V, class Hasher = xr_hash<K>>
using xr_unordered_map = robin_hood::unordered_node_map<K, V, Hasher>;

template <class T, class Hasher = xr_hash<T>>
using xr_unordered_set = robin_hood::unordered_node_set<T, Hasher>;

template <typename K, class V, class Hasher = xr_hash<K>>
using xr_unordered_flat_map = robin_hood::unordered_flat_map<K, V, Hasher>;

template <class T, class Hasher = xr_hash<T>>
using xr_unordered_flat_set = robin_hood::unordered_flat_set<T, Hasher>;

#else

template <class T>
using xr_hash = std::hash<T>;

template <typename K, class V>
using xr_pair = std::pair<K, V>;

template <typename K, class V, class Hasher = xr_hash<K>, class Traits = std::equal_to<K>,
	typename allocator = xalloc<std::pair<const K, V>>>
using xr_unordered_map = std::unordered_map<K, V, Hasher, Traits, allocator>;

template <class T, class Hasher = xr_hash<T>, class Traits = std::equal_to<T>, typename allocator = xalloc<T>>
using xr_unordered_set = std::unordered_set<T, Hasher, Traits, allocator>;

template <typename K, class V, class Hasher = xr_hash<K>, class Traits = std::equal_to<K>,
	typename allocator = xalloc<std::pair<const K, V>>>
using xr_unordered_flat_map = std::unordered_map<K, V, Hasher, Traits, allocator>;

template <class T, class Hasher = xr_hash<T>, class Traits = std::equal_to<T>, typename allocator = xalloc<T>>
using xr_unordered_flat_set = std::unordered_set<T, Hasher, Traits, allocator>;

#endif //USE_ROBINHOOD

template <typename T, typename allocator = xalloc<T>>
class xr_list : public std::list<T, allocator>
{
private:
	using inherited = std::list<T, allocator>;

public:
	using inherited::inherited;
	xr_list() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
};

template <typename K, class P = std::less<K>, typename allocator = xalloc<K>>
class xr_set : public std::set<K, P, allocator>
{
private:
	using inherited = std::set<K, P, allocator>;

public:
	using inherited::inherited;
	xr_set() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
};

template <typename K, class P = std::less<K>, typename allocator = xalloc<K>>
class xr_multiset : public std::multiset<K, P, allocator>
{
private:
	using inherited = std::multiset<K, P, allocator>;

public:
	using inherited::inherited;
	xr_multiset() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
};

template <typename K, class V, class P = std::less<K>, typename allocator = xalloc<std::pair<const K, V>>>
class xr_map : public std::map<K, V, P, allocator>
{
private:
	using inherited = std::map<K, V, P, allocator>;

public:
	using inherited::inherited;
	xr_map() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
};

template <typename K, class V, class P = std::less<K>, typename allocator = xalloc<std::pair<const K, V>>>
class xr_multimap : public std::multimap<K, V, P, allocator>
{
private:
	using inherited = std::multimap<K, V, P, allocator>;

public:
	using inherited::inherited;
	xr_multimap() = default;
	u32 size() const { return static_cast<u32>(inherited::size()); }
};

// Portable hash containers. Intel LLVM (icx-cl) does not provide the
// deprecated Microsoft <hash_map>/<hash_set> extensions.
template <typename V, class Hasher = std::hash<V>, class KeyEqual = std::equal_to<V>,
    typename Allocator = xalloc<V>>
class xr_hash_set : public std::unordered_set<V, Hasher, KeyEqual, Allocator>
{
private:
    using inherited = std::unordered_set<V, Hasher, KeyEqual, Allocator>;

public:
    using inherited::inherited;
    u32 size() const { return static_cast<u32>(inherited::size()); }
};

template <typename V, class Hasher = std::hash<V>, class KeyEqual = std::equal_to<V>,
    typename Allocator = xalloc<V>>
class xr_hash_multiset : public std::unordered_multiset<V, Hasher, KeyEqual, Allocator>
{
private:
    using inherited = std::unordered_multiset<V, Hasher, KeyEqual, Allocator>;

public:
    using inherited::inherited;
    u32 size() const { return static_cast<u32>(inherited::size()); }
};

template <typename K, class V, class Hasher = std::hash<K>, class KeyEqual = std::equal_to<K>,
    typename Allocator = xalloc<std::pair<const K, V>>>
class xr_hash_map : public std::unordered_map<K, V, Hasher, KeyEqual, Allocator>
{
private:
    using inherited = std::unordered_map<K, V, Hasher, KeyEqual, Allocator>;

public:
    using inherited::inherited;
    u32 size() const { return static_cast<u32>(inherited::size()); }
};

template <typename K, class V, class Hasher = std::hash<K>, class KeyEqual = std::equal_to<K>,
    typename Allocator = xalloc<std::pair<const K, V>>>
class xr_hash_multimap : public std::unordered_multimap<K, V, Hasher, KeyEqual, Allocator>
{
private:
    using inherited = std::unordered_multimap<K, V, Hasher, KeyEqual, Allocator>;

public:
    using inherited::inherited;
    u32 size() const { return static_cast<u32>(inherited::size()); }
};

#endif

template <class _Ty1, class _Ty2>
inline std::pair<_Ty1, _Ty2> mk_pair(_Ty1 _Val1, _Ty2 _Val2) { return (std::pair<_Ty1, _Ty2>(_Val1, _Val2)); }

struct pred_str
{
	IC bool operator()(const char* x, const char* y) const { return xr_strcmp(x, y) < 0; }
};

struct pred_stri
{
	IC bool operator()(const char* x, const char* y) const { return stricmp(x, y) < 0; }
};

// STL extensions
#define DEF_VECTOR(N,T) typedef xr_vector< T > N; typedef N::iterator N##_it;
#define DEF_LIST(N,T) typedef xr_list< T > N; typedef N::iterator N##_it;
#define DEF_DEQUE(N,T) typedef xr_deque< T > N; typedef N::iterator N##_it;
#define DEF_MAP(N,K,T) typedef xr_map< K, T > N; typedef N::iterator N##_it;

#define DEFINE_DEQUE(T,N,I) typedef xr_deque< T > N; typedef N::iterator I;
#define DEFINE_LIST(T,N,I) typedef xr_list< T > N; typedef N::iterator I;
#define DEFINE_VECTOR(T,N,I) typedef xr_vector< T > N; typedef N::iterator I;
#define DEFINE_MAP(K,T,N,I) typedef xr_map< K , T > N; typedef N::iterator I;
#define DEFINE_MAP_PRED(K,T,N,I,P) typedef xr_map< K, T, P > N; typedef N::iterator I;
#define DEFINE_MMAP(K,T,N,I) typedef xr_multimap< K, T > N; typedef N::iterator I;
#define DEFINE_SVECTOR(T,C,N,I) typedef svector< T, C > N; typedef N::iterator I;
#define DEFINE_SET(T,N,I) typedef xr_set< T > N; typedef N::iterator I;
#define DEFINE_SET_PRED(T,N,I,P) typedef xr_set< T, P > N; typedef N::iterator I;
#define DEFINE_STACK(T,N) typedef xr_stack< T > N;

#include "FixedVector.h"
#include "buffer_vector.h"

// auxilary definition
DEFINE_VECTOR(bool, boolVec, boolIt);
DEFINE_VECTOR(BOOL, BOOLVec, BOOLIt);
DEFINE_VECTOR(BOOL*, LPBOOLVec, LPBOOLIt);
DEFINE_VECTOR(Frect, FrectVec, FrectIt);
DEFINE_VECTOR(Irect, IrectVec, IrectIt);
DEFINE_VECTOR(Fplane, PlaneVec, PlaneIt);
DEFINE_VECTOR(Fvector2, Fvector2Vec, Fvector2It);
DEFINE_VECTOR(Fvector, FvectorVec, FvectorIt);
DEFINE_VECTOR(Fvector*, LPFvectorVec, LPFvectorIt);
DEFINE_VECTOR(Fcolor, FcolorVec, FcolorIt);
DEFINE_VECTOR(Fcolor*, LPFcolorVec, LPFcolorIt);
DEFINE_VECTOR(LPSTR, LPSTRVec, LPSTRIt);
DEFINE_VECTOR(LPCSTR, LPCSTRVec, LPCSTRIt);
DEFINE_VECTOR(string64, string64Vec, string64It);
DEFINE_VECTOR(xr_string, SStringVec, SStringVecIt);

DEFINE_VECTOR(s8, S8Vec, S8It);
DEFINE_VECTOR(s8*, LPS8Vec, LPS8It);
DEFINE_VECTOR(s16, S16Vec, S16It);
DEFINE_VECTOR(s16*, LPS16Vec, LPS16It);
DEFINE_VECTOR(s32, S32Vec, S32It);
DEFINE_VECTOR(s32*, LPS32Vec, LPS32It);
DEFINE_VECTOR(u8, U8Vec, U8It);
DEFINE_VECTOR(u8*, LPU8Vec, LPU8It);
DEFINE_VECTOR(u16, U16Vec, U16It);
DEFINE_VECTOR(u16*, LPU16Vec, LPU16It);
DEFINE_VECTOR(u32, U32Vec, U32It);
DEFINE_VECTOR(u32*, LPU32Vec, LPU32It);
DEFINE_VECTOR(float, FloatVec, FloatIt);
DEFINE_VECTOR(float*, LPFloatVec, LPFloatIt);
DEFINE_VECTOR(int, IntVec, IntIt);
DEFINE_VECTOR(int*, LPIntVec, LPIntIt);

#ifdef __BORLANDC__
DEFINE_VECTOR(AnsiString, AStringVec, AStringIt);
DEFINE_VECTOR(AnsiString*, LPAStringVec, LPAStringIt);
#endif

#endif
