#include "ZenRHI/RHIDiagnostics.h"
#include <span>
#include "ZenRHI/RHICommandList.h"
#include "RHICommands.h"
#include "ZenFoundation/Utils/Errors.h"

namespace zen
{
RHIResourceReferences::~RHIResourceReferences()
{
    Reset();
}

RHIResourceReferences::RHIResourceReferences(RHIResourceReferences&& other) noexcept
{
    Swap(other);
}

RHIResourceReferences& RHIResourceReferences::operator=(RHIResourceReferences&& other) noexcept
{
    if (this != &other)
    {
        Reset();

        Swap(other);
    }

    return *this;
}

void RHIResourceReferences::Retain(RHIResource* resource)
{
    if (resource != nullptr && m_unique.try_emplace(resource, m_resources.size()).second)
    {
        m_resources.push_back(resource);

        resource->AddReference();

        if (resource->GetResourceType() == RHIResourceType::eTextureView)
        {
            Retain(static_cast<RHITextureView*>(resource)->GetTexture());
        }
    }
}

void RHIResourceReferences::Reset()
{
    Rollback(0);
}

size_t RHIResourceReferences::GetCount() const
{
    return m_resources.size();
}

void RHIResourceReferences::Rollback(size_t count)
{
    ZEN_RHI_VERIFY(count <= m_resources.size());

    for (size_t i = count; i < m_resources.size(); ++i)
    {
        RHIResource* resource = m_resources[i];

        m_unique.erase(resource);

        resource->ReleaseReference();
    }

    m_resources.resize(count);
}

void RHIResourceReferences::Swap(RHIResourceReferences& other)
{
    std::swap(m_resources, other.m_resources);

    m_unique.Swap(other.m_unique);
}

void IRHICommandContext::OnFinalRelease()
{
    // A rejected release never reaches the destroyed backend; non-strict checks leak the context.
    const bool accepted = DispatchCleanup([this] { ZEN_DELETE(this); });

    VerifyTeardownOwnership(accepted, "Context released after RHI cleanup admission closed", __FILE__, __LINE__);
}

void RHICommandListDeleter::operator()(RHICommandList* commands) const
{
    ZEN_DELETE(commands);
}

RHICommandListPtr RHICommandList::DetachCommands(RHICommandListPtr reusable)
{
    RHICommandListPtr result = std::move(reusable);

    if (result == nullptr)
    {
        result.reset(ZEN_NEW() RHICommandList());
    }

    ZEN_RHI_VERIFY(result->GetCommandCount() == 0);

    result->m_contextOwner = m_contextOwner;

    result->m_cmdAllocator.Swap(m_cmdAllocator);

    result->m_resources.Swap(m_resources);

    std::swap(result->m_submissionDependencies, m_submissionDependencies);

    std::swap(result->m_bindlessEpochs, m_bindlessEpochs);

    result->m_pCmdHead    = m_pCmdHead;

    result->m_ppCmdPtr    = m_numCommands == 0 ? &result->m_pCmdHead : m_ppCmdPtr;

    result->m_numCommands = m_numCommands;

    m_pCmdHead            = nullptr;

    m_ppCmdPtr            = &m_pCmdHead;

    m_numCommands         = 0;

    return result;
}

void RHICommandList::ResetForReuse()
{
    // A list without a context belongs to no device, so it has no RHI thread to check.
    if (const RHIDeviceRuntime* runtime = GetContext() != nullptr ? GetContext()->GetRuntime() : nullptr)
    {
        runtime->thread.CheckOwnership();
    }

    Reset();

    m_contextOwner.Reset();
}

uint64_t RHICommandList::CaptureBindlessEpoch()
{
    IRHICommandContext* context = GetContext();

    uint64_t epoch              = context != nullptr ? context->RHIGetCurrentBindlessEpoch() : 0;

    if (epoch != 0 && (m_bindlessEpochs.empty() || m_bindlessEpochs.back() != epoch))
    {
        epoch = context->RHICaptureBindlessEpoch();

        if (epoch != 0 && (m_bindlessEpochs.empty() || m_bindlessEpochs.back() != epoch))
        {
            m_bindlessEpochs.push_back(epoch);
        }
        else if (epoch != 0)
        {
            context->RHIReleaseBindlessEpoch(epoch);
        }
    }

    return epoch;
}

void RHICommandList::ReleaseBindlessEpochs(size_t count)
{
    ZEN_RHI_VERIFY(count <= m_bindlessEpochs.size());

    for (size_t index = count; index < m_bindlessEpochs.size(); ++index)
    {
        GetContext()->RHIReleaseBindlessEpoch(m_bindlessEpochs[index]);
    }

    m_bindlessEpochs.resize(count);
}

void RHICommandList::Execute()
{
    // A list without a context belongs to no device, so it has no RHI thread to check.
    if (const RHIDeviceRuntime* runtime = GetContext() != nullptr ? GetContext()->GetRuntime() : nullptr)
    {
        runtime->thread.CheckOwnership();
    }

    RHICommand* pCmd                  = m_pCmdHead;

    const IRHICommandContext* context = GetContext();

    while (pCmd)
    {
        RHICommand* pNext = pCmd->pNextCmd;

        if (context == nullptr || !context->HasRecordingError())
        {
            pCmd->Execute(*this);
        }

        pCmd = pNext;
    }
}

void RHICommandList::Reset()
{
    RollbackCommands({&m_pCmdHead, 0, 0, 0, 0});

    m_cmdAllocator.Reset();

    m_submittedSerial    = 0;

    m_submissionBoundary = false;
}

void RHICommandList::RollbackCommands(CommandCheckpoint checkpoint)
{
    RHICommand* command = *checkpoint.tail;

    while (command != nullptr)
    {
        RHICommand* next = command->pNextCmd;

        command->~RHICommand();

        command = next;
    }

    *checkpoint.tail = nullptr;

    m_ppCmdPtr       = checkpoint.tail;

    m_numCommands    = checkpoint.count;

    ReleaseBindlessEpochs(checkpoint.captureCount);

    m_resources.Rollback(checkpoint.resourceCount);

    m_submissionDependencies.resize(checkpoint.dependencyCount);
}

RHICommandList* RHICommandList::Create(IRHICommandContext* pContext)
{
    RHICommandList* pCmdList = nullptr;

    if (pContext != nullptr)
    {
        pCmdList                 = ZEN_NEW() RHICommandList();

        pCmdList->m_contextOwner = RefCountPtr<IRHICommandContext>(pContext);
    }

    return pCmdList;
}

void RHICommandList::ClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size)
{
    RetainResource(pBuffer);

    ALLOC_CMD(RHICommandClearBuffer)(pBuffer, offset, size);
}

void RHICommandList::BuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info)
{
    RetainResource(info.pDestination);
    RetainResource(info.pSource);
    RetainResource(info.pScratchBuffer);
    RetainResource(info.description.micromap.pDataBuffer);
    RetainResource(info.description.micromap.pTriangleBuffer);
    RHICommandBuildAccelerationStructure* command    = ALLOC_CMD(RHICommandBuildAccelerationStructure)(info);
    const size_t                          count      = info.description.geometries.size();
    RHIAccelerationStructureGeometry*     geometries = AllocateCmdData<RHIAccelerationStructureGeometry>(count);
    for (size_t i = 0; i < count; ++i)
    {
        geometries[i] = info.description.geometries[i];
        RetainResource(geometries[i].pVertexBuffer);
        RetainResource(geometries[i].pIndexBuffer);
        RetainResource(geometries[i].pOpacityIndexBuffer);
        RetainResource(geometries[i].pOpacityMicromap);
    }
    command->info.description.geometries            = MakeVecView(geometries, count);
    const size_t                      instanceCount = info.description.instances.size();
    RHIAccelerationStructureInstance* instances     = AllocateCmdData<RHIAccelerationStructureInstance>(instanceCount);
    for (size_t i = 0; i < instanceCount; ++i)
    {
        instances[i] = info.description.instances[i];
        RetainResource(instances[i].pStructure);
    }
    command->info.description.instances = MakeVecView(instances, instanceCount);
    const size_t             usageCount = info.description.micromap.usages.size();
    RHIOpacityMicromapUsage* usages     = AllocateCmdData<RHIOpacityMicromapUsage>(usageCount);
    for (size_t i = 0; i < usageCount; ++i)
    {
        usages[i] = info.description.micromap.usages[i];
    }
    command->info.description.micromap.usages = MakeVecView(usages, usageCount);
}

void RHICommandList::CopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region)
{
    RetainResource(pSrcBuffer);

    RetainResource(pDstBuffer);

    ALLOC_CMD(RHICommandCopyBuffer)(pSrcBuffer, pDstBuffer, region);
}

void RHICommandList::ClearTexture(RHITexture* pTexture, const Color& color, const RHITextureSubresourceRange& range)
{
    RetainResource(pTexture);

    ALLOC_CMD(RHICommandClearTexture)(pTexture, color, range);
}

void RHICommandList::CopyTexture(RHITexture*                           pSrcTexture,
                                 RHITexture*                           pDstTexture,
                                 std::span<const RHITextureCopyRegion> regions)
{
    RetainResource(pSrcTexture);

    RetainResource(pDstTexture);

    RHICommandCopyTexture* pCmd = ALLOC_CMD(RHICommandCopyTexture)(pSrcTexture, pDstTexture);

    pCmd->copyRegions           = CopyCmdData(regions);
}

void RHICommandList::BlitTexture(RHITexture*                     pSrcTexture,
                                 RHITexture*                     pDstTexture,
                                 std::span<RHITextureBlitRegion> regions,
                                 RHISamplerFilter                filter)
{
    RetainResource(pSrcTexture);

    RetainResource(pDstTexture);

    RHICommandBlitTexture* pCmd = ALLOC_CMD(RHICommandBlitTexture)(pSrcTexture, pDstTexture, filter);

    pCmd->blitRegions           = CopyCmdData(regions);
}

void RHICommandList::CopyTextureToBuffer(RHITexture*                           pSrcTex,
                                         RHIBuffer*                            pDstBuffer,
                                         std::span<RHIBufferTextureCopyRegion> regions)
{
    RetainResource(pSrcTex);

    RetainResource(pDstBuffer);

    RHICommandCopyTextureToBuffer* pCmd = ALLOC_CMD(RHICommandCopyTextureToBuffer)(pSrcTex, pDstBuffer);

    pCmd->copyRegions                   = CopyCmdData(regions);
}

void RHICommandList::CopyBufferToTexture(RHIBuffer*                                  pSrcBuffer,
                                         RHITexture*                                 pDstTexture,
                                         std::span<const RHIBufferTextureCopyRegion> regions)
{
    RetainResource(pSrcBuffer);

    RetainResource(pDstTexture);

    RHICommandCopyBufferToTexture* pCmd = ALLOC_CMD(RHICommandCopyBufferToTexture)(pSrcBuffer, pDstTexture);

    pCmd->copyRegions                   = CopyCmdData(regions);
}

void RHICommandList::ResolveTexture(RHITexture* pSrcTexture,
                                    RHITexture* pDstTexture,
                                    uint32_t    srcLayer,
                                    uint32_t    srcMipmap,
                                    uint32_t    dstLayer,
                                    uint32_t    dstMipmap)
{
    RetainResource(pSrcTexture);

    RetainResource(pDstTexture);

    ALLOC_CMD(RHICommandResolveTexture)(pSrcTexture, pDstTexture, srcLayer, srcMipmap, dstLayer, dstMipmap);
}

void RHICommandList::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetViewport)(minX, minY, maxX, maxY);
}

void RHICommandList::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetScissor)(minX, minY, maxX, maxY);
}

void RHICommandList::SetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
    ALLOC_CMD(RHICommandSetDepthBias)(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

void RHICommandList::SetLineWidth(float width)
{
    ALLOC_CMD(RHICommandSetLineWidth)(width);
}

void RHICommandList::BeginDebugLabel(std::string_view name)
{
    ALLOC_CMD(RHICommandDebugLabel)(name, true);
}

void RHICommandList::EndDebugLabel()
{
    ALLOC_CMD(RHICommandDebugLabel)(NameID(), false);
}

void RHICommandList::BeginGPUTiming(const RHIGPUTimingPtr& result)
{
    ALLOC_CMD(RHICommandGPUTiming)(result, true);
}

void RHICommandList::EndGPUTiming(const RHIGPUTimingPtr& result)
{
    ALLOC_CMD(RHICommandGPUTiming)(result, false);
}

void RHICommandList::SetBlendConstants(const Color& color)
{
    ALLOC_CMD(RHICommandSetBlendConstants)(color);
}

void RHICommandList::BeginRendering(const RHIRenderingLayout* pRenderingLayout)
{
    ZEN_RHI_VERIFY(pRenderingLayout != nullptr);

    for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; ++i)
    {
        RetainResource(pRenderingLayout->colorRenderTargets[i].pTextureView);

        RetainResource(pRenderingLayout->colorRenderTargets[i].pTexture);
    }

    if (pRenderingLayout->hasDepthStencilRT)
    {
        RetainResource(pRenderingLayout->depthStencilRenderTarget.pTextureView);

        RetainResource(pRenderingLayout->depthStencilRenderTarget.pTexture);
    }

    ALLOC_CMD(RHICommandBeginRendering)(pRenderingLayout);
}

void RHICommandList::EndRendering()
{
    ALLOC_CMD(RHICommandEndRendering)();
}

void RHICommandList::BindPipeline(RHIPipeline* pPipeline)
{
    RetainResource(pPipeline);

    ALLOC_CMD(RHICommandBindPipeline)(pPipeline);
}

void RHICommandList::SetShaderParameters(const RHIBatchedShaderParameters& parameters)
{
    for (const RHIShaderResourceParameter& parameter : parameters.GetResourceParams())
    {
        RetainResource(parameter.pResource);

        RetainResource(parameter.pAuxResource);
    }

    for (const RHIShaderResourceParameter& parameter : parameters.GetBindlessParams())
    {
        RetainResource(parameter.pResource);

        RetainResource(parameter.pAuxResource);
    }

    ALLOC_CMD(RHICommandSetShaderParameters)(*this, parameters.GetView(),
                                             parameters.GetBindlessParams().empty() ? 0 : CaptureBindlessEpoch());
}

void RHICommandList::BindVertexBuffers(std::span<RHIBuffer*> vertexBuffers, std::span<uint64_t> offsets)
{
    for (RHIBuffer* buffer : vertexBuffers)
    {
        RetainResource(buffer);
    }

    ZEN_RHI_VERIFY(vertexBuffers.size() == offsets.size());

    RHICommandBindVertexBuffers* pCmd = ALLOC_CMD(RHICommandBindVertexBuffers)();

    pCmd->vertexBuffers               = CopyCmdData(vertexBuffers);
    pCmd->offsets                     = CopyCmdData(offsets);
}

void RHICommandList::BindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)
{
    RetainResource(pBuffer);

    ALLOC_CMD(RHICommandBindVertexBuffer)(pBuffer, offset);
}

void RHICommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    ALLOC_CMD(RHICommandDraw)(vertexCount, instanceCount, firstVertex, firstInstance, CaptureBindlessEpoch());
}

void RHICommandList::DrawIndexed(const RHIDrawIndexedArgs& args)
{
    RetainResource(args.pIndexBuffer);

    ALLOC_CMD(RHICommandDrawIndexed)(args, CaptureBindlessEpoch());
}

void RHICommandList::DrawIndexedIndirect(const RHIDrawIndexedIndirectArgs& args)
{
    RetainResource(args.pIndexBuffer);

    RetainResource(args.pIndirectBuffer);

    ALLOC_CMD(RHICommandDrawIndexedIndirect)(args, CaptureBindlessEpoch());
}

void RHICommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    ALLOC_CMD(RHICommandDispatch)(groupCountX, groupCountY, groupCountZ, CaptureBindlessEpoch());
}

void RHICommandList::DispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)
{
    RetainResource(pIndirectBuffer);

    ALLOC_CMD(RHICommandDispatchIndirect)(pIndirectBuffer, offset, CaptureBindlessEpoch());
}

void RHICommandList::TraceRays(uint32_t width, uint32_t height, uint32_t depth)
{
    ALLOC_CMD(RHICommandTraceRays)(width, height, depth, CaptureBindlessEpoch());
}

void RHICommandList::TraceRaysIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)
{
    RetainResource(pIndirectBuffer);
    ALLOC_CMD(RHICommandTraceRaysIndirect)(pIndirectBuffer, offset, CaptureBindlessEpoch());
}

void RHICommandList::SetPushConstants(RHIPipeline* pPipeline, const uint8_t* pData, uint32_t sizeBytes, uint32_t offset)
{
    RetainResource(pPipeline);

    RHICommandSetPushConstants* pCmd = ALLOC_CMD(RHICommandSetPushConstants)(pPipeline);

    // An empty payload is rejected during backend validation.
    pCmd->data   = pData ? CopyCmdData(std::span(pData, sizeBytes)) : std::span<const uint8_t>{};

    pCmd->offset = offset;
}

void RHICommandList::AddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages,
                                    RHIFlags<RHIPipelineStageFlagBits> dstStages,
                                    std::span<RHIMemoryTransition>     memoryTransitions,
                                    std::span<RHIBufferTransition>     bufferTransitions,
                                    std::span<RHITextureTransition>    textureTransitions)
{
    for (const RHIBufferTransition& transition : bufferTransitions)
    {
        RetainResource(transition.pBuffer);
    }

    for (const RHITextureTransition& transition : textureTransitions)
    {
        RetainResource(transition.pTexture);
    }

    RHICommandAddTransitions* pCmd = ALLOC_CMD(RHICommandAddTransitions)(srcStages, dstStages);

    pCmd->memoryTransitions        = CopyCmdData(memoryTransitions);
    pCmd->bufferTransitions        = CopyCmdData(bufferTransitions);
    pCmd->textureTransitions       = CopyCmdData(textureTransitions);
}

} // namespace zen
