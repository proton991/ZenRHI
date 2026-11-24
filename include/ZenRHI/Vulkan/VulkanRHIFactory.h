#pragma once
#include "ZenRHI/DynamicRHI.h"

namespace zen
{
RHIResult<std::vector<RHIAdapterInfo>> EnumerateVulkanAdapters(const RHIDeviceDesc& description);
RHIResult<DynamicRHI*>                 CreateVulkanRHIChecked(const RHIDeviceDesc& description);

DynamicRHI* CreateVulkanRHI(const RHIDeviceDesc& description);
} // namespace zen
