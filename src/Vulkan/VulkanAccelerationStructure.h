#pragma once
#include "ZenRHI/RHIAccelerationStructure.h"
#include "VulkanHeaders.h"

namespace zen
{
class VulkanRHI;
class VulkanAccelerationStructure : public RHIAccelerationStructure
{
public:
    static RHIResult<VulkanAccelerationStructure*> CreateObject(VulkanRHI*                                pRHI,
                                                                const RHIAccelerationStructureCreateInfo& info);

    static RHIAccelerationStructureBuildSizes GetBuildSizes(VulkanRHI& rhi, const RHIAccelerationStructureBuildDesc& desc);

    static bool ValidateDescription(VulkanRHI& rhi, const RHIAccelerationStructureBuildDesc& desc, bool requireBuffers);

    bool ValidateBuild(const RHIAccelerationStructureBuildInfo& info) const;

    static VkAccelerationStructureInstanceKHR EncodeInstance(const RHIAccelerationStructureInstance& instance,
                                                             uint64_t                                address);

    void RecordBuild(VkCommandBuffer commandBuffer, const RHIAccelerationStructureBuildInfo& info, uint64_t instanceAddress);

    uint64_t GetDeviceAddress() const
    {
        return m_deviceAddress;
    }

    VkAccelerationStructureKHR GetVkHandle() const
    {
        return m_vkHandle;
    }

    bool IsAsyncComputeAccessible() const override
    {
        return m_pStorageBuffer != nullptr && m_pStorageBuffer->IsAsyncComputeAccessible();
    }

protected:
    RHIError Init();

    template <typename T, typename... Args> friend T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args);

    void Destroy() override;

private:
    VulkanAccelerationStructure(VulkanRHI* pRHI, const RHIAccelerationStructureCreateInfo& info);

    VulkanRHI* m_pRHI{nullptr};

    bool IsUpdateCompatible(const RHIAccelerationStructureBuildDesc& desc) const;

    VkAccelerationStructureKHR m_vkHandle{VK_NULL_HANDLE};
    RHIBuffer*                 m_pStorageBuffer{nullptr};
    uint64_t                   m_deviceAddress{0};
    // Recording metadata only. Submission success and generation publication belong
    // to the caller; discarded builds must never be reused as update sources.
    bool                                         m_buildRecorded{false};
    bool                                         m_allowUpdate{false};
    uint32_t                                     m_instanceCount{0};
    HeapVector<RHIAccelerationStructureGeometry> m_geometries;
    HeapVector<uint64_t>                         m_indexIdentities;
    HeapVector<uint64_t>                         m_opacityIndexIdentities;
    HeapVector<RHIAccelerationStructure*>        m_sortedReferences;
    mutable RHIAccelerationStructureBuildSizes   m_cachedTLASSizes;
    mutable size_t                               m_cachedInstanceCount{0};
    mutable bool                                 m_cachedAllowUpdate{false};
};
} // namespace zen
