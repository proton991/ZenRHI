#pragma once
#include "ZenFoundation/Templates/HeapVector.h"
#include "Vulkan/VulkanHeaders.h"
#include "ZenRHI/RHIError.h"
#include <utility>

namespace zen
{
class VulkanRHI;
class VulkanDevice;
class VulkanSemaphore;

// Owns a window surface and destroys it with its instance. It moves between the viewport, the recreation info and
// the swapchain; a moved-from surface is empty.
class VulkanSurface
{
public:
    VulkanSurface() = default;

    VulkanSurface(VkInstance instance, VkSurfaceKHR surface) : m_instance(instance), m_surface(surface) {}

    VulkanSurface(VulkanSurface&& other) noexcept :
        m_instance(other.m_instance), m_surface(std::exchange(other.m_surface, VK_NULL_HANDLE))
    {}

    VulkanSurface& operator=(VulkanSurface&& other) noexcept
    {
        if (this != &other)
        {
            Reset();

            m_instance = other.m_instance;

            m_surface  = std::exchange(other.m_surface, VK_NULL_HANDLE);
        }

        return *this;
    }

    VulkanSurface(const VulkanSurface&)            = delete;

    VulkanSurface& operator=(const VulkanSurface&) = delete;

    ~VulkanSurface()
    {
        Reset();
    }

    VkSurfaceKHR Get() const
    {
        return m_surface;
    }

    void Reset()
    {
        if (m_surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);

            m_surface = VK_NULL_HANDLE;
        }
    }

private:
    VkInstance   m_instance{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
};

struct VulkanRetiredSwapchain
{
    VkSwapchainKHR               swapchain{VK_NULL_HANDLE};
    HeapVector<VulkanSemaphore*> presentationSemaphores;
};

struct VulkanSwapchainRecreateInfo
{
    VkSwapchainKHR                     swapchain{VK_NULL_HANDLE};
    VulkanSurface                      surface;
    HeapVector<VulkanRetiredSwapchain> retiredSwapchains;
};

struct VulkanSwapchainConfig
{
    uint32_t                      imageCount{0};
    VkSurfaceFormatKHR            format{};
    VkPresentModeKHR              presentMode{VK_PRESENT_MODE_FIFO_KHR};
    VkSurfaceTransformFlagBitsKHR transform{VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR};
    VkImageUsageFlags             usage{0};
    VkCompositeAlphaFlagBitsKHR   compositeAlpha{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR};
};

class VulkanSwapchain
{
public:
    // Consumes an already-created surface (and any predecessor) from recreateInfo.
    // Window/view APIs must not be called while creating GPU swapchain resources.
    VulkanSwapchain(VulkanDevice*                pDevice,
                    uint32_t                     width,
                    uint32_t                     height,
                    bool                         enableVSync,
                    VulkanSwapchainRecreateInfo* pRecreateInfo);

    VkSwapchainKHR GetVkHandle() const
    {
        return m_swapchain;
    }

    VkFormat GetFormat() const
    {
        return m_format;
    }

    // Rendering already encodes gamma. Copy the UNORM bytes to an sRGB surface unchanged.
    VkFormat GetBackBufferFormat() const
    {
        VkFormat format = m_format;

        if (format == VK_FORMAT_B8G8R8A8_SRGB)
        {
            format = VK_FORMAT_B8G8R8A8_UNORM;
        }
        else if (format == VK_FORMAT_R8G8B8A8_SRGB)
        {
            format = VK_FORMAT_R8G8B8A8_UNORM;
        }

        return format;
    }

    const uint32_t& GetNumSwapchainImages() const
    {
        return m_numImages;
    }

    const VkImage* GetSwapchainImages() const
    {
        return m_swapchainImages.data();
    }

    VkExtent2D GetExtent() const
    {
        return {m_internalWidth, m_internalHeight};
    }

    VkResult GetLastResult() const
    {
        return m_lastResult;
    }

    bool NeedsRecreation() const
    {
        return m_recreationRequested.load(std::memory_order_acquire);
    }

    // A failed query also reports true, so recreation surfaces the native error.
    bool HasPresentableExtent() const;

    int32_t AcquireNextImage(VulkanSemaphore** pOutSemaphore);

    RHIAcquireResult AcquireNextImageChecked(VulkanSemaphore** pOutSemaphore);

    RHIStatus GetStatus() const
    {
        return {m_error};
    }

    const RHIAcquireResult& GetAcquireResult() const
    {
        return m_acquireResult;
    }

    const RHIPresentResult& GetPresentResult() const
    {
        return m_presentResult;
    }

    bool Present(VulkanSemaphore* pRenderingCompleteSemaphore);

    void MarkAcquireSemaphoreSubmitted(uint64_t submissionSerial);

    // The image acquired and not yet presented, or -1. Present always releases it.
    int32_t GetAcquiredImageIndex() const
    {
        return m_imageIndex;
    }

    // The semaphore the acquired image's first use waits on.
    VulkanSemaphore* GetAcquireSemaphore() const
    {
        return m_imageIndex >= 0 ? m_acquireSync[m_semaphoreIndex].semaphore : nullptr;
    }

    VulkanSemaphore* GetRenderingCompleteSemaphore(uint32_t imageIndex) const
    {
        return m_presentSync[imageIndex].semaphore;
    }

    void Destroy(VulkanSwapchainRecreateInfo* pRecreateInfo);

private:
    struct AcquireSync
    {
        VulkanSemaphore* semaphore{nullptr};
        VkFence          fence{VK_NULL_HANDLE};
        uint64_t         submissionSerial{0};
        uint64_t         previousPresentSerial{0};
        uint32_t         imageIndex{0};
        bool             pending{false};
    };
    struct PresentSync
    {
        VulkanSemaphore* semaphore{nullptr};
        VkFence          fence{VK_NULL_HANDLE};
        uint64_t         serial{0};
        bool             pending{false};
    };

    void DestroyOldSwapchain(VkSwapchainKHR& oldSwapchain);

    // After an unproven completion wait: device loss makes destruction legal, any other failure is fatal.
    void WaitDeviceIdleForTeardown(const char* purpose) const;

    // Waits until no acquisition or presentation still uses the images.
    void WaitForImageWork(bool terminal);

    // Destroys the image semaphores and fences. With keepPendingPresents, semaphores of presentations still in
    // flight move into the returned record, which the successor swapchain retires.
    VulkanRetiredSwapchain DestroyImageSync(bool keepPendingPresents);

    // Creates the acquire and present semaphores and fences for every image; a failure lands in m_error.
    void CreateImageSync();

    void CompleteAcquire(AcquireSync& sync, bool wait);

    void ReleaseRetiredSwapchains();

    void WaitForPresent(PresentSync& sync);

    VulkanDevice*    m_pDevice{nullptr};
    VkSwapchainKHR   m_swapchain{VK_NULL_HANDLE};
    VulkanSurface    m_surface;
    uint32_t         m_internalWidth{0};
    uint32_t         m_internalHeight{0};
    VkFormat         m_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR  m_colorSpace{VK_COLORSPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_IMMEDIATE_KHR};
    uint32_t         m_numImages{0};

    HeapVector<VkImage>                m_swapchainImages;
    int32_t                            m_imageIndex{-1};
    int32_t                            m_semaphoreIndex{-1};
    HeapVector<AcquireSync>            m_acquireSync;
    HeapVector<PresentSync>            m_presentSync;
    HeapVector<VulkanRetiredSwapchain> m_retiredSwapchains;
    bool                               m_hasPresentFences{false};
    std::atomic<bool>                  m_recreationRequested{false};
    VkResult                           m_lastResult{VK_SUCCESS};
    bool                               m_acquiredSuboptimal{false};
    RHIError                           m_error{};
    RHIAcquireResult                   m_acquireResult{};
    RHIPresentResult                   m_presentResult{};
};
} // namespace zen
