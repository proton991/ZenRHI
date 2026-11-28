#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/Vulkan/VulkanSurface.h"
#include "Vulkan/VulkanViewport.h"
#include "ZenRHI/RHICommandList.h"
#include "ZenFoundation/Utils/Mutex.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include "Vulkan/VulkanTexture.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanDescriptorAllocator.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanResourceAllocator.h"
#include "Vulkan/VulkanSynchronization.h"
#include "Vulkan/Platform/VulkanWindowBridge.h"

namespace zen
{
RHIViewport* VulkanRHI::CreateViewport(zen::RHIWindowHandle pWindow, uint32_t width, uint32_t height, bool enableVSync)
{
    // The viewport's swapchain takes the surface; otherwise the info destroys it on return.
    VulkanSwapchainRecreateInfo surfaceInfo;

    RHIViewport* viewport = nullptr;

    if (!AreSubmissionsBlocked())
    {
        RHIResult<VkSurfaceKHR> created = CreateWindowSurface(GetInstance(), pWindow);
        if (created)
        {
            surfaceInfo.surface = VulkanSurface(GetInstance(), created.GetValue());
            viewport = m_runtime->thread.Invoke(&VulkanViewport::CreateObject, m_pDevice, pWindow, width, height, enableVSync,
                                                &surfaceInfo);
        }
        else
        {
            ZEN_RHI_LOG_ERROR("CreateWindowSurface failed: {}",
                              GetResultString(static_cast<VkResult>(created.GetError().nativeCode)));
        }
    }

    return viewport;
}

void VulkanRHI::DestroyViewport(RHIViewport* pViewport)
{
    // As in CreateViewport, the window owner waits for the RHI thread: it may destroy the
    // native window next, so the swapchain and surface must be gone before this returns.
    m_runtime->thread.Invoke(&RHIResource::ReleaseReference, pViewport);
}

VulkanViewport* VulkanViewport::CreateObject(VulkanDevice*                pDevice,
                                             zen::RHIWindowHandle         pWindow,
                                             uint32_t                     width,
                                             uint32_t                     height,
                                             bool                         enableVSync,
                                             VulkanSwapchainRecreateInfo* surfaceInfo)
{
    ZEN_ASSERT(pDevice->GetRHI()->GetRuntime().thread.IsCurrentThread());

    VulkanViewport* pViewport = VersatileResource::AllocMem<VulkanViewport>(pDevice->GetRHI()->GetResourceAllocator());

    new (pViewport) VulkanViewport(pDevice, pWindow, width, height, enableVSync);
    pViewport->BindRuntime(&pDevice->GetRHI()->GetRuntime());

    pViewport->Init();

    if (!pViewport->CreateSwapchain(surfaceInfo))
    {
        pViewport->ReleaseReference();

        pViewport = nullptr;
    }

    return pViewport;
}

VulkanViewport::VulkanViewport(VulkanDevice*        pDevice,
                               zen::RHIWindowHandle pWindowPtr,
                               uint32_t             width,
                               uint32_t             height,
                               bool                 enableVSync) :
    RHIViewport(pWindowPtr, width, height, enableVSync), m_pDevice(pDevice)
{}

void VulkanViewport::Init()
{
    m_depthFormat = m_pDevice->GetRHI()->GetSupportedDepthFormat();

    ZEN_RHI_LOG_INFO("Viewport backbuffer depth format: {}", VkToString(static_cast<VkFormat>(m_depthFormat)));
}

void VulkanViewport::Destroy()
{
    DestroySwapchain(nullptr);

    this->~VulkanViewport();

    VersatileResource::Free(m_pDevice->GetRHI()->GetResourceAllocator(), this);
}

bool VulkanViewport::CreateSwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    bool ready        = true;

    m_recreationError = {};

    m_suspended       = m_width == 0 || m_height == 0;

    if (!m_suspended)
    {
        m_pSwapchain      = ZEN_NEW() VulkanSwapchain(m_pDevice, m_width, m_height, m_enableVSync, pRecreateInfo);

        ready             = static_cast<bool>(m_pSwapchain->GetStatus());

        m_recreationError = m_pSwapchain->GetStatus().error;

        // No images means the surface has no presentable extent yet.
        m_suspended = m_pSwapchain->GetNumSwapchainImages() == 0;

        if (!m_suspended && ready)
        {
            const VkExtent2D extent = m_pSwapchain->GetExtent();

            m_width                 = extent.width;

            m_height                = extent.height;

            // Swapchain images belong to the presentation engine until acquired. Their first
            // transition is recorded by PrepareForPresent after acquisition, with a semaphore
            // wait before CopyBackBufferToSwapchainImage overwrites the entire image.
            ready = CreateBackBuffers();
        }
    }

    return ready;
}

bool VulkanViewport::CreateBackBuffers()
{
    RHITextureCreateInfo colorTexInfo{};

    colorTexInfo.width  = m_width;

    colorTexInfo.height = m_height;

    colorTexInfo.format = GetSwapchainFormat();

    colorTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eColorAttachment);

    colorTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferSrc);

    // Frames without scene rendering clear the logical presentation image with a transfer.
    colorTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);

    colorTexInfo.type                     = RHITextureType::e2D;

    colorTexInfo.tag                      = "color_back_buffer";

    const RHIResult<VulkanTexture*> color = VulkanTexture::CreateObject(m_pDevice->GetRHI(), colorTexInfo);

    m_pColorBackBuffer                    = color.GetValueOr(nullptr);

    RHITextureCreateInfo depthStencilTexInfo{};

    depthStencilTexInfo.width  = m_width;

    depthStencilTexInfo.height = m_height;

    depthStencilTexInfo.format = GetDepthStencilFormat();

    depthStencilTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eDepthStencilAttachment);

    depthStencilTexInfo.type                     = RHITextureType::e2D;

    depthStencilTexInfo.tag                      = "depth_stencil_back_buffer";

    const RHIResult<VulkanTexture*> depthStencil = VulkanTexture::CreateObject(m_pDevice->GetRHI(), depthStencilTexInfo);

    m_pDepthStencilBackBuffer                    = depthStencil.GetValueOr(nullptr);

    const bool created                           = m_pColorBackBuffer != nullptr && m_pDepthStencilBackBuffer != nullptr;

    VulkanCommandListContext context(RHICommandContextType::eGraphics, m_pDevice);

    bool ready = created && context.IsValid() && context.EnsureRecording();

    if (ready)
    {
        const VkCommandBuffer cmdBuffer = context.GetCommandBuffer()->GetVkHandle();

        VulkanPipelineBarrier barrier;

        barrier.AddImageBarrier(m_pColorBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, m_pColorBackBuffer->GetVkSubresourceRange(), 0,
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

        barrier.AddImageBarrier(m_pDepthStencilBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                m_pDepthStencilBackBuffer->GetVkSubresourceRange(), 0,
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        barrier.Execute(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

        ready = context.SubmitRecordedWorkloads() == RHISubmissionResult::eSuccess;

        if (!ready)
        {
            m_recreationError = m_pDevice->GetRHI()->GetSubmissionFailureCause();
        }
    }
    else if (!created)
    {
        m_recreationError = color ? depthStencil.GetError() : color.GetError();
    }
    else
    {
        m_recreationError =
            RHIErrorOr(context.GetRecordingError(),
                       MakeRHIError(RHIErrorCode::eBackendFailure, "Viewport backbuffer recording", __FILE__, __LINE__));
    }

    return ready;
}

void VulkanViewport::DestroySwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    m_pDevice->WaitForIdle();

    if (m_pSwapchain != nullptr)
    {
        m_pSwapchain->Destroy(m_pDevice->GetRHI()->AreSubmissionsBlocked() ? nullptr : pRecreateInfo);

        ZEN_DELETE(m_pSwapchain);

        m_pSwapchain = nullptr;
    }

    m_presentSignalGeneration = 0;

    if (m_pColorBackBuffer)
    {
        m_pDevice->GetRHI()->DestroyTexture(m_pColorBackBuffer);

        m_pColorBackBuffer = nullptr;
    }

    if (m_pDepthStencilBackBuffer)
    {
        m_pDevice->GetRHI()->DestroyTexture(m_pDepthStencilBackBuffer);

        m_pDepthStencilBackBuffer = nullptr;
    }
}

bool VulkanViewport::BeginResize(uint32_t width, uint32_t height, VulkanSwapchainRecreateInfo* recreateInfo)
{
    ZEN_ASSERT(m_pDevice->GetRHI()->GetRuntime().thread.IsCurrentThread());

    bool rebuild = false;

    if (m_pDevice->GetRHI()->AreSubmissionsBlocked())
    {
        ZEN_RHI_LOG_ERROR("Cannot resize while Vulkan submissions are blocked");
    }
    else if (width == 0 || height == 0)
    {
        // Preserve the last usable backbuffers while the window is minimized.
        m_suspended = true;
    }
    else
    {
        m_width                    = width;

        m_height                   = height;

        const bool recreateSurface = m_pSwapchain != nullptr && m_pSwapchain->GetLastResult() == VK_ERROR_SURFACE_LOST_KHR;

        DestroySwapchain(recreateSurface ? nullptr : recreateInfo);

        if (m_pDevice->GetRHI()->AreSubmissionsBlocked())
        {
            // Destruction is still legal after device loss, but recreation is not.
            if (recreateInfo->swapchain != VK_NULL_HANDLE)
            {
                vkDestroySwapchainKHR(m_pDevice->GetVkHandle(), recreateInfo->swapchain, nullptr);
            }

            // Resetting the info destroys its surface.
            *recreateInfo = {};

            ZEN_RHI_LOG_ERROR("Cannot recreate a swapchain after a failed device idle wait");
        }
        else
        {
            rebuild = true;
        }
    }

    return rebuild;
}

bool VulkanViewport::FinishResize(VulkanSwapchainRecreateInfo* recreateInfo, RHIError surfaceError)
{
    ZEN_ASSERT(m_pDevice->GetRHI()->GetRuntime().thread.IsCurrentThread());

    bool replaceSurface = false;
    m_recreationError   = surfaceError;
    if (surfaceError.IsFailure() || !CreateSwapchain(recreateInfo))
    {
        DestroySwapchain(nullptr);

        m_suspended    = true;

        replaceSurface = !surfaceError.IsFailure() && m_recreationError.nativeCode == VK_ERROR_SURFACE_LOST_KHR
                      && !m_pDevice->GetRHI()->AreSubmissionsBlocked();
        if (!replaceSurface)
        {
            m_pDevice->GetRHI()->BlockSubmissions(
                RHIErrorOr(m_recreationError,
                           MakeRHIError(RHIErrorCode::eBackendFailure, "Viewport backbuffer recreation", __FILE__, __LINE__)));

            ZEN_RHI_LOG_ERROR("Viewport backbuffer recreation failed");
        }
    }

    ZEN_RHI_VERIFY(recreateInfo->surface.Get() == VK_NULL_HANDLE);

    ZEN_RHI_VERIFY(recreateInfo->swapchain == VK_NULL_HANDLE);

    return replaceSurface;
}

bool VulkanViewport::TryAcquireNextImage()
{
    const bool usable = !m_suspended && m_pSwapchain != nullptr;

    // A rejected presentation-copy submission did not consume the acquire semaphore.
    // Retry the already acquired image instead of leaking another swapchain acquisition.
    if (usable && m_pSwapchain->GetAcquiredImageIndex() < 0)
    {
        VulkanSemaphore* semaphore = nullptr;

        m_pSwapchain->AcquireNextImage(&semaphore);
    }

    return usable && m_pSwapchain->GetAcquiredImageIndex() >= 0;
}

void VulkanViewport::CopyBackBufferToSwapchainImage(VkCommandBuffer cmdBufferVk,
                                                    VkImage         dstImage,
                                                    uint32_t        windowWidth,
                                                    uint32_t        windowHeight)
{
    const VkImageLayout prevLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    {
        VulkanPipelineBarrier barrier;

        barrier.AddImageBarrier(m_pColorBackBuffer->GetVkImage(), prevLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                m_pColorBackBuffer->GetVkSubresourceRange(), VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT);

        barrier.AddImageBarrier(dstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1), 0,
                                VK_ACCESS_TRANSFER_WRITE_BIT);

        // TRANSFER chains the acquired-image transition after PrepareForPresent's semaphore
        // wait. COLOR_ATTACHMENT_OUTPUT also covers the backbuffer writes in this batch.
        barrier.Execute(cmdBufferVk, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    ZEN_RHI_VERIFY_MSG(m_width == windowWidth && m_height == windowHeight,
                       "Presentation backbuffer and swapchain extents must match");
    {
        VkImageCopy region{};

        region.extent.width                  = m_width;

        region.extent.height                 = m_height;

        region.extent.depth                  = 1;

        region.srcOffset.x                   = 0;

        region.srcOffset.y                   = 0;

        region.srcOffset.z                   = 0;

        region.dstOffset.x                   = 0;

        region.dstOffset.y                   = 0;

        region.dstOffset.z                   = 0;

        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;

        region.srcSubresource.layerCount     = 1;

        region.srcSubresource.baseArrayLayer = 0;

        region.srcSubresource.mipLevel       = 0;

        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;

        region.dstSubresource.layerCount     = 1;

        region.dstSubresource.baseArrayLayer = 0;

        region.dstSubresource.mipLevel       = 0;

        vkCmdCopyImage(cmdBufferVk, m_pColorBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    {
        VulkanPipelineBarrier barrier;

        barrier.AddImageBarrier(m_pColorBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prevLayout,
                                m_pColorBackBuffer->GetVkSubresourceRange(), 0,
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

        barrier.AddImageBarrier(dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1),
                                VK_ACCESS_TRANSFER_WRITE_BIT, 0);

        barrier.Execute(cmdBufferVk, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
}

void VulkanViewport::RecordPresentCopy(RHICommandList* pCommandList)
{
    m_pDevice->GetRHI()->GetRuntime().thread.CheckOwnership();

    if (!m_pDevice->GetRHI()->AreSubmissionsBlocked() && TryAcquireNextImage())
    {
        m_presentAcquiredFailed = false;

        VulkanSemaphore* pRenderingCompleteSemaphore =
            m_pSwapchain->GetRenderingCompleteSemaphore(m_pSwapchain->GetAcquiredImageIndex());

        m_presentSignalGeneration          = pRenderingCompleteSemaphore->GetSignalGeneration();

        VulkanCommandListContext* pContext = static_cast<VulkanCommandListContext*>(pCommandList->GetContext());

        const VkExtent2D extent            = m_pSwapchain->GetExtent();

        // The wait must precede the copy's command buffer to stay in the same workload.
        pContext->AddWaitSemaphore(VK_PIPELINE_STAGE_TRANSFER_BIT, m_pSwapchain->GetAcquireSemaphore());

        // A failed recording rejects the whole batch, which discards the unsubmitted wait.
        // Record nothing into its native buffer; the acquired image stays held for a retry.
        if (pContext->EnsureRecording())
        {
            CopyBackBufferToSwapchainImage(pContext->GetCommandBuffer()->GetVkHandle(),
                                           m_pSwapchain->GetSwapchainImages()[m_pSwapchain->GetAcquiredImageIndex()],
                                           extent.width, extent.height);

            pContext->AddSignalSemaphore(pRenderingCompleteSemaphore);
        }
    }
    else
    {
        m_presentAcquiredFailed = true;
    }
}

RHIPresentResult VulkanViewport::Present()
{
    m_pDevice->GetRHI()->GetRuntime().thread.CheckOwnership();

    m_presentResult = {};

    if (m_pDevice->GetRHI()->AreSubmissionsBlocked())
    {
        m_presentResult = {RHISurfaceOutcome::eFailed, RHIPresentAcceptance::eNotAttempted,
                           m_pDevice->GetRHI()->GetTerminalError()};
    }
    else if (!m_suspended && m_pSwapchain != nullptr)
    {
        if (m_presentAcquiredFailed)
        {
            m_presentAcquiredFailed = false;

            m_presentResult.outcome = m_pSwapchain->GetAcquireResult().outcome;

            m_presentResult.error   = m_pSwapchain->GetAcquireResult().error;
        }
        else if (m_pSwapchain->GetAcquiredImageIndex() >= 0)
        {
            // Only a new accepted graphics-queue signal authorizes this copy.
            VulkanSemaphore* semaphore = m_pSwapchain->GetRenderingCompleteSemaphore(m_pSwapchain->GetAcquiredImageIndex());

            const uint64_t serial = semaphore->GetSignalSubmissionSerial(m_pDevice->GetGfxQueue(), m_presentSignalGeneration);

            if (serial != 0)
            {
                m_pSwapchain->MarkAcquireSemaphoreSubmitted(serial);

                m_pSwapchain->Present(semaphore);

                m_presentResult           = m_pSwapchain->GetPresentResult();

                m_presentSignalGeneration = 0;
            }
        }
    }

    return m_presentResult;
}

RHIStatus VulkanViewport::PrepareForPresent(RHICommandList* commands)
{
    RecordPresentCopy(commands);

    RHIError error = m_pDevice->GetRHI()->GetTerminalError();

    if (!error.IsFailure() && m_pSwapchain != nullptr)
    {
        error = m_pSwapchain->GetAcquireResult().error;
    }

    return {error};
}

bool VulkanViewport::NeedsRecreation() const
{
    // The swapchain belongs to the RHI thread; other threads wait for the answer there.
    return m_pDevice->GetRHI()->GetRuntime().thread.Invoke(
        [this] { return m_suspended || (m_pSwapchain != nullptr && m_pSwapchain->NeedsRecreation()); });
}

bool VulkanViewport::CanRecreate() const
{
    // The surface query runs on the RHI thread; other threads wait for the answer there.
    return m_pDevice->GetRHI()->GetRuntime().thread.Invoke([this] {
        return m_pSwapchain == nullptr || m_pSwapchain->GetNumSwapchainImages() != 0 || m_pSwapchain->HasPresentableExtent();
    });
}

void VulkanViewport::Resize(uint32_t width, uint32_t height)
{
    if (m_pWindow.provider && m_pWindow.provider->checkOwner)
    {
        m_pWindow.provider->checkOwner(m_pWindow.window);
    }

    VulkanSwapchainRecreateInfo recreateInfo;

    bool recreate =
        m_pDevice->GetRHI()->GetRuntime().thread.Invoke(&VulkanViewport::BeginResize, this, width, height, &recreateInfo);

    // Surface loss during native reconstruction must return to the window owner. Retry once
    // immediately; repeated loss remains suspended and pending for the next frame.
    for (uint32_t attempt = 0; recreate && attempt < 2; ++attempt)
    {
        RHIError surfaceError;
        if (recreateInfo.surface.Get() == VK_NULL_HANDLE)
        {
            RHIResult<VkSurfaceKHR> created = CreateWindowSurface(m_pDevice->GetRHI()->GetInstance(), m_pWindow);
            if (created)
            {
                recreateInfo.surface = VulkanSurface(m_pDevice->GetRHI()->GetInstance(), created.GetValue());
            }
            else
            {
                surfaceError = created.GetError();
            }
        }

        recreate =
            m_pDevice->GetRHI()->GetRuntime().thread.Invoke(&VulkanViewport::FinishResize, this, &recreateInfo, surfaceError);
    }
}

RHITexture* VulkanViewport::GetColorBackBuffer()
{
    return m_pColorBackBuffer;
}

RHITexture* VulkanViewport::GetDepthStencilBackBuffer()
{
    return m_pDepthStencilBackBuffer;
}
} // namespace zen
