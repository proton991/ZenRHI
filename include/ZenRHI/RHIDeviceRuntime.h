#pragma once
#include "ZenRHI/RHIFrameState.h"
#include "ZenRHI/RHIThread.h"
#include "ZenFoundation/Utils/RefCountPtr.h"
#include <atomic>
#include <functional>

namespace zen
{
// Hears the stable ID of each buffer and texture after its final release, on the releasing thread.
class RHIDestructionListener
{
public:
    virtual void OnResourceDestroyed(uint64_t stableId) = 0;

protected:
    ~RHIDestructionListener() = default;
};

// What a device shares with the objects it creates and with the executor that drives it: the ordered
// worker, frame pacing and the destruction listener. Objects hold a reference, so one released after
// its backend is gone meets a closed cleanup admission instead of freed memory.
class RHIDeviceRuntime : public RefCounted
{
public:
    RHIThread                            thread;
    RHIFrameState                        frameState;
    std::atomic<RHIDestructionListener*> destructionListener{nullptr};
};

using RHIDeviceRuntimePtr = RefCountPtr<RHIDeviceRuntime>;

// An object a device creates. Its final release runs on that device's worker.
class RHIDeviceChild
{
public:
    // Backends bind each object once, when they create it. An unbound object releases inline.
    void BindRuntime(RHIDeviceRuntime* runtime)
    {
        m_runtime = RHIDeviceRuntimePtr(runtime);
    }

    RHIDeviceRuntime* GetRuntime() const
    {
        return m_runtime.Get();
    }

protected:
    // False once the device has closed cleanup admission; the caller then reports the leak.
    bool DispatchCleanup(std::function<void()> cleanup) const
    {
        bool accepted = true;

        if (m_runtime)
        {
            accepted = m_runtime->thread.DispatchCleanup(std::move(cleanup));
        }
        else
        {
            cleanup();
        }

        return accepted;
    }

private:
    RHIDeviceRuntimePtr m_runtime;
};
} // namespace zen
