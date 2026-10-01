////////////////////////////////////////////////////////////////////////////
//	Module 		: detail_path_builder.h
//  Modified 	: 21.02.2005
//  Modified 	: 21.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Detail path builder
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "movement_manager.h"
#include "detail_path_manager.h"

extern int psAI_DetailPathMaxPerFrame;

class CDetailPathBuilder
{
private:
	typedef CMovementManager::CLevelPathManager CLevelPathManager;
	typedef CLevelPathManager::PATH PATH;
protected:
	CMovementManager* m_object;

private:
	const PATH* m_level_path;
	u32 m_path_vertex_index;

public:
	IC CDetailPathBuilder(CMovementManager* object)
	{
		VERIFY(object);
		m_object = object;
	}

	IC void setup(const PATH& level_path, const u32& path_vertex_index)
	{
		m_level_path = &level_path;
		m_path_vertex_index = path_vertex_index;
	}

	void register_to_process()
	{
		// A restriction change or a large combat transition can invalidate paths
		// for many NPCs in the same frame. The sequential detail-path lane then
		// materializes every request as one long worker tail. Admit a bounded
		// number per frame; rejected requests remain in BuildDetailPath state and
		// are retried by their next scheduler update.
		static u32 submission_frame = u32(-1);
		static u32 submissions = 0;
		if (submission_frame != Device.dwFrame)
		{
			submission_frame = Device.dwFrame;
			submissions = 0;
		}

		if ((psAI_DetailPathMaxPerFrame > 0) &&
			(submissions >= static_cast<u32>(psAI_DetailPathMaxPerFrame)))
			return;

		++submissions;
		m_object->m_wait_for_distributed_computation = true;
		Device.add_to_seq_parallel(
			fastdelegate::FastDelegate0<>(this, &CDetailPathBuilder::process), "detail_path");
	}

	void process_impl(bool separate_computing = true)
	{
		if (separate_computing)
			m_object->m_wait_for_distributed_computation = false;

		{

			m_object->detail().build_path(*m_level_path, m_path_vertex_index);
		}

		{

			m_object->on_build_path();
		}

		if (m_object->detail().failed())
			m_object->m_path_state = CMovementManager::ePathStateBuildLevelPath;
		else
			m_object->m_path_state = CMovementManager::ePathStatePathVerification;
	}

	void __stdcall process()
	{
		process_impl(true);
	}

	IC void remove()
	{
		if (m_object->m_wait_for_distributed_computation)
			m_object->m_wait_for_distributed_computation = false;

		Device.remove_from_seq_parallel(
			fastdelegate::FastDelegate0<>(
				this,
				&CDetailPathBuilder::process
			)
		);
	}
};
