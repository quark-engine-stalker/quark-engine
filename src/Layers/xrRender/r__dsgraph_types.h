#pragma once

#include "../../xrCore/fixedmap.h"

//#ifndef USE_MEMORY_MONITOR
//#	define USE_DOUG_LEA_ALLOCATOR_FOR_RENDER
//#endif // USE_MEMORY_MONITOR

#ifdef USE_DOUG_LEA_ALLOCATOR_FOR_RENDER
//	extern doug_lea_allocator	g_render_lua_allocator;

	template <class T>
	class doug_lea_alloc {
	public:
		typedef	size_t		size_type;
		typedef ptrdiff_t	difference_type;
		typedef T*			pointer;
		typedef const T*	const_pointer;
		typedef T&			reference;
		typedef const T&	const_reference;
		typedef T			value_type;

	public:
		template<class _Other>	
		struct rebind			{	typedef doug_lea_alloc<_Other> other;	};
	public:
								pointer					address			(reference _Val) const					{	return (&_Val);	}
								const_pointer			address			(const_reference _Val) const			{	return (&_Val);	}
														doug_lea_alloc	()										{	}
														doug_lea_alloc	(const doug_lea_alloc<T>&)				{	}
		template<class _Other>							doug_lea_alloc	(const doug_lea_alloc<_Other>&)			{	}
		template<class _Other>	doug_lea_alloc<T>&		operator=		(const doug_lea_alloc<_Other>&)			{	return (*this);	}
								pointer					allocate		(size_type n, const void* p=0) const	{	return (T*)g_render_lua_allocator.malloc_impl(sizeof(T)*(u32)n);	}
								void					deallocate		(pointer p, size_type n) const			{	g_render_lua_allocator.free_impl	((void*&)p);				}
								void					deallocate		(void* p, size_type n) const			{	g_render_lua_allocator.free_impl	(p);				}
								char*					__charalloc		(size_type n)							{	return (char*)allocate(n); }
								void					construct		(pointer p, const T& _Val)				{	std::_Construct(p, _Val);	}
								void					destroy			(pointer p)								{	std::_Destroy(p);			}
								size_type				max_size		() const								{	size_type _Count = (size_type)(-1) / sizeof (T);	return (0 < _Count ? _Count : 1);	}
	};

	template<class _Ty,	class _Other>	inline	bool operator==(const doug_lea_alloc<_Ty>&, const doug_lea_alloc<_Other>&)		{	return (true);							}
	template<class _Ty, class _Other>	inline	bool operator!=(const doug_lea_alloc<_Ty>&, const doug_lea_alloc<_Other>&)		{	return (false);							}

	struct doug_lea_allocator_wrapper {
		template <typename T>
		struct helper {
			typedef doug_lea_alloc<T>	result;
		};

		static	void	*alloc		(const u32 &n)	{	return g_render_lua_allocator.malloc_impl((u32)n);	}
		template <typename T>
		static	void	dealloc		(T *&p)			{	g_render_lua_allocator.free_impl((void*&)p);	}
	};

#	define render_alloc				doug_lea_alloc
	typedef doug_lea_allocator_wrapper	render_allocator;

#else // USE_DOUG_LEA_ALLOCATOR_FOR_RENDER
#	define render_alloc				xalloc
typedef xr_allocator render_allocator;
#endif // USE_DOUG_LEA_ALLOCATOR_FOR_RENDER

class dxRender_Visual;

// #define	USE_RESOURCE_DEBUGGER

namespace R_dsgraph
{
	// Elementary types
	struct _NormalItem
	{
		float ssa;
		dxRender_Visual* pVisual;
	};

	struct _MatrixItem
	{
		float ssa;
		IRenderable* pObject;
		dxRender_Visual* pVisual;
		Fmatrix Matrix; // matrix (copy)
	};

	struct _MatrixItemS : public _MatrixItem
	{
		ShaderElement* se;
	};

	struct _LodItem
	{
		float ssa;
		dxRender_Visual* pVisual;
	};

#ifdef USE_RESOURCE_DEBUGGER
	typedef	ref_vs						vs_type;
	typedef	ref_ps						ps_type;
		typedef	ref_gs						gs_type;
		typedef	ref_hs						hs_type;
		typedef	ref_ds						ds_type;
#else
		typedef	SVS*					vs_type;
		typedef	ID3DGeometryShader*		gs_type;
			typedef	ID3D11HullShader*		hs_type;
			typedef	ID3D11DomainShader*		ds_type;
	typedef ID3DPixelShader* ps_type;
#endif

	// NORMAL
	typedef xr_vector<_NormalItem, render_allocator::helper<_NormalItem>::result> mapNormalDirect;

	struct mapNormalItems : public mapNormalDirect
	{
		float ssa;
	};

	struct mapNormalTextures : public FixedMAP<STextureList*, mapNormalItems, render_allocator>
	{
		float ssa;
	};

	struct mapNormalStates : public FixedMAP<ID3DState*, mapNormalTextures, render_allocator>
	{
		float ssa;
	};

	struct mapNormalCS : public FixedMAP<R_constant_table*, mapNormalStates, render_allocator>
	{
		float ssa;
	};
	struct	mapNormalAdvStages
	{
		hs_type		hs;
		ds_type		ds;
		mapNormalCS	mapCS;
	};
	struct	mapNormalPS			: public	FixedMAP<ps_type, mapNormalAdvStages,render_allocator>						{	float	ssa;	};
	struct	mapNormalGS			: public	FixedMAP<gs_type, mapNormalPS,render_allocator>						{	float	ssa;	};
	struct	mapNormalVS			: public	FixedMAP<vs_type, mapNormalGS,render_allocator>						{	};
	typedef mapNormalVS mapNormal_T;
	typedef mapNormal_T mapNormalPasses_T[SHADER_PASSES_MAX];

	// MATRIX
	typedef xr_vector<_MatrixItem, render_allocator::helper<_MatrixItem>::result> mapMatrixDirect;

	struct mapMatrixItems : public mapMatrixDirect
	{
		float ssa;
	};

	struct mapMatrixTextures : public FixedMAP<STextureList*, mapMatrixItems, render_allocator>
	{
		float ssa;
	};

	struct mapMatrixStates : public FixedMAP<ID3DState*, mapMatrixTextures, render_allocator>
	{
		float ssa;
	};

	struct mapMatrixCS : public FixedMAP<R_constant_table*, mapMatrixStates, render_allocator>
	{
		float ssa;
	};
	struct	mapMatrixAdvStages
	{
		hs_type		hs;
		ds_type		ds;
		mapMatrixCS	mapCS;
	};
	struct	mapMatrixPS			: public	FixedMAP<ps_type, mapMatrixAdvStages,render_allocator>						{	float	ssa;	};
	struct	mapMatrixGS			: public	FixedMAP<gs_type, mapMatrixPS,render_allocator>						{	float	ssa;	};
	struct	mapMatrixVS			: public	FixedMAP<vs_type, mapMatrixGS,render_allocator>						{	};
	typedef mapMatrixVS mapMatrix_T;
	typedef mapMatrix_T mapMatrixPasses_T[SHADER_PASSES_MAX];

	// Top level
	typedef FixedMAP<float, _MatrixItemS, render_allocator> mapSorted_T;
	typedef mapSorted_T::TNode mapSorted_Node;

	typedef FixedMAP<float, _MatrixItemS, render_allocator> mapHUD_T;
	typedef mapHUD_T::TNode mapHUD_Node;

	typedef FixedMAP<float, _MatrixItemS, render_allocator> mapScopeHUD_T; // Redotix99: for 3D Shader Based Scopes
	typedef mapScopeHUD_T::TNode mapScopeHUD_T_Node;

	typedef FixedMAP<float, _MatrixItemS, render_allocator> HUDMask_T;
	typedef HUDMask_T::TNode HUDMask_Node;

	// LOD entries are never searched or deduplicated: the renderer only appends
	// all of them and later consumes them in distance order. A contiguous flat
	// queue avoids a binary-tree walk and node reallocation fixups on every
	// insertion, then performs one cache-friendly sort immediately before use.
	struct mapLOD_T
	{
		struct TNode
		{
			float key;
			u32 insertion_order;
			_LodItem val;
		};

		typedef xr_vector<TNode, render_allocator::helper<TNode>::result> node_vector;

	private:
		node_vector nodes;
		u32 next_insertion_order = 0;
		bool sorted = true;

		void sort_nodes()
		{
			if (sorted)
				return;

			std::sort(nodes.begin(), nodes.end(), [](const TNode& left, const TNode& right)
			{
				if (left.key < right.key)
					return true;
				if (left.key > right.key)
					return false;

				// FixedMAP inserts equal keys into the left branch, so its LR traversal
				// returns the newest equal-key entry first.
				return left.insertion_order > right.insertion_order;
			});
			sorted = true;
		}

	public:
		TNode* insertInAnyWay(const float& key)
		{
			nodes.emplace_back();
			TNode& node = nodes.back();
			node.key = key;
			node.insertion_order = next_insertion_order++;
			node.val.ssa = 0.f;
			node.val.pVisual = nullptr;
			sorted = false;
			return &node;
		}

		template <typename container_type>
		void getLR(container_type& destination)
		{
			sort_nodes();
			for (const TNode& node : nodes)
				destination.push_back(node.val);
		}

		template <typename container_type>
		void getRL(container_type& destination)
		{
			sort_nodes();
			for (node_vector::const_reverse_iterator node = nodes.rbegin(); node != nodes.rend(); ++node)
				destination.push_back(node->val);
		}

		void clear()
		{
			nodes.clear();
			next_insertion_order = 0;
			sorted = true;
		}

		void destroy()
		{
			node_vector empty;
			nodes.swap(empty);
			next_insertion_order = 0;
			sorted = true;
		}
	};
	typedef mapLOD_T::TNode mapLOD_Node;

	typedef FixedMAP<float, _MatrixItemS, render_allocator> mapLandscape_T;
	typedef mapLandscape_T::TNode mapLandscape_Node;

	typedef FixedMAP<float, _MatrixItemS, render_allocator> mapWater_T;
	typedef mapWater_T::TNode mapWater_Node;

};
