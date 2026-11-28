#pragma once
#include "ZenRHI/RHIDiagnostics.h"
#include <span>

#include "ZenRHI/RHICommon.h"
#include "ZenFoundation/Templates/SmallVector.h"
#include "ZenFoundation/Templates/HashMap.h"
#include "ZenFoundation/Utils/Helpers.h"
#include "ZenRHI/RHIResource.h"
#include "VulkanHeaders.h"
#include "Vulkan/VulkanSetLayout.h"

namespace zen
{
class VulkanRHI;

// Includes the actual global set even when the shader does not declare its runtime arrays.
bool ShaderLayoutFitsLimits(const RHIShaderGroupInfo&                                 info,
                            const VkPhysicalDeviceLimits&                             limits,
                            const VkPhysicalDeviceDescriptorIndexingProperties&       indexingLimits,
                            const VkPhysicalDeviceAccelerationStructurePropertiesKHR& accelerationLimits,
                            const RHIBindlessHeapCapacities&                          globalCapacities);

// Fragment-stage resources plus color attachments, against maxPerStageResources and its update-after-bind limit.
bool FragmentResourcesFitLimits(NameID                                              name,
                                const RHIShaderResourceDescriptorTable&             table,
                                uint64_t                                            globalImages,
                                uint32_t                                            colorAttachmentCount,
                                const VkPhysicalDeviceLimits&                       limits,
                                const VkPhysicalDeviceDescriptorIndexingProperties& indexingLimits);

class VulkanDevice;
class VulkanShader : public RHIShader
{
public:
    static RHIResult<VulkanShader*> CreateObject(VulkanRHI* pRHI, const RHIShaderCreateInfo& createInfo);

    uint32_t GetNumShaderStages() const
    {
        return m_stageCreateInfos.size();
    }

    const VkPipelineShaderStageCreateInfo* GetStageCreateInfoData() const
    {
        return m_stageCreateInfos.data();
    }

    VkPipelineLayout GetVkPipelineLayout() const
    {
        return m_pipelineLayout;
    }

    uint32_t GetNumDescriptorSetLayouts() const
    {
        return m_setLayouts.size();
    }

    const VulkanSetLayout& GetSetLayout(uint32_t set) const
    {
        ZEN_RHI_VERIFY(set < m_setLayouts.size());

        return *m_setLayouts[set];
    }

    VkShaderStageFlags GetPushConstantsStageFlags() const
    {
        return m_pushConstantsStageFlags;
    }

    const VkPipelineVertexInputStateCreateInfo* GetVertexInputStateCreateInfoData() const
    {
        return &m_vertexInputInfo.stateCI;
    }

    bool HasGlobalBindlessSet() const
    {
        return m_hasGlobalBindlessSet;
    }

protected:
    RHIError Init();

    template <typename T, typename... Args> friend T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args);

    void Destroy() override;

private:
    VulkanShader(VulkanRHI* pRHI, const RHIShaderCreateInfo& createInfo) : RHIShader(createInfo), m_pRHI(pRHI) {}

    VulkanRHI* m_pRHI{nullptr};



    RHIError InitNativeObjects(const RHIShaderGroupInfo& sgInfo);

    // False when an override's type differs from the reflected constant's.
    bool ApplySpecializationOverrides(RHIShaderGroupInfo& sgInfo) const;

    void BuildSpecializationInfo(std::span<const RHIShaderSpecializationConstant> constants);

    bool CreateShaderModules();

    void BuildVertexInput(const RHIShaderGroupInfo& sgInfo);

    bool CreatePipelineLayout(const RHIShaderGroupInfo& sgInfo);

    struct VertexInputInfo
    {
        SmallVector<VkVertexInputBindingDescription>   vkBindings;
        SmallVector<VkVertexInputAttributeDescription> vkAttributes;
        VkPipelineVertexInputStateCreateInfo           stateCI{};
    } m_vertexInputInfo;
    HeapVector<VkSpecializationMapEntry>         m_spcMapEntries{};
    HeapVector<uint32_t>                         m_specializationData{};
    VkSpecializationInfo                         m_specializationInfo{};
    VkShaderStageFlags                           m_pushConstantsStageFlags{};
    SmallVector<VkPipelineShaderStageCreateInfo> m_stageCreateInfos;

    // Shared layouts, one per set; set 0 is the global heap's. Empty unless every set has one.
    SmallVector<const VulkanSetLayout*, kInlineDescriptorSetCount> m_setLayouts;

    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    bool             m_hasGlobalBindlessSet{false};
};

class VulkanPipeline : public RHIPipeline
{
public:
    static RHIResult<VulkanPipeline*> CreateObject(VulkanRHI* pRHI, const RHIGfxPipelineCreateInfo& createInfo);

    static RHIResult<VulkanPipeline*> CreateObject(VulkanRHI* pRHI, const RHIComputePipelineCreateInfo& createInfo);

    static RHIResult<VulkanPipeline*> CreateObject(VulkanRHI* pRHI, const RHIRayTracingPipelineCreateInfo& createInfo);

    const std::array<VkStridedDeviceAddressRegionKHR, RHIShaderBindingTableLayout::Count>& GetShaderBindingTable() const
    {
        return m_shaderBindingTable;
    }

    VkPipelineBindPoint GetVkPipelineBindPoint() const
    {
        return m_bindPoint;
    }

    VkPipeline GetVkPipeline() const
    {
        return m_vkPipeline;
    }

    bool UsesDynamicState(RHIDynamicState state) const
    {
        return m_gfxStates.dynamicStates.enabledStates.test(ToUnderlying(state));
    }

    VkPipelineLayout GetVkPipelineLayout() const
    {
        return static_cast<VulkanShader*>(m_pShader)->GetVkPipelineLayout();
    }

    VkShaderStageFlags GetPushConstantsStageFlags() const
    {
        return static_cast<VulkanShader*>(m_pShader)->GetPushConstantsStageFlags();
    }

protected:
    RHIError Init();

    template <typename T, typename... Args> friend T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args);

    void Destroy() override;

private:
    VulkanPipeline(VulkanRHI* pRHI, const RHIGfxPipelineCreateInfo& createInfo) : RHIPipeline(createInfo), m_pRHI(pRHI) {}

    VulkanPipeline(VulkanRHI* pRHI, const RHIComputePipelineCreateInfo& createInfo) : RHIPipeline(createInfo), m_pRHI(pRHI) {}

    VulkanPipeline(VulkanRHI* pRHI, const RHIRayTracingPipelineCreateInfo& createInfo) : RHIPipeline(createInfo), m_pRHI(pRHI)
    {}

    VulkanRHI* m_pRHI{nullptr};

    RHIError InitGraphics(const RHIRenderingLayout& layout);

    RHIError InitCompute();

    RHIError InitRayTracing(const RHIRayTracingPipelineCreateInfo& createInfo, const RHIShaderBindingTableLayout& layout);

    bool InitShaderBindingTable(std::span<const RHIRayTracingShaderGroup> groups, const RHIShaderBindingTableLayout& layout);

    VkPipeline          m_vkPipeline{VK_NULL_HANDLE};
    VkPipelineBindPoint m_bindPoint{VK_PIPELINE_BIND_POINT_GRAPHICS};
    // Immutable after creation, and retained with the pipeline through GPU retirement.
    RHIResourcePtr<RHIBuffer>                                                       m_shaderBindingTableBuffer;
    std::array<VkStridedDeviceAddressRegionKHR, RHIShaderBindingTableLayout::Count> m_shaderBindingTable{};
};
} // namespace zen
