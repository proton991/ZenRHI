#include "Vulkan/VulkanCommandRecorders.h"
#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanFaultReport.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanRHI.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace zen
{
void VulkanBreadcrumbRecorder::Reset()
{
    m_names.clear();

    m_open.clear();

    m_openLabels.clear();

    if (m_buffer != VK_NULL_HANDLE)
    {
        std::memset(m_allocation.info.pMappedData, 0, kMaxBreadcrumbs * 2 * sizeof(uint32_t));
    }
}

void VulkanBreadcrumbRecorder::Begin(VulkanCommandBuffer& commands, NameID name)
{
    VulkanDevice* device = commands.GetDevice();

    if (device->GetRHI()->GetDeviceDesc().DeviceLossDiagnostics())
    {
        if (!m_registered)
        {
            device->RegisterDiagnosticBuffer(&commands);

            m_registered = true;
        }

        uint32_t index = UINT32_MAX;

        if (commands.IsOutsideRenderPass() && m_names.size() < kMaxBreadcrumbs)
        {
            if (m_buffer == VK_NULL_HANDLE)
            {
                VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

                info.size  = kMaxBreadcrumbs * 2 * sizeof(uint32_t);

                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                if (!device->GetRHI()
                         ->GetMemoryAllocator()
                         ->AllocBuffer(&info, RHIBufferAllocateType::eCPURead, &m_buffer, &m_allocation)
                         .IsFailure())
                {
                    std::memset(m_allocation.info.pMappedData, 0, size_t(info.size));
                }
            }

            if (m_buffer != VK_NULL_HANDLE)
            {
                index = static_cast<uint32_t>(m_names.size());

                m_names.push_back(name);

                Write(commands, index, 1);
            }
        }

        m_open.push_back(index);

        m_openLabels.push_back(name);
    }
}

void VulkanBreadcrumbRecorder::End(VulkanCommandBuffer& commands)
{
    if (!m_open.empty())
    {
        const uint32_t index = m_open.back();

        m_open.pop_back();

        m_openLabels.pop_back();

        if (index != UINT32_MAX && commands.IsOutsideRenderPass())
        {
            Write(commands, index, 2);
        }
    }
}

void VulkanBreadcrumbRecorder::Write(VulkanCommandBuffer& commands, uint32_t index, uint32_t value)
{
    const VkCommandBuffer handle = commands.GetVkHandle();

    if (commands.GetDevice()->GetExtensionFlags().hasBufferMarker && vkCmdWriteBufferMarkerAMD != nullptr)
    {
        vkCmdWriteBufferMarkerAMD(handle, value == 1 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  m_buffer, (uint64_t(index) * 2 + value - 1) * sizeof(uint32_t), 1);
    }
    else
    {
        // The fill waits for all earlier work, so a marker means everything recorded before it
        // has finished. Later work does not wait for the fill: blocking it would serialize every
        // pass boundary. RecordHostVisibility makes the fills visible to the host.
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};

        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer              = m_buffer;
        barrier.offset              = (uint64_t(index) * 2 + value - 1) * sizeof(uint32_t);
        barrier.size                = sizeof(uint32_t);

        vkCmdPipelineBarrier(handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                             &barrier, 0, nullptr);

        vkCmdFillBuffer(handle, m_buffer, barrier.offset, barrier.size, value);
    }
}

void VulkanBreadcrumbRecorder::RecordHostVisibility(VkCommandBuffer handle) const
{
    if (m_buffer != VK_NULL_HANDLE)
    {
        VkMemoryBarrier visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER};

        visibility.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        visibility.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

        vkCmdPipelineBarrier(handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &visibility, 0,
                             nullptr, 0, nullptr);
    }
}

void VulkanBreadcrumbRecorder::Report(const VulkanCommandBuffer& commands, VulkanFaultReport& report) const
{
    const volatile uint32_t* values    = static_cast<const volatile uint32_t*>(m_allocation.info.pMappedData);
    const VulkanQueue*       queue     = commands.GetCommandBufferPool()->GetQueue();
    const char*              started   = "none observed";
    const char*              completed = "none observed";

    for (uint32_t i = 0; values != nullptr && i < m_names.size(); ++i)
    {
        const uint32_t startedValue   = values[i * 2];
        const uint32_t completedValue = values[i * 2 + 1];

        if (startedValue != 0 || completedValue != 0)
        {
            started = m_names[i].CStr();
        }

        if (completedValue != 0)
        {
            completed = m_names[i].CStr();
        }
    }

    report.Append("RHI GPU breadcrumb queue={}:{} buffer={} last_started={} last_completed={}\n", queue->GetFamilyIndex(),
                  queue->GetQueueIndex(), reinterpret_cast<void*>(commands.GetVkHandle()), started, completed);

    for (const NameID label : m_openLabels)
    {
        report.Append("open label={}\n", label.CStr());
    }
}

void VulkanBreadcrumbRecorder::Release(VulkanCommandBuffer& commands)
{
    if (m_registered)
    {
        commands.GetDevice()->UnregisterDiagnosticBuffer(&commands);

        m_registered = false;
    }

    if (m_buffer != VK_NULL_HANDLE)
    {
        commands.GetDevice()->GetRHI()->GetMemoryAllocator()->FreeBuffer(m_buffer, m_allocation);

        m_buffer = VK_NULL_HANDLE;
    }
}

void VulkanGPUTimingRecorder::BeginRecording(VulkanCommandBuffer& commands)
{
    VulkanDevice*                  device = commands.GetDevice();
    const VkQueueFamilyProperties& family =
        device->GetQueueFamilyProperties(commands.GetCommandBufferPool()->GetQueue()->GetFamilyIndex());

    m_frameInterval = device->GetRHI()->RegisterNativeGPUFrameRecording(
        (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0);

    m_frameRecording = true;

    Begin(commands, m_frameInterval);
}

void VulkanGPUTimingRecorder::EndRecording(VulkanCommandBuffer& commands)
{
    End(commands, m_frameInterval);

    m_frameInterval.Reset();
}

void VulkanGPUTimingRecorder::CloseFrameRecording(VulkanDevice& device)
{
    if (m_frameRecording)
    {
        device.GetRHI()->ReleaseNativeGPUFrameRecording();

        m_frameRecording = false;
    }
}

void VulkanGPUTimingRecorder::Begin(VulkanCommandBuffer& commands, const RHIGPUTimingPtr& result)
{
    if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
    {
        RHIGPUTimingStatus status = RHIGPUTimingStatus::ePending;

        if (!commands.IsOutsideRenderPass())
        {
            status = RHIGPUTimingStatus::eError;
        }
        else if (m_scopes.size() >= kMaxScopes)
        {
            status = RHIGPUTimingStatus::eDropped;
        }
        else if (std::any_of(m_scopes.begin(), m_scopes.end(),
                             [&result](const Scope& scope) { return scope.result == result; }))
        {
            status = RHIGPUTimingStatus::eError;
        }
        else
        {
            status = PreparePool(commands);
        }

        if (status == RHIGPUTimingStatus::ePending)
        {
            if (!m_reset)
            {
                VulkanDevice*                  device = commands.GetDevice();
                const VkQueueFamilyProperties& family =
                    device->GetQueueFamilyProperties(commands.GetCommandBufferPool()->GetQueue()->GetFamilyIndex());

                if ((family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0)
                {
                    vkCmdResetQueryPool(commands.GetVkHandle(), m_pool, 0, 2 * kMaxScopes);
                }
                else
                {
                    // This buffer exclusively owns the pool. Reuse follows completion/readback
                    // or discard of unsubmitted work; no submitted query can still be using it.
                    vkResetQueryPool(device->GetVkHandle(), m_pool, 0, 2 * kMaxScopes);
                }

                m_reset = true;
            }

            const uint32_t query = static_cast<uint32_t>(m_scopes.size()) * 2;

            m_scopes.push_back({result, false});

            vkCmdWriteTimestamp(commands.GetVkHandle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool, query);
        }
        else
        {
            result->Publish(status);
        }
    }
}

RHIGPUTimingStatus VulkanGPUTimingRecorder::PreparePool(VulkanCommandBuffer& commands)
{
    RHIGPUTimingStatus status = RHIGPUTimingStatus::ePending;

    if (m_pool == VK_NULL_HANDLE)
    {
        VulkanDevice*                  device = commands.GetDevice();
        const VkQueueFamilyProperties& family =
            device->GetQueueFamilyProperties(commands.GetCommandBufferPool()->GetQueue()->GetFamilyIndex());

        m_validBits         = family.timestampValidBits;

        m_period            = device->GetPhysicalDeviceProperties().limits.timestampPeriod;

        const bool canReset = (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0
                                ? vkCmdResetQueryPool != nullptr
                                : device->GetExtensionFlags().hasHostQueryReset && vkResetQueryPool != nullptr;

        if (!canReset || m_validBits == 0 || m_validBits > 64 || !std::isfinite(m_period) || m_period <= 0
            || vkCreateQueryPool == nullptr || vkDestroyQueryPool == nullptr || vkCmdWriteTimestamp == nullptr
            || vkGetQueryPoolResults == nullptr)
        {
            status = RHIGPUTimingStatus::eUnsupported;
        }
        else
        {
            const VkResult acquired = device->AcquireGPUTimingPool(m_pool);

            if (acquired != VK_SUCCESS)
            {
                m_pool = VK_NULL_HANDLE;

                status = acquired == VK_ERROR_TOO_MANY_OBJECTS ? RHIGPUTimingStatus::eDropped : RHIGPUTimingStatus::eError;
            }
        }
    }

    return status;
}

void VulkanGPUTimingRecorder::End(VulkanCommandBuffer& commands, const RHIGPUTimingPtr& result)
{
    if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
    {
        bool ended = false;

        for (size_t index = m_scopes.size(); index > 0 && !ended; --index)
        {
            Scope& scope = m_scopes[index - 1];

            if (scope.result == result && !scope.ended && commands.IsOutsideRenderPass())
            {
                vkCmdWriteTimestamp(commands.GetVkHandle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool,
                                    static_cast<uint32_t>(index - 1) * 2 + 1);

                scope.ended = true;

                ended       = true;
            }
        }

        if (!ended)
        {
            result->Publish(RHIGPUTimingStatus::eError);
        }
    }
}

void VulkanGPUTimingRecorder::Resolve(VulkanDevice& device)
{
    if (!m_scopes.empty())
    {
        struct TimestampValue
        {
            uint64_t ticks{0};
            uint64_t available{0};
        };

        std::array<TimestampValue, 2 * kMaxScopes> values{};

        const uint32_t count = static_cast<uint32_t>(m_scopes.size()) * 2;

        // The queue's timeline proves completion. Never wait for a profiler query.
        const VkResult status =
            vkGetQueryPoolResults(device.GetVkHandle(), m_pool, 0, count, count * sizeof(TimestampValue), values.data(),
                                  sizeof(TimestampValue), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

        device.GetRHI()->CheckDeviceLoss(status, "vkGetQueryPoolResults");

        for (uint32_t index = 0; index < m_scopes.size(); ++index)
        {
            Scope&     scope     = m_scopes[index];
            const bool available = scope.ended && (status == VK_SUCCESS || status == VK_NOT_READY)
                                && values[2 * index].available != 0 && values[2 * index + 1].available != 0;

            if (available)
            {
                scope.result->PublishTimestamps({values[2 * index].ticks, values[2 * index + 1].ticks, m_validBits, m_period});
            }
            else
            {
                scope.result->Publish(scope.ended ? RHIGPUTimingStatus::eError : RHIGPUTimingStatus::eDiscarded);
            }
        }

        m_scopes.clear();

        ReleasePool(device);
    }
}

void VulkanGPUTimingRecorder::Discard(VulkanDevice& device)
{
    CloseFrameRecording(device);

    m_frameInterval.Reset();

    for (Scope& scope : m_scopes)
    {
        scope.result->Publish(RHIGPUTimingStatus::eDiscarded);
    }

    m_scopes.clear();

    ReleasePool(device);

    // A released pool is reset again by the next recording that takes one.
    m_reset = false;
}

void VulkanGPUTimingRecorder::ReleasePool(VulkanDevice& device)
{
    if (m_pool != VK_NULL_HANDLE)
    {
        device.ReleaseGPUTimingPool(m_pool);

        m_pool = VK_NULL_HANDLE;
    }
}
} // namespace zen
