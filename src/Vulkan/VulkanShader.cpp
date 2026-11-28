#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/RHICommon.h"
#include "ZenRHI/RHIResource.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanTypes.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanResourceAllocator.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include "ZenRHI/RHIShaderUtil.h"
#include "Core/RHIValidation.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanSetLayout.h"
#include "Vulkan/VulkanBindlessHeap.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Utils/Errors.h"
#include "ZenFoundation/Utils/Helpers.h"

namespace zen
{
namespace
{
struct DescriptorCounts
{
    uint64_t values[ToUnderlying(VulkanDescriptorLimitClass::eMax)]{};

    void Add(const RHIShaderResourceDescriptor& descriptor)
    {
        const VulkanDescriptorLimitClass descriptorClass = GetVulkanDescriptorTraits(descriptor.type).limitClass;
        if (descriptorClass != VulkanDescriptorLimitClass::eMax)
        {
            // KHR acceleration-structure limits count bindings, unlike the descriptor-element limits.
            values[ToUnderlying(descriptorClass)] +=
                descriptorClass == VulkanDescriptorLimitClass::eAccelerationStructure ? 1 : descriptor.arraySize;
        }
        // A combined image sampler also counts against the sampler limits.
        if (descriptor.type == RHIShaderResourceType::eSamplerWithTexture)
        {
            values[ToUnderlying(VulkanDescriptorLimitClass::eSampler)] += descriptor.arraySize;
        }
    }

    uint64_t ResourceCount() const
    {
        // Combined image samplers count once; separate samplers and acceleration structures are excluded.
        return values[ToUnderlying(VulkanDescriptorLimitClass::eUniformBuffer)]
             + values[ToUnderlying(VulkanDescriptorLimitClass::eStorageBuffer)]
             + values[ToUnderlying(VulkanDescriptorLimitClass::eSampledImage)]
             + values[ToUnderlying(VulkanDescriptorLimitClass::eStorageImage)]
             + values[ToUnderlying(VulkanDescriptorLimitClass::eInputAttachment)];
    }
};

bool CheckShaderLimit(NameID name, const char* limitName, uint64_t count, uint32_t limit)
{
    const bool valid = count <= limit;
    if (!valid)
    {
        ZEN_RHI_LOG_ERROR("Shader '{}' exceeds {}: {} (limit {})", name.CStr(), limitName, count, limit);
    }
    return valid;
}

DescriptorCounts CountStageDescriptors(const RHIShaderResourceDescriptorTable& table, RHIShaderStage stage)
{
    DescriptorCounts counts;
    for (const std::vector<RHIShaderResourceDescriptor>& set : table)
    {
        for (const RHIShaderResourceDescriptor& descriptor : set)
        {
            if (!descriptor.bindless && descriptor.stageFlags.HasFlag(RHIShaderStageToFlagBits(stage)))
            {
                counts.Add(descriptor);
            }
        }
    }
    return counts;
}

bool StageResourcesFitLimits(NameID                                              name,
                             uint64_t                                            ordinaryCount,
                             uint64_t                                            globalImages,
                             uint32_t                                            colorAttachmentCount,
                             const VkPhysicalDeviceLimits&                       limits,
                             const VkPhysicalDeviceDescriptorIndexingProperties& indexingLimits)
{
    bool valid =
        CheckShaderLimit(name, "maxPerStageResources", ordinaryCount + colorAttachmentCount, limits.maxPerStageResources);
    valid &= CheckShaderLimit(name, "maxPerStageUpdateAfterBindResources", ordinaryCount + globalImages + colorAttachmentCount,
                              indexingLimits.maxPerStageUpdateAfterBindResources);
    return valid;
}
} // namespace

bool FragmentResourcesFitLimits(NameID                                              name,
                                const RHIShaderResourceDescriptorTable&             table,
                                uint64_t                                            globalImages,
                                uint32_t                                            colorAttachmentCount,
                                const VkPhysicalDeviceLimits&                       limits,
                                const VkPhysicalDeviceDescriptorIndexingProperties& indexingLimits)
{
    return StageResourcesFitLimits(name, CountStageDescriptors(table, RHIShaderStage::eFragment).ResourceCount(), globalImages,
                                   colorAttachmentCount, limits, indexingLimits);
}

bool ShaderLayoutFitsLimits(const RHIShaderGroupInfo&                                 info,
                            const VkPhysicalDeviceLimits&                             limits,
                            const VkPhysicalDeviceDescriptorIndexingProperties&       indexingLimits,
                            const VkPhysicalDeviceAccelerationStructurePropertiesKHR& accelerationLimits,
                            const RHIBindlessHeapCapacities&                          globalCapacities)
{
    bool valid = CheckShaderLimit(info.name, "maxPushConstantsSize", info.pushConstants.size, limits.maxPushConstantsSize);
    DescriptorCounts total;
    for (const std::vector<RHIShaderResourceDescriptor>& set : info.SRDTable)
    {
        for (const RHIShaderResourceDescriptor& descriptor : set)
        {
            if (!descriptor.bindless)
            {
                total.Add(descriptor);
                if (descriptor.type == RHIShaderResourceType::eUniformBuffer)
                {
                    valid &= CheckShaderLimit(info.name, "maxUniformBufferRange", descriptor.blockSize,
                                              limits.maxUniformBufferRange);
                }
            }
        }
    }

    const uint32_t layoutLimits[] = {
        limits.maxDescriptorSetSamplers,
        std::min(limits.maxDescriptorSetUniformBuffers, limits.maxDescriptorSetUniformBuffersDynamic),
        limits.maxDescriptorSetStorageBuffers,
        limits.maxDescriptorSetSampledImages,
        limits.maxDescriptorSetStorageImages,
        limits.maxDescriptorSetInputAttachments,
        accelerationLimits.maxDescriptorSetAccelerationStructures};
    const uint32_t stageLimits[] = {limits.maxPerStageDescriptorSamplers,
                                    limits.maxPerStageDescriptorUniformBuffers,
                                    limits.maxPerStageDescriptorStorageBuffers,
                                    limits.maxPerStageDescriptorSampledImages,
                                    limits.maxPerStageDescriptorStorageImages,
                                    limits.maxPerStageDescriptorInputAttachments,
                                    accelerationLimits.maxPerStageDescriptorAccelerationStructures};
    const char*    classNames[]  = {"sampler descriptors",
                                    "dynamic uniform-buffer descriptors",
                                    "storage-buffer descriptors",
                                    "sampled-image/texel-buffer descriptors",
                                    "storage-image/texel-buffer descriptors",
                                    "input-attachment descriptors",
                                    "acceleration-structure bindings"};
    static_assert(std::size(layoutLimits) == ToUnderlying(VulkanDescriptorLimitClass::eMax));
    static_assert(std::size(stageLimits) == ToUnderlying(VulkanDescriptorLimitClass::eMax));
    static_assert(std::size(classNames) == ToUnderlying(VulkanDescriptorLimitClass::eMax));

    const uint64_t globalSamplers = globalCapacities.Get(RHIBindlessHeapType::eSampler);
    const uint64_t globalImages   = globalCapacities.GetImageSlots();
    for (uint32_t descriptorClass = 0; descriptorClass < ToUnderlying(VulkanDescriptorLimitClass::eMax); ++descriptorClass)
    {
        valid &= CheckShaderLimit(info.name, classNames[descriptorClass], total.values[descriptorClass],
                                  layoutLimits[descriptorClass]);
    }
    // Only sampled images and samplers occur in the global update-after-bind layout. Vulkan
    // requires each update-after-bind limit to reach its ordinary limit, so the other combined
    // limits follow from the stricter ordinary-layout checks above.
    valid &= CheckShaderLimit(info.name, "combined layout samplers",
                              total.values[ToUnderlying(VulkanDescriptorLimitClass::eSampler)] + globalSamplers,
                              indexingLimits.maxDescriptorSetUpdateAfterBindSamplers);
    valid &= CheckShaderLimit(info.name, "combined layout sampled images",
                              total.values[ToUnderlying(VulkanDescriptorLimitClass::eSampledImage)] + globalImages,
                              indexingLimits.maxDescriptorSetUpdateAfterBindSampledImages);
    for (uint32_t stage = 0; stage < ToUnderlying(RHIShaderStage::eMax); ++stage)
    {
        const DescriptorCounts counts = CountStageDescriptors(info.SRDTable, static_cast<RHIShaderStage>(stage));
        for (uint32_t descriptorClass = 0; descriptorClass < ToUnderlying(VulkanDescriptorLimitClass::eMax); ++descriptorClass)
        {
            valid &= CheckShaderLimit(info.name, classNames[descriptorClass], counts.values[descriptorClass],
                                      stageLimits[descriptorClass]);
        }
        valid &= CheckShaderLimit(info.name, "combined per-stage samplers",
                                  counts.values[ToUnderlying(VulkanDescriptorLimitClass::eSampler)] + globalSamplers,
                                  indexingLimits.maxPerStageDescriptorUpdateAfterBindSamplers);
        valid &= CheckShaderLimit(info.name, "combined per-stage sampled images",
                                  counts.values[ToUnderlying(VulkanDescriptorLimitClass::eSampledImage)] + globalImages,
                                  indexingLimits.maxPerStageDescriptorUpdateAfterBindSampledImages);
        valid &= StageResourcesFitLimits(info.name, counts.ResourceCount(), globalImages, 0, limits, indexingLimits);
    }
    return valid;
}

RHIShader* VulkanRHI::CreateShader(const RHIShaderCreateInfo& createInfo)
{
    RHIWorkerTimer timer(RHIWorkerTime::eResource);
    return AreSubmissionsBlocked() ? nullptr : VulkanShader::CreateObject(this, createInfo).GetValueOr(nullptr);
}

RHIResult<VulkanShader*> VulkanShader::CreateObject(VulkanRHI* pRHI, const RHIShaderCreateInfo& createInfo)
{
    const char* rule = ValidateShaderStages(createInfo.spirv.Get(), pRHI->QueryGPUInfo());

    if (rule != nullptr)
    {
        ZEN_RHI_LOG_ERROR("Shader '{}': {}", createInfo.name.c_str(), rule);
    }

    VulkanShader* shader = rule == nullptr ? NewVulkanResource<VulkanShader>(pRHI, pRHI, createInfo) : nullptr;

    return shader != nullptr ? KeepIfInitialized(shader, shader->Init())
                             : RejectDescription<VulkanShader>(rule, __FILE__, __LINE__);
}

RHIError VulkanShader::Init()
{
    // Replaced once reflection and the layout rules accept the shader.
    RHIError error = MakeRHIError(RHIErrorCode::eInvalidArgument, "Shader reflection or layout rules", __FILE__, __LINE__);

    // Descriptor set counts are a device capability; no engine-wide count applies.
    const uint32_t maxDescriptorSets = m_pRHI->GetDevice()->GetPhysicalDeviceProperties().limits.maxBoundDescriptorSets;

    RHIShaderGroupInfo sgInfo{};

    bool reflected = m_reflection.has_value();

    if (reflected)
    {
        sgInfo = *m_reflection;
    }
    else
    {
        reflected = RHIShaderUtil::ReflectShaderGroupInfo(m_shaderGroupSPIRV, maxDescriptorSets, sgInfo);

        if (reflected)
        {
            m_reflection = sgInfo;
        }
    }

    if (reflected)
    {
        // Layout-rule diagnostics name the shader that is rejected.
        sgInfo.name = m_name;

        reflected   = RHIShaderUtil::ValidateShaderLayout(sgInfo, maxDescriptorSets);
    }

    if (reflected && sgInfo.SRDTable.size() > kGlobalBindlessHeapIndex
        && m_pRHI->GetDevice()->GetPhysicalDeviceFeatures().imageCubeArray != VK_TRUE)
    {
        for (const RHIShaderResourceDescriptor& descriptor : sgInfo.SRDTable[kGlobalBindlessHeapIndex])
        {
            if (descriptor.binding == ZEN_BINDLESS_HEAP_BINDING_TEXTURECUBEARRAY)
            {
                ZEN_RHI_LOG_ERROR("Shader '{}' requests the cube-array heap, which requires imageCubeArray", m_name.CStr());
                reflected = false;
            }
        }
    }

    reflected = reflected && ApplySpecializationOverrides(sgInfo);

    if (reflected)
    {
        m_SRDTable = sgInfo.SRDTable;

        if (m_SRDTable.size() <= kGlobalBindlessHeapIndex)
        {
            m_SRDTable.resize(kGlobalBindlessHeapIndex + 1);
        }

        for (std::vector<RHIShaderResourceDescriptor> const& setSRDs : m_SRDTable)
        {
            for (const RHIShaderResourceDescriptor& srd : setSRDs)
            {
                ++m_SRDCount[ToUnderlying(srd.type)];

                if (!srd.name.empty())
                {
                    m_namedSRDLut.emplace(srd.name, &srd);
                }
            }
        }

        BuildSpecializationInfo(sgInfo.specializationConstants);

        // Reject every device-limit violation before creating modules, set layouts, or the pipeline layout.
        const VulkanDevice* device     = m_pRHI->GetDevice();
        const bool          fitsLimits = ShaderLayoutFitsLimits(
            sgInfo, device->GetPhysicalDeviceProperties().limits, device->GetDescriptorIndexingProperties(),
            device->GetAccelerationStructureProperties(), m_pRHI->GetBindlessHeap()->GetCapacities());
        error = fitsLimits ? InitNativeObjects(sgInfo)
                           : MakeRHIError(RHIErrorCode::eUnsupported, "Shader descriptor limits", __FILE__, __LINE__);
    }

    return error;
}

RHIError VulkanShader::InitNativeObjects(const RHIShaderGroupInfo& sgInfo)
{
    bool ready = CreateShaderModules();

    // Every pipeline layout starts with the global heap's set; the other sets come from the shared cache.
    for (uint32_t set = 0; ready && set < m_SRDTable.size(); ++set)
    {
        const VulkanSetLayout* layout = set == kGlobalBindlessHeapIndex
                                          ? &m_pRHI->GetBindlessHeap()->GetSetLayout()
                                          : m_pRHI->GetSetLayoutCache()->GetOrCreate(m_SRDTable[set]);

        ready                         = layout != nullptr && layout->handle != VK_NULL_HANDLE;

        m_setLayouts.push_back(layout);
    }

    if (!ready)
    {
        m_setLayouts.clear();
    }

    m_hasGlobalBindlessSet = !m_SRDTable[kGlobalBindlessHeapIndex].empty();

    BuildVertexInput(sgInfo);

    ready = ready && CreatePipelineLayout(sgInfo);

    return ready ? RHIError{} : MakeRHIError(RHIErrorCode::eBackendFailure, "Create shader native objects", __FILE__, __LINE__);
}

bool VulkanShader::ApplySpecializationOverrides(RHIShaderGroupInfo& sgInfo) const
{
    bool applied = true;

    for (RHIShaderSpecializationConstant& constant : sgInfo.specializationConstants)
    {
        const HashMap<uint32_t, RHIShaderSpecializationValue>::const_iterator overrideValue =
            m_specializationConstants.find(constant.constantId);
        if (overrideValue != m_specializationConstants.end())
        {
            if (overrideValue->second.type == constant.type)
            {
                constant.bits = overrideValue->second.bits;
            }
            else
            {
                ZEN_RHI_LOG_ERROR("Shader '{}' specialization constant {} has override type {}, expected {}", m_name.CStr(),
                                  constant.constantId, ToUnderlying(overrideValue->second.type), ToUnderlying(constant.type));
                applied = false;
            }
        }
    }

    return applied;
}

// Vulkan scalar specialization values occupy four bytes each, VkBool32 included, in reflection order.
void VulkanShader::BuildSpecializationInfo(std::span<const RHIShaderSpecializationConstant> constants)
{

    if (!constants.empty())
    {
        m_spcMapEntries.resize(constants.size());

        m_specializationData.resize(constants.size());

        for (uint32_t i = 0; i < constants.size(); i++)
        {
            VkSpecializationMapEntry& entry = m_spcMapEntries[i];

            // Vulkan scalar specialization values occupy four bytes, including VkBool32.
            entry.constantID        = constants[i].constantId;

            entry.offset            = i * sizeof(uint32_t);

            entry.size              = sizeof(uint32_t);

            m_specializationData[i] = constants[i].type == RHIShaderSpecializationConstantType::eBool
                                        ? (constants[i].bits != 0 ? VK_TRUE : VK_FALSE)
                                        : constants[i].bits;
        }
    }

    m_specializationInfo.mapEntryCount = static_cast<uint32_t>(m_spcMapEntries.size());

    m_specializationInfo.pMapEntries   = m_spcMapEntries.empty() ? nullptr : m_spcMapEntries.data();

    m_specializationInfo.dataSize      = m_specializationData.size() * sizeof(uint32_t);

    m_specializationInfo.pData         = m_specializationData.empty() ? nullptr : m_specializationData.data();
}

bool VulkanShader::CreateShaderModules()
{
    bool ready = true;

    m_stageCreateInfos.reserve(m_shaderGroupSPIRV->GetStageCount());

    for (uint32_t i = 0; ready && i < m_shaderGroupSPIRV->GetModules().size(); i++)
    {
        const RHIShaderGroupSPIRV::Module& shader = m_shaderGroupSPIRV->GetModules()[i];
        RHIShaderStage                     stage  = shader.stage;

        if (m_shaderGroupSPIRV->HasShaderStage(stage))
        {
            std::span<const uint8_t> spirvCode = shader.code;

            VkShaderModuleCreateInfo shaderModuleCI;

            InitVkStruct(shaderModuleCI, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);

            shaderModuleCI.codeSize = spirvCode.size();

            shaderModuleCI.pCode    = reinterpret_cast<const uint32_t*>(spirvCode.data());

            VkShaderModule module{VK_NULL_HANDLE};

            const VkResult result = vkCreateShaderModule(m_pRHI->GetVkDevice(), &shaderModuleCI, nullptr, &module);

            ready                 = result == VK_SUCCESS && module != VK_NULL_HANDLE;

            if (!ready)
            {
                m_pRHI->CheckDeviceLoss(result, "vkCreateShaderModule");

                ZEN_RHI_LOG_ERROR("vkCreateShaderModule failed: {}", GetResultString(result));

                break;
            }

            VkPipelineShaderStageCreateInfo pipelineShaderStageCI;

            InitVkStruct(pipelineShaderStageCI, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);

            pipelineShaderStageCI.stage               = ShaderStageToVkShaderStageFlagBits(stage);

            pipelineShaderStageCI.pName               = "main";

            pipelineShaderStageCI.module              = module;

            pipelineShaderStageCI.pSpecializationInfo = &m_specializationInfo;

            m_stageCreateInfos.push_back(pipelineShaderStageCI);
        }
    }

    return ready;
}

void VulkanShader::BuildVertexInput(const RHIShaderGroupInfo& sgInfo)
{
    const size_t vertexInputCount = sgInfo.vertexInputAttributes.size();

    if (vertexInputCount > 0)
    {
        m_vertexInputInfo.vkAttributes.resize(vertexInputCount);

        // packed data for vertex inputs, only 1 binding
        VkVertexInputBindingDescription vkBinding{};

        vkBinding.binding   = 0;

        vkBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        vkBinding.stride    = sgInfo.vertexBindingStride;

        m_vertexInputInfo.vkBindings.emplace_back(vkBinding);

        // populate attributes
        for (uint32_t i = 0; i < vertexInputCount; i++)
        {
            m_vertexInputInfo.vkAttributes[i].binding  = sgInfo.vertexInputAttributes[i].binding;

            m_vertexInputInfo.vkAttributes[i].location = sgInfo.vertexInputAttributes[i].location;

            m_vertexInputInfo.vkAttributes[i].offset   = sgInfo.vertexInputAttributes[i].offset;

            m_vertexInputInfo.vkAttributes[i].format   = static_cast<VkFormat>(sgInfo.vertexInputAttributes[i].format);
        }

        InitVkStruct(m_vertexInputInfo.stateCI, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);

        m_vertexInputInfo.stateCI.vertexAttributeDescriptionCount = m_vertexInputInfo.vkAttributes.size();

        m_vertexInputInfo.stateCI.pVertexAttributeDescriptions    = m_vertexInputInfo.vkAttributes.data();

        m_vertexInputInfo.stateCI.vertexBindingDescriptionCount   = m_vertexInputInfo.vkBindings.size();

        m_vertexInputInfo.stateCI.pVertexBindingDescriptions      = m_vertexInputInfo.vkBindings.data();
    }
    else
    {
        InitVkStruct(m_vertexInputInfo.stateCI, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);

        m_vertexInputInfo.stateCI.vertexAttributeDescriptionCount = 0;

        m_vertexInputInfo.stateCI.pVertexAttributeDescriptions    = nullptr;

        m_vertexInputInfo.stateCI.vertexBindingDescriptionCount   = 0;

        m_vertexInputInfo.stateCI.pVertexBindingDescriptions      = nullptr;
    }
}

bool VulkanShader::CreatePipelineLayout(const RHIShaderGroupInfo& sgInfo)
{
    VkPipelineLayoutCreateInfo pipelineLayoutCI;

    InitVkStruct(pipelineLayoutCI, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

    SmallVector<VkDescriptorSetLayout, kInlineDescriptorSetCount> setLayoutHandles;

    for (const VulkanSetLayout* layout : m_setLayouts)
    {
        setLayoutHandles.push_back(layout->handle);
    }

    pipelineLayoutCI.pSetLayouts    = setLayoutHandles.data();

    pipelineLayoutCI.setLayoutCount = setLayoutHandles.size();

    VkPushConstantRange prc{};

    const RHIShaderGroupInfo::ShaderPushConstants& pc = sgInfo.pushConstants;

    if (pc.size > 0)
    {
        prc.stageFlags                          = ShaderStageFlagsBitsToVkShaderStageFlags(pc.stageFlags);

        prc.size                                = pc.size;

        pipelineLayoutCI.pushConstantRangeCount = 1;

        pipelineLayoutCI.pPushConstantRanges    = &prc;
    }

    const VkResult result = vkCreatePipelineLayout(m_pRHI->GetVkDevice(), &pipelineLayoutCI, nullptr, &m_pipelineLayout);

    const bool ready      = result == VK_SUCCESS;

    if (!ready)
    {
        m_pRHI->CheckDeviceLoss(result, "vkCreatePipelineLayout");

        ZEN_RHI_LOG_ERROR("vkCreatePipelineLayout failed: {}", GetResultString(result));

        if (m_pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_pRHI->GetVkDevice(), m_pipelineLayout, nullptr);

            m_pipelineLayout = VK_NULL_HANDLE;
        }
    }

    m_pushConstantsStageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(sgInfo.pushConstants.stageFlags);

    const NameID debugName(fmt::format("{}_PipelineLayout", sgInfo.name.c_str()));

    if (ready)
    {
        m_pRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<uint64_t>(m_pipelineLayout),
                                           debugName);
    }

    return ready;
}

void VulkanShader::Destroy()
{
    for (VkPipelineShaderStageCreateInfo& stageCreateInfo : m_stageCreateInfos)
    {
        vkDestroyShaderModule(m_pRHI->GetVkDevice(), stageCreateInfo.module, nullptr);
    }

    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_pRHI->GetVkDevice(), m_pipelineLayout, nullptr);
    }

    DeleteVulkanResource(m_pRHI, this);
}
} // namespace zen
