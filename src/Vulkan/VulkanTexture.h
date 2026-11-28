#pragma once
#include "VulkanHeaders.h"
#include "VulkanMemory.h"
#include "ZenRHI/RHIResource.h"

namespace zen
{
class VulkanTextureView;
class VulkanRHI;
VkImageCreateInfo BuildImageCreateInfo(const RHITextureCreateInfo& info);
float             ClampSamplerAnisotropy(float requested, float limit);

class VulkanSampler : public RHISampler
{
public:
    static RHIResult<VulkanSampler*> CreateObject(VulkanRHI* pRHI, const RHISamplerCreateInfo& createInfo);

    VkSampler GetVkSampler() const
    {
        return m_vkSampler;
    }

protected:
    RHIError Init();

    template <typename T, typename... Args> friend T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args);

    void Destroy() override;

private:
    VulkanSampler(VulkanRHI* pRHI, const RHISamplerCreateInfo& createInfo) : RHISampler(createInfo), m_pRHI(pRHI) {}

    VulkanRHI* m_pRHI{nullptr};

    VkSampler m_vkSampler{VK_NULL_HANDLE};
};

class VulkanTexture : public RHITexture
{
public:
    static RHIResult<VulkanTexture*> CreateObject(VulkanRHI* pRHI, const RHITextureCreateInfo& createInfo);

    VulkanRHI* GetRHI() const
    {
        return m_pRHI;
    }

    bool IsAsyncComputeAccessible() const override
    {
        return true; // Engine allocation sharing includes graphics and compute families.
    }

    RHITextureView* CreateView(const RHITextureViewCreateInfo& createInfo) override;

    VkImageSubresourceRange GetVkSubresourceRange() const
    {
        VkImageSubresourceRange range{};
        range.aspectMask     = m_vkAspectFlags;
        range.baseMipLevel   = 0;
        range.levelCount     = m_vkImageCI.mipLevels;
        range.baseArrayLayer = 0;
        range.layerCount     = m_vkImageCI.arrayLayers;

        return range;
    }

    static VkImageSubresourceRange GetVkSubresourceRange(VkImageAspectFlags aspect,
                                                         uint32_t           baseMipLevel   = 0,
                                                         uint32_t           levelCount     = VK_REMAINING_MIP_LEVELS,
                                                         uint32_t           baseArrayLayer = 0,
                                                         uint32_t           layerCount     = VK_REMAINING_ARRAY_LAYERS)
    {
        VkImageSubresourceRange range{};
        range.aspectMask     = aspect;
        range.baseMipLevel   = baseMipLevel;
        range.levelCount     = levelCount;
        range.baseArrayLayer = baseArrayLayer;
        range.layerCount     = layerCount;

        return range;
    }

    VkImage GetVkImage() const
    {
        return m_vkImage;
    }

    VkImageView GetVkImageView() const;

    // Borrowed, texture-owned view of mip 0 and all array layers for rendering.
    RHITextureView* GetAttachmentView();

    const VulkanMemoryAllocation& GetMemoryAllocation() const
    {
        return m_memAlloc;
    }

    uint64_t GetAllocationSize() const override
    {
        return m_memAlloc.info.size;
    }

    const VkImageCreateInfo& GetVkImageCreateInfo() const
    {
        return m_vkImageCI;
    }

    VkImageUsageFlags GetVkImageUsage() const
    {
        return m_vkImageCI.usage;
    }

protected:
    RHIError Init();

    template <typename T, typename... Args> friend T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args);

    void Destroy() override;

private:
    VulkanTexture(VulkanRHI* pRHI, const RHITextureCreateInfo& createInfo) : RHITexture(createInfo), m_pRHI(pRHI) {}

    VulkanRHI* m_pRHI{nullptr};

    RHIResult<VulkanTextureView*> CreateViewOnRHIThread(const RHITextureViewCreateInfo& createInfo);

    VkImage                m_vkImage{VK_NULL_HANDLE};
    VkImageCreateInfo      m_vkImageCI{};
    VulkanMemoryAllocation m_memAlloc{};

    VkImageAspectFlags m_vkAspectFlags{};
    RHITextureView*    m_pAttachmentView{nullptr};
};

class VulkanTextureView : public RHITextureView
{
public:
    static RHIResult<VulkanTextureView*> CreateObject(VulkanTexture* pTexture, const RHITextureViewCreateInfo& createInfo);

    VkImageView GetVkImageView() const
    {
        return m_vkImageView;
    }

    VkImageViewType GetVkImageViewType() const
    {
        return m_vkImageViewType;
    }

protected:
    RHIError Init();

    template <typename T, typename... Args> friend T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args);

    void Destroy() override;

private:
    VulkanTextureView(VulkanTexture* pTexture, const RHITextureViewCreateInfo& createInfo) :
        RHITextureView(pTexture, createInfo), m_pRHI(pTexture->GetRHI())
    {}

    VulkanRHI* m_pRHI{nullptr};

    VkImageView     m_vkImageView{VK_NULL_HANDLE};
    VkImageViewType m_vkImageViewType{VK_IMAGE_VIEW_TYPE_MAX_ENUM};
};
} // namespace zen
