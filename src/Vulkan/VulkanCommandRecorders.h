#pragma once
#include "Vulkan/VulkanHeaders.h"
#include "Vulkan/VulkanMemory.h"
#include "ZenRHI/RHIGPUFrameTiming.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Templates/NameID.h"

namespace zen
{
class VulkanCommandBuffer;
class VulkanDevice;
class VulkanFaultReport;

// Device-loss breadcrumbs: a host-visible started/completed marker pair per label, read back when the device faults.
class VulkanBreadcrumbRecorder
{
public:
    static constexpr uint32_t kMaxBreadcrumbs = 1024;

    // Clears the markers for a new recording.
    void Reset();

    void Begin(VulkanCommandBuffer& commands, NameID name);

    void End(VulkanCommandBuffer& commands);

    // Makes the markers visible to the host. Recorded just before the buffer ends.
    void RecordHostVisibility(VkCommandBuffer handle) const;

    void Report(const VulkanCommandBuffer& commands, VulkanFaultReport& report) const;

    // Frees the marker buffer and leaves the device's diagnostic list.
    void Release(VulkanCommandBuffer& commands);

private:
    void Write(VulkanCommandBuffer& commands, uint32_t index, uint32_t value);

    VkBuffer               m_buffer{VK_NULL_HANDLE};
    VulkanMemoryAllocation m_allocation;
    HeapVector<NameID>     m_names;
    HeapVector<uint32_t>   m_open;
    HeapVector<NameID>     m_openLabels;
    bool                   m_registered{false};
};

// GPU timing: a timestamp pair per scope in a pooled query pool, published once the buffer's work completes. Every
// recording also times itself as part of the frame interval.
class VulkanGPUTimingRecorder
{
public:
    static constexpr uint32_t kMaxScopes = 512;

    // Heavy startup work can record over 180 native buffers before steady-state reuse.
    // Bound profiler storage while accommodating startup and the frames in flight.
    static constexpr uint32_t kMaxPools = 256;

    void BeginRecording(VulkanCommandBuffer& commands);

    void EndRecording(VulkanCommandBuffer& commands);

    // Native acceptance, rather than vkEndCommandBuffer, closes the frame capture boundary.
    void CloseFrameRecording(VulkanDevice& device);

    void Begin(VulkanCommandBuffer& commands, const RHIGPUTimingPtr& result);

    void End(VulkanCommandBuffer& commands, const RHIGPUTimingPtr& result);

    // After completion: publishes every scope and returns the query pool.
    void Resolve(VulkanDevice& device);

    // Without completion: every open scope is discarded.
    void Discard(VulkanDevice& device);

private:
    struct Scope
    {
        RHIGPUTimingPtr result;
        bool            ended{false};
    };

    RHIGPUTimingStatus PreparePool(VulkanCommandBuffer& commands);

    void ReleasePool(VulkanDevice& device);

    VkQueryPool       m_pool{VK_NULL_HANDLE};
    HeapVector<Scope> m_scopes;
    uint32_t          m_validBits{0};
    double            m_period{0};
    bool              m_reset{false};
    RHIGPUTimingPtr   m_frameInterval;
    bool              m_frameRecording{false};
};
} // namespace zen
