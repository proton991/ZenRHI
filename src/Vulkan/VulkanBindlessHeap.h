#pragma once
#include "Vulkan/VulkanBindlessCapacity.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanSetLayout.h"
#include "ZenRHI/RHIResource.h"
#include "ZenRHI/RHIShaderParameters.h"
#include "ZenFoundation/Templates/FlatHashMap.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Utils/Mutex.h"
#include <atomic>
#include <span>

namespace zen
{
RHIBindlessHeapType GetBindlessHeapType(const RHIResource* resource);

class VulkanBindlessHeap
{
public:
    explicit VulkanBindlessHeap(VulkanDevice* pDevice) : m_pDevice(pDevice) {}

    RHIError InitChecked();

    void Destroy();

    // Published slots remain immutable until explicitly retired and all earlier
    // recordings and submissions finish. recordedEpoch permits playback of an old registration.
    bool RegisterBindlessResource(RHIResource*       pResource,
                                  uint32_t           slotIdx,
                                  RHIBindlessHandle* pOutHandle    = nullptr,
                                  uint64_t           recordedEpoch = 0,
                                  uint64_t           transaction   = 0);

    uint64_t BeginTransaction();

    // Validates a whole bindless parameter group under one lock before anything is published:
    // each parameter must name its resource's heap binding and type, each slot must accept
    // its resource for this transaction, and no slot may receive two different resources.
    bool CanRegisterBindlessResources(std::span<const RHIShaderResourceParameter> parameters,
                                      uint64_t                                    recordedEpoch,
                                      uint64_t                                    transaction);

    bool TransferTransaction(uint64_t transaction, size_t workloads);
    void ReleaseTransferredTransaction(uint64_t transaction);

    void PrepareTransaction(uint64_t transaction);

    void ResolveTransaction(uint64_t transaction, bool commit);

    bool UnregisterBindlessResource(RHIBindlessHandle handle);

    bool IsRegistered(RHIBindlessHandle handle);

    void CollectRetiredResources();

    // Ordinary registrations remain immutable. A complete, drained heap can be replaced
    // explicitly without recreating its descriptor set/layout or allowing stale handles.
    bool ResetRegistrations();

    // CPU commands hold their epoch until reset; native recordings transfer their
    // count to queue serials only after submission succeeds. Zero is invalid.
    uint64_t GetCurrentEpoch() const
    {
        return m_publishedEpoch.load(std::memory_order_acquire);
    }

    uint64_t CaptureEpoch();

    void ReleaseEpoch(uint64_t epoch);

    // Write all registered bindless resources in batch, call WriteDescriptorSetBatch
    void Flush();

    const VulkanSetLayout& GetSetLayout() const
    {
        return m_setLayout;
    }

    VkDescriptorSet GetGlobalBindlessSet() const;

    // Slot counts fixed at Init from the device description; zero when the heaps are unavailable.
    uint32_t GetCapacity(RHIBindlessHeapType heapType) const
    {
        return m_capacities.Get(heapType);
    }

    const RHIBindlessHeapCapacities& GetCapacities() const
    {
        return m_capacities;
    }

private:
    struct BindlessDSWrite
    {
        uint32_t     slotIdx{0};
        RHIResource* pResource{nullptr};
    };

    struct BindlessSlotState
    {
        uint64_t     resourceId{0};
        RHIResource* pResource{nullptr};
        RHITexture*  pTextureOwner{nullptr};
        uint64_t     generation{0};
        uint64_t     retiredEpoch{0};
        uint64_t     transaction{0};
    };

    void CollectRetiredResourcesLocked();

    // Drops a slot's references and unflushed writes, and makes the slot the next allocation candidate.
    void ReleaseSlot(RHIBindlessHandle handle, BindlessSlotState& slot);

    // Drops every registration, pending write and retirement while keeping the native heap.
    void ReleaseAllSlots();

    BindlessSlotState* FindRegistration(RHIBindlessHandle handle);

    // The first free slot after the last claim, or kInvalidBindlessSlotIndex.
    uint32_t FindFreeSlotLocked(RHIBindlessHeapType heapType) const;

    // Fills an empty slot; the resource and a texture view's texture gain a reference.
    void ClaimSlotLocked(RHIBindlessHeapType heapType, uint32_t slotIdx, RHIResource* pResource, uint64_t transaction);

    bool CanRegisterLocked(RHIBindlessHeapType heap,
                           const RHIResource*  resource,
                           uint32_t            slotIndex,
                           uint64_t            recordedEpoch,
                           uint64_t            transaction);

    RHIError CreateGlobalBindlessDescriptorSet();

    struct BindlessJournal
    {
        HeapVector<RHIBindlessHandle> slots;
        size_t                        queued{};
        size_t                        owners{};
    };

    void QueueTransactionWrites(BindlessJournal& journal);

    void WriteDescriptorSetBatch();

    VulkanDevice* m_pDevice{nullptr};

    VkDescriptorSet  m_vkSet{VK_NULL_HANDLE};
    VkDescriptorPool m_vkPool{VK_NULL_HANDLE};
    VulkanSetLayout  m_setLayout{};

    RHIBindlessHeapCapacities m_capacities{kNoBindlessHeaps};

    uint32_t m_heapAllocCount[ToUnderlying(RHIBindlessHeapType::eMax)]{};

    uint64_t                      m_epoch{0};
    std::atomic<uint64_t>         m_publishedEpoch{0};
    std::atomic<bool>             m_hasPendingWrites{false};
    HeapVector<uint64_t>          m_epochs;
    HeapVector<RHIBindlessHandle> m_retiredSlots;

    FlatHashMap<uint64_t, BindlessJournal> m_journals;

    HeapVector<BindlessDSWrite>   m_pendingWrites[ToUnderlying(RHIBindlessHeapType::eMax)];
    HeapVector<BindlessSlotState> m_slotStates[ToUnderlying(RHIBindlessHeapType::eMax)];

    Mutex m_mutex;
};
} // namespace zen
