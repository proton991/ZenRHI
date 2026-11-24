#pragma once

#include "RHIOptions.h"
#include <atomic>
#include <cstdint>

namespace zen
{
enum class RHIWorkerTime : uint8_t
{
    eTranslation,
    ePipeline,
    eResource,
    eSubmission,
    eWait,
    eIdle,
    eCallerBlocked,
    eCount
};

using RHIWorkerTimes = std::array<uint64_t, static_cast<size_t>(RHIWorkerTime::eCount)>;

// Exclusive CPU nanoseconds. A caller wait runs on the requesting thread and is independent of worker time.
class RHIWorkerTimer
{
public:
    explicit RHIWorkerTimer(RHIWorkerTime category, bool measure = true) :
        m_enabled(measure && RHIOptions::GetInstance().WorkerTimingEnabled()), m_category(category)
    {
        if (m_enabled)
        {
            m_parent = s_active;
            s_active = this;
            m_start  = std::chrono::steady_clock::now();
        }
    }

    ~RHIWorkerTimer()
    {
        if (m_enabled)
        {
            const uint64_t elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
            s_totals[static_cast<size_t>(m_category)].fetch_add(elapsed - m_children, std::memory_order_relaxed);
            s_active = m_parent;
            if (m_parent)
            {
                m_parent->m_children += elapsed;
            }
        }
    }

    RHIWorkerTimer(const RHIWorkerTimer&)            = delete;
    RHIWorkerTimer& operator=(const RHIWorkerTimer&) = delete;

    static RHIWorkerTimes Read()
    {
        RHIWorkerTimes times{};
        if (RHIOptions::GetInstance().WorkerTimingEnabled())
        {
            for (size_t i = 0; i < times.size(); ++i)
            {
                times[i] = s_totals[i].load(std::memory_order_relaxed);
            }
        }
        return times;
    }

private:
    bool                                  m_enabled;
    RHIWorkerTime                         m_category;
    RHIWorkerTimer*                       m_parent{};
    std::chrono::steady_clock::time_point m_start;
    uint64_t                              m_children{};

    static inline thread_local RHIWorkerTimer*                                                  s_active{};
    static inline std::array<std::atomic<uint64_t>, static_cast<size_t>(RHIWorkerTime::eCount)> s_totals{};
};

template <typename Function, typename... Args>
std::invoke_result_t<Function, Args...> TimedRHICall(RHIWorkerTime category, Function fn, Args&&... args)
{
    RHIWorkerTimer timer(category);
    return fn(std::forward<Args>(args)...);
}

// Cumulative backend counters. Consumers subtract snapshots for a frame/run interval.
struct RHIExecutionCounters
{
    uint64_t draws{0};
    uint64_t dispatches{0};
    uint64_t submissions{0};
    uint64_t descriptorHits{0};
    uint64_t descriptorMisses{0};
    uint64_t descriptorInserts{0};
    uint64_t descriptorRetirements{0};
    uint64_t bindlessCaptures{0};
    uint64_t buffersCreated{0};
    uint64_t buffersDestroyed{0};
    uint64_t rayTraces{0};
};

struct RHIExecutionCounterStorage
{
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> dispatches{0};
    std::atomic<uint64_t> submissions{0};
    std::atomic<uint64_t> descriptorHits{0};
    std::atomic<uint64_t> descriptorMisses{0};
    std::atomic<uint64_t> descriptorInserts{0};
    std::atomic<uint64_t> descriptorRetirements{0};
    std::atomic<uint64_t> bindlessCaptures{0};
    std::atomic<uint64_t> buffersCreated{0};
    std::atomic<uint64_t> buffersDestroyed{0};
    std::atomic<uint64_t> rayTraces{0};

    void Increment(std::atomic<uint64_t>& counter)
    {
        if (RHIOptions::GetInstance().ExecutionCountersEnabled())
        {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    }

    RHIExecutionCounters Read() const
    {
        return {draws.load(std::memory_order_relaxed),
                dispatches.load(std::memory_order_relaxed),
                submissions.load(std::memory_order_relaxed),
                descriptorHits.load(std::memory_order_relaxed),
                descriptorMisses.load(std::memory_order_relaxed),
                descriptorInserts.load(std::memory_order_relaxed),
                descriptorRetirements.load(std::memory_order_relaxed),
                bindlessCaptures.load(std::memory_order_relaxed),
                buffersCreated.load(std::memory_order_relaxed),
                buffersDestroyed.load(std::memory_order_relaxed),
                rayTraces.load(std::memory_order_relaxed)};
    }
};
} // namespace zen
