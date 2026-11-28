#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanResourceSharing.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanUniformRing.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanResourceAllocator.h"
#include "Vulkan/VulkanTypes.h"

namespace zen
{
void VulkanUniformRing::Init(uint32_t numSlots, uint32_t blockSize, uint32_t reservedBlocksPerSlot)
{
    // Verification is active in every build, so the body can rely on these.
    ZEN_RHI_VERIFY(numSlots > 0 && blockSize > 0 && reservedBlocksPerSlot > 0);

    const size_t uniformBufferAlignment = m_pRHI->QueryGPUInfo().uniformBufferAlignment;

    ZEN_RHI_VERIFY(uniformBufferAlignment > 0 && uniformBufferAlignment <= UINT32_MAX);

    m_blockSize      = blockSize;

    m_alignment      = static_cast<uint32_t>(uniformBufferAlignment);

    m_currentSlotIdx = 0;

    m_slots.resize(numSlots);

    for (Slot& slot : m_slots)
    {
        slot.blocks.reserve(reservedBlocksPerSlot);
    }
}

void VulkanUniformRing::Destroy()
{
    for (Slot& slot : m_slots)
    {
        for (Block& block : slot.blocks)
        {
            DestroyBlock(block);
        }

        slot.blocks.clear();

        slot.currentBlockIdx = 0;
    }

    m_slots.clear();

    m_blockSize      = 0;

    m_currentSlotIdx = 0;

    m_alignment      = 1;
}

void VulkanUniformRing::BeginFrame(uint32_t frameNum)
{
    if (!m_slots.empty())
    {
        m_currentSlotIdx = frameNum % static_cast<uint32_t>(m_slots.size());

        Slot& slot       = m_slots[m_currentSlotIdx];

        ++slot.reuseCount;

        for (uint32_t i = 0; i < slot.blocks.size(); ++i)
        {
            Block& block = slot.blocks[i];

            // Keep recent demand plus one spare. Age each slot only when it is reused.
            if (i <= slot.usedBlocks)
            {
                block.lastNeededReuseCount = slot.reuseCount;
            }

            // Invalidate borrowed cached values before any block can be reset by Alloc.
            // Recorded commands remain protected by pending counts / queue serials.
            block.memory.generation = ++m_nextGeneration;

            block.resetPending      = true;
        }

        while (!slot.blocks.empty())
        {
            Block& block = slot.blocks.back();

            if (slot.reuseCount - block.lastNeededReuseCount < kTrimDelay || !block.CanReuse(m_pRHI->GetLifetimeTracker()))
            {
                break;
            }

            DestroyBlock(block);

            slot.blocks.pop_back();
        }

        slot.currentBlockIdx = 0;

        slot.usedBlocks      = 0;
    }
}

bool VulkanUniformRing::Block::CanReuse(VulkanLifetimeTracker& tracker) const
{
    return memory.pBuffer->GetRefCount() == 1 && tracker.IsComplete(lifetimeId);
}

const VulkanUniformRing::Block* VulkanUniformRing::FindBlock(uint64_t blockId) const
{
    const uint32_t slotIndex  = static_cast<uint32_t>(blockId >> 32);

    const uint32_t blockIndex = static_cast<uint32_t>(blockId) - 1;

    return slotIndex < m_slots.size() && blockIndex < m_slots[slotIndex].blocks.size() ? &m_slots[slotIndex].blocks[blockIndex]
                                                                                       : nullptr;
}

uint64_t VulkanUniformRing::GetBlockGeneration(uint64_t blockId) const
{
    const Block* block = FindBlock(blockId);

    return block != nullptr ? block->memory.generation : 0;
}

uint64_t VulkanUniformRing::GetBlockLifetime(uint64_t blockId) const
{
    const Block* block = FindBlock(blockId);

    return block != nullptr ? block->lifetimeId : 0;
}

uint32_t VulkanUniformRing::GetAllocatedBlockCount(uint32_t slotIndex) const
{
    return slotIndex < m_slots.size() ? static_cast<uint32_t>(m_slots[slotIndex].blocks.size()) : 0;
}

RHIResult<VulkanUniformBlock> VulkanUniformRing::Alloc(uint32_t size)
{
    ZEN_RHI_VERIFY(size > 0);

    ZEN_RHI_VERIFY(!m_slots.empty());

    VulkanUniformBlock allocation{};

    RHIError error = MakeRHIError(RHIErrorCode::eOutOfDeviceMemory, "Allocate packed uniform buffer", __FILE__, __LINE__);

    if (size > m_blockSize)
    {
        error = MakeRHIError(RHIErrorCode::eUnsupported, "Allocate packed uniform buffer", __FILE__, __LINE__);
        ZEN_RHI_LOG_ERROR("Packed uniform allocation {} exceeds block capacity {}", size, m_blockSize);
    }

    if (size > 0 && !m_slots.empty() && size <= m_blockSize)
    {
        Slot& slot = m_slots[m_currentSlotIdx];

        while (true)
        {
            if (slot.currentBlockIdx == slot.blocks.size())
            {
                RHIResult<VulkanUniformBlock> created = CreateBlock();

                if (!created)
                {
                    error = created.GetError();

                    break;
                }

                VulkanUniformBlock block = created.GetValue();

                block.blockId =
                    (static_cast<uint64_t>(m_currentSlotIdx) << 32) | (static_cast<uint64_t>(slot.currentBlockIdx) + 1);

                block.generation             = ++m_nextGeneration;

                Block& storage               = slot.blocks.emplace_back();

                storage.memory               = block;

                storage.lifetimeId           = m_pRHI->GetLifetimeTracker().Create();

                storage.lastNeededReuseCount = slot.reuseCount;
            }

            Block& storage = slot.blocks[slot.currentBlockIdx];

            if (storage.resetPending)
            {
                // Unsubmitted and unfinished GPU work can outlive a slot reuse.
                if (!storage.CanReuse(m_pRHI->GetLifetimeTracker()))
                {
                    ++slot.currentBlockIdx;

                    continue;
                }

                storage.memory.offset = 0;

                storage.resetPending  = false;
            }

            VulkanUniformBlock& block = storage.memory;

            const uint64_t alignedOffset =
                ((static_cast<uint64_t>(block.offset) + m_alignment - 1) / m_alignment) * m_alignment;

            const uint64_t allocationEnd = alignedOffset + size;

            if (allocationEnd <= block.size)
            {
                allocation.pBuffer    = block.pBuffer;

                allocation.offset     = static_cast<uint32_t>(alignedOffset);

                allocation.size       = size;

                allocation.pMapped    = block.pMapped + alignedOffset;

                allocation.blockId    = block.blockId;

                allocation.generation = block.generation;

                block.offset          = static_cast<uint32_t>(allocationEnd);

                slot.usedBlocks       = slot.currentBlockIdx + 1;

                break;
            }

            ++slot.currentBlockIdx;
        }
    }

    return allocation.IsValid() ? RHIResult<VulkanUniformBlock>(allocation) : RHIResult<VulkanUniformBlock>(error);
}

RHIResult<VulkanUniformBlock> VulkanUniformRing::CreateBlock() const
{
    RHIBufferCreateInfo createInfo{};

    createInfo.size         = m_blockSize;

    createInfo.allocateType = RHIBufferAllocateType::eCPUWriteGPURead;

    createInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eUniformBuffer);

    createInfo.tag = "VulkanUniformRing";

    VulkanUniformBlock block{};

    // A blocked device reports its terminal error; otherwise the buffer's own creation error.
    const RHIResult<VulkanBuffer*> buffer =
        m_pRHI->AreSubmissionsBlocked()
            ? RHIResult<VulkanBuffer*>(
                  RHIErrorOr(m_pRHI->GetTerminalError(),
                             MakeRHIError(RHIErrorCode::eBackendFailure, "Create packed uniform buffer", __FILE__, __LINE__)))
            : VulkanBuffer::CreateObject(m_pRHI, createInfo);

    block.pBuffer  = buffer.GetValueOr(nullptr);

    RHIError error = buffer.GetError();

    if (block.pBuffer != nullptr)
    {
        block.size    = m_blockSize;

        block.pMapped = block.pBuffer->Map();

        if (block.pMapped == nullptr)
        {
            error = MakeVulkanError(VK_ERROR_MEMORY_MAP_FAILED, "Map packed uniform buffer", __FILE__, __LINE__);
            m_pRHI->DestroyBuffer(block.pBuffer);

            block = {};
        }
    }

    return error.IsFailure() ? RHIResult<VulkanUniformBlock>(error) : RHIResult<VulkanUniformBlock>(block);
}

void VulkanUniformRing::DestroyBlock(Block& block) const
{
    if (block.memory.pBuffer != nullptr)
    {
        m_pRHI->GetLifetimeTracker().Retire(block.lifetimeId, block.memory.pBuffer, [](void* resource) {
            RHIBuffer* buffer = static_cast<RHIBuffer*>(resource);

            buffer->Unmap();

            buffer->ReleaseReference();
        });
    }

    block = {};
}
} // namespace zen
