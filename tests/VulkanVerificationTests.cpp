#include "Vulkan/VulkanCommon.h"
#include <gtest/gtest.h>

#include "Vulkan/VulkanRHI.h"
#include <barrier>
#include <thread>

namespace
{
using namespace zen;

class TerminalPublicationProbe
{
public:
    void Observe()
    {
        for (uint32_t index = 0; index < rounds; ++index)
        {
            gate.arrive_and_wait();
            do
            {
                const RHIError error = backend->GetTerminalError();
                if ((error.IsFailure() && error.resourceId != expected.resourceId)
                    || (backend->HasDeviceLoss() && !backend->AreSubmissionsBlocked()))
                {
                    inconsistent = true;
                }
            } while (!finished.load(std::memory_order_acquire));
            gate.arrive_and_wait();
        }
    }

    static constexpr uint32_t rounds = 2048;
    RHIError                  expected{RHIErrorCode::eDeviceLost, -4, "submit", __FILE__, __LINE__, 1234567};
    std::barrier<>            gate{2};
    std::atomic<bool>         finished{};
    VulkanRHI*                backend{};
    bool                      inconsistent{};
};
} // namespace

TEST(VulkanVerificationTest, TerminalFailureIsPublishedAsACompleteRecord)
{
    TerminalPublicationProbe probe;
    std::thread              observer(&TerminalPublicationProbe::Observe, &probe);
    for (uint32_t index = 0; index < probe.rounds; ++index)
    {
        zen::VulkanRHI backend{RHIDeviceDesc{}};
        probe.backend = &backend;
        probe.finished.store(false, std::memory_order_relaxed);
        probe.gate.arrive_and_wait();
        backend.BlockSubmissions(probe.expected);
        probe.finished.store(true, std::memory_order_release);
        probe.gate.arrive_and_wait();
    }
    observer.join();
    EXPECT_FALSE(probe.inconsistent);
}

TEST(VulkanVerificationTest, SuccessAndExpectedStatusesRemainNonfatal)
{
    for (VkResult result : {VK_SUCCESS, VK_NOT_READY, VK_TIMEOUT, VK_SUBOPTIMAL_KHR})
    {
        EXPECT_FALSE(zen::MakeVulkanError(result, "expected status", __FILE__, __LINE__).IsFailure());
    }
}

TEST(VulkanVerificationTest, NativeFailurePreservesCauseAndLocation)
{
    const zen::RHIError error = zen::MakeVulkanError(VK_ERROR_OUT_OF_DEVICE_MEMORY, "test allocation", __FILE__, 42);

    EXPECT_EQ(error.code, zen::RHIErrorCode::eOutOfDeviceMemory);

    EXPECT_EQ(error.nativeCode, VK_ERROR_OUT_OF_DEVICE_MEMORY);

    EXPECT_STREQ(error.operation, "test allocation");

    EXPECT_STREQ(error.source, __FILE__);

    EXPECT_EQ(error.line, 42u);
}

TEST(VulkanVerificationTest, DeviceLossHasADistinctCode)
{
    EXPECT_EQ(zen::MakeVulkanError(VK_ERROR_DEVICE_LOST, "test device loss").code, zen::RHIErrorCode::eDeviceLost);
}
