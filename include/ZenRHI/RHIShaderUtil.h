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
};
} // namespace zen
