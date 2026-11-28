#include "ZenRHI/RHIDiagnostics.h"
#include <span>
#include "Vulkan/VulkanDevice.h"
#include "ZenRHI/RHICommandList.h"
#include "ZenRHI/RHICommon.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanQueueSelection.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanExtension.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanSynchronization.h"
#include "Vulkan/VulkanCopyCapabilities.h"
#include "Vulkan/VulkanDescriptorAllocator.h"

#include <algorithm>
#include "VulkanFaultReport.h"
#include "VulkanResourceSharing.h"
#include "Vulkan/VulkanBindlessCapacity.h"

namespace zen
{
void VulkanRHI::CheckDeviceLoss(VkResult result, const char* operation)
{
    if (result == VK_ERROR_DEVICE_LOST)
    {
        BlockSubmissions({RHIErrorCode::eDeviceLost, int64_t(result), operation, __FILE__, __LINE__});

        if (m_pDevice != nullptr)
        {
            m_pDevice->ReportDeviceLoss(operation);
        }
    }
}

RHIError VulkanRHI::ReportCreationFailure(VkResult result, const char* operation, const char* source, uint32_t line)
{
    CheckDeviceLoss(result, operation);

    // A result of VK_SUCCESS still failed a post-condition.
    return RHIErrorOr(MakeVulkanError(result, operation, source, line),
                      MakeRHIError(RHIErrorCode::eBackendFailure, operation, source, line));
}

void VulkanDevice::RegisterDiagnosticBuffer(VulkanCommandBuffer* buffer)
{
    m_diagnosticBuffers.push_back(buffer);
}

void VulkanDevice::UnregisterDiagnosticBuffer(VulkanCommandBuffer* buffer)
{
    m_diagnosticBuffers.erase(std::remove(m_diagnosticBuffers.begin(), m_diagnosticBuffers.end(), buffer),
                              m_diagnosticBuffers.end());
}

namespace
{
// Device cache files contain the driver bytes followed by their length and checksum, both little endian on Windows x64.
struct CacheTrailer
{
    uint64_t length;
    uint64_t checksum;
};

uint64_t CacheChecksum(std::span<const uint8_t> data)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : data)
    {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    return hash;
}

std::span<const uint8_t> CachePayload(std::span<const uint8_t> seed, const VkPhysicalDeviceProperties& gpu)
{
    std::span<const uint8_t> payload;
    if (seed.size() >= sizeof(VkPipelineCacheHeaderVersionOne) + sizeof(CacheTrailer))
    {
        VkPipelineCacheHeaderVersionOne header{};
        CacheTrailer                    trailer{};
        const size_t                    length = seed.size() - sizeof(trailer);
        std::memcpy(&header, seed.data(), sizeof(header));
        std::memcpy(&trailer, seed.data() + length, sizeof(trailer));
        if (trailer.length == length && header.headerSize == sizeof(header)
            && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header.vendorID == gpu.vendorID
            && header.deviceID == gpu.deviceID
            && std::memcmp(header.pipelineCacheUUID, gpu.pipelineCacheUUID, VK_UUID_SIZE) == 0
            && trailer.checksum == CacheChecksum(seed.first(length)))
        {
            payload = seed.first(length);
        }
    }
    return payload;
}
} // namespace

RHIError VulkanRHI::SnapshotPipelineCache(size_t byteLimit, std::vector<uint8_t>& bytes)
{
    RHIError              error = GetTerminalError();
    const VkPipelineCache cache = m_pDevice->GetPipelineCache();
    size_t                size{};
    if (!error.IsFailure() && cache == VK_NULL_HANDLE)
    {
        error = MakeRHIError(RHIErrorCode::eUnsupported, "Device has no pipeline cache", __FILE__, __LINE__);
    }
    if (!error.IsFailure())
    {
        error = MakeVulkanError(vkGetPipelineCacheData(GetVkDevice(), cache, &size, nullptr), "Query pipeline cache size");
        if (!error.IsFailure()
            && (size < sizeof(VkPipelineCacheHeaderVersionOne) || size > byteLimit || byteLimit - size < sizeof(CacheTrailer)))
        {
            error =
                MakeRHIError(RHIErrorCode::eInvalidArgument, "Pipeline cache outside snapshot size limit", __FILE__, __LINE__);
        }
    }
    if (!error.IsFailure())
    {
        std::vector<uint8_t> snapshot(size + sizeof(CacheTrailer));
        const VkResult       result = vkGetPipelineCacheData(GetVkDevice(), cache, &size, snapshot.data());
        if (result == VK_SUCCESS)
        {
            const CacheTrailer trailer{size, CacheChecksum(std::span(snapshot).first(size))};
            snapshot.resize(size + sizeof(trailer));
            std::memcpy(snapshot.data() + size, &trailer, sizeof(trailer));
            bytes.swap(snapshot);
        }
        else
        {
            error = result == VK_INCOMPLETE ? RHIError{RHIErrorCode::eBackendFailure, result,
                                                       "Incomplete pipeline cache snapshot", __FILE__, __LINE__}
                                            : MakeVulkanError(result, "vkGetPipelineCacheData");
        }
    }

    CheckDeviceLoss(static_cast<VkResult>(error.nativeCode), error.operation);
    return error;
}

void VulkanDevice::ReportDeviceLoss(const char* operation)
{
    if (!m_faultReported)
    {
        m_faultReported = true;

        VulkanFaultReport report;
        report.Append("RHI device lost: operation={} gpu={} vendor={} device={} driver={}\n", operation, m_gpuProps.deviceName,
                      m_gpuProps.vendorID, m_gpuProps.deviceID, m_gpuProps.driverVersion);
        for (uint32_t i = 0; i < ToUnderlying(RHICommandContextType::eMax); ++i)
        {
            const VulkanQueue* queue = GetQueue(static_cast<RHICommandContextType>(i));
            if (queue)
            {
                report.Append("queue={}:{} submitted={} completed={}\n", queue->GetFamilyIndex(), queue->GetQueueIndex(),
                              queue->GetLastSubmittedSerial(), queue->GetLastCompletedSerial());
            }
        }
        if (m_device && m_extensionFlags.hasDeviceFault && vkGetDeviceFaultInfoEXT != nullptr
            && m_pRHI->GetDeviceDesc().DeviceLossDiagnostics())
        {
            // Fixed storage remains usable after allocation failure. VK_INCOMPLETE
            // explicitly reports a truncated report rather than allocating during loss.
            VkDeviceFaultAddressInfoEXT addresses[64]{};

            VkDeviceFaultVendorInfoEXT vendors[16]{};

            VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};

            VkResult             result = vkGetDeviceFaultInfoEXT(m_device, &counts, nullptr);
            VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            if (result == VK_SUCCESS)
            {
                counts.addressInfoCount = std::min(counts.addressInfoCount, 64u);
                counts.vendorInfoCount  = std::min(counts.vendorInfoCount, 16u);
                counts.vendorBinarySize = 0;
                info.pAddressInfos      = addresses;
                info.pVendorInfos       = vendors;
                result                  = vkGetDeviceFaultInfoEXT(m_device, &counts, &info);
            }
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                counts.addressInfoCount = counts.vendorInfoCount = 0;
                info.description[0]                              = 0;
            }
            report.Append("RHI device fault result={} description={}\n", int(result), info.description);
            for (uint32_t i = 0; i < std::min(counts.addressInfoCount, 64u); ++i)
            {
                report.Append("fault address={} precision={} type={}\n", addresses[i].reportedAddress,
                              addresses[i].addressPrecision, uint32_t(addresses[i].addressType));
            }
            for (uint32_t i = 0; i < std::min(counts.vendorInfoCount, 16u); ++i)
            {
                report.Append("vendor fault={} code={} data={}\n", vendors[i].description, vendors[i].vendorFaultCode,
                              vendors[i].vendorFaultData);
            }
        }
        for (const VulkanCommandBuffer* buffer : m_diagnosticBuffers)
        {
            buffer->ReportBreadcrumbs(report);
        }
        report.Publish();
    }
}

VulkanDevice::VulkanDevice(VulkanRHI* pRHI, VkPhysicalDevice gpu) : m_device(VK_NULL_HANDLE), m_pRHI(pRHI), m_gpu(gpu)
{
    vkGetPhysicalDeviceProperties(m_gpu, &m_gpuProps);
}

std::string VulkanDevice::GetUnsupportedReason(VkPhysicalDevice gpu, const RHIDeviceDesc& desc, RHIError* error)
{
    std::string reason;

    VkPhysicalDeviceProperties properties{};

    vkGetPhysicalDeviceProperties(gpu, &properties);

    if (properties.apiVersion < kVulkanApiVersion)
    {
        reason = "Vulkan 1.2 is required";
    }

    if (reason.empty())
    {
        const HeapVector<VkExtensionProperties> extensions = GetSupportedDeviceExtensions(gpu, error);

        for (const char* required : {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME})
        {
            if (std::none_of(extensions.begin(), extensions.end(), [required](const VkExtensionProperties& extension) {
                    return strcmp(required, extension.extensionName) == 0;
                }))
            {
                reason = std::string("Required extension is missing: ") + required;

                break;
            }
        }
    }

    if (reason.empty())
    {
        uint32_t count = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);

        HeapVector<VkQueueFamilyProperties> queues(count);

        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.data());

        if (std::none_of(queues.begin(), queues.end(), [](const VkQueueFamilyProperties& queue) {
                return queue.queueCount != 0
                    && (queue.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                           == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
            }))
        {
            reason = "A graphics queue with compute support is required";
        }
    }

    bool cubeArrays = false;

    if (reason.empty())
    {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

        VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};

        VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};

        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};

        features.pNext = &dynamic;

        dynamic.pNext  = &indexing;

        indexing.pNext = &timeline;

        vkGetPhysicalDeviceFeatures2(gpu, &features);

        cubeArrays = features.features.imageCubeArray == VK_TRUE;

        if (!dynamic.dynamicRendering)
        {
            reason = "The dynamicRendering feature is required";
        }
        else if (!timeline.timelineSemaphore)
        {
            reason = "The timelineSemaphore feature is required";
        }
        else if (const char* missing = MissingDescriptorIndexingFeature(indexing))
        {
            reason = fmt::format("The {} descriptor indexing feature is required", missing);
        }
    }

    if (reason.empty())
    {
        VkPhysicalDeviceDescriptorIndexingProperties limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};

        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

        properties2.pNext = &limits;

        vkGetPhysicalDeviceProperties2(gpu, &properties2);

        const RHIBindlessHeapCapacities capacities = GetBindlessHeapRequest(desc.BindlessHeapCapacities(), cubeArrays);
        RHIBindlessHeapCapacities       fitted     = kNoBindlessHeaps;

        if (!FitBindlessHeapCapacities(limits, capacities, fitted))
        {
            reason = fmt::format(
                "Descriptor indexing limits cannot fit the bindless heap requests with at least one slot per heap ({} 2D textures, {} cube textures, {} samplers, {} 2D arrays, {} cube arrays)",
                capacities.Get(RHIBindlessHeapType::eTexture2D), capacities.Get(RHIBindlessHeapType::eTextureCube),
                capacities.Get(RHIBindlessHeapType::eSampler), capacities.Get(RHIBindlessHeapType::eTexture2DArray),
                capacities.Get(RHIBindlessHeapType::eTextureCubeArray));
        }
    }

    return reason;
}

static std::string GetQueuePropString(const VkQueueFamilyProperties& queueProp)
{
    std::string queuePropStr;

    if ((queueProp.queueFlags & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT)
    {
        queuePropStr += " Gfx";
    }

    if ((queueProp.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT)
    {
        queuePropStr += " Compute";
    }

    if ((queueProp.queueFlags & VK_QUEUE_TRANSFER_BIT) == VK_QUEUE_TRANSFER_BIT)
    {
        queuePropStr += " Transfer";
    }

    if ((queueProp.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) == VK_QUEUE_SPARSE_BINDING_BIT)
    {
        queuePropStr += " Sparse";
    }

    return queuePropStr;
}

DataFormat VulkanRHI::GetSupportedDepthFormat()
{
    VkFormat defaultFormat{VK_FORMAT_D16_UNORM};

    const HeapVector<VkFormat> formatList = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                                             VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};

    for (const VkFormat& format : formatList)
    {
        VkFormatProperties formatProps;

        vkGetPhysicalDeviceFormatProperties(m_pDevice->GetPhysicalDeviceHandle(), format, &formatProps);

        if (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            defaultFormat = format;

            break;
        }
    }

    return static_cast<DataFormat>(defaultFormat);
}

const RHIGPUInfo& VulkanRHI::QueryGPUInfo() const
{
    return m_gpuInfo;
}

RHITextureCopyCapabilities VulkanRHI::GetTextureCopyCapabilities(DataFormat format) const
{
    VkFormatProperties properties{};

    vkGetPhysicalDeviceFormatProperties(m_pDevice->GetPhysicalDeviceHandle(), static_cast<VkFormat>(format), &properties);

    return MakeTextureCopyCapabilities(properties);
}

RHIQueueCopyCapabilities VulkanRHI::GetQueueCopyCapabilities(RHICommandContextType type) const
{
    const VulkanQueue* queue = m_pDevice->GetQueue(type);

    return MakeQueueCopyCapabilities(m_pDevice->GetQueueFamilyProperties(queue->GetFamilyIndex()));
}

/**
 * Create VkDevice and initialise
 */
RHIError VulkanDevice::InitChecked()
{
    RHIError          error;
    const std::string reason = GetUnsupportedReason(m_gpu, m_pRHI->GetDeviceDesc(), &error);
    if (!error.IsFailure() && !reason.empty())
    {
        ZEN_RHI_LOG_ERROR("Cannot initialize Vulkan device '{}': {}", m_gpuProps.deviceName, reason);
        error = MakeRHIError(RHIErrorCode::eUnsupported, "Vulkan device profile", __FILE__, __LINE__);
    }
    return error.IsFailure() ? error : InitializeSupportedDevice();
}

RHIError VulkanDevice::InitializeSupportedDevice()
{
    RHIError error;
    // query base features
    vkGetPhysicalDeviceFeatures(m_gpu, &m_supportedFeatures);

    // query queue properties
    uint32_t count = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &count, nullptr);

    ZEN_ASSERT(count > 0);

    m_queueFamilyProps.resize(count);

    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &count, m_queueFamilyProps.data());

    VulkanDeviceExtensionSelection selection = SelectDeviceExtensions(*this, &error);

    m_extensionFlags                         = selection.flags;

    VkPhysicalDeviceProperties2 physicalDeviceProperties2;

    InitVkStruct(physicalDeviceProperties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);

    InitVkStruct(m_descriptorIndexingProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES);

    physicalDeviceProperties2.pNext = &m_descriptorIndexingProperties;

    vkGetPhysicalDeviceProperties2(m_gpu, &physicalDeviceProperties2);

    if (m_extensionFlags.hasAccelerationStructure)
    {
        InitVkStruct(m_accelerationStructureProperties,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR);
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &m_accelerationStructureProperties;
        vkGetPhysicalDeviceProperties2(m_gpu, &properties);
    }

    if (!error.IsFailure())
    {
        error = SetupDevice(selection);
    }
    if (!error.IsFailure())
    {
        m_pSemaphoreManager = ZEN_NEW() VulkanSemaphoreManager(this);
        if (!m_pSemaphoreManager)
        {
            error = MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate Vulkan synchronization managers");
        }
    }
    return error;
}

RHIError VulkanDevice::SetupDevice(VulkanDeviceExtensionSelection& selection)
{
    VkDeviceCreateInfo deviceInfo;

    InitVkStruct(deviceInfo, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

    m_extensions = selection.names;

    // The enabled features chain lives in the selection, which outlives vkCreateDevice.
    deviceInfo.pNext = selection.enabledFeatures.Link(selection.enabled);

    // set up queue info
    HeapVector<VkDeviceQueueCreateInfo> deviceQueueInfos;

    const VulkanQueueSelection queues = SelectVulkanQueues(m_queueFamilyProps);

    // Init's supported-device check requires a nonempty graphics/compute family.
    ZEN_ASSERT(queues.IsValid());

    ZEN_RHI_LOG_INFO("Found {} Vulkan Queue Families", m_queueFamilyProps.size());

    uint32_t numPriorities = 0;

    for (int32_t queueFamilyIndex = 0; queueFamilyIndex < m_queueFamilyProps.size(); queueFamilyIndex++)
    {
        VkQueueFamilyProperties const& queueFamilyProp = m_queueFamilyProps[queueFamilyIndex];

        if (queueFamilyProp.queueCount == 0)
        {
            continue;
        }

        const uint32_t requestedCount = queues.GetRequestedQueueCount(queueFamilyIndex);

        if (requestedCount == 0)
        {
            ZEN_RHI_LOG_INFO("Skipping unused queue family at index: {} ({})", queueFamilyIndex,
                             GetQueuePropString(queueFamilyProp));

            continue;
        }

        VkDeviceQueueCreateInfo queueInfo;

        InitVkStruct(queueInfo, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);

        queueInfo.queueFamilyIndex = queueFamilyIndex;

        queueInfo.queueCount       = requestedCount;

        deviceQueueInfos.emplace_back(queueInfo);

        numPriorities += requestedCount;

        ZEN_RHI_LOG_INFO("Initializing Queue Family at index {} ({})", queueFamilyIndex, GetQueuePropString(queueFamilyProp));
    }

    HeapVector<float> queuePriorities;

    queuePriorities.resize(numPriorities);

    float* pCurrentPriority = queuePriorities.data();

    for (int i = 0; i < deviceQueueInfos.size(); i++)
    {
        VkDeviceQueueCreateInfo& queueInfo = deviceQueueInfos[i];

        queueInfo.pQueuePriorities         = pCurrentPriority;

        for (uint32_t queueIndex = 0; queueIndex < queueInfo.queueCount; queueIndex++)
        {
            *pCurrentPriority++ = 1.0f;
        }
    }

    HeapVector<const char*> extensionNames;

    extensionNames.reserve(m_extensions.size());

    for (NameID extension : m_extensions)
    {
        extensionNames.push_back(extension.CStr());
    }

    deviceInfo.enabledExtensionCount   = static_cast<uint32_t>(extensionNames.size());

    deviceInfo.ppEnabledExtensionNames = extensionNames.empty() ? nullptr : extensionNames.data();

    deviceInfo.queueCreateInfoCount    = static_cast<uint32_t>(deviceQueueInfos.size());

    deviceInfo.pQueueCreateInfos       = deviceQueueInfos.data();

    VkPhysicalDeviceFeatures enabledFeatures{};

    enabledFeatures.independentBlend               = m_supportedFeatures.independentBlend;

    enabledFeatures.dualSrcBlend                   = m_supportedFeatures.dualSrcBlend;

    enabledFeatures.geometryShader                 = m_supportedFeatures.geometryShader;

    enabledFeatures.fragmentStoresAndAtomics       = m_supportedFeatures.fragmentStoresAndAtomics;

    enabledFeatures.vertexPipelineStoresAndAtomics = m_supportedFeatures.vertexPipelineStoresAndAtomics;

    enabledFeatures.samplerAnisotropy              = m_supportedFeatures.samplerAnisotropy;
    enabledFeatures.textureCompressionBC           = m_supportedFeatures.textureCompressionBC;
    enabledFeatures.imageCubeArray                 = m_supportedFeatures.imageCubeArray;
    enabledFeatures.shaderInt64                    = m_extensionFlags.hasBufferInt64Atomics;
    enabledFeatures.fullDrawIndexUint32            = m_supportedFeatures.fullDrawIndexUint32;

    enabledFeatures.fillModeNonSolid               = m_supportedFeatures.fillModeNonSolid;

    enabledFeatures.depthClamp                     = m_supportedFeatures.depthClamp;

    enabledFeatures.depthBiasClamp                 = m_supportedFeatures.depthBiasClamp;

    enabledFeatures.wideLines                      = m_supportedFeatures.wideLines;

    enabledFeatures.sampleRateShading              = m_supportedFeatures.sampleRateShading;

    enabledFeatures.alphaToOne                     = m_supportedFeatures.alphaToOne;

    enabledFeatures.depthBounds                    = m_supportedFeatures.depthBounds;

    enabledFeatures.logicOp                        = m_supportedFeatures.logicOp;

    enabledFeatures.multiDrawIndirect              = m_supportedFeatures.multiDrawIndirect;

    enabledFeatures.drawIndirectFirstInstance      = m_supportedFeatures.drawIndirectFirstInstance;

    // The current graphics pipeline contract has no patch-control-point state.
    enabledFeatures.tessellationShader = VK_FALSE;

    enabledFeatures.robustBufferAccess =
        m_pRHI->GetDeviceDesc().RobustBufferAccessEnabled() ? m_supportedFeatures.robustBufferAccess : VK_FALSE;

    // Capability queries describe features actually enabled on this logical device.
    m_enabledFeatures           = enabledFeatures;

    deviceInfo.pEnabledFeatures = &m_enabledFeatures;

    const VkResult result       = vkCreateDevice(m_gpu, &deviceInfo, nullptr, &m_device);

    m_pRHI->CheckDeviceLoss(result, "vkCreateDevice");

    RHIError error = MakeVulkanError(result, "vkCreateDevice");
    if (!error.IsFailure())
    {
        ZEN_RHI_LOG_INFO("Vulkan Device Created");

        // display extension info
        for (NameID extension : m_extensions)
        {
            ZEN_RHI_LOG_INFO("Enabled Device Extension: {}", extension.CStr());
        }

        // load device func
        volkLoadDevice(m_device);

        // Keep native compilation data for pipeline recreation during this device's lifetime.
        // Flags stay zero so the driver synchronizes concurrent pipeline creation internally.
        VkPipelineCacheCreateInfo pipelineCacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};

        const std::span<const uint8_t> seed    = m_pRHI->GetDeviceDesc().PipelineCacheSeed();
        const std::span<const uint8_t> payload = CachePayload(seed, m_gpuProps);
        pipelineCacheInfo.initialDataSize      = payload.size();
        pipelineCacheInfo.pInitialData         = payload.data();
        if (!seed.empty())
        {
            ZEN_RHI_LOG_INFO("Vulkan pipeline cache seed: {} of {} bytes accepted", payload.size(), seed.size());
        }
        const VkResult cacheResult = vkCreatePipelineCache(m_device, &pipelineCacheInfo, nullptr, &m_pipelineCache);

        if (cacheResult != VK_SUCCESS)
        {
            m_pipelineCache = VK_NULL_HANDLE;

            if (cacheResult == VK_ERROR_DEVICE_LOST)
            {
                m_pRHI->CheckDeviceLoss(cacheResult, "vkCreatePipelineCache");

                error = MakeVulkanError(cacheResult, "vkCreatePipelineCache");
            }
            ZEN_RHI_LOG_WARN("Vulkan pipeline cache unavailable ({}); creating pipelines without a cache",
                             int32_t(cacheResult));
        }
    }
    if (!error.IsFailure())
    {
        m_pGfxQueue = ZEN_NEW() VulkanQueue(this, queues.graphics.familyIndex, queues.graphics.queueIndex);
        error       = m_pGfxQueue ? m_pGfxQueue->GetCreationError()
                                  : MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate VulkanQueue");
        if (!error.IsFailure())
        {
            m_pComputeQueue = ZEN_NEW() VulkanQueue(this, queues.compute.familyIndex, queues.compute.queueIndex);
            error           = m_pComputeQueue ? m_pComputeQueue->GetCreationError()
                                              : MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate VulkanQueue");
        }
        if (!error.IsFailure())
        {
            m_pTransferQueue = ZEN_NEW() VulkanQueue(this, queues.transfer.familyIndex, queues.transfer.queueIndex);
            error            = m_pTransferQueue ? m_pTransferQueue->GetCreationError()
                                                : MakeVulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Allocate VulkanQueue");
        }
        ZEN_RHI_LOG_INFO("Vulkan queues: graphics={}:{}; compute={}:{}; transfer={}:{}", queues.graphics.familyIndex,
                         queues.graphics.queueIndex, queues.compute.familyIndex, queues.compute.queueIndex,
                         queues.transfer.familyIndex, queues.transfer.queueIndex);

        for (bool transferUsage : {false, true})
        {
            m_queueSharing[transferUsage ? 1 : 0] = MakeQueueSharing(queues.graphics.familyIndex, queues.compute.familyIndex,
                                                                     queues.transfer.familyIndex, transferUsage);
        }
    }
    return error;
}

void VulkanDevice::SetObjectName(VkObjectType type, uint64_t handle, NameID name)
{
    VkDebugUtilsObjectNameInfoEXT info;

    InitVkStruct(info, VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT);

    info.objectType   = type;

    info.objectHandle = handle;

    info.pObjectName  = name.IsNone() ? nullptr : name.CStr();

    if (m_pRHI->GetInstanceExtensionFlags().hasDebugUtils && vkSetDebugUtilsObjectNameEXT != nullptr)
    {
        vkSetDebugUtilsObjectNameEXT(m_device, &info);
    }
}

void VulkanDevice::WaitForIdle()
{
    if (m_device != VK_NULL_HANDLE)
    {
        const VkResult result = TimedRHICall(RHIWorkerTime::eWait, vkDeviceWaitIdle, m_device);

        m_pRHI->CheckDeviceLoss(result, "vkDeviceWaitIdle");

        if (result != VK_SUCCESS)
        {
            ZEN_RHI_VERIFY_MSG_F(result == VK_ERROR_DEVICE_LOST, "Cannot safely tear down after vkDeviceWaitIdle failed: {}",
                                 int32_t(result));

            // Device loss ends the lifetime wait without establishing valid contents/serials.
            ZEN_RHI_LOG_ERROR("Vulkan device idle wait failed: {}", int32_t(result));
        }
        else
        {
            for (uint32_t i = 0; i < ToUnderlying(RHICommandContextType::eMax); i++)
            {
                if (VulkanQueue* queue = GetQueue(static_cast<RHICommandContextType>(i)))
                {
                    queue->ProcessPendingWorkloads(0);
                }
            }
        }
    }
}

VkResult VulkanDevice::AcquireGPUTimingPool(VkQueryPool& pool)
{
    LockAuto lock(&m_timingPoolMutex);

    pool            = VK_NULL_HANDLE;

    VkResult result = VK_SUCCESS;

    if (!m_freeTimingPools.empty())
    {
        pool = m_freeTimingPools.back();

        m_freeTimingPools.pop_back();
    }
    else if (m_timingPools.size() < VulkanGPUTimingRecorder::kMaxPools)
    {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};

        info.queryType  = VK_QUERY_TYPE_TIMESTAMP;

        info.queryCount = 2 * VulkanGPUTimingRecorder::kMaxScopes;

        result          = vkCreateQueryPool(m_device, &info, nullptr, &pool);

        if (result == VK_SUCCESS)
        {
            m_timingPools.push_back(pool);
        }
    }
    else
    {
        result = VK_ERROR_TOO_MANY_OBJECTS;
    }

    return result;
}

void VulkanDevice::ReleaseGPUTimingPool(VkQueryPool pool)
{
    LockAuto lock(&m_timingPoolMutex);

    m_freeTimingPools.push_back(pool);
}

void VulkanDevice::DestroyQueues()
{
    ZEN_DELETE(m_pGfxQueue);

    m_pGfxQueue = nullptr;

    ZEN_DELETE(m_pComputeQueue);

    m_pComputeQueue = nullptr;

    ZEN_DELETE(m_pTransferQueue);

    m_pTransferQueue = nullptr;
}

void VulkanDevice::Destroy()
{
    DestroyQueues();

    if (m_pSemaphoreManager != nullptr)
    {
        m_pSemaphoreManager->Destroy();

        ZEN_DELETE(m_pSemaphoreManager);
    }

    if (m_device != VK_NULL_HANDLE)
    {
        for (VkQueryPool pool : m_timingPools)
        {
            vkDestroyQueryPool(m_device, pool, nullptr);
        }

        m_timingPools.clear();

        m_freeTimingPools.clear();

        if (m_pipelineCache != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);

            m_pipelineCache = VK_NULL_HANDLE;
        }

        vkDestroyDevice(m_device, nullptr);
    }
}

RHIQueueCapabilities VulkanRHI::GetQueueCapabilities() const
{
    return m_pDevice != nullptr ? m_pDevice->GetQueueCapabilities() : RHIQueueCapabilities{};
}

RHIQueueCapabilities VulkanDevice::GetQueueCapabilities() const
{
    RHIQueueCapabilities capabilities;

    if (m_device != VK_NULL_HANDLE)
    {
        const VulkanQueue* compute = GetComputeQueue();

        capabilities.computeSupported =
            compute != nullptr && (GetQueueFamilyProperties(compute->GetFamilyIndex()).queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;

        // Every queue signals a timeline semaphore, so other queues wait for it on the GPU.
        capabilities.asyncSubmissionDependencies = true;

        for (uint32_t i = 0; i < capabilities.queueIds.size(); ++i)
        {
            const VulkanQueue* queue = GetQueue(static_cast<RHICommandContextType>(i));

            capabilities.queueIds[i] = i;

            for (uint32_t j = 0; j < i; ++j)
            {
                const VulkanQueue* previous = GetQueue(static_cast<RHICommandContextType>(j));

                if (queue != nullptr && previous != nullptr && queue->GetVkHandle() == previous->GetVkHandle())
                {
                    capabilities.queueIds[i] = capabilities.queueIds[j];

                    break;
                }
            }
        }
    }

    return capabilities;
}

bool VulkanRHI::PrepareSubmissionDependencies(IRHICommandContext*                      context,
                                              std::span<const RHISubmissionDependency> dependencies)
{
    m_runtime->thread.CheckOwnership();

    if (!m_submissionBlocked)
    {
        SetSubmissionError({});
    }

    bool result                        = context != nullptr && !m_submissionBlocked;

    VulkanCommandListContext* consumer = static_cast<VulkanCommandListContext*>(context);

    for (const RHISubmissionDependency& dependency : dependencies)
    {
        if (result)
        {
            VulkanQueue* producer =
                dependency.queue < RHICommandContextType::eMax ? m_pDevice->GetQueue(dependency.queue) : nullptr;

            const uint64_t submitted = producer != nullptr ? producer->GetLastSubmittedSerial() : 0;

            const uint64_t serial =
                dependency.serial == RHISubmissionDependency::kLatestSubmitted ? submitted : dependency.serial;

            result = producer != nullptr && serial <= submitted;

            if (result && serial != 0 && producer->GetLastCompletedSerial() < serial
                && producer->GetVkHandle() != consumer->GetQueue()->GetVkHandle())
            {
                consumer->AddWaitSemaphore(VkPipelineStageFlags(RHISubmissionDependency::kWaitStage),
                                           producer->m_pTimelineSemaphore, serial);
            }
        }
    }

    if (!result)
    {
        ZEN_RHI_LOG_ERROR("Vulkan submission dependency is invalid or failed");

        const RHIError error = GetSubmissionFailureCause(
            MakeRHIError(RHIErrorCode::eInvalidArgument, "Vulkan submission dependency", __FILE__, __LINE__));

        SetSubmissionError(error);

        BlockSubmissions(error);
    }

    return result;
}

uint64_t VulkanRHI::GetLastSubmittedSerial(RHICommandContextType contextType) const
{
    VulkanQueue* pQueue = m_pDevice->GetQueue(contextType);

    return pQueue != nullptr ? pQueue->GetLastSubmittedSerial() : 0;
}

uint64_t VulkanRHI::QueryLastCompletedSerial(RHICommandContextType contextType)
{
    VulkanQueue* pQueue = m_pDevice->GetQueue(contextType);

    if (pQueue != nullptr && !m_submissionBlocked)
    {
        pQueue->ProcessPendingWorkloads(0);
    }

    return pQueue != nullptr ? pQueue->GetLastCompletedSerial() : 0;
}

RHIWaitResult VulkanRHI::WaitForCompletion(RHICommandContextType contextType, uint64_t submissionSerial, uint64_t timeoutNS)
{
    VulkanQueue* pQueue = m_pDevice->GetQueue(contextType);

    return MakeWaitResult(pQueue != nullptr && pQueue->WaitForCompletion(submissionSerial, timeoutNS));
}

void VulkanRHI::WaitDeviceIdle()
{
    if ((m_pDevice != nullptr) && (m_pDevice->GetVkHandle() != VK_NULL_HANDLE))
    {
        m_pDevice->WaitForIdle();
    }
}
} // namespace zen
