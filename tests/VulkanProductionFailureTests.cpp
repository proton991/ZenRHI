#include "ZenRHI/Vulkan/VulkanInterop.h"
#include "TestShaderModule.h"
#include "VulkanIntegrationFixture.h"
#include "Vulkan/VulkanDescriptorAllocator.h"
#include "Vulkan/VulkanSetLayout.h"
#include "ZenRHI/TestSupport/ScopedVulkanCall.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanMemory.h"
#include "Vulkan/VulkanDescriptorAllocator.h"
#include "Vulkan/VulkanBindlessHeap.h"
#include "Vulkan/VulkanSynchronization.h"
#include "Vulkan/VulkanCommandList.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include <gtest/gtest.h>
#include <filesystem>
#include "ZenRHI/TestSupport/Validation.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanQueue.h"

namespace
{
using namespace zen;

enum class StartupFault
{
    eInstance,
    eDevice
};
StartupFault          startupFault;
PFN_vkDestroyInstance originalDestroyInstance;
PFN_vkDestroyDevice   originalDestroyDevice;
uint32_t              destroyedInstances;
uint32_t              destroyedDevices;

VKAPI_ATTR void VKAPI_CALL CountDestroyedInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
    ++destroyedInstances;
    originalDestroyInstance(instance, allocator);
}

VKAPI_ATTR void VKAPI_CALL CountDestroyedDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    ++destroyedDevices;
    originalDestroyDevice(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL FailEnumeration(VkInstance, uint32_t*, VkPhysicalDevice*)
{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL RejectBindless(VkDevice,
                                          const VkDescriptorSetLayoutCreateInfo*,
                                          VkDescriptorSetLayoutSupport* support)
{
    support->supported = VK_FALSE;
}

class LateReleaseResource : public RHIResource
{
public:
    LateReleaseResource() : RHIResource(RHIResourceType::eSampler) {}

    uint32_t destroyCalls{0};

protected:
    void Destroy() override
    {
        ++destroyCalls;
    }
};

void InjectStartupFault(const RHIMessage& diagnostic) noexcept
{
    if (diagnostic.text.starts_with("Active validation layer="))
    {
        originalDestroyInstance = vkDestroyInstance;
        vkDestroyInstance       = CountDestroyedInstance;
        if (startupFault == StartupFault::eInstance)
        {
            vkEnumeratePhysicalDevices = FailEnumeration;
        }
    }
    if (startupFault == StartupFault::eDevice && diagnostic.text.starts_with("Vulkan queues:"))
    {
        originalDestroyDevice           = vkDestroyDevice;
        vkDestroyDevice                 = CountDestroyedDevice;
        vkGetDescriptorSetLayoutSupport = RejectBindless;
    }
}

TEST(VulkanProductionFailureTest, CheckedStartupRestoresHostAndRetries)
{
    const RHIMessageCallback previous = GetRHIMessageCallback();
    RHIDeviceDesc            desc     = test::MakeTestDeviceDesc();
    SetRHIMessageCallback(InjectStartupFault);
    desc.SetPreferredDevice("startup retry test");
    // Force the same physical adapter as the rest of this hardware run.
    const char* preferred = std::getenv("ZEN_VULKAN_DEVICE");
    if (preferred && *preferred)
    {
        desc.SetPreferredDevice(preferred);
    }
    for (StartupFault fault : {StartupFault::eInstance, StartupFault::eDevice})
    {
        destroyedInstances = destroyedDevices = 0;
        startupFault                          = fault;
        RHIResult<DynamicRHI*> failed         = CreateRHIChecked(desc);
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.GetError().nativeCode,
                  fault == StartupFault::eInstance ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(destroyedInstances, 1u);
        EXPECT_EQ(destroyedDevices, fault == StartupFault::eDevice ? 1u : 0u);
    }
    desc.surfaceExtensions = []() -> std::span<const char* const> {
        static constexpr const char* names[] = {"VK_ZEN_unavailable_host_surface"};
        return names;
    };
    SetRHIMessageCallback(previous);
    RHIResult<DynamicRHI*> missing = CreateRHIChecked(desc);
    if (missing)
    {
        missing.GetValue()->Destroy();
        ZEN_DELETE(missing.GetValue());
    }
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.GetError().nativeCode, VK_ERROR_EXTENSION_NOT_PRESENT);

    RHIResult<DynamicRHI*> created = CreateRHIChecked(test::MakeTestDeviceDesc());
    ASSERT_TRUE(created);
    EXPECT_NE(GetVulkanDeviceHandles(*created.GetValue()).device, VK_NULL_HANDLE);
    created.GetValue()->Destroy();
    ZEN_DELETE(created.GetValue());
}

TEST(VulkanProductionFailureTest, SecondBackendIsRefusedWhileOneIsLive)
{
    RHIResult<DynamicRHI*> created = CreateRHIChecked(test::MakeTestDeviceDesc());
    ASSERT_TRUE(created);

    // The loader and the device entry points are process-wide, so neither call may unload or replace them.
    RHIResult<std::vector<RHIAdapterInfo>> adapters = EnumerateRHIAdapters(test::MakeTestDeviceDesc());
    RHIResult<DynamicRHI*>                 second   = CreateRHIChecked(test::MakeTestDeviceDesc());
    ASSERT_FALSE(adapters);
    ASSERT_FALSE(second);
    EXPECT_EQ(adapters.GetError().code, RHIErrorCode::eInvalidArgument);
    EXPECT_EQ(second.GetError().code, RHIErrorCode::eInvalidArgument);

    created.GetValue()->WaitDeviceIdle();
    created.GetValue()->Destroy();
    ZEN_DELETE(created.GetValue());

    EXPECT_TRUE(EnumerateRHIAdapters(test::MakeTestDeviceDesc()));
}

TEST(VulkanProductionFailureTest, StandaloneTeardownLeaksLateReleases)
{
    const bool             strict  = GetRHIStrictTeardownChecks();
    RHIResult<DynamicRHI*> created = CreateRHIChecked(test::MakeTestDeviceDesc());
    ASSERT_TRUE(created);

    LateReleaseResource* late = ZEN_NEW() LateReleaseResource();
    late->BindRuntime(&created.GetValue()->GetRuntime());

    SetRHIStrictTeardownChecks(false);
    created.GetValue()->Destroy();
    ZEN_DELETE(created.GetValue());

    // Without an executor the backend closes admission itself, so the release never reaches the deleted backend.
    EXPECT_EQ(late->ReleaseReference(), 0u);
    EXPECT_EQ(late->destroyCalls, 0u);

    SetRHIStrictTeardownChecks(strict);
    ZEN_DELETE(late);
}

struct PipelineFailure
{
    static inline PFN_vkCreateGraphicsPipelines graphics;
    static inline PFN_vkCreateComputePipelines  compute;
    static inline PFN_vkDestroyPipeline         destroy;
    static inline bool                          partial;
    static inline uint32_t                      destroyed;

    static VKAPI_ATTR VkResult VKAPI_CALL Graphics(VkDevice                            device,
                                                   VkPipelineCache                     cache,
                                                   uint32_t                            count,
                                                   const VkGraphicsPipelineCreateInfo* infos,
                                                   const VkAllocationCallbacks*        allocator,
                                                   VkPipeline*                         pipelines)
    {
        if (partial)
        {
            EXPECT_EQ(graphics(device, cache, count, infos, allocator, pipelines), VK_SUCCESS);
        }

        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Compute(VkDevice                           device,
                                                  VkPipelineCache                    cache,
                                                  uint32_t                           count,
                                                  const VkComputePipelineCreateInfo* infos,
                                                  const VkAllocationCallbacks*       allocator,
                                                  VkPipeline*                        pipelines)
    {
        if (partial)
        {
            EXPECT_EQ(compute(device, cache, count, infos, allocator, pipelines), VK_SUCCESS);
        }

        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            ++destroyed;
        }

        destroy(device, pipeline, allocator);
    }
};

void AddProductionStage(RHIShaderCreateInfo& info, RHIShaderStage stage, const char* file)
{
    info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

    zen::test::LoadShaderModule(info, stage, std::filesystem::path(ZENRHI_TEST_SHADER_DIR) / file);
}

TEST(VulkanProductionFailureTest, GraphicsAndComputeFailuresCleanPartialOutputsAndAllowRetry)
{
    test::VulkanSession session;

    PipelineFailure::graphics = vkCreateGraphicsPipelines;

    PipelineFailure::compute  = vkCreateComputePipelines;

    PipelineFailure::destroy  = vkDestroyPipeline;

    test::ScopedVulkanCall<PFN_vkDestroyPipeline> destruction(vkDestroyPipeline, PipelineFailure::Destroy);

    for (bool compute : {false, true})
    {
        RHIShaderCreateInfo shaderInfo{};

        if (compute)
        {
            AddProductionStage(shaderInfo, RHIShaderStage::eCompute, "pipeline_specialization.comp.spv");
        }
        else
        {
            AddProductionStage(shaderInfo, RHIShaderStage::eVertex, "pipeline.vert.spv");

            AddProductionStage(shaderInfo, RHIShaderStage::eFragment, "pipeline.frag.spv");
        }

        RHIShader* shader = session.rhi.CreateShader(shaderInfo);

        ASSERT_NE(shader, nullptr);

        RHIRenderingLayout layout{};

        layout.SetRenderArea(0, 0, 4, 4);

        layout.numColorRenderTargets        = 1;

        layout.colorRenderTargets[0].format = DataFormat::eR8G8B8A8UNORM;

        RHIGfxPipelineCreateInfo graphicsInfo{};

        graphicsInfo.pShader          = shader;

        graphicsInfo.pRenderingLayout = &layout;

        graphicsInfo.states.colorBlendState.AddAttachment();

        RHIComputePipelineCreateInfo computeInfo{};

        computeInfo.pShader = shader;

        for (bool partial : {false, true})
        {
            PipelineFailure::partial   = partial;

            PipelineFailure::destroyed = 0;

            test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines> graphicsFailure(vkCreateGraphicsPipelines,
                                                                                  PipelineFailure::Graphics);

            test::ScopedVulkanCall<PFN_vkCreateComputePipelines> computeFailure(vkCreateComputePipelines,
                                                                                PipelineFailure::Compute);

            EXPECT_EQ(compute ? session.rhi.CreatePipeline(computeInfo) : session.rhi.CreatePipeline(graphicsInfo), nullptr);

            EXPECT_EQ(PipelineFailure::destroyed, partial ? 1u : 0u);
        }

        RHIPipeline* retry = compute ? session.rhi.CreatePipeline(computeInfo) : session.rhi.CreatePipeline(graphicsInfo);

        ASSERT_NE(retry, nullptr);

        session.rhi.DestroyPipeline(retry);

        session.rhi.DestroyShader(shader);
    }
}

TEST(VulkanProductionFailureTest, InvalidGraphicsLayoutReturnsNull)
{
    test::VulkanSession session;

    RHIGfxPipelineCreateInfo info{};

    EXPECT_EQ(session.rhi.CreatePipeline(info), nullptr);

    RHIShaderCreateInfo shaderInfo{};

    AddProductionStage(shaderInfo, RHIShaderStage::eVertex, "pipeline.vert.spv");

    AddProductionStage(shaderInfo, RHIShaderStage::eFragment, "pipeline.frag.spv");

    RHIShader* shader = session.rhi.CreateShader(shaderInfo);

    ASSERT_NE(shader, nullptr);

    RHIRenderingLayout layout{};

    info.pShader                 = shader;

    info.pRenderingLayout        = &layout;

    layout.numColorRenderTargets = MAX_NUM_COLOR_ATTACHMENTS + 1;

    EXPECT_EQ(session.rhi.CreatePipeline(info), nullptr);

    layout.numColorRenderTargets        = 1;

    layout.colorRenderTargets[0].format = DataFormat::eUndefined;

    EXPECT_EQ(session.rhi.CreatePipeline(info), nullptr);

    session.rhi.DestroyShader(shader);
}

TEST(VulkanProductionFailureTest, BufferAllocationAndMappingFailureReturnNullWithoutLeaks)
{
    test::VulkanSession session;

    for (bool mapping : {false, true})
    {
        VulkanMemoryAllocator allocator(&session.rhi);

        const PFN_vkAllocateMemory allocate = vkAllocateMemory;

        test::ScopedVulkanCall<PFN_vkAllocateMemory> allocationFailure(
            vkAllocateMemory, [](VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*) {
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            });

        test::ScopedVulkanCall<PFN_vkMapMemory> mappingFailure(vkMapMemory, [](VkDevice, VkDeviceMemory, VkDeviceSize,
                                                                               VkDeviceSize, VkMemoryMapFlags,
                                                                               void**) { return VK_ERROR_MEMORY_MAP_FAILED; });

        // VMA captures these function pointers at initialization. Restore allocation
        // for the mapping case so the failure happens after native memory is created.
        if (mapping)
        {
            vkAllocateMemory = allocate;
        }

        allocator.Init(session.rhi.GetInstance(), session.rhi.GetDevice()->GetPhysicalDeviceHandle(), session.rhi.GetVkDevice(),
                       {});

        VulkanMemoryAllocator* original = VulkanRHITestAccess::ReplaceMemoryAllocator(session.rhi, &allocator);

        RHIBufferCreateInfo info{};

        info.size         = 4096;

        info.allocateType = RHIBufferAllocateType::eCPUWrite;

        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);

        EXPECT_EQ(session.rhi.CreateBuffer(info), nullptr);
        EXPECT_EQ(allocator.GetGPUMemoryStats().hostUploadAllocationBytes, 0u);
        EXPECT_EQ(allocator.GetGPUMemoryStats().peakHostUploadAllocationBytes, 0u);

        // A failed mapping may leave an empty VMA block cached; allocator teardown
        // reports allocation bytes and must release every native commitment.
        if (!mapping)
        {
            EXPECT_EQ(allocator.GetGPUMemoryStats().committedBytes, 0u);
        }

        VulkanRHITestAccess::ReplaceMemoryAllocator(session.rhi, original);
    }
}

TEST(VulkanProductionFailureTest, HostUploadAllocationStatisticsTrackNativeResourceLifetime)
{
    test::VulkanSession     session;
    const RHIGPUMemoryStats before = session.rhi.GetGPUMemoryStats();
    ASSERT_TRUE(before.available);
    RHIBufferCreateInfo info{};
    info.size         = 17;
    info.allocateType = RHIBufferAllocateType::eCPUWrite;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* upload = session.rhi.CreateBuffer(info);
    ASSERT_NE(upload, nullptr);
    const uint64_t bytes = upload->GetAllocationSize();
    EXPECT_GE(bytes, info.size);
    EXPECT_EQ(session.rhi.GetGPUMemoryStats().hostUploadAllocationBytes, before.hostUploadAllocationBytes + bytes);
    EXPECT_GE(session.rhi.GetGPUMemoryStats().peakHostUploadAllocationBytes, before.hostUploadAllocationBytes + bytes);
    upload->AddReference();
    session.rhi.DestroyBuffer(upload);
    // Releasing the pool owner is insufficient while a recorded user retains it.
    EXPECT_EQ(session.rhi.GetGPUMemoryStats().hostUploadAllocationBytes, before.hostUploadAllocationBytes + bytes);
    upload->ReleaseReference();
    EXPECT_EQ(session.rhi.GetGPUMemoryStats().hostUploadAllocationBytes, before.hostUploadAllocationBytes);
    EXPECT_GE(session.rhi.GetGPUMemoryStats().peakHostUploadAllocationBytes, before.hostUploadAllocationBytes + bytes);
    info.allocateType   = RHIBufferAllocateType::eCPURead;
    RHIBuffer* readback = session.rhi.CreateBuffer(info);
    ASSERT_NE(readback, nullptr);
    EXPECT_EQ(session.rhi.GetGPUMemoryStats().hostUploadAllocationBytes, before.hostUploadAllocationBytes);
    session.rhi.DestroyBuffer(readback);
}

TEST(VulkanProductionFailureTest, ShaderStagesLayoutsAndSamplerRejectNativeFailure)
{
    test::VulkanSession session;

    RHIShaderCreateInfo shaderInfo{};

    AddProductionStage(shaderInfo, RHIShaderStage::eCompute, "descriptor_uniform.comp.spv");
    {
        test::ScopedVulkanCall<PFN_vkCreateShaderModule> fail(
            vkCreateShaderModule, [](VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            });

        EXPECT_EQ(session.rhi.CreateShader(shaderInfo), nullptr);
    }

    {
        test::ScopedVulkanCall<PFN_vkCreateDescriptorSetLayout> fail(
            vkCreateDescriptorSetLayout, [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*,
                                            VkDescriptorSetLayout*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        EXPECT_EQ(session.rhi.CreateShader(shaderInfo), nullptr);
    }

    {
        test::ScopedVulkanCall<PFN_vkCreatePipelineLayout> fail(
            vkCreatePipelineLayout, [](VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*,
                                       VkPipelineLayout*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        EXPECT_EQ(session.rhi.CreateShader(shaderInfo), nullptr);
    }

    {
        test::ScopedVulkanCall<PFN_vkCreateSampler> fail(vkCreateSampler,
                                                         [](VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*,
                                                            VkSampler*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        EXPECT_EQ(session.rhi.CreateSampler({}), nullptr);
    }

    RHIShader* retry = session.rhi.CreateShader(shaderInfo);

    ASSERT_NE(retry, nullptr);

    session.rhi.DestroyShader(retry);
}

TEST(VulkanProductionFailureTest, DescriptorPoolFailureHandsOutAndPublishesNoSet)
{
    test::VulkanSession session;

    RHIShaderResourceDescriptor descriptor{};
    descriptor.type               = RHIShaderResourceType::eStorageBuffer;
    descriptor.arraySize          = 1;
    descriptor.stageFlags         = RHIShaderStageFlagBits::eCompute;
    const VulkanSetLayout* layout = session.rhi.GetSetLayoutCache()->GetOrCreate({&descriptor, 1});
    ASSERT_NE(layout, nullptr);
    const VulkanDescriptorAllocator::Key key{layout->id, {1}};
    VulkanDescriptorAllocator            allocator(session.rhi.GetDevice());
    {
        test::ScopedVulkanCall<PFN_vkCreateDescriptorPool> fail(
            vkCreateDescriptorPool, [](VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*,
                                       VkDescriptorPool*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        const VulkanDescriptorAllocator::Set set = allocator.Insert(key, *layout, {});

        EXPECT_EQ(set.handle, VK_NULL_HANDLE);

        EXPECT_EQ(set.lifetimeId, 0u);

        EXPECT_EQ(set.error.code, RHIErrorCode::eOutOfDeviceMemory);
    }

    EXPECT_EQ(allocator.Find(key).handle, VK_NULL_HANDLE);

    EXPECT_NE(allocator.Insert(key, *layout, {}).handle, VK_NULL_HANDLE);

    EXPECT_NE(allocator.Find(key).handle, VK_NULL_HANDLE);
}

TEST(VulkanProductionFailureTest, FailedCommandPoolReturnsNoContextAndAllowsRetry)
{
    test::VulkanSession session;
    {
        test::ScopedVulkanCall<PFN_vkCreateCommandPool> fail(
            vkCreateCommandPool, [](VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            });

        EXPECT_EQ(session.rhi.GetCommandContext(RHICommandContextType::eGraphics), nullptr);

        EXPECT_EQ(RHICommandList::Create(nullptr), nullptr);
    }

    IRHICommandContext* retry = session.rhi.GetCommandContext(RHICommandContextType::eGraphics);

    ASSERT_NE(retry, nullptr);

    ZEN_DELETE(retry);
}

TEST(VulkanProductionFailureTest, AbandonedRecordingRollsBackOnlyItsOwnBindlessRegistration)
{
    test::VulkanSession session;

    VulkanBindlessHeap* manager = session.rhi.GetBindlessHeap();

    RHISampler* retained        = session.rhi.CreateSampler({});

    RHISampler* abandoned       = session.rhi.CreateSampler({});

    ASSERT_NE(retained, nullptr);

    ASSERT_NE(abandoned, nullptr);

    RHIBindlessHandle published;

    ASSERT_TRUE(manager->RegisterBindlessResource(retained, 0, &published));

    VulkanCommandListContext* context =
        static_cast<VulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

    const uint64_t transaction = context->GetRecordingTransaction();

    ASSERT_TRUE(manager->RegisterBindlessResource(abandoned, 1, nullptr, 0, transaction));

    EXPECT_EQ(abandoned->GetRefCount(), 2u);

    ZEN_DELETE(context);

    EXPECT_EQ(abandoned->GetRefCount(), 1u);

    EXPECT_TRUE(manager->IsRegistered(published));

    EXPECT_EQ(retained->GetRefCount(), 2u);

    EXPECT_TRUE(manager->UnregisterBindlessResource(published));

    session.rhi.DestroySampler(abandoned);

    session.rhi.DestroySampler(retained);
}

struct RecordingFailureProbe
{
    static inline uint32_t submissions{0};
    static inline uint32_t dispatches{0};

    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)
    {
        ++submissions;

        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    static VKAPI_ATTR void VKAPI_CALL Dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t)
    {
        ++dispatches;
    }
};

TEST(VulkanProductionFailureTest, MemoryBudgetReportsNativeHeapsWhenSupported)
{
    test::VulkanSession session;

    const RHIGPUMemoryStats stats = session.rhi.GetGPUMemoryStats();

    EXPECT_TRUE(stats.available);

    EXPECT_EQ(stats.budgetAvailable, session.rhi.GetDevice()->GetExtensionFlags().hasMemoryBudget != 0);

    if (stats.budgetAvailable)
    {
        EXPECT_GT(stats.heapCount, 0u);

        EXPECT_LE(stats.heapCount, stats.heaps.size());

        for (uint32_t i = 0; i < stats.heapCount; ++i)
        {
            EXPECT_GT(stats.heaps[i].sizeBytes, 0u);

            EXPECT_GT(stats.heaps[i].budgetBytes, 0u);
        }
    }
}

// The diagnostic limit emulates a smaller GPU (an 8 GB card) on a larger one: device-local heaps,
// their budgets and the allocator's capacity all stop at it, so budget-driven choices see that device.
TEST(VulkanProductionFailureTest, DeviceMemoryLimitCapsHeapsBudgetsAndAllocations)
{
    test::VulkanSession session;
    const bool          memoryBudget = session.rhi.GetDevice()->GetExtensionFlags().hasMemoryBudget != 0;
    const uint64_t      eightGB      = 8ull * 1024 * 1024 * 1024;
    {
        VulkanMemoryAllocator allocator(&session.rhi);
        allocator.Init(session.rhi.GetInstance(), session.rhi.GetDevice()->GetPhysicalDeviceHandle(), session.rhi.GetVkDevice(),
                       {false, memoryBudget, eightGB});
        const RHIGPUMemoryStats stats = allocator.GetGPUMemoryStats();
        EXPECT_EQ(stats.budgetAvailable, memoryBudget);
        for (uint32_t i = 0; i < stats.heapCount; ++i)
        {
            if (stats.heaps[i].deviceLocal)
            {
                EXPECT_LE(stats.heaps[i].sizeBytes, eightGB) << i;
                EXPECT_LE(stats.heaps[i].budgetBytes, eightGB) << i;
            }
        }
    }

    // A small limit shows the cap is enforced: device-local commitments stop at it. Like a full card,
    // an allocation that prefers device memory may then fail or be placed in host memory.
    const uint64_t        limit = 256ull * 1024 * 1024;
    VulkanMemoryAllocator allocator(&session.rhi);
    allocator.Init(session.rhi.GetInstance(), session.rhi.GetDevice()->GetPhysicalDeviceHandle(), session.rhi.GetVkDevice(),
                   {false, false, limit});
    VulkanMemoryAllocator* original = VulkanRHITestAccess::ReplaceMemoryAllocator(session.rhi, &allocator);
    RHIBufferCreateInfo    info{};
    info.size         = limit / 2;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    RHIBuffer* first = session.rhi.CreateBuffer(info);
    EXPECT_NE(first, nullptr);
    EXPECT_GE(allocator.GetGPUMemoryStats().deviceLocalBytes, limit / 2);
    info.size                               = limit;
    RHIBuffer*                       second = session.rhi.CreateBuffer(info);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(session.rhi.GetPhysicalDevice(), &memory);
    uint64_t cappedDeviceBytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
    {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            cappedDeviceBytes += std::min<uint64_t>(limit, memory.memoryHeaps[i].size);
        }
    }
    EXPECT_LE(allocator.GetGPUMemoryStats().deviceLocalBytes, cappedDeviceBytes);
    if (first != nullptr)
    {
        session.rhi.DestroyBuffer(first);
    }
    if (second != nullptr)
    {
        session.rhi.DestroyBuffer(second);
    }
    VulkanRHITestAccess::ReplaceMemoryAllocator(session.rhi, original);
}

TEST(VulkanProductionFailureTest, ShutdownRejectsOutstandingResourcesAndCommandContexts)
{
    EXPECT_DEATH(
        {
            SetRHIStrictTeardownChecks(true);

            test::VulkanSession session;

            session.rhi.GetCommandContext(RHICommandContextType::eGraphics);

            session.rhi.Destroy();
        },
        "All RHI command contexts must be released before queue teardown");

    EXPECT_DEATH(
        {
            SetRHIStrictTeardownChecks(true);

            test::VulkanSession session;

            RHIBufferCreateInfo info{};

            info.allocateType = RHIBufferAllocateType::eGPU;

            info.size         = 16;

            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

            session.rhi.CreateBuffer(info);

            session.rhi.Destroy();
        },
        "All RHI resources must be released before backend teardown");
}

#if defined(NDEBUG)
// Debug VMA asserts on leaked allocations, so only Release can observe teardown continuing.
// The deliberate leak would also be reported by validation at device destruction.
TEST(VulkanProductionFailureTest, NonStrictShutdownLogsOutstandingResourcesAndContinues)
{
    EXPECT_EXIT(
        {
            SetRHIStrictTeardownChecks(false);

            RHIDeviceDesc desc = test::MakeTestDeviceDesc();

            desc.SetValidationEnabled(false);

            test::VulkanSession session(desc);

            RHIBufferCreateInfo info{};

            info.allocateType = RHIBufferAllocateType::eGPU;

            info.size         = 16;

            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

            session.rhi.CreateBuffer(info);

            session.rhi.Destroy();

            std::fprintf(stderr, "teardown continued\n");

            std::fflush(stderr);

            std::_Exit(0);
        },
        testing::ExitedWithCode(0), "teardown continued");
}
#endif

class VulkanProductionDiagnosticsTest : public testing::TestWithParam<bool>
{};

TEST_P(VulkanProductionDiagnosticsTest, DeviceLossReportsCompletedGpuBreadcrumbAndDriverFault)
{
    RHIDeviceDesc desc = test::MakeTestDeviceDesc();

    desc.SetDeviceLossDiagnostics(true);

    test::VulkanSession session(desc);

    if (GetParam())
    {
        session.rhi.GetDevice()->GetExtensionFlags().hasBufferMarker = 0;
    }

    VulkanCommandListContext* context =
        static_cast<VulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

    context->RHIBeginDebugLabel("production_diagnostic_pass");

    context->RHIEndDebugLabel();

    ASSERT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);

    session.rhi.WaitDeviceIdle();

    // Exercise device-fault reporting without deliberately losing the user's GPU.
    session.rhi.GetDevice()->GetExtensionFlags().hasDeviceFault = 1;

    test::ScopedVulkanCall<PFN_vkGetDeviceFaultInfoEXT> fault(
        vkGetDeviceFaultInfoEXT, [](VkDevice, VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
            counts->addressInfoCount = 0;

            counts->vendorInfoCount  = 0;

            if (info)
            {
                std::strcpy(info->description, "injected driver fault");
            }

            return VK_SUCCESS;
        });

    VulkanCommandListContext* active =
        static_cast<VulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));
    const std::string label(180, 'x');
    for (uint32_t i = 0; i < 100; ++i)
    {
        active->RHIBeginDebugLabel(label);
    }
    testing::internal::CaptureStderr();

    session.rhi.CheckDeviceLoss(VK_ERROR_DEVICE_LOST, "injected submission");

    const std::string diagnostic = testing::internal::GetCapturedStderr();

    EXPECT_NE(diagnostic.find("last_started=production_diagnostic_pass"), std::string::npos);

    EXPECT_NE(diagnostic.find("last_completed=production_diagnostic_pass"), std::string::npos);

    EXPECT_NE(diagnostic.find("injected driver fault"), std::string::npos);

    EXPECT_NE(diagnostic.find("[report truncated]"), std::string::npos);
    EXPECT_TRUE(session.rhi.AreSubmissionsBlocked());
    for (uint32_t i = 0; i < 100; ++i)
    {
        active->RHIEndDebugLabel();
    }
    ZEN_DELETE(active);
    ZEN_DELETE(context);
}

INSTANTIATE_TEST_SUITE_P(NativeAndPortable, VulkanProductionDiagnosticsTest, testing::Bool());

TEST(VulkanProductionFailureTest, FailedCommandRecordingNeverReachesSubmissionAndCanRetry)
{
    test::VulkanSession session;

    for (uint32_t stage = 0; stage < 4; ++stage)
    {
        VulkanCommandListContext* context =
            static_cast<VulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

        ASSERT_NE(context, nullptr);

        // Give reset a real, discarded native recording to reset.
        if (stage == 2)
        {
            ASSERT_TRUE(context->EnsureRecording());

            context->DiscardRecording();
        }

        const PFN_vkAllocateCommandBuffers allocate = vkAllocateCommandBuffers;

        const PFN_vkBeginCommandBuffer begin        = vkBeginCommandBuffer;

        const PFN_vkResetCommandBuffer reset        = vkResetCommandBuffer;

        const PFN_vkEndCommandBuffer end            = vkEndCommandBuffer;

        RecordingFailureProbe::submissions          = 0;
        {
            test::ScopedVulkanCall<PFN_vkAllocateCommandBuffers> failAllocate(
                vkAllocateCommandBuffers,
                stage == 0 ?
                    +[](VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    allocate);

            test::ScopedVulkanCall<PFN_vkBeginCommandBuffer> failBegin(
                vkBeginCommandBuffer,
                stage == 1 ?
                    +[](VkCommandBuffer, const VkCommandBufferBeginInfo*) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    begin);

            test::ScopedVulkanCall<PFN_vkResetCommandBuffer> failReset(
                vkResetCommandBuffer,
                stage == 2 ?
                    +[](VkCommandBuffer, VkCommandBufferResetFlags) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    reset);

            test::ScopedVulkanCall<PFN_vkEndCommandBuffer> failEnd(
                vkEndCommandBuffer,
                stage == 3 ?
                    +[](VkCommandBuffer) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    end);

            test::ScopedVulkanCall<PFN_vkQueueSubmit> countSubmit(vkQueueSubmit, RecordingFailureProbe::Submit);

            context->EnsureRecording();

            EXPECT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eRejected);

            EXPECT_EQ(RecordingFailureProbe::submissions, 0u);
        }

        context->DiscardRecording();

        EXPECT_TRUE(context->EnsureRecording());

        EXPECT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);

        session.rhi.WaitDeviceIdle();

        ZEN_DELETE(context);
    }
}

TEST(VulkanProductionFailureTest, FailedUniformOrDescriptorPreparationSuppressesDispatch)
{
    test::VulkanSession session;

    RHIShaderCreateInfo shaderInfo{};

    AddProductionStage(shaderInfo, RHIShaderStage::eCompute, "descriptor_uniform.comp.spv");

    RHIShader* shader = session.rhi.CreateShader(shaderInfo);

    ASSERT_NE(shader, nullptr);

    RHIPipeline* pipeline = session.rhi.CreatePipeline(RHIComputePipelineCreateInfo{shader});

    ASSERT_NE(pipeline, nullptr);

    for (bool undersized : {true, false})
    {
        VulkanCommandListContext* context =
            static_cast<VulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

        RHIBufferCreateInfo bufferInfo{};

        bufferInfo.size         = undersized ? 4 : 16;

        bufferInfo.allocateType = RHIBufferAllocateType::eCPUWrite;

        bufferInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eUniformBuffer);

        RHIBuffer* buffer = session.rhi.CreateBuffer(bufferInfo);

        ASSERT_NE(buffer, nullptr);

        RHIBatchedShaderParameters parameters;

        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), buffer, nullptr, 0);

        context->RHIBindPipeline(pipeline);

        context->RHISetShaderParameters(parameters);

        RecordingFailureProbe::dispatches  = 0;

        RecordingFailureProbe::submissions = 0;
        {
            test::ScopedVulkanCall<PFN_vkAllocateDescriptorSets> fail(
                vkAllocateDescriptorSets,
                [](VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

            test::ScopedVulkanCall<PFN_vkCmdDispatch> countDispatch(vkCmdDispatch, RecordingFailureProbe::Dispatch);

            test::ScopedVulkanCall<PFN_vkQueueSubmit> countSubmit(vkQueueSubmit, RecordingFailureProbe::Submit);

            context->RHIDispatch(1, 1, 1);

            context->RHIDispatch(1, 1, 1);

            EXPECT_TRUE(context->GetRecordingError().IsFailure());

            EXPECT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eRejected);

            EXPECT_EQ(RecordingFailureProbe::dispatches, 0u);

            EXPECT_EQ(RecordingFailureProbe::submissions, 0u);
        }

        ZEN_DELETE(context);

        session.rhi.DestroyBuffer(buffer);
    }

    session.rhi.DestroyPipeline(pipeline);

    session.rhi.DestroyShader(shader);
}

struct ImpossibleBindlessProfile
{
    static inline PFN_vkGetPhysicalDeviceProperties2 original;

    static VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2* output)
    {
        original(gpu, output);
        for (VkBaseOutStructure* node = static_cast<VkBaseOutStructure*>(output->pNext); node != nullptr; node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES)
            {
                reinterpret_cast<VkPhysicalDeviceDescriptorIndexingProperties*>(node)->maxPerStageUpdateAfterBindResources = 3;
            }
        }
    }
};

struct UnsupportedBindlessLayout
{
    static VKAPI_ATTR void VKAPI_CALL Query(VkDevice,
                                            const VkDescriptorSetLayoutCreateInfo*,
                                            VkDescriptorSetLayoutSupport* support)
    {
        support->supported = VK_FALSE;
    }
};

TEST(VulkanProductionFailureDeathTest, UnsupportedGlobalHeapLayoutStopsStartup)
{
    EXPECT_DEATH(
        {
            test::VulkanSession                                         session;
            test::ScopedVulkanCall<PFN_vkGetDescriptorSetLayoutSupport> query(vkGetDescriptorSetLayoutSupport,
                                                                              UnsupportedBindlessLayout::Query);
            VulkanBindlessHeap                                          manager(session.rhi.GetDevice());
            zen::test::InitOrAbort(manager);
        },
        "does not support the global bindless layout");
}

TEST(VulkanProductionFailureDeathTest, DeviceInitializationReportsImpossibleBindlessMinimum)
{
    EXPECT_DEATH(
        {
            test::VulkanSession session;
            ImpossibleBindlessProfile::original = vkGetPhysicalDeviceProperties2;
            test::ScopedVulkanCall<PFN_vkGetPhysicalDeviceProperties2> properties(vkGetPhysicalDeviceProperties2,
                                                                                  ImpossibleBindlessProfile::Properties);
            VulkanDevice                                               device(&session.rhi, session.rhi.GetPhysicalDevice());
            zen::test::InitOrAbort(device);
        },
        "at least one slot per heap");
}

TEST(VulkanProductionFailureTest, InvalidShaderDoesNotPublishAndSpecializationUsesOwnedBytecode)
{
    test::VulkanSession session;

    RHIShaderCreateInfo invalid;

    invalid.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);

    invalid.spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    invalid.spirv->SetStageSPIRV(RHIShaderStage::eCompute, HeapVector<uint8_t>{0, 1, 2, 3});

    EXPECT_EQ(session.rhi.CreateShader(invalid), nullptr);

    RHIShaderCreateInfo baseInfo;

    AddProductionStage(baseInfo, RHIShaderStage::eCompute, "pipeline_specialization.comp.spv");

    RHIShader* base = session.rhi.CreateShader(baseInfo);

    ASSERT_NE(base, nullptr);

    RHIShaderCreateInfo specialized                                   = base->GetCreateInfo();

    specialized.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = "missing-specialization-source.spv";

    RHIShader* shader                                                 = session.rhi.CreateShader(specialized);

    ASSERT_NE(shader, nullptr);

    session.rhi.DestroyShader(shader);

    ASSERT_TRUE(specialized.reflection.has_value());

    specialized.reflection->SRDTable.resize(2);

    RHIShaderResourceDescriptor invalidDescriptor;

    invalidDescriptor.type = RHIShaderResourceType::eMax;

    specialized.reflection->SRDTable[1].push_back(invalidDescriptor);

    EXPECT_EQ(session.rhi.CreateShader(specialized), nullptr);

    session.rhi.DestroyShader(base);
}
} // namespace
