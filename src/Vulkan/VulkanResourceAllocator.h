#pragma once
#include "VulkanBuffer.h"
#include "VulkanAccelerationStructure.h"
#include "VulkanTexture.h"
#include "VulkanPipeline.h"
#include "VulkanViewport.h"
#include "VulkanRHI.h"
#include "ZenFoundation/Memory/PagedAllocator.h"

namespace zen
{
// This helps using a single paged allocator for many resource types.
template <typename... RESOURCE_TYPES> struct VersatileResourceTemplate
{
    static constexpr size_t RESOURCE_SIZES[]  = {sizeof(RESOURCE_TYPES)...};
    static constexpr size_t MAX_RESOURCE_SIZE = std::max_element(RESOURCE_SIZES, RESOURCE_SIZES + sizeof...(RESOURCE_TYPES))[0];
    static constexpr size_t MAX_RESOURCE_ALIGNMENT = std::max({alignof(RESOURCE_TYPES)...});
    static_assert(MAX_RESOURCE_SIZE % MAX_RESOURCE_ALIGNMENT == 0);
    alignas(RESOURCE_TYPES...) uint8_t data[MAX_RESOURCE_SIZE];

    template <typename T> static T* AllocMem(PagedAllocator<VersatileResourceTemplate>& allocator)
    {
        T* pMem = (T*)allocator.Alloc();
        return pMem;
    }

    template <typename T> static void Free(PagedAllocator<VersatileResourceTemplate>& p_allocator, T* pMem)
    {
        p_allocator.Free((VersatileResourceTemplate*)pMem);
    }
};

// Allocates a resource from the device's resource pages, constructs it and binds it to the device runtime.
template <typename T, typename... Args> T* NewVulkanResource(VulkanRHI* pRHI, Args&&... args)
{
    T* resource = VersatileResource::AllocMem<T>(pRHI->GetResourceAllocator());

    new (resource) T(std::forward<Args>(args)...);

    resource->BindRuntime(&pRHI->GetRuntime());

    return resource;
}

// Destroys a resource made by NewVulkanResource and returns its storage. The allocator is read before the destructor
// runs, because the resource's members end with it.
template <typename T> void DeleteVulkanResource(VulkanRHI* pRHI, T* resource)
{
    PagedAllocator<VersatileResource>& allocator = pRHI->GetResourceAllocator();

    resource->~T();

    VersatileResource::Free(allocator, resource);
}

// Keeps a resource whose initialisation succeeded; otherwise releases it and returns the error.
template <typename T> RHIResult<T*> KeepIfInitialized(T* resource, const RHIError& error)
{
    if (error.IsFailure())
    {
        resource->ReleaseReference();
    }

    return error.IsFailure() ? RHIResult<T*>(error) : RHIResult<T*>(resource);
}

template <typename T> RHIResult<T*> RejectDescription(const char* rule, const char* source, uint32_t line)
{
    return RHIResult<T*>(MakeRHIError(RHIErrorCode::eInvalidArgument, rule, source, line));
}
} // namespace zen