#pragma once
#include "Vulkan/VulkanHeaders.h"
#include "ZenRHI/RHICommon.h"
#include "ZenFoundation/Templates/FlatHashMap.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Templates/SmallVector.h"
#include "ZenFoundation/Utils/Mutex.h"
#include <algorithm>
#include <cstring>
#include <span>

namespace zen
{
class VulkanDevice;

// Inline storage for per-set containers, not a limit. Descriptor set indices are bounded
// only by the device, reported as RHIGPUInfo::maxBoundDescriptorSets.
inline constexpr uint32_t kInlineDescriptorSetCount = 8;

// Descriptor counts per resource type. Descriptor pools are sized and grouped by it.
struct VulkanDescriptorPoolKey
{
    uint32_t descriptorCount[ToUnderlying(RHIShaderResourceType::eMax)] = {};

    bool operator==(const VulkanDescriptorPoolKey& other) const
    {
        return memcmp(descriptorCount, other.descriptorCount, sizeof(descriptorCount)) == 0;
    }
};

// One descriptor set layout, shared by every shader that declares the same bindings in a set.
struct VulkanSetLayout
{
    struct DynamicOffsetSlot
    {
        uint32_t binding{0};
        uint32_t count{0};
    };

    VkDescriptorSetLayout handle{VK_NULL_HANDLE};
    // Equal ids mean equal layouts, so sets with equal contents are shared. The global heap's id is 0.
    uint32_t                id{0};
    VulkanDescriptorPoolKey poolKey{};
    // Uniform buffers bind with dynamic offsets, one per array element, in binding order.
    SmallVector<DynamicOffsetSlot> dynamicOffsets;
};

// Creates each distinct set layout once. Layouts live until the device is destroyed.
class VulkanSetLayoutCache
{
public:
    explicit VulkanSetLayoutCache(VulkanDevice* pDevice) : m_pDevice(pDevice) {}

    ~VulkanSetLayoutCache();

    VulkanSetLayoutCache(const VulkanSetLayoutCache&)            = delete;

    VulkanSetLayoutCache& operator=(const VulkanSetLayoutCache&) = delete;

    // The shared layout for one set's descriptors, or nullptr when Vulkan rejects it.
    const VulkanSetLayout* GetOrCreate(std::span<const RHIShaderResourceDescriptor> descriptors);

private:
    struct Key
    {
        HeapVector<uint64_t> data;

        bool operator==(const Key& other) const
        {
            return data.size() == other.data.size() && std::equal(data.begin(), data.end(), other.data.begin());
        }
    };

    struct KeyHasher
    {
        size_t operator()(const Key& key) const noexcept;
    };

    VulkanSetLayout* Create(std::span<const RHIShaderResourceDescriptor* const> descriptors);

    VulkanDevice*                                 m_pDevice{nullptr};
    Mutex                                         m_mutex;
    FlatHashMap<Key, VulkanSetLayout*, KeyHasher> m_layouts;
    uint32_t                                      m_nextId{1};
};
} // namespace zen
