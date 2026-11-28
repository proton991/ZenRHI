#include "Vulkan/VulkanDescriptorAllocator.h"
#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanTypes.h"
#include "ZenFoundation/Memory/Memory.h"
#include "ZenFoundation/Templates/SmallVector.h"
#include "ZenFoundation/Utils/Helpers.h"

namespace zen
{
namespace
{
// A chain's pools hold 32, 64, ... 512 sets.
constexpr uint32_t kInitialSetsPerPool = 32;
constexpr uint32_t kMaxPoolGrowthShift = 4;
// A free arena unused for this many frames is destroyed, one per frame.
constexpr uint32_t kArenaIdleFrames = 8;

struct PoolKeyHasher
{
    size_t operator()(const VulkanDescriptorPoolKey& poolKey) const noexcept
    {
        size_t hash = 0;

        for (uint32_t descriptorCount : poolKey.descriptorCount)
        {
            util::HashCombine(hash, descriptorCount);
        }

        return hash;
    }
};

bool IsPoolExhausted(VkResult result)
{
    return result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL;
}
} // namespace

struct VulkanDescriptorArena
{
    struct Pool
    {
        VkDescriptorPool handle{VK_NULL_HANDLE};
        uint32_t         capacity{0};
        uint32_t         allocated{0};
    };

    struct Chain
    {
        SmallVector<Pool> pools;
        uint32_t          active{0};
    };

    explicit VulkanDescriptorArena(VulkanDevice* pDevice) :
        pDevice(pDevice), lifetimeId(pDevice->GetRHI()->GetLifetimeTracker().Create())
    {}

    ~VulkanDescriptorArena()
    {
        for (std::pair<const VulkanDescriptorPoolKey, Chain>& chain : chains)
        {
            for (const Pool& pool : chain.second.pools)
            {
                if (pool.handle != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorPool(pDevice->GetVkHandle(), pool.handle, nullptr);
                }
            }
        }
    }

    VulkanDescriptorArena(const VulkanDescriptorArena&)            = delete;

    VulkanDescriptorArena& operator=(const VulkanDescriptorArena&) = delete;

    // Earlier pools of the chain are full. A full pool passes the request on; when none is left the chain grows.
    RHIResult<VkDescriptorSet> Allocate(const VulkanSetLayout& layout)
    {
        Chain&          chain  = chains[layout.poolKey];
        VkDescriptorSet set    = VK_NULL_HANDLE;
        VkResult        result = VK_ERROR_OUT_OF_POOL_MEMORY;
        RHIError        error{};

        while (set == VK_NULL_HANDLE && IsPoolExhausted(result) && chain.active < chain.pools.size())
        {
            result        = AllocateFromPool(chain.pools[chain.active], layout.handle, set);

            chain.active += IsPoolExhausted(result) ? 1 : 0;
        }

        if (set == VK_NULL_HANDLE && IsPoolExhausted(result))
        {
            const uint32_t growth = std::min(static_cast<uint32_t>(chain.pools.size()), kMaxPoolGrowthShift);

            RHIResult<Pool> pool  = CreatePool(layout.poolKey, kInitialSetsPerPool << growth);

            if (pool)
            {
                chain.pools.push_back(pool.GetValue());

                chain.active = static_cast<uint32_t>(chain.pools.size()) - 1;

                result       = AllocateFromPool(chain.pools[chain.active], layout.handle, set);
            }
            else
            {
                error = pool.GetError();
            }
        }

        if (set == VK_NULL_HANDLE && !error.IsFailure())
        {
            error = pDevice->GetRHI()->ReportCreationFailure(result, "vkAllocateDescriptorSets", __FILE__, __LINE__);
        }

        return set != VK_NULL_HANDLE ? RHIResult<VkDescriptorSet>(set) : RHIResult<VkDescriptorSet>(error);
    }

    // Returns VK_ERROR_OUT_OF_POOL_MEMORY without calling Vulkan for a full or dropped pool.
    VkResult AllocateFromPool(Pool& pool, VkDescriptorSetLayout layout, VkDescriptorSet& set)
    {
        VkResult result = VK_ERROR_OUT_OF_POOL_MEMORY;

        if (pool.handle != VK_NULL_HANDLE && pool.allocated < pool.capacity)
        {
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

            allocateInfo.descriptorPool      = pool.handle;

            allocateInfo.descriptorSetCount  = 1;

            allocateInfo.pSetLayouts         = &layout;

            result                           = vkAllocateDescriptorSets(pDevice->GetVkHandle(), &allocateInfo, &set);

            pool.allocated                  += result == VK_SUCCESS ? 1 : 0;

            set                              = result == VK_SUCCESS ? set : VK_NULL_HANDLE;
        }

        return result;
    }

    RHIResult<Pool> CreatePool(const VulkanDescriptorPoolKey& poolKey, uint32_t capacity)
    {
        // Types that share a Vulkan descriptor type may repeat; the pool reserves their sum.
        SmallVector<VkDescriptorPoolSize, ToUnderlying(RHIShaderResourceType::eMax)> sizes;

        for (uint32_t type = 0; type < ToUnderlying(RHIShaderResourceType::eMax); ++type)
        {
            if (poolKey.descriptorCount[type] > 0)
            {
                sizes.push_back({GetVulkanDescriptorTraits(static_cast<RHIShaderResourceType>(type)).type,
                                 poolKey.descriptorCount[type] * capacity});
            }
        }

        VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};

        poolCI.maxSets       = capacity;

        poolCI.poolSizeCount = static_cast<uint32_t>(sizes.size());

        poolCI.pPoolSizes    = sizes.empty() ? nullptr : sizes.data();

        Pool pool{VK_NULL_HANDLE, capacity, 0};

        const VkResult result = vkCreateDescriptorPool(pDevice->GetVkHandle(), &poolCI, nullptr, &pool.handle);

        return result == VK_SUCCESS ? RHIResult<Pool>(pool)
                                    : RHIResult<Pool>(pDevice->GetRHI()->ReportCreationFailure(result, "vkCreateDescriptorPool",
                                                                                               __FILE__, __LINE__));
    }

    void Reset()
    {
        for (std::pair<const VulkanDescriptorPoolKey, Chain>& chain : chains)
        {
            for (Pool& pool : chain.second.pools)
            {
                const VkResult result = pool.handle != VK_NULL_HANDLE && pool.allocated > 0
                                          ? vkResetDescriptorPool(pDevice->GetVkHandle(), pool.handle, 0)
                                          : VK_SUCCESS;

                pool.allocated        = 0;

                if (result != VK_SUCCESS)
                {
                    // A pool that cannot reset is dropped; the chain grows past it.
                    ZEN_RHI_LOG_ERROR("vkResetDescriptorPool failed: {}", GetResultString(result));

                    vkDestroyDescriptorPool(pDevice->GetVkHandle(), pool.handle, nullptr);

                    pool.handle = VK_NULL_HANDLE;
                }
            }

            chain.second.active = 0;
        }

        setCount   = 0;

        idleFrames = 0;
    }

    VulkanDevice*                                              pDevice{nullptr};
    uint64_t                                                   lifetimeId{0};
    uint64_t                                                   generation{0};
    uint32_t                                                   setCount{0};
    uint32_t                                                   idleFrames{0};
    FlatHashMap<VulkanDescriptorPoolKey, Chain, PoolKeyHasher> chains;
};

namespace
{
void DestroyArena(void* arena)
{
    ZEN_DELETE(static_cast<VulkanDescriptorArena*>(arena));
}

void RetireArena(VulkanDescriptorArena* arena)
{
    arena->pDevice->GetRHI()->GetLifetimeTracker().Retire(arena->lifetimeId, arena, DestroyArena);
}
} // namespace

size_t VulkanDescriptorAllocator::KeyHasher::operator()(const Key& key) const noexcept
{
    size_t hash = 0;

    util::HashCombine(hash, key.layoutId);

    for (uint64_t value : key.bindings)
    {
        util::HashCombine(hash, value);
    }

    return hash;
}

VulkanDescriptorAllocator::VulkanDescriptorAllocator(VulkanDevice* pDevice, uint32_t setsPerArena) :
    m_pDevice(pDevice), m_setsPerArena(std::max(setsPerArena, 1u))
{}

VulkanDescriptorAllocator::~VulkanDescriptorAllocator()
{
    for (uint32_t i = 0; i < m_ringSize; ++i)
    {
        RetireArena(m_ring[i]);
    }

    for (VulkanDescriptorArena* arena : m_free)
    {
        RetireArena(arena);
    }

    for (VulkanDescriptorArena* arena : m_retired)
    {
        RetireArena(arena);
    }
}

VulkanDescriptorAllocator::Set VulkanDescriptorAllocator::Find(const Key& key)
{
    Set set{};

    const FlatHashMap<Key, Entry, KeyHasher>::iterator found = m_entries.find(key);

    for (uint32_t i = 0; found != m_entries.end() && i < std::min(m_ringSize, kSearchedArenas); ++i)
    {
        if (m_ring[i]->generation == found->second.generation)
        {
            set = {found->second.handle, m_ring[i]->lifetimeId};
        }
    }

    RHIExecutionCounterStorage& counters = m_pDevice->GetRHI()->GetExecutionCounterStorage();

    counters.Increment(set.handle != VK_NULL_HANDLE ? counters.descriptorHits : counters.descriptorMisses);

    return set;
}

VulkanDescriptorAllocator::Set VulkanDescriptorAllocator::Insert(const Key&                      key,
                                                                 const VulkanSetLayout&          layout,
                                                                 std::span<VkWriteDescriptorSet> writes)
{
    RHIExecutionCounterStorage& counters = m_pDevice->GetRHI()->GetExecutionCounterStorage();

    counters.Increment(counters.descriptorInserts);

    if (m_ringSize == 0 || m_ring[0]->setCount >= m_setsPerArena)
    {
        StartArena();
    }

    VulkanDescriptorArena* arena               = m_ring[0];

    const RHIResult<VkDescriptorSet> allocated = arena->Allocate(layout);

    Set set{allocated.GetValueOr(VK_NULL_HANDLE), arena->lifetimeId, allocated.GetError()};

    if (set.handle != VK_NULL_HANDLE)
    {
        for (VkWriteDescriptorSet& write : writes)
        {
            write.dstSet = set.handle;
        }

        if (!writes.empty())
        {
            vkUpdateDescriptorSets(m_pDevice->GetVkHandle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        m_entries[key] = {set.handle, arena->generation};

        ++arena->setCount;
    }
    else
    {
        set.lifetimeId = 0;
    }

    return set;
}

void VulkanDescriptorAllocator::BeginFrame()
{
    ReclaimRetiredArenas();

    for (VulkanDescriptorArena* arena : m_free)
    {
        ++arena->idleFrames;
    }

    if (!m_free.empty() && m_free[0]->idleFrames > kArenaIdleFrames)
    {
        RetireArena(m_free[0]);

        m_free.pop_front();
    }
}

void VulkanDescriptorAllocator::StartArena()
{
    if (m_ringSize == kRingArenas)
    {
        // The oldest arena's sets were not searchable; drop their entries and reset the arena once its work completes.
        m_retired.push_back(m_ring[--m_ringSize]);

        RHIExecutionCounterStorage& counters = m_pDevice->GetRHI()->GetExecutionCounterStorage();

        counters.Increment(counters.descriptorRetirements);

        const uint64_t oldestLive = m_ring[m_ringSize - 1]->generation;

        for (FlatHashMap<Key, Entry, KeyHasher>::iterator it = m_entries.begin(); it != m_entries.end();)
        {
            if (it->second.generation < oldestLive)
            {
                it = m_entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    ReclaimRetiredArenas();

    for (uint32_t i = m_ringSize; i > 0; --i)
    {
        m_ring[i] = m_ring[i - 1];
    }

    VulkanDescriptorArena* arena = nullptr;

    if (!m_free.empty())
    {
        arena = m_free[0];

        m_free.pop_front();
    }
    else
    {
        arena = ZEN_NEW() VulkanDescriptorArena(m_pDevice);
    }

    arena->generation = ++m_revision;

    m_ring[0]         = arena;

    ++m_ringSize;
}

void VulkanDescriptorAllocator::ReclaimRetiredArenas()
{
    VulkanLifetimeTracker& tracker = m_pDevice->GetRHI()->GetLifetimeTracker();

    size_t kept                    = 0;

    for (VulkanDescriptorArena* arena : m_retired)
    {
        if (tracker.IsComplete(arena->lifetimeId))
        {
            arena->Reset();

            m_free.push_back(arena);
        }
        else
        {
            m_retired[kept++] = arena;
        }
    }

    m_retired.resize(kept);
}
} // namespace zen
