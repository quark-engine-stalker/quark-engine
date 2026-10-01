////////////////////////////////////////////////////////////////////////////
//	Module 		: graph_engine_inline.h
//	Created 	: 21.03.2002
//  Modified 	: 03.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Graph engine inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CGraphEngine::CGraphEngine(u32 max_vertex_count)
{
	m_max_vertex_count = max_vertex_count;
	const u64 estimated_workspace_bytes = u64(max_vertex_count) * 16u + 2u * 1024u * 1024u;
	const u32 memory_limited_count = estimated_workspace_bytes ?
		_max(2u, _min(8u, u32((128ull * 1024ull * 1024ull) / estimated_workspace_bytes))) : 1u;
	m_algorithm_pool_limit = _max(1u, _min(memory_limited_count, _min(8u, u32(CPU::ID.n_cores))));
#ifdef AI_COMPILER
	m_algorithm_pool_limit = 1;
#endif
	// Allocate and zero the bounded pool while the level is loading. Growing the
	// A* index arrays on the first multi-NPC burst would otherwise create a hitch.
	for (u32 index = 0; index < m_algorithm_pool_limit; ++index)
	{
		CAlgorithm* algorithm = xr_new<CAlgorithm>(max_vertex_count);
		algorithm->data_storage().set_min_bucket_value(_dist_type(0));
		algorithm->data_storage().set_max_bucket_value(_dist_type(2000));
		m_algorithm_pool.push_back(SAlgorithmSlot(algorithm));
	}

#ifndef AI_COMPILER
	m_solver_algorithm = xr_new<CSolverAlgorithm>(16 * 1024);
	m_string_algorithm = xr_new<CStringAlgorithm>(1024);
#endif // AI_COMPILER
}

IC CGraphEngine::~CGraphEngine()
{
	for (u32 i = 0; i < m_algorithm_pool.size(); ++i)
	{
		VERIFY(!m_algorithm_pool[i].in_use);
		xr_delete(m_algorithm_pool[i].algorithm);
	}
	m_algorithm_pool.clear();
#ifndef AI_COMPILER
	xr_delete(m_solver_algorithm);
	xr_delete(m_string_algorithm);
#endif // AI_COMPILER
}

IC CGraphEngine::CAlgorithm* CGraphEngine::acquire_algorithm()
{
	std::unique_lock<std::mutex> lock(m_algorithm_pool_mutex);
	for (;;)
	{
		for (u32 i = 0; i < m_algorithm_pool.size(); ++i)
		{
			if (!m_algorithm_pool[i].in_use)
			{
				m_algorithm_pool[i].in_use = true;
				return m_algorithm_pool[i].algorithm;
			}
		}

		if (m_algorithm_pool.size() < m_algorithm_pool_limit)
		{
			CAlgorithm* algorithm = xr_new<CAlgorithm>(m_max_vertex_count);
			algorithm->data_storage().set_min_bucket_value(_dist_type(0));
			algorithm->data_storage().set_max_bucket_value(_dist_type(2000));
			m_algorithm_pool.push_back(SAlgorithmSlot(algorithm));
			m_algorithm_pool.back().in_use = true;
			return algorithm;
		}

		m_algorithm_pool_available.wait(lock);
	}
}

IC void CGraphEngine::release_algorithm(CAlgorithm* algorithm)
{
	{
		std::lock_guard<std::mutex> lock(m_algorithm_pool_mutex);
		for (u32 i = 0; i < m_algorithm_pool.size(); ++i)
		{
			if (m_algorithm_pool[i].algorithm != algorithm)
				continue;

			VERIFY(m_algorithm_pool[i].in_use);
			m_algorithm_pool[i].in_use = false;
			m_algorithm_pool_available.notify_one();
			return;
		}
	}
	VERIFY2(false, "Releasing an unknown graph-search workspace");
}

IC CGraphEngine::CAlgorithmGuard::CAlgorithmGuard(CGraphEngine& owner) :
	m_owner(owner),
	m_algorithm(owner.acquire_algorithm())
{
}

IC CGraphEngine::CAlgorithmGuard::~CAlgorithmGuard()
{
	m_owner.release_algorithm(m_algorithm);
}

#ifndef AI_COMPILER
IC const CGraphEngine::CSolverAlgorithm& CGraphEngine::solver_algorithm() const
{
	return (*m_solver_algorithm);
}
#endif // AI_COMPILER

template <
	typename _Graph,
	typename _Parameters
>
IC bool CGraphEngine::search(
	const _Graph& graph,
	const _index_type& start_node,
	const _index_type& dest_node,
	xr_vector<_index_type>* node_path,
	const _Parameters& parameters
)
{
	if (start_node == _index_type(-1) || dest_node == _index_type(-1))
		return false;

#ifndef AI_COMPILER
	CTimer ai_path_timer;
	if (g_bEnableStatGather)
		ai_path_timer.Start();
	START_PROFILE("graph_engine")
		START_PROFILE("graph_engine/search")
#endif
			typedef CPathManager<_Graph, CAlgorithm::CDataStorage, _Parameters, _dist_type, _index_type, _iteration_type
			> CPathManagerGeneric;

			CPathManagerGeneric path_manager;

			CAlgorithmGuard algorithm(*this);
			path_manager.setup(
				&graph,
				&algorithm->data_storage(),
				node_path,
				start_node,
				dest_node,
				parameters
			);

			bool successfull = algorithm->find(path_manager);
			CAStarBucketTelemetry::record(
				algorithm->data_storage().bucket_overflow_count(),
				algorithm->data_storage().max_bucket_chain_length());

#ifndef AI_COMPILER
			if (g_bEnableStatGather)
				Device.Statistic->AI_Path.Add(ai_path_timer.GetElapsed_ticks());
#endif
			return (successfull);
#ifndef AI_COMPILER
		STOP_PROFILE
	STOP_PROFILE
#endif
}

template <
	typename _Graph,
	typename _Parameters
>
IC bool CGraphEngine::search(
	const _Graph& graph,
	const _index_type& start_node,
	const _index_type& dest_node,
	xr_vector<_index_type>* node_path,
	_Parameters& parameters
)
{
	if (start_node == _index_type(-1) || dest_node == _index_type(-1))
		return false;

#ifndef AI_COMPILER
	CTimer ai_path_timer;
	if (g_bEnableStatGather)
		ai_path_timer.Start();
	START_PROFILE("graph_engine")
		START_PROFILE("graph_engine/search")
#endif
			typedef CPathManager<_Graph, CAlgorithm::CDataStorage, _Parameters, _dist_type, _index_type, _iteration_type
			> CPathManagerGeneric;

			CPathManagerGeneric path_manager;

			CAlgorithmGuard algorithm(*this);
			path_manager.setup(
				&graph,
				&algorithm->data_storage(),
				node_path,
				start_node,
				dest_node,
				parameters
			);

			bool successfull = algorithm->find(path_manager);
			CAStarBucketTelemetry::record(
				algorithm->data_storage().bucket_overflow_count(),
				algorithm->data_storage().max_bucket_chain_length());

#ifndef AI_COMPILER
			if (g_bEnableStatGather)
				Device.Statistic->AI_Path.Add(ai_path_timer.GetElapsed_ticks());
#endif
			return (successfull);
#ifndef AI_COMPILER
		STOP_PROFILE
	STOP_PROFILE
#endif
}

template <
	typename _Graph,
	typename _Parameters,
	typename _PathManager
>
IC bool CGraphEngine::search(
	const _Graph& graph,
	const _index_type& start_node,
	const _index_type& dest_node,
	xr_vector<_index_type>* node_path,
	const _Parameters& parameters,
	_PathManager& path_manager
)
{
	if (start_node == _index_type(-1) || dest_node == _index_type(-1))
		return false;

#ifndef AI_COMPILER
	CTimer ai_path_timer;
	if (g_bEnableStatGather)
		ai_path_timer.Start();
	START_PROFILE("graph_engine")
		START_PROFILE("graph_engine/search")
#endif
			CAlgorithmGuard algorithm(*this);
			path_manager.setup(
				&graph,
				&algorithm->data_storage(),
				node_path,
				start_node,
				dest_node,
				parameters
			);

			bool successfull = algorithm->find(path_manager);
			CAStarBucketTelemetry::record(
				algorithm->data_storage().bucket_overflow_count(),
				algorithm->data_storage().max_bucket_chain_length());

#ifndef AI_COMPILER
			if (g_bEnableStatGather)
				Device.Statistic->AI_Path.Add(ai_path_timer.GetElapsed_ticks());
#endif
			return (successfull);
#ifndef AI_COMPILER
		STOP_PROFILE
	STOP_PROFILE
#endif
}

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
IC bool CGraphEngine::search(
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
)
{
#ifndef AI_COMPILER
	Device.Statistic->AI_Path.Begin();
	START_PROFILE("graph_engine")
		START_PROFILE("graph_engine/proble_solver")
#endif
			typedef CProblemSolver<T1, T2, T3, T4, T5, T6, T7, T8> CSProblemSolver;
			typedef CPathManager<CSProblemSolver, CSolverAlgorithm::CDataStorage, _Parameters, _solver_dist_type,
			                     _solver_index_type, GraphEngineSpace::_iteration_type> CSolverPathManager;

			CSolverPathManager path_manager;

			path_manager.setup(
				&graph,
				&m_solver_algorithm->data_storage(),
				node_path,
				start_node,
				dest_node,
				parameters
			);

			bool successfull = m_solver_algorithm->find(path_manager);

#ifndef AI_COMPILER
			Device.Statistic->AI_Path.End();
#endif
			return (successfull);
#ifndef AI_COMPILER
		STOP_PROFILE
	STOP_PROFILE
#endif
}

template <
	typename _Graph,
	typename _Parameters
>
IC bool CGraphEngine::search(
	const _Graph& graph,
	const shared_str& start_node,
	const shared_str& dest_node,
	xr_vector<shared_str>* node_path,
	_Parameters& parameters
)
{
#ifndef AI_COMPILER
	Device.Statistic->AI_Path.Begin();
	START_PROFILE("graph_engine")
		START_PROFILE("graph_engine/search")
#endif

			typedef CPathManager<_Graph, CStringAlgorithm::CDataStorage, _Parameters, float, shared_str, u32>
				CPathManagerGeneric;

			CPathManagerGeneric path_manager;

			path_manager.setup(
				&graph,
				&m_string_algorithm->data_storage(),
				node_path,
				start_node,
				dest_node,
				parameters
			);

			bool successfull = m_string_algorithm->find(path_manager);

#ifndef AI_COMPILER
			Device.Statistic->AI_Path.End();
#endif
			return (successfull);
#ifndef AI_COMPILER
		STOP_PROFILE
	STOP_PROFILE
#endif
}

#endif // AI_COMPILER
