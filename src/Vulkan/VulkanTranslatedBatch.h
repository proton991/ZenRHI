#pragma once
#include "ZenFoundation/Templates/HeapVector.h"
#include <cstdint>

namespace zen
{
class RHICommandList;
class VulkanCommandListContext;
class VulkanWorkload;

// Workloads translated from command lists and not yet submitted, with the lists and transactions that own them.
struct VulkanTranslatedBatch
{
    struct ListRange
    {
        VulkanCommandListContext* pContext{nullptr};
        RHICommandList*           pCommands{nullptr};
        uint32_t                  firstWorkload{0};
        uint32_t                  workloadCount{0};
    };

    HeapVector<VulkanWorkload*> workloads;
    HeapVector<ListRange>       lists;
    HeapVector<uint64_t>        transactions;
};
} // namespace zen
