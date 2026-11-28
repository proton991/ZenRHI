#pragma once
#include "ZenRHI/RHIResource.h"

namespace zen
{
// Backend-neutral creation rules, checked against the device's RHIGPUInfo. Each returns the rule a description
// breaks, or null. Backends add the checks that need native queries, such as format support and native limits.
bool IsTextureShapeValid(const RHITextureCreateInfo& info);

const char* ValidateBufferCreateInfo(const RHIBufferCreateInfo& info, const RHIGPUInfo& gpu);

const char* ValidateTextureCreateInfo(const RHITextureCreateInfo& info, const RHIGPUInfo& gpu);

const char* ValidateTextureViewCreateInfo(const RHITextureCreateInfo&     image,
                                          const RHITextureViewCreateInfo& view,
                                          const RHIGPUInfo&               gpu);

const char* ValidateSamplerCreateInfo(const RHISamplerCreateInfo& info, const RHIGPUInfo& gpu);

// The stage contract: at least one stage, no tessellation, and ray-tracing stages only on their own.
const char* ValidateShaderStages(const RHIShaderGroupSPIRV* spirv, const RHIGPUInfo& gpu);
} // namespace zen
