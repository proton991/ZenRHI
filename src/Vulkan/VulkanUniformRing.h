#pragma once
#include "Vulkan/VulkanBuffer.h"
#include "ZenFoundation/Templates/HeapVector.h"

namespace zen
{
struct VulkanUniformBlock
{
    RHIBuffer* pBuffer{nullptr};
    uint32_t   offset{0};
    uint32_t   size{0};
    uint8_t*   pMapped{nullptr};
    uint64_t   blockId{0};
    uint64_t   generation{0};

    bool IsValid() const
    {
        return pBuffer != nullptr && pMapped != nullptr;
    }
};

class VulkanUniformRing
{
public:
    explicit VulkanUniformRing(VulkanRHI* pRHI) : m_pRHI(pRHI) {}

    // Reserve block metadata up front; each slot can grow without invalidating earlier allocations.
    void Init(uint32_t numSlots, uint32_t blockSize, uint32_t reservedBlocksPerSlot);

    void Destroy();

    void BeginFrame(uint32_t frameNum);

    RHIResult<VulkanUniformBlock> Alloc(uint32_t size);

    static constexpr uint32_t kTrimDelay = 120;

    uint32_t GetAllocatedBlockCount(uint32_t slotIndex) const;

    // Allocation views are borrowed. Native workloads register each referenced block
    // once, then transfer that recording to an accepted queue serial or discard it.
    uint64_t GetBlockGeneration(uint64_t blockId) const;

    uint64_t GetBlockLifetime(uint64_t blockId) const;

private:
    VulkanRHI* m_pRHI{nullptr};
    struct Block
    {
        VulkanUniformBlock memory;
        uint64_t           lifetimeId{0};
        uint64_t           lastNeededReuseCount{0};
        bool               resetPending{false};

        bool CanReuse(VulkanLifetimeTracker& tracker) const;
    };

    const Block* FindBlock(uint64_t blockId) const;

    struct Slot
    {
        HeapVector<Block> blocks;
        uint32_t          currentBlockIdx{0};
        uint32_t          usedBlocks{0};
        uint64_t          reuseCount{0};
    };

    RHIResult<VulkanUniformBlock> CreateBlock() const;

    void DestroyBlock(Block& block) const;

    HeapVector<Slot> m_slots;

    uint32_t m_blockSize{0};

    uint32_t m_currentSlotIdx{0};

    uint32_t m_alignment{256};

    // Never reset on Destroy/Init: an old cached location must not match new storage.
    uint64_t m_nextGeneration{0};
};
} // namespace zen
