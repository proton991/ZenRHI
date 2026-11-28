#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include <span>
#include "Vulkan/VulkanTypes.h"
#include "Vulkan/VulkanBindlessHeap.h"
#include "ZenRHI/RHIShaderUtil.h"
#include "ZenRHI/RHICommon.h"
#include "ZenRHI/RHIResource.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanTexture.h"
#include "ZenFoundation/Memory/Memory.h"
#include "ZenFoundation/Utils/Errors.h"

#include <algorithm>
#include <limits>

namespace zen
{
RHIBindlessHeapType GetBindlessHeapType(const RHIResource* pResource)
{
    RHIBindlessHeapType heapType = RHIBindlessHeapType::eMax;

    switch (pResource->GetResourceType())
    {
        case RHIResourceType::eSampler: heapType = RHIBindlessHeapType::eSampler; break;

        case RHIResourceType::eTexture:
        case RHIResourceType::eTextureView:
        {
            const VulkanTextureView* view = TryVulkanTextureView(pResource);

            if (view == nullptr)
            {
                break;
            }

            const VkImageViewType viewType = view->GetVkImageViewType();

            switch (viewType)
            {
                case VK_IMAGE_VIEW_TYPE_2D: heapType = RHIBindlessHeapType::eTexture2D; break;
                case VK_IMAGE_VIEW_TYPE_CUBE: heapType = RHIBindlessHeapType::eTextureCube; break;
                case VK_IMAGE_VIEW_TYPE_2D_ARRAY: heapType = RHIBindlessHeapType::eTexture2DArray; break;
                case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: heapType = RHIBindlessHeapType::eTextureCubeArray; break;
                default: break;
            }

            break;
        }

        default: break;
    }

    return heapType;
}

bool IsValidBindlessResource(const RHIResource* pResource, RHIBindlessHeapType heapType)
{
    bool valid = false;

    if (heapType == RHIBindlessHeapType::eSampler)
    {
        const VulkanSampler* pSampler = TryVulkanSampler(pResource);

        valid                         = pSampler != nullptr && pSampler->GetVkSampler() != VK_NULL_HANDLE;
    }
    else if (GetBindlessHeapResourceType(heapType) == RHIShaderResourceType::eTexture)
    {
        const VulkanTextureView* pTextureView = TryVulkanTextureView(pResource);

        valid                                 = pTextureView != nullptr && pTextureView->GetVkImageView() != VK_NULL_HANDLE
                                             && pTextureView->GetTexture()->GetBaseInfo().samples == SampleCount::e1
                                             && pTextureView->GetTexture()->GetBaseInfo().usageFlags.HasFlag(RHITextureUsageFlagBits::eSampled);
    }

    return valid;
}

RHIError VulkanBindlessHeap::InitChecked()
{
    RHIError error;

    const bool                      usable = m_pDevice != nullptr && m_pDevice->GetExtensionFlags().hasDescriptorIndexing != 0;
    const RHIBindlessHeapCapacities capacities =
        usable ? GetBindlessHeapRequest(m_pDevice->GetRHI()->GetDeviceDesc().BindlessHeapCapacities(),
                                        m_pDevice->GetPhysicalDeviceFeatures().imageCubeArray == VK_TRUE)
               : kNoBindlessHeaps;
    RHIBindlessHeapCapacities fitted = kNoBindlessHeaps;

    if (usable && FitBindlessHeapCapacities(m_pDevice->GetDescriptorIndexingProperties(), capacities, fitted))
    {
        // Later option changes must not resize the live heaps.
        m_capacities = fitted;

        if (fitted.slots != capacities.slots)
        {
            ZEN_RHI_LOG_INFO(
                "Bindless heap requests reduced to device limits: 2D {} -> {}, cube {} -> {}, sampler {} -> {}, 2D array {} -> {}, cube array {} -> {}",
                capacities.Get(RHIBindlessHeapType::eTexture2D), fitted.Get(RHIBindlessHeapType::eTexture2D),
                capacities.Get(RHIBindlessHeapType::eTextureCube), fitted.Get(RHIBindlessHeapType::eTextureCube),
                capacities.Get(RHIBindlessHeapType::eSampler), fitted.Get(RHIBindlessHeapType::eSampler),
                capacities.Get(RHIBindlessHeapType::eTexture2DArray), fitted.Get(RHIBindlessHeapType::eTexture2DArray),
                capacities.Get(RHIBindlessHeapType::eTextureCubeArray), fitted.Get(RHIBindlessHeapType::eTextureCubeArray));
        }

        error = CreateGlobalBindlessDescriptorSet();

        if (m_vkSet != VK_NULL_HANDLE)
        {
            m_epoch = m_pDevice->GetRHI()->GetLifetimeTracker().Create();

            m_epochs.push_back(m_epoch);

            m_publishedEpoch.store(m_epoch, std::memory_order_release);

            for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
            {
                const RHIBindlessHeapType heapType = static_cast<RHIBindlessHeapType>(heapIdx);

                m_slotStates[heapIdx].resize(GetCapacity(heapType));

                m_heapAllocCount[heapIdx] = 0;
            }

            ZEN_RHI_LOG_INFO("Bindless heaps: {} 2D textures, {} cube textures, {} samplers, {} 2D arrays, {} cube arrays",
                             GetCapacity(RHIBindlessHeapType::eTexture2D), GetCapacity(RHIBindlessHeapType::eTextureCube),
                             GetCapacity(RHIBindlessHeapType::eSampler), GetCapacity(RHIBindlessHeapType::eTexture2DArray),
                             GetCapacity(RHIBindlessHeapType::eTextureCubeArray));
        }
        else
        {
            m_capacities = kNoBindlessHeaps;
        }
    }
    return error;
}

void VulkanBindlessHeap::Destroy()
{
    for (uint64_t epoch : m_epochs)
    {
        m_pDevice->GetRHI()->GetLifetimeTracker().Retire(epoch);
    }

    m_epochs.clear();

    m_epoch = 0;

    m_publishedEpoch.store(0, std::memory_order_release);

    ReleaseAllSlots();

    for (HeapVector<BindlessSlotState>& slots : m_slotStates)
    {
        slots.clear();
    }

    m_journals.clear();

    if (m_pDevice != nullptr)
    {
        const VkDevice device = m_pDevice->GetVkHandle();

        if (m_vkPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, m_vkPool, nullptr);
        }

        if (m_setLayout.handle != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m_setLayout.handle, nullptr);
        }
    }

    m_vkSet            = VK_NULL_HANDLE;

    m_vkPool           = VK_NULL_HANDLE;

    m_setLayout.handle = VK_NULL_HANDLE;

    m_capacities       = kNoBindlessHeaps;
}

bool VulkanBindlessHeap::ResetRegistrations()
{
    LockAuto lock(&m_mutex);

    bool reset = m_journals.empty();

    if (m_vkSet != VK_NULL_HANDLE)
    {
        VulkanLifetimeTracker& tracker = m_pDevice->GetRHI()->GetLifetimeTracker();

        for (uint64_t epoch : m_epochs)
        {
            reset = reset && tracker.IsComplete(epoch);
        }

        if (reset)
        {
            // Reserve before publication; failure leaves the existing heap usable.
            m_epochs.reserve(m_epochs.size() + 1);

            const uint64_t epoch = tracker.Create();

            m_epochs.push_back(epoch);

            m_epoch = epoch;

            m_publishedEpoch.store(epoch, std::memory_order_release);

            ReleaseAllSlots();

            // Earlier complete epochs retire during the next ordinary registration/collection.
        }
    }

    return reset;
}

bool VulkanBindlessHeap::RegisterBindlessResource(RHIResource*       pResource,
                                                  uint32_t           slotIdx,
                                                  RHIBindlessHandle* pOutHandle,
                                                  uint64_t           recordedEpoch,
                                                  uint64_t           transaction)
{
    LockAuto lock(&m_mutex);

    if (pOutHandle != nullptr)
    {
        *pOutHandle = {};
    }

    CollectRetiredResourcesLocked();

    const RHIBindlessHeapType heapType = pResource != nullptr ? GetBindlessHeapType(pResource) : RHIBindlessHeapType::eMax;

    bool registered =
        m_vkSet != VK_NULL_HANDLE && heapType != RHIBindlessHeapType::eMax && IsValidBindlessResource(pResource, heapType);

    if (registered)
    {
        slotIdx    = slotIdx == kInvalidBindlessSlotIndex ? FindFreeSlotLocked(heapType) : slotIdx;

        registered = slotIdx < GetCapacity(heapType);
    }

    if (registered)
    {
        const BindlessSlotState& slot = m_slotStates[ToUnderlying(heapType)][slotIdx];

        if (slot.pResource != nullptr)
        {
            // An occupied slot is accepted only when it already holds the same resource.
            registered = CanRegisterLocked(heapType, pResource, slotIdx, recordedEpoch, transaction);
        }
        else
        {
            ClaimSlotLocked(heapType, slotIdx, pResource, transaction);
        }

        if (registered && pOutHandle != nullptr)
        {
            *pOutHandle = {heapType, slotIdx, slot.generation};
        }
    }

    return registered;
}

uint32_t VulkanBindlessHeap::FindFreeSlotLocked(RHIBindlessHeapType heapType) const
{
    const uint32_t heapIdx  = ToUnderlying(heapType);
    const uint32_t capacity = GetCapacity(heapType);
    uint32_t       found    = kInvalidBindlessSlotIndex;

    // Search from the allocation cursor. A claim moves it past the slot; a release moves it back to the freed slot.
    for (uint32_t i = 0; found == kInvalidBindlessSlotIndex && i < capacity; ++i)
    {
        const uint32_t candidate = (m_heapAllocCount[heapIdx] + i) % capacity;

        found                    = m_slotStates[heapIdx][candidate].pResource == nullptr ? candidate : found;
    }

    return found;
}

void VulkanBindlessHeap::ClaimSlotLocked(RHIBindlessHeapType heapType,
                                         uint32_t            slotIdx,
                                         RHIResource*        pResource,
                                         uint64_t            transaction)
{
    static std::atomic<uint64_t> nextGeneration{1};

    const uint32_t     heapIdx = ToUnderlying(heapType);
    BindlessSlotState& slot    = m_slotStates[heapIdx][slotIdx];

    // Transactional writes enter the native update batch at submission.
    if (transaction != 0)
    {
        HeapVector<RHIBindlessHandle>& journal = m_journals[transaction].slots;
        journal.reserve(journal.size() + 1);
    }
    else
    {
        m_pendingWrites[heapIdx].push_back({slotIdx, pResource});
        m_hasPendingWrites.store(true, std::memory_order_release);
    }

    pResource->AddReference();

    slot.pResource = pResource;

    if (pResource->GetResourceType() == RHIResourceType::eTextureView)
    {
        slot.pTextureOwner = static_cast<RHITextureView*>(pResource)->GetTexture();
        slot.pTextureOwner->AddReference();
    }

    slot.resourceId           = pResource->GetStableId();
    slot.generation           = nextGeneration.fetch_add(1, std::memory_order_relaxed);
    slot.transaction          = transaction;
    m_heapAllocCount[heapIdx] = (slotIdx + 1) % GetCapacity(heapType);

    if (transaction != 0)
    {
        m_journals[transaction].slots.push_back({heapType, slotIdx, slot.generation});
    }
}

uint64_t VulkanBindlessHeap::BeginTransaction()
{
    static std::atomic<uint64_t> nextTransaction{1};

    return nextTransaction.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
// One parameter's heap slot, encoded as heap << 32 | slot index.
struct BindlessSlotClaim
{
    uint64_t           slot{0};
    const RHIResource* resource{nullptr};
};

bool IsEarlierSlot(const BindlessSlotClaim& lhs, const BindlessSlotClaim& rhs)
{
    return lhs.slot < rhs.slot;
}
} // namespace

bool VulkanBindlessHeap::CanRegisterBindlessResources(std::span<const RHIShaderResourceParameter> parameters,
                                                      uint64_t                                    recordedEpoch,
                                                      uint64_t                                    transaction)
{
    SmallVector<BindlessSlotClaim, 16> claims;

    bool valid = true;

    {
        LockAuto lock(&m_mutex);

        for (size_t i = 0; valid && i < parameters.size(); ++i)
        {
            const RHIShaderResourceParameter& parameter = parameters[i];

            const RHIBindlessHeapType heap =
                parameter.pResource != nullptr ? GetBindlessHeapType(parameter.pResource) : RHIBindlessHeapType::eMax;
            valid = heap != RHIBindlessHeapType::eMax && parameter.binding == ToUnderlying(heap)
                 && RHIShaderUtil::IsGlobalHeapBinding(parameter.set, parameter.binding, parameter.resourceType)
                 && parameter.pAuxResource == nullptr && parameter.bufferOffset == 0
                 && CanRegisterLocked(heap, parameter.pResource, parameter.arrayIndex, recordedEpoch, transaction);

            if (valid)
            {
                claims.push_back({(uint64_t(ToUnderlying(heap)) << 32) | parameter.arrayIndex, parameter.pResource});
            }
        }
    }

    // Sorting places claims on one slot next to each other; they must name one resource.
    std::sort(claims.begin(), claims.end(), IsEarlierSlot);

    for (size_t i = 1; valid && i < claims.size(); ++i)
    {
        valid = claims[i].slot != claims[i - 1].slot || claims[i].resource == claims[i - 1].resource;
    }

    return valid;
}

bool VulkanBindlessHeap::CanRegisterLocked(RHIBindlessHeapType heap,
                                           const RHIResource*  resource,
                                           uint32_t            slotIndex,
                                           uint64_t            recordedEpoch,
                                           uint64_t            transaction)
{
    bool valid = m_vkSet != VK_NULL_HANDLE && IsValidBindlessResource(resource, heap) && slotIndex < GetCapacity(heap);

    if (valid)
    {
        const BindlessSlotState& slot = m_slotStates[ToUnderlying(heap)][slotIndex];

        const bool earlierRecording   = recordedEpoch != 0 && recordedEpoch <= slot.retiredEpoch
                                     && std::binary_search(m_epochs.begin(), m_epochs.end(), recordedEpoch)
                                     && m_pDevice->GetRHI()->GetLifetimeTracker().HasRecordings(recordedEpoch);

        valid                         = slot.pResource == nullptr
                                     || (slot.resourceId == resource->GetStableId() && (slot.retiredEpoch == 0 || earlierRecording)
                                         && (slot.transaction == 0 || slot.transaction == transaction));
    }

    return valid;
}

void VulkanBindlessHeap::QueueTransactionWrites(BindlessJournal& journal)
{
    while (journal.queued < journal.slots.size())
    {
        const RHIBindlessHandle  handle = journal.slots[journal.queued++];
        const BindlessSlotState* slot   = FindRegistration(handle);
        ZEN_RHI_VERIFY(slot != nullptr);
        m_pendingWrites[ToUnderlying(handle.heapType)].push_back({handle.slotIndex, slot->pResource});
        m_hasPendingWrites.store(true, std::memory_order_release);
    }
}

bool VulkanBindlessHeap::TransferTransaction(uint64_t transaction, size_t workloads)
{
    bool transferred = false;
    if (workloads == 0)
    {
        ResolveTransaction(transaction, false);
    }
    else
    {
        LockAuto   lock(&m_mutex);
        const auto journal = m_journals.find(transaction);
        if (journal != m_journals.end())
        {
            journal->second.owners += workloads;
            transferred             = true;
        }
    }
    return transferred;
}

void VulkanBindlessHeap::ReleaseTransferredTransaction(uint64_t transaction)
{
    bool last = false;
    {
        LockAuto   lock(&m_mutex);
        const auto journal = m_journals.find(transaction);
        if (journal != m_journals.end())
        {
            ZEN_RHI_VERIFY(journal->second.owners != 0);
            last = --journal->second.owners == 0;
        }
    }
    if (last)
    {
        ResolveTransaction(transaction, false);
    }
}

void VulkanBindlessHeap::PrepareTransaction(uint64_t transaction)
{
    LockAuto   lock(&m_mutex);
    const auto journal = m_journals.find(transaction);
    if (journal != m_journals.end())
    {
        QueueTransactionWrites(journal->second);
    }
}

void VulkanBindlessHeap::ResolveTransaction(uint64_t transaction, bool commit)
{
    LockAuto lock(&m_mutex);

    const auto journal = m_journals.find(transaction);

    if (journal != m_journals.end())
    {
        if (commit)
        {
            QueueTransactionWrites(journal->second);
        }
        for (RHIBindlessHandle handle : journal->second.slots)
        {
            BindlessSlotState* slot = FindRegistration(handle);

            if (slot != nullptr && slot->transaction == transaction)
            {
                if (commit)
                {
                    slot->transaction = 0;
                }
                else
                {
                    ReleaseSlot(handle, *slot);
                }
            }
        }

        m_journals.erase(journal);
    }
}

VulkanBindlessHeap::BindlessSlotState* VulkanBindlessHeap::FindRegistration(RHIBindlessHandle handle)
{
    VulkanBindlessHeap::BindlessSlotState* returnValue{};

    if (!handle.IsValid() || m_vkSet == VK_NULL_HANDLE || handle.slotIndex >= GetCapacity(handle.heapType))
    {
        returnValue = nullptr;
    }
    else
    {
        BindlessSlotState& slot = m_slotStates[ToUnderlying(handle.heapType)][handle.slotIndex];

        returnValue             = slot.pResource != nullptr && slot.generation == handle.generation ? &slot : nullptr;
    }

    return returnValue;
}

bool VulkanBindlessHeap::IsRegistered(RHIBindlessHandle handle)
{
    LockAuto lock(&m_mutex);

    const BindlessSlotState* slot = FindRegistration(handle);

    return slot != nullptr && slot->retiredEpoch == 0 && slot->transaction == 0;
}

bool VulkanBindlessHeap::UnregisterBindlessResource(RHIBindlessHandle handle)
{
    LockAuto lock(&m_mutex);

    BindlessSlotState* slot = FindRegistration(handle);

    bool retired            = false;

    if (slot != nullptr && slot->retiredEpoch == 0 && slot->transaction == 0 && m_epoch != UINT64_MAX)
    {
        // Reserve before changing publication state. A failed allocation leaves it active.
        m_retiredSlots.reserve(m_retiredSlots.size() + 1);

        m_epochs.reserve(m_epochs.size() + 1);

        const uint64_t nextEpoch = m_pDevice->GetRHI()->GetLifetimeTracker().Create();

        m_retiredSlots.push_back(handle);

        slot->retiredEpoch = m_epoch;

        m_epoch            = nextEpoch;

        m_epochs.push_back(m_epoch);

        m_publishedEpoch.store(m_epoch, std::memory_order_release);

        CollectRetiredResourcesLocked();

        retired = true;
    }

    return retired;
}

uint64_t VulkanBindlessHeap::CaptureEpoch()
{
    LockAuto lock(&m_mutex);

    uint64_t epoch = 0;

    if (m_vkSet != VK_NULL_HANDLE)
    {
        // Recording is allowed on any recording thread. Collection can destroy native resources,
        // so leave it to the RHI thread's registration/retirement sweeps.
        epoch = m_epoch;

        m_pDevice->GetRHI()->GetExecutionCounterStorage().Increment(
            m_pDevice->GetRHI()->GetExecutionCounterStorage().bindlessCaptures);

        m_pDevice->GetRHI()->GetLifetimeTracker().RetainRecording(epoch);
    }

    return epoch;
}

void VulkanBindlessHeap::ReleaseEpoch(uint64_t epoch)
{
    m_pDevice->GetRHI()->GetLifetimeTracker().ReleaseRecordings(MakeVecView(&epoch, 1));
}

void VulkanBindlessHeap::CollectRetiredResources()
{
    LockAuto lock(&m_mutex);

    CollectRetiredResourcesLocked();
}

void VulkanBindlessHeap::CollectRetiredResourcesLocked()
{
    VulkanLifetimeTracker& tracker = m_pDevice->GetRHI()->GetLifetimeTracker();

    uint64_t earliestEpoch         = UINT64_MAX;

    size_t retained                = 0;

    for (uint64_t epoch : m_epochs)
    {
        const bool completed = tracker.IsComplete(epoch);

        if (completed && epoch != m_epoch)
        {
            tracker.Retire(epoch);
        }
        else
        {
            m_epochs[retained++] = epoch;

            if (!completed)
            {
                earliestEpoch = std::min(earliestEpoch, epoch);
            }
        }
    }

    m_epochs.resize(retained);

    for (HeapVector<RHIBindlessHandle>::iterator it = m_retiredSlots.begin(); it != m_retiredSlots.end();)
    {
        BindlessSlotState* slot = FindRegistration(*it);

        if (slot != nullptr && slot->retiredEpoch < earliestEpoch)
        {
            // Live old recordings keep their writes intact until this point.
            ReleaseSlot(*it, *slot);

            it = m_retiredSlots.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void VulkanBindlessHeap::ReleaseSlot(RHIBindlessHandle handle, BindlessSlotState& slot)
{
    // Unflushed writes must not outlive their retained resources or later overwrite a recycled slot.
    HeapVector<BindlessDSWrite>& writes = m_pendingWrites[ToUnderlying(handle.heapType)];

    writes.erase(std::remove_if(writes.begin(), writes.end(),
                                [handle](const BindlessDSWrite& write) { return write.slotIdx == handle.slotIndex; }),
                 writes.end());

    RHIResource* resource                           = slot.pResource;

    RHITexture* texture                             = slot.pTextureOwner;

    slot                                            = {};

    m_heapAllocCount[ToUnderlying(handle.heapType)] = handle.slotIndex;

    resource->ReleaseReference();

    if (texture != nullptr)
    {
        texture->ReleaseReference();
    }
}

void VulkanBindlessHeap::ReleaseAllSlots()
{
    m_hasPendingWrites.store(false, std::memory_order_release);

    m_retiredSlots.clear();

    for (uint32_t heap = 0; heap < kBindlessHeapCount; ++heap)
    {
        m_pendingWrites[heap].clear();

        for (BindlessSlotState& slot : m_slotStates[heap])
        {
            if (slot.pResource != nullptr)
            {
                slot.pResource->ReleaseReference();
            }

            if (slot.pTextureOwner != nullptr)
            {
                slot.pTextureOwner->ReleaseReference();
            }

            slot = {};
        }

        m_heapAllocCount[heap] = 0;
    }
}

void VulkanBindlessHeap::Flush()
{
    if (m_hasPendingWrites.load(std::memory_order_acquire))
    {
        LockAuto lock(&m_mutex);

        if (m_vkSet != VK_NULL_HANDLE && m_hasPendingWrites.load(std::memory_order_relaxed))
        {
            WriteDescriptorSetBatch();

            m_hasPendingWrites.store(false, std::memory_order_release);
        }
    }
}

VkDescriptorSet VulkanBindlessHeap::GetGlobalBindlessSet() const
{
    return m_vkSet;
}

RHIError VulkanBindlessHeap::CreateGlobalBindlessDescriptorSet()
{
    VkDescriptorSetLayoutBinding bindings[kBindlessHeapCount]{};

    VkDescriptorBindingFlags bindingFlags[kBindlessHeapCount]{};

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        const RHIBindlessHeapType heapType = static_cast<RHIBindlessHeapType>(heapIdx);

        bindings[heapIdx].binding          = heapIdx;

        bindings[heapIdx].descriptorType   = GetVulkanDescriptorTraits(GetBindlessHeapResourceType(heapType)).type;

        bindings[heapIdx].descriptorCount  = GetCapacity(heapType);

        bindings[heapIdx].stageFlags       = VK_SHADER_STAGE_ALL;

        bindingFlags[heapIdx] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                              | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{};

    InitVkStruct(bindingFlagsCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);

    bindingFlagsCI.bindingCount  = kBindlessHeapCount;

    bindingFlagsCI.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutCI{};

    InitVkStruct(layoutCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);

    layoutCI.pNext        = &bindingFlagsCI;

    layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

    layoutCI.bindingCount = kBindlessHeapCount;

    layoutCI.pBindings    = bindings;

    VkDescriptorSetLayoutSupport support{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};

    vkGetDescriptorSetLayoutSupport(m_pDevice->GetVkHandle(), &layoutCI, &support);

    // Device selection cannot ask before the device exists, and no shader can be created
    // without this layout, so continuing would only fail later and less clearly.
    VkResult    result    = VK_ERROR_FEATURE_NOT_PRESENT;
    const char* operation = "vkGetDescriptorSetLayoutSupport";
    if (!support.supported)
    {
        ZEN_RHI_LOG_ERROR("The Vulkan device does not support the global bindless layout");
    }
    if (support.supported)
    {
        operation = "vkCreateDescriptorSetLayout";
        result    = vkCreateDescriptorSetLayout(m_pDevice->GetVkHandle(), &layoutCI, nullptr, &m_setLayout.handle);
    }

    if (result != VK_SUCCESS)
    {
        ZEN_RHI_LOG_ERROR("Global bindless layout creation failed: {}", GetResultString(result));
    }

    if (result == VK_SUCCESS)
    {
        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<uint32_t>(m_capacities.GetImageSlots())},
            {VK_DESCRIPTOR_TYPE_SAMPLER, GetCapacity(RHIBindlessHeapType::eSampler)}};

        VkDescriptorPoolCreateInfo poolCI{};

        InitVkStruct(poolCI, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);

        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

        poolCI.maxSets       = 1;

        poolCI.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));

        poolCI.pPoolSizes    = poolSizes;

        operation            = "vkCreateDescriptorPool";
        result               = vkCreateDescriptorPool(m_pDevice->GetVkHandle(), &poolCI, nullptr, &m_vkPool);

        if (result != VK_SUCCESS)
        {
            ZEN_RHI_LOG_ERROR("Global bindless pool creation failed: {}", GetResultString(result));
        }
    }

    if (result == VK_SUCCESS)
    {
        VkDescriptorSetAllocateInfo allocateInfo{};

        InitVkStruct(allocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);

        allocateInfo.descriptorPool     = m_vkPool;

        allocateInfo.descriptorSetCount = 1;

        allocateInfo.pSetLayouts        = &m_setLayout.handle;

        operation                       = "vkAllocateDescriptorSets";
        result                          = vkAllocateDescriptorSets(m_pDevice->GetVkHandle(), &allocateInfo, &m_vkSet);

        if (result != VK_SUCCESS)
        {
            ZEN_RHI_LOG_ERROR("Global bindless set allocation failed: {}", GetResultString(result));
        }
    }

    if (result != VK_SUCCESS)
    {
        if (m_vkPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_pDevice->GetVkHandle(), m_vkPool, nullptr);
        }

        if (m_setLayout.handle != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_pDevice->GetVkHandle(), m_setLayout.handle, nullptr);
        }

        m_vkSet            = VK_NULL_HANDLE;

        m_vkPool           = VK_NULL_HANDLE;

        m_setLayout.handle = VK_NULL_HANDLE;
    }
    m_pDevice->GetRHI()->CheckDeviceLoss(result, operation);

    return MakeVulkanError(result, operation);
}

void VulkanBindlessHeap::WriteDescriptorSetBatch()
{
    uint32_t numWrites = 0;

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        numWrites += static_cast<uint32_t>(m_pendingWrites[heapIdx].size());
    }

    HeapVector<VkDescriptorImageInfo> imageInfos(numWrites);

    HeapVector<VkWriteDescriptorSet> descriptorWrites(numWrites);

    uint32_t writeIdx = 0;

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        const RHIBindlessHeapType heapType = static_cast<RHIBindlessHeapType>(heapIdx);

        for (const BindlessDSWrite& pendingWrite : m_pendingWrites[heapIdx])
        {
            VkDescriptorImageInfo& imageInfo = imageInfos[writeIdx];

            if (heapType == RHIBindlessHeapType::eSampler)
            {
                imageInfo.sampler = TO_VK_SAMPLER(pendingWrite.pResource)->GetVkSampler();
            }
            else
            {
                imageInfo.imageView   = TryVulkanTextureView(pendingWrite.pResource)->GetVkImageView();

                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            VkWriteDescriptorSet& descriptorWrite = descriptorWrites[writeIdx];

            InitVkStruct(descriptorWrite, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);

            descriptorWrite.dstSet          = m_vkSet;

            descriptorWrite.dstBinding      = heapIdx;

            descriptorWrite.dstArrayElement = pendingWrite.slotIdx;

            descriptorWrite.descriptorCount = 1;

            descriptorWrite.descriptorType  = GetVulkanDescriptorTraits(GetBindlessHeapResourceType(heapType)).type;

            descriptorWrite.pImageInfo      = &imageInfo;

            ++writeIdx;
        }
    }

    if (numWrites > 0)
    {
        vkUpdateDescriptorSets(m_pDevice->GetVkHandle(), numWrites, descriptorWrites.data(), 0, nullptr);
    }

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        m_pendingWrites[heapIdx].clear();
    }
}
} // namespace zen
