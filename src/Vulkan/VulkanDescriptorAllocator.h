#pragma once
#include "Vulkan/VulkanSetLayout.h"
#include "ZenRHI/RHIShaderParameters.h"
#include "ZenFoundation/Templates/FlatHashMap.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include <algorithm>
#include <span>

namespace zen
{
class VulkanDevice;
struct VulkanDescriptorArena;

// Hands out descriptor sets by content. A set whose layout and resources match one written recently is reused;
// otherwise a set is allocated from the newest arena and written. An arena keeps a growing pool chain per pool key.
// When the newest arena fills, a new one starts: the newest two stay searchable, the third only stays alive, and
// the one before leaves the ring. A retired arena is reset only after every recording and submission that bound its
// sets has completed. Contexts declare that by recording the set's lifetime id.
class VulkanDescriptorAllocator
{
public:
    static constexpr uint32_t kSetsPerArena = 512;

    // The layout id, then canonical binding metadata followed by every resource slot, including nulls.
    struct Key
    {
        uint32_t             layoutId{0};
        HeapVector<uint64_t> bindings;

        bool operator==(const Key& other) const noexcept
        {
            return layoutId == other.layoutId && bindings.size() == other.bindings.size()
                && std::equal(bindings.begin(), bindings.end(), other.bindings.begin());
        }
    };

    struct Set
    {
        VkDescriptorSet handle{VK_NULL_HANDLE};
        // The arena's lifetime id; zero when there is no set.
        uint64_t lifetimeId{0};
        // Why there is no set after an insert.
        RHIError error{};
    };

    explicit VulkanDescriptorAllocator(VulkanDevice* pDevice, uint32_t setsPerArena = kSetsPerArena);

    // Arenas are destroyed once their work completes, including uncertain submissions kept until queue teardown.
    ~VulkanDescriptorAllocator();

    VulkanDescriptorAllocator(const VulkanDescriptorAllocator&)            = delete;

    VulkanDescriptorAllocator& operator=(const VulkanDescriptorAllocator&) = delete;

    // A searchable set with these contents, or no set.
    Set Find(const Key& key);

    // Allocates a set of this layout, points the writes at it, applies them and makes the set findable under key.
    Set Insert(const Key& key, const VulkanSetLayout& layout, std::span<VkWriteDescriptorSet> writes);

    // Resets retired arenas whose work completed, and destroys a free arena that stayed unused.
    void BeginFrame();

    // Changes whenever a new arena starts. Sets resolved under an older revision must be found again.
    uint64_t GetRevision() const
    {
        return m_revision;
    }

private:
    static constexpr uint32_t kSearchedArenas = 2;
    static constexpr uint32_t kRingArenas     = kSearchedArenas + 1;

    struct KeyHasher
    {
        size_t operator()(const Key& key) const noexcept;
    };

    struct Entry
    {
        VkDescriptorSet handle{VK_NULL_HANDLE};
        uint64_t        generation{0};
    };

    void StartArena();

    void ReclaimRetiredArenas();

    VulkanDevice* m_pDevice{nullptr};
    uint32_t      m_setsPerArena{kSetsPerArena};
    // Newest first. An arena's generation is the revision that started it.
    VulkanDescriptorArena*             m_ring[kRingArenas]{};
    uint32_t                           m_ringSize{0};
    HeapVector<VulkanDescriptorArena*> m_retired;
    HeapVector<VulkanDescriptorArena*> m_free;
    // The newest set per key. Publishing only follows a miss, so no older entry can be searchable.
    FlatHashMap<Key, Entry, KeyHasher> m_entries;
    uint64_t                           m_revision{0};
};
} // namespace zen
