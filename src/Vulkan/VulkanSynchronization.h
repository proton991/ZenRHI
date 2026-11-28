#pragma once
#include "ZenFoundation/Templates/Queue.h"
#include "ZenFoundation/Templates/SmallVector.h"
#include "VulkanCommon.h"
#include "ZenRHI/RHIResource.h"


namespace zen
{
class VulkanDevice;
class VulkanQueue;

class VulkanSemaphore
{
public:
    explicit VulkanSemaphore(VulkanDevice*   pDevice,
                             VkSemaphoreType semaphoreType = VK_SEMAPHORE_TYPE_BINARY,
                             uint64_t        initialValue  = 0);

    virtual ~VulkanSemaphore();

    const RHIError& GetError() const
    {
        return m_error;
    }

    VkSemaphore GetVkHandle() const
    {
        return m_semaphore;
    }

    void SetDebugName(NameID name);

    bool IsTimeline() const
    {
        return m_type == VK_SEMAPHORE_TYPE_TIMELINE;
    }

    VkResult GetCounterValue(uint64_t& value) const;

    VkResult Wait(uint64_t value, uint64_t timeNS) const;

    // CPU-side queue acceptance, not GPU completion or the semaphore's native state.
    // Read and publication are serialized with queue submission.
    uint64_t GetSignalGeneration() const
    {
        return m_signalGeneration;
    }

    uint64_t GetSignalSubmissionSerial(const VulkanQueue* pQueue, uint64_t previousGeneration) const
    {
        return m_pSignalQueue == pQueue && m_signalGeneration != previousGeneration ? m_signalSubmissionSerial : 0;
    }

private:
    friend class VulkanQueue;
    friend class VulkanSemaphoreManager;

    VulkanDevice*      m_pDevice{nullptr};
    VkSemaphore        m_semaphore{VK_NULL_HANDLE};
    VkSemaphoreType    m_type{VK_SEMAPHORE_TYPE_BINARY};
    const VulkanQueue* m_pSignalQueue{nullptr};
    uint64_t           m_signalSubmissionSerial{0};
    uint64_t           m_signalGeneration{0};
    RHIError           m_error{};
};

class VulkanSemaphoreManager
{
public:
    explicit VulkanSemaphoreManager(VulkanDevice* pDevice) : m_pDevice(pDevice) {}

    void Destroy();

    VulkanSemaphore* CreateBinarySemaphore();

    const RHIError& GetLastError() const
    {
        return m_error;
    }

    // The caller must prove all semaphore operations have completed, including an acquired
    // binary semaphore that was never waited on.
    void DestroySemaphore(VulkanSemaphore*& sem);

private:
    VulkanDevice*                m_pDevice{nullptr};
    HeapVector<VulkanSemaphore*> m_usedSemaphores;
    RHIError                     m_error{};
#if defined(ZEN_DEBUG)
    uint32_t m_allocatedSemaphoreCount{0};
#endif
};

class VulkanPipelineBarrier
{
public:
    // for Image-only transitions
    void AddImageBarrier(VkImage                        image,
                         VkImageLayout                  srcLayout,
                         VkImageLayout                  dstLayout,
                         const VkImageSubresourceRange& range,
                         VkAccessFlags                  srcAccess,
                         VkAccessFlags                  dstAccess);

    void AddBufferBarrier(VkBuffer buffer, uint64_t offset, uint64_t size, VkAccessFlags srcAccess, VkAccessFlags dstAccess);

    void AddMemoryBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess);

    void Execute(VkCommandBuffer cmdBuffer, VkPipelineStageFlags srcStageFlags, VkPipelineStageFlags dstStageFlags);

private:
    SmallVector<VkImageMemoryBarrier>  m_imageBarriers;
    SmallVector<VkMemoryBarrier>       m_memoryBarriers;
    SmallVector<VkBufferMemoryBarrier> m_bufferBarriers;
};
} // namespace zen
