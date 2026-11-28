#pragma once
#include "ZenRHI/RHICommandList.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "Vulkan/VulkanHeaders.h"
#include "Vulkan/VulkanSwapchain.h"
#include "ZenRHI/RHIResource.h"
namespace zen
{
class VulkanRHI;
class VulkanQueue;
class VulkanDevice;
class VulkanSemaphore;
class VulkanTexture;
class VulkanCommandListContext;

class VulkanViewport : public RHIViewport
{
public:
    static VulkanViewport* CreateObject(VulkanDevice*                pDevice,
                                        zen::RHIWindowHandle         pWindow,
                                        uint32_t                     width,
                                        uint32_t                     height,
                                        bool                         enableVSync,
                                        VulkanSwapchainRecreateInfo* surfaceInfo);

    ~VulkanViewport() {}

    uint32_t GetWidth() const final
    {
        return m_width;
    }

    uint32_t GetHeight() const final
    {
        return m_height;
    }

    DataFormat GetSwapchainFormat() final
    {
        return m_pSwapchain != nullptr ? static_cast<DataFormat>(m_pSwapchain->GetBackBufferFormat()) : DataFormat::eUndefined;
    }

    DataFormat GetDepthStencilFormat() final
    {
        return m_depthFormat;
    }

    RHIStatus PrepareForPresent(RHICommandList* commands) final;

    RHIPresentResult Present() final;

    bool NeedsRecreation() const final;

    bool CanRecreate() const final;

    RHITexture* GetColorBackBuffer() final;

    RHITextureSubresourceRange GetColorBackBufferRange() final
    {
        RHITextureSubresourceRange range;
        range.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        range.layerCount     = 1;
        range.levelCount     = 1;
        range.baseMipLevel   = 0;
        range.baseArrayLayer = 0;

        return range;
    }

    RHITexture* GetDepthStencilBackBuffer() final;

    RHITextureSubresourceRange GetDepthStencilBackBufferRange() final
    {
        RHITextureSubresourceRange range{};
        range.aspect.SetFlag(RHITextureAspectFlagBits::eDepth);

        if (FormatIsDepthStencil(m_depthFormat))
        {
            range.aspect.SetFlag(RHITextureAspectFlagBits::eStencil);
        }

        range.layerCount     = 1;
        range.levelCount     = 1;
        range.baseMipLevel   = 0;
        range.baseArrayLayer = 0;

        return range;
    }

    void Resize(uint32_t width, uint32_t height) final;

protected:
    void Init();

    void Destroy() override;

private:
    friend struct VulkanViewportTestAccess;

    VulkanViewport(VulkanDevice* pDevice, zen::RHIWindowHandle pWindowPtr, uint32_t width, uint32_t height, bool enableVSync);

    bool CreateSwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo);

    // Creates the color and depth back buffers and records their first transition; a failure lands in m_recreationError.
    bool CreateBackBuffers();

    void DestroySwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo);

    bool TryAcquireNextImage();

    void RecordPresentCopy(RHICommandList* pCommandList);

    bool BeginResize(uint32_t width, uint32_t height, VulkanSwapchainRecreateInfo* recreateInfo);

    bool FinishResize(VulkanSwapchainRecreateInfo* recreateInfo, RHIError surfaceError);

    void CopyBackBufferToSwapchainImage(VkCommandBuffer cmdBufferVk,
                                        VkImage         dstImage,
                                        uint32_t        windowWidth,
                                        uint32_t        windowHeight);

    VulkanDevice* m_pDevice{nullptr};

    DataFormat       m_depthFormat;
    VulkanSwapchain* m_pSwapchain{nullptr};
    VulkanTexture*   m_pColorBackBuffer{nullptr};
    VulkanTexture*   m_pDepthStencilBackBuffer{nullptr};

    uint64_t         m_presentSignalGeneration{0};
    bool             m_presentAcquiredFailed{false};
    bool             m_suspended{false};
    RHIPresentResult m_presentResult{};
    RHIError         m_recreationError{};
};
} // namespace zen
