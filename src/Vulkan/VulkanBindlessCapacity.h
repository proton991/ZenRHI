#pragma once
#include "Vulkan/VulkanHeaders.h"
#include "ZenRHI/RHICommon.h"

namespace zen
{
inline constexpr uint32_t kBindlessHeapCount = ToUnderlying(RHIBindlessHeapType::eMax);

inline constexpr RHIBindlessHeapCapacities kNoBindlessHeaps{{0, 0, 0, 0, 0}};

// Every pipeline layout holds the global heaps beside ordinary descriptors and color
// attachments, and all of them count toward the same limits. Fitting keeps this many free,
// well above the 69 resources of the engine's largest shader stage and color target set.
inline constexpr uint32_t kBindlessOrdinaryDescriptorReserve = 256;

// True when the update-after-bind descriptor-indexing limits can hold the global heaps.
bool BindlessHeapsFitLimits(const VkPhysicalDeviceDescriptorIndexingProperties& limits,
                            const RHIBindlessHeapCapacities&                    capacities);

// The configured request for a device. Without imageCubeArray nothing can use the cube-array
// heap, so it requests only its minimum slot and leaves the budget to the other heaps.
RHIBindlessHeapCapacities GetBindlessHeapRequest(const RHIBindlessHeapCapacities& requested, bool imageCubeArray);

// Reduces requested heap sizes to the numeric device limits less the ordinary reserve,
// preserving at least one slot per heap. Failure leaves outCapacities unchanged.
bool FitBindlessHeapCapacities(const VkPhysicalDeviceDescriptorIndexingProperties& limits,
                               const RHIBindlessHeapCapacities&                    requested,
                               RHIBindlessHeapCapacities&                          outCapacities);
} // namespace zen
