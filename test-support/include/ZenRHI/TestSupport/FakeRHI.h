#pragma once
#include "ZenRHI/RHICommandListExecutor.h"
#include "ZenRHI/RHIGPUFrameTiming.h"
#include "ZenRHI/RHIShaderUtil.h"
#include "ZenFoundation/Utils/UniquePtr.h"
#include <gtest/gtest.h>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif

namespace zen
{
// White-box state tests can inject acceptance/failure without exposing production mutators.
struct RHISubmissionStateTestAccess
{
    static bool Queue(RHISubmissionState& state)
    {
        return state.Queue();
    }

    static bool Accept(RHISubmissionState& state, uint32_t group, RHISubmissionDependency accepted)
    {
        return state.Accept(group, accepted);
    }

    static bool FinishSubmission(RHISubmissionState& state)
    {
        return state.FinishSubmission();
    }

    static void Fail(RHISubmissionState& state)
    {
        state.Fail();
    }
};

} // namespace zen

namespace
{
using namespace zen;
std::unordered_set<uint64_t>                    destroyed;
std::unordered_map<NameID, RHIShaderCreateInfo> reflectedShaderInfos;

class TestBuffer : public RHIBuffer
{
public:
    bool     asyncComputeAccessible{true};
    uint64_t reportedAllocationBytes{0};

    uint64_t GetAllocationSize() const override
    {
        return reportedAllocationBytes;
    }

    bool IsAsyncComputeAccessible() const override
    {
        return asyncComputeAccessible;
    }

    explicit TestBuffer(const RHIBufferCreateInfo& info) : RHIBuffer(info), bytes(info.size, 0xCD) {}

    uint8_t* Map() override
    {
        return bytes.data();
    }

    void Unmap() override {}

    uint64_t GetDeviceAddress() const override
    {
        return GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eDeviceAddress) ? GetStableId() * 4096 : 0;
    }

    bool SetTexelFormat(DataFormat) override
    {
        return true;
    }

    std::vector<uint8_t> bytes;

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());

        ZEN_DELETE(this);
    }
};

class TestAccelerationStructure : public RHIAccelerationStructure
{
public:
    explicit TestAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) : RHIAccelerationStructure(info) {}

    bool IsAsyncComputeAccessible() const override
    {
        return true;
    }

private:
    void Init() override {}
    void Destroy() override
    {
        destroyed.insert(GetStableId());
        ZEN_DELETE(this);
    }
};

class TestTextureView : public RHITextureView
{
public:
    TestTextureView(RHITexture* texture, const RHITextureViewCreateInfo& info) :
        RHITextureView(texture, info), m_textureIdentity(texture->GetStableId())
    {}

private:
    uint64_t m_textureIdentity;

    void Init() override {}

    void Destroy() override
    {
        EXPECT_FALSE(destroyed.contains(m_textureIdentity));

        destroyed.insert(GetStableId());

        ZEN_DELETE(this);
    }
};

class TestSampler : public RHISampler
{
public:
    explicit TestSampler(const RHISamplerCreateInfo& info) : RHISampler(info) {}

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());

        ZEN_DELETE(this);
    }
};

class TestTexture : public RHITexture
{
public:
    bool asyncComputeAccessible{true};

    bool IsAsyncComputeAccessible() const override
    {
        return asyncComputeAccessible;
    }

    explicit TestTexture(const RHITextureCreateInfo& info) : RHITexture(info)
    {
        RHITextureViewCreateInfo view{};

        view.format      = info.format;

        view.type        = info.type;

        view.mipLevels   = info.mipmaps;

        view.arrayLayers = info.arrayLayers;

        m_pDefaultView   = CreateView(view);
    }

    // One byte per texel and layer: the R8 distance-field atlas accounts its native allocation.
    uint64_t GetAllocationSize() const override
    {
        return uint64_t(GetWidth()) * GetHeight() * GetDepth() * GetBaseInfo().arrayLayers;
    }

    RHITextureView* CreateView(const RHITextureViewCreateInfo& info) override
    {
        TestTextureView* view = ZEN_NEW() TestTextureView(this, info);

        RegisterOwnedView(view);

        return view;
    }

private:
    void Init() override {}

    void Destroy() override
    {
        DestroyOwnedViews();

        destroyed.insert(GetStableId());

        ZEN_DELETE(this);
    }
};

class TestViewport : public RHIViewport
{
public:
    TestViewport() : RHIViewport({}, 8, 8, false) {}

    ~TestViewport() override
    {
        ReleaseForTeardown();
    }

    void ReleaseForTeardown()
    {
        // Stack storage can outlive the backend, but its owning reference cannot.
        if (!m_released)
        {
            GetRHIThread().Invoke([this] { ReleaseReference(); });

            m_released = true;
        }
    }

    RHITexture* color{};
    RHITexture* depth{};
    uint32_t    preparePresents{0};
    uint32_t    presents{0};
    bool        presentResult{true};
    bool        recordPresentCommands{false};
    bool        recreationRequested{false};
    bool        recreationPossible{true};
    uint32_t    resizes{0};

    void PrepareForPresent(RHICommandList* commands) override
    {
        ++preparePresents;

        if (recordPresentCommands)
        {
            commands->Dispatch(1, 1, 1);
        }
    }

    bool Present() override
    {
        ++presents;

        return presentResult;
    }

    bool NeedsRecreation() const override
    {
        return recreationRequested;
    }

    bool CanRecreate() const override
    {
        return recreationPossible;
    }

    uint32_t GetWidth() const override
    {
        return m_width;
    }

    uint32_t GetHeight() const override
    {
        return m_height;
    }

    DataFormat GetSwapchainFormat() override
    {
        return color->GetFormat();
    }

    DataFormat GetDepthStencilFormat() override
    {
        return depth->GetFormat();
    }

    RHITexture* GetColorBackBuffer() override
    {
        return color;
    }

    RHITexture* GetDepthStencilBackBuffer() override
    {
        return depth;
    }

    RHITextureSubResourceRange GetColorBackBufferRange() override
    {
        return color->GetSubResourceRange();
    }

    RHITextureSubResourceRange GetDepthStencilBackBufferRange() override
    {
        return depth->GetSubResourceRange();
    }

    void Resize(uint32_t width, uint32_t height) override
    {
        ++resizes;

        recreationRequested = false;

        m_width             = width;

        m_height            = height;
    }

private:
    void Init() override {}

    void Destroy() override {}

    bool m_released{false};
};

class TestShader : public RHIShader
{
public:
    void SetBenchmarkSPIRV(size_t size)
    {
        HeapVector<uint8_t> bytes(size);

        for (size_t i = 0; i < size; ++i)
        {
            bytes[i] = uint8_t(i * 37);
        }

        m_shaderGroupSPIRV->SetStageSPIRV(RHIShaderStage::eCompute, std::move(bytes));
    }

    void EnableGeometryStage()
    {
        m_shaderStageFlags.SetFlag(RHIShaderStageFlagBits::eGeometry);
    }

    void SetDescriptorStages(NameID name, RHIShaderStageFlagBits stage)
    {
        for (RHIShaderResourceDescriptor& descriptor : m_SRDTable[0])
        {
            if (descriptor.name == std::string_view(name))
            {
                descriptor.stageFlags = int64_t(stage);
            }
        }
    }

    void SetDescriptorArray(NameID name, uint32_t count, uint32_t blockSize = 0)
    {
        for (RHIShaderResourceDescriptor& descriptor : m_SRDTable[0])
        {
            if (descriptor.name == std::string_view(name))
            {
                descriptor.arraySize = count;

                descriptor.blockSize = blockSize;
            }
        }
    }

    void SetDescriptorReadOnly(NameID name)
    {
        for (RHIShaderResourceDescriptor& descriptor : m_SRDTable[0])
        {
            if (descriptor.name == std::string_view(name))
            {
                descriptor.writable = false;
            }
        }
    }

    TestShader(const RHIShaderCreateInfo& info, uint32_t maxDescriptorSets) : RHIShader(info)
    {
        if (!info.stageFlags.IsEmpty())
        {
            RHIShaderGroupSPIRVPtr spirv = info.spirv;
            ZEN_VERIFY(spirv);
            RHIShaderGroupInfo reflected{};

            ZEN_VERIFY(RHIShaderUtil::ReflectShaderGroupInfo(spirv, maxDescriptorSets, reflected));

            m_SRDTable = std::move(reflected.SRDTable);

            for (std::vector<RHIShaderResourceDescriptor> const& set : m_SRDTable)
            {
                for (RHIShaderResourceDescriptor const& descriptor : set)
                {
                    m_namedSRDLut[descriptor.name] = &descriptor;
                }
            }
        }
        else
        {
            m_SRDTable.resize(1);

            for (const std::pair<NameID, RHIShaderResourceType>& binding :
                 std::vector<std::pair<NameID, RHIShaderResourceType>>{
                     {"buffer", RHIShaderResourceType::eStorageBuffer},
                     {"texture", RHIShaderResourceType::eSamplerWithTexture},
                     {"uTextureArray", RHIShaderResourceType::eSamplerWithTexture},
                     {"uTexture2DHeap", RHIShaderResourceType::eTexture},
                     {"uSamplerHeap", RHIShaderResourceType::eSampler},
                     {"image", RHIShaderResourceType::eImage},
                     {"value", RHIShaderResourceType::eUniformBuffer},
                     {"read_buffer", RHIShaderResourceType::eStorageBuffer},
                     {"write_buffer", RHIShaderResourceType::eStorageBuffer},
                     {"write_image", RHIShaderResourceType::eImage}})
            {
                RHIShaderResourceDescriptor descriptor{};

                descriptor.name      = binding.first;

                descriptor.type      = binding.second;

                descriptor.binding   = static_cast<uint32_t>(m_SRDTable[0].size());

                descriptor.arraySize = descriptor.name == "uTextureArray" ? 1024 : 1;

                descriptor.bindless  = descriptor.name == "uTexture2DHeap" || descriptor.name == "uSamplerHeap";

                descriptor.writable =
                    binding.second == RHIShaderResourceType::eImage || binding.second == RHIShaderResourceType::eStorageBuffer;

                if (descriptor.name == "read_buffer")
                {
                    descriptor.writable = false;
                }

                if (descriptor.name == "write_buffer" || descriptor.name == "write_image")
                {
                    descriptor.readable = false;
                }

                m_SRDTable[0].push_back(descriptor);
            }

            for (RHIShaderResourceDescriptor const& descriptor : m_SRDTable[0])
            {
                m_namedSRDLut[descriptor.name] = &descriptor;
            }
        }
    }

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());

        ZEN_DELETE(this);
    }
};

class TestPipeline : public RHIPipeline
{
public:
    explicit TestPipeline(const RHIGfxPipelineCreateInfo& info) : RHIPipeline(info) {}

    explicit TestPipeline(const RHIComputePipelineCreateInfo& info) : RHIPipeline(info) {}

    explicit TestPipeline(const RHIRayTracingPipelineCreateInfo& info) : RHIPipeline(info) {}

    RHIPolygonCullMode GetCullMode() const
    {
        return m_gfxStates.rasterizationState.cullMode;
    }

    RHIPolygonFrontFace GetFrontFace() const
    {
        return m_gfxStates.rasterizationState.frontFace;
    }

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());

        ZEN_DELETE(this);
    }
};

using TestDescriptorLocation = std::array<uint32_t, 2>;
using TestDispatchExtent     = std::array<uint32_t, 3>;

class TestContext : public IRHICommandContext
{
public:
    explicit TestContext(RHICommandContextType type) : type(type) {}

    RHICommandContextType type;
    std::function<void()> wait;
    uint32_t              proxyDestructions{0};
    std::thread::id       lastProxyDestroyThread;
    struct BarrierBatch
    {
        RHIFlags<RHIPipelineStageFlagBits> source;
        RHIFlags<RHIPipelineStageFlagBits> destination;
        std::vector<RHIBufferTransition>   buffers;
        std::vector<RHITextureTransition>  textures;
    };
    std::vector<BarrierBatch> barrierBatches;
    struct TextureClear
    {
        RHITexture*                texture;
        Color                      color;
        RHITextureSubResourceRange range;
    };
    std::vector<TextureClear>                       textureClears;
    std::vector<size_t>                             clearsBeforeDraw;
    std::vector<size_t>                             clearsBeforeDispatch;
    std::vector<RHIBufferTransition>                bufferTransitions;
    std::vector<RHITextureTransition>               textureTransitions;
    std::vector<RHIFlags<RHIPipelineStageFlagBits>> barrierSources;
    std::vector<RHIMemoryTransition>                memoryBarriers;
    std::vector<RHIBufferTextureCopyRegion>         textureCopies;
    std::vector<RHITextureCopyRegion>               imageCopies;
    HeapVector<std::pair<RHITexture*, RHITexture*>> imageCopyTextures;
    std::vector<RHITextureBlitRegion>               blits;
    std::vector<RHIBufferCopyRegion>                bufferCopies;
    std::vector<RHIBuffer*>                         bufferCopyDestinations;
    std::vector<std::vector<uint8_t>>               values;
    HeapVector<TestDescriptorLocation>              valueBindings;
    std::vector<std::vector<uint8_t>>               pushConstants;
    std::vector<RHIResource*>                       boundResources;
    std::vector<uint32_t>                           boundArrayIndices;
    std::vector<RHIBuffer*>                         indirectDraws;

    HeapVector<std::pair<RHIShader*, RHIShaderResourceParameter>> boundShaderResources;

    struct IndirectDrawCall
    {
        RHIBuffer*   buffer;
        uint64_t     offset;
        uint32_t     count;
        uint32_t     stride;
        RHIPipeline* pipeline;
        uint32_t     material;
        uint32_t     pushSize;
        uint32_t     pushOffset;
    };
    HeapVector<IndirectDrawCall>                          indirectDrawCalls;
    uint32_t                                              lastPushOffset{0};
    std::vector<RHIBuffer*>                               indirectDispatches;
    HeapVector<RHIShader*>                                indirectShaders;
    uint32_t                                              drawCount{0};
    uint32_t                                              dispatchCount{0};
    RHIShader*                                            boundShader{nullptr};
    RHIPipeline*                                          boundPipeline{nullptr};
    HeapVector<RHIPipeline*>                              indexedPipelines;
    HeapVector<RHIPipeline*>                              pushConstantPipelines;
    HeapVector<std::pair<RHIShader*, TestDispatchExtent>> dispatches;
    uint32_t                                              renderingCount{0};
    uint32_t                                              vertexBufferCount{0};
    std::vector<RHIBuffer*>                               vertexBuffers;
    std::vector<RHIRenderingLayout>                       renderingLayouts;
    std::vector<std::pair<size_t, size_t>>                renderingCommandOffsets;
    std::vector<std::array<uint32_t, 2>>                  indexedDraws;

    RHICommandContextType GetContextType() override
    {
        return type;
    }

    void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout) override
    {
        EXPECT_NE(pRenderingLayout, nullptr);

        renderingLayouts.push_back(*pRenderingLayout);

        renderingCommandOffsets.emplace_back(pushConstants.size(), indexedDraws.size());

        ++renderingCount;
    }

    void RHIEndRendering() override {}

    void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override {}

    void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override {}

    void RHISetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor) override {}

    void RHISetLineWidth(float lineWidth) override {}

    void RHISetBlendConstants(const Color& blendConstants) override {}

    void RHIBindPipeline(RHIPipeline* pPipeline) override
    {
        boundPipeline = pPipeline;
        boundShader   = pPipeline != nullptr ? pPipeline->GetShader() : nullptr;
    }

    void RHISetShaderParameters(RHIShaderParameterView parameters) override
    {
        for (const RHIShaderValueParameter& value : parameters.GetValueParams())
        {
            std::span<const uint8_t> bytes = parameters.GetValueBytes(value);

            values.emplace_back(bytes.begin(), bytes.end());
            valueBindings.push_back({value.set, value.binding});
        }

        for (const RHIShaderResourceParameter& resource : parameters.GetResourceParams())
        {
            boundResources.push_back(resource.pResource);
            boundShaderResources.emplace_back(boundShader, resource);

            boundArrayIndices.push_back(resource.arrayIndex);
        }

        for (const RHIShaderResourceParameter& resource : parameters.GetBindlessParams())
        {
            boundResources.push_back(resource.pResource);
            boundShaderResources.emplace_back(boundShader, resource);

            boundArrayIndices.push_back(resource.arrayIndex);
        }
    }

    void RHIBindVertexBuffers(std::span<RHIBuffer*> pBuffers, std::span<uint64_t> offsets) override
    {
        vertexBufferCount = static_cast<uint32_t>(pBuffers.size());

        vertexBuffers.insert(vertexBuffers.end(), pBuffers.begin(), pBuffers.end());
    }

    void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) override {}

    void RHIDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
    {
        clearsBeforeDraw.push_back(textureClears.size());

        ++drawCount;
    }

    void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                        DataFormat indexFormat,
                        uint64_t   indexBufferOffset,
                        uint32_t   indexCount,
                        uint32_t   instanceCount,
                        uint32_t   firstIndex,
                        int32_t    vertexOffset,
                        uint32_t   firstInstance) override
    {
        indexedDraws.push_back({firstIndex, indexCount});
        indexedPipelines.push_back(boundPipeline);
    }

    void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint64_t   indexBufferOffset,
                                uint64_t   offset,
                                uint32_t   drawCount,
                                uint32_t   stride) override
    {
        indirectDraws.push_back(pIndirectBuffer);
        uint32_t       material = UINT32_MAX;
        const uint32_t pushSize = pushConstants.empty() ? 0 : static_cast<uint32_t>(pushConstants.back().size());
        if (pushSize >= sizeof(material))
        {
            std::memcpy(&material, pushConstants.back().data(), sizeof(material));
        }
        indirectDrawCalls.push_back(
            {pIndirectBuffer, offset, drawCount, stride, boundPipeline, material, pushSize, lastPushOffset});
    }

    void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override
    {
        clearsBeforeDispatch.push_back(textureClears.size());
        dispatches.emplace_back(boundShader, TestDispatchExtent{groupCountX, groupCountY, groupCountZ});

        ++dispatchCount;
    }

    void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset) override
    {
        indirectDispatches.push_back(pIndirectBuffer);
        indirectShaders.push_back(boundShader);
    }

    void RHITraceRays(uint32_t width, uint32_t height, uint32_t depth) override
    {
        rayTraces.emplace_back(boundShader, TestDispatchExtent{width, height, depth});
    }

    void RHITraceRaysIndirect(RHIBuffer* buffer, uint64_t offset) override
    {
        indirectRayTraces.emplace_back(buffer, offset);
    }

    HeapVector<std::pair<RHIShader*, TestDispatchExtent>> rayTraces;
    HeapVector<std::pair<RHIBuffer*, uint64_t>>           indirectRayTraces;

    void RHISetPushConstants(RHIPipeline* pPipeline, std::span<const uint8_t> data, uint32_t offset = 0) override
    {
        pushConstants.emplace_back(data.begin(), data.end());
        lastPushOffset = offset;
        pushConstantPipelines.push_back(pPipeline);
    }

    void RHIAddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages,
                           RHIFlags<RHIPipelineStageFlagBits> dstStages,
                           std::span<RHIMemoryTransition>     memoryTransitions,
                           std::span<RHIBufferTransition>     bufferTransitionsView,
                           std::span<RHITextureTransition>    textureTransitionsView) override
    {
        barrierBatches.push_back({srcStages,
                                  dstStages,
                                  {bufferTransitionsView.begin(), bufferTransitionsView.end()},
                                  {textureTransitionsView.begin(), textureTransitionsView.end()}});

        barrierSources.push_back(srcStages);

        memoryBarriers.insert(memoryBarriers.end(), memoryTransitions.begin(), memoryTransitions.end());

        bufferTransitions.insert(bufferTransitions.end(), bufferTransitionsView.begin(), bufferTransitionsView.end());

        textureTransitions.insert(textureTransitions.end(), textureTransitionsView.begin(), textureTransitionsView.end());
    }

    uint64_t clearOffset{0};
    uint64_t clearSize{0};

    void RHIClearBuffer(RHIBuffer*, uint64_t offset, uint64_t size) override
    {
        clearOffset = offset;

        clearSize   = size;
    }

    HeapVector<uint32_t>                         accelerationStructureBuildCounts;
    HeapVector<RHIAccelerationStructureInstance> accelerationStructureInstances;
    void RHIBuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info) override
    {
        accelerationStructureBuildCounts.push_back(info.description.type == RHIAccelerationStructureType::eTopLevel
                                                       ? static_cast<uint32_t>(info.description.instances.size())
                                                       : info.description.geometries[0].indexCount);
        accelerationStructureInstances.clear();
        for (const RHIAccelerationStructureInstance& instance : info.description.instances)
        {
            accelerationStructureInstances.push_back(instance);
        }
    }

    void RHICopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region) override
    {
        TestBuffer* source      = static_cast<TestBuffer*>(pSrcBuffer);

        TestBuffer* destination = static_cast<TestBuffer*>(pDstBuffer);

        EXPECT_LE(region.srcOffset + region.size, source->bytes.size());

        EXPECT_LE(region.dstOffset + region.size, destination->bytes.size());

        std::memcpy(destination->bytes.data() + region.dstOffset, source->bytes.data() + region.srcOffset, region.size);

        bufferCopies.push_back(region);

        bufferCopyDestinations.push_back(pDstBuffer);
    }

    void RHIClearTexture(RHITexture* pTex, const Color& color, const RHITextureSubResourceRange& range) override
    {
        textureClears.push_back({pTex, color, range});
    }

    void RHICopyTexture(RHITexture* pSrcTexture, RHITexture* pDstTexture, std::span<RHITextureCopyRegion> regions) override
    {
        imageCopyTextures.emplace_back(pSrcTexture, pDstTexture);

        imageCopies.insert(imageCopies.end(), regions.begin(), regions.end());
    }

    void RHIBlitTexture(RHITexture*                     pSrcTexture,
                        RHITexture*                     pDstTexture,
                        std::span<RHITextureBlitRegion> regions,
                        RHISamplerFilter                filter) override
    {
        blits.insert(blits.end(), regions.begin(), regions.end());
    }

    void RHICopyTextureToBuffer(RHITexture*                           pSrcTex,
                                RHIBuffer*                            pDstBuffer,
                                std::span<RHIBufferTextureCopyRegion> regions) override
    {}

    void RHICopyBufferToTexture(RHIBuffer*                            pSrcBuffer,
                                RHITexture*                           pDstTexture,
                                std::span<RHIBufferTextureCopyRegion> regions) override
    {
        textureCopies.insert(textureCopies.end(), regions.begin(), regions.end());
    }

    void RHIResolveTexture(RHITexture* pSrcTexture,
                           RHITexture* pDstTexture,
                           uint32_t    srcLayer,
                           uint32_t    srcMipmap,
                           uint32_t    dstLayer,
                           uint32_t    dstMipmap) override
    {}

    void RHIWaitUntilCompleted() override
    {
        wait();
    }
};
// Command lists own their contexts; proxies delegate to shared observation logs.
class TestContextProxy : public IRHICommandContext
{
public:
    explicit TestContextProxy(TestContext& log) : log(log) {}

    ~TestContextProxy() override
    {
        ++log.proxyDestructions;

        log.lastProxyDestroyThread = std::this_thread::get_id();
    }

    TestContext& log;

    void RHIBuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info) override
    {
        log.RHIBuildAccelerationStructure(info);
    }

    RHICommandContextType GetContextType() override
    {
        return log.GetContextType();
    }

    void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout) override
    {
        return log.RHIBeginRendering(pRenderingLayout);
    }

    void RHIEndRendering() override
    {
        return log.RHIEndRendering();
    }

    void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override
    {
        return log.RHISetScissor(minX, minY, maxX, maxY);
    }

    void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override
    {
        return log.RHISetViewport(minX, minY, maxX, maxY);
    }

    void RHISetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor) override
    {
        return log.RHISetDepthBias(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
    }

    void RHISetLineWidth(float lineWidth) override
    {
        return log.RHISetLineWidth(lineWidth);
    }

    void RHISetBlendConstants(const Color& blendConstants) override
    {
        return log.RHISetBlendConstants(blendConstants);
    }

    void RHIBindPipeline(RHIPipeline* pPipeline) override
    {
        return log.RHIBindPipeline(pPipeline);
    }

    void RHISetShaderParameters(RHIShaderParameterView parameters) override
    {
        log.RHISetShaderParameters(parameters);
    }

    void RHIBindVertexBuffers(std::span<RHIBuffer*> pBuffers, std::span<uint64_t> offsets) override
    {
        return log.RHIBindVertexBuffers(pBuffers, offsets);
    }

    void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) override
    {
        return log.RHIBindVertexBuffer(pBuffer, offset);
    }

    void RHIDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
    {
        return log.RHIDraw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                        DataFormat indexFormat,
                        uint64_t   indexBufferOffset,
                        uint32_t   indexCount,
                        uint32_t   instanceCount,
                        uint32_t   firstIndex,
                        int32_t    vertexOffset,
                        uint32_t   firstInstance) override
    {
        return log.RHIDrawIndexed(pIndexBuffer, indexFormat, indexBufferOffset, indexCount, instanceCount, firstIndex,
                                  vertexOffset, firstInstance);
    }

    void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint64_t   indexBufferOffset,
                                uint64_t   offset,
                                uint32_t   drawCount,
                                uint32_t   stride) override
    {
        return log.RHIDrawIndexedIndirect(pIndirectBuffer, pIndexBuffer, indexFormat, indexBufferOffset, offset, drawCount,
                                          stride);
    }

    void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override
    {
        return log.RHIDispatch(groupCountX, groupCountY, groupCountZ);
    }

    void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset) override
    {
        return log.RHIDispatchIndirect(pIndirectBuffer, offset);
    }

    void RHITraceRays(uint32_t width, uint32_t height, uint32_t depth) override
    {
        log.RHITraceRays(width, height, depth);
    }

    void RHITraceRaysIndirect(RHIBuffer* buffer, uint64_t offset) override
    {
        log.RHITraceRaysIndirect(buffer, offset);
    }

    void RHISetPushConstants(RHIPipeline* pPipeline, std::span<const uint8_t> data, uint32_t offset = 0) override
    {
        return log.RHISetPushConstants(pPipeline, data, offset);
    }

    void RHIAddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages,
                           RHIFlags<RHIPipelineStageFlagBits> dstStages,
                           std::span<RHIMemoryTransition>     memoryTransitions,
                           std::span<RHIBufferTransition>     bufferTransitions,
                           std::span<RHITextureTransition>    textureTransitions) override
    {
        return log.RHIAddTransitions(srcStages, dstStages, memoryTransitions, bufferTransitions, textureTransitions);
    }

    void RHIClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size) override
    {
        return log.RHIClearBuffer(pBuffer, offset, size);
    }

    void RHICopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region) override
    {
        return log.RHICopyBuffer(pSrcBuffer, pDstBuffer, region);
    }

    void RHIClearTexture(RHITexture* pTex, const Color& color, const RHITextureSubResourceRange& range) override
    {
        return log.RHIClearTexture(pTex, color, range);
    }

    void RHICopyTexture(RHITexture* pSrcTexture, RHITexture* pDstTexture, std::span<RHITextureCopyRegion> regions) override
    {
        return log.RHICopyTexture(pSrcTexture, pDstTexture, regions);
    }

    void RHIBlitTexture(RHITexture*                     pSrcTexture,
                        RHITexture*                     pDstTexture,
                        std::span<RHITextureBlitRegion> regions,
                        RHISamplerFilter                filter) override
    {
        return log.RHIBlitTexture(pSrcTexture, pDstTexture, regions, filter);
    }

    void RHICopyTextureToBuffer(RHITexture*                           pSrcTex,
                                RHIBuffer*                            pDstBuffer,
                                std::span<RHIBufferTextureCopyRegion> regions) override
    {
        return log.RHICopyTextureToBuffer(pSrcTex, pDstBuffer, regions);
    }

    void RHICopyBufferToTexture(RHIBuffer*                            pSrcBuffer,
                                RHITexture*                           pDstTexture,
                                std::span<RHIBufferTextureCopyRegion> regions) override
    {
        return log.RHICopyBufferToTexture(pSrcBuffer, pDstTexture, regions);
    }

    void RHIResolveTexture(RHITexture* pSrcTexture,
                           RHITexture* pDstTexture,
                           uint32_t    srcLayer,
                           uint32_t    srcMipmap,
                           uint32_t    dstLayer,
                           uint32_t    dstMipmap) override
    {
        return log.RHIResolveTexture(pSrcTexture, pDstTexture, srcLayer, srcMipmap, dstLayer, dstMipmap);
    }

    void RHIWaitUntilCompleted() override
    {
        return log.RHIWaitUntilCompleted();
    }
};
class TestRHI : public DynamicRHI
{
public:
    TestRHI()
    {
        graphics.wait = compute.wait = transfer.wait = [this] {
            completed = submitted;
        };

        info.uniformBufferAlignment = 16;

        info.storageBufferAlignment = 16;

        // The fake device reports a typical desktop descriptor set limit.
        info.maxBoundDescriptorSets = 32;
    }

    TestContext                                                graphics{RHICommandContextType::eGraphics};
    TestContext                                                compute{RHICommandContextType::eAsyncCompute};
    TestContext                                                transfer{RHICommandContextType::eTransfer};
    std::array<uint64_t, 3>                                    submitted{};
    std::array<uint64_t, 3>                                    completed{};
    RHIGPUInfo                                                 info{};
    RHIGPUMemoryStats                                          memoryStats{};
    RHIQueueCopyCapabilities                                   graphicsCopy{true, true, true, {1, 1, 1}};
    RHIQueueCopyCapabilities                                   computeCopy{false, true, true, {1, 1, 1}};
    RHIQueueCopyCapabilities                                   transferCopy{false, false, true, {1, 1, 1}};
    std::unordered_map<DataFormat, RHITextureCopyCapabilities> copyCapabilities;
    RHIQueueCapabilities                                       submissionQueueCapabilities{false, false, {0, 1, 2}};
    mutable uint32_t                                           queueCapabilityQueries{0};
    struct SubmissionDependency
    {
        RHICommandContextType   consumer;
        RHISubmissionDependency producer;
    };
    HeapVector<SubmissionDependency>                        gpuDependencies;
    uint32_t                                                pipelineCount{0};
    uint32_t                                                frameBegins{0};
    uint32_t                                                deviceIdleWaits{0};
    mutable uint64_t                                        progressQueries{0};
    uint32_t                                                contextCreations{0};
    bool                                                    failContextCreation{false};
    bool                                                    failSubmissionWait{false};
    bool                                                    failProgressQuery{false};
    bool                                                    submissionsBlocked{false};
    std::vector<std::pair<RHICommandContextType, uint64_t>> submissionWaits;
    uint32_t                                                textureCreations{0};
    uint32_t                                                textureViewCreations{0};
    uint32_t                                                failTextureViewCreationAt{0};
    HeapVector<uint64_t>                                    createdTextureIds;
    RHITexture*                                             lastCreatedTexture{nullptr};
    uint32_t                                                bufferCreations{0};
    uint32_t                                                failBufferCreationAt{0};
    uint64_t                                                bufferAllocationPadding{UINT64_MAX};
    uint64_t                                                bufferAllocationAlignment{1};
    bool                                                    failBufferAllocationQuery{false};
    uint32_t                                                bufferAllocationQueries{0};
    bool                                                    failIndexBufferCreation{false};
    HeapVector<RHIBufferCreateInfo>                         createdBufferInfos;
    RHIBufferCreateInfo                                     lastBufferInfo;
    uint64_t                                                lastBufferId{0};
    std::thread::id                                         lastBufferCreationThread;
    std::thread::id                                         lastTextureCreationThread;
    uint32_t                                                finalizedLists{0};
    uint32_t                                                submissionAttempts{0};
    uint32_t                                                failSubmissionAt{0};
    RHISubmissionResult                                     submissionFailure{RHISubmissionResult::eRejected};
    bool                                                    submitBeforeFailure{false};
    std::function<void()>                                   beforeSubmission;
    std::vector<RHICommandList*>                            pendingCommandLists;
    uint32_t                                                failTextureCreationAt{0};
    uint32_t                                                failPipelineCreationAt{0};
    const RHIRenderingLayout*                               lastPipelineLayout{nullptr};
    std::vector<RHIPipeline*>                               createdPipelines;

    void Init() override {}

    void Destroy() override
    {
        // Recorded layouts retain attachment references; release them inside the finalizer.
        graphics.renderingLayouts.clear();

        compute.renderingLayouts.clear();

        transfer.renderingLayouts.clear();
    }

    void BeginFrame() override
    {
        ++frameBegins;
    }

    IRHICommandContext* GetCommandContext(RHICommandContextType type) override
    {
        IRHICommandContext* result = nullptr;

        ++contextCreations;

        TestContext& context = type == RHICommandContextType::eAsyncCompute ? compute
                             : type == RHICommandContextType::eTransfer     ? transfer
                                                                            : graphics;

        if (!failContextCreation)
        {
            result = ZEN_NEW() TestContextProxy(context);
        }

        return result;
    }

    IRHICommandContext* GetTransferCommandContext() override
    {
        return GetCommandContext(RHICommandContextType::eTransfer);
    }

    RHIAPIType GetAPIType() override
    {
        return RHIAPIType::eVulkan;
    }

    std::string_view GetName() override
    {
        static const NameID name("test");

        return name;
    }

    DataFormat GetSupportedDepthFormat() override
    {
        return DataFormat::eD32SFloat;
    }

    RHIViewport* CreateViewport(zen::RHIWindowHandle, uint32_t, uint32_t, bool) override
    {
        return nullptr;
    }

    void DestroyViewport(RHIViewport*) override {}

    bool failShaderCreation{false};

    RHIShader* CreateShader(const RHIShaderCreateInfo& info) override
    {
        RHIShader* result{};

        if (!failShaderCreation)
        {
            if (std::unordered_map<NameID, RHIShaderCreateInfo>::iterator it = reflectedShaderInfos.find(info.name);
                it != reflectedShaderInfos.end())
            {
                RHIShaderCreateInfo reflectedInfo     = it->second;

                reflectedInfo.name                    = info.name;

                reflectedInfo.specializationConstants = info.specializationConstants;

                result                                = ZEN_NEW() TestShader(reflectedInfo, this->info.maxBoundDescriptorSets);
            }
            else
            {
                result = ZEN_NEW() TestShader(info, this->info.maxBoundDescriptorSets);
            }
        }

        return result;
    }

    void DestroyShader(RHIShader* shader) override
    {
        shader->ReleaseReference();
    }

    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& info) override
    {
        RHIPipeline* pResult = nullptr;

        bool valid           = true;

        EXPECT_NE(info.pRenderingLayout, nullptr);

        ++pipelineCount;

        lastPipelineLayout = info.pRenderingLayout;

        valid              = !(pipelineCount == failPipelineCreationAt);

        if (valid)
        {
            TestPipeline* pipeline = ZEN_NEW() TestPipeline(info);

            createdPipelines.push_back(pipeline);

            pResult = pipeline;
        }

        return pResult;
    }

    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& info) override
    {
        RHIPipeline* result{};

        ++pipelineCount;

        if (pipelineCount != failPipelineCreationAt)
        {
            TestPipeline* pipeline = ZEN_NEW() TestPipeline(info);

            createdPipelines.push_back(pipeline);

            result = pipeline;
        }

        return result;
    }

    RHIPipeline* CreatePipeline(const RHIRayTracingPipelineCreateInfo& info) override
    {
        RHIPipeline* result = nullptr;
        ++pipelineCount;
        std::array<uint32_t, RHIShaderBindingTableLayout::Count> counts{};
        if (pipelineCount != failPipelineCreationAt && info.pShader != nullptr
            && ValidateRayTracingShaderGroups(info.pShader->GetModules(), info.groups, counts))
        {
            TestPipeline* pipeline = ZEN_NEW() TestPipeline(info);
            createdPipelines.push_back(pipeline);
            result = pipeline;
        }
        return result;
    }

    void DestroyPipeline(RHIPipeline* pipeline) override
    {
        pipeline->ReleaseReference();
    }

    RHISampler* CreateSampler(const RHISamplerCreateInfo& info) override
    {
        return ZEN_NEW() TestSampler(info);
    }

    void DestroySampler(RHISampler* sampler) override
    {
        sampler->ReleaseReference();
    }

    RHITexture* CreateTexture(const RHITextureCreateInfo& info) override
    {
        RHITexture* result{};

        ++textureCreations;

        lastTextureCreationThread = std::this_thread::get_id();

        if (textureCreations != failTextureCreationAt)
        {
            result             = ZEN_NEW() TestTexture(info);

            lastCreatedTexture = result;

            createdTextureIds.push_back(result->GetStableId());
        }

        return result;
    }

    RHITextureView* CreateTextureView(RHITexture* texture, const RHITextureViewCreateInfo& info) override
    {
        RHITextureView* view = nullptr;

        ++textureViewCreations;

        if (textureViewCreations != failTextureViewCreationAt)
        {
            view = texture->CreateView(info);
        }

        return view;
    }

    void DestroyTexture(RHITexture* texture) override
    {
        texture->ReleaseReference();
    }

    uint64_t GetBufferAllocationSize(const RHIBufferCreateInfo& info) override
    {
        ++bufferAllocationQueries;
        const uint64_t padding = bufferAllocationPadding == UINT64_MAX ? 0 : bufferAllocationPadding;
        return failBufferAllocationQuery
                 ? 0
                 : (info.size + bufferAllocationAlignment - 1) / bufferAllocationAlignment * bufferAllocationAlignment
                       + padding;
    }

    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& info) override
    {
        RHIBuffer* result = nullptr;

        ++bufferCreations;

        lastBufferCreationThread = std::this_thread::get_id();

        const bool declined      = failIndexBufferCreation && info.usageFlags.HasFlag(RHIBufferUsageFlagBits::eIndexBuffer);

        if (bufferCreations != failBufferCreationAt && !declined)
        {
            TestBuffer* buffer = ZEN_NEW() TestBuffer(info);
            buffer->reportedAllocationBytes =
                bufferAllocationPadding == UINT64_MAX && bufferAllocationAlignment == 1 ? 0 : GetBufferAllocationSize(info);
            result         = buffer;

            lastBufferInfo = info;

            createdBufferInfos.push_back(info);

            lastBufferId = result->GetStableId();
        }

        return result;
    }

    RHIAccelerationStructureBuildSizes GetAccelerationStructureBuildSizes(
        const RHIAccelerationStructureBuildDesc& desc) override
    {
        const uint64_t instanceBytes =
            desc.type == RHIAccelerationStructureType::eTopLevel ? std::max(size_t(1), desc.instances.size()) * 64 : 0;
        return info.rayQuery.IsUsable() ? RHIAccelerationStructureBuildSizes{4096, 1024, 512, instanceBytes}
                                        : RHIAccelerationStructureBuildSizes{};
    }

    RHIAccelerationStructure* CreateAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) override
    {
        return ZEN_NEW() TestAccelerationStructure(info);
    }

    void DestroyBuffer(RHIBuffer* buffer) override
    {
        buffer->ReleaseReference();
    }

    RHIStatus FinalizeCommandLists(std::span<RHICommandList*> lists, std::vector<RHIPlatformCommandList*>&) override
    {
        // Recorded GPU waits are order-independent here, so all lists are prepared before any is queued.
        bool prepared = true;

        for (RHICommandList* list : lists)
        {
            prepared = prepared && list != nullptr
                    && PrepareSubmissionDependencies(list->GetContext(), list->GetSubmissionDependencies());
        }

        submissionsBlocked |= !prepared;

        for (RHICommandList* list : lists)
        {
            if (list != nullptr)
            {
                list->SetSubmittedSerial(0);
            }

            if (!submissionsBlocked)
            {
                ++finalizedLists;

                pendingCommandLists.push_back(list);
            }
        }

        return submissionsBlocked ? RHIStatus{{RHIErrorCode::eBackendFailure, 0, "FinalizeCommandLists"}} : RHIStatus{};
    }

    void SubmitPlatformCommandLists(std::span<RHIPlatformCommandList*>) override {}

    RHISubmissionResult FlushAllGPUCommands() override
    {
        RHISubmissionResult result{};

        if (pendingCommandLists.empty())
        {
            result = RHISubmissionResult::eSuccess;
        }
        else
        {
            ++submissionAttempts;

            if (beforeSubmission)
            {
                beforeSubmission();
            }

            const bool fail = submissionAttempts == failSubmissionAt;

            if (fail && !submitBeforeFailure)
            {
                pendingCommandLists.clear();

                result = submissionFailure;
            }
            else
            {
                // Execute accepted commands only. Failed native submission has no GPU byte effects.
                std::array<bool, 3> started{};

                for (RHICommandList* list : pendingCommandLists)
                {
                    list->Execute();

                    const size_t queue = Index(list->GetContext()->GetContextType());

                    if (list->HasSubmissionBoundary() || !started[queue])
                    {
                        ++submitted[queue];
                    }

                    started[queue] = true;

                    list->SetSubmittedSerial(submitted[queue]);
                }

                pendingCommandLists.clear();

                result = fail ? submissionFailure : RHISubmissionResult::eSuccess;
            }
        }

        return result;
    }

    size_t Index(RHICommandContextType type) const
    {
        return static_cast<size_t>(type);
    }

    RHIQueueCapabilities GetQueueCapabilities() const override
    {
        ++queueCapabilityQueries;

        return submissionQueueCapabilities;
    }

    bool PrepareSubmissionDependencies(IRHICommandContext*                      context,
                                       std::span<const RHISubmissionDependency> dependencies) override
    {
        bool result = true;

        if (!submissionQueueCapabilities.asyncSubmissionDependencies)
        {
            result = DynamicRHI::PrepareSubmissionDependencies(context, dependencies);
        }
        else
        {
            for (RHISubmissionDependency dependency : dependencies)
            {
                if (dependency.serial == RHISubmissionDependency::kLatestSubmitted)
                {
                    dependency.serial = GetLastSubmittedSerial(dependency.queue);
                }

                result = result && dependency.serial <= GetLastSubmittedSerial(dependency.queue);

                if (result && dependency.serial != 0
                    && !submissionQueueCapabilities.AreQueuesShared(dependency.queue, context->GetContextType()))
                {
                    gpuDependencies.push_back({context->GetContextType(), dependency});
                }
            }
        }

        return result;
    }

    bool AreSubmissionsBlocked() const override
    {
        return submissionsBlocked;
    }

    uint64_t GetLastSubmittedSerial(RHICommandContextType type) const override
    {
        ++progressQueries;

        return submitted[Index(type)];
    }

    uint64_t QueryLastCompletedSerial(RHICommandContextType type) override
    {
        ++progressQueries;

        if (failProgressQuery)
        {
            submissionsBlocked = true;
        }

        return completed[Index(type)];
    }

    bool WaitForCompletion(RHICommandContextType type, uint64_t serial, uint64_t timeoutNS = UINT64_MAX) override
    {
        bool result{};

        submissionWaits.emplace_back(type, serial);

        if ((!failSubmissionWait) && (!(serial > submitted[Index(type)])))
        {
            if (completed[Index(type)] >= serial)
            {
                result = true;
            }
            else if (timeoutNS == 0)
            {
                result = false;
            }
            else
            {
                completed[Index(type)] = serial;

                result                 = true;
            }
        }

        return result;
    }

    void WaitDeviceIdle() override
    {
        ++deviceIdleWaits;

        completed = submitted;
    }

    const RHIGPUInfo& QueryGPUInfo() const override
    {
        return info;
    }

    RHIGPUMemoryStats GetGPUMemoryStats() const override
    {
        return memoryStats;
    }

    RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const override
    {
        RHITextureCopyCapabilities result{};

        if (const std::unordered_map<DataFormat, RHITextureCopyCapabilities>::const_iterator it = copyCapabilities.find(format);
            it != copyCapabilities.end())
        {
            result = it->second;
        }
        else
        {
            result = {true, true, true, true, true};
        }

        return result;
    }

    RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const override
    {
        return type == RHICommandContextType::eAsyncCompute ? computeCopy
             : type == RHICommandContextType::eTransfer
                       && !submissionQueueCapabilities.AreQueuesShared(RHICommandContextType::eTransfer,
                                                                       RHICommandContextType::eGraphics)
                 ? transferCopy
                 : graphicsCopy;
    }
};
struct ArenaDataCommand : RHICommand
{
    ArenaDataCommand(const uint8_t* bytes, uint32_t size, uint8_t value, uint32_t& destructions) :
        bytes(bytes), size(size), value(value), destructions(destructions)
    {}

    ~ArenaDataCommand() override
    {
        ++destructions;
    }

    void Execute(RHICommandListBase&) override
    {
        for (uint32_t i = 0; i < size; ++i)
        {
            ASSERT_EQ(bytes[i], value);
        }
    }

    const uint8_t* bytes;
    uint32_t       size;
    uint8_t        value;
    uint32_t&      destructions;
};

void RecordArenaData(RHICommandList& commands, uint8_t value, uint32_t& destructions)
{
    for (uint32_t size : {1024u, 70u * 1024u, 256u * 1024u})
    {
        uint8_t* bytes = commands.AllocateCmdData<uint8_t>(size);
        std::memset(bytes, value, size);
        commands.AllocateCmdTyped<ArenaDataCommand>(bytes, size, value, destructions);
    }
}

class RHISubmissionGate
{
public:
    RHISubmissionGate() : m_release(m_open.get_future().share())
    {
        entered = m_entered.get_future();
    }

    ~RHISubmissionGate()
    {
        Open();
    }

    void OnSubmission()
    {
        if (!m_used)
        {
            m_used              = true;
            executionThread     = std::this_thread::get_id();
            executionFrameState = GRHIFrameState;
            m_entered.set_value();
            releasedInTime = m_release.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        }
    }

    void Open()
    {
        if (!m_opened.exchange(true))
        {
            m_open.set_value();
        }
    }

    std::future<void> entered;
    std::thread::id   executionThread;
    RHIFrameState     executionFrameState;
    std::atomic<bool> releasedInTime{false};

private:
    std::promise<void>       m_entered;
    std::promise<void>       m_open;
    std::shared_future<void> m_release;
    std::atomic<bool>        m_opened{false};
    bool                     m_used{false};
};

class RHIExecutorTest : public testing::Test
{
protected:
    void SetUp() override
    {
        destroyed.clear();
        GRHIFrameState.Init(3);
        rhi         = ZEN_NEW() TestRHI();
        executor    = ZEN_NEW() RHICommandListExecutor(rhi, RHIExecutionMode::eThreaded);
        GDynamicRHI = executor;
    }

    void TearDown() override
    {
        gate.Open();
        executor->Destroy();
        ZEN_DELETE(executor);
        GDynamicRHI = nullptr;
    }

    void ArmGate()
    {
        executor->FlushRHIThread();
        rhi->beforeSubmission = std::bind_front(&RHISubmissionGate::OnSubmission, &gate);
    }

    TestRHI*                rhi{nullptr};
    RHICommandListExecutor* executor{nullptr};
    RHISubmissionGate       gate;
};

#if defined(ZEN_WIN32)
class RHIMessageWindow
{
public:
    RHIMessageWindow()
    {
        handle = CreateWindowExW(0, L"STATIC", L"RHI message test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    }

    ~RHIMessageWindow()
    {
        if (handle != nullptr)
        {
            DestroyWindow(handle);
        }
    }

    void Send()
    {
        DWORD_PTR reply = 0;
        SetLastError(ERROR_SUCCESS);
        const LRESULT delivered = SendMessageTimeoutW(handle, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &reply);
        if (delivered != 0)
        {
            ++deliveredCount;
        }
        else
        {
            lastError = GetLastError();
        }
    }

    HWND     handle{nullptr};
    uint32_t deliveredCount{0};
    DWORD    lastError{ERROR_SUCCESS};
};

class RHIGatedWindowMessage
{
public:
    RHIGatedWindowMessage(RHIThread& thread, RHIMessageWindow& window) :
        m_thread(thread), m_window(window), m_release(m_open.get_future().share())
    {
        entered = m_entered.get_future();
    }

    ~RHIGatedWindowMessage()
    {
        Open();
        m_thread.Stop();
    }

    void Run()
    {
        m_entered.set_value();
        m_release.wait();
        m_window.Send();
    }

    void Open()
    {
        if (!m_opened)
        {
            m_opened = true;
            m_open.set_value();
        }
    }

    std::future<void> entered;

private:
    RHIThread&               m_thread;
    RHIMessageWindow&        m_window;
    std::promise<void>       m_entered;
    std::promise<void>       m_open;
    std::shared_future<void> m_release;
    bool                     m_opened{false};
};

// Runs on RHI while the window thread waits, past the point where Windows ghosts hung windows.
bool IsHungAfterTimeout(HWND window)
{
    UINT timeoutMs = 5000;
    SystemParametersInfoW(SPI_GETHUNGAPPTIMEOUT, 0, &timeoutMs, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs + 500));
    return IsHungAppWindow(window) != FALSE;
}

void SleepDuringWait()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

uint64_t GetCurrentThreadCPUTime100ns()
{
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
    return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime)
         + ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}
#endif
} // namespace

namespace
{
void ExpectPoint(const RHISubmissionPoint& point, RHICommandContextType queue, uint64_t serial)
{
    RHISubmissionDependency resolved;
    ASSERT_EQ(point.Resolve(resolved), RHISubmissionPointStatus::eAccepted);
    EXPECT_EQ(resolved.queue, queue);
    EXPECT_EQ(resolved.serial, serial);
}

} // namespace

namespace
{
RHIQueueCapabilities DistinctComputeQueues()
{
    return {true, true, {0, 1, 2}};
}
} // namespace

namespace
{
struct TestOwnedSchedule
{
    HeapVector<RHICommandListPtr>   lists;
    HeapVector<RHISubmissionGroup>  groups;
    RefCountPtr<RHISubmissionState> state;

    TestOwnedSchedule(RHICommandListExecutor& executor, std::span<const RHICommandContextType> queues)
    {
        state = MakeRefCountPtr<RHISubmissionState>(queues);
        for (RHICommandContextType queue : queues)
        {
            lists.emplace_back(RHICommandList::Create(executor.GetCommandContext(queue)));
            groups.push_back({lists.back().get()});
        }
    }

    void Depends(uint32_t consumer, uint32_t producer)
    {
        groups[consumer].predecessors.push_back({lists[producer]->GetContext()->GetContextType(), 0, state, producer});
    }
};

} // namespace
