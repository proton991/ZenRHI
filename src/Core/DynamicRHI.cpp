#include "ZenRHI/DynamicRHI.h"
#include "ZenRHI/RHIThread.h"

namespace zen
{
DynamicRHI::DynamicRHI()
{
    GetRHIThread().OpenAdmission();
}

DynamicRHI::~DynamicRHI()
{
    if (GDynamicRHI == this)
    {
        GDynamicRHI = nullptr;
    }
}
} // namespace zen
