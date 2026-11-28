#pragma once
#include "Vulkan/VulkanHeaders.h"
#include "ZenFoundation/Utils/UniquePtr.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Templates/NameID.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include <bitset>

namespace zen
{
struct RHIError;
// The first descriptor indexing feature the global heap needs and the device lacks, or null. Takes either the
// descriptor indexing or the Vulkan 1.2 features struct; both name the members alike.
template <typename Features> const char* MissingDescriptorIndexingFeature(const Features& features)
{
    const char* missing = nullptr;
    if (!features.runtimeDescriptorArray)
    {
        missing = "runtimeDescriptorArray";
    }
    else if (!features.descriptorBindingPartiallyBound)
    {
        missing = "descriptorBindingPartiallyBound";
    }
    else if (!features.descriptorBindingUpdateUnusedWhilePending)
    {
        missing = "descriptorBindingUpdateUnusedWhilePending";
    }
    else if (!features.descriptorBindingSampledImageUpdateAfterBind)
    {
        missing = "descriptorBindingSampledImageUpdateAfterBind";
    }
    else if (!features.shaderSampledImageArrayNonUniformIndexing)
    {
        missing = "shaderSampledImageArrayNonUniformIndexing";
    }
    return missing;
}

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

struct InstanceExtensionFlags
{
    uint32_t hasDebugUtils            : 1;
    uint32_t hasValidationFeatures    : 1;
    uint32_t hasSurfaceMaintenanceKHR : 1;
    uint32_t hasSurfaceMaintenanceEXT : 1;
};

class VulkanExtension
{
public:
    explicit VulkanExtension(NameID extensionName) : m_extensionName(extensionName) {}

    NameID GetName() const
    {
        return m_extensionName;
    }

    void SetSupport(bool supported = true)
    {
        m_supported = supported;
    }

    bool IsSupported() const
    {
        return m_supported;
    }

private:
    NameID m_extensionName;
    // supported by driver
    bool m_supported{false};
};
class VulkanInstanceExtension;
using VulkanInstanceExtensionArray = HeapVector<UniquePtr<VulkanInstanceExtension>>;

class VulkanInstanceExtension : public VulkanExtension
{
public:
    explicit VulkanInstanceExtension(NameID extensionName, bool required = false) :
        VulkanExtension(extensionName), required(required)
    {}

    bool required;

    static HeapVector<VkExtensionProperties> GetSupportedInstanceExtensions(NameID layerName = {}, RHIError* error = nullptr);

    // surfaceExtensions names the host window system's instance extensions; null adds none.
    static VulkanInstanceExtensionArray GetEnabledInstanceExtensions(InstanceExtensionFlags&          extensionFlags,
                                                                     RHIDeviceDesc::SurfaceExtensions surfaceExtensions,
                                                                     RHIError*                        error = nullptr);
};

struct DeviceExtensionFlags
{
    uint32_t hasBufferDeviceAddress      : 1;
    uint32_t hasAccelerationStructure    : 1;
    uint32_t hasRaytracingPipeline       : 1;
    uint32_t hasTraceRaysIndirect        : 1;
    uint32_t hasInvocationReorderEXT     : 1;
    uint32_t hasInvocationReorderNV      : 1;
    uint32_t hasRayQuery                 : 1;
    uint32_t hasOpacityMicromap          : 1;
    uint32_t hasDescriptorIndexing       : 1;
    uint32_t hasHostQueryReset           : 1;

    uint32_t hasDeferredHostOperation    : 1;
    uint32_t hasSwapchainMaintenance1    : 1;
    uint32_t hasCalibratedTimestamps     : 1;
    uint32_t hasMemoryBudget             : 1;
    uint32_t hasBufferInt64Atomics       : 1;
    uint32_t hasSamplerMirrorClampToEdge : 1;
    uint32_t hasDeviceFault              : 1;
    uint32_t hasBufferMarker             : 1;
    uint32_t hasPipelineExecutableInfo   : 1;
};

// The device extensions ZenRHI asks for. Promoted Vulkan 1.2 features come from the 1.2 features struct instead.
enum class VulkanDeviceExtensionName : uint32_t
{
    eSwapchain,
    eMemoryBudget,
    eBufferMarker,
    eDeferredHostOperations,
    eShaderNonSemanticInfo,
    eDynamicRendering,
    eAccelerationStructure,
    eRayTracingPipeline,
    eInvocationReorderEXT,
    eInvocationReorderNV,
    eRayQuery,
    eDeviceFault,
    ePipelineExecutable,
    eOpacityMicromap,
    eDeviceAddressCommands,
    eSynchronization2,
    eExtendedDynamicState,
    eCalibratedTimestampsKHR,
    eCalibratedTimestampsEXT,
    eSwapchainMaintenanceKHR,
    eSwapchainMaintenanceEXT,
    eMax
};

using VulkanDeviceExtensionSet = std::bitset<static_cast<size_t>(VulkanDeviceExtensionName::eMax)>;

// Every features struct the device queries and enables. Link chains the ones a set of extensions brings.
struct VulkanDeviceFeatures
{
    VkPhysicalDeviceFeatures2                   core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features            vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT invocationReorderEXT{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV invocationReorderNV{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV};
    VkPhysicalDeviceRayQueryFeaturesKHR                     rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceFaultFeaturesEXT                        fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipelineExecutable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceOpacityMicromapFeaturesKHR       micromap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_KHR};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR addressCommands{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicState{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchainMaintenance{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};

    // Chains core, then the 1.2 struct, then each struct an extension of the set brings. Returns the 1.2 struct,
    // where a device create chain starts.
    void* Link(const VulkanDeviceExtensionSet& extensions);
};

// What a device can enable: extension names, the features to enable with them, and the resulting capabilities.
struct VulkanDeviceExtensionSelection
{
    VulkanDeviceExtensionSet enabled;
    HeapVector<NameID>       names;
    VulkanDeviceFeatures     supported;
    VulkanDeviceFeatures     enabledFeatures;
    DeviceExtensionFlags     flags{};

    bool IsEnabled(const char* name) const;
};

HeapVector<VkExtensionProperties> GetSupportedDeviceExtensions(VkPhysicalDevice gpu, RHIError* error = nullptr);

// Reads the advertised names and the reported features once. Optional ray tracing keeps its dependency chain:
// acceleration structures need buffer device addresses and deferred host operations; pipelines and queries need
// acceleration structures.
VulkanDeviceExtensionSelection SelectDeviceExtensions(VulkanDevice& device, RHIError* error = nullptr);
} // namespace zen
