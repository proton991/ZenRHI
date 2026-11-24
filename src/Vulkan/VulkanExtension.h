#pragma once
#include "Vulkan/VulkanHeaders.h"
#include "ZenFoundation/Utils/UniquePtr.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Templates/NameID.h"

namespace zen
{
struct RHIError;
const char* MissingDescriptorIndexingFeature(const VkPhysicalDeviceDescriptorIndexingFeatures& features);

// The loader may change the list between the count and fill calls.
template <typename T, typename Query, typename... Args>
VkResult EnumerateVulkanProperties(HeapVector<T>& items, Query query, Args... args)
{
    VkResult result;
    uint32_t count;
    do
    {
        count  = 0;
        result = query(args..., &count, nullptr);
        if (result == VK_SUCCESS && count != 0)
        {
            items.resize(count);
            result = query(args..., &count, items.data());
        }
    } while (result == VK_INCOMPLETE);
    items.resize(result == VK_SUCCESS ? count : 0);
    return result;
}

class VulkanDevice;

enum class VulkanAPIVersion : uint32_t
{
    VULKAN_1_0 = VK_MAKE_API_VERSION(0, 1, 0, 0),
    VULKAN_1_1 = VK_MAKE_API_VERSION(0, 1, 1, 0),
    VULKAN_1_2 = VK_MAKE_API_VERSION(0, 1, 2, 0),
    VULKAN_1_3 = VK_MAKE_API_VERSION(0, 1, 3, 0),
    UNKNOWN
};

enum class EnableMode : uint32_t
{
    eAuto,
    eManual
};

struct InstanceExtensionFlags
{
    uint32_t hasGetPhysicalDeviceProperties : 1;
    uint32_t hasDebugUtils                  : 1;
    uint32_t hasValidationFeatures          : 1;
    uint32_t hasSurfaceMaintenanceKHR       : 1;
    uint32_t hasSurfaceMaintenanceEXT       : 1;
};

class VulkanExtension
{
public:
    VulkanExtension(NameID extensionName, EnableMode enableMode = EnableMode::eAuto) :
        m_extensionName(extensionName), m_supported(false), m_enabled(enableMode == EnableMode::eAuto)
    {}

    NameID GetName() const
    {
        return m_extensionName;
    }

    void SetSupport(bool supported = true)
    {
        m_supported = supported;
    }

    void SetCoreSupport()
    {
        m_supported = true;
        m_core      = true;
    }

    bool RequiresExtensionName() const
    {
        return !m_core;
    }

    void SetEnable()
    {
        m_enabled = true;
    }

    bool IsEnabledAndSupported() const
    {
        return m_enabled && m_supported;
    }

private:
    NameID m_extensionName;
    // supported by driver
    bool m_supported{false};
    // enabled in RHI initialization
    bool m_enabled{false};
    bool m_core{false};
};
class VulkanInstanceExtension;
using VulkanInstanceExtensionArray = HeapVector<UniquePtr<VulkanInstanceExtension>>;

class VulkanInstanceExtension : public VulkanExtension
{
public:
    explicit VulkanInstanceExtension(NameID extensionName, bool required = false) :
        VulkanExtension(extensionName, EnableMode::eAuto), required(required)
    {}

    bool required;

    static HeapVector<VkExtensionProperties> GetSupportedInstanceExtensions(NameID layerName = {}, RHIError* error = nullptr);

    static VulkanInstanceExtensionArray GetEnabledInstanceExtensions(InstanceExtensionFlags& extensionFlags,
                                                                     RHIError*               error = nullptr);
};

class VulkanDeviceExtension;
using VulkanDeviceExtensionArray = HeapVector<UniquePtr<VulkanDeviceExtension>>;

class VulkanDeviceExtension : public VulkanExtension
{
public:
    explicit VulkanDeviceExtension(VulkanDevice* pDevice, NameID extensionName, EnableMode enableMode = EnableMode::eAuto) :
        VulkanExtension(extensionName, enableMode), m_pDevice(pDevice)
    {}

    virtual ~VulkanDeviceExtension() = default;

    static HeapVector<VkExtensionProperties> GetSupportedExtensions(VkPhysicalDevice gpu, RHIError* error = nullptr);

    static VulkanDeviceExtensionArray GetEnabledExtensions(VulkanDevice* pDevice, RHIError* error = nullptr);

#ifdef VK_KHR_get_physical_device_properties2
    virtual void BeforePhysicalDeviceFeatures(VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr) {}

    virtual void AfterPhysicalDeviceFeatures() {}

    virtual void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) {}

#endif

protected:
    VulkanDevice* m_pDevice{nullptr};
};

} // namespace zen
