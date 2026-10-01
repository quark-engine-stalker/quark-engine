#include "stdafx.h"
#include "AIAsyncPrepare.h"

namespace AIAsyncPrepare
{
Task::Task() : m_function(nullptr), m_context(nullptr), m_status(static_cast<LONG>(TaskStatus::Idle))
{}

Task::~Task()
{
	wait();
}

bool Task::submit(prepare_function function, void* context)
{
	if (!function || !context || status() != TaskStatus::Idle || !m_group.empty())
		return false;

	m_function = function;
	m_context = context;
	InterlockedExchange(&m_status, static_cast<LONG>(TaskStatus::Running));

	// Prepare work is speculative and has a scheduler-sized window before commit.
	// Keep it below frame-critical high-priority lanes so render/audio/simulation
	// jobs can overtake queued AI precompute without waiting behind it.
	if (xr_jobs::submit(&Task::worker_entry, this, &m_group, xr_jobs::priority::normal))
		return true;

	InterlockedExchange(&m_status, static_cast<LONG>(TaskStatus::Idle));
	m_function = nullptr;
	m_context = nullptr;
	return false;
}

TaskStatus Task::status() const
{
	return static_cast<TaskStatus>(InterlockedCompareExchange(
		const_cast<LONG*>(&m_status), 0, 0));
}

bool Task::ready() const
{
	return status() == TaskStatus::Ready && m_group.empty();
}

bool Task::retire_ready()
{
	if (!ready())
		return false;

	m_function = nullptr;
	m_context = nullptr;
	InterlockedExchange(&m_status, static_cast<LONG>(TaskStatus::Idle));
	return true;
}

void Task::wait()
{
	xr_jobs::wait(m_group);
	if (status() == TaskStatus::Ready)
		retire_ready();
}

void Task::worker_entry(void* context)
{
	Task& task = *static_cast<Task*>(context);

	prepare_function function = task.m_function;
	void* function_context = task.m_context;
	if (function && function_context)
		function(function_context);

	// Release publishes every result write before the main-thread Ready poll.
	InterlockedExchange(&task.m_status, static_cast<LONG>(TaskStatus::Ready));
}
} // namespace AIAsyncPrepare
