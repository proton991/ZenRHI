#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanRHI.h"
#include "ZenRHI/RHIOptions.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanDescriptorPool.h"
#include "Vulkan/VulkanExtension.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanMemory.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/Platform/VulkanMacOSPlatform.h"
#include <cstdlib>

namespace zen
{
VulkanMemoryAllocator* GVkMemAllocator = nullptr;
VulkanRHI*             GVulkanRHI      = nullptr;

namespace
{
static bool HasExtensionEnabled(const HeapVector<NameID>& extensions, NameID extensionName)
{
    return std::find(extensions.begin(), extensions.end(), extensionName) != extensions.end();
}

static const char* GetPhysicalDeviceTypeName(VkPhysicalDeviceType deviceType)
{
    const char* result{};

    switch (deviceType)
    {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: result = "Other"; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: result = "Integrated"; break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: result = "Discrete"; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: result = "Virtual"; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: result = "CPU"; break;
        default: result = "Unknown"; break;
    }

    return result;
}

static int64_t GetPhysicalDeviceTypeScore(VkPhysicalDeviceType deviceType)
{
    int64_t result{};

    switch (deviceType)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: result = 40000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: result = 30000; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: result = 20000; break;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: result = 15000; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: result = 1000; break;
        default: result = 0; break;
    }

    return result;
}

struct VulkanQueueFamilySummary
{
    bool hasGraphics{false};
    bool hasDedicatedCompute{false};
    bool hasDedicatedTransfer{false};
    bool hasAsyncTransfer{false};
};

struct VulkanPhysicalDeviceCandidateInfo
{
    bool                       isValid{false};
    std::string                unsupportedReason;
    int64_t                    score{0};
    uint64_t                   deviceLocalMemoryBytes{0};
    VkPhysicalDeviceProperties properties{};
    VulkanQueueFamilySummary   queueSummary{};
    bool                       hasDescriptorIndexing{false};
    bool                       hasDynamicRendering{false};
    bool                       hasTimelineSemaphore{false};
    bool                       hasBufferDeviceAddress{false};
    bool                       hasAccelerationStructure{false};
    bool                       hasRayTracingPipeline{false};
    bool                       hasRayQuery{false};
    bool                       hasGeometryShader{false};
    bool                       hasSamplerAnisotropy{false};
};

static VulkanQueueFamilySummary GetQueueFamilySummary(VkPhysicalDevice physicalDevice)
{
    VulkanQueueFamilySummary result{};

    VulkanQueueFamilySummary summary{};

    uint32_t queueFamilyCount = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

    if (queueFamilyCount == 0)
    {
        result = summary;
    }
    else
    {
        HeapVector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);

        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProperties.data());

        for (const VkQueueFamilyProperties& queueFamilyProperty : queueFamilyProperties)
        {
            if (queueFamilyProperty.queueCount == 0)
            {
                continue;
            }

            const bool hasGraphics = (queueFamilyProperty.queueFlags & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT;

            const bool hasCompute  = (queueFamilyProperty.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT;

            const bool hasTransfer = (queueFamilyProperty.queueFlags & VK_QUEUE_TRANSFER_BIT) == VK_QUEUE_TRANSFER_BIT;

            if (hasGraphics)
            {
                summary.hasGraphics = true;
            }

            if (hasCompute && !hasGraphics)
            {
                summary.hasDedicatedCompute = true;
            }

            if (hasTransfer && !hasGraphics)
            {
                summary.hasAsyncTransfer = true;
            }

            if (hasTransfer && !hasGraphics && !hasCompute)
            {
                summary.hasDedicatedTransfer = true;
            }
        }

        result = summary;
    }

    return result;
}

static uint64_t GetDeviceLocalMemoryBytes(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    uint64_t deviceLocalMemoryBytes = 0;

    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i)
    {
        const VkMemoryHeap& memoryHeap = memoryProperties.memoryHeaps[i];

        if ((memoryHeap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            deviceLocalMemoryBytes += memoryHeap.size;
        }
    }

    return deviceLocalMemoryBytes;
}

static VulkanPhysicalDeviceCandidateInfo EvaluatePhysicalDeviceCandidate(VkPhysicalDevice physicalDevice, RHIError& error)
{
    VulkanPhysicalDeviceCandidateInfo result{};

    static constexpr uint64_t cMiB = 1024ull * 1024ull;

    VulkanPhysicalDeviceCandidateInfo candidateInfo{};

    vkGetPhysicalDeviceProperties(physicalDevice, &candidateInfo.properties);

    candidateInfo.queueSummary      = GetQueueFamilySummary(physicalDevice);

    candidateInfo.unsupportedReason = VulkanDevice::GetUnsupportedReason(physicalDevice, &error);

    if (!candidateInfo.unsupportedReason.empty())
    {
        result = candidateInfo;
    }
    else
    {
        VkPhysicalDeviceFeatures2 physicalDeviceFeatures2{};

        InitVkStruct(physicalDeviceFeatures2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);

        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};

        InitVkStruct(descriptorIndexingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);

        VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeatures{};

        InitVkStruct(dynamicRenderingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR);

        VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphoreFeatures{};

        InitVkStruct(timelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);

        VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};

        InitVkStruct(bufferDeviceAddressFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);

        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};

        InitVkStruct(accelerationStructureFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};

        InitVkStruct(rayTracingPipelineFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);

        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};

        InitVkStruct(rayQueryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);

        physicalDeviceFeatures2.pNext       = &descriptorIndexingFeatures;

        descriptorIndexingFeatures.pNext    = &dynamicRenderingFeatures;

        dynamicRenderingFeatures.pNext      = &timelineSemaphoreFeatures;

        timelineSemaphoreFeatures.pNext     = &bufferDeviceAddressFeatures;

        bufferDeviceAddressFeatures.pNext   = &accelerationStructureFeatures;

        accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;

        rayTracingPipelineFeatures.pNext    = &rayQueryFeatures;

        rayQueryFeatures.pNext              = nullptr;

        vkGetPhysicalDeviceFeatures2(physicalDevice, &physicalDeviceFeatures2);

        candidateInfo.hasGeometryShader         = physicalDeviceFeatures2.features.geometryShader == VK_TRUE;

        candidateInfo.hasSamplerAnisotropy      = physicalDeviceFeatures2.features.samplerAnisotropy == VK_TRUE;

        candidateInfo.hasDescriptorIndexing      = MissingDescriptorIndexingFeature(descriptorIndexingFeatures) == nullptr;

        candidateInfo.hasDynamicRendering       = dynamicRenderingFeatures.dynamicRendering == VK_TRUE;

        candidateInfo.hasTimelineSemaphore      = timelineSemaphoreFeatures.timelineSemaphore == VK_TRUE;

        candidateInfo.hasBufferDeviceAddress    = bufferDeviceAddressFeatures.bufferDeviceAddress == VK_TRUE;

        candidateInfo.hasAccelerationStructure  = accelerationStructureFeatures.accelerationStructure == VK_TRUE;

        candidateInfo.hasRayTracingPipeline     = rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE;

        candidateInfo.hasRayQuery               = rayQueryFeatures.rayQuery == VK_TRUE;

        candidateInfo.deviceLocalMemoryBytes    = GetDeviceLocalMemoryBytes(physicalDevice);

        candidateInfo.isValid                   = true;

        candidateInfo.score                    += GetPhysicalDeviceTypeScore(candidateInfo.properties.deviceType);

        candidateInfo.score += static_cast<int64_t>(candidateInfo.deviceLocalMemoryBytes / (1024ull * cMiB)) * 250;

        candidateInfo.score += static_cast<int64_t>(candidateInfo.properties.limits.maxImageDimension2D / 1024);

        candidateInfo.score += static_cast<int64_t>(candidateInfo.properties.limits.maxPushConstantsSize / 32);

        if (candidateInfo.queueSummary.hasDedicatedCompute)
        {
            candidateInfo.score += 5000;
        }

        if (candidateInfo.queueSummary.hasDedicatedTransfer)
        {
            candidateInfo.score += 3000;
        }

        if (candidateInfo.queueSummary.hasAsyncTransfer)
        {
            candidateInfo.score += 1000;
        }

        if (candidateInfo.hasDescriptorIndexing)
        {
            candidateInfo.score += 4000;
        }

        if (candidateInfo.hasDynamicRendering)
        {
            candidateInfo.score += 2500;
        }

        if (candidateInfo.hasTimelineSemaphore)
        {
            candidateInfo.score += 2500;
        }

        if (candidateInfo.hasBufferDeviceAddress)
        {
            candidateInfo.score += 2000;
        }

        if (candidateInfo.hasAccelerationStructure)
        {
            candidateInfo.score += 1500;
        }

        if (candidateInfo.hasRayTracingPipeline)
        {
            candidateInfo.score += 1500;
        }

        if (candidateInfo.hasRayQuery)
        {
            candidateInfo.score += 1000;
        }

        if (candidateInfo.hasGeometryShader)
        {
            candidateInfo.score += 500;
        }

        if (candidateInfo.hasSamplerAnisotropy)
        {
            candidateInfo.score += 250;
        }

        result = candidateInfo;
    }

    return result;
}
} // namespace

namespace
{
std::atomic<uint64_t> processValidationErrors{};
}

void VulkanRHI::CountValidationMessage(RHIMessageSeverity severity, std::string_view id)
{
    std::lock_guard<std::mutex> lock(m_validationMutex);
    const auto                  entry =
        std::find_if(m_validationCounts.begin(), m_validationCounts.end(),
                     [severity, id](const ValidationMessageCount& item) { return severity == item.severity && id == item.id; });
    if (entry == m_validationCounts.end())
    {
        m_validationCounts.push_back({severity, std::string(id), 1});
    }
    else
    {
        ++entry->count;
    }
    if (severity == RHIMessageSeverity::eError)
    {
        processValidationErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

uint64_t VulkanRHI::GetValidationCount(RHIMessageSeverity severity, std::string_view id) const
{
    std::lock_guard<std::mutex> lock(m_validationMutex);
    uint64_t                    total = 0;
    for (const ValidationMessageCount& item : m_validationCounts)
    {
        if (severity == item.severity && (id.empty() || id == item.id))
        {
            total += item.count;
        }
    }
    return total;
}

namespace test
{
uint64_t GetValidationCount(RHIMessageSeverity severity, std::string_view id)
{
    return GVulkanRHI ? GVulkanRHI->GetValidationCount(severity, id) : 0;
}

uint64_t GetProcessValidationErrors()
{
    return processValidationErrors.load(std::memory_order_relaxed);
}
} // namespace test

VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
                                                           VkDebugUtilsMessageTypeFlagsEXT             messageType,
                                                           const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                           void*                                       pUserData)
{
    const RHIMessageSeverity severity =
        (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)     ? RHIMessageSeverity::eError
        : (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? RHIMessageSeverity::eWarning
                                                                              : RHIMessageSeverity::eInfo;
    if (pUserData)
    {
        static_cast<VulkanRHI*>(pUserData)->CountValidationMessage(severity,
                                                                   RHIErrorText(pCallbackData->pMessageIdName, "unknown"));
    }
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        ZEN_RHI_LOG_WARN("{} - {}: {}", pCallbackData->messageIdNumber,
                         RHIErrorText(pCallbackData->pMessageIdName, "unknown message"),
                         RHIErrorText(pCallbackData->pMessage, ""));
    }
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        ZEN_RHI_LOG_ERROR("Vulkan validation error {} - {}: {}", pCallbackData->messageIdNumber,
                          RHIErrorText(pCallbackData->pMessageIdName, "unknown message"),
                          RHIErrorText(pCallbackData->pMessage, ""))
    }
    else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) && RHIOptions::GetInstance().DebugPrintfEnabled())
    {
        ZEN_RHI_LOG_INFO("{} - {}: {}", pCallbackData->messageIdNumber,
                         RHIErrorText(pCallbackData->pMessageIdName, "unknown message"),
                         RHIErrorText(pCallbackData->pMessage, ""));
    }

    return VK_FALSE;
}

void VulkanRHI::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& dbgMessengerCI)
{
    VkDebugUtilsMessageSeverityFlagsEXT messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    constexpr VkDebugUtilsMessageTypeFlagsEXT messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                                          | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                                          | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    if (RHIOptions::GetInstance().DebugPrintfEnabled())
    {
        messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    }

    InitVkStruct(dbgMessengerCI, VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);

    dbgMessengerCI.pNext           = nullptr;

    dbgMessengerCI.flags           = 0;

    dbgMessengerCI.messageSeverity = messageSeverity;

    dbgMessengerCI.messageType     = messageType;

    dbgMessengerCI.pfnUserCallback = DebugUtilsMessengerCallback;

    dbgMessengerCI.pUserData       = this;
}

static void FlagInstanceExtensionSupported(VulkanInstanceExtensionArray& instanceExtensions, NameID extensionName)
{
    for (UniquePtr<VulkanInstanceExtension>& extension : instanceExtensions)
    {
        if (extension->GetName() == extensionName)
        {
            extension->SetSupport();
        }
    }
}

RHIError VulkanRHI::SetupInstanceLayers(VulkanInstanceExtensionArray& instanceExtensions)
{
    RHIError error;
    if (RHIOptions::GetInstance().ValidationEnabled())
    {
        HeapVector<VkLayerProperties> layers;
        const VkResult                result = EnumerateVulkanProperties(layers, vkEnumerateInstanceLayerProperties);
        error                                = MakeVulkanError(result, "vkEnumerateInstanceLayerProperties");
        const char* name                     = "VK_LAYER_KHRONOS_validation";
        for (const VkLayerProperties& layer : layers)
        {
            if (strcmp(layer.layerName, name) == 0)
            {
                m_instanceLayers.emplace_back(name);
                m_validationLayer = layer;
                HeapVector<VkExtensionProperties> extensions =
                    VulkanInstanceExtension::GetSupportedInstanceExtensions(name, &error);
                for (const VkExtensionProperties& extension : extensions)
                {
                    FlagInstanceExtensionSupported(instanceExtensions, extension.extensionName);
                }
                break;
            }
        }
        if (!error.IsFailure() && m_instanceLayers.empty())
        {
            ZEN_RHI_LOG_WARN("Requested {} layer not supported!", name);
            if (RHIOptions::GetInstance().ValidationMode() == RHIValidationMode::eRequired)
            {
                error = MakeVulkanError(VK_ERROR_LAYER_NOT_PRESENT, "VK_LAYER_KHRONOS_validation");
            }
        }
    }
    return error;
}

RHIError VulkanRHI::SetupInstanceExtensions(VulkanInstanceExtensionArray& instanceExtensions)
{
    RHIError error;
    for (UniquePtr<VulkanInstanceExtension>& extension : instanceExtensions)
    {
        if (extension->IsEnabledAndSupported())
        {
            m_instanceExtensions.emplace_back(extension->GetName());
            if (extension->GetName() == NameID(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
            {
                m_instanceExtensionFlags.hasValidationFeatures = 1;
            }

            ZEN_RHI_LOG_INFO("Enabled Instance Extension: {}", extension->GetName().CStr());
        }
        else if (extension->required)
        {
            ZEN_RHI_LOG_ERROR("Required instance extension is missing: {}", extension->GetName().CStr());
            error = MakeVulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Select Vulkan instance extensions");
            break;
        }
    }
    return error;
}

RHIError VulkanRHI::CreateInstance()
{
    VkApplicationInfo appInfo;

    InitVkStruct(appInfo, VK_STRUCTURE_TYPE_APPLICATION_INFO);

    appInfo.apiVersion         = ZEN_VK_API_VERSION;

    appInfo.applicationVersion = ZEN_VK_APP_VERSION;

    appInfo.engineVersion      = ZEN_ENGINE_VERSION;

    appInfo.pApplicationName   = "ZenEngineRHI";

    appInfo.pEngineName        = "ZenEngine";

    RHIError                     error;
    VulkanInstanceExtensionArray instanceExtensions =
        VulkanInstanceExtension::GetEnabledInstanceExtensions(m_instanceExtensionFlags, &error);

    if (!error.IsFailure())
    {
        error = SetupInstanceLayers(instanceExtensions);
    }

    if (!error.IsFailure())
    {
        error = SetupInstanceExtensions(instanceExtensions);
    }

    VkDebugUtilsMessengerCreateInfoEXT debugMessengerCI;

    PopulateDebugMessengerCreateInfo(debugMessengerCI);

    HeapVector<VkValidationFeatureEnableEXT> validationFeatureEnables = {VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT};

    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};

    validationFeatures.enabledValidationFeatureCount = RHIOptions::GetInstance().DebugPrintfEnabled() ? 1 : 0;

    validationFeatures.pEnabledValidationFeatures    = validationFeatureEnables.data();

    validationFeatures.pNext                         = m_instanceExtensionFlags.hasDebugUtils ? &debugMessengerCI : nullptr;

    HeapVector<const char*> layerNames;

    layerNames.reserve(m_instanceLayers.size());

    for (NameID layer : m_instanceLayers)
    {
        layerNames.push_back(layer.CStr());
    }

    HeapVector<const char*> extensionNames;

    extensionNames.reserve(m_instanceExtensions.size());

    for (NameID extension : m_instanceExtensions)
    {
        extensionNames.push_back(extension.CStr());
    }

    VkInstanceCreateInfo instanceInfo;

    InitVkStruct(instanceInfo, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
#if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    if (HasExtensionEnabled(m_instanceExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    instanceInfo.pApplicationInfo        = &appInfo;

    instanceInfo.enabledLayerCount       = static_cast<uint32_t>(layerNames.size());

    instanceInfo.ppEnabledLayerNames     = layerNames.empty() ? nullptr : layerNames.data();

    instanceInfo.enabledExtensionCount   = static_cast<uint32_t>(extensionNames.size());

    instanceInfo.ppEnabledExtensionNames = extensionNames.empty() ? nullptr : extensionNames.data();

    const bool validationEnabled         = HasExtensionEnabled(m_instanceLayers, NameID("VK_LAYER_KHRONOS_validation"));

    instanceInfo.pNext =
        validationEnabled && RHIOptions::GetInstance().DebugPrintfEnabled() && m_instanceExtensionFlags.hasValidationFeatures
            ? static_cast<const void*>(&validationFeatures)
            : (m_instanceExtensionFlags.hasDebugUtils ? &debugMessengerCI : nullptr);

    const bool layerSettings =
        validationEnabled && HasExtensionEnabled(m_instanceExtensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    const VkBool32               enabled    = VK_TRUE;
    const VkBool32               disabled   = VK_FALSE;
    const char*                  layer      = "VK_LAYER_KHRONOS_validation";
    const VkLayerSettingEXT      settings[] = {{layer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enabled},
                                               {layer, "enable_message_limit", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &disabled}};
    VkLayerSettingsCreateInfoEXT settingsInfo{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
    if (layerSettings)
    {
        settingsInfo.pNext        = instanceInfo.pNext;
        settingsInfo.settingCount = uint32_t(std::size(settings));
        settingsInfo.pSettings    = settings;
        instanceInfo.pNext        = &settingsInfo;
    }
    if (!error.IsFailure() && RHIOptions::GetInstance().ValidationMode() == RHIValidationMode::eRequired
        && (!layerSettings || !m_instanceExtensionFlags.hasDebugUtils))
    {
        error = MakeVulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Required Vulkan validation settings and messenger");
    }
    if (!error.IsFailure())
    {
        error = MakeVulkanError(vkCreateInstance(&instanceInfo, nullptr, &m_instance), "vkCreateInstance");
    }
    if (!error.IsFailure())
    {
        volkLoadInstance(m_instance);
        if (m_instanceExtensionFlags.hasDebugUtils)
        {
            error = MakeVulkanError(vkCreateDebugUtilsMessengerEXT(m_instance, &debugMessengerCI, nullptr, &m_messenger),
                                    "vkCreateDebugUtilsMessengerEXT");
        }
        if (!error.IsFailure() && layerSettings && m_messenger)
        {
            ZEN_RHI_LOG_INFO(
                "Active validation layer={} spec={} implementation={} requested_validate_sync=true requested_enable_message_limit=false",
                layer, m_validationLayer.specVersion, m_validationLayer.implementationVersion);
        }
    }
    return error;
}

RHIError VulkanRHI::SelectGPU(std::vector<RHIAdapterInfo>* adapters)
{
    HeapVector<VkPhysicalDevice> physicalDevices;
    const VkResult               result = EnumerateVulkanProperties(physicalDevices, vkEnumeratePhysicalDevices, m_instance);
    RHIError                     error  = MakeVulkanError(result, "vkEnumeratePhysicalDevices");

    uint32_t selectedIndex              = 0;

    bool foundValidCandidate            = false;

    VulkanPhysicalDeviceCandidateInfo selectedCandidateInfo{};

    std::string rejectionReasons;

    // Diagnostic override for cross-vendor runs; executables without options use the environment.
    std::string preferredDevice = RHIOptions::GetInstance().PreferredDevice();

    if (const char* environment = std::getenv("ZEN_VULKAN_DEVICE"); preferredDevice.empty() && environment != nullptr)
    {
        preferredDevice = environment;
    }

    bool selectedPreferred = false;

    for (uint32_t i = 0; i < physicalDevices.size(); i++)
    {
        const VulkanPhysicalDeviceCandidateInfo candidateInfo = EvaluatePhysicalDeviceCandidate(physicalDevices[i], error);
        if (error.IsFailure())
        {
            break;
        }

        ZEN_RHI_LOG_INFO("Found Vulkan Compatible GPU: {} ({})", candidateInfo.properties.deviceName,
                         GetPhysicalDeviceTypeName(candidateInfo.properties.deviceType));

        if (adapters)
        {
            const VkPhysicalDeviceProperties& gpu = candidateInfo.properties;
            adapters->push_back({gpu.deviceName, gpu.vendorID, gpu.deviceID, gpu.driverVersion, gpu.apiVersion,
                                 candidateInfo.unsupportedReason});
        }
        if (!candidateInfo.isValid)
        {
            ZEN_RHI_LOG_WARN("Rejecting GPU '{}': {}", candidateInfo.properties.deviceName, candidateInfo.unsupportedReason);

            rejectionReasons += fmt::format("{}: {}\n", candidateInfo.properties.deviceName, candidateInfo.unsupportedReason);

            continue;
        }

        ZEN_RHI_LOG_INFO(
            "GPU Candidate '{}': score={}, localMemory={} MiB, dedicatedCompute={}, dedicatedTransfer={}, asyncTransfer={}, descriptorIndexing={}, dynamicRendering={}, timelineSemaphore={}, bufferDeviceAddress={}, rayTracingPipeline={}, rayQuery={}, geometryShader={}",
            candidateInfo.properties.deviceName, candidateInfo.score,
            candidateInfo.deviceLocalMemoryBytes / (1024ull * 1024ull), candidateInfo.queueSummary.hasDedicatedCompute,
            candidateInfo.queueSummary.hasDedicatedTransfer, candidateInfo.queueSummary.hasAsyncTransfer,
            candidateInfo.hasDescriptorIndexing, candidateInfo.hasDynamicRendering, candidateInfo.hasTimelineSemaphore,
            candidateInfo.hasBufferDeviceAddress, candidateInfo.hasRayTracingPipeline, candidateInfo.hasRayQuery,
            candidateInfo.hasGeometryShader);

        const bool preferred =
            !preferredDevice.empty()
            && std::string_view(candidateInfo.properties.deviceName).find(preferredDevice) != std::string_view::npos;

        const bool isBetterCandidate =
            !foundValidCandidate || (preferred && !selectedPreferred)
            || (preferred == selectedPreferred
                && (candidateInfo.score > selectedCandidateInfo.score
                    || (candidateInfo.score == selectedCandidateInfo.score
                        && candidateInfo.deviceLocalMemoryBytes > selectedCandidateInfo.deviceLocalMemoryBytes)));

        if (isBetterCandidate)
        {
            selectedIndex         = i;

            selectedCandidateInfo = candidateInfo;

            foundValidCandidate   = true;

            selectedPreferred     = preferred;
        }
    }

    if (!error.IsFailure() && !adapters)
    {
        if (foundValidCandidate)
        {
            if (!preferredDevice.empty() && !selectedPreferred)
            {
                ZEN_RHI_LOG_WARN("No valid Vulkan GPU name contains '{}'; using the highest-scoring device", preferredDevice);
            }
            ZEN_RHI_LOG_INFO("Selected Vulkan GPU: {} ({}, score={}, localMemory={} MiB, vendor={}, driver={}, api={})",
                             selectedCandidateInfo.properties.deviceName,
                             GetPhysicalDeviceTypeName(selectedCandidateInfo.properties.deviceType),
                             selectedCandidateInfo.score, selectedCandidateInfo.deviceLocalMemoryBytes / (1024ull * 1024ull),
                             selectedCandidateInfo.properties.vendorID, selectedCandidateInfo.properties.driverVersion,
                             selectedCandidateInfo.properties.apiVersion);
            m_pDevice = ZEN_NEW() VulkanDevice(physicalDevices[selectedIndex]);
            if (!m_pDevice)
            {
                error = MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate VulkanDevice");
            }
        }
        else
        {
            ZEN_RHI_LOG_ERROR("No Vulkan device satisfies the required profile. {}", rejectionReasons);
            error = MakeRHIError(RHIErrorCode::eUnsupported, "Select Vulkan physical device", __FILE__, __LINE__);
        }
    }
    return error;
}

VulkanRHI::VulkanRHI() : m_resourceAllocator(ZEN_DEFAULT_PAGESIZE, false)
{
#if defined(ZEN_MACOS)
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 1);
#endif
    m_loaderResult = volkInitialize();
}

VulkanRHI::~VulkanRHI()
{
    if (m_loaderResult == VK_SUCCESS)
    {
        volkFinalize();
    }
    if (GVulkanRHI == this)
    {
        GVulkanRHI = nullptr;
    }
    if (GDynamicRHI == this)
    {
        GDynamicRHI = nullptr;
    }
}

VkPhysicalDevice VulkanRHI::GetPhysicalDevice() const
{
    return m_pDevice->GetPhysicalDeviceHandle();
}

VkDevice VulkanRHI::GetVkDevice() const
{
    return m_pDevice->GetVkHandle();
}

IRHICommandContext* VulkanRHI::GetCommandContext(RHICommandContextType contextType)
{
    FVulkanCommandListContext* context = ZEN_NEW() FVulkanCommandListContext(contextType, m_pDevice);

    if (!context->IsValid())
    {
        ZEN_DELETE(context);

        context = nullptr;
    }

    return context;
}

IRHICommandContext* VulkanRHI::GetTransferCommandContext()
{
    return GetCommandContext(RHICommandContextType::eTransfer);
}

void VulkanRHI::Init()
{
    const RHIError error = InitChecked();
    ZEN_RHI_VERIFY_MSG_F(!error.IsFailure(), "{} failed ({})", error.operation, error.nativeCode);
}

RHIError VulkanRHI::InitChecked(std::vector<RHIAdapterInfo>* adapters)
{
    RHIError error = MakeVulkanError(m_loaderResult, "volkInitialize");
    GVulkanRHI     = this;
    if (!error.IsFailure())
    {
        error = CreateInstance();
    }
    if (!error.IsFailure())
    {
        error = SelectGPU(adapters);
    }
    if (!error.IsFailure() && !adapters)
    {
        m_resourceAllocator.Init();
        m_pResourceFactory = ZEN_NEW() VulkanResourceFactory();
        error = m_pResourceFactory ? m_pDevice->InitChecked()
                                   : MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate VulkanResourceFactory");
        if (!error.IsFailure())
        {
            PopulateGPUInfo();
            GVkMemAllocator = ZEN_NEW() VulkanMemoryAllocator();
            error = GVkMemAllocator
                      ? MakeVulkanError(GVkMemAllocator->Init(m_instance, GetPhysicalDevice(), GetVkDevice(),
                                                              m_pDevice->GetExtensionFlags().hasBufferDeviceAddress != 0,
                                                              m_pDevice->GetExtensionFlags().hasMemoryBudget != 0),
                                        "vmaCreateAllocator")
                      : MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate VulkanMemoryAllocator");
        }
        if (!error.IsFailure())
        {
            m_pDescriptorPoolManager2        = ZEN_NEW() VulkanDescriptorPoolManager2(m_pDevice);
            m_pBindlessDescriptorPoolManager = ZEN_NEW() VulkanBindlessDescriptorPoolManager();
            error = m_pDescriptorPoolManager2 && m_pBindlessDescriptorPoolManager
                      ? m_pBindlessDescriptorPoolManager->InitChecked()
                      : MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate Vulkan descriptor pool managers");
        }
        if (!error.IsFailure())
        {
            m_gpuInfo.bindlessHeapCapacities = m_pBindlessDescriptorPoolManager->GetCapacities();
            m_pUniformBufferAllocator        = ZEN_NEW() VulkanUniformBufferAllocator();
            if (!m_pUniformBufferAllocator)
            {
                error = MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate VulkanUniformBufferAllocator");
            }
        }
        if (!error.IsFailure())
        {
            m_pUniformBufferAllocator->Init(RHIFrameState::kMaxFramesInFlight, 4 * 1024 * 1024, 8);
            m_initialized = true;
        }
    }
    return error;
}

void VulkanRHI::PopulateGPUInfo()
{
    const VkPhysicalDeviceProperties properties = m_pDevice->GetPhysicalDeviceProperties();

    const VkPhysicalDeviceLimits& limits        = properties.limits;

    std::copy_n(properties.deviceName, m_gpuInfo.deviceName.size(), m_gpuInfo.deviceName.begin());

    m_gpuInfo.vendorID         = properties.vendorID;

    m_gpuInfo.deviceID         = properties.deviceID;

    m_gpuInfo.apiVersion       = properties.apiVersion;

    m_gpuInfo.driverVersionRaw = properties.driverVersion;

    VkPhysicalDeviceMemoryProperties memory{};

    vkGetPhysicalDeviceMemoryProperties(m_pDevice->GetPhysicalDeviceHandle(), &memory);

    for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
    {
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
        {
            m_gpuInfo.deviceLocalMemoryBytes = std::max(m_gpuInfo.deviceLocalMemoryBytes, memory.memoryHeaps[heap].size);
        }
    }

    m_gpuInfo.supportGeometryShader   = m_pDevice->GetPhysicalDeviceFeatures().geometryShader;

    m_gpuInfo.supportIndependentBlend = m_pDevice->GetPhysicalDeviceFeatures().independentBlend != VK_FALSE;

    m_gpuInfo.supportVertexPipelineStoresAndAtomics =
        m_pDevice->GetPhysicalDeviceFeatures().vertexPipelineStoresAndAtomics != VK_FALSE;

    m_gpuInfo.supportSamplerAnisotropy         = m_pDevice->GetPhysicalDeviceFeatures().samplerAnisotropy != VK_FALSE;
    m_gpuInfo.supportImageCubeArray            = m_pDevice->GetPhysicalDeviceFeatures().imageCubeArray != VK_FALSE;
    m_gpuInfo.supportSamplerMirrorClampToEdge  = m_pDevice->GetExtensionFlags().hasSamplerMirrorClampToEdge != 0;
    m_gpuInfo.maxDrawIndexedIndexValue         = m_pDevice->GetPhysicalDeviceProperties().limits.maxDrawIndexedIndexValue;

    m_gpuInfo.supportFillModeNonSolid          = m_pDevice->GetPhysicalDeviceFeatures().fillModeNonSolid != VK_FALSE;

    m_gpuInfo.supportDepthClamp                = m_pDevice->GetPhysicalDeviceFeatures().depthClamp != VK_FALSE;

    m_gpuInfo.supportDepthBiasClamp            = m_pDevice->GetPhysicalDeviceFeatures().depthBiasClamp != VK_FALSE;

    m_gpuInfo.supportWideLines                 = m_pDevice->GetPhysicalDeviceFeatures().wideLines != VK_FALSE;

    m_gpuInfo.supportSampleRateShading         = m_pDevice->GetPhysicalDeviceFeatures().sampleRateShading != VK_FALSE;

    m_gpuInfo.supportAlphaToOne                = m_pDevice->GetPhysicalDeviceFeatures().alphaToOne != VK_FALSE;

    m_gpuInfo.supportDepthBounds               = m_pDevice->GetPhysicalDeviceFeatures().depthBounds != VK_FALSE;

    m_gpuInfo.supportLogicOp                   = m_pDevice->GetPhysicalDeviceFeatures().logicOp != VK_FALSE;

    m_gpuInfo.supportMultiDrawIndirect         = m_pDevice->GetPhysicalDeviceFeatures().multiDrawIndirect != VK_FALSE;

    m_gpuInfo.supportDrawIndirectFirstInstance = m_pDevice->GetPhysicalDeviceFeatures().drawIndirectFirstInstance != VK_FALSE;
    m_gpuInfo.maxDrawIndirectCount             = m_pDevice->GetPhysicalDeviceProperties().limits.maxDrawIndirectCount;
    m_gpuInfo.maxVertexOutputComponents        = m_pDevice->GetPhysicalDeviceProperties().limits.maxVertexOutputComponents;
    m_gpuInfo.maxFragmentInputComponents       = m_pDevice->GetPhysicalDeviceProperties().limits.maxFragmentInputComponents;

    m_gpuInfo.supportTessellationShader        = m_pDevice->GetPhysicalDeviceFeatures().tessellationShader != VK_FALSE;

    m_gpuInfo.supportFragmentStoresAndAtomics  = m_pDevice->GetPhysicalDeviceFeatures().fragmentStoresAndAtomics;
    m_gpuInfo.supportBufferInt64Atomics        = m_pDevice->GetExtensionFlags().hasBufferInt64Atomics != 0;

    m_gpuInfo.uniformBufferAlignment           = limits.minUniformBufferOffsetAlignment;

    m_gpuInfo.storageBufferAlignment           = limits.minStorageBufferOffsetAlignment;

    m_gpuInfo.maxComputeWorkGroupInvocations   = limits.maxComputeWorkGroupInvocations;
    m_gpuInfo.maxComputeSharedMemorySize       = limits.maxComputeSharedMemorySize;
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2        subgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    subgroupProperties.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(m_pDevice->GetPhysicalDeviceHandle(), &subgroupProperties);
    m_gpuInfo.supportComputeSubgroupBallot = (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0
                                          && (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
    m_gpuInfo.supportComputeSubgroupReduction =
        m_gpuInfo.supportComputeSubgroupBallot && (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0;

    m_gpuInfo.maxBoundDescriptorSets = limits.maxBoundDescriptorSets;

    m_gpuInfo.maxStorageBufferRange  = limits.maxStorageBufferRange;

    m_gpuInfo.maxColorAttachments    = limits.maxColorAttachments;
    m_gpuInfo.subTexelPrecisionBits  = limits.subTexelPrecisionBits;

    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        m_gpuInfo.maxComputeWorkGroupSize[axis]  = limits.maxComputeWorkGroupSize[axis];

        m_gpuInfo.maxComputeWorkGroupCount[axis] = limits.maxComputeWorkGroupCount[axis];
    }

    RHIRayQueryCapabilities&    rayQuery   = m_gpuInfo.rayQuery;
    const DeviceExtensionFlags& extensions = m_pDevice->GetExtensionFlags();
    rayQuery.bufferDeviceAddress           = extensions.hasBufferDeviceAddress != 0;
    rayQuery.accelerationStructure         = extensions.hasAccelerationStructure != 0;
    rayQuery.rayQuery                      = extensions.hasRayQuery != 0;
    rayQuery.opacityMicromap               = extensions.hasOpacityMicromap != 0;
    rayQuery.graphicsBuild                 = GetQueueCopyCapabilities(RHICommandContextType::eGraphics).compute;
    if (rayQuery.accelerationStructure)
    {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &accelerationProperties;
        vkGetPhysicalDeviceProperties2(m_pDevice->GetPhysicalDeviceHandle(), &properties);
        rayQuery.scratchAlignment = accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
        rayQuery.maxGeometries    = accelerationProperties.maxGeometryCount;
        rayQuery.maxPrimitives    = accelerationProperties.maxPrimitiveCount;
        rayQuery.maxInstances     = accelerationProperties.maxInstanceCount;
        VkFormatProperties formats{};
        vkGetPhysicalDeviceFormatProperties(m_pDevice->GetPhysicalDeviceHandle(), VK_FORMAT_R32G32B32_SFLOAT, &formats);
        rayQuery.triangleFloat3 =
            (formats.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR) != 0;
    }

    RHIRayTracingCapabilities& rayTracing = m_gpuInfo.rayTracing;
    rayTracing.pipeline                   = extensions.hasRaytracingPipeline != 0;
    rayTracing.indirect                   = rayTracing.pipeline && extensions.hasTraceRaysIndirect != 0;
    rayTracing.invocationReorder          = extensions.hasInvocationReorderEXT ? RHIShaderInvocationReorder::eEXT
                                          : extensions.hasInvocationReorderNV  ? RHIShaderInvocationReorder::eNV
                                                                               : RHIShaderInvocationReorder::eNone;
    if (rayTracing.pipeline)
    {
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR tracing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &tracing;
        vkGetPhysicalDeviceProperties2(m_pDevice->GetPhysicalDeviceHandle(), &properties);
        rayTracing.shaderGroupHandleSize         = tracing.shaderGroupHandleSize;
        rayTracing.shaderGroupHandleAlignment    = tracing.shaderGroupHandleAlignment;
        rayTracing.shaderGroupBaseAlignment      = tracing.shaderGroupBaseAlignment;
        rayTracing.maxShaderGroupStride          = tracing.maxShaderGroupStride;
        rayTracing.maxRayRecursionDepth          = tracing.maxRayRecursionDepth;
        rayTracing.maxRayDispatchInvocationCount = tracing.maxRayDispatchInvocationCount;
        rayTracing.maxRayHitAttributeSize        = tracing.maxRayHitAttributeSize;
    }

    if (rayQuery.opacityMicromap)
    {
        VkPhysicalDeviceOpacityMicromapPropertiesKHR micromap{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &micromap;
        vkGetPhysicalDeviceProperties2(m_pDevice->GetPhysicalDeviceHandle(), &properties);
        rayQuery.maxOpacitySubdivisionLevel = micromap.maxOpacity4StateSubdivisionLevel;
        rayQuery.maxMicromapTriangles       = micromap.maxMicromapTriangles;
    }
}

RHIBindlessHandle VulkanRHI::RegisterBindlessResource(RHIResource* pResource, uint32_t slotIndex)
{
    RHIBindlessHandle handle;

    if (m_pBindlessDescriptorPoolManager != nullptr)
    {
        m_pBindlessDescriptorPoolManager->RegisterBindlessResource(pResource, slotIndex, &handle);
    }

    return handle;
}

bool VulkanRHI::UnregisterBindlessResource(RHIBindlessHandle handle)
{
    return m_pBindlessDescriptorPoolManager != nullptr && m_pBindlessDescriptorPoolManager->UnregisterBindlessResource(handle);
}

bool VulkanRHI::IsBindlessResourceRegistered(RHIBindlessHandle handle)
{
    return m_pBindlessDescriptorPoolManager != nullptr && m_pBindlessDescriptorPoolManager->IsRegistered(handle);
}

bool VulkanRHI::ResetBindlessResources()
{
    const bool reset = !AreSubmissionsBlocked()
                    && (m_pBindlessDescriptorPoolManager == nullptr || m_pBindlessDescriptorPoolManager->ResetRegistrations());

    return reset;
}

void VulkanRHI::CollectRetiredBindlessResources()
{
    if (m_pDevice != nullptr && !AreSubmissionsBlocked())
    {
        QueryLastCompletedSerial(RHICommandContextType::eGraphics);

        QueryLastCompletedSerial(RHICommandContextType::eAsyncCompute);

        QueryLastCompletedSerial(RHICommandContextType::eTransfer);
    }

    if (m_pBindlessDescriptorPoolManager != nullptr)
    {
        m_pBindlessDescriptorPoolManager->CollectRetiredResources();
    }

    m_lifetimeTracker.Collect();
}

void VulkanRHI::BeginFrame()
{
    GetRHIThread().CheckOwnership();

    GVkMemAllocator->BeginFrame(static_cast<uint32_t>(ToValue(GRHIFrameState.GetFrameNumber())));

    CollectRetiredBindlessResources();

    ZEN_RHI_VERIFY(m_pDescriptorPoolManager2 != nullptr);

    ZEN_RHI_VERIFY(m_pUniformBufferAllocator != nullptr);

    m_pDescriptorPoolManager2->BeginFrame(static_cast<uint32_t>(ToValue(GRHIFrameState.GetFrameNumber())));

    // RenderDevice has waited for this frame slot before reusing its uniform storage.
    m_pUniformBufferAllocator->BeginFrame(ToIndex(GRHIFrameState.GetFrameSlot()));
}

void VulkanRHI::BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing)
{
    GetRHIThread().CheckOwnership();

    const bool overlapping = m_gpuFrameTiming != nullptr;

    if (overlapping)
    {
        m_gpuFrameTiming->Seal(RHIGPUTimingStatus::eError);
    }

    m_gpuFrameTiming = timing;

    if (m_gpuFrameTiming != nullptr)
    {
        if (overlapping || m_pendingNativeRecordings != 0 || AreSubmissionsBlocked())
        {
            m_gpuFrameTiming->Seal(RHIGPUTimingStatus::eError);
        }
        else if (!m_pDevice->GetExtensionFlags().hasCalibratedTimestamps)
        {
            m_gpuFrameTiming->Seal(RHIGPUTimingStatus::eUnsupported);
        }
    }
}

void VulkanRHI::EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded)
{
    GetRHIThread().CheckOwnership();

    const bool valid = timing != nullptr && timing == m_gpuFrameTiming && succeeded && m_pendingNativeRecordings == 0
                    && !AreSubmissionsBlocked();

    if (m_gpuFrameTiming != nullptr)
    {
        m_gpuFrameTiming->Seal(valid ? RHIGPUTimingStatus::eAvailable : RHIGPUTimingStatus::eError);

        m_gpuFrameTiming.Reset();
    }

    if (!valid && timing != nullptr)
    {
        timing->Seal(RHIGPUTimingStatus::eError);
    }
}

RHIGPUTimingPtr VulkanRHI::RegisterNativeGPUFrameRecording(bool included)
{
    ++m_pendingNativeRecordings;

    RHIGPUTimingPtr interval;

    if (m_gpuFrameTiming != nullptr && m_gpuFrameTiming->GetStatus() == RHIGPUTimingStatus::ePending)
    {
        if (included)
        {
            interval = m_gpuFrameTiming->AddInterval();
        }
        else
        {
            m_gpuFrameTiming->ExcludeInterval();
        }
    }

    return interval;
}

void VulkanRHI::ReleaseNativeGPUFrameRecording()
{
    if (m_pendingNativeRecordings != 0)
    {
        --m_pendingNativeRecordings;
    }
    else if (m_gpuFrameTiming != nullptr)
    {
        m_gpuFrameTiming->Seal(RHIGPUTimingStatus::eError);
    }
}

void VulkanRHI::Destroy()
{
    const bool wasInitialized = m_initialized;
    m_initialized             = false;
    if (m_gpuFrameTiming != nullptr)
    {
        m_gpuFrameTiming->Seal(RHIGPUTimingStatus::eDiscarded);

        m_gpuFrameTiming.Reset();
    }

    if (wasInitialized)
    {
        WaitDeviceIdle();
    }

    DestroyPlatformCommandListPool();

    if (m_pDevice != nullptr)
    {
        m_pDevice->DestroyQueues();
    }

    if (m_pUniformBufferAllocator != nullptr)
    {
        m_pUniformBufferAllocator->Destroy();

        ZEN_DELETE(m_pUniformBufferAllocator);

        m_pUniformBufferAllocator = nullptr;
    }

    if (m_pDescriptorPoolManager2 != nullptr)
    {
        m_pDescriptorPoolManager2->Destroy();

        ZEN_DELETE(m_pDescriptorPoolManager2);

        m_pDescriptorPoolManager2 = nullptr;
    }

    if (m_pBindlessDescriptorPoolManager != nullptr)
    {
        m_pBindlessDescriptorPoolManager->Destroy();

        ZEN_DELETE(m_pBindlessDescriptorPoolManager);

        m_pBindlessDescriptorPoolManager = nullptr;
    }

    m_lifetimeTracker.Destroy();

    VerifyTeardownOwnership(m_resourceAllocator.GetAllocationCount() == 0,
                            "All RHI resources must be released before backend teardown", __FILE__, __LINE__);

    ZEN_DELETE(GVkMemAllocator);

    GVkMemAllocator = nullptr;

    if (m_pDevice != nullptr)
    {
        m_pDevice->Destroy();

        ZEN_DELETE(m_pDevice);

        m_pDevice = nullptr;
    }

    ZEN_DELETE(m_pResourceFactory);

    m_pResourceFactory = nullptr;

    // destroy debug utils messenger
    if (m_messenger != VK_NULL_HANDLE)
    {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);

        m_messenger = VK_NULL_HANDLE;
    }

    // destroy instance
    if (m_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_instance, nullptr);

        m_instance = VK_NULL_HANDLE;
    }
}
} // namespace zen
