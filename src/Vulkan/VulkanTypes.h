#pragma once
#include "VulkanHeaders.h"
#include "ZenRHI/RHICommon.h"

#include "VulkanResourceCast.h"
#include <algorithm>
#include <iterator>

namespace zen
{
// The device limits a descriptor counts against, in the order ShaderLayoutFitsLimits checks them.
enum class VulkanDescriptorLimitClass : uint32_t
{
    eSampler,
    eUniformBuffer,
    eStorageBuffer,
    eSampledImage,
    eStorageImage,
    eInputAttachment,
    eAccelerationStructure,
    eMax
};

// What one shader resource type needs from Vulkan.
struct VulkanDescriptorTraits
{
    // Also the pool size type. Uniform buffers bind as dynamic uniform buffers.
    VkDescriptorType           type;
    VulkanDescriptorLimitClass limitClass;
    // Resources per array element: a combined sampler binds its sampler, then its texture or texel buffer.
    uint32_t stride;
    // The usage the bound texture or buffer must have; none for samplers and acceleration structures.
    RHITextureUsageFlagBits textureUsage;
    RHIBufferUsageFlagBits  bufferUsage;
    VkImageLayout           imageLayout;
};

// Indexed by RHIShaderResourceType; the last entry answers eMax and out-of-range types.
inline constexpr VulkanDescriptorTraits kVulkanDescriptorTraits[] = {
    {VK_DESCRIPTOR_TYPE_SAMPLER, VulkanDescriptorLimitClass::eSampler, 1, {}, {}, VK_IMAGE_LAYOUT_UNDEFINED},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
     VulkanDescriptorLimitClass::eSampledImage,
     1,
     RHITextureUsageFlagBits::eSampled,
     {},
     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
     VulkanDescriptorLimitClass::eSampledImage,
     2,
     RHITextureUsageFlagBits::eSampled,
     {},
     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
     VulkanDescriptorLimitClass::eStorageImage,
     1,
     RHITextureUsageFlagBits::eStorage,
     {},
     VK_IMAGE_LAYOUT_GENERAL},
    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
     VulkanDescriptorLimitClass::eSampledImage,
     1,
     {},
     RHIBufferUsageFlagBits::eTextureBuffer,
     VK_IMAGE_LAYOUT_UNDEFINED},
    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
     VulkanDescriptorLimitClass::eSampledImage,
     2,
     {},
     RHIBufferUsageFlagBits::eTextureBuffer,
     VK_IMAGE_LAYOUT_UNDEFINED},
    {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
     VulkanDescriptorLimitClass::eStorageImage,
     1,
     {},
     RHIBufferUsageFlagBits::eImageBuffer,
     VK_IMAGE_LAYOUT_UNDEFINED},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
     VulkanDescriptorLimitClass::eUniformBuffer,
     1,
     {},
     RHIBufferUsageFlagBits::eUniformBuffer,
     VK_IMAGE_LAYOUT_UNDEFINED},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
     VulkanDescriptorLimitClass::eStorageBuffer,
     1,
     {},
     RHIBufferUsageFlagBits::eStorageBuffer,
     VK_IMAGE_LAYOUT_UNDEFINED},
    {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
     VulkanDescriptorLimitClass::eInputAttachment,
     1,
     RHITextureUsageFlagBits::eInputAttachment,
     {},
     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
     VulkanDescriptorLimitClass::eAccelerationStructure,
     1,
     {},
     {},
     VK_IMAGE_LAYOUT_UNDEFINED},
    {VK_DESCRIPTOR_TYPE_MAX_ENUM, VulkanDescriptorLimitClass::eMax, 1, {}, {}, VK_IMAGE_LAYOUT_UNDEFINED},
};
static_assert(std::size(kVulkanDescriptorTraits) == ToUnderlying(RHIShaderResourceType::eMax) + 1);

inline const VulkanDescriptorTraits& GetVulkanDescriptorTraits(RHIShaderResourceType type)
{
    return kVulkanDescriptorTraits[std::min(ToUnderlying(type), ToUnderlying(RHIShaderResourceType::eMax))];
}

VkShaderStageFlagBits ShaderStageToVkShaderStageFlagBits(RHIShaderStage stage);

VkShaderStageFlags ShaderStageFlagsBitsToVkShaderStageFlags(RHIFlags<RHIShaderStageFlagBits> stageFlags);

VkPrimitiveTopology ToVkPrimitiveTopology(RHIDrawPrimitiveType type);

VkCullModeFlags ToVkCullModeFlags(RHIPolygonCullMode mode);

VkFrontFace ToVkFrontFace(RHIPolygonFrontFace frontFace);

VkSampleCountFlagBits ToVkSampleCountFlagBits(SampleCount count);

VkCompareOp ToVkCompareOp(RHIDepthCompareOperator op);

VkStencilOp ToVkStencilOp(RHIStencilOp op);

VkPipelineRasterizationStateCreateInfo ToVkRasterizationState(const RHIGfxPipelineRasterizationState& state);

VkPipelineDepthStencilStateCreateInfo ToVkDepthStencilState(const RHIGfxPipelineDepthStencilState& state);

VkLogicOp ToVkLogicOp(RHIBlendLogicOp op);

VkBlendOp ToVkBlendOp(RHIBlendOp op);

VkBlendFactor ToVkBlendFactor(RHIBlendFactor factor);

VkDynamicState ToVkDynamicState(RHIDynamicState state);

VkImageType ToVkImageType(RHITextureType type);

VkImageViewType ToVkImageViewType(RHITextureType type);

VkImageViewCreateInfo MakeVkImageViewCreateInfo(RHITextureType                    type,
                                                DataFormat                        format,
                                                VkImage                           image,
                                                const RHITextureSubresourceRange& range,
                                                bool                              arrayView = false);

VkImageUsageFlags ToVkImageUsageFlags(RHIFlags<RHITextureUsageFlagBits> flags);

VkFormat ToVkFormat(DataFormat format);

VkAttachmentLoadOp ToVkAttachmentLoadOp(RHIRenderTargetLoadOp loadOp);

VkAttachmentStoreOp ToVkAttachmentStoreOp(RHIRenderTargetStoreOp storeOp);

VkImageLayout ToVkImageLayout(RHITextureLayout layout);

VkImageAspectFlags ToVkAspectFlags(RHIFlags<RHITextureAspectFlagBits> aspect);

VkFilter ToVkFilter(RHISamplerFilter filter);

VkSamplerAddressMode ToVkSamplerAddressMode(RHISamplerRepeatMode mode);

VkBorderColor ToVkBorderColor(RHISamplerBorderColor color);

VkClearColorValue ToVkClearColor(const Color& color, DataFormat format);

VkClearDepthStencilValue ToVkClearDepthStencil(const RHIRenderTargetClearValue& clearValue);



void ToVkImageSubresourceRange(const RHITextureSubresourceRange& range, VkImageSubresourceRange* pVkRange);

void ToVkImageSubresourceLayers(const RHITextureSubresourceLayers& layers, VkImageSubresourceLayers* pVkLayers);

void ToVkImageCopy(const RHITextureCopyRegion& region, VkImageCopy* pCopy);

void ToVkImageBlit(const RHITextureBlitRegion& region, VkImageBlit* pBlit);

void ToVkBufferImageCopy(const RHIBufferTextureCopyRegion& region, VkBufferImageCopy* pCopy);
} // namespace zen
