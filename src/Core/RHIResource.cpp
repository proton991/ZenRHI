#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/RHIResource.h"
#include "ZenRHI/DynamicRHI.h"

namespace zen
{
RHIResource::~RHIResource()
{
    ZEN_RHI_VERIFY(m_counter.GetValue() == 0);

    // The last owner may be an RHI batch or a bindless registration. Publish only
    // identity: the host must never dereference a resource after final release.
    const RHIDeviceRuntime* runtime = GetRuntime();

    if (runtime != nullptr && (m_resourceType == RHIResourceType::eBuffer || m_resourceType == RHIResourceType::eTexture))
    {
        RHIDestructionListener* listener = runtime->destructionListener.load(std::memory_order_acquire);

        if (listener != nullptr)
        {
            listener->OnResourceDestroyed(m_stableId);
        }
    }
}
} // namespace zen
