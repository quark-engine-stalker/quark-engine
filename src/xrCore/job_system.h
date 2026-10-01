#pragma once

namespace xr_jobs
{
using job_function = void (*)(void*);

enum class priority : u8
{
    high = 0,
    normal = 1,
    background = 2
};

class XRCORE_API __declspec(align(64)) task_group
{
public:
    task_group();
    ~task_group();

    task_group(const task_group&) = delete;
    task_group& operator=(const task_group&) = delete;

    bool empty() const;

public:
    // Internal completion counter used by the pool. Do not modify directly.
    volatile LONG pending_;
};

// Lifetime gate for jobs that reference an owning object such as CLevel.
// cancel_and_wait() closes the gate, skips queued jobs and waits for already
// running jobs before the owner releases its memory.
class XRCORE_API task_scope
{
public:
    task_scope();
    ~task_scope();

    task_scope(const task_scope&) = delete;
    task_scope& operator=(const task_scope&) = delete;

    bool accepting() const;
    void cancel_and_wait();

    // Internal admission/completion API used by the worker pool.
    bool try_acquire(u32& generation);
    bool should_execute(u32 generation) const;
    void release();

private:
    SRWLOCK admission_lock_;
    __declspec(align(4)) volatile LONG accepting_;
    __declspec(align(4)) volatile LONG generation_;
    __declspec(align(4)) volatile LONG pending_;
};

// total_threads includes the calling/main thread. Zero selects the engine default
// and honors the existing -max-threads command-line override.
XRCORE_API bool initialize(u32 total_threads = 0);
XRCORE_API void shutdown();

XRCORE_API u32 worker_count();
XRCORE_API u32 total_thread_count();
// Number of pool workers that can make progress in this call, including the
// caller when it is not already occupying a worker-pool thread.
XRCORE_API u32 available_thread_count();
XRCORE_API bool is_worker_thread();

// A failed pool initialization degrades to synchronous execution, preserving
// correctness instead of dropping work.
XRCORE_API bool submit(job_function function, void* context, task_group* group = nullptr,
    priority job_priority = priority::normal);

// Publish repeated workers that share one dynamic workload in one queue lock
// and one semaphore release. Each worker invokes function(context) once.
XRCORE_API bool submit_many(job_function function, void* context, u32 count,
    task_group* group = nullptr, priority job_priority = priority::normal);

// Scoped submit is rejected after task_scope::cancel_and_wait() starts. Queued
// jobs from a cancelled generation are completed without invoking their callback.
XRCORE_API bool submit_scoped(job_function function, void* context, task_scope& scope,
    task_group* group = nullptr, priority job_priority = priority::normal);

XRCORE_API void wait(task_group& group);
XRCORE_API void wait_idle();
} // namespace xr_jobs
