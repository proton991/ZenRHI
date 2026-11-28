#pragma once
#include "VulkanHeaders.h"
#include "Vulkan/VulkanExtension.h"
#include "ZenRHI/RHICommandList.h"
#include "ZenFoundation/Utils/UniquePtr.h"
#include "ZenFoundation/Utils/Mutex.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include <array>
#include <string>

namespace zen
{
class RHIDeviceDesc;
class VulkanRHI;
class VulkanQueue;
class VulkanSemaphoreManager;
class VulkanCommandBuffer;

// Queue families that share a resource without ownership transfers.
struct VulkanQueueSharing
{
    std::array<uint32_t, 3> families{};
    uint32_t                count{0};
};

class VulkanDevice
{
public:
    // pRHI owns the device and must outlive it.
    VulkanDevice(VulkanRHI* pRHI, VkPhysicalDevice gpu);

    VulkanRHI* GetRHI() const
    {
        return m_pRHI;
    }

    // Empty means the device satisfies the backend's minimum capability profile.
    // Why the GPU cannot host a device created from desc; empty when it can.
    static std::string GetUnsupportedReason(VkPhysicalDevice gpu, const RHIDeviceDesc& desc, RHIError* error = nullptr);

    RHIError InitChecked();

    void Destroy();

    // Drain retained queue ownership while allocators and descriptor managers still exist.
    void DestroyQueues();

    void RegisterDiagnosticBuffer(VulkanCommandBuffer* buffer);

    void UnregisterDiagnosticBuffer(VulkanCommandBuffer* buffer);

    void ReportDeviceLoss(const char* operation);

    VkResult AcquireGPUTimingPool(VkQueryPool& pool);

    // Call only after all referencing submissions retire, or the recording is discarded.
    void ReleaseGPUTimingPool(VkQueryPool pool);

    VkDevice GetVkHandle() const
    {
        return m_device;
    }

    VkPipelineCache GetPipelineCache() const
    {
        return m_pipelineCache;
    }

    const VkPhysicalDeviceProperties& GetPhysicalDeviceProperties() const
    {
        return m_gpuProps;
    }

    DeviceExtensionFlags& GetExtensionFlags()
    {
        return m_extensionFlags;
    }

    VkPhysicalDevice GetPhysicalDeviceHandle() const
    {
        return m_gpu;
    }

    void SetObjectName(VkObjectType type, uint64_t handle, NameID name);

    const VkPhysicalDeviceDescriptorIndexingProperties& GetDescriptorIndexingProperties() const
    {
        return m_descriptorIndexingProperties;
    }

    const VkPhysicalDeviceAccelerationStructurePropertiesKHR& GetAccelerationStructureProperties() const
    {
        return m_accelerationStructureProperties;
    }


    VulkanSemaphoreManager* GetSemaphoreManager() const
    {
        return m_pSemaphoreManager;
    }

    VulkanQueue* GetGfxQueue() const
    {
        return m_pGfxQueue;
    }

    VulkanQueue* GetComputeQueue() const
    {
        return m_pComputeQueue;
    }

    VulkanQueue* GetTransferQueue() const
    {
        return m_pTransferQueue;
    }

    // Graphics and compute, plus the transfer family for resources with transfer usage. Fixed with the queues.
    const VulkanQueueSharing& GetQueueSharing(bool transferUsage) const
    {
        return m_queueSharing[transferUsage ? 1 : 0];
    }

    RHIQueueCapabilities GetQueueCapabilities() const;

    VulkanQueue* GetQueue(RHICommandContextType type) const
    {
        VulkanQueue* result{};

        if (type == RHICommandContextType::eGraphics)
        {
            result = m_pGfxQueue;
        }
        else if (type == RHICommandContextType::eAsyncCompute)
        {
            result = m_pComputeQueue;
        }
        else if (type == RHICommandContextType::eTransfer)
        {
            result = m_pTransferQueue;
        }
        else
        {
            result = m_pGfxQueue;
        }

        return result;
    }

    const VkQueueFamilyProperties& GetQueueFamilyProperties(uint32_t familyIndex) const
    {
        return m_queueFamilyProps[familyIndex];
    }

    const VkPhysicalDeviceFeatures& GetPhysicalDeviceFeatures()
    {
        return m_enabledFeatures;
    }


    void WaitForIdle();

private:
    RHIError InitializeSupportedDevice();
    RHIError SetupDevice(VulkanDeviceExtensionSelection& selection);

    VulkanRHI*       m_pRHI{nullptr};
    VkPhysicalDevice m_gpu{VK_NULL_HANDLE};

    // gpu hardware properties
    VkPhysicalDeviceProperties m_gpuProps{};

    // logical device
    VkDevice                         m_device{VK_NULL_HANDLE};
    VkPipelineCache                  m_pipelineCache{VK_NULL_HANDLE};
    Mutex                            m_timingPoolMutex;
    HeapVector<VkQueryPool>          m_timingPools;
    HeapVector<VkQueryPool>          m_freeTimingPools;
    HeapVector<VulkanCommandBuffer*> m_diagnosticBuffers;
    bool                             m_faultReported{false};

    // basic features
    // What the GPU reports, and what this device enabled; capability queries read the enabled set.
    VkPhysicalDeviceFeatures m_supportedFeatures{};
    VkPhysicalDeviceFeatures m_enabledFeatures{};

    // descriptor indexing properties
    VkPhysicalDeviceDescriptorIndexingProperties       m_descriptorIndexingProperties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_accelerationStructureProperties{};

    DeviceExtensionFlags m_extensionFlags{};

    HeapVector<VkQueueFamilyProperties> m_queueFamilyProps;

    HeapVector<NameID> m_extensions;

    // queue infos
    VulkanQueue* m_pGfxQueue{nullptr};
    VulkanQueue* m_pComputeQueue{nullptr};
    VulkanQueue* m_pTransferQueue{nullptr};

    std::array<VulkanQueueSharing, 2> m_queueSharing{};

    VulkanSemaphoreManager* m_pSemaphoreManager{nullptr};
};
} // namespace zen
