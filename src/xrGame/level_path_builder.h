////////////////////////////////////////////////////////////////////////////
//	Module 		: level_path_builder.h
//  Modified 	: 21.02.2005
//  Modified 	: 21.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Level path builder
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "movement_manager.h"
#include "level_path_manager.h"
#include "detail_path_builder.h"

extern int psAI_LevelPathMaxPerFrame;

class CLevelPathBuilder : public CDetailPathBuilder
{
private:
	typedef CDetailPathBuilder inherited;

private:
	Fvector m_temp;
	u32 m_start_vertex_id;
	u32 m_dest_vertex_id;
	const Fvector* m_precise_position;
	u32 m_next_retry_time;
	bool m_extrapolate_path;
	bool m_use_delay_after_fail;
	// Restriction borders are collected on the game thread, then published as
	// an immutable query-local hash set to the A* worker. Keep both containers
	// on the builder so repeated path requests reuse their allocations.
	xr_vector<u32> m_blocked_vertex_list;
	xr_unordered_flat_set<u32> m_blocked_vertices;

private:
	enum
	{
		time_to_wait_after_fail_min = u32(1000),
		time_to_wait_after_fail_max = u32(1500),
	};

public:
	IC CLevelPathBuilder(CMovementManager* object) :
		inherited(object),
		m_next_retry_time(0),
		m_use_delay_after_fail(true)
	{}

	IC const u32& dest_vertex_id() const
	{
		return (m_dest_vertex_id);
	}

	IC void use_delay_after_fail(bool const value)
	{
		m_use_delay_after_fail = value;
		if (!value) m_next_retry_time = 0;
	}

	IC void setup(const u32& start_vertex_id, const u32& dest_vertex_id, bool extrapolate_path,
	              const Fvector* precise_position)
	{
		VERIFY(ai().level_graph().valid_vertex_id(start_vertex_id));
		m_start_vertex_id = start_vertex_id;

		VERIFY(ai().level_graph().valid_vertex_id(dest_vertex_id));
		m_dest_vertex_id = dest_vertex_id;

		m_blocked_vertex_list.clear_not_free();
		m_object->restrictions().collect_path_border(start_vertex_id, dest_vertex_id, m_blocked_vertex_list);
		m_blocked_vertices.clear();
		m_blocked_vertices.reserve(m_blocked_vertex_list.size());
		for (u32 index = 0; index < m_blocked_vertex_list.size(); ++index)
			m_blocked_vertices.insert(m_blocked_vertex_list[index]);
		m_object->base_level_params()->blocked_vertices =
			m_blocked_vertices.empty() ? nullptr : &m_blocked_vertices;

		m_extrapolate_path = extrapolate_path;
		if (!precise_position)
			m_precise_position = 0;
		else
		{
			m_temp = *precise_position;
			m_precise_position = &m_temp;
		}
	}

	void register_to_process()
	{
		// Keep failed searches from immediately entering the queue again.
		if (Device.dwTimeGlobal < m_next_retry_time)
			return;

		// Mass restriction invalidation used to submit an unbounded burst of A*
		// searches in one frame. Those jobs are individually parallel, but the
		// frame still waits for the longest queue tail. Stagger submissions while
		// leaving rejected objects in BuildLevelPath for a later scheduler tick.
		static u32 submission_frame = u32(-1);
		static u32 submissions = 0;
		if (submission_frame != Device.dwFrame)
		{
			submission_frame = Device.dwFrame;
			submissions = 0;
		}

		if ((psAI_LevelPathMaxPerFrame > 0) &&
			(submissions >= static_cast<u32>(psAI_LevelPathMaxPerFrame)))
			return;

		++submissions;
		m_object->m_wait_for_distributed_computation = true;
		Device.add_to_seq_parallel_independent(
			fastdelegate::FastDelegate0<>(this, &CLevelPathBuilder::process));
	}

	void process_impl(bool separate_compute = false)
	{
		m_object->m_wait_for_distributed_computation = false;
		{

			m_object->level_path().build_path(m_start_vertex_id, m_dest_vertex_id);
		}

		if (m_object->level_path().failed())
		{
			if (m_use_delay_after_fail)
			{
				const u32 retry_span = time_to_wait_after_fail_max - time_to_wait_after_fail_min;
				const u32 retry_seed = u32(m_object->object().ID()) * 1664525u + Device.dwFrame * 1013904223u;
				m_next_retry_time = Device.dwTimeGlobal + time_to_wait_after_fail_min + retry_seed % retry_span;
			}

			m_object->m_path_state = CMovementManager::ePathStateBuildLevelPath;
			return;
		}

		{

			m_object->level_path().select_intermediate_vertex();
		}

		m_object->m_path_state = CMovementManager::ePathStateBuildDetailPath;
		if (separate_compute)
			return;

		m_object->detail().set_state_patrol_path(m_extrapolate_path);
		m_object->detail().set_start_position(m_object->object().Position());
		m_object->detail().set_start_direction(Fvector().setHP(-m_object->m_body.current.yaw, 0));

		if (m_precise_position)
			m_object->detail().set_dest_position(*m_precise_position);

		inherited::setup(m_object->level_path().path(), m_object->level_path().intermediate_index());
		inherited::process_impl(separate_compute);
	}

	void __stdcall process()
	{
		m_object->build_level_path(true);
	}

	IC void remove()
	{
		if (m_object->m_wait_for_distributed_computation)
			m_object->m_wait_for_distributed_computation = false;

		Device.remove_from_seq_parallel_independent(
			fastdelegate::FastDelegate0<>(
				this,
				&CLevelPathBuilder::process
			)
		);
	}
};
