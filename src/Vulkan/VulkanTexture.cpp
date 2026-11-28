#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanResourceSharing.h"
#include "Vulkan/VulkanTexture.h"
#include "Core/RHIValidation.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanMemory.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanResourceAllocator.h"
#include "Vulkan/VulkanTypes.h"

namespace zen
{
VkImageCreateInfo BuildImageCreateInfo(const RHITextureCreateInfo& info)
{
    VkImageCreateInfo imageCI;

    InitVkStruct(imageCI, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

    imageCI.extent.width  = info.width;

    imageCI.extent.height = info.height;

    imageCI.extent.depth  = info.depth;

    imageCI.samples       = ToVkSampleCountFlagBits(info.samples);

    imageCI.tiling        = VK_IMAGE_TILING_OPTIMAL;

    imageCI.arrayLayers   = info.arrayLayers;

    imageCI.mipLevels     = info.mipmaps;

    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    imageCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    if (info.type == RHITextureType::eCube)
    {
        imageCI.imageType = VK_IMAGE_TYPE_2D;

        imageCI.flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    else
    {
        imageCI.imageType = ToVkImageType(info.type);
    }

    imageCI.usage  = ToVkImageUsageFlags(info.usageFlags);

    imageCI.format = ToVkFormat(info.format);

    if (info.mutableFormat != false)
    {
        imageCI.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }

    return imageCI;
}

float ClampSamplerAnisotropy(float requested, float limit)
{
    return std::isnan(requested) ? 1.0f : std::clamp(requested, 1.0f, limit);
}

namespace
{
bool ImageUsageAllowsViews(VkImageUsageFlags usage)
{
    constexpr VkImageUsageFlags viewUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                                          | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    return (usage & viewUsage) != 0;
}

bool IsTextureViewFormatSupported(VulkanRHI& rhi, const RHITextureCreateInfo& image, DataFormat format)
{
    // Compressed images require a compatible block class, not just equal bytes.
    // Depth/stencil formats are only compatible with themselves.
    bool supported = GetTextureFormatBlockInfo(format).bytes != 0
                  && (format == image.format
                      || (image.mutableFormat && GetTextureFormatAspects(image.format).HasFlag(RHITextureAspectFlagBits::eColor)
                          && GetTextureFormatAspects(format).HasFlag(RHITextureAspectFlagBits::eColor)
                          && AreTextureColorViewFormatsCompatible(image.format, format)));
    if (supported && format != image.format)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(rhi.GetDevice()->GetPhysicalDeviceHandle(), ToVkFormat(format), &properties);

        VkFormatFeatureFlags required = 0;
        if (image.usageFlags.HasFlag(RHITextureUsageFlagBits::eSampled))
        {
            required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        }
        if (image.usageFlags.HasFlag(RHITextureUsageFlagBits::eStorage))
        {
            required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        }
        if (image.usageFlags.HasFlag(RHITextureUsageFlagBits::eColorAttachment)
            || image.usageFlags.HasFlag(RHITextureUsageFlagBits::eInputAttachment))
        {
            required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        }
        // Only color formats can differ from the image format. Identical formats were checked at image creation.
        supported = properties.optimalTilingFeatures != 0 && (properties.optimalTilingFeatures & required) == required;
    }
    return supported;
}

const char* ValidateTextureDescription(VulkanRHI& rhi, const RHITextureCreateInfo& info)
{
    const char* error = ValidateTextureCreateInfo(info, rhi.QueryGPUInfo());
    if (error == nullptr && GetTextureFormatBlockInfo(info.format).width > 1
        && rhi.GetDevice()->GetPhysicalDeviceFeatures().textureCompressionBC != VK_TRUE)
    {
        error = "BC texture compression is unsupported";
    }
    if (error == nullptr)
    {
        const VkImageCreateInfo native = BuildImageCreateInfo(info);
        VkImageFormatProperties properties{};
        const VkResult          result =
            vkGetPhysicalDeviceImageFormatProperties(rhi.GetDevice()->GetPhysicalDeviceHandle(), native.format,
                                                     native.imageType, native.tiling, native.usage, native.flags, &properties);
        if (result != VK_SUCCESS)
        {
            error = "format/type/usage combination is unsupported";
        }
        else if (info.width > properties.maxExtent.width || info.height > properties.maxExtent.height
                 || info.depth > properties.maxExtent.depth || info.arrayLayers > properties.maxArrayLayers
                 || info.mipmaps > properties.maxMipLevels || (properties.sampleCounts & native.samples) == 0)
        {
            error = "image exceeds device format limits";
        }
    }
    return error;
}
} // namespace

RHISampler* VulkanRHI::CreateSampler(const RHISamplerCreateInfo& createInfo)
{
    RHIWorkerTimer timer(RHIWorkerTime::eResource);
    return AreSubmissionsBlocked() ? nullptr : VulkanSampler::CreateObject(this, createInfo).GetValueOr(nullptr);
}

RHIResult<VulkanSampler*> VulkanSampler::CreateObject(VulkanRHI* pRHI, const RHISamplerCreateInfo& createInfo)
{
    const char* rule = ValidateSamplerCreateInfo(createInfo, pRHI->QueryGPUInfo());

    if (rule == nullptr
        && std::abs(createInfo.lodBias) > pRHI->GetDevice()->GetPhysicalDeviceProperties().limits.maxSamplerLodBias)
    {
        rule = "LOD bias exceeds the device limit";
    }

    if (rule != nullptr)
    {
        ZEN_RHI_LOG_ERROR("Sampler description: {}", rule);
    }

    VulkanSampler* sampler = rule == nullptr ? NewVulkanResource<VulkanSampler>(pRHI, pRHI, createInfo) : nullptr;

    return sampler != nullptr ? KeepIfInitialized(sampler, sampler->Init())
                              : RejectDescription<VulkanSampler>(rule, __FILE__, __LINE__);
}

RHIError VulkanSampler::Init()
{
    RHIError error{};

    VkSamplerCreateInfo samplerCI;

    InitVkStruct(samplerCI, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

    samplerCI.magFilter = ToVkFilter(m_baseInfo.magFilter);

    samplerCI.minFilter = ToVkFilter(m_baseInfo.minFilter);

    samplerCI.mipmapMode =
        m_baseInfo.mipFilter == RHISamplerFilter::eLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

    samplerCI.addressModeU = ToVkSamplerAddressMode(m_baseInfo.repeatU);

    samplerCI.addressModeV = ToVkSamplerAddressMode(m_baseInfo.repeatV);

    samplerCI.addressModeW = ToVkSamplerAddressMode(m_baseInfo.repeatW);

    samplerCI.mipLodBias   = m_baseInfo.lodBias;

    samplerCI.anisotropyEnable =
        m_baseInfo.useAnisotropy && (m_pRHI->GetDevice()->GetPhysicalDeviceFeatures().samplerAnisotropy == VK_TRUE);

    samplerCI.maxAnisotropy = ClampSamplerAnisotropy(
        m_baseInfo.maxAnisotropy, m_pRHI->GetDevice()->GetPhysicalDeviceProperties().limits.maxSamplerAnisotropy);

    samplerCI.compareEnable           = m_baseInfo.enableCompare;

    samplerCI.compareOp               = ToVkCompareOp(m_baseInfo.compareOp);

    samplerCI.minLod                  = m_baseInfo.minLod;

    samplerCI.maxLod                  = m_baseInfo.maxLod;

    samplerCI.borderColor             = ToVkBorderColor(m_baseInfo.borderColor);

    samplerCI.unnormalizedCoordinates = m_baseInfo.unnormalizedUVW;

    const VkResult result             = vkCreateSampler(m_pRHI->GetVkDevice(), &samplerCI, nullptr, &m_vkSampler);

    if (result != VK_SUCCESS)
    {
        m_vkSampler = VK_NULL_HANDLE;

        error       = m_pRHI->ReportCreationFailure(result, "vkCreateSampler", __FILE__, __LINE__);

        ZEN_RHI_LOG_ERROR("vkCreateSampler failed: {}", GetResultString(result));
    }

    return error;
}

void VulkanSampler::Destroy()
{
    vkDestroySampler(m_pRHI->GetVkDevice(), m_vkSampler, nullptr);

    this->~VulkanSampler();

    VersatileResource::Free(m_pRHI->GetResourceAllocator(), this);
}

RHITexture* VulkanRHI::CreateTexture(const RHITextureCreateInfo& createInfo)
{
    RHIWorkerTimer timer(RHIWorkerTime::eResource);
    return AreSubmissionsBlocked() ? nullptr : VulkanTexture::CreateObject(this, createInfo).GetValueOr(nullptr);
}

RHITextureView* VulkanRHI::CreateTextureView(RHITexture* pBaseTexture, const RHITextureViewCreateInfo& createInfo)
{
    RHIWorkerTimer  timer(RHIWorkerTime::eResource);
    RHITextureView* view = nullptr;

    if (pBaseTexture == nullptr)
    {
        ZEN_RHI_LOG_ERROR("Cannot create a view of a null texture");
    }
    else if (!AreSubmissionsBlocked())
    {
        view = pBaseTexture->CreateView(createInfo);
    }

    return view;
}

RHIResult<VulkanTexture*> VulkanTexture::CreateObject(VulkanRHI* pRHI, const RHITextureCreateInfo& createInfo)
{
    const char* rule = ValidateTextureDescription(*pRHI, createInfo);

    if (rule != nullptr)
    {
        ZEN_RHI_LOG_ERROR("Texture '{}': {}", createInfo.tag.c_str(), rule);
    }

    VulkanTexture* texture = rule == nullptr ? NewVulkanResource<VulkanTexture>(pRHI, pRHI, createInfo) : nullptr;

    return texture != nullptr ? KeepIfInitialized(texture, texture->Init())
                              : RejectDescription<VulkanTexture>(rule, __FILE__, __LINE__);
}

RHITextureView* VulkanTexture::CreateView(const RHITextureViewCreateInfo& createInfo)
{
    return m_pRHI->GetRuntime()
        .thread.Invoke(&VulkanTexture::CreateViewOnRHIThread, this, std::cref(createInfo))
        .GetValueOr(nullptr);
}

RHIResult<VulkanTextureView*> VulkanTexture::CreateViewOnRHIThread(const RHITextureViewCreateInfo& createInfo)
{
    const char* error = nullptr;

    if (!ImageUsageAllowsViews(m_vkImageCI.usage))
    {
        error = "Texture usage does not permit image views";
    }
    else if (!IsTextureViewFormatSupported(*m_pRHI, m_baseInfo, createInfo.format))
    {
        error = "Texture view format is incompatible or unsupported for the image usage";
    }
    else
    {
        error = ValidateTextureViewCreateInfo(m_baseInfo, createInfo, m_pRHI->QueryGPUInfo());
    }

    if (error != nullptr)
    {
        ZEN_RHI_LOG_ERROR("{}", error);
    }

    RHIResult<VulkanTextureView*> view = error == nullptr ? VulkanTextureView::CreateObject(this, createInfo)
                                                          : RejectDescription<VulkanTextureView>(error, __FILE__, __LINE__);

    if (view)
    {
        RegisterOwnedView(view.GetValue());
    }

    return view;
}

RHITextureView* VulkanTexture::GetAttachmentView()
{
    if (m_baseInfo.type == RHITextureType::e3D)
    {
        ZEN_RHI_LOG_ERROR("Rendering to 3D texture slices is not supported");
    }
    else if (m_pAttachmentView == nullptr)
    {
        if (m_baseInfo.mipmaps == 1 && m_baseInfo.type != RHITextureType::eCube)
        {
            m_pAttachmentView = m_pDefaultView;
        }
        else
        {
            RHITextureViewCreateInfo info{};

            info.format       = m_baseInfo.format;

            info.type         = m_baseInfo.type == RHITextureType::eCube ? RHITextureType::e2D : m_baseInfo.type;

            info.arrayLayers  = m_baseInfo.arrayLayers;

            m_pAttachmentView = CreateView(info);
        }
    }

    return m_pAttachmentView;
}

VkImageView VulkanTexture::GetVkImageView() const
{
    ZEN_RHI_VERIFY(m_pDefaultView != nullptr);

    const VulkanTextureView* pDefaultView = TO_VK_TEXTURE_VIEW(m_pDefaultView);

    const VkImageView imageView           = pDefaultView != nullptr ? pDefaultView->GetVkImageView() : VK_NULL_HANDLE;

    return imageView;
}

RHIError VulkanTexture::Init()
{
    VkImageCreateInfo imageCI = BuildImageCreateInfo(m_baseInfo);

    const bool transfer       = (imageCI.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) != 0;

    // Replaced by the allocation result once the queue-sharing setup runs the allocation.
    RHIError error = MakeRHIError(RHIErrorCode::eBackendFailure, "Create texture", __FILE__, __LINE__);

    AllocateWithQueueSharing(imageCI, m_pRHI->GetDevice()->GetQueueSharing(transfer), [this, &imageCI, &error] {
        error = m_pRHI->GetMemoryAllocator()->AllocImage(&imageCI, m_baseInfo.cpuReadable, &m_vkImage, &m_memAlloc);
    });

    if (!error.IsFailure())
    {
        m_vkImageCI = imageCI;

        if (!m_baseInfo.tag.empty())
        {
            m_pRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(m_vkImage), m_baseInfo.tag);
        }

        m_vkAspectFlags = ToVkAspectFlags(GetTextureFormatAspects(m_baseInfo.format));

        if (ImageUsageAllowsViews(imageCI.usage))
        {
            RHITextureViewCreateInfo viewCI{};

            viewCI.format       = m_baseInfo.format;

            viewCI.type         = m_baseInfo.type;

            viewCI.arrayLayers  = m_baseInfo.arrayLayers;

            viewCI.mipLevels    = m_baseInfo.mipmaps;

            viewCI.baseMipLevel = 0;

            viewCI.tag          = m_baseInfo.tag;

            const RHIResult<VulkanTextureView*> view =
                m_pRHI->GetRuntime().thread.Invoke(&VulkanTexture::CreateViewOnRHIThread, this, std::cref(viewCI));

            m_pDefaultView = view.GetValueOr(nullptr);

            error          = view.GetError();
        }

        const bool needsAttachmentView = m_baseInfo.type != RHITextureType::e3D
                                      && (m_baseInfo.usageFlags.HasFlag(RHITextureUsageFlagBits::eColorAttachment)
                                          || m_baseInfo.usageFlags.HasFlag(RHITextureUsageFlagBits::eDepthStencilAttachment));

        if (!error.IsFailure() && needsAttachmentView && GetAttachmentView() == nullptr)
        {
            error = MakeRHIError(RHIErrorCode::eBackendFailure, "Create texture attachment view", __FILE__, __LINE__);
        }
    }
    else
    {
        ZEN_RHI_LOG_ERROR("Texture '{}' allocation failed", m_baseInfo.tag.c_str());
    }

    return error;
}

void VulkanTexture::Destroy()
{
    DestroyOwnedViews();

    if (m_vkImage != VK_NULL_HANDLE)
    {
        m_pRHI->GetMemoryAllocator()->FreeImage(m_vkImage, m_memAlloc);
    }

    this->~VulkanTexture();

    VersatileResource::Free(m_pRHI->GetResourceAllocator(), this);
}

RHIResult<VulkanTextureView*> VulkanTextureView::CreateObject(VulkanTexture*                  pTexture,
                                                              const RHITextureViewCreateInfo& createInfo)
{
    VulkanTextureView* view = NewVulkanResource<VulkanTextureView>(pTexture->GetRHI(), pTexture, createInfo);

    return KeepIfInitialized(view, view->Init());
}

RHIError VulkanTextureView::Init()
{
    RHIError error{};

    VulkanTexture* pVkTexture        = TO_VK_TEXTURE(m_pTexture);

    RHITextureSubresourceRange range = m_subResourceRange;

    // Combined depth/stencil formats use the existing depth-sampling default.
    if (FormatIsDepthStencil(m_viewInfo.format) && m_viewInfo.aspect.IsEmpty())
    {
        range.aspect = int64_t(RHITextureAspectFlagBits::eDepth);
    }

    const VkImageViewCreateInfo imageViewCI =
        MakeVkImageViewCreateInfo(m_viewInfo.type, m_viewInfo.format, pVkTexture->GetVkImage(), range, m_viewInfo.arrayView);

    const VkResult result = vkCreateImageView(m_pRHI->GetVkDevice(), &imageViewCI, nullptr, &m_vkImageView);

    m_vkImageViewType     = result == VK_SUCCESS ? imageViewCI.viewType : VK_IMAGE_VIEW_TYPE_MAX_ENUM;

    if (result != VK_SUCCESS)
    {
        m_vkImageView = VK_NULL_HANDLE;

        error         = m_pRHI->ReportCreationFailure(result, "vkCreateImageView", __FILE__, __LINE__);

        ZEN_RHI_LOG_ERROR("Texture view '{}' creation failed: {}", m_viewInfo.tag.c_str(), GetResultString(result));
    }
    else if (!m_viewInfo.tag.empty())
    {
        m_pRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(m_vkImageView),
                                           m_viewInfo.tag);
    }

    return error;
}

void VulkanTextureView::Destroy()
{
    if (m_vkImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_pRHI->GetVkDevice(), m_vkImageView, nullptr);

        m_vkImageView = nullptr;
    }

    this->~VulkanTextureView();

    VersatileResource::Free(m_pRHI->GetResourceAllocator(), this);
}

} // namespace zen
