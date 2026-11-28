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
#include "Vulkan/VulkanBindlessHeap.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Utils/Errors.h"
#include "ZenFoundation/Utils/Helpers.h"

namespace zen
{
static void LogPipelineStatistics(VulkanRHI& rhi, VkPipeline pipeline, NameID shader);

static bool EnablePipelineStatistics(VulkanRHI& rhi, NameID shader)
{
    const bool enabled = rhi.GetDeviceDesc().PipelineStatistics()
                      && rhi.GetDevice()->GetExtensionFlags().hasPipelineExecutableInfo
                      && vkGetPipelineExecutablePropertiesKHR != nullptr && vkGetPipelineExecutableStatisticsKHR != nullptr;
    if (!enabled && rhi.GetDeviceDesc().PipelineStatistics())
    {
        ZEN_RHI_LOG_WARN("Pipeline statistics unsupported for '{}'", shader.CStr());
    }
    return enabled;
}

RHIPipeline* VulkanRHI::CreatePipeline(const RHIRayTracingPipelineCreateInfo& createInfo)
{
    RHIWorkerTimer timer(RHIWorkerTime::ePipeline);
    return AreSubmissionsBlocked() ? nullptr : VulkanPipeline::CreateObject(this, createInfo).GetValueOr(nullptr);
}

RHIPipeline* VulkanRHI::CreatePipeline(const RHIComputePipelineCreateInfo& createInfo)
{
    RHIWorkerTimer timer(RHIWorkerTime::ePipeline);
    return AreSubmissionsBlocked() ? nullptr : VulkanPipeline::CreateObject(this, createInfo).GetValueOr(nullptr);
}

RHIPipeline* VulkanRHI::CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo)
{
    RHIWorkerTimer timer(RHIWorkerTime::ePipeline);
    return AreSubmissionsBlocked() ? nullptr : VulkanPipeline::CreateObject(this, createInfo).GetValueOr(nullptr);
}

static VkPipelineColorBlendAttachmentState MakeColorBlendAttachment(const RHIGfxPipelineColorBlendState& state, uint32_t index)
{
    VkPipelineColorBlendAttachmentState native{};
    if (state.attachmentsMask.test(index))
    {
        const RHIGfxPipelineColorBlendState::Attachment& attachment = state.attachments[index];
        native.blendEnable                                          = attachment.enableBlend;
        native.srcColorBlendFactor                                  = ToVkBlendFactor(attachment.srcColorBlendFactor);
        native.dstColorBlendFactor                                  = ToVkBlendFactor(attachment.dstColorBlendFactor);
        native.colorBlendOp                                         = ToVkBlendOp(attachment.colorBlendOp);
        native.srcAlphaBlendFactor                                  = ToVkBlendFactor(attachment.srcAlphaBlendFactor);
        native.dstAlphaBlendFactor                                  = ToVkBlendFactor(attachment.dstAlphaBlendFactor);
        native.alphaBlendOp                                         = ToVkBlendOp(attachment.alphaBlendOp);
        native.colorWriteMask                                       = attachment.colorWriteMask;
    }
    return native;
}

static bool UsesDualSourceBlend(VkBlendFactor factor)
{
    return factor == VK_BLEND_FACTOR_SRC1_COLOR || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR
        || factor == VK_BLEND_FACTOR_SRC1_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}

static bool ValidateGraphicsPipeline(VulkanRHI& rhi, const RHIGfxPipelineCreateInfo& createInfo)
{
    const RHIRenderingLayout* layout = createInfo.pRenderingLayout;

    bool valid = createInfo.pShader != nullptr && createInfo.pShader->GetResourceType() == RHIResourceType::eShader
              && layout != nullptr && layout->numColorRenderTargets <= MAX_NUM_COLOR_ATTACHMENTS
              && createInfo.states.primitiveType != RHIDrawPrimitiveType::ePatchList;

    if (valid)
    {
        VulkanDevice*                   device   = rhi.GetDevice();
        const VkPhysicalDeviceFeatures& features = device->GetPhysicalDeviceFeatures();
        const VulkanShader*             shader   = static_cast<const VulkanShader*>(createInfo.pShader);
        VkShaderStageFlags              stages   = 0;
        for (uint32_t stage = 0; stage < shader->GetNumShaderStages(); ++stage)
        {
            stages |= shader->GetStageCreateInfoData()[stage].stage;
        }
        const VkShaderStageFlags supportedStages =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        valid &= (stages & VK_SHADER_STAGE_VERTEX_BIT) != 0 && (stages & ~supportedStages) == 0;
        const RHIGfxPipelineStates&               states        = createInfo.states;
        const RHIGfxPipelineRasterizationState&   rasterization = states.rasterizationState;
        const VkPipelineColorBlendAttachmentState first         = MakeColorBlendAttachment(states.colorBlendState, 0);
        bool                                      independent   = false;
        bool                                      dualSource    = false;
        for (uint32_t index = 0; index < layout->numColorRenderTargets; ++index)
        {
            const VkPipelineColorBlendAttachmentState current = MakeColorBlendAttachment(states.colorBlendState, index);
            independent |= current.blendEnable != first.blendEnable || current.srcColorBlendFactor != first.srcColorBlendFactor
                        || current.dstColorBlendFactor != first.dstColorBlendFactor
                        || current.colorBlendOp != first.colorBlendOp
                        || current.srcAlphaBlendFactor != first.srcAlphaBlendFactor
                        || current.dstAlphaBlendFactor != first.dstAlphaBlendFactor
                        || current.alphaBlendOp != first.alphaBlendOp || current.colorWriteMask != first.colorWriteMask;
            dualSource  |= UsesDualSourceBlend(current.srcColorBlendFactor) || UsesDualSourceBlend(current.dstColorBlendFactor)
                        || UsesDualSourceBlend(current.srcAlphaBlendFactor) || UsesDualSourceBlend(current.dstAlphaBlendFactor);
        }
        struct FeatureRequirement
        {
            bool        requested;
            VkBool32    enabled;
            const char* name;
        };
        const FeatureRequirement requirements[] = {
            {rasterization.wireframe, features.fillModeNonSolid, "fillModeNonSolid"},
            {rasterization.enableDepthClamp, features.depthClamp, "depthClamp"},
            {states.depthStencilState.enableDepthBoundsTest, features.depthBounds, "depthBounds"},
            {states.multiSampleState.enableAlphaToOne, features.alphaToOne, "alphaToOne"},
            {states.multiSampleState.enableSampleShading, features.sampleRateShading, "sampleRateShading"},
            {states.colorBlendState.enableLogicOp, features.logicOp, "logicOp"},
            {rasterization.enableDepthBias && rasterization.depthBiasClamp != 0.0f
                 && !states.dynamicStates.enabledStates.test(ToUnderlying(RHIDynamicState::eDepthBias)),
             features.depthBiasClamp, "depthBiasClamp"},
            {rasterization.lineWidth != 1.0f
                 && !states.dynamicStates.enabledStates.test(ToUnderlying(RHIDynamicState::eLineWidth)),
             features.wideLines, "wideLines"},
            {independent, features.independentBlend, "independentBlend"},
            {dualSource, features.dualSrcBlend, "dualSrcBlend"},
            {(stages & VK_SHADER_STAGE_GEOMETRY_BIT) != 0
                 || (states.primitiveType >= RHIDrawPrimitiveType::eLineListWithAdjacency
                     && states.primitiveType <= RHIDrawPrimitiveType::eTriangleStripWithAdjacency),
             features.geometryShader, "geometryShader"}};
        for (const FeatureRequirement& requirement : requirements)
        {
            if (requirement.requested && !requirement.enabled)
            {
                ZEN_RHI_LOG_ERROR("Graphics pipeline '{}' requires disabled feature {}", shader->GetName(), requirement.name);
                valid = false;
            }
        }
        const uint32_t dualSourceLimit = device->GetPhysicalDeviceProperties().limits.maxFragmentDualSrcAttachments;
        if (dualSource && features.dualSrcBlend && layout->numColorRenderTargets > dualSourceLimit)
        {
            ZEN_RHI_LOG_ERROR("Graphics pipeline '{}' uses dual-source blending with {} color targets; the device allows {}",
                              shader->GetName(), layout->numColorRenderTargets, dualSourceLimit);
            valid = false;
        }
        valid &=
            FragmentResourcesFitLimits(createInfo.pShader->GetName(), *createInfo.pShader->GetSRDTable(),
                                       rhi.GetBindlessHeap()->GetCapacities().GetImageSlots(), layout->numColorRenderTargets,
                                       device->GetPhysicalDeviceProperties().limits, device->GetDescriptorIndexingProperties());

        // Layouts may describe formats without owning concrete textures/views.
        const SampleCount samples = createInfo.states.multiSampleState.sampleCount;

        for (uint32_t i = 0; i < layout->numColorRenderTargets; ++i)
        {
            const RHIRenderTarget& target  = layout->colorRenderTargets[i];

            valid                         &= target.format != DataFormat::eUndefined && target.numSamples == samples
                                          && !(FormatIsDepthOnly(target.format) || FormatIsStencilOnly(target.format)
                                               || FormatIsDepthStencil(target.format));
        }

        if (layout->hasDepthStencilRT)
        {
            const RHIRenderTarget& target  = layout->depthStencilRenderTarget;

            valid                         &= target.numSamples == samples
                                          && (FormatIsDepthOnly(target.format) || FormatIsStencilOnly(target.format)
                                              || FormatIsDepthStencil(target.format));
        }

        valid &= (createInfo.states.colorBlendState.attachmentsMask >> layout->numColorRenderTargets).none();
    }

    if (!valid)
    {
        ZEN_RHI_LOG_ERROR(
            "Graphics pipeline requires a compatible shader, rendering layout, supported non-patch topology, stages, and enabled features");
    }

    return valid;
}

RHIResult<VulkanPipeline*> VulkanPipeline::CreateObject(VulkanRHI* pRHI, const RHIGfxPipelineCreateInfo& createInfo)
{
    VulkanPipeline* pipeline =
        ValidateGraphicsPipeline(*pRHI, createInfo) ? NewVulkanResource<VulkanPipeline>(pRHI, pRHI, createInfo) : nullptr;

    return pipeline != nullptr ? KeepIfInitialized(pipeline, pipeline->InitGraphics(*createInfo.pRenderingLayout))
                               : RejectDescription<VulkanPipeline>("Graphics pipeline description", __FILE__, __LINE__);
}

RHIResult<VulkanPipeline*> VulkanPipeline::CreateObject(VulkanRHI* pRHI, const RHIComputePipelineCreateInfo& createInfo)
{
    const VulkanShader* shader =
        createInfo.pShader != nullptr && createInfo.pShader->GetResourceType() == RHIResourceType::eShader
            ? static_cast<const VulkanShader*>(createInfo.pShader)
            : nullptr;

    const bool valid = shader != nullptr && shader->GetNumShaderStages() == 1
                    && shader->GetStageCreateInfoData()[0].stage == VK_SHADER_STAGE_COMPUTE_BIT;

    if (!valid)
    {
        ZEN_RHI_LOG_ERROR("Compute pipeline description: the shader must have exactly one compute stage");
    }

    VulkanPipeline* pipeline = valid ? NewVulkanResource<VulkanPipeline>(pRHI, pRHI, createInfo) : nullptr;

    return pipeline != nullptr ? KeepIfInitialized(pipeline, pipeline->Init())
                               : RejectDescription<VulkanPipeline>("Compute pipeline description", __FILE__, __LINE__);
}

RHIError VulkanPipeline::Init()
{
    m_bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;

    return InitCompute();
}

RHIResult<VulkanPipeline*> VulkanPipeline::CreateObject(VulkanRHI* pRHI, const RHIRayTracingPipelineCreateInfo& createInfo)
{
    const RHIRayTracingCapabilities&                         capabilities = pRHI->QueryGPUInfo().rayTracing;
    std::array<uint32_t, RHIShaderBindingTableLayout::Count> counts{};
    RHIShaderBindingTableLayout                              layout;
    const bool valid = capabilities.IsUsable() && createInfo.pShader != nullptr
                    && createInfo.pShader->GetResourceType() == RHIResourceType::eShader && createInfo.maxRecursionDepth > 0
                    && createInfo.maxRecursionDepth <= capabilities.maxRayRecursionDepth
                    && ValidateRayTracingShaderGroups(createInfo.pShader->GetModules(), createInfo.groups, counts)
                    && BuildShaderBindingTableLayout(capabilities, counts, layout)
                    && layout.size <= SIZE_MAX - (layout.alignment - 1) && vkCreateRayTracingPipelinesKHR != nullptr
                    && vkGetRayTracingShaderGroupHandlesKHR != nullptr;
    if (!valid)
    {
        ZEN_RHI_LOG_ERROR("Ray-tracing pipeline description: unsupported capability or invalid groups or limits");
    }
    VulkanPipeline* pipeline = valid ? NewVulkanResource<VulkanPipeline>(pRHI, pRHI, createInfo) : nullptr;
    return pipeline != nullptr ? KeepIfInitialized(pipeline, pipeline->InitRayTracing(createInfo, layout))
                               : RejectDescription<VulkanPipeline>("Ray-tracing pipeline description", __FILE__, __LINE__);
}

RHIError VulkanPipeline::InitRayTracing(const RHIRayTracingPipelineCreateInfo& createInfo,
                                        const RHIShaderBindingTableLayout&     layout)
{
    RHIError                                         error{};
    const VulkanShader*                              shader = TO_VK_SHADER(m_pShader);
    HeapVector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    groups.reserve(createInfo.groups.size());
    for (const RHIRayTracingShaderGroup& group : createInfo.groups)
    {
        VkRayTracingShaderGroupCreateInfoKHR native{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        native.type               = group.type == RHIRayTracingShaderGroupType::eHit
                                      ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
                                      : VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        native.generalShader      = group.generalShader;
        native.closestHitShader   = group.closestHitShader;
        native.anyHitShader       = group.anyHitShader;
        native.intersectionShader = VK_SHADER_UNUSED_KHR;
        groups.push_back(native);
    }
    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.stageCount                   = shader->GetNumShaderStages();
    info.pStages                      = shader->GetStageCreateInfoData();
    info.groupCount                   = static_cast<uint32_t>(groups.size());
    info.pGroups                      = groups.data();
    info.maxPipelineRayRecursionDepth = createInfo.maxRecursionDepth;
    info.layout                       = shader->GetVkPipelineLayout();
    const bool statistics             = EnablePipelineStatistics(*m_pRHI, shader->GetName());
    if (statistics)
    {
        info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    }
    if (m_pRHI->GetDevice()->GetExtensionFlags().hasOpacityMicromap)
    {
        info.flags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_KHR;
    }
    const VkResult result = vkCreateRayTracingPipelinesKHR(
        m_pRHI->GetVkDevice(), VK_NULL_HANDLE, m_pRHI->GetDevice()->GetPipelineCache(), 1, &info, nullptr, &m_vkPipeline);
    if (result == VK_SUCCESS && m_vkPipeline != VK_NULL_HANDLE)
    {
        if (statistics)
        {
            LogPipelineStatistics(*m_pRHI, m_vkPipeline, shader->GetName());
            for (uint32_t i = 0; vkGetRayTracingShaderGroupStackSizeKHR != nullptr && i < groups.size(); ++i)
            {
                const VkRayTracingShaderGroupCreateInfoKHR& group  = groups[i];
                const VkDevice                              device = m_pRHI->GetVkDevice();
                const VkDeviceSize                          general =
                    group.generalShader == VK_SHADER_UNUSED_KHR
                        ? 0
                        : vkGetRayTracingShaderGroupStackSizeKHR(device, m_vkPipeline, i, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
                const VkDeviceSize closest = group.closestHitShader == VK_SHADER_UNUSED_KHR
                                               ? 0
                                               : vkGetRayTracingShaderGroupStackSizeKHR(device, m_vkPipeline, i,
                                                                                        VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
                const VkDeviceSize any =
                    group.anyHitShader == VK_SHADER_UNUSED_KHR
                        ? 0
                        : vkGetRayTracingShaderGroupStackSizeKHR(device, m_vkPipeline, i, VK_SHADER_GROUP_SHADER_ANY_HIT_KHR);
                ZEN_RHI_LOG_INFO("Ray pipeline stack: shader='{}' group={} general={} closest={} any={}", shader->GetName(), i,
                                 general, closest, any);
            }
        }
        if (!InitShaderBindingTable(createInfo.groups, layout))
        {
            error = MakeRHIError(RHIErrorCode::eBackendFailure, "Create shader binding table", __FILE__, __LINE__);
        }
    }
    else
    {
        error = m_pRHI->ReportCreationFailure(result, "vkCreateRayTracingPipelinesKHR", __FILE__, __LINE__);
        ZEN_RHI_LOG_ERROR("vkCreateRayTracingPipelinesKHR failed: {}", GetResultString(result));
    }
    m_bindPoint = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;

    return error;
}

bool VulkanPipeline::InitShaderBindingTable(std::span<const RHIRayTracingShaderGroup> groups,
                                            const RHIShaderBindingTableLayout&        layout)
{
    const uint32_t      handleSize = m_pRHI->QueryGPUInfo().rayTracing.shaderGroupHandleSize;
    HeapVector<uint8_t> handles(groups.size() * size_t(handleSize));
    const VkResult      result = vkGetRayTracingShaderGroupHandlesKHR(
        m_pRHI->GetVkDevice(), m_vkPipeline, 0, static_cast<uint32_t>(groups.size()), handles.size(), handles.data());
    bool ready = result == VK_SUCCESS;
    if (!ready)
    {
        m_pRHI->CheckDeviceLoss(result, "vkGetRayTracingShaderGroupHandlesKHR");
        ZEN_RHI_LOG_ERROR("vkGetRayTracingShaderGroupHandlesKHR failed: {}", GetResultString(result));
    }
    if (ready)
    {
        RHIBufferCreateInfo info{};
        info.size         = layout.size + layout.alignment - 1;
        info.allocateType = RHIBufferAllocateType::eCPUWriteGPURead;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eShaderBindingTable, RHIBufferUsageFlagBits::eDeviceAddress);
        m_shaderBindingTableBuffer =
            RHIResourcePtr<RHIBuffer>::Adopt(VulkanBuffer::CreateObject(m_pRHI, info).GetValueOr(nullptr));
        ready = m_shaderBindingTableBuffer != nullptr && m_shaderBindingTableBuffer->GetDeviceAddress() != 0
             && m_shaderBindingTableBuffer->Map() != nullptr;
    }
    if (ready)
    {
        const uint64_t address = m_shaderBindingTableBuffer->GetDeviceAddress();
        const uint64_t padding = (uint64_t(layout.alignment) - address % layout.alignment) % layout.alignment;
        uint8_t*       mapped  = m_shaderBindingTableBuffer->Map() + padding;
        std::memset(mapped, 0, static_cast<size_t>(layout.size));
        std::array<uint32_t, RHIShaderBindingTableLayout::Count> record{};
        for (size_t group = 0; group < groups.size(); ++group)
        {
            const uint32_t                     regionIndex = ToUnderlying(groups[group].type);
            const RHIShaderBindingTableRegion& region      = layout.regions[regionIndex];
            std::memcpy(mapped + region.offset + region.stride * record[regionIndex]++, handles.data() + group * handleSize,
                        handleSize);
        }
        for (uint32_t regionIndex = 0; regionIndex < RHIShaderBindingTableLayout::Count; ++regionIndex)
        {
            const RHIShaderBindingTableRegion& region = layout.regions[regionIndex];
            if (region.size != 0)
            {
                m_shaderBindingTable[regionIndex] = {address + padding + region.offset, region.stride, region.size};
            }
        }
        // CPUWriteGPURead memory is host coherent. Initial writes precede every submission.
        m_shaderBindingTableBuffer->Unmap();
    }
    return ready;
}

void VulkanPipeline::Destroy()
{
    vkDestroyPipeline(m_pRHI->GetVkDevice(), m_vkPipeline, nullptr);

    DeleteVulkanResource(m_pRHI, this);
}

RHIError VulkanPipeline::InitGraphics(const RHIRenderingLayout& layout)
{
    // Input Assembly
    VkPipelineInputAssemblyStateCreateInfo IAStateCI;

    InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);

    IAStateCI.topology               = ToVkPrimitiveTopology(m_gfxStates.primitiveType);

    IAStateCI.primitiveRestartEnable = VK_FALSE;

    // Viewport State
    VkPipelineViewportStateCreateInfo VPStateCI;

    InitVkStruct(VPStateCI, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);

    VPStateCI.scissorCount       = 1;

    VPStateCI.viewportCount      = 1;

    const Rect2<int>& renderArea = layout.renderArea;

    const VkViewport viewport{static_cast<float>(renderArea.minX),
                              static_cast<float>(renderArea.minY),
                              static_cast<float>(renderArea.Width()),
                              static_cast<float>(renderArea.Height()),
                              0.0f,
                              1.0f};

    const VkRect2D scissor{{static_cast<int32_t>(renderArea.minX), static_cast<int32_t>(renderArea.minY)},
                           {static_cast<uint32_t>(renderArea.Width()), static_cast<uint32_t>(renderArea.Height())}};

    VPStateCI.pViewports                                         = &viewport;

    VPStateCI.pScissors                                          = &scissor;

    const VkPipelineRasterizationStateCreateInfo rasterizationCI = ToVkRasterizationState(m_gfxStates.rasterizationState);

    // Multisample state
    const RHIGfxPipelineMultiSampleState& multisampleState = m_gfxStates.multiSampleState;

    // Vulkan consumes one 32-bit word per 32 samples, in sample-index order.
    const VkSampleMask sampleMasks[] = {static_cast<VkSampleMask>(multisampleState.sampleMasks),
                                        static_cast<VkSampleMask>(multisampleState.sampleMasks >> 32)};

    VkPipelineMultisampleStateCreateInfo MSStateCI;

    InitVkStruct(MSStateCI, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);

    MSStateCI.rasterizationSamples                        = ToVkSampleCountFlagBits(multisampleState.sampleCount);

    MSStateCI.sampleShadingEnable                         = multisampleState.enableSampleShading;

    MSStateCI.minSampleShading                            = multisampleState.minSampleShading;

    MSStateCI.alphaToCoverageEnable                       = multisampleState.enableAlphaToCoverage;

    MSStateCI.alphaToOneEnable                            = multisampleState.enableAlphaToOne;

    MSStateCI.pSampleMask                                 = sampleMasks;

    const VkPipelineDepthStencilStateCreateInfo DSStateCI = ToVkDepthStencilState(m_gfxStates.depthStencilState);

    // Color Blend State
    const RHIGfxPipelineColorBlendState& colorBlendState = m_gfxStates.colorBlendState;

    HeapVector<VkPipelineColorBlendAttachmentState> vkCBAttStates;

    // Attachment indices are render-target locations; unmasked targets disable color writes.
    vkCBAttStates.resize(layout.numColorRenderTargets);

    for (uint32_t i = 0; i < layout.numColorRenderTargets; ++i)
    {
        vkCBAttStates[i] = MakeColorBlendAttachment(colorBlendState, i);
    }

    VkPipelineColorBlendStateCreateInfo CBStateCI;

    InitVkStruct(CBStateCI, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);

    CBStateCI.logicOpEnable     = colorBlendState.enableLogicOp;

    CBStateCI.logicOp           = ToVkLogicOp(colorBlendState.logicOp);

    CBStateCI.attachmentCount   = static_cast<uint32_t>(vkCBAttStates.size());

    CBStateCI.pAttachments      = vkCBAttStates.empty() ? nullptr : vkCBAttStates.data();

    CBStateCI.blendConstants[0] = colorBlendState.blendConstants.r;

    CBStateCI.blendConstants[1] = colorBlendState.blendConstants.g;

    CBStateCI.blendConstants[2] = colorBlendState.blendConstants.b;

    CBStateCI.blendConstants[3] = colorBlendState.blendConstants.a;

    // Dynamic States
    VkPipelineDynamicStateCreateInfo dynamicStateCI;

    InitVkStruct(dynamicStateCI, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);

    const std::bitset<static_cast<size_t>(RHIDynamicState::eMax)>& enabledStates = m_gfxStates.dynamicStates.enabledStates;

    HeapVector<VkDynamicState> vkDynamicStates;

    vkDynamicStates.reserve(enabledStates.count());

    for (size_t i = 0; i < enabledStates.size(); ++i)
    {
        if (enabledStates[i])
        {
            vkDynamicStates.push_back(ToVkDynamicState(static_cast<RHIDynamicState>(i)));
        }
    }

    dynamicStateCI.dynamicStateCount = static_cast<uint32_t>(vkDynamicStates.size());

    dynamicStateCI.pDynamicStates    = vkDynamicStates.empty() ? nullptr : vkDynamicStates.data();

    VulkanShader* pShader            = TO_VK_SHADER(m_pShader);

    VkGraphicsPipelineCreateInfo pipelineCI;

    InitVkStruct(pipelineCI, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);

    pipelineCI.stageCount          = pShader->GetNumShaderStages();

    pipelineCI.pStages             = pShader->GetStageCreateInfoData();

    pipelineCI.pVertexInputState   = pShader->GetVertexInputStateCreateInfoData();

    pipelineCI.pInputAssemblyState = &IAStateCI;

    pipelineCI.pViewportState      = &VPStateCI;

    pipelineCI.pRasterizationState = &rasterizationCI;

    pipelineCI.pMultisampleState   = &MSStateCI;

    pipelineCI.pDepthStencilState  = &DSStateCI;

    pipelineCI.pColorBlendState    = &CBStateCI;

    pipelineCI.pDynamicState       = &dynamicStateCI;

    pipelineCI.layout              = pShader->GetVkPipelineLayout();

    pipelineCI.subpass             = 0;

    HeapVector<VkFormat> colorAttachmentFormats;

    VkPipelineRenderingCreateInfoKHR renderingCI;

    pipelineCI.renderPass = VK_NULL_HANDLE;

    colorAttachmentFormats.reserve(layout.numColorRenderTargets);

    for (uint32_t i = 0; i < layout.numColorRenderTargets; i++)
    {
        VkFormat colorFormat = ToVkFormat(layout.colorRenderTargets[i].format);

        colorAttachmentFormats.emplace_back(colorFormat);
    }

    InitVkStruct(renderingCI, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR);

    renderingCI.colorAttachmentCount    = layout.numColorRenderTargets;

    renderingCI.pColorAttachmentFormats = colorAttachmentFormats.data();

    if (layout.hasDepthStencilRT)
    {
        const DataFormat format                          = layout.depthStencilRenderTarget.format;

        const RHIFlags<RHITextureAspectFlagBits> aspects = layout.depthStencilRenderTarget.GetAspects();

        if (aspects.HasFlag(RHITextureAspectFlagBits::eDepth))
        {
            renderingCI.depthAttachmentFormat = ToVkFormat(format);
        }

        if (aspects.HasFlag(RHITextureAspectFlagBits::eStencil))
        {
            renderingCI.stencilAttachmentFormat = ToVkFormat(format);
        }
    }

    pipelineCI.pNext      = &renderingCI;

    const VkResult result = vkCreateGraphicsPipelines(m_pRHI->GetVkDevice(), m_pRHI->GetDevice()->GetPipelineCache(), 1,
                                                      &pipelineCI, nullptr, &m_vkPipeline);

    RHIError error{};

    if (result != VK_SUCCESS || m_vkPipeline == VK_NULL_HANDLE)
    {
        error = m_pRHI->ReportCreationFailure(result, "vkCreateGraphicsPipelines", __FILE__, __LINE__);

        ZEN_RHI_LOG_ERROR("vkCreateGraphicsPipelines failed: {}", GetResultString(result));
    }

    return error;
}

// Compiler-defined statistics are logged with their native names and formats. In particular,
// do not interpret a vendor's local-memory counter as a portable byte footprint.
static void LogPipelineStatistics(VulkanRHI& rhi, VkPipeline pipeline, NameID shader)
{
    const VkDevice    device = rhi.GetVkDevice();
    VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    info.pipeline            = pipeline;
    uint32_t executableCount = 0;
    VkResult result          = vkGetPipelineExecutablePropertiesKHR(device, &info, &executableCount, nullptr);
    HeapVector<VkPipelineExecutablePropertiesKHR> executables;
    if (result == VK_SUCCESS && executableCount > 0)
    {
        executables.resize(executableCount);
        for (VkPipelineExecutablePropertiesKHR& property : executables)
        {
            property = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
        }
        result = vkGetPipelineExecutablePropertiesKHR(device, &info, &executableCount, executables.data());
    }
    for (uint32_t i = 0; result == VK_SUCCESS && i < executableCount; ++i)
    {
        ZEN_RHI_LOG_INFO("Pipeline statistics: shader='{}' executable={} name='{}' subgroup={}", shader.CStr(), i,
                         executables[i].name, executables[i].subgroupSize);
        VkPipelineExecutableInfoKHR executable{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        executable.pipeline        = pipeline;
        executable.executableIndex = i;
        uint32_t statisticCount    = 0;
        result                     = vkGetPipelineExecutableStatisticsKHR(device, &executable, &statisticCount, nullptr);
        HeapVector<VkPipelineExecutableStatisticKHR> statistics;
        if (result == VK_SUCCESS && statisticCount > 0)
        {
            statistics.resize(statisticCount);
            for (VkPipelineExecutableStatisticKHR& statistic : statistics)
            {
                statistic = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
            }
            result = vkGetPipelineExecutableStatisticsKHR(device, &executable, &statisticCount, statistics.data());
        }
        if (result == VK_SUCCESS)
        {
            for (uint32_t s = 0; s < statisticCount; ++s)
            {
                const VkPipelineExecutableStatisticKHR& statistic = statistics[s];
                std::string                             value;
                switch (statistic.format)
                {
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                        value = statistic.value.b32 ? "true" : "false";
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                        value = fmt::format("{}", statistic.value.i64);
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                        value = fmt::format("{}", statistic.value.u64);
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                        value = fmt::format("{}", statistic.value.f64);
                        break;
                    default: value = "unknown format"; break;
                }
                ZEN_RHI_LOG_INFO("Pipeline statistic: shader='{}' executable={} {}={} ({})", shader.CStr(), i, statistic.name,
                                 value, statistic.description);
            }
        }
    }
    if (result != VK_SUCCESS)
    {
        ZEN_RHI_LOG_WARN("Pipeline statistics unavailable or incomplete for '{}': {}", shader.CStr(), GetResultString(result));
    }
    else if (executableCount == 0)
    {
        ZEN_RHI_LOG_INFO("Pipeline statistics: shader='{}' reports no executables", shader.CStr());
    }
}

RHIError VulkanPipeline::InitCompute()
{
    VulkanShader* pShader = TO_VK_SHADER(m_pShader);

    VkComputePipelineCreateInfo pipelineCI{};

    pipelineCI.sType      = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;

    pipelineCI.stage      = pShader->GetStageCreateInfoData()[0];

    pipelineCI.layout     = pShader->GetVkPipelineLayout();

    const bool statistics = EnablePipelineStatistics(*m_pRHI, pShader->GetName());
    if (statistics)
    {
        pipelineCI.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    }

    const VkResult result = vkCreateComputePipelines(m_pRHI->GetVkDevice(), m_pRHI->GetDevice()->GetPipelineCache(), 1,
                                                     &pipelineCI, nullptr, &m_vkPipeline);

    RHIError error{};

    if (result != VK_SUCCESS || m_vkPipeline == VK_NULL_HANDLE)
    {
        error = m_pRHI->ReportCreationFailure(result, "vkCreateComputePipelines", __FILE__, __LINE__);

        ZEN_RHI_LOG_ERROR("vkCreateComputePipelines failed: {}", GetResultString(result));
    }
    else if (statistics)
    {
        LogPipelineStatistics(*m_pRHI, m_vkPipeline, pShader->GetName());
    }

    return error;
}

} // namespace zen
