#pragma once
#include "Vulkan/VulkanTranslatedBatch.h"
#include <span>
#include "ZenRHI/RHICommandList.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "VulkanExtension.h"
#include "VulkanLifetimeTracker.h"
#include "ZenFoundation/Memory/PagedAllocator.h"
#include "ZenFoundation/Templates/ObjectPool.h"
#include "ZenRHI/DynamicRHI.h"
#include "ZenRHI/RHIGPUFrameTiming.h"

#define ZEN_VK_APP_VERSION VK_MAKE_API_VERSION(0, 1, 0, 0)

namespace zen
{
class VulkanDevice;
class VulkanViewport;
class VulkanDescriptorAllocator;
class VulkanSetLayoutCache;
class VulkanBindlessHeap;
class VulkanUniformRing;
class VulkanShader;
class VulkanTexture;
class VulkanTextureView;
class VulkanBuffer;
class VulkanAccelerationStructure;
class VulkanSampler;
class VulkanPipeline;
class VulkanMemoryAllocator;

template <typename... RESOURCE_TYPES> struct VersatileResourceTemplate;

using VersatileResource = VersatileResourceTemplate<VulkanShader,
                                                    VulkanTexture,
                                                    VulkanTextureView,
                                                    VulkanSampler,
                                                    VulkanBuffer,
                                                    VulkanPipeline,
                                                    VulkanViewport,
                                                    VulkanAccelerationStructure>;

class VulkanRHI : public DynamicRHI
{
public:
    explicit VulkanRHI(const RHIDeviceDesc& desc);

    ~VulkanRHI() override;

    IRHICommandContext* GetCommandContext(RHICommandContextType contextType) override;

    RHIExecutionCounterStorage& GetExecutionCounterStorage()
    {
        return m_executionCounters;
    }

    RHIExecutionCounters GetExecutionCounters() const override
    {
        return m_executionCounters.Read();
    }

    bool PrepareSubmissionDependencies(IRHICommandContext*                      context,
                                       std::span<const RHISubmissionDependency> dependencies) override;

    bool IsInitialized() const
    {
        return m_initialized;
    }
    RHIError InitChecked(std::vector<RHIAdapterInfo>* adapters = nullptr);

    void Destroy() override;

    void BeginFrame() override;

    void BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing) override;

    void EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded) override;

    RHIGPUTimingPtr RegisterNativeGPUFrameRecording(bool included);

    void ReleaseNativeGPUFrameRecording();

    RHIAPIType GetAPIType() override
    {
        return RHIAPIType::eVulkan;
    }

    std::string_view GetName() override
    {
        static const NameID name("VulkanRHI");

        return name;
    }

    VulkanDevice* GetDevice() const
    {
        return m_pDevice;
    }

    VkInstance GetInstance() const
    {
        return m_instance;
    }

    DataFormat GetSupportedDepthFormat() override;

    VkPhysicalDevice GetPhysicalDevice() const;

    VkDevice GetVkDevice() const;

    RHIViewport* CreateViewport(zen::RHIWindowHandle pWindow, uint32_t width, uint32_t height, bool enableVSync) final;

    void DestroyViewport(RHIViewport* pViewport) final;

    RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) final;

    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) final;

    RHIPipeline* CreatePipeline(const RHIRayTracingPipelineCreateInfo& createInfo) final;

    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo) final;

    RHIError SnapshotPipelineCache(size_t byteLimit, std::vector<uint8_t>& bytes) override;

    RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) final;

    RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo) final;

    RHITextureView* CreateTextureView(RHITexture* pBaseTexture, const RHITextureViewCreateInfo& createInfo) final;

    RHIAccelerationStructureBuildSizes GetAccelerationStructureBuildSizes(
        const RHIAccelerationStructureBuildDesc& desc) override;

    RHIAccelerationStructure* CreateAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) override;

    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) final;

    uint64_t GetBufferAllocationSize(const RHIBufferCreateInfo& createInfo) final;

    RHISubmissionResult SubmitCommandLists(std::span<RHICommandList*> lists) final;

    // The two halves of SubmitCommandLists. Translation appends to batch and, on failure, discards only what this
    // call added. Submission enqueues the batch, flushes every queue and consumes the batch.
    RHIStatus TranslateCommandLists(std::span<RHICommandList*> lists, VulkanTranslatedBatch& batch);

    RHISubmissionResult SubmitTranslated(VulkanTranslatedBatch& batch);

    void BlockSubmissions(RHIError error)
    {
        std::lock_guard<std::mutex> lock(m_terminalMutex);

        if (!m_terminalError.IsFailure())
        {
            m_terminalError = error;
        }

        m_submissionBlocked.store(true, std::memory_order_release);
        if (error.code == RHIErrorCode::eDeviceLost)
        {
            m_deviceLost.store(true, std::memory_order_release);
        }
    }

    RHIError GetTerminalError() const override
    {
        RHIError error;
        if (AreSubmissionsBlocked())
        {
            std::lock_guard<std::mutex> lock(m_terminalMutex);
            error = m_terminalError;
        }
        return error;
    }

    bool HasDeviceLoss() const override
    {
        return m_deviceLost;
    }

    RHIError GetLastSubmissionError() const override
    {
        return m_submissionError.IsFailure() ? m_submissionError : GetTerminalError();
    }

    RHIError GetSubmissionFailureCause(const RHIError& fallback) const
    {
        return RHIErrorOr(GetLastSubmissionError(), fallback);
    }

    RHIError GetSubmissionFailureCause() const
    {
        return GetSubmissionFailureCause(MakeRHIError(RHIErrorCode::eBackendFailure, "Vulkan submission", __FILE__, __LINE__));
    }

    void SetSubmissionError(RHIError error)
    {
        m_submissionError = error;
    }

    bool AreSubmissionsBlocked() const final
    {
        return m_submissionBlocked;
    }

    RHIQueueCapabilities GetQueueCapabilities() const final;

    uint64_t GetLastSubmittedSerial(RHICommandContextType contextType) const final;

    uint64_t QueryLastCompletedSerial(RHICommandContextType contextType) final;

    RHIWaitResult WaitForCompletion(RHICommandContextType contextType,
                                    uint64_t              submissionSerial,
                                    uint64_t              timeoutNS = UINT64_MAX) final;

    void WaitDeviceIdle() final;

    const RHIGPUInfo& QueryGPUInfo() const final;

    const RHIDeviceDesc& GetDeviceDesc() const final
    {
        return m_desc;
    }

    RHIDeviceRuntime& GetRuntime() final
    {
        return *m_runtime;
    }

    uint64_t GetNativeObject(RHINativeObject object) const final;

    // Blocks submissions and reports device diagnostics when result is VK_ERROR_DEVICE_LOST.
    void CheckDeviceLoss(VkResult result, const char* operation);

    // Checks a failed native object creation for device loss and returns it as an error. Creation failures
    // stay separate from submission failures.
    [[nodiscard]] RHIError ReportCreationFailure(VkResult result, const char* operation, const char* source, uint32_t line);

    RHIGPUMemoryStats GetGPUMemoryStats() const final;

    RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const final;

    RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const final;

    PagedAllocator<VersatileResource>& GetResourceAllocator()
    {
        return m_resourceAllocator;
    }

    VulkanMemoryAllocator* GetMemoryAllocator() const
    {
        return m_pMemoryAllocator;
    }

    VulkanDescriptorAllocator* GetDescriptorAllocator() const
    {
        return m_pDescriptorAllocator;
    }

    VulkanBindlessHeap* GetBindlessHeap() const
    {
        return m_pBindlessHeap;
    }

    VulkanSetLayoutCache* GetSetLayoutCache() const
    {
        return m_pSetLayoutCache;
    }

    RHIBindlessHandle RegisterBindlessResource(RHIResource* pResource, uint32_t slotIndex = kInvalidBindlessSlotIndex) override;

    bool UnregisterBindlessResource(RHIBindlessHandle handle) override;

    bool IsBindlessResourceRegistered(RHIBindlessHandle handle) override;

    void CollectRetiredBindlessResources() override;

    bool ResetBindlessResources() override;

    VulkanUniformRing* GetUniformRing() const
    {
        return m_pUniformRing;
    }

    VulkanLifetimeTracker& GetLifetimeTracker()
    {
        return m_lifetimeTracker;
    }

    InstanceExtensionFlags& GetInstanceExtensionFlags()
    {
        return m_instanceExtensionFlags;
    }

protected:
    RHIError CreateInstance();
    void     PopulateGPUInfo();

private:
    friend struct VulkanRHITestAccess;

    RHIDeviceDesc              m_desc;
    RHIDeviceRuntimePtr        m_runtime{MakeRefCountPtr<RHIDeviceRuntime>()};
    RHIExecutionCounterStorage m_executionCounters;

    RHIError SetupInstanceLayers(VulkanInstanceExtensionArray& instanceExtensions);

    RHIError SetupInstanceExtensions(VulkanInstanceExtensionArray& instanceExtensions);


    void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& dbgMessengerCI);

    RHIError          SelectGPU(std::vector<RHIAdapterInfo>* adapters);
    VkResult          m_loaderResult{};
    bool              m_initialized{};
    VkLayerProperties m_validationLayer{};

    VkInstance               m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_messenger{VK_NULL_HANDLE};

    HeapVector<NameID> m_instanceLayers;
    HeapVector<NameID> m_instanceExtensions;

    InstanceExtensionFlags m_instanceExtensionFlags{};

    VulkanDevice* m_pDevice{nullptr};

    RHIGPUInfo m_gpuInfo{};

    VulkanDescriptorAllocator* m_pDescriptorAllocator{nullptr};
    VulkanBindlessHeap*        m_pBindlessHeap{nullptr};
    VulkanSetLayoutCache*      m_pSetLayoutCache{nullptr};
    VulkanUniformRing*         m_pUniformRing{nullptr};
    VulkanMemoryAllocator*     m_pMemoryAllocator{nullptr};

    PagedAllocator<VersatileResource> m_resourceAllocator;

    VulkanTranslatedBatch m_translatedBatch; // Reused by SubmitCommandLists.

    mutable std::mutex    m_terminalMutex;
    std::atomic<bool>     m_submissionBlocked{false};
    RHIError              m_terminalError{};
    RHIError              m_submissionError{};
    std::atomic<bool>     m_deviceLost{false};
    RHIGPUFrameTimingPtr  m_gpuFrameTiming;
    uint32_t              m_pendingNativeRecordings{0};
    VulkanLifetimeTracker m_lifetimeTracker;
};

} // namespace zen
