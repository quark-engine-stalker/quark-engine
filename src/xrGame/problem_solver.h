////////////////////////////////////////////////////////////////////////////
//	Module 		: problem_solver.h
//	Created 	: 24.02.2004
//  Modified 	: 10.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Problem solver
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "associative_vector.h"

template <
	typename _operator_condition,
	typename _condition_state,
	typename _operator,
	typename _condition_evaluator,
	typename _operator_id_type,
	bool _reverse_search = false,
	typename _operator_ptr = _operator*,
	typename _condition_evaluator_ptr = _condition_evaluator*>
class CProblemSolver
{
public:
	enum
	{
		reverse_search = _reverse_search,
	};

private:
	typedef CProblemSolver<
		_operator_condition,
		_condition_state,
		_operator,
		_condition_evaluator,
		_operator_id_type,
		_reverse_search,
		_operator_ptr,
		_condition_evaluator_ptr
	> self_type;

public:
	typedef _operator_condition COperatorCondition;
	typedef _operator COperator;
	typedef _condition_state CState;
	typedef _condition_evaluator CConditionEvaluator;
	typedef _operator_ptr _operator_ptr;
	typedef _condition_evaluator_ptr _condition_evaluator_ptr;
	typedef typename _operator_condition::_condition_type _condition_type;
	typedef typename _operator_condition::_value_type _value_type;
	typedef typename _operator::_edge_value_type _edge_value_type;
	typedef CState _index_type;
	typedef _operator_id_type _edge_type;

	struct SOperator
	{
		_operator_id_type m_operator_id;
		_operator_ptr m_operator;

		IC SOperator(const _operator_id_type& operator_id, _operator_ptr _operator) :
			m_operator_id(operator_id),
			m_operator(_operator)
		{
		}

		bool operator<(const _operator_id_type& operator_id) const
		{
			return (m_operator_id < operator_id);
		}

		_operator_ptr get_operator() const
		{
			return (m_operator);
		}
	};

	typedef xr_vector<SOperator> OPERATOR_VECTOR;
	typedef typename OPERATOR_VECTOR::const_iterator const_iterator;
	typedef associative_vector<_condition_type, _condition_evaluator_ptr> EVALUATORS;

	struct SSolveProfile
	{
		struct SEvaluatorProfile
		{
			u32 condition_id = u32(-1);
			u32 calls = 0;
			u64 total_ticks = 0;
			u64 max_ticks = 0;
		};

		static constexpr u32 max_evaluator_profiles = 64;

		u64 actuality_ticks = 0;
		u64 search_ticks = 0;
		u64 evaluator_ticks = 0;
		u64 operator_apply_ticks = 0;
		u64 operator_weight_ticks = 0;
		u64 solution_rebuild_ticks = 0;
		u32 evaluator_calls = 0;
		u32 operator_apply_calls = 0;
		u32 operator_weight_calls = 0;
		u32 solution_rebuild_calls = 0;
		u32 visited_nodes = 0;
		u32 solution_size = 0;
		u32 evaluator_profile_count = 0;
		SEvaluatorProfile evaluator_profiles[max_evaluator_profiles] = {};
		bool search_performed = false;
		bool actual_shortcut = false;
		bool failed = false;
	};

protected:
	OPERATOR_VECTOR m_operators;
	EVALUATORS m_evaluators;
	xr_vector<_edge_type> m_solution;
	CState m_target_state;
	mutable CState m_current_state;
	mutable CState m_temp;
	mutable bool m_applied;
	bool m_actuality;
	bool m_solution_changed;
	bool m_failed;
	mutable bool m_solve_profile_enabled;
	mutable SSolveProfile m_solve_profile;

private:
	IC _value_type evaluate_profiled(const _condition_type& condition_id, _condition_evaluator_ptr evaluator) const;
	IC bool is_goal_reached_dispatch(const _index_type& vertex_index) const
	{
		if constexpr (reverse_search)
			return is_goal_reached_impl(vertex_index, true);
		else
			return is_goal_reached_impl(vertex_index);
	}

	IC bool is_goal_reached_impl(const _index_type& vertex_index) const;
	IC bool is_goal_reached_impl(const _index_type& vertex_index, bool) const;

	IC _edge_value_type estimate_edge_weight_impl(const _index_type& vertex_index) const;
	IC _edge_value_type estimate_edge_weight_impl(const _index_type& vertex_index, bool) const;

	IC _edge_value_type estimate_edge_weight_dispatch(const _index_type& vertex_index) const
	{
		if constexpr (reverse_search)
			return estimate_edge_weight_impl(vertex_index, true);
		else
			return estimate_edge_weight_impl(vertex_index);
	}

protected:
#ifdef DEBUG
	IC		void						validate_properties		(const CState &conditions) const;
#endif


public:
	// common interface
	IC CProblemSolver();
	virtual ~CProblemSolver();
	void init();
	virtual void setup();
	IC bool actual() const;

	// graph interface
	IC _edge_value_type get_edge_weight(const _index_type& vertex_index0, const _index_type& vertex_index1,
	                                    const const_iterator& i) const;
	IC bool is_accessible(const _index_type& vertex_index) const;
	IC const _index_type& value(const _index_type& vertex_index, const_iterator& i, bool reverse_search) const;
	IC void begin(const _index_type& vertex_index, const_iterator& b, const_iterator& e) const;
	IC bool is_goal_reached(const _index_type& vertex_index) const;
	IC _edge_value_type estimate_edge_weight(const _index_type& vertex_index) const;

	// operator interface
	IC virtual void add_operator(const _edge_type& operator_id, _operator_ptr _operator);
	IC virtual void remove_operator(const _edge_type& operator_id);
	IC _operator_ptr get_operator(const _operator_id_type& operator_id);
	IC const OPERATOR_VECTOR& operators() const;

	// state interface
	IC void set_target_state(const CState& state);
	IC const CState& current_state() const;
	IC const CState& target_state() const;

	// evaluator interface
	IC virtual void add_evaluator(const _condition_type& condition_id, _condition_evaluator_ptr evaluator);
	IC virtual void remove_evaluator(const _condition_type& condition_id);
	IC _condition_evaluator_ptr evaluator(const _condition_type& condition_id) const;
	IC const EVALUATORS& evaluators() const;
	IC void evaluate_condition(typename xr_vector<COperatorCondition>::const_iterator& I,
	                           typename xr_vector<COperatorCondition>::const_iterator& E,
	                           const _condition_type& condition_id) const;

	// solver interface
	IC void solve();
	IC const xr_vector<_edge_type>& solution() const;
	IC void set_solve_profile_enabled(bool enabled);
	IC bool solve_profile_enabled() const;
	IC const SSolveProfile& solve_profile() const;
	IC void record_solution_rebuild_profile(u64 elapsed_ticks) const;
	virtual void clear();
};

#ifndef AI_COMPILER
#	include "ai_space.h"
#endif

#include "graph_engine.h"
#include "object_broker.h"

#include "problem_solver_inline.h"
