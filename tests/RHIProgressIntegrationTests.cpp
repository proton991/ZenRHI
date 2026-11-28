#include "ZenRHI/RHICommandListExecutor.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRHI.h"
#include "VulkanIntegrationFixture.h"
#include <gtest/gtest.h>

namespace
{
using namespace zen;

class RHIProgressIntegrationTest : public testing::TestWithParam<RHIExecutionMode>
{
protected:
    static VKAPI_ATTR VkResult VKAPI_CALL Counter(VkDevice, VkSemaphore, uint64_t* value)
    {
        *value = counterResult == VK_SUCCESS ? 0 : UINT64_MAX;

        return counterResult;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice, const VkSemaphoreWaitInfo*, uint64_t)
    {
        return VK_ERROR_DEVICE_LOST;
    }

    void SetUp() override
    {
        backend  = static_cast<VulkanRHI*>(CreateRHI(test::MakeTestDeviceDesc()));

        executor = ZEN_NEW() RHICommandListExecutor(backend, GetParam());

        RHIBufferCreateInfo info{};

        info.allocateType = RHIBufferAllocateType::eGPU;

        info.size         = 64;

        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferDstBuffer);

        buffer = executor->CreateBuffer(info);

        commands.reset(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

        commands->ClearBuffer(buffer, 0, 64);

        originalCounter = vkGetSemaphoreCounterValue;

        originalWait    = vkWaitSemaphores;

        executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::InstallCounter);
    }

    void TearDown() override
    {
        if (executor != nullptr)
        {
            executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::RestoreFunctions, this);

            executor->WaitDeviceIdle();

            commands.reset();

            buffer->ReleaseReference();

            executor->Destroy();

            ZEN_DELETE(executor);
        }
        else if (backend != nullptr)
        {
            backend->Destroy();

            ZEN_DELETE(backend);
        }
    }

    static void InstallCounter()
    {
        counterResult              = VK_SUCCESS;

        vkGetSemaphoreCounterValue = &Counter;
    }

    static void FailCounter()
    {
        counterResult = VK_ERROR_DEVICE_LOST;
    }

    static void FailWait()
    {
        vkWaitSemaphores = &Wait;
    }

    void RestoreFunctions()
    {
        vkGetSemaphoreCounterValue = originalCounter;

        vkWaitSemaphores           = originalWait;
    }

    RHIBatchResult SubmitPendingFrame()
    {
        const RHIBatchResult result = executor->SubmitFrame(*commands, nullptr).Wait();

        executor->GetRuntime().thread.Flush();

        EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);

        EXPECT_GT(result.requiredSerials.Get(RHICommandContextType::eGraphics), 0u);

        EXPECT_FALSE(executor->AreSubmissionsBlocked());

        return result;
    }

    void CheckBlocked(uint64_t submittedSerial)
    {
        EXPECT_TRUE(executor->GetRuntime().thread.Invoke(&VulkanRHI::AreSubmissionsBlocked, backend));

        EXPECT_TRUE(executor->AreSubmissionsBlocked());

        EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics), 0u);

        EXPECT_FALSE(executor->SubmitFrame(*commands, nullptr).IsValid());

        EXPECT_EQ(executor->FlushAllGPUCommands(), RHISubmissionResult::eFatal);

        EXPECT_EQ(executor->GetLastSubmittedSerial(RHICommandContextType::eGraphics), submittedSerial);
    }

    static inline VkResult         counterResult{VK_SUCCESS};
    VulkanRHI*                     backend{nullptr};
    RHICommandListExecutor*        executor{nullptr};
    RHIBuffer*                     buffer{nullptr};
    RHICommandListPtr              commands;
    PFN_vkGetSemaphoreCounterValue originalCounter{nullptr};
    PFN_vkWaitSemaphores           originalWait{nullptr};
};
} // namespace

TEST_P(RHIProgressIntegrationTest, CounterFailureDuringPollingBlocksSubmissions)
{
    const RHIBatchResult result = SubmitPendingFrame();

    executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::FailCounter);

    if (GetParam() == RHIExecutionMode::eInline)
    {
        EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics), 0u);
    }
    else
    {
        executor->PollGPUProgress();
    }

    executor->GetRuntime().thread.Flush();

    CheckBlocked(result.requiredSerials.Get(RHICommandContextType::eGraphics));
}

TEST_P(RHIProgressIntegrationTest, CounterFailureDuringFlushBlocksSubmissions)
{
    const RHIBatchResult result = SubmitPendingFrame();

    executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::FailCounter);

    executor->FlushRHIThread();

    CheckBlocked(result.requiredSerials.Get(RHICommandContextType::eGraphics));
}

TEST_P(RHIProgressIntegrationTest, WaitFailureBlocksSubmissions)
{
    const RHIBatchResult result = SubmitPendingFrame();

    executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::FailWait);

    EXPECT_FALSE(executor->WaitForCompletion(RHICommandContextType::eGraphics,
                                             result.requiredSerials.Get(RHICommandContextType::eGraphics), UINT64_MAX));

    CheckBlocked(result.requiredSerials.Get(RHICommandContextType::eGraphics));
}

TEST_P(RHIProgressIntegrationTest, CounterFailureDuringRetirementBlocksSubmissions)
{
    const RHIBatchResult result = SubmitPendingFrame();

    executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::FailCounter);

    executor->CollectRetiredBindlessResources();

    CheckBlocked(result.requiredSerials.Get(RHICommandContextType::eGraphics));
}

TEST_P(RHIProgressIntegrationTest, CounterFailureDuringSubmissionFailsFrameTicket)
{
    const RHIBatchResult first = SubmitPendingFrame();

    executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::FailCounter);

    commands->ClearBuffer(buffer, 0, 64);

    const RHIBatchResult failed = executor->SubmitFrame(*commands, nullptr).Wait();

    EXPECT_EQ(failed.submission, RHISubmissionResult::eFatal);

    CheckBlocked(first.requiredSerials.Get(RHICommandContextType::eGraphics));
}

TEST_P(RHIProgressIntegrationTest, CounterFailureAfterSubmissionFailsFrameTicket)
{
    executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::FailCounter);

    const RHICommandContextType queue     = RHICommandContextType::eGraphics;

    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queue));

    const RHIBatchResult failed           = executor->SubmitFrame(*commands, nullptr, state).Wait();

    EXPECT_EQ(failed.submission, RHISubmissionResult::eFatal);

    EXPECT_GT(failed.requiredSerials.Get(RHICommandContextType::eGraphics), 0u);

    const RHISubmissionPoint point{queue, 0, state, 0};

    RHISubmissionDependency accepted;

    EXPECT_EQ(point.Resolve(accepted), RHISubmissionPointStatus::eFailed);

    EXPECT_FALSE(state->IsSubmissionFinished());

    CheckBlocked(failed.requiredSerials.Get(RHICommandContextType::eGraphics));
}

TEST_P(RHIProgressIntegrationTest, ComputeProgressFailureRetainsSerialAndStopsDependentGroup)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics};

    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(queues);

    RHICommandListPtr compute(RHICommandList::Create(executor->GetCommandContext(queues[0])));

    RHIBufferCreateInfo info{};

    info.allocateType = RHIBufferAllocateType::eGPU;

    info.size         = 64;

    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    RHIResourcePtr<RHIBuffer> output(executor->CreateBuffer(info), false);

    compute->ClearBuffer(output.Get(), 0, 64);

    SmallVector<RHISubmissionGroup, 2> groups(2);

    groups[0].commands = compute.get();

    groups[1].commands = commands.get();

    groups[1].predecessors.push_back({queues[0], 0, state, 0});

    executor->GetRuntime().thread.Invoke(&RHIProgressIntegrationTest::FailCounter);

    const RHIBatchResult failed = executor->SubmitFrame(groups, nullptr, state).Wait();

    EXPECT_EQ(failed.submission, RHISubmissionResult::eFatal);

    EXPECT_GT(failed.requiredSerials.Get(queues[0]), 0u);

    ASSERT_EQ(failed.groups.size(), 2u);

    EXPECT_EQ(failed.groups[0].submission, RHISubmissionResult::eFatal);

    EXPECT_EQ(failed.groups[1].submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(executor->GetLastSubmittedSerial(queues[1]), 0u);

    EXPECT_TRUE(executor->AreSubmissionsBlocked());

    RHISubmissionDependency accepted;

    EXPECT_EQ(state->Resolve(0, accepted), RHISubmissionPointStatus::eFailed);

    EXPECT_FALSE(executor->SubmitFrame(groups, nullptr).IsValid());
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RHIProgressIntegrationTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
