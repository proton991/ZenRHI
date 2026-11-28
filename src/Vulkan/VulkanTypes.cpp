#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanTypes.h"
#include "ZenFoundation/Utils/Errors.h"

namespace zen
{
VkShaderStageFlagBits ShaderStageToVkShaderStageFlagBits(RHIShaderStage stage)
{
    VkShaderStageFlagBits result{};

    switch (stage)
    {
        case RHIShaderStage::eVertex: result = VK_SHADER_STAGE_VERTEX_BIT; break;
        case RHIShaderStage::eTesselationControl: result = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; break;
        case RHIShaderStage::eTesselationEvaluation: result = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; break;
        case RHIShaderStage::eGeometry: result = VK_SHADER_STAGE_GEOMETRY_BIT; break;
        case RHIShaderStage::eFragment: result = VK_SHADER_STAGE_FRAGMENT_BIT; break;
        case RHIShaderStage::eCompute: result = VK_SHADER_STAGE_COMPUTE_BIT; break;
        case RHIShaderStage::eRayGeneration: result = VK_SHADER_STAGE_RAYGEN_BIT_KHR; break;
        case RHIShaderStage::eMiss: result = VK_SHADER_STAGE_MISS_BIT_KHR; break;
        case RHIShaderStage::eClosestHit: result = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR; break;
        case RHIShaderStage::eAnyHit: result = VK_SHADER_STAGE_ANY_HIT_BIT_KHR; break;
        case RHIShaderStage::eCallable: result = VK_SHADER_STAGE_CALLABLE_BIT_KHR; break;
        default: result = VK_SHADER_STAGE_ALL; break;
    }

    return result;
}

VkShaderStageFlags ShaderStageFlagsBitsToVkShaderStageFlags(RHIFlags<RHIShaderStageFlagBits> stageFlags)
{
    VkShaderStageFlags flags{};

    for (uint32_t k = 0; k < ToUnderlying(RHIShaderStage::eMax); k++)
    {
        RHIShaderStage stage = static_cast<RHIShaderStage>(k);

        if (stageFlags.HasFlag(RHIShaderStageToFlagBits(stage)))
        {
            flags |= ShaderStageToVkShaderStageFlagBits(stage);
        }
    }

    return flags;
}

VkPrimitiveTopology ToVkPrimitiveTopology(RHIDrawPrimitiveType type)
{
    return static_cast<VkPrimitiveTopology>(type);
}

VkCullModeFlags ToVkCullModeFlags(RHIPolygonCullMode mode)
{
    return static_cast<VkCullModeFlags>(mode);
}

VkFrontFace ToVkFrontFace(RHIPolygonFrontFace frontFace)
{
    return static_cast<VkFrontFace>(frontFace);
}

VkSampleCountFlagBits ToVkSampleCountFlagBits(SampleCount count)
{
    VkSampleCountFlagBits result{};

    switch (count)
    {
        case SampleCount::e1: result = VK_SAMPLE_COUNT_1_BIT; break;
        case SampleCount::e2: result = VK_SAMPLE_COUNT_2_BIT; break;
        case SampleCount::e4: result = VK_SAMPLE_COUNT_4_BIT; break;
        case SampleCount::e8: result = VK_SAMPLE_COUNT_8_BIT; break;
        case SampleCount::e16: result = VK_SAMPLE_COUNT_16_BIT; break;
        case SampleCount::e32: result = VK_SAMPLE_COUNT_32_BIT; break;
        case SampleCount::e64: result = VK_SAMPLE_COUNT_64_BIT; break;
        default:
        case SampleCount::eMax: result = VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM; break;
    }

    return result;
}

VkCompareOp ToVkCompareOp(RHIDepthCompareOperator op)
{
    return static_cast<VkCompareOp>(op);
}

VkStencilOp ToVkStencilOp(RHIStencilOp op)
{
    return static_cast<VkStencilOp>(op);
}

VkLogicOp ToVkLogicOp(RHIBlendLogicOp op)
{
    return static_cast<VkLogicOp>(op);
}

VkBlendOp ToVkBlendOp(RHIBlendOp op)
{
    return static_cast<VkBlendOp>(op);
}

VkBlendFactor ToVkBlendFactor(RHIBlendFactor factor)
{
    return static_cast<VkBlendFactor>(factor);
}

VkDynamicState ToVkDynamicState(RHIDynamicState state)
{
    return static_cast<VkDynamicState>(state);
}

VkImageType ToVkImageType(RHITextureType type)
{
    return static_cast<VkImageType>(type);
}

VkImageViewType ToVkImageViewType(RHITextureType type)
{
    VkImageViewType result{};

    switch (type)
    {
        case RHITextureType::e1D: result = VK_IMAGE_VIEW_TYPE_1D; break;
        case RHITextureType::e2D: result = VK_IMAGE_VIEW_TYPE_2D; break;
        case RHITextureType::e3D: result = VK_IMAGE_VIEW_TYPE_3D; break;
        case RHITextureType::eCube: result = VK_IMAGE_VIEW_TYPE_CUBE; break;
        case RHITextureType::eMax: result = VK_IMAGE_VIEW_TYPE_MAX_ENUM; break;
    }

    return result;
}

VkImageViewCreateInfo MakeVkImageViewCreateInfo(RHITextureType                    type,
                                                DataFormat                        format,
                                                VkImage                           image,
                                                const RHITextureSubresourceRange& range,
                                                bool                              arrayView)
{
    VkImageViewCreateInfo info{};

    info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;

    info.image    = image;

    info.viewType = ToVkImageViewType(type);

    if (type == RHITextureType::e1D && (arrayView || range.layerCount > 1))
    {
        info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    }

    if (type == RHITextureType::e2D && (arrayView || range.layerCount > 1))
    {
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }

    if (type == RHITextureType::eCube && (arrayView || range.layerCount > 6))
    {
        info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }

    info.format = ToVkFormat(format);

    ToVkImageSubresourceRange(range, &info.subresourceRange);

    return info;
}

VkImageUsageFlags ToVkImageUsageFlags(RHIFlags<RHITextureUsageFlagBits> flagBits)
{
    VkImageUsageFlags flags{};

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eTransferSrc))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eTransferDst))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eSampled))
    {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eStorage))
    {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eColorAttachment))
    {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eDepthStencilAttachment))
    {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eTransientAttachment))
    {
        flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }

    if (flagBits.HasFlag(RHITextureUsageFlagBits::eInputAttachment))
    {
        flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }

    return flags;
}

VkFormat ToVkFormat(DataFormat format)
{
    return static_cast<VkFormat>(format);
}

VkAttachmentLoadOp ToVkAttachmentLoadOp(RHIRenderTargetLoadOp loadOp)
{
    return static_cast<VkAttachmentLoadOp>(loadOp);
}

VkAttachmentStoreOp ToVkAttachmentStoreOp(RHIRenderTargetStoreOp storeOp)
{
    return static_cast<VkAttachmentStoreOp>(storeOp);
}

VkImageLayout ToVkImageLayout(RHITextureLayout layout)
{
    VkImageLayout result{};

    switch (layout)
    {
        case RHITextureLayout::eUndefined: result = VK_IMAGE_LAYOUT_UNDEFINED; break;
        case RHITextureLayout::eGeneral: result = VK_IMAGE_LAYOUT_GENERAL; break;
        case RHITextureLayout::eColorTarget: result = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; break;
        case RHITextureLayout::eDepthStencilTarget: result = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; break;
        case RHITextureLayout::eShaderReadOnly: result = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; break;
        case RHITextureLayout::eTransferSrc: result = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; break;
        case RHITextureLayout::eTransferDst: result = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; break;
        default: result = VK_IMAGE_LAYOUT_UNDEFINED; break;
    }

    return result;
}

VkImageAspectFlags ToVkAspectFlags(RHIFlags<RHITextureAspectFlagBits> aspect)
{
    return static_cast<VkImageAspectFlags>(aspect);
}

VkFilter ToVkFilter(RHISamplerFilter filter)
{
    return static_cast<VkFilter>(filter);
}

VkSamplerAddressMode ToVkSamplerAddressMode(RHISamplerRepeatMode mode)
{
    return static_cast<VkSamplerAddressMode>(mode);
}

VkBorderColor ToVkBorderColor(RHISamplerBorderColor color)
{
    return static_cast<VkBorderColor>(color);
}

VkClearColorValue ToVkClearColor(const Color& color, DataFormat format)
{
    VkClearColorValue result{};
    const float       components[] = {color.r, color.g, color.b, color.a};
    for (uint32_t i = 0; i < 4; ++i)
    {
        const double value = std::isnan(components[i]) ? 0.0 : static_cast<double>(components[i]);
        if (FormatIsUnsignedInteger(format))
        {
            result.uint32[i] = static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(UINT32_MAX)));
        }
        else if (FormatIsSignedInteger(format))
        {
            result.int32[i] =
                static_cast<int32_t>(std::clamp(value, static_cast<double>(INT32_MIN), static_cast<double>(INT32_MAX)));
        }
        else
        {
            result.float32[i] = components[i];
        }
    }
    return result;
}

VkClearDepthStencilValue ToVkClearDepthStencil(const RHIRenderTargetClearValue& clearValue)
{
    VkClearDepthStencilValue depthStencilValue{};

    depthStencilValue.depth   = clearValue.depth;

    depthStencilValue.stencil = clearValue.stencil;

    return depthStencilValue;
}

void ToVkImageSubresourceRange(const RHITextureSubresourceRange& range, VkImageSubresourceRange* pVkRange)
{
    *pVkRange                = {};

    pVkRange->aspectMask     = ToVkAspectFlags(range.aspect);

    pVkRange->layerCount     = range.layerCount;

    pVkRange->levelCount     = range.levelCount;

    pVkRange->baseArrayLayer = range.baseArrayLayer;

    pVkRange->baseMipLevel   = range.baseMipLevel;
}

void ToVkImageSubresourceLayers(const RHITextureSubresourceLayers& layers, VkImageSubresourceLayers* pVkLayers)
{
    *pVkLayers                = {};

    pVkLayers->aspectMask     = ToVkAspectFlags(layers.aspect);

    pVkLayers->layerCount     = layers.layerCount;

    pVkLayers->mipLevel       = layers.mipmap;

    pVkLayers->baseArrayLayer = layers.baseArrayLayer;
}

void ToVkImageCopy(const RHITextureCopyRegion& region, VkImageCopy* pCopy)
{
    *pCopy = {};

    ToVkImageSubresourceLayers(region.srcSubresources, &pCopy->srcSubresource);

    ToVkImageSubresourceLayers(region.dstSubresources, &pCopy->dstSubresource);

    pCopy->srcOffset.x   = region.srcOffset.x;

    pCopy->srcOffset.y   = region.srcOffset.y;

    pCopy->srcOffset.z   = region.srcOffset.z;

    pCopy->dstOffset.x   = region.dstOffset.x;

    pCopy->dstOffset.y   = region.dstOffset.y;

    pCopy->dstOffset.z   = region.dstOffset.z;

    pCopy->extent.width  = region.size.x;

    pCopy->extent.height = region.size.y;

    pCopy->extent.depth  = region.size.z;
}

void ToVkImageBlit(const RHITextureBlitRegion& region, VkImageBlit* pBlit)
{
    *pBlit = {};

    ToVkImageSubresourceLayers(region.srcSubresources, &pBlit->srcSubresource);

    ToVkImageSubresourceLayers(region.dstSubresources, &pBlit->dstSubresource);

    pBlit->srcOffsets[0].x = region.srcOffset0.x;

    pBlit->srcOffsets[0].y = region.srcOffset0.y;

    pBlit->srcOffsets[0].z = region.srcOffset0.z;

    pBlit->srcOffsets[1].x = region.srcOffset1.x;

    pBlit->srcOffsets[1].y = region.srcOffset1.y;

    pBlit->srcOffsets[1].z = region.srcOffset1.z;

    pBlit->dstOffsets[0].x = region.dstOffset0.x;

    pBlit->dstOffsets[0].y = region.dstOffset0.y;

    pBlit->dstOffsets[0].z = region.dstOffset0.z;

    pBlit->dstOffsets[1].x = region.dstOffset1.x;

    pBlit->dstOffsets[1].y = region.dstOffset1.y;

    pBlit->dstOffsets[1].z = region.dstOffset1.z;
}

void ToVkBufferImageCopy(const RHIBufferTextureCopyRegion& region, VkBufferImageCopy* pCopy)
{
    *pCopy = {};

    ToVkImageSubresourceLayers(region.textureSubresources, &pCopy->imageSubresource);

    pCopy->bufferOffset       = region.bufferOffset;

    pCopy->imageOffset.x      = region.textureOffset.x;

    pCopy->imageOffset.y      = region.textureOffset.y;

    pCopy->imageOffset.z      = region.textureOffset.z;

    pCopy->imageExtent.width  = region.textureSize.x;

    pCopy->imageExtent.height = region.textureSize.y;

    pCopy->imageExtent.depth  = region.textureSize.z;
}

VkPipelineRasterizationStateCreateInfo ToVkRasterizationState(const RHIGfxPipelineRasterizationState& state)
{
    VkPipelineRasterizationStateCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};

    info.cullMode                = ToVkCullModeFlags(state.cullMode);
    info.frontFace               = ToVkFrontFace(state.frontFace);
    info.depthClampEnable        = state.enableDepthClamp;
    info.rasterizerDiscardEnable = state.discardPrimitives;
    info.polygonMode             = state.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    info.depthBiasEnable         = state.enableDepthBias;
    info.depthBiasClamp          = state.depthBiasClamp;
    info.depthBiasConstantFactor = state.depthBiasConstantFactor;
    info.depthBiasSlopeFactor    = state.depthBiasSlopeFactor;
    info.lineWidth               = state.lineWidth;

    return info;
}

VkPipelineDepthStencilStateCreateInfo ToVkDepthStencilState(const RHIGfxPipelineDepthStencilState& state)
{
    VkPipelineDepthStencilStateCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    info.depthTestEnable                   = state.enableDepthTest;
    info.depthWriteEnable                  = state.enableDepthWrite;
    info.depthCompareOp                    = ToVkCompareOp(state.depthCompareOp);
    info.depthBoundsTestEnable             = state.enableDepthBoundsTest;
    info.stencilTestEnable                 = state.enableStencilTest;
    info.minDepthBounds                    = state.minDepthBounds;
    info.maxDepthBounds                    = state.maxDepthBounds;

    const RHIStencilOpState* faces[]       = {&state.frontOp, &state.backOp};
    VkStencilOpState*        nativeFaces[] = {&info.front, &info.back};

    for (uint32_t face = 0; face < 2; ++face)
    {
        nativeFaces[face]->failOp      = ToVkStencilOp(faces[face]->fail);
        nativeFaces[face]->passOp      = ToVkStencilOp(faces[face]->pass);
        nativeFaces[face]->depthFailOp = ToVkStencilOp(faces[face]->depthFail);
        nativeFaces[face]->compareOp   = ToVkCompareOp(faces[face]->compare);
        nativeFaces[face]->compareMask = faces[face]->compareMask;
        nativeFaces[face]->writeMask   = faces[face]->writeMask;
        nativeFaces[face]->reference   = faces[face]->reference;
    }

    return info;
}

} // namespace zen
