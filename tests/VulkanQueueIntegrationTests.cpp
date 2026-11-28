#include "Vulkan/VulkanSynchronization.h"
#include "Vulkan/VulkanTypes.h"
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanExtension.h"
#include "Vulkan/VulkanCommandList.h"
#include <unordered_map>
#include <vector>
#include <type_traits>

#include "VulkanIntegrationFixture.h"

// Intercept the Vulkan calls made by the actual queue and semaphore implementations.
// No graphics device is needed, and a later submission stays pending until explicitly waited.

namespace
{
template <typename T> struct ScopedVulkanFunction
{
    T& slot;
    T  previous;

    ScopedVulkanFunction(T& slot, T replacement) : slot(slot), previous(slot)
    {
        slot = replacement;
    }

    ~ScopedVulkanFunction()
    {
        slot = previous;
    }
};

struct QueueDriver
{
    static inline uint64_t              submitted{}, completed{};
    static inline VkResult              waitResult{VK_SUCCESS};
    static inline uint64_t              submitCalls{}, failSubmitAt{};
    static inline VkResult              submitResult{VK_SUCCESS};
    static inline VkResult              statusResult{VK_SUCCESS};
    static inline std::vector<uint64_t> waitedSerials, timeouts;

    static VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice, VkPhysicalDeviceProperties* p)
    {
        *p = {};
    }

    static VKAPI_ATTR void VKAPI_CALL GetQueue(VkDevice, uint32_t, uint32_t, VkQueue* queue)
    {
        *queue = reinterpret_cast<VkQueue>(uintptr_t(1));
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice,
                                                          const VkSemaphoreCreateInfo*,
                                                          const VkAllocationCallbacks*,
                                                          VkSemaphore* semaphore)
    {
        *semaphore = (VkSemaphore)uintptr_t(1);

        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}

    static VKAPI_ATTR VkResult VKAPI_CALL Counter(VkDevice, VkSemaphore, uint64_t* value)
    {
        *value = completed;

        return statusResult;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphore(VkDevice, const VkSemaphoreWaitInfo* info, uint64_t timeout)
    {
        waitedSerials.push_back(info->pValues[0]);

        timeouts.push_back(timeout);

        if (waitResult == VK_SUCCESS)
        {
            completed = std::max(completed, info->pValues[0]);
        }

        return waitResult;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue, uint32_t count, const VkSubmitInfo* infos, VkFence)
    {
        EXPECT_EQ(count, 1u);

        VkResult result = VK_SUCCESS;

        if (++submitCalls == failSubmitAt)
        {
            result = submitResult;
        }
        else
        {
            ++submitted;

            const VkTimelineSemaphoreSubmitInfo* timeline = static_cast<const VkTimelineSemaphoreSubmitInfo*>(infos[0].pNext);

            EXPECT_NE(timeline, nullptr);

            if (timeline != nullptr)
            {
                EXPECT_EQ(timeline->pSignalSemaphoreValues[0], submitted);
            }
        }

        return result;
    }
};

class VulkanQueueWaitTest : public testing::Test
{
protected:
    static void CheckTimeout(zen::VulkanQueue& queue)
    {
        EXPECT_FALSE(queue.WaitForCompletion(2, 0));

        EXPECT_FALSE(queue.GetDevice()->GetRHI()->AreSubmissionsBlocked());

        QueueDriver::waitResult = VK_TIMEOUT;

        EXPECT_FALSE(queue.WaitForCompletion(2, 1000000000));

        EXPECT_EQ(queue.GetLastCompletedSerial(), 0u);

        EXPECT_FALSE(queue.GetDevice()->GetRHI()->AreSubmissionsBlocked());

        ASSERT_EQ(QueueDriver::waitedSerials.size(), 1u);

        EXPECT_LE(QueueDriver::timeouts[0], 1000000000u);

        QueueDriver::waitResult = VK_SUCCESS;

        EXPECT_TRUE(queue.WaitForCompletion(2, 1000000000));

        EXPECT_EQ(queue.GetLastCompletedSerial(), 2u);

        ASSERT_EQ(QueueDriver::timeouts.size(), 3u);

        EXPECT_LE(QueueDriver::timeouts[2], QueueDriver::timeouts[1]);

        EXPECT_TRUE(queue.WaitForCompletion(3, UINT64_MAX));
    }

    static void CheckWaitFailure(zen::VulkanQueue& queue)
    {
        EXPECT_TRUE(queue.WaitForCompletion(1, UINT64_MAX));

        QueueDriver::waitResult = VK_ERROR_DEVICE_LOST;

        EXPECT_FALSE(queue.WaitForCompletion(3, UINT64_MAX));

        EXPECT_EQ(queue.GetLastCompletedSerial(), 1u);

        EXPECT_TRUE(queue.GetDevice()->GetRHI()->AreSubmissionsBlocked());

        EXPECT_EQ(QueueDriver::waitedSerials.size(), 2u);

        QueueDriver::waitResult = VK_SUCCESS;

        EXPECT_FALSE(queue.WaitForCompletion(3, UINT64_MAX));

        EXPECT_EQ(QueueDriver::waitedSerials.size(), 2u);

        EXPECT_EQ(queue.GetLastCompletedSerial(), 1u);
    }

    static void CheckQueryFailure(VkResult failure, zen::VulkanQueue& queue)
    {
        EXPECT_TRUE(queue.WaitForCompletion(1, UINT64_MAX));

        QueueDriver::statusResult = failure;

        QueueDriver::completed    = UINT64_MAX;

        // A failed status query must never proceed to a wait that could hide the error.
        EXPECT_FALSE(queue.WaitForCompletion(3, UINT64_MAX));

        EXPECT_EQ(QueueDriver::waitedSerials.size(), 1u);

        EXPECT_EQ(queue.GetLastCompletedSerial(), 1u);

        EXPECT_EQ(queue.GetLastSubmittedSerial(), 3u);

        EXPECT_TRUE(queue.GetDevice()->GetRHI()->AreSubmissionsBlocked());

        QueueDriver::statusResult = VK_SUCCESS;

        queue.ProcessPendingWorkloads(0);

        EXPECT_EQ(queue.GetLastCompletedSerial(), 1u);

        uint64_t serial = UINT64_MAX;

        queue.EnqueueWorkload(ZEN_NEW() zen::VulkanWorkload(&queue));

        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), zen::RHISubmissionResult::eFatal);

        EXPECT_EQ(serial, 0u);

        EXPECT_EQ(QueueDriver::submitCalls, 3u);
    }

    static void CheckQueryFailureDuringSubmission(zen::VulkanQueue& queue)
    {
        QueueDriver::statusResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;

        queue.EnqueueWorkload(ZEN_NEW() zen::VulkanWorkload(&queue));

        uint64_t serial = UINT64_MAX;

        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), zen::RHISubmissionResult::eFatal);

        EXPECT_TRUE(queue.GetDevice()->GetRHI()->AreSubmissionsBlocked());

        EXPECT_EQ(serial, 0u);

        EXPECT_EQ(queue.GetLastCompletedSerial(), 0u);

        EXPECT_EQ(QueueDriver::submitCalls, 3u);
    }

    template <typename Callback> void WithQueue(Callback callback)
    {
        using namespace zen;

        test::VulkanSession session;

        VulkanDevice& device   = *session.rhi.GetDevice();

        QueueDriver::submitted = QueueDriver::completed = 0;

        QueueDriver::waitResult                         = VK_SUCCESS;

        QueueDriver::submitCalls = QueueDriver::failSubmitAt = 0;

        QueueDriver::submitResult                            = VK_SUCCESS;

        QueueDriver::statusResult                            = VK_SUCCESS;

        QueueDriver::waitedSerials.clear();

        QueueDriver::timeouts.clear();

        ScopedVulkanFunction properties(vkGetPhysicalDeviceProperties, &QueueDriver::Properties);

        ScopedVulkanFunction getQueue(vkGetDeviceQueue, &QueueDriver::GetQueue);

        ScopedVulkanFunction createSemaphore(vkCreateSemaphore, &QueueDriver::CreateSemaphore);

        ScopedVulkanFunction destroySemaphore(vkDestroySemaphore, &QueueDriver::DestroySemaphore);

        ScopedVulkanFunction counter(vkGetSemaphoreCounterValue, &QueueDriver::Counter);

        ScopedVulkanFunction waitSemaphore(vkWaitSemaphores, &QueueDriver::WaitSemaphore);

        ScopedVulkanFunction submit(vkQueueSubmit, &QueueDriver::Submit);

        {
            VulkanQueue queue(&device, 0);

            for (uint64_t serial = 1; serial <= 3; ++serial)
            {
                queue.EnqueueWorkload(ZEN_NEW() VulkanWorkload(&queue));

                uint64_t acceptedSerial = 0;

                EXPECT_EQ(queue.SubmitPendingWorkloads(acceptedSerial), RHISubmissionResult::eSuccess);

                EXPECT_EQ(acceptedSerial, serial);
            }

            callback(queue);
        }
    }
};
} // namespace

static_assert(std::is_same_v<decltype(std::declval<zen::VulkanQueue>().GetLastSubmittedSerial()), uint64_t>);
static_assert(std::is_same_v<decltype(std::declval<zen::VulkanQueue>().GetLastCompletedSerial()), uint64_t>);

TEST_F(VulkanQueueWaitTest, StopsAtRequestedSerialAndDoesNotWaitDuringPolling)
{
    WithQueue([](zen::VulkanQueue& queue) {
        EXPECT_TRUE(queue.WaitForCompletion(0, 0));

        EXPECT_FALSE(queue.WaitForCompletion(4, UINT64_MAX));

        EXPECT_TRUE(QueueDriver::waitedSerials.empty());

        EXPECT_FALSE(queue.WaitForCompletion(1, 0));

        EXPECT_TRUE(QueueDriver::waitedSerials.empty());

        EXPECT_TRUE(queue.WaitForCompletion(1, UINT64_MAX));

        EXPECT_EQ(queue.GetLastCompletedSerial(), 1u);

        EXPECT_EQ(queue.GetLastSubmittedSerial(), 3u);

        EXPECT_EQ(QueueDriver::waitedSerials, (std::vector<uint64_t>{1}));

        EXPECT_TRUE(queue.WaitForCompletion(1, 0));

        queue.ProcessPendingWorkloads(0);

        EXPECT_EQ(queue.GetLastCompletedSerial(), 1u);

        EXPECT_EQ(QueueDriver::waitedSerials.size(), 1u);

        EXPECT_TRUE(queue.WaitForCompletion(3, UINT64_MAX));

        EXPECT_EQ(queue.GetLastCompletedSerial(), 3u);

        EXPECT_EQ(QueueDriver::waitedSerials, (std::vector<uint64_t>{1, 2, 3}));
    });
}

TEST_F(VulkanQueueWaitTest, BoundedWaitRetiresOnlyTheRequestedPrefix)
{
    WithQueue([](zen::VulkanQueue& queue) {
        QueueDriver::completed = 3;
        EXPECT_TRUE(queue.WaitForCompletion(1, 0));
        EXPECT_EQ(queue.GetLastCompletedSerial(), 1u);
        EXPECT_TRUE(queue.WaitForCompletion(3, 0));
        EXPECT_EQ(queue.GetLastCompletedSerial(), 3u);
    });
}

TEST_F(VulkanQueueWaitTest, TimeoutDoesNotBlockSubmissionsOrPublishCompletion)
{
    WithQueue(&VulkanQueueWaitTest::CheckTimeout);
}

TEST_F(VulkanQueueWaitTest, WaitFailureBlocksSubmissionsPermanently)
{
    WithQueue(&VulkanQueueWaitTest::CheckWaitFailure);
}


TEST_F(VulkanQueueWaitTest, RejectedSubmissionDoesNotPublishSerialOrCompletionAndCanBeRebuilt)
{
    WithQueue([](zen::VulkanQueue& queue) {
        using namespace zen;

        QueueDriver::failSubmitAt = QueueDriver::submitCalls + 1;

        QueueDriver::submitResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;

        for (int i = 0; i < 3; ++i)
        {
            queue.EnqueueWorkload(ZEN_NEW() VulkanWorkload(&queue));
        }

        uint64_t serial = UINT64_MAX;

        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eRejected);

        EXPECT_EQ(serial, 0u);

        EXPECT_EQ(queue.GetLastSubmittedSerial(), 3u);

        EXPECT_EQ(queue.GetLastCompletedSerial(), 0u);

        EXPECT_FALSE(queue.WaitForCompletion(4, 0));

        queue.DiscardPendingWorkloads();

        queue.DiscardPendingWorkloads();

        const uint64_t calls = QueueDriver::submitCalls;

        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);

        EXPECT_EQ(serial, 0u);

        EXPECT_EQ(QueueDriver::submitCalls, calls);

        for (int i = 0; i < 3; ++i)
        {
            queue.EnqueueWorkload(ZEN_NEW() VulkanWorkload(&queue));
        }

        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);

        EXPECT_EQ(serial, 4u);

        EXPECT_TRUE(queue.WaitForCompletion(serial, UINT64_MAX));

        EXPECT_EQ(queue.GetLastCompletedSerial(), serial);
    });
}

TEST_F(VulkanQueueWaitTest, FailedBatchRetainsOnlyItsAcceptedPrefix)
{
    WithQueue([](zen::VulkanQueue& queue) {
        using namespace zen;

        QueueDriver::failSubmitAt = QueueDriver::submitCalls + 1;

        QueueDriver::submitResult = VK_ERROR_OUT_OF_HOST_MEMORY;

        for (int i = 0; i < 3; ++i)
        {
            queue.EnqueueWorkload(ZEN_NEW() VulkanWorkload(&queue));
        }

        uint64_t serial = 0;

        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eRejected);

        EXPECT_EQ(serial, 0u);

        EXPECT_EQ(queue.GetLastSubmittedSerial(), 3u);

        queue.DiscardPendingWorkloads();

        EXPECT_TRUE(queue.WaitForCompletion(queue.GetLastSubmittedSerial(), UINT64_MAX));

        EXPECT_FALSE(queue.WaitForCompletion(queue.GetLastSubmittedSerial() + 1, 0));
    });
}

TEST_F(VulkanQueueWaitTest, DeviceLossDoesNotInventASubmissionSerial)
{
    WithQueue([](zen::VulkanQueue& queue) {
        using namespace zen;

        QueueDriver::failSubmitAt = QueueDriver::submitCalls + 1;

        QueueDriver::submitResult = VK_ERROR_DEVICE_LOST;

        queue.EnqueueWorkload(ZEN_NEW() VulkanWorkload(&queue));

        uint64_t serial = 0;

        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eFatal);

        EXPECT_EQ(serial, 0u);

        EXPECT_EQ(queue.GetLastSubmittedSerial(), 3u);

        EXPECT_EQ(queue.GetLastCompletedSerial(), 0u);

        queue.DiscardPendingWorkloads(true);

        // Device loss is terminal even when the injection did not lose the real device.
        EXPECT_FALSE(queue.WaitForCompletion(3, UINT64_MAX));

        EXPECT_TRUE(queue.GetDevice()->GetRHI()->AreSubmissionsBlocked());

        EXPECT_EQ(queue.GetLastCompletedSerial(), 0u);
    });
}

TEST_F(VulkanQueueWaitTest, FailedCompletionQueryCannotPublishReturnedGarbage)
{
    for (VkResult failure : {VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY})
    {
        WithQueue(std::bind_front(&VulkanQueueWaitTest::CheckQueryFailure, failure));
    }
}

TEST_F(VulkanQueueWaitTest, CompletionQueryFailurePreventsNativeSubmission)
{
    WithQueue(&VulkanQueueWaitTest::CheckQueryFailureDuringSubmission);
}
