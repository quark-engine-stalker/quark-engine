#include "stdafx.h"
#pragma hdrstop

#include "job_system.h"
#include "engine_error_logger.h"

#include <process.h>
#include <thread>

namespace xr_jobs
{
namespace
{
constexpr u32 queue_capacity = 4096;
constexpr u32 priority_count = 3;
constexpr u32 normal_fairness_interval = 8;
constexpr u32 background_fairness_interval = 64;
static_assert(static_cast<u32>(priority::background) + 1 == priority_count, "priority queue count mismatch");

using wait_on_address_function = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using wake_by_address_all_function = VOID(WINAPI*)(PVOID);

struct address_wait_api
{
    wait_on_address_function wait = nullptr;
    wake_by_address_all_function wake_all = nullptr;

    address_wait_api()
    {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel)
            return;

        wait = reinterpret_cast<wait_on_address_function>(GetProcAddress(kernel, "WaitOnAddress"));
        wake_all = reinterpret_cast<wake_by_address_all_function>(GetProcAddress(kernel, "WakeByAddressAll"));
    }
};

address_wait_api& get_address_wait_api()
{
    static address_wait_api api;
    return api;
}

void wait_for_address_change(volatile LONG* address, LONG expected)
{
    address_wait_api& api = get_address_wait_api();
    if (api.wait)
    {
        api.wait(address, &expected, sizeof(expected), 1);
        return;
    }

    // Windows 7 compatibility fallback.
    Sleep(1);
}

void wake_address_waiters(volatile LONG* address)
{
    address_wait_api& api = get_address_wait_api();
    if (api.wake_all)
        api.wake_all(const_cast<LONG*>(address));
}

struct job_record
{
    job_function function = nullptr;
    void* context = nullptr;
    task_group* group = nullptr;
    task_scope* scope = nullptr;
    u32 scope_generation = 0;
    priority job_priority = priority::normal;
};

struct __declspec(align(64)) priority_queue
{
    CRITICAL_SECTION lock{};
    u32 head = 0;
    u32 tail = 0;
    u32 size = 0;
    job_record records[queue_capacity]{};
};

class job_system_impl;
thread_local bool worker_thread_ = false;
thread_local u32 worker_dispatch_sequence_ = 0;

struct worker_startup
{
    job_system_impl* owner = nullptr;
    u32 index = 0;
};

class job_system_impl
{
public:
    job_system_impl()
    {
        for (u32 index = 0; index < priority_count; ++index)
            InitializeCriticalSectionAndSpinCount(&queues_[index].lock, 4000);
    }

    ~job_system_impl()
    {
        shutdown();
        for (u32 index = 0; index < priority_count; ++index)
            DeleteCriticalSection(&queues_[index].lock);
    }

    bool initialize(u32 total_threads)
    {
        if (initialized_)
            return true;

        total_threads_ = resolve_total_threads(total_threads);
        worker_count_ = total_threads_ > 1 ? total_threads_ - 1 : 0;

        accepting_ = TRUE;
        stopping_ = FALSE;
        outstanding_ = 0;
        for (u32 index = 0; index < priority_count; ++index)
        {
            queues_[index].head = 0;
            queues_[index].tail = 0;
            queues_[index].size = 0;
        }

        if (worker_count_ == 0)
        {
            initialized_ = TRUE;
            return true;
        }

        semaphore_ = CreateSemaphoreW(nullptr, 0, 0x7fffffffL, nullptr);
        if (!semaphore_)
            return false;

        worker_handles_ = xr_alloc<HANDLE>(worker_count_);
        worker_startups_ = xr_alloc<worker_startup>(worker_count_);
        if (!worker_handles_ || !worker_startups_)
        {
            cleanup_failed_initialize(0);
            return false;
        }

        ZeroMemory(worker_handles_, sizeof(HANDLE) * worker_count_);
        ZeroMemory(worker_startups_, sizeof(worker_startup) * worker_count_);

        Debug._initialize(false);

        u32 created = 0;
        for (u32 i = 0; i < worker_count_; ++i)
        {
            worker_startups_[i].owner = this;
            worker_startups_[i].index = i;

            const uintptr_t handle = _beginthreadex(nullptr, 0, &job_system_impl::worker_entry,
                &worker_startups_[i], 0, nullptr);
            if (!handle)
            {
                cleanup_failed_initialize(created);
                return false;
            }

            worker_handles_[i] = reinterpret_cast<HANDLE>(handle);
            ++created;
        }

        initialized_ = TRUE;
        return true;
    }

    void shutdown()
    {
        if (!initialized_)
            return;

        // Stop queue admission first. Nested submissions made by jobs already being
        // drained execute synchronously and cannot be stranded in a dying queue.
        InterlockedExchange(&accepting_, FALSE);
        wait_idle();
        InterlockedExchange(&stopping_, TRUE);

        if (worker_count_ > 0)
        {
            ReleaseSemaphore(semaphore_, static_cast<LONG>(worker_count_), nullptr);
            for (u32 i = 0; i < worker_count_; ++i)
            {
                if (worker_handles_[i])
                {
                    WaitForSingleObject(worker_handles_[i], INFINITE);
                    CloseHandle(worker_handles_[i]);
                }
            }
        }

        xr_free(worker_handles_);
        xr_free(worker_startups_);

        if (semaphore_)
        {
            CloseHandle(semaphore_);
            semaphore_ = nullptr;
        }

        worker_count_ = 0;
        total_threads_ = 1;
        initialized_ = FALSE;
        stopping_ = FALSE;
        accepting_ = FALSE;
    }

    bool submit(job_function function, void* context, task_group* group, priority job_priority,
        task_scope* scope)
    {
        if (!function)
            return false;

        u32 scope_generation = 0;
        if (scope && !scope->try_acquire(scope_generation))
            return false;

        if (!initialized_ || InterlockedCompareExchange(&accepting_, TRUE, TRUE) == FALSE)
        {
            execute_synchronously(function, context, group, scope, scope_generation);
            return true;
        }

        if (worker_count_ == 0)
        {
            execute_synchronously(function, context, group, scope, scope_generation);
            return true;
        }

        if (group)
            InterlockedIncrement(&group->pending_);
        InterlockedIncrement(&outstanding_);

        const job_record job{function, context, group, scope, scope_generation, job_priority};
        while (!try_push(job, job_priority))
        {

            // A producer may help drain only the requested priority range. This
            // prevents bounded-queue deadlocks without running unrelated work.
            if (!try_execute_one(job_priority))
                SwitchToThread();

            if (InterlockedCompareExchange(&accepting_, TRUE, TRUE) == FALSE)
            {
                execute(job);
                return true;
            }
        }

        ReleaseSemaphore(semaphore_, 1, nullptr);
        return true;
    }

    bool submit_many(job_function function, void* context, u32 count, task_group* group,
        priority job_priority)
    {
        if (!count)
            return true;
        if (!function || count > 0x7fffffffu)
            return false;

        // Oversized batches are not expected for fork/join lanes, but retaining
        // the scalar admission path keeps the public API correct for every count.
        if (count > queue_capacity)
        {

            for (u32 index = 0; index < count; ++index)
            {
                if (!submit(function, context, group, job_priority, nullptr))
                    return false;
            }
            return true;
        }

        if (!initialized_ || InterlockedCompareExchange(&accepting_, TRUE, TRUE) == FALSE ||
            worker_count_ == 0)
        {
            execute_synchronously_many(function, context, count, group);
            return true;
        }

        const LONG signed_count = static_cast<LONG>(count);
        if (group)
            InterlockedExchangeAdd(&group->pending_, signed_count);
        InterlockedExchangeAdd(&outstanding_, signed_count);

        const job_record job{function, context, group, nullptr, 0, job_priority};
        while (!try_push_many(job, count, job_priority))
        {

            // Make bounded-queue progress without publishing a partially admitted
            // batch whose completion accounting would be harder to reason about.
            if (!try_execute_one(job_priority))
                SwitchToThread();

            if (InterlockedCompareExchange(&accepting_, TRUE, TRUE) == FALSE)
            {
                for (u32 index = 0; index < count; ++index)
                    execute(job);
                return true;
            }
        }

        // ReleaseSemaphore accepts a count, so all lanes become runnable with one
        // kernel transition instead of one transition per submitted worker.
        ReleaseSemaphore(semaphore_, signed_count, nullptr);
        return true;
    }

    void wait(task_group& group)
    {

        for (;;)
        {
            const LONG pending = InterlockedCompareExchange(&group.pending_, 0, 0);
            if (pending == 0)
                break;

            // A worker may execute another task to prevent nested-job deadlocks.
            // The main thread does not steal arbitrary work from generic waits,
            // avoiding reentrant gameplay execution during loading and teardown.
            if (worker_thread_ && try_execute_one_fair())
                continue;

            LONG expected = pending;
            wait_for_address_change(&group.pending_, expected);
        }
    }

    void wait_idle()
    {

        for (;;)
        {
            const LONG outstanding = InterlockedCompareExchange(&outstanding_, 0, 0);
            if (outstanding == 0)
                break;

            if (worker_thread_ ? try_execute_one_fair() : try_execute_one())
                continue;

            LONG expected = outstanding;
            wait_for_address_change(&outstanding_, expected);
        }
    }

    u32 worker_count() const { return worker_count_; }
    u32 total_thread_count() const { return total_threads_; }

private:
    static unsigned __stdcall worker_entry(void* parameter)
    {
        worker_startup* startup = static_cast<worker_startup*>(parameter);
        job_system_impl* owner = startup->owner;

        string64 thread_name_buffer;
        xr_sprintf(thread_name_buffer, "X-Ray Job Worker #%u", startup->index);
        thread_name(thread_name_buffer);
        _initialize_cpu_thread();
        worker_thread_ = true;
        worker_dispatch_sequence_ = startup->index * 17u;
        QUARK_DIAGNOSTIC_STAGE("worker/idle");
        QUARK_DIAGNOSTIC_BREADCRUMB("jobs", "job worker started");

        owner->worker_loop();

        QUARK_DIAGNOSTIC_BREADCRUMB("jobs", "job worker stopped");
        QUARK_DIAGNOSTIC_CLEAR_STAGE();
        worker_thread_ = false;
        return 0;
    }

    void worker_loop()
    {
        for (;;)
        {
            const DWORD wait_result = WaitForSingleObject(semaphore_, INFINITE);
            if (wait_result != WAIT_OBJECT_0)
                return;

            job_record job;
            if (try_pop_fair(job))
            {
                execute(job);
                continue;
            }

            // Empty semaphore tokens are used only to release blocked workers at
            // shutdown. Under normal operation each token corresponds to one job.
            if (InterlockedCompareExchange(&stopping_, TRUE, TRUE) != FALSE)
                return;
        }
    }

    bool try_push(const job_record& job, priority job_priority)
    {
        const u32 queue_index = static_cast<u32>(job_priority);
        R_ASSERT(queue_index < priority_count);
        priority_queue& queue = queues_[queue_index];

        EnterCriticalSection(&queue.lock);

        if (queue.size == queue_capacity)
        {
            LeaveCriticalSection(&queue.lock);
            return false;
        }

        queue.records[queue.tail] = job;
        queue.tail = (queue.tail + 1) % queue_capacity;
        ++queue.size;

        LeaveCriticalSection(&queue.lock);
        return true;
    }

    bool try_push_many(const job_record& job, u32 count, priority job_priority)
    {
        const u32 queue_index = static_cast<u32>(job_priority);
        R_ASSERT(queue_index < priority_count);
        R_ASSERT(count <= queue_capacity);
        priority_queue& queue = queues_[queue_index];

        EnterCriticalSection(&queue.lock);
        if (queue_capacity - queue.size < count)
        {
            LeaveCriticalSection(&queue.lock);
            return false;
        }

        for (u32 index = 0; index < count; ++index)
        {
            queue.records[queue.tail] = job;
            queue.tail = (queue.tail + 1) % queue_capacity;
        }
        queue.size += count;

        LeaveCriticalSection(&queue.lock);
        return true;
    }

    bool try_pop_exact(job_record& job, u32 queue_index)
    {
        R_ASSERT(queue_index < priority_count);
        priority_queue& queue = queues_[queue_index];
        EnterCriticalSection(&queue.lock);

        if (queue.size == 0)
        {
            LeaveCriticalSection(&queue.lock);
            return false;
        }

        job = queue.records[queue.head];
        queue.head = (queue.head + 1) % queue_capacity;
        --queue.size;
        LeaveCriticalSection(&queue.lock);
        return true;
    }

    bool try_pop(job_record& job, priority maximum_priority = priority::background)
    {
        const u32 last_queue = static_cast<u32>(maximum_priority);
        R_ASSERT(last_queue < priority_count);
        for (u32 queue_index = 0; queue_index <= last_queue; ++queue_index)
        {
            if (try_pop_exact(job, queue_index))
                return true;
        }

        return false;
    }

    bool try_pop_fair(job_record& job)
    {
        const u32 sequence = ++worker_dispatch_sequence_;

        // Strict high-priority dispatch is retained for frame latency, but normal
        // and background queues receive bounded service under sustained high load.
        if ((sequence % background_fairness_interval) == 0)
        {
            if (try_pop_exact(job, static_cast<u32>(priority::background)))
                return true;
            if (try_pop_exact(job, static_cast<u32>(priority::normal)))
                return true;
            return try_pop_exact(job, static_cast<u32>(priority::high));
        }

        if ((sequence % normal_fairness_interval) == 0)
        {
            if (try_pop_exact(job, static_cast<u32>(priority::normal)))
                return true;
            if (try_pop_exact(job, static_cast<u32>(priority::high)))
                return true;
            return try_pop_exact(job, static_cast<u32>(priority::background));
        }

        if (try_pop_exact(job, static_cast<u32>(priority::high)))
            return true;
        if (try_pop_exact(job, static_cast<u32>(priority::normal)))
            return true;
        return try_pop_exact(job, static_cast<u32>(priority::background));
    }

    bool try_execute_one(priority maximum_priority = priority::background)
    {
        if (!semaphore_ || WaitForSingleObject(semaphore_, 0) != WAIT_OBJECT_0)
            return false;

        job_record job;
        if (!try_pop(job, maximum_priority))
        {
            // Semaphore tokens are not tied to a priority. If no eligible job is
            // available, return the token for another worker or waiter.
            ReleaseSemaphore(semaphore_, 1, nullptr);
            return false;
        }

        execute(job);
        return true;
    }

    bool try_execute_one_fair()
    {
        if (!semaphore_ || WaitForSingleObject(semaphore_, 0) != WAIT_OBJECT_0)
            return false;

        job_record job;
        if (!try_pop_fair(job))
        {
            ReleaseSemaphore(semaphore_, 1, nullptr);
            return false;
        }

        execute(job);
        return true;
    }

    void execute_synchronously(job_function function, void* context, task_group* group,
        task_scope* scope, u32 scope_generation)
    {
        if (group)
            InterlockedIncrement(&group->pending_);

        const bool execute_callback = !scope || scope->should_execute(scope_generation);

        if (execute_callback)
            function(context);

        if (group && InterlockedDecrement(&group->pending_) == 0)
            wake_address_waiters(&group->pending_);

        // Scope completion is deliberately last: cancel_and_wait() may allow the
        // owner (and its member task_group) to be destroyed as soon as this reaches 0.
        if (scope)
            scope->release();
    }

    void execute_synchronously_many(job_function function, void* context, u32 count,
        task_group* group)
    {
        const LONG signed_count = static_cast<LONG>(count);
        if (group)
            InterlockedExchangeAdd(&group->pending_, signed_count);

        {
            for (u32 index = 0; index < count; ++index)
                function(context);
        }

        if (group)
        {
            const LONG previous = InterlockedExchangeAdd(&group->pending_, -signed_count);
            R_ASSERT(previous >= signed_count);
            if (previous == signed_count)
                wake_address_waiters(&group->pending_);
        }
    }

    void execute(const job_record& job)
    {

        const bool execute_callback = !job.scope || job.scope->should_execute(job.scope_generation);

        if (execute_callback)
        {
            if (worker_thread_)
                QUARK_DIAGNOSTIC_STAGE_ADDRESS("worker/job-callback", job.function);
            job.function(job.context);
        }

        complete(job);
    }

    void complete(const job_record& job)
    {
        if (job.group && InterlockedDecrement(&job.group->pending_) == 0)
            wake_address_waiters(&job.group->pending_);

        if (InterlockedDecrement(&outstanding_) == 0)
            wake_address_waiters(&outstanding_);

        // Must remain the final access to any owner-bound state in this job record.
        if (job.scope)
            job.scope->release();
    }

    static u32 resolve_total_threads(u32 requested)
    {
        u32 logical_threads = CPU::ID.n_threads;
        if (logical_threads == 0)
            logical_threads = std::thread::hardware_concurrency();
        if (logical_threads == 0)
            logical_threads = 4;

        u32 total = requested ? requested : logical_threads;

        if (!requested && Core.Params)
        {
            constexpr LPCSTR option = "-max-threads";
            LPCSTR location = strstr(Core.Params, option);
            if (location)
            {
                u32 override_value = 0;
                if (sscanf_s(location + xr_strlen(option), "%u", &override_value) == 1 && override_value >= 1)
                    total = override_value;
            }
        }

        if (total < 1)
            total = 1;
        if (total > logical_threads)
            total = logical_threads;
        return total;
    }

    void cleanup_failed_initialize(u32 created_workers)
    {
        InterlockedExchange(&stopping_, TRUE);

        if (semaphore_ && created_workers > 0)
            ReleaseSemaphore(semaphore_, static_cast<LONG>(created_workers), nullptr);

        for (u32 i = 0; i < created_workers; ++i)
        {
            if (worker_handles_[i])
            {
                WaitForSingleObject(worker_handles_[i], INFINITE);
                CloseHandle(worker_handles_[i]);
            }
        }

        xr_free(worker_handles_);
        xr_free(worker_startups_);

        if (semaphore_)
        {
            CloseHandle(semaphore_);
            semaphore_ = nullptr;
        }

        worker_count_ = 0;
        total_threads_ = 1;
        accepting_ = FALSE;
        stopping_ = FALSE;
        initialized_ = FALSE;
    }

private:
    priority_queue queues_[priority_count]{};

    HANDLE semaphore_ = nullptr;
    HANDLE* worker_handles_ = nullptr;
    worker_startup* worker_startups_ = nullptr;
    u32 worker_count_ = 0;
    u32 total_threads_ = 1;

    __declspec(align(64)) volatile LONG outstanding_ = 0;
    __declspec(align(64)) volatile LONG accepting_ = FALSE;
    volatile LONG stopping_ = FALSE;
    bool initialized_ = false;
};

SRWLOCK initialization_lock = SRWLOCK_INIT;
volatile LONG system_published = FALSE;
volatile LONG shutdown_requested = FALSE;

job_system_impl& system_storage()
{
    static job_system_impl system;
    return system;
}

job_system_impl* get_system()
{
    return InterlockedCompareExchange(&system_published, TRUE, TRUE) != FALSE ? &system_storage() : nullptr;
}
} // namespace

task_group::task_group() : pending_(0)
{}

static_assert(alignof(task_group) >= 64, "task_group counter must not share a cache line");
static_assert(sizeof(task_group) >= 64, "task_group must reserve its cache line");

task_group::~task_group()
{
    // A task group owns the completion counter referenced by queued records. Never
    // allow its storage to disappear while workers can still complete into it.
    if (!empty())
        xr_jobs::wait(*this);
}

bool task_group::empty() const
{
    return InterlockedCompareExchange(const_cast<LONG*>(&pending_), 0, 0) == 0;
}

task_scope::task_scope() : accepting_(TRUE), generation_(1), pending_(0)
{
    InitializeSRWLock(&admission_lock_);
}

task_scope::~task_scope()
{
    cancel_and_wait();
}

bool task_scope::accepting() const
{
    return InterlockedCompareExchange(const_cast<LONG*>(&accepting_), TRUE, TRUE) != FALSE;
}

bool task_scope::try_acquire(u32& generation)
{
    AcquireSRWLockShared(&admission_lock_);
    if (InterlockedCompareExchange(&accepting_, TRUE, TRUE) == FALSE)
    {
        ReleaseSRWLockShared(&admission_lock_);
        return false;
    }

    generation = static_cast<u32>(InterlockedCompareExchange(&generation_, 0, 0));
    InterlockedIncrement(&pending_);
    ReleaseSRWLockShared(&admission_lock_);
    return true;
}

bool task_scope::should_execute(u32 generation) const
{
    if (InterlockedCompareExchange(const_cast<LONG*>(&accepting_), TRUE, TRUE) == FALSE)
        return false;

    return static_cast<u32>(InterlockedCompareExchange(const_cast<LONG*>(&generation_), 0, 0)) == generation;
}

void task_scope::release()
{
    if (InterlockedDecrement(&pending_) == 0)
        wake_address_waiters(&pending_);
}

void task_scope::cancel_and_wait()
{
    AcquireSRWLockExclusive(&admission_lock_);
    if (InterlockedCompareExchange(&accepting_, FALSE, FALSE) != FALSE)
    {
        InterlockedExchange(&accepting_, FALSE);
        InterlockedIncrement(&generation_);
    }
    ReleaseSRWLockExclusive(&admission_lock_);

    for (;;)
    {
        const LONG pending = InterlockedCompareExchange(&pending_, 0, 0);
        if (pending == 0)
            return;

        LONG expected = pending;
        wait_for_address_change(&pending_, expected);
    }
}

bool initialize(u32 total_threads)
{
    AcquireSRWLockExclusive(&initialization_lock);

    if (InterlockedCompareExchange(&shutdown_requested, TRUE, TRUE) != FALSE)
    {
        ReleaseSRWLockExclusive(&initialization_lock);
        return false;
    }

    if (InterlockedCompareExchange(&system_published, TRUE, TRUE) != FALSE)
    {
        ReleaseSRWLockExclusive(&initialization_lock);
        return true;
    }

    job_system_impl& system = system_storage();
    const bool initialized = system.initialize(total_threads);
    if (initialized)
        InterlockedExchange(&system_published, TRUE);

    ReleaseSRWLockExclusive(&initialization_lock);
    return initialized;
}

void shutdown()
{
    AcquireSRWLockExclusive(&initialization_lock);
    InterlockedExchange(&shutdown_requested, TRUE);

    if (InterlockedCompareExchange(&system_published, TRUE, TRUE) == FALSE)
    {
        ReleaseSRWLockExclusive(&initialization_lock);
        return;
    }

    // Keep the system published while jobs drain. Nested submissions observe
    // accepting_ == FALSE and execute synchronously instead of touching a dying pool.
    system_storage().shutdown();
    InterlockedExchange(&system_published, FALSE);

    ReleaseSRWLockExclusive(&initialization_lock);
}

u32 worker_count()
{
    job_system_impl* system = get_system();
    return system ? system->worker_count() : 0;
}

u32 total_thread_count()
{
    job_system_impl* system = get_system();
    return system ? system->total_thread_count() : 1;
}

u32 available_thread_count()
{
    const u32 workers = worker_count();
    return workers + (is_worker_thread() ? 0u : 1u);
}

bool is_worker_thread()
{
    return worker_thread_;
}

bool submit(job_function function, void* context, task_group* group, priority job_priority)
{
    if (!function)
        return false;

    job_system_impl* system = get_system();
    if (!system)
    {
        if (!initialize())
        {
            // Preserve correctness when worker creation is unavailable.
            if (group)
                InterlockedIncrement(&group->pending_);
            function(context);
            if (group && InterlockedDecrement(&group->pending_) == 0)
                wake_address_waiters(&group->pending_);
            return true;
        }
        system = get_system();
    }

    return system->submit(function, context, group, job_priority, nullptr);
}

bool submit_many(job_function function, void* context, u32 count, task_group* group,
    priority job_priority)
{
    if (!count)
        return true;
    if (!function || count > 0x7fffffffu)
        return false;

    job_system_impl* system = get_system();
    if (!system)
    {
        if (!initialize())
        {
            const LONG signed_count = static_cast<LONG>(count);
            if (group)
                InterlockedExchangeAdd(&group->pending_, signed_count);
            for (u32 index = 0; index < count; ++index)
                function(context);
            if (group)
            {
                const LONG previous = InterlockedExchangeAdd(&group->pending_, -signed_count);
                R_ASSERT(previous >= signed_count);
                if (previous == signed_count)
                    wake_address_waiters(&group->pending_);
            }
            return true;
        }
        system = get_system();
    }

    return system->submit_many(function, context, count, group, job_priority);
}

bool submit_scoped(job_function function, void* context, task_scope& scope,
    task_group* group, priority job_priority)
{
    if (!function)
        return false;

    job_system_impl* system = get_system();
    if (!system)
    {
        if (!initialize())
        {
            u32 generation = 0;
            if (!scope.try_acquire(generation))
                return false;

            if (group)
                InterlockedIncrement(&group->pending_);
            if (scope.should_execute(generation))
                function(context);
            if (group && InterlockedDecrement(&group->pending_) == 0)
                wake_address_waiters(&group->pending_);
            scope.release();
            return true;
        }
        system = get_system();
    }

    return system->submit(function, context, group, job_priority, &scope);
}

void wait(task_group& group)
{
    job_system_impl* system = get_system();
    if (!system)
    {
        while (!group.empty())
        {
            LONG expected = InterlockedCompareExchange(&group.pending_, 0, 0);
            if (expected != 0)
                wait_for_address_change(&group.pending_, expected);
        }
        return;
    }

    system->wait(group);
}

void wait_idle()
{
    job_system_impl* system = get_system();
    if (system)
        system->wait_idle();
}
} // namespace xr_jobs
