#include "stdafx.h"
#include "../xrCore/job_system.h"
#pragma hdrstop

struct TTAPI_WORKER_PARAMS
{
    LPPTTAPI_WORKER_FUNC function = nullptr;
    LPVOID context = nullptr;
};

static TTAPI_WORKER_PARAMS ttapi_fallback_worker;
static TTAPI_WORKER_PARAMS* ttapi_workers = nullptr;
static DWORD ttapi_workers_count = 0;
static DWORD ttapi_assigned_workers = 0;
static BOOL ttapi_initialized = FALSE;

static void ttapi_job_entry(void* context)
{
    TTAPI_WORKER_PARAMS* worker = static_cast<TTAPI_WORKER_PARAMS*>(context);
    if (worker->function)
        worker->function(worker->context);
}

DWORD ttapi_Init(_processor_info* ID)
{
    UNUSED(ID);

    if (ttapi_initialized)
        return ttapi_workers_count;

    xr_jobs::initialize();
    ttapi_workers_count = xr_jobs::total_thread_count();
    if (ttapi_workers_count == 0)
        ttapi_workers_count = 1;

    ttapi_workers = static_cast<TTAPI_WORKER_PARAMS*>(calloc(ttapi_workers_count, sizeof(TTAPI_WORKER_PARAMS)));
    if (!ttapi_workers)
    {
        ttapi_workers = &ttapi_fallback_worker;
        ttapi_workers_count = 1;
    }

    ttapi_initialized = TRUE;
    return ttapi_workers_count;
}

DWORD ttapi_GetWorkersCount()
{
    return ttapi_workers_count ? ttapi_workers_count : 1;
}

VOID ttapi_AddWorker(LPPTTAPI_WORKER_FUNC function, LPVOID context)
{
    R_ASSERT(ttapi_initialized);
    R_ASSERT(ttapi_assigned_workers < ttapi_workers_count);

    if (!ttapi_initialized || !ttapi_workers || ttapi_assigned_workers >= ttapi_workers_count)
        return;

    TTAPI_WORKER_PARAMS& worker = ttapi_workers[ttapi_assigned_workers++];
    worker.function = function;
    worker.context = context;
}

VOID ttapi_RunAllWorkers()
{
    if (ttapi_assigned_workers == 0)
        return;

    xr_jobs::task_group group;
    const DWORD main_worker_index = ttapi_assigned_workers - 1;

    for (DWORD i = 0; i < main_worker_index; ++i)
    {
        if (!xr_jobs::submit(&ttapi_job_entry, &ttapi_workers[i], &group, xr_jobs::priority::high))
            ttapi_job_entry(&ttapi_workers[i]);
    }

    ttapi_job_entry(&ttapi_workers[main_worker_index]);
    xr_jobs::wait(group);

    for (DWORD i = 0; i < ttapi_assigned_workers; ++i)
    {
        ttapi_workers[i].function = nullptr;
        ttapi_workers[i].context = nullptr;
    }

    ttapi_assigned_workers = 0;
}

VOID ttapi_Done()
{
    if (!ttapi_initialized)
        return;

    if (ttapi_workers != &ttapi_fallback_worker)
        free(ttapi_workers);
    ttapi_workers = nullptr;
    ttapi_fallback_worker = {};
    ttapi_workers_count = 0;
    ttapi_assigned_workers = 0;
    ttapi_initialized = FALSE;
}
