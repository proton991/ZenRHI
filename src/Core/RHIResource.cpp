#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/RHIResource.h"
#include "ZenRHI/DynamicRHI.h"

namespace zen
{
RHIResource::~RHIResource()
{
    ZEN_RHI_VERIFY(m_counter.GetValue() == 0);

    // The last owner may be an RHI batch or a bindless registration. Publish only
    // identity: RenderCore must never dereference a resource after final release.
    if (GDynamicRHI != nullptr && (m_resourceType == RHIResourceType::eBuffer || m_resourceType == RHIResourceType::eTexture))
    {
        GDynamicRHI->NotifyResourceDestroyed(m_stableId);
    }
}
} // namespace zen
