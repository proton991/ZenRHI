#pragma once
#include "ZenRHI/RHIResource.h"
#include "VulkanHeaders.h"
#include "VulkanMemory.h"

namespace zen
{
class VulkanRHI;
class VulkanLifetimeTracker;
class VulkanBuffer : public RHIBuffer
{
public:
    static RHIResult<VulkanBuffer*> CreateObject(VulkanRHI* pRHI, const RHIBufferCreateInfo& createInfo);

    bool IsAsyncComputeAccessible() const override
    {
        return true; // Engine allocation sharing includes graphics and compute families.
    }

    uint64_t GetDeviceAddress() const override
    {
        return m_deviceAddress;
    }

    uint8_t* Map() override;

    void Unmap() override;

    bool SetTexelFormat(DataFormat format) override;

    VkBuffer GetVkBuffer() const
    {
        return m_vkBuffer;
    }

    const VulkanMemoryAllocation& GetMemoryAllocation() const
    {
        return m_memAlloc;
    }

    uint64_t GetAllocationSize() const override
    {
        return m_memAlloc.info.size;
    }

    VkBufferView GetVkBufferView() const
    {
        return m_bufferView;
    }

    bool IsHostReadback() const
    {
        return m_allocateType == RHIBufferAllocateType::eCPURead;
    }

protected:
    RHIError Init();

    template <typename T, typename... Args> friend T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args);

    void Destroy() override;

private:
    VulkanBuffer(VulkanRHI* pRHI, const RHIBufferCreateInfo& createInfo) : RHIBuffer(createInfo), m_pRHI(pRHI) {}

    VulkanRHI* m_pRHI{nullptr};

    bool SetTexelFormatOnRHIThread(DataFormat format);

    uint64_t               m_deviceAddress{0};
    VkBuffer               m_vkBuffer{VK_NULL_HANDLE};
    VkBufferView           m_bufferView{VK_NULL_HANDLE};
    DataFormat             m_texelFormat{DataFormat::eUndefined};
    VulkanMemoryAllocation m_memAlloc{};
};
} // namespace zen
