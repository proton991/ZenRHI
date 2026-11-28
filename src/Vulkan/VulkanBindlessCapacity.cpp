#include "Vulkan/VulkanBindlessCapacity.h"
#include <algorithm>
#include <limits>

namespace zen
{
bool BindlessHeapsFitLimits(const VkPhysicalDeviceDescriptorIndexingProperties& limits,
                            const RHIBindlessHeapCapacities&                    capacities)
{
    const uint64_t sampledImageCount = capacities.GetImageSlots();

    const uint64_t samplerCount      = capacities.Get(RHIBindlessHeapType::eSampler);

    const uint64_t descriptorCount   = sampledImageCount + samplerCount;

    return sampledImageCount <= limits.maxDescriptorSetUpdateAfterBindSampledImages
        && sampledImageCount <= limits.maxPerStageDescriptorUpdateAfterBindSampledImages
        && samplerCount <= limits.maxDescriptorSetUpdateAfterBindSamplers
        && samplerCount <= limits.maxPerStageDescriptorUpdateAfterBindSamplers
        && sampledImageCount <= limits.maxPerStageUpdateAfterBindResources
        && descriptorCount <= limits.maxUpdateAfterBindDescriptorsInAllPools;
}

namespace
{
void ReduceBindlessHeapCapacities(RHIBindlessHeapCapacities& capacities, uint32_t budget, bool imagesOnly)
{
    const RHIBindlessHeapCapacities requested = capacities;
    const uint32_t                  heapCount = imagesOnly ? kBindlessHeapCount - 1 : kBindlessHeapCount;
    const uint64_t total = capacities.GetImageSlots() + (imagesOnly ? 0 : capacities.Get(RHIBindlessHeapType::eSampler));
    if (total > budget)
    {
        const uint64_t requestedExtras = total - heapCount;
        const uint64_t availableExtras = budget - heapCount;
        uint64_t       remaining       = availableExtras;
        for (uint32_t index = 0; index < kBindlessHeapCount; ++index)
        {
            const RHIBindlessHeapType heap = static_cast<RHIBindlessHeapType>(index);
            if (!imagesOnly || heap != RHIBindlessHeapType::eSampler)
            {
                // Both factors fit 32 bits, so their product fits 64 bits.
                const uint64_t extras = (uint64_t(requested.Get(heap) - 1) * availableExtras) / requestedExtras;
                capacities.Set(heap, 1 + static_cast<uint32_t>(extras));
                remaining -= extras;
            }
        }
        // Flooring loses fewer than heapCount slots. Distribute them in binding order.
        for (uint32_t index = 0; remaining != 0 && index < kBindlessHeapCount; ++index)
        {
            const RHIBindlessHeapType heap = static_cast<RHIBindlessHeapType>(index);
            if ((!imagesOnly || heap != RHIBindlessHeapType::eSampler) && capacities.Get(heap) < requested.Get(heap))
            {
                capacities.Set(heap, capacities.Get(heap) + 1);
                --remaining;
            }
        }
        ZEN_RHI_VERIFY(remaining == 0);
    }
}
} // namespace

RHIBindlessHeapCapacities GetBindlessHeapRequest(const RHIBindlessHeapCapacities& requested, bool imageCubeArray)
{
    RHIBindlessHeapCapacities request = requested;

    if (!imageCubeArray)
    {
        request.Set(RHIBindlessHeapType::eTextureCubeArray, 1);
    }

    return request;
}

bool FitBindlessHeapCapacities(const VkPhysicalDeviceDescriptorIndexingProperties& limits,
                               const RHIBindlessHeapCapacities&                    requested,
                               RHIBindlessHeapCapacities&                          outCapacities)
{
    const uint32_t imageLimit =
        std::min({limits.maxDescriptorSetUpdateAfterBindSampledImages, limits.maxPerStageDescriptorUpdateAfterBindSampledImages,
                  limits.maxPerStageUpdateAfterBindResources});
    const uint32_t samplerLimit =
        std::min(limits.maxDescriptorSetUpdateAfterBindSamplers, limits.maxPerStageDescriptorUpdateAfterBindSamplers);
    // Ordinary sets use their own pools, so the update-after-bind pool budget needs no reserve.
    bool valid = imageLimit >= kBindlessOrdinaryDescriptorReserve + kBindlessHeapCount - 1
              && samplerLimit >= kBindlessOrdinaryDescriptorReserve + 1
              && limits.maxUpdateAfterBindDescriptorsInAllPools >= kBindlessHeapCount;
    for (uint32_t count : requested.slots)
    {
        valid = valid && count != 0;
    }
    if (valid)
    {
        RHIBindlessHeapCapacities fitted = requested;
        ReduceBindlessHeapCapacities(fitted, imageLimit - kBindlessOrdinaryDescriptorReserve, true);
        fitted.Set(RHIBindlessHeapType::eSampler,
                   std::min(fitted.Get(RHIBindlessHeapType::eSampler), samplerLimit - kBindlessOrdinaryDescriptorReserve));
        ReduceBindlessHeapCapacities(fitted, limits.maxUpdateAfterBindDescriptorsInAllPools, false);
        ZEN_RHI_VERIFY(BindlessHeapsFitLimits(limits, fitted));
        outCapacities = fitted;
    }
    return valid;
}
} // namespace zen
