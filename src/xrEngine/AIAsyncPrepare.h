#pragma once

#include "../xrCore/job_system.h"

namespace AIAsyncPrepare
{
using prepare_function = void (*)(void*);

enum class TaskStatus : u8
{
	Idle,
	Running,
	Ready
};

// Reusable ownership/lifetime primitive for AI read-only prepare work. The
// workload owns its snapshot and result storage; Task owns only submission and
// completion state. The caller never waits on the gameplay hot path.
class ENGINE_API Task
{
public:
	Task();
	~Task();

	Task(const Task&) = delete;
	Task& operator=(const Task&) = delete;

	bool submit(prepare_function function, void* context);
	TaskStatus status() const;
	bool ready() const;
	bool retire_ready();
	void wait();

private:
	static void worker_entry(void* context);

	xr_jobs::task_group m_group;
	prepare_function m_function;
	void* m_context;
	volatile LONG m_status;
};
} // namespace AIAsyncPrepare
