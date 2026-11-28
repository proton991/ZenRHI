#include "ZenRHI/RHIDiagnostics.h"
#include <span>
#include "ZenRHI/RHICommon.h"
#include "Vulkan/VulkanDescriptorAllocator.h"
#include "Vulkan/VulkanBindlessHeap.h"
#include "Vulkan/VulkanDescriptorState.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanUniformRing.h"
#include "Vulkan/VulkanAccelerationStructure.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanTexture.h"
#include "Vulkan/VulkanTypes.h"
#include "ZenFoundation/Templates/VectorView.h"
#include "ZenFoundation/Utils/Errors.h"
#include "ZenFoundation/Utils/Helpers.h"
#include <algorithm>
#include <cstring>

namespace zen
{
namespace
{
VkImageView GetImageView(RHIResource* pResource, RHITextureUsageFlagBits requiredUsage)
{
    VkImageView              imageView = VK_NULL_HANDLE;
    const VulkanTextureView* view      = TryVulkanTextureView(pResource);

    if (view != nullptr && view->GetTexture()->GetBaseInfo().usageFlags.HasFlag(requiredUsage))
    {
        imageView = view->GetVkImageView();
    }

    return imageView;
}

struct DescriptorWriteBatch
{
    struct Payload
    {
        VkDescriptorImageInfo                        image{};
        VkDescriptorBufferInfo                       buffer{};
        VkBufferView                                 bufferView{VK_NULL_HANDLE};
        VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo{};
        VkAccelerationStructureKHR                   accelerationStructure{VK_NULL_HANDLE};
    };

    explicit DescriptorWriteBatch(uint32_t maxDescriptorCount) : writes(maxDescriptorCount), payloads(maxDescriptorCount) {}

    HeapVector<VkWriteDescriptorSet> writes;
    HeapVector<Payload>              payloads;
    uint32_t                         numWrites{0};
};

// The offset is aligned and the range fits the buffer from that offset.
bool BufferElementFits(VulkanRHI& rhi, RHIShaderResourceType type, const VulkanBuffer& buffer, uint64_t range, uint64_t offset)
{
    const VkPhysicalDeviceLimits& limits = rhi.GetDevice()->GetPhysicalDeviceProperties().limits;
    const VkDeviceSize alignment = type == RHIShaderResourceType::eUniformBuffer ? limits.minUniformBufferOffsetAlignment
                                                                                 : limits.minStorageBufferOffsetAlignment;
    const uint64_t     size      = buffer.GetRequiredSize();

    return offset <= size && range <= size - offset && (offset == 0 || offset % alignment == 0);
}

// Dynamic offsets are bind-time state that the content key leaves out, so a cache hit checks them again.
bool DynamicOffsetsFit(VulkanRHI&                      rhi,
                       const RHIShaderResourceBinding& binding,
                       uint32_t                        valueRange,
                       std::span<const uint32_t>       offsets)
{
    bool fits = true;

    for (uint32_t index = 0; fits && index < offsets.size() && index < binding.resources.size(); ++index)
    {
        const VulkanBuffer* buffer = TryVulkanBuffer(binding.resources[index]);

        fits                       = buffer == nullptr
                                  || BufferElementFits(rhi, binding.type, *buffer, valueRange > 0 ? valueRange : buffer->GetRequiredSize(),
                                                       offsets[index]);
    }

    return fits;
}

// Resolves one array element into payload and points write at it. False when the resources do not fit the binding.
bool ResolveElement(VulkanRHI&                      rhi,
                    const RHIShaderResourceBinding& binding,
                    uint32_t                        element,
                    uint32_t                        valueRange,
                    std::span<const uint32_t>       offsets,
                    DescriptorWriteBatch::Payload&  payload,
                    VkWriteDescriptorSet&           write)
{
    const VulkanDescriptorTraits& traits    = GetVulkanDescriptorTraits(binding.type);
    const uint32_t                index     = element * traits.stride;
    RHIResource*                  resource  = binding.resources[index];
    RHIResource*                  auxiliary = traits.stride == 2 ? binding.resources[index + 1] : nullptr;
    bool                          valid     = false;

    payload                                 = {};

    switch (binding.type)
    {
        case RHIShaderResourceType::eAccelerationStructure:
        {
            const VulkanAccelerationStructure* structure = TryVulkanAccelerationStructure(resource);

            payload.accelerationStructure                = structure != nullptr ? structure->GetVkHandle() : VK_NULL_HANDLE;
            valid                                        = payload.accelerationStructure != VK_NULL_HANDLE
                                                        && structure->GetType() == RHIAccelerationStructureType::eTopLevel;
            payload.accelerationInfo                     = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
            payload.accelerationInfo.accelerationStructureCount = 1;
            payload.accelerationInfo.pAccelerationStructures    = &payload.accelerationStructure;
            write.pNext                                         = &payload.accelerationInfo;
            break;
        }
        case RHIShaderResourceType::eSampler:
        case RHIShaderResourceType::eSamplerWithTexture:
        {
            const VulkanSampler* sampler = TryVulkanSampler(resource);

            payload.image.sampler        = sampler != nullptr ? sampler->GetVkSampler() : VK_NULL_HANDLE;
            valid                        = payload.image.sampler != VK_NULL_HANDLE;
            if (binding.type == RHIShaderResourceType::eSamplerWithTexture)
            {
                payload.image.imageView   = GetImageView(auxiliary, traits.textureUsage);
                payload.image.imageLayout = traits.imageLayout;
                valid                     = valid && payload.image.imageView != VK_NULL_HANDLE;
            }
            write.pImageInfo = &payload.image;
            break;
        }
        case RHIShaderResourceType::eTexture:
        case RHIShaderResourceType::eImage:
        case RHIShaderResourceType::eInputAttachment:
            payload.image.imageView   = GetImageView(resource, traits.textureUsage);
            payload.image.imageLayout = traits.imageLayout;
            valid                     = payload.image.imageView != VK_NULL_HANDLE;
            write.pImageInfo          = &payload.image;
            break;
        case RHIShaderResourceType::eTextureBuffer:
        case RHIShaderResourceType::eSamplerWithTextureBuffer:
        case RHIShaderResourceType::eImageBuffer:
        {
            const VulkanBuffer* buffer = TryVulkanBuffer(traits.stride == 2 ? auxiliary : resource);

            payload.bufferView         = buffer != nullptr ? buffer->GetVkBufferView() : VK_NULL_HANDLE;
            valid                  = payload.bufferView != VK_NULL_HANDLE && buffer->GetUsageFlags().HasFlag(traits.bufferUsage)
                                  && (traits.stride != 2 || TryVulkanSampler(resource) != nullptr);
            write.pTexelBufferView = &payload.bufferView;
            break;
        }
        case RHIShaderResourceType::eUniformBuffer:
        case RHIShaderResourceType::eStorageBuffer:
        {
            const VulkanBuffer* buffer = TryVulkanBuffer(resource);
            if (buffer != nullptr)
            {
                const VkPhysicalDeviceLimits& limits   = rhi.GetDevice()->GetPhysicalDeviceProperties().limits;
                const VkDeviceSize            maxRange = binding.type == RHIShaderResourceType::eUniformBuffer
                                                           ? limits.maxUniformBufferRange
                                                           : limits.maxStorageBufferRange;

                payload.buffer.buffer                  = buffer->GetVkBuffer();
                payload.buffer.range                   = valueRange > 0 ? valueRange : buffer->GetRequiredSize();
                valid = buffer->GetUsageFlags().HasFlag(traits.bufferUsage) && payload.buffer.buffer != VK_NULL_HANDLE
                     && payload.buffer.range > 0 && payload.buffer.range <= maxRange
                     && BufferElementFits(rhi, binding.type, *buffer, payload.buffer.range,
                                          index < offsets.size() ? offsets[index] : 0);
            }
            write.pBufferInfo = &payload.buffer;
            break;
        }
        default: break;
    }

    return valid;
}

// Checks every element of a binding and appends one write per element. Unset elements of a descriptor array stay
// unwritten; their bindings are partially bound.
bool ResolveDescriptorBinding(VulkanRHI&                      rhi,
                              const RHIShaderResourceBinding& binding,
                              uint32_t                        arraySize,
                              uint32_t                        valueRange,
                              std::span<const uint32_t>       offsets,
                              DescriptorWriteBatch&           batch)
{
    const VulkanDescriptorTraits& traits = GetVulkanDescriptorTraits(binding.type);
    const uint32_t                count  = static_cast<uint32_t>(binding.resources.size()) / traits.stride;
    bool valid = binding.type < RHIShaderResourceType::eMax && count > 0 && binding.resources.size() % traits.stride == 0;

    for (uint32_t element = 0; valid && element < count; ++element)
    {
        const uint32_t index = element * traits.stride;
        const bool     unset =
            binding.resources[index] == nullptr && (traits.stride == 1 || binding.resources[index + 1] == nullptr);

        if (!unset || arraySize <= 1)
        {
            ZEN_RHI_VERIFY(batch.numWrites < batch.writes.size());

            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstBinding      = binding.binding;
            write.dstArrayElement = element;
            write.descriptorCount = 1;
            write.descriptorType  = traits.type;

            valid = ResolveElement(rhi, binding, element, valueRange, offsets, batch.payloads[batch.numWrites], write);

            if (valid)
            {
                batch.writes[batch.numWrites++] = write;
            }
        }
    }

    return valid;
}
} // namespace

void VulkanDescriptorSetState::SetPipeline(VulkanPipeline* pPipeline)
{
    const uint64_t identity = pPipeline != nullptr ? pPipeline->GetStableId() : 0;

    if (m_pPipeline != pPipeline || m_pipelineIdentity != identity)
    {
        ClearAllSetStates();

        // Per-set state covers exactly the bound shader's layout; the device bounds that count.
        const VulkanShader* shader = pPipeline != nullptr ? TO_CVK_SHADER(pPipeline->GetShader()) : nullptr;

        m_setStates.resize(shader != nullptr ? shader->GetNumDescriptorSetLayouts() : 0);

        m_packedValueBuffers.clear();

        m_cacheRevision          = 0;

        m_pPipeline              = pPipeline;
        m_pipelineIdentity       = identity;

        m_parametersValid        = true;

        m_lastWorkloadGeneration = 0;
    }
}

bool VulkanDescriptorSetState::ValidateResourceParameter(const RHIShaderResourceParameter& param)
{
    RHIShader* shader                             = m_pPipeline->GetShader();

    const RHIShaderResourceDescriptor* descriptor = shader->GetSRDByLocation(param.set, param.binding);

    const uint64_t resourceCount = (uint64_t(param.arrayIndex) + 1) * GetVulkanDescriptorTraits(param.resourceType).stride;

    const char* problem          = nullptr;

    if (param.set >= m_setStates.size() || descriptor == nullptr)
    {
        problem = "the shader declares no such binding";
    }
    else if (descriptor->bindless)
    {
        problem = "the binding belongs to the global bindless heap";
    }
    else if (descriptor->type != param.resourceType)
    {
        problem = "the resource type differs from the reflected type";
    }
    else if (param.arrayIndex >= descriptor->arraySize || resourceCount > UINT32_MAX)
    {
        problem = "the element is outside the reflected array";
    }
    else if (param.bufferOffset != 0 && param.resourceType != RHIShaderResourceType::eUniformBuffer)
    {
        problem = "only uniform-buffer parameters accept a buffer offset";
    }

    if (problem != nullptr)
    {
        ZEN_RHI_LOG_ERROR("Shader '{}' rejected the resource parameter at set {}, binding {}, element {}: {}",
                          shader->GetName(), param.set, param.binding, param.arrayIndex, problem);
    }

    return problem == nullptr;
}

bool VulkanDescriptorSetState::SetShaderParameters(RHIShaderParameterView parameters,
                                                   uint64_t               recordedEpoch,
                                                   uint64_t               transaction)
{
    VulkanBindlessHeap* manager = m_pRHI->GetBindlessHeap();

    bool valid                  = m_parametersValid && m_pPipeline != nullptr;

    for (const RHIShaderValueParameter& parameter : parameters.GetValueParams())
    {
        valid &= parameter.set < m_setStates.size();
    }

    for (const RHIShaderResourceParameter& parameter : parameters.GetResourceParams())
    {
        valid &= m_pPipeline != nullptr && ValidateResourceParameter(parameter);
    }

    const std::span<const RHIShaderResourceParameter> bindless = parameters.GetBindlessParams();

    if (valid && !bindless.empty())
    {
        valid = manager->CanRegisterBindlessResources(bindless, recordedEpoch, transaction);
    }

    if (valid)
    {
        for (const RHIShaderValueParameter& parameter : parameters.GetValueParams())
        {
            const std::span<const uint8_t> bytes = parameters.GetValueBytes(parameter);

            if (!bytes.empty())
            {
                SetPackedValueParameter(parameter.set, parameter.binding, parameter.byteSize, bytes.data());
            }
        }

        for (const RHIShaderResourceParameter& parameter : parameters.GetResourceParams())
        {
            WriteResourceParameter(parameter);
        }

        for (const RHIShaderResourceParameter& parameter : bindless)
        {
            valid &= manager->RegisterBindlessResource(parameter.pResource, parameter.arrayIndex, nullptr, recordedEpoch,
                                                       transaction);
        }
    }

    m_parametersValid &= valid;

    return m_parametersValid;
}

RHIStatus VulkanDescriptorSetState::FlushPendingDescriptorWrites(VulkanCommandListContext*    pContext,
                                                                 HeapVector<VkDescriptorSet>& outDescriptorSets,
                                                                 uint32_t&                    outFirstSet,
                                                                 HeapVector<uint32_t>&        outDynamicOffsets)
{
    bool      valid = m_parametersValid && pContext != nullptr && m_pPipeline != nullptr;
    RHIStatus status{valid ? RHIError{}
                           : MakeRHIError(RHIErrorCode::eInvalidArgument, "Prepare shader descriptors", __FILE__, __LINE__)};

    if (valid)
    {
        VulkanShader* shader = TO_VK_SHADER(m_pPipeline->GetShader());

        if (shader != nullptr && shader->HasGlobalBindlessSet())
        {
            pContext->RecordCurrentBindlessEpoch();

            m_pRHI->GetBindlessHeap()->Flush();
        }

        const VulkanDescriptorAllocator* cache = m_pRHI->GetDescriptorAllocator();

        bool reuse                             = m_lastContext == pContext && m_lastWorkloadGeneration != 0
                                              && m_lastWorkloadGeneration == pContext->GetWorkloadGeneration()
                                              && m_lastCacheRevision == cache->GetRevision();

        const VulkanUniformRing* allocator     = m_pRHI->GetUniformRing();

        for (const SetState& set : m_setStates)
        {
            reuse &= !set.dirty;

            for (const BindingState& binding : set.bindings)
            {
                reuse &= binding.uniformBlockId == 0
                      || allocator->GetBlockGeneration(binding.uniformBlockId) == binding.uniformGeneration;
            }
        }

        for (const PackedValueBufferState& value : m_packedValueBuffers)
        {
            reuse &= !value.dirty;
        }

        if (reuse)
        {
            outDescriptorSets = m_resolvedSets;

            outDynamicOffsets = m_resolvedOffsets;

            outFirstSet       = m_resolvedFirstSet;
        }
        else
        {
            outDescriptorSets.clear();

            outDynamicOffsets.clear();

            outFirstSet = 0;

            status      = FlushPackedValueBuffers();
            valid       = static_cast<bool>(status);

            if (valid)
            {
                status = BuildDescriptorSetList(pContext, outDescriptorSets, outFirstSet, outDynamicOffsets);
                valid  = static_cast<bool>(status);
            }

            m_resolvedSets           = outDescriptorSets;

            m_resolvedOffsets        = outDynamicOffsets;

            m_resolvedFirstSet       = outFirstSet;

            m_lastContext            = pContext;

            m_lastWorkloadGeneration = pContext->GetWorkloadGeneration();

            m_lastCacheRevision      = cache->GetRevision();
        }
    }

    if (!valid)
    {
        outDescriptorSets.clear();

        outDynamicOffsets.clear();

        outFirstSet              = 0;

        m_lastWorkloadGeneration = 0;
    }

    return status;
}

void VulkanDescriptorSetState::Reset()
{
    m_parametersValid = true;

    ClearAllSetStates();

    m_setStates.clear();

    m_packedValueBuffers.clear();


    m_cacheRevision          = 0;

    m_pPipeline              = nullptr;
    m_pipelineIdentity       = 0;

    m_lastWorkloadGeneration = 0;

    m_resolvedSets.clear();

    m_resolvedOffsets.clear();
}

RHIStatus VulkanDescriptorSetState::ResolveSet(uint32_t setIdx)
{
    RHIStatus status{MakeRHIError(RHIErrorCode::eInvalidArgument, "Prepare shader descriptors", __FILE__, __LINE__)};

    if (ValidateSetState(setIdx))
    {
        SetState&     setState = m_setStates[setIdx];
        VulkanShader* shader   = TO_VK_SHADER(m_pPipeline->GetShader());

        if (setIdx == kGlobalBindlessHeapIndex && shader->HasGlobalBindlessSet())
        {
            setState.vkSet      = m_pRHI->GetBindlessHeap()->GetGlobalBindlessSet();
            setState.lifetimeId = 0;
            status.error        = setState.vkSet != VK_NULL_HANDLE ? RHIError{} : status.error;
        }
        else
        {
            const VulkanDescriptorAllocator::Set set = ResolveOrdinarySet(setIdx, *shader, status);

            setState.vkSet                           = status ? set.handle : VK_NULL_HANDLE;
            setState.lifetimeId                      = status ? set.lifetimeId : 0;
        }

        setState.dirty = !static_cast<bool>(status);
    }

    return status;
}

VulkanDescriptorAllocator::Set VulkanDescriptorSetState::ResolveOrdinarySet(uint32_t      setIdx,
                                                                            VulkanShader& shader,
                                                                            RHIStatus&    status)
{
    const SetState&                setState  = m_setStates[setIdx];
    VulkanDescriptorAllocator*     allocator = m_pRHI->GetDescriptorAllocator();
    VulkanDescriptorAllocator::Key key{};
    VulkanDescriptorAllocator::Set set{};

    if (allocator != nullptr && BuildContentKey(setIdx, key))
    {
        set                          = allocator->Find(key);

        const bool needsWrite        = set.handle == VK_NULL_HANDLE;

        uint32_t maxDescriptorWrites = 0;

        for (uint32_t i = 0; needsWrite && i < setState.bindings.size(); ++i)
        {
            const RHIShaderResourceBinding& binding = setState.bindings[i].srb;
            maxDescriptorWrites +=
                static_cast<uint32_t>(binding.resources.size()) / GetVulkanDescriptorTraits(binding.type).stride;
        }

        DescriptorWriteBatch writeBatch(maxDescriptorWrites);

        bool valid = true;

        // A hit holds resources and ranges that were validated when its set was written, as the key names both; only
        // the dynamic offsets can differ. A miss validates every element while it builds the writes.
        for (const BindingState& binding : setState.bindings)
        {
            valid = valid
                 && (needsWrite ? ResolveDescriptorBinding(*m_pRHI, binding.srb,
                                                           shader.GetSRDByLocation(setIdx, binding.srb.binding)->arraySize,
                                                           binding.valueRange, binding.dynamicOffsets, writeBatch)
                                : DynamicOffsetsFit(*m_pRHI, binding.srb, binding.valueRange, binding.dynamicOffsets));
        }

        if (!valid)
        {
            ZEN_RHI_LOG_ERROR("Invalid Vulkan descriptor binding resources");
        }
        else if (needsWrite)
        {
            set          = allocator->Insert(key, shader.GetSetLayout(setIdx),
                                             std::span<VkWriteDescriptorSet>(writeBatch.writes.data(), writeBatch.numWrites));

            status.error = set.handle != VK_NULL_HANDLE
                             ? RHIError{}
                             : RHIErrorOr(set.error, MakeRHIError(RHIErrorCode::eBackendFailure, "Allocate shader descriptors",
                                                                  __FILE__, __LINE__));
        }
        else
        {
            status = {};
        }
    }

    return set;
}

bool VulkanDescriptorSetState::BuildContentKey(uint32_t setIdx, VulkanDescriptorAllocator::Key& outKey)
{
    bool complete = false;

    outKey        = {};

    if (m_pPipeline != nullptr && setIdx < m_setStates.size())
    {
        VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

        if (pShader != nullptr && setIdx < pShader->GetNumDescriptorSetLayouts())
        {
            complete        = true;

            outKey.layoutId = pShader->GetSetLayout(setIdx).id;

            HeapVector<const BindingState*> bindingOrder;

            for (const BindingState& binding : m_setStates[setIdx].bindings)
            {
                bindingOrder.push_back(&binding);
            }

            std::sort(bindingOrder.begin(), bindingOrder.end(),
                      [](const BindingState* a, const BindingState* b) { return a->srb.binding < b->srb.binding; });

            outKey.bindings.push_back(bindingOrder.size());

            for (const BindingState* binding : bindingOrder)
            {
                outKey.bindings.push_back(binding->srb.binding);

                outKey.bindings.push_back(ToUnderlying(binding->srb.type));

                outKey.bindings.push_back(binding->valueRange);

                outKey.bindings.push_back(binding->srb.resources.size());

                // Dynamic offsets are bind-time state, not descriptor contents.
                for (const RHIResource* resource : binding->srb.resources)
                {
                    outKey.bindings.push_back(resource != nullptr ? resource->GetStableId() : 0);
                }
            }
        }
    }

    return complete;
}

void VulkanDescriptorSetState::ClearAllSetStates()
{
    for (SetState& setState : m_setStates)
    {
        setState.vkSet      = VK_NULL_HANDLE;

        setState.lifetimeId = 0;

        setState.bindings.clear();

        setState.dirty = false;
    }

    m_packedValueBuffers.clear();
}

void VulkanDescriptorSetState::InvalidateResolvedCaches()
{
    for (SetState& setState : m_setStates)
    {
        setState.vkSet      = VK_NULL_HANDLE;

        setState.lifetimeId = 0;
    }
}

VulkanDescriptorSetState::BindingState& VulkanDescriptorSetState::FindOrAddBinding(SetState&             setState,
                                                                                   uint32_t              bindingIdx,
                                                                                   RHIShaderResourceType type)
{
    BindingState* pBindingState = nullptr;

    for (BindingState& bindingState : setState.bindings)
    {
        if (bindingState.srb.binding == bindingIdx)
        {
            pBindingState = &bindingState;
        }
    }

    if (pBindingState == nullptr)
    {
        BindingState& bindingState = setState.bindings.emplace_back();

        // todo: maintain order (0, 1, 2, 4...) while adding bindings
        bindingState.srb.binding = bindingIdx;

        bindingState.srb.type    = type;

        pBindingState            = &bindingState;
    }
    else
    {
        ZEN_RHI_VERIFY(pBindingState->srb.type == type);
    }

    return *pBindingState;
}

void VulkanDescriptorSetState::WriteResourceParameter(const RHIShaderResourceParameter& param)
{
    ZEN_RHI_VERIFY(m_pPipeline != nullptr);

    ZEN_RHI_VERIFY(param.set < m_setStates.size());

    if (m_pPipeline != nullptr && param.set < m_setStates.size())
    {
        SetState& setState         = m_setStates[param.set];

        BindingState& bindingState = FindOrAddBinding(setState, param.binding, param.resourceType);

        if (param.resourceType == RHIShaderResourceType::eUniformBuffer)
        {
            // Each external UBO element has its own offset, defaulting to zero.
            // Pending packed bytes must not override these explicit bindings.
            if (bindingState.dynamicOffsets.size() <= param.arrayIndex)
            {
                bindingState.dynamicOffsets.resize(param.arrayIndex + 1);
            }

            bindingState.dynamicOffsets[param.arrayIndex] = param.bufferOffset;

            bindingState.uniformBlockId                   = 0;

            bindingState.uniformGeneration                = 0;

            const RHIShaderResourceDescriptor* srd = m_pPipeline->GetShader()->GetSRDByLocation(param.set, param.binding);

            bindingState.valueRange                = srd != nullptr ? srd->blockSize : 0;

            for (VulkanDescriptorSetState::PackedValueBufferState& packed : m_packedValueBuffers)
            {
                if (packed.setIdx == param.set && packed.bindingIdx == param.binding)
                {
                    packed.dirty = false;
                }
            }
        }

        const uint32_t resourceStride = GetVulkanDescriptorTraits(param.resourceType).stride;

        const uint32_t baseIndex      = param.arrayIndex * resourceStride;

        const uint32_t resourceCount  = baseIndex + resourceStride;

        if (bindingState.srb.resources.size() < resourceCount)
        {
            bindingState.srb.resources.resize(resourceCount);
        }

        if (resourceStride == 2)
        {
            // Batched parameters carry the texture/buffer first; descriptor bindings store
            // combined resources as sampler, texture/buffer pairs.
            bindingState.srb.resources[baseIndex]     = param.pAuxResource;

            bindingState.srb.resources[baseIndex + 1] = param.pResource;
        }
        else
        {
            bindingState.srb.resources[baseIndex] = param.pResource;
        }

        setState.dirty = true;
    }
}

bool VulkanDescriptorSetState::ValidateSetState(uint32_t setIdx)
{
    VulkanShader* shader = m_pPipeline != nullptr ? TO_VK_SHADER(m_pPipeline->GetShader()) : nullptr;

    bool valid           = shader != nullptr && setIdx < m_setStates.size() && setIdx < shader->GetNumDescriptorSetLayouts();

    if (valid && !(setIdx == kGlobalBindlessHeapIndex && shader->HasGlobalBindlessSet()))
    {
        const HeapVector<VulkanDescriptorSetState::BindingState>& bindings = m_setStates[setIdx].bindings;

        for (const VulkanDescriptorSetState::BindingState& binding : bindings)
        {
            const RHIShaderResourceDescriptor* descriptor  = shader->GetSRDByLocation(setIdx, binding.srb.binding);

            valid                                         &= descriptor != nullptr && descriptor->type == binding.srb.type
                                                          && binding.srb.resources.size() / GetVulkanDescriptorTraits(binding.srb.type).stride <= descriptor->arraySize;
        }

        for (RHIShaderResourceDescriptor const& descriptor : (*shader->GetSRDTable())[setIdx])
        {
            if (descriptor.arraySize == 1)
            {
                valid &= std::any_of(bindings.begin(), bindings.end(),
                                     [bindingIndex = descriptor.binding](const BindingState& binding) {
                                         return binding.srb.binding == bindingIndex;
                                     });
            }
        }
    }

    if (!valid)
    {
        ZEN_RHI_LOG_ERROR("Invalid Vulkan descriptor set bindings");
    }

    return valid;
}

void VulkanDescriptorSetState::SyncCacheRevision(const VulkanDescriptorAllocator& allocator)
{
    if (m_cacheRevision != allocator.GetRevision())
    {
        InvalidateResolvedCaches();

        m_cacheRevision = allocator.GetRevision();
    }
}

RHIStatus VulkanDescriptorSetState::BuildDescriptorSetList(VulkanCommandListContext*    pContext,
                                                           HeapVector<VkDescriptorSet>& outDescriptorSets,
                                                           uint32_t&                    outFirstSet,
                                                           HeapVector<uint32_t>&        outDynamicOffsets)
{
    VulkanShader* pShader     = TO_VK_SHADER(m_pPipeline->GetShader());

    bool      allSetsResolved = pShader != nullptr;
    RHIStatus status{allSetsResolved
                         ? RHIError{}
                         : MakeRHIError(RHIErrorCode::eInvalidArgument, "Prepare shader descriptors", __FILE__, __LINE__)};

    if (pShader != nullptr)
    {
        const VulkanDescriptorAllocator* cache = m_pRHI->GetDescriptorAllocator();

        ZEN_RHI_VERIFY(cache != nullptr);

        SyncCacheRevision(*cache);

        const uint32_t numSets = pShader->GetNumDescriptorSetLayouts();

        ZEN_RHI_VERIFY(numSets == m_setStates.size());

        uint32_t firstUsed = numSets;

        uint32_t lastUsed  = 0;

        bool anyUsed       = false;

        for (uint32_t setIdx = 0; setIdx < numSets; setIdx++)
        {
            const SetState& setState = m_setStates[setIdx];

            const bool declaredSet   = !(*pShader->GetSRDTable())[setIdx].empty();

            // Resolve declared sets even without parameters: scalar bindings must be present,
            // and partially-bound arrays still need a native set. Only empty layout gaps may be skipped.
            if (!declaredSet && setState.vkSet == VK_NULL_HANDLE && setState.bindings.empty())
            {
                continue;
            }

            firstUsed = std::min(firstUsed, setIdx);

            lastUsed  = setIdx;

            anyUsed   = true;
        }

        if (anyUsed)
        {
            outFirstSet                       = firstUsed;

            const uint32_t descriptorSetCount = lastUsed - firstUsed + 1;

            outDescriptorSets.resize(descriptorSetCount);

            for (uint32_t i = firstUsed; i <= lastUsed; i++)
            {
                // Resolving an earlier set can rotate the cache. Check again before
                // dereferencing a resolved handle/container from a previous draw.
                SyncCacheRevision(*cache);

                SetState& setState     = m_setStates[i];

                const bool needResolve = setState.dirty || setState.vkSet == VK_NULL_HANDLE;

                if (needResolve)
                {
                    status = ResolveSet(i);
                }
                if (!status)
                {
                    ZEN_RHI_LOG_ERROR("[VulkanRHI][VulkanDescriptorSetState]: ResolveSet failed!");

                    allSetsResolved = false;

                    break;
                }

                if (allSetsResolved)
                {
                    if (setState.lifetimeId != 0)
                    {
                        // Retain on every bind, including unchanged bindings in a new
                        // workload. Queue retirement releases this ownership.
                        pContext->RecordLifetime(setState.lifetimeId);
                    }

                    for (const BindingState& binding : setState.bindings)
                    {
                        pContext->RecordUniformBufferBlock(binding.uniformBlockId);
                    }

                    outDescriptorSets[i - firstUsed] = setState.vkSet;

                    AppendDynamicOffsetsForSet(i, outDynamicOffsets);
                }
            }

            if (!allSetsResolved)
            {
                outDescriptorSets.clear();

                outDynamicOffsets.clear();

                outFirstSet = 0;
            }
        }
    }

    return status;
}

void VulkanDescriptorSetState::AppendDynamicOffsetsForSet(uint32_t setIdx, HeapVector<uint32_t>& outDynamicOffsets)
{
    const VulkanShader* shader = TO_CVK_SHADER(m_pPipeline->GetShader());

    if (setIdx < m_setStates.size() && shader != nullptr)
    {
        for (const VulkanSetLayout::DynamicOffsetSlot& slot : shader->GetSetLayout(setIdx).dynamicOffsets)
        {
            const BindingState* binding = nullptr;

            for (const BindingState& state : m_setStates[setIdx].bindings)
            {
                if (state.srb.binding == slot.binding)
                {
                    binding = &state;

                    break;
                }
            }

            for (uint32_t element = 0; element < slot.count; ++element)
            {
                outDynamicOffsets.push_back(
                    binding != nullptr && element < binding->dynamicOffsets.size() ? binding->dynamicOffsets[element] : 0);
            }
        }
    }
}

void VulkanDescriptorSetState::SetPackedValueParameter(uint32_t       setIdx,
                                                       uint32_t       bindingIdx,
                                                       uint32_t       byteSize,
                                                       const uint8_t* pData)
{
    ZEN_RHI_VERIFY(pData != nullptr);

    if (pData != nullptr && byteSize > 0)
    {
        PackedValueBufferState* pBufferState = FindOrAddPackedValueBuffer(setIdx, bindingIdx);

        if (pBufferState != nullptr)
        {
            if (pBufferState->bytes.size() < byteSize)
            {
                pBufferState->bytes.resize(byteSize);
            }

            std::memcpy(pBufferState->bytes.data(), pData, byteSize);

            pBufferState->dirty = true;
        }
    }
}

RHIStatus VulkanDescriptorSetState::FlushPackedValueBuffers()
{
    VulkanUniformRing* pAllocator = m_pRHI->GetUniformRing();

    RHIStatus status{pAllocator != nullptr
                         ? RHIError{}
                         : MakeRHIError(RHIErrorCode::eBackendFailure, "Allocate packed uniform buffer", __FILE__, __LINE__)};

    if (pAllocator != nullptr)
    {
        for (PackedValueBufferState& bufferState : m_packedValueBuffers)
        {
            SetState& setState = m_setStates[bufferState.setIdx];

            BindingState& bindingState =
                FindOrAddBinding(setState, bufferState.bindingIdx, RHIShaderResourceType::eUniformBuffer);

            // Cached values retain CPU bytes, not transient buffer ownership. Refresh
            // before resolving descriptors if the slot was reused or its tail was trimmed.
            if (bindingState.uniformBlockId != 0
                && pAllocator->GetBlockGeneration(bindingState.uniformBlockId) != bindingState.uniformGeneration)
            {
                bufferState.dirty = true;

                // The old native buffer may already have been destroyed. Do not leave
                // a dangling resource in descriptor resolution if the upload must retry.
                bindingState.srb.resources.clear();

                bindingState.uniformBlockId    = 0;

                bindingState.uniformGeneration = 0;

                setState.dirty                 = true;
            }

            if (bufferState.dirty && bufferState.blockSize > 0)
            {
                const RHIResult<VulkanUniformBlock> allocated = pAllocator->Alloc(bufferState.blockSize);

                VulkanUniformBlock block                      = allocated.GetValueOr({});

                if (!allocated)
                {
                    status.error = allocated.GetError();

                    break;
                }

                if (block.IsValid())
                {
                    std::memcpy(block.pMapped, bufferState.bytes.data(), bufferState.blockSize);

                    const bool bufferChanged = bindingState.srb.resources.size() != 1
                                            || bindingState.srb.resources[0] != block.pBuffer
                                            || bindingState.uniformGeneration != block.generation
                                            || bindingState.valueRange != bufferState.blockSize;

                    bindingState.srb.resources.clear();

                    bindingState.srb.resources.push_back(block.pBuffer);

                    bindingState.dynamicOffsets.resize(1);

                    bindingState.dynamicOffsets[0]  = block.offset;

                    bindingState.valueRange         = bufferState.blockSize;

                    bindingState.uniformBlockId     = block.blockId;

                    bindingState.uniformGeneration  = block.generation;

                    setState.dirty                 |= bufferChanged;

                    bufferState.dirty               = false;
                }
            }
        }
    }

    return status;
}

VulkanDescriptorSetState::PackedValueBufferState* VulkanDescriptorSetState::FindOrAddPackedValueBuffer(uint32_t setIdx,
                                                                                                       uint32_t bindingIdx)
{
    PackedValueBufferState* pBufferState = nullptr;

    for (PackedValueBufferState& bufferState : m_packedValueBuffers)
    {
        if (bufferState.setIdx == setIdx && bufferState.bindingIdx == bindingIdx)
        {
            pBufferState = &bufferState;

            break;
        }
    }

    if (pBufferState == nullptr)
    {
        const RHIShaderResourceDescriptor* pSRD = m_pPipeline->GetShader()->GetSRDByLocation(setIdx, bindingIdx);

        if (pSRD == nullptr || pSRD->type != RHIShaderResourceType::eUniformBuffer || pSRD->arraySize != 1 || pSRD->bindless)
        {
            m_parametersValid = false;

            ZEN_RHI_LOG_ERROR("Packed uniform values require a single uniform-buffer descriptor");
        }
        else
        {
            PackedValueBufferState& bufferState = m_packedValueBuffers.emplace_back();

            bufferState.setIdx                  = setIdx;

            bufferState.bindingIdx              = bindingIdx;

            bufferState.blockSize               = pSRD->blockSize;

            bufferState.bytes.resize(pSRD->blockSize);

            pBufferState = &bufferState;
        }
    }

    return pBufferState;
}
} // namespace zen
