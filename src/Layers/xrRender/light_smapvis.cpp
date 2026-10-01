#include "stdafx.h"
#include "light.h"
#include "FBasicVisual.h"

smapvis::smapvis()
{
    testQ_id = R_occlusion::invalid_id;
    invalidate();
    frame_sleep = 0;
}

smapvis::~smapvis()
{
    // Deletion may originate from gameplay. Cancellation queues an owner value;
    // it must not poll the D3D11 immediate context on the deleting thread.
    invalidate();
}

void smapvis::invalidate()
{
    xrCriticalSectionGuard guard(query_lock);
    RImplementation.occq_cancel(testQ_id);
    state = state_counting;
    testQ_V = 0;
    testQ_frame = 0;
    test_count = 0;
    test_current = 0;
    frame_sleep = Device.dwFrame + ps_r__LightSleepFrames;
    invisible.clear();
}

void smapvis::begin()
{
    xrCriticalSectionGuard guard(query_lock);
    RImplementation.clear_Counters();

    switch (state)
    {
    case state_counting:
        break;

    case state_working:
        RImplementation.occq_cancel(testQ_id);
        testQ_V = 0;
        mark();
        RImplementation.set_Feedback(this, test_current);
        break;

    case state_usingTC:
        mark();
        break;
    }
}

void smapvis::end()
{
    xrCriticalSectionGuard guard(query_lock);
    u32 ts, td;
    RImplementation.get_Counters(ts, td);
    RImplementation.Stats.ic_total += ts;
    RImplementation.set_Feedback(0, 0);

    switch (state)
    {
    case state_counting:
        if (sleep())
        {
            test_count = ts;
            test_current = 0;
            state = state_working;
        }
        break;

    case state_working:
        if (testQ_V)
        {
            RImplementation.occq_begin(testQ_id);
            RImplementation.marker += 1;
            RImplementation.r_dsgraph_insert_static(testQ_V);
            RImplementation.r_dsgraph_render_graph(0);
            RImplementation.occq_end(testQ_id);
            testQ_frame = Device.dwFrame + 1;
        }
        break;

    case state_usingTC:
        break;
    }
}

void smapvis::flushoccq()
{
    xrCriticalSectionGuard guard(query_lock);
    if (testQ_frame != Device.dwFrame)
        return;

    if ((state != state_working) || (!testQ_V))
        return;

    const u64 fragments = RImplementation.occq_get(testQ_id);
    if (fragments == 0)
    {
        invisible.push_back(testQ_V);
        test_count--;
    }
    else
    {
        test_current++;
    }

    testQ_V = 0;

    if (test_current == test_count && state == state_working)
        state = state_usingTC;
}

void smapvis::resetoccq()
{
    xrCriticalSectionGuard guard(query_lock);
    if (testQ_frame == Device.dwFrame + 1)
        testQ_frame--;

    flushoccq();
}

void smapvis::mark()
{
    xrCriticalSectionGuard guard(query_lock);
    RImplementation.Stats.ic_culled += invisible.size();
    const u32 current_marker = RImplementation.marker + 1;

    for (u32 i = 0; i < invisible.size(); ++i)
        invisible[i]->vis.marker = current_marker;
}

void smapvis::rfeedback_static(dxRender_Visual* V)
{
    xrCriticalSectionGuard guard(query_lock);
    testQ_V = V;
    RImplementation.set_Feedback(0, 0);
}
