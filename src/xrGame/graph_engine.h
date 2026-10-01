////////////////////////////////////////////////////////////////////////////
//	Module 		: graph_engine.h
//	Created 	: 21.03.2002
//  Modified 	: 26.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Graph engine
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "a_star.h"
#include "edge_path.h"
#include "vertex_manager_fixed.h"
#include "vertex_manager_hash_fixed.h"
#include "vertex_allocator_fixed.h"
#include "data_storage_bucket_list.h"
#include "data_storage_binary_heap.h"
#include "path_manager.h"
#include "graph_engine_space.h"
#include "profiler.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

#ifndef AI_COMPILER
#	include "operator_condition.h"
#	include "condition_state.h"
#	include "operator_abstract.h"
#endif // AI_COMPILER

namespace hash_fixed_vertex_manager
{
	IC u32 to_u32(GraphEngineSpace::CWorldState const& other)
	{
		return (other.hash_value());
	}
} // namespace hash_fixed_vertex_manager

using namespace GraphEngineSpace;

class CAStarBucketTelemetry
{
public:
	struct SSnapshot
	{
		u64 searches;
		u64 overflow_searches;
		u64 overflow_events;
		u32 max_overflow_events_per_search;
		u32 max_chain_length;
	};

private:
	static IC std::atomic<u64>& searches_counter()
	{
		static std::atomic<u64> value(0);
		return value;
	}

	static IC std::atomic<u64>& overflow_searches_counter()
	{
		static std::atomic<u64> value(0);
		return value;
	}

	static IC std::atomic<u64>& overflow_events_counter()
	{
		static std::atomic<u64> value(0);
		return value;
	}

	static IC std::atomic<u32>& max_overflow_counter()
	{
		static std::atomic<u32> value(0);
		return value;
	}

	static IC std::atomic<u32>& max_chain_counter()
	{
		static std::atomic<u32> value(0);
		return value;
	}

	static IC void update_max(std::atomic<u32>& target, u32 value)
	{
		u32 current = target.load(std::memory_order_relaxed);
		while ((current < value) && !target.compare_exchange_weak(
			current, value, std::memory_order_relaxed, std::memory_order_relaxed))
		{
		}
	}

public:
	static IC void record(u32 overflow_events, u32 max_chain_length)
	{
		searches_counter().fetch_add(1, std::memory_order_relaxed);
		if (overflow_events)
		{
			overflow_searches_counter().fetch_add(1, std::memory_order_relaxed);
			overflow_events_counter().fetch_add(overflow_events, std::memory_order_relaxed);
			update_max(max_overflow_counter(), overflow_events);
		}
		update_max(max_chain_counter(), max_chain_length);
	}

	static IC SSnapshot snapshot()
	{
		SSnapshot result;
		result.searches = searches_counter().load(std::memory_order_relaxed);
		result.overflow_searches = overflow_searches_counter().load(std::memory_order_relaxed);
		result.overflow_events = overflow_events_counter().load(std::memory_order_relaxed);
		result.max_overflow_events_per_search = max_overflow_counter().load(std::memory_order_relaxed);
		result.max_chain_length = max_chain_counter().load(std::memory_order_relaxed);
		return result;
	}

	static IC void reset()
	{
		searches_counter().store(0, std::memory_order_relaxed);
		overflow_searches_counter().store(0, std::memory_order_relaxed);
		overflow_events_counter().store(0, std::memory_order_relaxed);
		max_overflow_counter().store(0, std::memory_order_relaxed);
		max_chain_counter().store(0, std::memory_order_relaxed);
	}
};

class CGraphEngine
{
public:
#ifndef AI_COMPILER
	typedef CDataStorageBinaryHeap CSolverPriorityQueue;
	typedef CDataStorageBinaryHeap CStringPriorityQueue;
#endif // AI_COMPILER
	typedef CDataStorageBucketList<u32, u32, 8 * 1024, false> CPriorityQueue;

	typedef CVertexManagerFixed<u32, u32, 8> CVertexManager;

#ifndef AI_COMPILER
	typedef CVertexManagerHashFixed<
		u32,
		_solver_index_type,
		256,
		8 * 1024
	> CSolverVertexManager;
	typedef CVertexManagerHashFixed<
		u32,
		shared_str,
		128,
		1024
	> CStringVertexManager;
#endif // AI_COMPILER
#ifdef AI_COMPILER
	typedef CVertexAllocatorFixed<2*1024*1024>				CVertexAllocator;
#else
	typedef CVertexAllocatorFixed<64 * 1024> CVertexAllocator;
	typedef CVertexAllocatorFixed<8 * 1024> CSolverVertexAllocator;
	typedef CVertexAllocatorFixed<1024> CStringVertexAllocator;
#endif // AI_COMPILER

	typedef CAStar<
		_dist_type,
		CPriorityQueue,
		CVertexManager,
		CVertexAllocator
	> CAlgorithm;

#ifndef AI_COMPILER
	typedef CAStar<
		_solver_dist_type,
		CSolverPriorityQueue,
		CSolverVertexManager,
		CSolverVertexAllocator,
		true,
		CEdgePath<
			_solver_edge_type,
			true
		>
	> CSolverAlgorithm;

	typedef CAStar<
		float,
		CStringPriorityQueue,
		CStringVertexManager,
		CStringVertexAllocator
	> CStringAlgorithm;
#endif // AI_COMPILER

	struct SAlgorithmSlot
	{
		CAlgorithm* algorithm;
		bool in_use;

		SAlgorithmSlot(CAlgorithm* value) : algorithm(value), in_use(false) {}
	};

	std::mutex m_algorithm_pool_mutex;
	std::condition_variable m_algorithm_pool_available;
	xr_vector<SAlgorithmSlot> m_algorithm_pool;
	u32 m_algorithm_pool_limit;
	u32 m_max_vertex_count;

	CAlgorithm* acquire_algorithm();
	void release_algorithm(CAlgorithm* algorithm);

	class CAlgorithmGuard
	{
	private:
		CGraphEngine& m_owner;
		CAlgorithm* m_algorithm;

	public:
		IC explicit CAlgorithmGuard(CGraphEngine& owner);
		IC ~CAlgorithmGuard();
		IC CAlgorithm* operator->() const { return m_algorithm; }
	};

#ifndef AI_COMPILER
	CSolverAlgorithm* m_solver_algorithm;
	CStringAlgorithm* m_string_algorithm;
#endif // AI_COMPILER

public:

	IC CGraphEngine(u32 max_vertex_count);
	virtual ~CGraphEngine();
#ifndef AI_COMPILER
	IC const CSolverAlgorithm& solver_algorithm() const;
#endif // AI_COMPILER

	template <
		typename _Graph,
		typename _Parameters
	>
	IC bool search(
		const _Graph& graph,
		const shared_str& start_node,
		const shared_str& dest_node,
		xr_vector<shared_str>* node_path,
		_Parameters& parameters
	);

	template <
		typename _Graph,
		typename _Parameters
	>
	IC bool search(
		const _Graph& graph,
		const _index_type& start_node,
		const _index_type& dest_node,
		xr_vector<_index_type>* node_path,
		const _Parameters& parameters
	);

	template <
		typename _Graph,
		typename _Parameters
	>
	IC bool search(
		const _Graph& graph,
		const _index_type& start_node,
		const _index_type& dest_node,
		xr_vector<_index_type>* node_path,
		_Parameters& parameters
	);

	template <
		typename _Graph,
		typename _Parameters,
		typename _PathManager
	>
	IC bool search(
		const _Graph& graph,
		const _index_type& start_node,
		const _index_type& dest_node,
		xr_vector<_index_type>* node_path,
		const _Parameters& parameters,
		_PathManager& path_manager
	);

#ifndef AI_COMPILER
	template <
		typename T1,
		typename T2,
		typename T3,
		typename T4,
		typename T5,
		bool T6,
		typename T7,
		typename T8,
		typename _Parameters
	>
	IC bool search(
		const CProblemSolver<
			T1,
			T2,
			T3,
			T4,
			T5,
			T6,
			T7,
			T8
		>& graph,
		const _solver_index_type& start_node,
		const _solver_index_type& dest_node,
		xr_vector<_solver_edge_type>* node_path,
		const _Parameters& parameters
	);
#endif // AI_COMPILER
};

#include "graph_engine_inline.h"
