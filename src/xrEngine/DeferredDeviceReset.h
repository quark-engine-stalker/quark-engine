#pragma once

#include <Windows.h>

// Coalesces display-mode reset requests until the renderer reaches its next
// main-thread idle boundary. An explicit vid_restart consumes the request by
// calling Clear() as part of CRenderDevice::Reset().
class CDeferredDeviceReset
{
    volatile LONG m_pending = FALSE;

public:
    void Request() noexcept
    {
        InterlockedExchange(&m_pending, TRUE);
    }

    bool Take() noexcept
    {
        return InterlockedExchange(&m_pending, FALSE) != FALSE;
    }

    void Clear() noexcept
    {
        InterlockedExchange(&m_pending, FALSE);
    }
};
