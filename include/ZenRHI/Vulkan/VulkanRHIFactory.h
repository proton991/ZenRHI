#pragma once

namespace zen
{
class DynamicRHI;
class RHIDeviceDesc;

DynamicRHI* CreateVulkanRHI(const RHIDeviceDesc& description);
} // namespace zen
