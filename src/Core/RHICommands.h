#pragma once
#include "ZenRHI/RHICommandList.h"

// Recorded commands are private to RHICommandList. Each one replays a single IRHICommandContext call.
#define ALLOC_CMD(...) AllocateCmdTyped<__VA_ARGS__>

namespace zen
{
struct RHICommandWithBindlessEpoch : public RHICommand
{
    explicit RHICommandWithBindlessEpoch(uint64_t epoch) : m_bindlessEpoch(epoch) {}

    RHICommandWithBindlessEpoch(const RHICommandWithBindlessEpoch&)            = delete;

    RHICommandWithBindlessEpoch& operator=(const RHICommandWithBindlessEpoch&) = delete;

    void Execute(RHICommandList& cmdList) final
    {
        IRHICommandContext* context = cmdList.GetContext();
        context->RHISetRecordedBindlessEpoch(m_bindlessEpoch);

        ExecuteCommand(cmdList);

        context->RHISetRecordedBindlessEpoch(0);
    }

    virtual void ExecuteCommand(RHICommandList& cmdList) = 0;

private:
    uint64_t m_bindlessEpoch;
};

struct RHICommandClearBuffer : public RHICommand
{
    RHIBuffer* pBuffer;
    uint64_t   offset;
    uint64_t   size;

    RHICommandClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size) : pBuffer(pBuffer), offset(offset), size(size) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIClearBuffer(pBuffer, offset, size);
    }
};

struct RHICommandBuildAccelerationStructure : public RHICommand
{
    RHIAccelerationStructureBuildInfo info;

    explicit RHICommandBuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& buildInfo) : info(buildInfo) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIBuildAccelerationStructure(info);
    }
};

struct RHICommandCopyBuffer : public RHICommand
{
    RHIBuffer*          pSrcBuffer;
    RHIBuffer*          pDstBuffer;
    RHIBufferCopyRegion copyRegion;

    RHICommandCopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region) :
        pSrcBuffer(pSrcBuffer), pDstBuffer(pDstBuffer), copyRegion(region)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHICopyBuffer(pSrcBuffer, pDstBuffer, copyRegion);
    }
};

struct RHICommandClearTexture : public RHICommand
{
    RHITexture*                pTexture;
    Color                      clearColor;
    RHITextureSubresourceRange range;

    RHICommandClearTexture(RHITexture* pTexture, const Color& color, const RHITextureSubresourceRange& range) :
        pTexture(pTexture), clearColor(color), range(range)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIClearTexture(pTexture, clearColor, range);
    }
};

struct RHICommandCopyTexture : public RHICommand
{
    RHITexture*                     pSrcTexture;
    RHITexture*                     pDstTexture;
    std::span<RHITextureCopyRegion> copyRegions;

    RHICommandCopyTexture(RHITexture* pSrcTexture, RHITexture* pDstTexture) : pSrcTexture(pSrcTexture), pDstTexture(pDstTexture)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHICopyTexture(pSrcTexture, pDstTexture, copyRegions);
    }
};

struct RHICommandBlitTexture : public RHICommand
{
    RHITexture*                     pSrcTexture;
    RHITexture*                     pDstTexture;
    std::span<RHITextureBlitRegion> blitRegions;
    RHISamplerFilter                filter;

    RHICommandBlitTexture(RHITexture* pSrcTexture, RHITexture* pDstTexture, RHISamplerFilter filter) :
        pSrcTexture(pSrcTexture), pDstTexture(pDstTexture), filter(filter)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIBlitTexture(pSrcTexture, pDstTexture, blitRegions, filter);
    }
};

struct RHICommandCopyBufferToTexture : public RHICommand
{
    RHIBuffer*                            pSrcBuffer;
    RHITexture*                           pDstTexture;
    std::span<RHIBufferTextureCopyRegion> copyRegions;

    RHICommandCopyBufferToTexture(RHIBuffer* pSrcBuffer, RHITexture* pDstTexture) :
        pSrcBuffer(pSrcBuffer), pDstTexture(pDstTexture)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHICopyBufferToTexture(pSrcBuffer, pDstTexture, copyRegions);
    }
};

struct RHICommandCopyTextureToBuffer : public RHICommand
{
    RHITexture*                           pSrcTexture;
    RHIBuffer*                            pDstBuffer;
    std::span<RHIBufferTextureCopyRegion> copyRegions;

    RHICommandCopyTextureToBuffer(RHITexture* pSrcTexture, RHIBuffer* pDstBuffer) :
        pSrcTexture(pSrcTexture), pDstBuffer(pDstBuffer)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHICopyTextureToBuffer(pSrcTexture, pDstBuffer, copyRegions);
    }
};

struct RHICommandResolveTexture : public RHICommand
{
    RHITexture* pSrcTexture;
    RHITexture* pDstTexture;
    uint32_t    srcLayer;
    uint32_t    srcMipmap;
    uint32_t    dstLayer;
    uint32_t    dstMipmap;

    RHICommandResolveTexture(RHITexture* pSrcTexture,
                             RHITexture* pDstTexture,
                             uint32_t    srcLayer,
                             uint32_t    srcMipmap,
                             uint32_t    dstLayer,
                             uint32_t    dstMipmap) :
        pSrcTexture(pSrcTexture),
        pDstTexture(pDstTexture),
        srcLayer(srcLayer),
        srcMipmap(srcMipmap),
        dstLayer(dstLayer),
        dstMipmap(dstMipmap)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIResolveTexture(pSrcTexture, pDstTexture, srcLayer, srcMipmap, dstLayer, dstMipmap);
    }
};

struct RHICommandBeginRendering final : public RHICommand
{
    RHIRenderingLayout renderingLayout;

    explicit RHICommandBeginRendering(const RHIRenderingLayout* pRenderingLayout) : renderingLayout(*pRenderingLayout) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIBeginRendering(&renderingLayout);
    }
};

struct RHICommandEndRendering final : public RHICommand
{
    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIEndRendering();
    }
};

struct RHICommandDebugLabel final : public RHICommand
{
private:
    NameID name;
    bool   begin;

public:
    RHICommandDebugLabel(std::string_view label, bool isBegin) : name(label), begin(isBegin) {}

    void Execute(RHICommandList& cmdList) override
    {
        if (begin)
        {
            cmdList.GetContext()->RHIBeginDebugLabel(name);
        }
        else
        {
            cmdList.GetContext()->RHIEndDebugLabel();
        }
    }
};

struct RHICommandGPUTiming final : public RHICommand
{
    RHIGPUTimingPtr result;
    bool            begin;
    bool            executed{false};

    RHICommandGPUTiming(const RHIGPUTimingPtr& timing, bool isBegin) : result(timing), begin(isBegin) {}

    ~RHICommandGPUTiming() override
    {
        if (begin && !executed && result != nullptr)
        {
            result->Publish(RHIGPUTimingStatus::eDiscarded);
        }
    }

    void Execute(RHICommandList& cmdList) override
    {
        if (begin)
        {
            cmdList.GetContext()->RHIBeginGPUTiming(result);
        }
        else
        {
            cmdList.GetContext()->RHIEndGPUTiming(result);
        }
        executed = true;
    }
};

struct RHICommandBindPipeline final : public RHICommand
{
    RHIPipeline* pPipeline;

    explicit RHICommandBindPipeline(RHIPipeline* pPipeline) : pPipeline(pPipeline) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIBindPipeline(pPipeline);
    }
};

struct RHICommandSetShaderParameters final : public RHICommandWithBindlessEpoch
{
    RHIShaderParameterView parameters;

    RHICommandSetShaderParameters(RHICommandList& list, RHIShaderParameterView source, uint64_t epoch = 0) :
        RHICommandWithBindlessEpoch(epoch)
    {
        parameters.valueParameters    = list.CopyCmdData(source.valueParameters);

        parameters.valueBytes         = list.CopyCmdData(source.valueBytes);

        parameters.resourceParameters = list.CopyCmdData(source.resourceParameters);

        parameters.bindlessParameters = list.CopyCmdData(source.bindlessParameters);
    }

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHISetShaderParameters(parameters);
    }
};

struct RHICommandSetScissor final : public RHICommand
{
    uint32_t minX;
    uint32_t minY;
    uint32_t maxX;
    uint32_t maxY;

    RHICommandSetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) :
        minX(minX), minY(minY), maxX(maxX), maxY(maxY)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHISetScissor(minX, minY, maxX, maxY);
    }
};

struct RHICommandSetViewport final : public RHICommand
{
    uint32_t minX;
    uint32_t minY;
    uint32_t maxX;
    uint32_t maxY;

    RHICommandSetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) :
        minX(minX), minY(minY), maxX(maxX), maxY(maxY)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHISetViewport(minX, minY, maxX, maxY);
    }
};

struct RHICommandSetDepthBias : public RHICommand
{
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;

    RHICommandSetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor) :
        depthBiasConstantFactor(depthBiasConstantFactor),
        depthBiasClamp(depthBiasClamp),
        depthBiasSlopeFactor(depthBiasSlopeFactor)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHISetDepthBias(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
    }
};

struct RHICommandSetLineWidth : public RHICommand
{
    float lineWidth;

    explicit RHICommandSetLineWidth(float lineWidth) : lineWidth(lineWidth) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHISetLineWidth(lineWidth);
    }
};

struct RHICommandSetBlendConstants : public RHICommand
{
    Color blendConstants;

    explicit RHICommandSetBlendConstants(Color blendConstants) : blendConstants(std::move(blendConstants)) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHISetBlendConstants(blendConstants);
    }
};

struct RHICommandBindVertexBuffer final : public RHICommand
{
    RHIBuffer* pVertexBuffer;
    uint64_t   offset;

    RHICommandBindVertexBuffer(RHIBuffer* pVertexBuffer, uint64_t offset) : pVertexBuffer(pVertexBuffer), offset(offset) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIBindVertexBuffer(pVertexBuffer, offset);
    }
};

struct RHICommandBindVertexBuffers final : public RHICommand
{
    std::span<RHIBuffer*> vertexBuffers;
    std::span<uint64_t>   offsets;

    RHICommandBindVertexBuffers() = default;

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIBindVertexBuffers(vertexBuffers, offsets);
    }
};

struct RHICommandDraw final : public RHICommandWithBindlessEpoch
{
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;

    RHICommandDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance, uint64_t epoch) :
        RHICommandWithBindlessEpoch(epoch),
        vertexCount(vertexCount),
        instanceCount(instanceCount),
        firstVertex(firstVertex),
        firstInstance(firstInstance)
    {}

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIDraw(vertexCount, instanceCount, firstVertex, firstInstance);
    }
};

struct RHICommandDrawIndexed final : public RHICommandWithBindlessEpoch
{
    RHIDrawIndexedArgs args;

    RHICommandDrawIndexed(const RHIDrawIndexedArgs& args, uint64_t epoch) : RHICommandWithBindlessEpoch(epoch), args(args) {}

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexed(args.pIndexBuffer, args.indexFormat, args.indexBufferOffset, args.indexCount,
                                             args.instanceCount, args.firstIndex, args.vertexOffset, args.firstInstance);
    }
};

struct RHICommandDrawIndexedIndirect final : public RHICommandWithBindlessEpoch
{
    RHIDrawIndexedIndirectArgs args;

    RHICommandDrawIndexedIndirect(const RHIDrawIndexedIndirectArgs& args, uint64_t epoch) :
        RHICommandWithBindlessEpoch(epoch), args(args)
    {}

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexedIndirect(args.pIndirectBuffer, args.pIndexBuffer, args.indexFormat,
                                                     args.indexBufferOffset, args.offset, args.drawCount, args.stride);
    }
};

struct RHICommandDispatch final : public RHICommandWithBindlessEpoch
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;

    RHICommandDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ, uint64_t epoch) :
        RHICommandWithBindlessEpoch(epoch), groupCountX(groupCountX), groupCountY(groupCountY), groupCountZ(groupCountZ)
    {}

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIDispatch(groupCountX, groupCountY, groupCountZ);
    }
};

struct RHICommandDispatchIndirect final : public RHICommandWithBindlessEpoch
{
    RHIBuffer* pIndirectBuffer;
    uint64_t   offset;

    RHICommandDispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset, uint64_t epoch) :
        RHICommandWithBindlessEpoch(epoch), pIndirectBuffer(pIndirectBuffer), offset(offset)
    {}

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIDispatchIndirect(pIndirectBuffer, offset);
    }
};

struct RHICommandTraceRays final : public RHICommandWithBindlessEpoch
{
    uint32_t width;
    uint32_t height;
    uint32_t depth;

    RHICommandTraceRays(uint32_t width, uint32_t height, uint32_t depth, uint64_t epoch) :
        RHICommandWithBindlessEpoch(epoch), width(width), height(height), depth(depth)
    {}

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHITraceRays(width, height, depth);
    }
};

struct RHICommandTraceRaysIndirect final : public RHICommandWithBindlessEpoch
{
    RHIBuffer* buffer;
    uint64_t   offset;

    RHICommandTraceRaysIndirect(RHIBuffer* buffer, uint64_t offset, uint64_t epoch) :
        RHICommandWithBindlessEpoch(epoch), buffer(buffer), offset(offset)
    {}

    void ExecuteCommand(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHITraceRaysIndirect(buffer, offset);
    }
};

struct RHICommandSetPushConstants final : public RHICommand
{
    RHIPipeline*             pPipeline;
    std::span<const uint8_t> data;
    uint32_t                 offset{0};

    explicit RHICommandSetPushConstants(RHIPipeline* pPipeline) : pPipeline(pPipeline) {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHISetPushConstants(pPipeline, data, offset);
    }
};

struct RHICommandAddTransitions final : public RHICommand
{
    RHIFlags<RHIPipelineStageFlagBits> srcStages;
    RHIFlags<RHIPipelineStageFlagBits> dstStages;

    std::span<RHIMemoryTransition>  memoryTransitions;
    std::span<RHIBufferTransition>  bufferTransitions;
    std::span<RHITextureTransition> textureTransitions;

    RHICommandAddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages, RHIFlags<RHIPipelineStageFlagBits> dstStages) :
        srcStages(srcStages), dstStages(dstStages)
    {}

    void Execute(RHICommandList& cmdList) override
    {
        cmdList.GetContext()->RHIAddTransitions(srcStages, dstStages, memoryTransitions, bufferTransitions, textureTransitions);
    }
};
} // namespace zen
