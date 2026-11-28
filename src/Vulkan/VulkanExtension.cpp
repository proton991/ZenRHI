#include "ZenRHI/RHIDiagnostics.h"
#include "ZenFoundation/Utils/UniquePtr.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanExtension.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/Platform/VulkanWindowBridge.h"

namespace zen
{
static int FindExtensionIndex(NameID extensionName, const HeapVector<VkExtensionProperties>& supported)
{
    int index = -1;

    for (size_t i = 0; i < supported.size(); ++i)
    {
        if (strcmp(extensionName.CStr(), supported[i].extensionName) == 0)
        {
            index = static_cast<int>(i);

            break;
        }
    }

    if (index == -1)
    {
        ZEN_RHI_LOG_WARN("Instance extension {} not supported!", extensionName.CStr());
    }

    return index;
}

template <class ExtensionType>
static void FlagExtensionSupported(HeapVector<ExtensionType>& extensions, const HeapVector<VkExtensionProperties>& supported)
{
    for (ExtensionType& extension : extensions)
    {
        int index = FindExtensionIndex(extension->GetName(), supported);

        if (index != -1)
        {
            extension->SetSupport();
        }
    }
}

static bool ExtensionNameLess(const VkExtensionProperties& a, const VkExtensionProperties& b)
{
    return strcmp(a.extensionName, b.extensionName) < 0;
}

HeapVector<VkExtensionProperties> VulkanInstanceExtension::GetSupportedInstanceExtensions(NameID layerName, RHIError* error)
{
    HeapVector<VkExtensionProperties> extensions;

    const char* pLayerName = layerName.IsNone() ? nullptr : layerName.CStr();

    const VkResult result  = EnumerateVulkanProperties(extensions, vkEnumerateInstanceExtensionProperties, pLayerName);
    if (result != VK_SUCCESS && error)
    {
        *error = MakeVulkanError(result, "vkEnumerateInstanceExtensionProperties");
    }

    std::sort(extensions.begin(), extensions.end(), ExtensionNameLess);

    return extensions;
}

static bool HasInstanceExtension(const VulkanInstanceExtensionArray& extensions, NameID name)
{
    bool supported = false;

    for (const UniquePtr<VulkanInstanceExtension>& extension : extensions)
    {
        if (extension->GetName() == name)
        {
            supported = extension->IsSupported();

            break;
        }
    }

    return supported;
}

VulkanInstanceExtensionArray VulkanInstanceExtension::GetEnabledInstanceExtensions(
    InstanceExtensionFlags&          extensionFlags,
    RHIDeviceDesc::SurfaceExtensions surfaceExtensions,
    RHIError*                        error)
{
    VulkanInstanceExtensionArray enabledExtensions;

#define ADD_INSTANCE_EXTENSION(EXTENSION_NAME) \
    enabledExtensions.emplace_back(MakeUnique<VulkanInstanceExtension>(EXTENSION_NAME));

    enabledExtensions.emplace_back(MakeUnique<VulkanInstanceExtension>(VK_KHR_SURFACE_EXTENSION_NAME, true));

    ADD_INSTANCE_EXTENSION(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    ADD_INSTANCE_EXTENSION(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    ADD_INSTANCE_EXTENSION(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);

    ADD_INSTANCE_EXTENSION(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

    // Do not opt into newer extensions by name when building against an older SDK.
    // The matching validation layer only recognizes the EXT maintenance structs.
#if defined(VK_KHR_surface_maintenance1)
    ADD_INSTANCE_EXTENSION(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#endif
    ADD_INSTANCE_EXTENSION(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

    AddWindowInstanceExtensions(enabledExtensions, surfaceExtensions);

    FlagExtensionSupported(enabledExtensions, VulkanInstanceExtension::GetSupportedInstanceExtensions({}, error));

    const bool hasSurfaceCapabilities2 =
        HasInstanceExtension(enabledExtensions, NameID(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));

    for (const UniquePtr<VulkanInstanceExtension>& extension : enabledExtensions)
    {
        const NameID name = extension->GetName();
#if defined(VK_KHR_surface_maintenance1)
        if (name == NameID(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME))
        {
            extension->SetSupport(extension->IsSupported() && hasSurfaceCapabilities2);

            extensionFlags.hasSurfaceMaintenanceKHR = extension->IsSupported();
        }
#endif
        if (name == NameID(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME))
        {
            extension->SetSupport(extension->IsSupported() && hasSurfaceCapabilities2);

            extensionFlags.hasSurfaceMaintenanceEXT = extension->IsSupported();
        }

        if (name == NameID(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            extensionFlags.hasDebugUtils = extension->IsSupported();
        }
    }

#undef ADD_INSTANCE_EXTENSION

    return enabledExtensions;
}

HeapVector<VkExtensionProperties> GetSupportedDeviceExtensions(VkPhysicalDevice gpu, RHIError* error)
{
    HeapVector<VkExtensionProperties> extensions;

    const VkResult result = EnumerateVulkanProperties(extensions, vkEnumerateDeviceExtensionProperties, gpu, nullptr);
    if (result != VK_SUCCESS && error)
    {
        *error = MakeVulkanError(result, "vkEnumerateDeviceExtensionProperties");
    }

    std::sort(extensions.begin(), extensions.end(), ExtensionNameLess);

    return extensions;
}

namespace
{
using Extension = VulkanDeviceExtensionName;

// Names in VulkanDeviceExtensionName order; an SDK without a name leaves its entry empty.
constexpr const char* kDeviceExtensionNames[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
    VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
    VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
    VK_KHR_OPACITY_MICROMAP_EXTENSION_NAME,
    VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
#if defined(VK_KHR_calibrated_timestamps)
    VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
#else
    "",
#endif
#if defined(VK_EXT_calibrated_timestamps)
    VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
#else
    "",
#endif
#if defined(VK_KHR_swapchain_maintenance1)
    VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
#else
    "",
#endif
    VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
};
static_assert(std::size(kDeviceExtensionNames) == ToUnderlying(Extension::eMax));

bool IsAdvertised(const HeapVector<VkExtensionProperties>& supported, const char* name)
{
    bool advertised = false;

    for (uint32_t i = 0; !advertised && name[0] != '\0' && i < supported.size(); ++i)
    {
        advertised = strcmp(name, supported[i].extensionName) == 0;
    }

    return advertised;
}

// The ray-tracing invocation reorder hint; reordering counts only when the device reorders.
template <typename Properties> bool ReordersInvocations(VkPhysicalDevice gpu, Properties properties, int32_t reorderMode)
{
    VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

    query.pNext = &properties;

    vkGetPhysicalDeviceProperties2(gpu, &query);

    return static_cast<int32_t>(properties.rayTracingInvocationReorderReorderingHint) == reorderMode;
}
} // namespace

void* VulkanDeviceFeatures::Link(const VulkanDeviceExtensionSet& extensions)
{
    struct Entry
    {
        Extension extension;
        void*     features;
    };

    // The micromap's companion structs follow its own enablement, not their names'.
    const Entry entries[]    = {{Extension::eDynamicRendering, &dynamicRendering},
                                {Extension::eAccelerationStructure, &accelerationStructure},
                                {Extension::eRayTracingPipeline, &rayTracingPipeline},
                                {Extension::eInvocationReorderEXT, &invocationReorderEXT},
                                {Extension::eInvocationReorderNV, &invocationReorderNV},
                                {Extension::eRayQuery, &rayQuery},
                                {Extension::eDeviceFault, &fault},
                                {Extension::ePipelineExecutable, &pipelineExecutable},
                                {Extension::eOpacityMicromap, &micromap},
                                {Extension::eOpacityMicromap, &addressCommands},
                                {Extension::eOpacityMicromap, &synchronization2},
                                {Extension::eOpacityMicromap, &extendedDynamicState},
                                {Extension::eSwapchainMaintenanceKHR, &swapchainMaintenance},
                                {Extension::eSwapchainMaintenanceEXT, &swapchainMaintenance}};

    core.pNext               = &vulkan12;

    VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&vulkan12);

    for (const Entry& entry : entries)
    {
        if (extensions.test(ToUnderlying(entry.extension)))
        {
            tail->pNext = static_cast<VkBaseOutStructure*>(entry.features);

            tail        = tail->pNext;
        }
    }

    tail->pNext = nullptr;

    return &vulkan12;
}

bool VulkanDeviceExtensionSelection::IsEnabled(const char* name) const
{
    bool found = false;

    for (uint32_t i = 0; !found && i < names.size(); ++i)
    {
        found = strcmp(names[i].CStr(), name) == 0;
    }

    return found;
}

VulkanDeviceExtensionSelection SelectDeviceExtensions(VulkanDevice& device, RHIError* error)
{
    const VkPhysicalDevice                  gpu      = device.GetPhysicalDeviceHandle();
    const RHIDeviceDesc&                    desc     = device.GetRHI()->GetDeviceDesc();
    const InstanceExtensionFlags&           instance = device.GetRHI()->GetInstanceExtensionFlags();
    const HeapVector<VkExtensionProperties> names    = GetSupportedDeviceExtensions(gpu, error);

    VulkanDeviceExtensionSet advertised;

    for (uint32_t i = 0; i < ToUnderlying(Extension::eMax); ++i)
    {
        advertised.set(i, IsAdvertised(names, kDeviceExtensionNames[i]));
    }

    // Requests that depend on the description, and alternatives where only one name is used.
    const bool micromap =
        advertised[ToUnderlying(Extension::eOpacityMicromap)] && advertised[ToUnderlying(Extension::eDeviceAddressCommands)]
        && advertised[ToUnderlying(Extension::eSynchronization2)] && advertised[ToUnderlying(Extension::eExtendedDynamicState)];
    for (Extension member : {Extension::eOpacityMicromap, Extension::eDeviceAddressCommands, Extension::eSynchronization2,
                             Extension::eExtendedDynamicState})
    {
        advertised.set(ToUnderlying(member), micromap);
    }
    advertised.set(ToUnderlying(Extension::eBufferMarker),
                   advertised[ToUnderlying(Extension::eBufferMarker)] && desc.DeviceLossDiagnostics());
    advertised.set(ToUnderlying(Extension::ePipelineExecutable),
                   advertised[ToUnderlying(Extension::ePipelineExecutable)] && desc.PipelineStatistics());
    advertised.set(ToUnderlying(Extension::eCalibratedTimestampsEXT),
                   advertised[ToUnderlying(Extension::eCalibratedTimestampsEXT)]
                       && !advertised[ToUnderlying(Extension::eCalibratedTimestampsKHR)]);
    advertised.set(ToUnderlying(Extension::eSwapchainMaintenanceKHR),
                   advertised[ToUnderlying(Extension::eSwapchainMaintenanceKHR)] && instance.hasSurfaceMaintenanceKHR);
    advertised.set(ToUnderlying(Extension::eSwapchainMaintenanceEXT),
                   advertised[ToUnderlying(Extension::eSwapchainMaintenanceEXT)] && instance.hasSurfaceMaintenanceEXT
                       && !advertised[ToUnderlying(Extension::eSwapchainMaintenanceKHR)]);

    VulkanDeviceExtensionSelection selection;
    VulkanDeviceFeatures&          supported = selection.supported;

    supported.Link(advertised);

    vkGetPhysicalDeviceFeatures2(gpu, &supported.core);

    const VkPhysicalDeviceVulkan12Features& v12 = supported.vulkan12;
    DeviceExtensionFlags&                   f   = selection.flags;

    f.hasDeferredHostOperation                  = advertised[ToUnderlying(Extension::eDeferredHostOperations)];
    f.hasCalibratedTimestamps                   = advertised[ToUnderlying(Extension::eCalibratedTimestampsKHR)]
                                               || advertised[ToUnderlying(Extension::eCalibratedTimestampsEXT)];
    // Promoted to Vulkan 1.2. With the 1.2 struct chained, enabling the old name would also require the bit.
    f.hasSamplerMirrorClampToEdge = v12.samplerMirrorClampToEdge == VK_TRUE;
    f.hasMemoryBudget             = advertised[ToUnderlying(Extension::eMemoryBudget)];
    f.hasBufferMarker             = advertised[ToUnderlying(Extension::eBufferMarker)];
    f.hasDescriptorIndexing       = MissingDescriptorIndexingFeature(v12) == nullptr;
    f.hasHostQueryReset           = v12.hostQueryReset == VK_TRUE;
    f.hasBufferDeviceAddress      = v12.bufferDeviceAddress == VK_TRUE;
    f.hasBufferInt64Atomics       = v12.shaderBufferInt64Atomics == VK_TRUE && supported.core.features.shaderInt64 == VK_TRUE;
    f.hasPipelineExecutableInfo =
        advertised[ToUnderlying(Extension::ePipelineExecutable)] && supported.pipelineExecutable.pipelineExecutableInfo;
    f.hasDeviceFault           = advertised[ToUnderlying(Extension::eDeviceFault)] && supported.fault.deviceFault;
    f.hasSwapchainMaintenance1 = (advertised[ToUnderlying(Extension::eSwapchainMaintenanceKHR)]
                                  || advertised[ToUnderlying(Extension::eSwapchainMaintenanceEXT)])
                              && supported.swapchainMaintenance.swapchainMaintenance1;
    f.hasAccelerationStructure = advertised[ToUnderlying(Extension::eAccelerationStructure)]
                              && supported.accelerationStructure.accelerationStructure && !desc.RayTracingDisabled()
                              && f.hasBufferDeviceAddress && f.hasDeferredHostOperation;
    f.hasRaytracingPipeline    = advertised[ToUnderlying(Extension::eRayTracingPipeline)]
                              && supported.rayTracingPipeline.rayTracingPipeline && f.hasAccelerationStructure;
    f.hasTraceRaysIndirect     = f.hasRaytracingPipeline && supported.rayTracingPipeline.rayTracingPipelineTraceRaysIndirect;
    f.hasInvocationReorderEXT =
        advertised[ToUnderlying(Extension::eInvocationReorderEXT)] && supported.invocationReorderEXT.rayTracingInvocationReorder
        && f.hasRaytracingPipeline
        && ReordersInvocations(gpu,
                               VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT{
                                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_EXT},
                               VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_EXT);
    f.hasInvocationReorderNV =
        advertised[ToUnderlying(Extension::eInvocationReorderNV)] && supported.invocationReorderNV.rayTracingInvocationReorder
        && f.hasRaytracingPipeline && !f.hasInvocationReorderEXT
        && ReordersInvocations(gpu,
                               VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV{
                                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_NV},
                               VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV);
    f.hasRayQuery = advertised[ToUnderlying(Extension::eRayQuery)] && supported.rayQuery.rayQuery && f.hasAccelerationStructure;
    f.hasOpacityMicromap = micromap && supported.micromap.micromap && supported.addressCommands.deviceAddressCommands
                        && supported.synchronization2.synchronization2 && supported.extendedDynamicState.extendedDynamicState
                        && f.hasRayQuery;

    // Name-only extensions enable when advertised; the others only with their capability.
    VulkanDeviceExtensionSet& enabled = selection.enabled;

    enabled                           = advertised;
    enabled.set(ToUnderlying(Extension::eDynamicRendering),
                advertised[ToUnderlying(Extension::eDynamicRendering)] && supported.dynamicRendering.dynamicRendering);
    enabled.set(ToUnderlying(Extension::eAccelerationStructure), f.hasAccelerationStructure);
    enabled.set(ToUnderlying(Extension::eRayTracingPipeline), f.hasRaytracingPipeline);
    enabled.set(ToUnderlying(Extension::eInvocationReorderEXT), f.hasInvocationReorderEXT);
    enabled.set(ToUnderlying(Extension::eInvocationReorderNV), f.hasInvocationReorderNV);
    enabled.set(ToUnderlying(Extension::eRayQuery), f.hasRayQuery);
    enabled.set(ToUnderlying(Extension::eOpacityMicromap), f.hasOpacityMicromap);
    enabled.set(ToUnderlying(Extension::eDeviceFault), f.hasDeviceFault);
    enabled.set(ToUnderlying(Extension::ePipelineExecutable), f.hasPipelineExecutableInfo);
    enabled.set(ToUnderlying(Extension::eSwapchainMaintenanceKHR),
                advertised[ToUnderlying(Extension::eSwapchainMaintenanceKHR)] && f.hasSwapchainMaintenance1);
    enabled.set(ToUnderlying(Extension::eSwapchainMaintenanceEXT),
                advertised[ToUnderlying(Extension::eSwapchainMaintenanceEXT)] && f.hasSwapchainMaintenance1);

    for (uint32_t i = 0; i < ToUnderlying(Extension::eMax); ++i)
    {
        if (enabled[i])
        {
            selection.names.push_back(NameID(kDeviceExtensionNames[i]));
        }
    }

    // Only the features ZenRHI uses are enabled. Link sets every pNext again before use.
    VulkanDeviceFeatures& on                                  = selection.enabledFeatures;

    on.vulkan12.shaderSampledImageArrayNonUniformIndexing     = f.hasDescriptorIndexing;
    on.vulkan12.runtimeDescriptorArray                        = f.hasDescriptorIndexing;
    on.vulkan12.descriptorBindingPartiallyBound               = f.hasDescriptorIndexing;
    on.vulkan12.descriptorBindingUpdateUnusedWhilePending     = f.hasDescriptorIndexing;
    on.vulkan12.descriptorBindingSampledImageUpdateAfterBind  = f.hasDescriptorIndexing;
    on.vulkan12.hostQueryReset                                = v12.hostQueryReset;
    on.vulkan12.samplerMirrorClampToEdge                      = v12.samplerMirrorClampToEdge;
    on.vulkan12.timelineSemaphore                             = v12.timelineSemaphore;
    on.vulkan12.bufferDeviceAddress                           = v12.bufferDeviceAddress;
    on.vulkan12.shaderBufferInt64Atomics                      = f.hasBufferInt64Atomics;
    on.dynamicRendering                                       = supported.dynamicRendering;
    on.accelerationStructure.accelerationStructure            = VK_TRUE;
    on.rayTracingPipeline.rayTracingPipeline                  = VK_TRUE;
    on.rayTracingPipeline.rayTracingPipelineTraceRaysIndirect = f.hasTraceRaysIndirect;
    on.invocationReorderEXT                                   = supported.invocationReorderEXT;
    on.invocationReorderNV                                    = supported.invocationReorderNV;
    on.rayQuery                                               = supported.rayQuery;
    on.fault                                                  = supported.fault;
    on.fault.deviceFaultVendorBinary                          = VK_FALSE;
    on.pipelineExecutable                                     = supported.pipelineExecutable;
    on.micromap                                               = supported.micromap;
    on.addressCommands                                        = supported.addressCommands;
    on.synchronization2                                       = supported.synchronization2;
    on.extendedDynamicState                                   = supported.extendedDynamicState;
    on.swapchainMaintenance                                   = supported.swapchainMaintenance;

    return selection;
}

} // namespace zen
