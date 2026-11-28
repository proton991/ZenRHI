#include "Vulkan/VulkanSetLayout.h"
#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanTypes.h"
#include "ZenFoundation/Utils/Helpers.h"

namespace zen
{
namespace
{
bool BindsBefore(const RHIShaderResourceDescriptor* left, const RHIShaderResourceDescriptor* right)
{
    return left->binding < right->binding;
}
} // namespace

size_t VulkanSetLayoutCache::KeyHasher::operator()(const Key& key) const noexcept
{
    size_t hash = 0;

    for (uint64_t value : key.data)
    {
        util::HashCombine(hash, value);
    }

    return hash;
}

VulkanSetLayoutCache::~VulkanSetLayoutCache()
{
    for (std::pair<const Key, VulkanSetLayout*>& entry : m_layouts)
    {
        vkDestroyDescriptorSetLayout(m_pDevice->GetVkHandle(), entry.second->handle, nullptr);

        ZEN_DELETE(entry.second);
    }
}

const VulkanSetLayout* VulkanSetLayoutCache::GetOrCreate(std::span<const RHIShaderResourceDescriptor> descriptors)
{
    HeapVector<const RHIShaderResourceDescriptor*> sorted;

    for (const RHIShaderResourceDescriptor& descriptor : descriptors)
    {
        sorted.push_back(&descriptor);
    }

    std::sort(sorted.begin(), sorted.end(), BindsBefore);

    // Binding flags, pool sizes and dynamic offsets all follow from these four fields.
    Key key;

    for (const RHIShaderResourceDescriptor* descriptor : sorted)
    {
        key.data.push_back((uint64_t(descriptor->binding) << 32) | ToUnderlying(descriptor->type));

        key.data.push_back((uint64_t(descriptor->arraySize) << 32)
                           | ShaderStageFlagsBitsToVkShaderStageFlags(descriptor->stageFlags));
    }

    LockAuto lock(&m_mutex);

    const FlatHashMap<Key, VulkanSetLayout*, KeyHasher>::iterator found = m_layouts.find(key);

    VulkanSetLayout* layout = found != m_layouts.end()
                                ? found->second
                                : Create(std::span<const RHIShaderResourceDescriptor* const>(sorted.data(), sorted.size()));

    if (found == m_layouts.end() && layout != nullptr)
    {
        m_layouts.emplace(std::move(key), layout);
    }

    return layout;
}

VulkanSetLayout* VulkanSetLayoutCache::Create(std::span<const RHIShaderResourceDescriptor* const> descriptors)
{
    VulkanSetLayout* layout = ZEN_NEW() VulkanSetLayout();

    HeapVector<VkDescriptorSetLayoutBinding> bindings;

    HeapVector<VkDescriptorBindingFlags> bindingFlags;

    for (const RHIShaderResourceDescriptor* descriptor : descriptors)
    {
        VkDescriptorSetLayoutBinding binding{};

        binding.binding         = descriptor->binding;

        binding.descriptorType  = GetVulkanDescriptorTraits(descriptor->type).type;

        binding.descriptorCount = descriptor->arraySize;

        binding.stageFlags      = ShaderStageFlagsBitsToVkShaderStageFlags(descriptor->stageFlags);

        bindings.push_back(binding);

        bindingFlags.push_back(descriptor->arraySize > 1 ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0);

        layout->poolKey.descriptorCount[ToUnderlying(descriptor->type)] += descriptor->arraySize;

        if (descriptor->type == RHIShaderResourceType::eUniformBuffer)
        {
            layout->dynamicOffsets.push_back({descriptor->binding, descriptor->arraySize});
        }
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};

    flagsCI.bindingCount  = static_cast<uint32_t>(bindingFlags.size());

    flagsCI.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    layoutCI.pNext        = &flagsCI;

    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());

    layoutCI.pBindings    = bindings.empty() ? nullptr : bindings.data();

    const VkResult result = vkCreateDescriptorSetLayout(m_pDevice->GetVkHandle(), &layoutCI, nullptr, &layout->handle);

    if (result == VK_SUCCESS && layout->handle != VK_NULL_HANDLE)
    {
        layout->id = m_nextId++;
    }
    else
    {
        m_pDevice->GetRHI()->CheckDeviceLoss(result, "vkCreateDescriptorSetLayout");

        ZEN_RHI_LOG_ERROR("vkCreateDescriptorSetLayout failed: {}", GetResultString(result));

        ZEN_DELETE(layout);

        layout = nullptr;
    }

    return layout;
}
} // namespace zen
