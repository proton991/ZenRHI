#pragma once

#include "ZenFoundation/Templates/FrameTimeline.h"

namespace zen
{
enum class RHIFrameNumber : uint64_t
{
    Invalid = ~0ull
};

enum class RHIFrameSlot : uint32_t
{
};

constexpr uint32_t ToIndex(RHIFrameSlot slot)
{
    return static_cast<uint32_t>(slot);
}

constexpr uint64_t ToValue(RHIFrameNumber frameNumber)
{
    return static_cast<uint64_t>(frameNumber);
}

class RHIFrameState
{
public:
    using Number                                     = RHIFrameNumber;
    using Slot                                       = RHIFrameSlot;
    static constexpr uint32_t kMaxFramesInFlight     = 4u;
    static constexpr uint32_t kDefaultFramesInFlight = 3u;

    void Init(uint32_t numFramesInFlight)
    {
        m_timeline.Init(numFramesInFlight);
    }

    void Advance()
    {
        m_timeline.Advance();
    }

    Number GetFrameNumber()
    {
        return m_timeline.GetFrameNumber();
    }

    Slot GetFrameSlot()
    {
        return m_timeline.GetFrameSlot();
    }

    uint32_t GetNumFramesInFlight() const
    {
        return m_timeline.GetNumFramesInFlight();
    }

    uint64_t NumFramesSince(Number frame) const
    {
        return m_timeline.NumFramesSince(frame);
    }

private:
    TFrameTimeline<RHIFrameState> m_timeline;
};
} // namespace zen

extern zen::RHIFrameState GRHIFrameState;
