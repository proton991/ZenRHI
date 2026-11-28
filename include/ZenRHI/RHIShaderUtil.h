#pragma once
#include "RHIResource.h"

namespace zen
{
class RHIShaderUtil
{
public:
    // maxDescriptorSets is the device's maxBoundDescriptorSets; every set index must be below it.
    [[nodiscard]] static bool ReflectShaderGroupInfo(RHIShaderGroupSPIRVPtr shaderGroupSpirv,
                                                     uint32_t               maxDescriptorSets,
                                                     RHIShaderGroupInfo&    shaderGroupInfo);

    // The layout rules every backend relies on: the set count fits maxDescriptorSets, runtime arrays appear
    // only in the global heap set, that set holds only heap bindings, and each descriptor name has one location.
    [[nodiscard]] static bool ValidateShaderLayout(const RHIShaderGroupInfo& info, uint32_t maxDescriptorSets);

    // Set kGlobalBindlessHeapIndex holds only the global heap: binding N is heap N, of that heap's resource type.
    static constexpr bool IsGlobalHeapBinding(uint32_t set, uint32_t binding, RHIShaderResourceType type)
    {
        return set == kGlobalBindlessHeapIndex && binding < ToUnderlying(RHIBindlessHeapType::eMax)
            && type == GetBindlessHeapResourceType(static_cast<RHIBindlessHeapType>(binding));
    }
};
} // namespace zen
