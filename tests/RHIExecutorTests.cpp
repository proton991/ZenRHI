#include "ZenRHI/TestSupport/FakeRHI.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include "Core/RHICommands.h"
using namespace zen;

TEST(RHIQueueCapabilitiesTest, ExecutorCachesNativeSharingWithoutBackendQueriesInEitherMode)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        TestRHI* backend                     = ZEN_NEW() TestRHI();
        backend->submissionQueueCapabilities = {true, true, {0, 1, 1}};
        {
            RHICommandListExecutor executor(backend, mode);
            EXPECT_EQ(backend->queueCapabilityQueries, 1u);
            // Hardware capabilities are immutable for this executor's lifetime.
            backend->submissionQueueCapabilities = {};
            for (uint32_t i = 0; i < 3; ++i)
            {
                const RHIQueueCapabilities capabilities = executor.GetQueueCapabilities();
                EXPECT_TRUE(capabilities.SupportsAsyncCompute());
                EXPECT_TRUE(
                    capabilities.AreQueuesShared(RHICommandContextType::eAsyncCompute, RHICommandContextType::eTransfer));
                EXPECT_FALSE(
                    capabilities.AreQueuesShared(RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute));
                EXPECT_FALSE(capabilities.AreQueuesShared(RHICommandContextType::eMax, RHICommandContextType::eGraphics));
            }
            EXPECT_EQ(backend->queueCapabilityQueries, 1u);
            EXPECT_EQ(backend->contextCreations, 0u);
            EXPECT_EQ(backend->submissionAttempts, 0u);
        }
    }
}

TEST(RHICommandListTest, DetachedStorageReusesAllArenaBlocksWithoutAllocating)
{
    TestRHI backend;

    uint32_t          destructions = 0;
    RHICommandList    commands;
    RHICommandListPtr reusable;
    for (uint8_t frame = 0; frame < 2; ++frame)
    {
        RecordArenaData(commands, frame, destructions);
        reusable = commands.DetachCommands(std::move(reusable));
        reusable->Execute();
        reusable->Reset();
    }
    const size_t allocations = DefaultAllocator::GetTrackedAllocationEvents();
    for (uint8_t frame = 2; frame < 18; ++frame)
    {
        RecordArenaData(commands, frame, destructions);
        reusable = commands.DetachCommands(std::move(reusable));
        EXPECT_EQ(commands.GetCommandCount(), 0u);
        EXPECT_EQ(reusable->GetCommandCount(), 3u);
        reusable->Execute();
        reusable->Reset();
        EXPECT_EQ(destructions, uint32_t(frame + 1) * 3);
    }
    EXPECT_EQ(DefaultAllocator::GetTrackedAllocationEvents(), allocations);
}

TEST_F(RHIExecutorTest, DetachedListsKeepContextAliveUntilFinalReleaseOnRhi)
{
    RHICommandListPtr   producer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    IRHICommandContext* context = producer->GetContext();
    RHICommandListPtr   first   = producer->DetachCommands();
    RHICommandListPtr   second  = producer->DetachCommands();
    EXPECT_EQ(context->GetRefCount(), 3u);
    producer.reset();
    EXPECT_EQ(context->GetRefCount(), 2u);
    executor->GetRuntime().thread.Invoke(&RHICommandList::ResetForReuse, first.get());
    EXPECT_EQ(context->GetRefCount(), 1u);
    EXPECT_EQ(rhi->graphics.proxyDestructions, 0u);
    first.reset();
    second.reset();
    const std::thread::id worker = executor->GetRuntime().thread.Invoke([] { return std::this_thread::get_id(); });
    EXPECT_EQ(rhi->graphics.proxyDestructions, 1u);
    EXPECT_EQ(rhi->graphics.lastProxyDestroyThread, worker);
}

TEST_F(RHIExecutorTest, RecyclesPresentContextsAndReleasesProducerContextOnRhi)
{
    TestViewport      viewport;
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    for (uint32_t frame = 0; frame < 16; ++frame)
    {
        commands->Draw(3, 1, 0, 0);
        const RHIBatchResult result = executor->SubmitFrame(*commands, &viewport).Wait();
        EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
        EXPECT_TRUE(executor->WaitForCompletion(RHICommandContextType::eGraphics,
                                                result.requiredSerials.Get(RHICommandContextType::eGraphics)));
    }
    EXPECT_EQ(viewport.presents, 16u);
    EXPECT_EQ(rhi->contextCreations, 2u);
    EXPECT_EQ(rhi->graphics.proxyDestructions, 0u);
    commands.reset();
    const std::thread::id worker = executor->GetRuntime().thread.Invoke([] { return std::this_thread::get_id(); });
    EXPECT_EQ(rhi->graphics.proxyDestructions, 1u);
    EXPECT_EQ(rhi->graphics.lastProxyDestroyThread, worker);
    viewport.ReleaseForTeardown();
    executor->Destroy();
    EXPECT_EQ(rhi->graphics.proxyDestructions, 2u);
    EXPECT_EQ(rhi->graphics.lastProxyDestroyThread, worker);
}

TEST_F(RHIExecutorTest, DelayedGpuCompletionRetainsListsAndBoundsTheRetiredCache)
{
    TestViewport       viewport;
    RHICommandListPtr  commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    constexpr uint32_t frameCount = RHIFrameState::kMaxFramesInFlight + 3;
    RHIBatchResult     result;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        commands->Draw(3, 1, 0, 0);
        result = executor->SubmitFrame(*commands, &viewport).Wait();
        EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
    }
    RHIBufferCreateInfo bufferInfo{};
    bufferInfo.size   = 64;
    RHIBuffer* buffer = executor->CreateBuffer(bufferInfo);
    ASSERT_NE(buffer, nullptr);
    const uint64_t bufferId = buffer->GetStableId();
    executor->ReleaseWhenComplete(RHIResourcePtr<RHIResource>::Adopt(buffer));
    executor->FlushRHIThread();
    EXPECT_FALSE(destroyed.contains(bufferId));
    commands.reset();
    executor->FlushRHIThread();
    EXPECT_EQ(rhi->contextCreations, frameCount + 1);
    EXPECT_EQ(rhi->graphics.proxyDestructions, 0u);
    EXPECT_TRUE(executor->WaitForCompletion(RHICommandContextType::eGraphics,
                                            result.requiredSerials.Get(RHICommandContextType::eGraphics)));
    EXPECT_EQ(rhi->contextCreations - rhi->graphics.proxyDestructions, RHIFrameState::kMaxFramesInFlight);
    executor->FlushRHIThread();
    EXPECT_TRUE(destroyed.contains(bufferId));
    viewport.ReleaseForTeardown();
    executor->Destroy();
    EXPECT_EQ(rhi->graphics.proxyDestructions, rhi->contextCreations);
}

TEST_F(RHIExecutorTest, DetachedLayoutAndResourcesSurviveCpuAndGpuDelay)
{
    RHITextureCreateInfo info{};
    info.format = DataFormat::eR8G8B8A8UNORM;
    info.type   = RHITextureType::e2D;
    info.width = info.height   = 8;
    RHITexture*        texture = executor->CreateTexture(info);
    const uint64_t     id      = texture->GetStableId();
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 8, 8);
    layout.AddColorRenderTarget(info.format, texture, RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->BeginRendering(&layout);
    commands->EndRendering();
    ArmGate();
    RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(ticket.IsReady());
    EXPECT_EQ(commands->GetCommandCount(), 0u);
    commands->Reset();
    layout.SetRenderArea(0, 0, 1, 1);
    texture->ReleaseReference();
    EXPECT_FALSE(destroyed.contains(id));
    gate.Open();
    const RHIBatchResult result = ticket.Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(ToValue(gate.executionFrameState.GetFrameNumber()), 0u);
    EXPECT_EQ(ToIndex(gate.executionFrameState.GetFrameSlot()), 0u);
    EXPECT_NE(gate.executionThread, std::this_thread::get_id());
    EXPECT_TRUE(gate.releasedInTime);
    ASSERT_EQ(rhi->graphics.renderingLayouts.size(), 1u);
    EXPECT_EQ(rhi->graphics.renderingLayouts[0].renderArea.maxX, 8);
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics), 0u);
    EXPECT_TRUE(executor->WaitForCompletion(RHICommandContextType::eGraphics,
                                            result.requiredSerials.Get(RHICommandContextType::eGraphics)));
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RHIExecutorTest, RejectionStopsAlreadyQueuedDependentBatches)
{
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    rhi->failSubmissionAt = 1;
    ArmGate();
    commands->Draw(3, 1, 0, 0);
    RHISubmissionTicket first = executor->SubmitFrame(*commands, nullptr);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    commands->Draw(3, 1, 0, 0);
    RHISubmissionTicket second = executor->SubmitFrame(*commands, nullptr);
    gate.Open();
    EXPECT_EQ(first.Wait().submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(second.Wait().submission, RHISubmissionResult::eFatal);
    executor->FlushRHIThread();
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(rhi->graphics.drawCount, 0u);
}

namespace
{
struct CancellationFrameProbe
{
    static inline std::string message;
    static void               Capture(const RHIMessage& diagnostic) noexcept
    {
        if (diagnostic.text.starts_with("RHI stopped at frame"))
        {
            message.assign(diagnostic.text);
        }
    }

    void Advance()
    {
        frameState->Advance();
        started.Signal();
        while (!done.load(std::memory_order_relaxed))
        {
            frameState->Advance();
        }
    }
    RHIFrameState*    frameState{nullptr};
    RHIThreadEvent    started;
    std::atomic<bool> done{};
};
} // namespace

TEST_F(RHIExecutorTest, CancellationPrintsOneCoherentFrameWhileWorkerAdvances)
{
    executor->EndFrame();
    executor->FlushRHIThread();
    const uint64_t         publishedFrame = ToValue(executor->GetRuntime().frameState.GetFrameNumber());
    CancellationFrameProbe probe;
    probe.frameState                  = &executor->GetRuntime().frameState;
    const RHIMessageCallback previous = GetRHIMessageCallback();
    CancellationFrameProbe::message.clear();
    SetRHIMessageCallback(CancellationFrameProbe::Capture);
    executor->GetRuntime().thread.Dispatch(std::bind_front(&CancellationFrameProbe::Advance, &probe));
    probe.started.Wait();
    std::thread stopper([this] { executor->GetRuntime().thread.Stop(); });
    while (!executor->GetRuntime().thread.IsStopping())
    {
        std::this_thread::yield();
    }
    const RHIBatchResult result = executor->SubmitGroups({}, {});
    probe.done.store(true);
    stopper.join();
    SetRHIMessageCallback(previous);
    EXPECT_EQ(result.cause.code, RHIErrorCode::eCancelled);
    unsigned long long frame = 0;
    unsigned int       slot  = 0;
    ASSERT_EQ(std::sscanf(CancellationFrameProbe::message.c_str(), "RHI stopped at frame %llu (slot %u)", &frame, &slot), 2);
    EXPECT_EQ(frame, publishedFrame);
    EXPECT_EQ(frame % executor->GetRuntime().frameState.GetNumFramesInFlight(), slot);
}

TEST_F(RHIExecutorTest, RollbackReleasesNewReferencesAndKeepsEarlierCommands)
{
    RHIBufferCreateInfo info{};
    info.size                = 64;
    RHIBuffer*        first  = executor->CreateBuffer(info);
    RHIBuffer*        second = executor->CreateBuffer(info);
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearBuffer(first, 0, 64);
    const RHICommandList::CommandCheckpoint checkpoint = commands->GetCommandCheckpoint();
    commands->CopyBuffer(first, second, RHIBufferCopyRegion{});
    EXPECT_EQ(first->GetRefCount(), 2u);
    EXPECT_EQ(second->GetRefCount(), 2u);
    commands->RollbackCommands(checkpoint);
    EXPECT_EQ(commands->GetCommandCount(), 1u);
    EXPECT_EQ(first->GetRefCount(), 2u);
    EXPECT_EQ(second->GetRefCount(), 1u);
    commands->Reset();
    EXPECT_EQ(first->GetRefCount(), 1u);
    first->ReleaseReference();
    second->ReleaseReference();
}

TEST_F(RHIExecutorTest, FlushSamplesGpuProgressWithoutWaitingForGpuCompletion)
{
    RHITextureCreateInfo info{};
    RHITexture*          texture = executor->CreateTexture(info);
    const uint64_t       id      = texture->GetStableId();
    RHICommandListPtr    commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(texture, Color(0.f), RHITextureSubresourceRange());
    const RHIBatchResult result = executor->SubmitFrame(*commands, nullptr).Wait();
    ASSERT_GT(result.requiredSerials.Get(RHICommandContextType::eGraphics), 0u);
    texture->ReleaseReference();
    executor->FlushRHIThread();
    EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics), 0u);
    EXPECT_FALSE(destroyed.contains(id));
    executor->GetRuntime().thread.Invoke([this] { rhi->completed = rhi->submitted; });
    executor->FlushRHIThread();
    EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics),
              result.requiredSerials.Get(RHICommandContextType::eGraphics));
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_F(RHIExecutorTest, ProgressPollsCoalesceWhileTheWorkerIsBusy)
{
    ArmGate();
    executor->GetRuntime().thread.Dispatch(std::bind_front(&RHISubmissionGate::OnSubmission, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    for (uint32_t poll = 0; poll < 128; ++poll)
    {
        executor->PollGPUProgress();
    }
    // A poll request must leave capacity for real work even when the worker is blocked.
    EXPECT_TRUE(executor->GetRuntime().thread.TryDispatch([] {}));
    gate.Open();
    executor->GetRuntime().thread.Flush();
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_FALSE(executor->AreSubmissionsBlocked());
}

TEST_F(RHIExecutorTest, ProgressPollRetriesAfterQueueSaturationWithoutBlocking)
{
    executor->GetRuntime().thread.Invoke([this] { rhi->submitted[0] = rhi->completed[0] = 1; });
    ArmGate();
    executor->GetRuntime().thread.Dispatch(std::bind_front(&RHISubmissionGate::OnSubmission, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    uint32_t queued = 0;
    while (queued < 128 && executor->GetRuntime().thread.TryDispatch([] {}))
    {
        ++queued;
    }
    EXPECT_GT(queued, 0u);
    EXPECT_LT(queued, 128u);
    executor->PollGPUProgress();
    gate.Open();
    executor->GetRuntime().thread.Flush();
    EXPECT_TRUE(gate.releasedInTime);
    // The failed enqueue must clear the pending flag so an idle caller can retry.
    executor->GetRuntime().thread.Invoke([this] { rhi->submitted[0] = rhi->completed[0] = 2; });
    executor->PollGPUProgress();
    executor->GetRuntime().thread.Flush();
    EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics), 2u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

#if defined(ZEN_WIN32)
TEST(RHIThreadTest, WindowsInvokeServicesSentMessagesAndPreservesPostedMessages)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    const UINT postedMessage = WM_APP + 31;
    ASSERT_TRUE(PostMessageW(window.handle, postedMessage, 71, 0));
    PostQuitMessage(19);
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded);
    thread.Invoke(&RHIMessageWindow::Send, &window);
    thread.Stop();
    EXPECT_EQ(window.deliveredCount, 1u) << "Win32 error " << window.lastError;

    // RHI waits must not consume the application's events or exit request.
    MSG message{};
    EXPECT_TRUE(PeekMessageW(&message, window.handle, postedMessage, postedMessage, PM_REMOVE));
    EXPECT_EQ(message.message, postedMessage);
    EXPECT_EQ(message.wParam, WPARAM(71));
    EXPECT_TRUE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    EXPECT_EQ(message.message, UINT(WM_QUIT));
    EXPECT_EQ(message.wParam, WPARAM(19));
}

TEST(RHIThreadTest, WindowsFullQueueServicesSentMessagesBeforeAcceptingMoreWork)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded, 1);
    RHIGatedWindowMessage gate(thread, window);
    thread.Dispatch(std::bind_front(&RHIGatedWindowMessage::Run, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    HeapVector<uint32_t> order;
    thread.Dispatch([&order] { order.push_back(1); });
    gate.Open();
    // The worker cannot free this full FIFO until its sent window message completes.
    thread.Dispatch([&order] { order.push_back(2); });
    thread.Stop();
    EXPECT_EQ(window.deliveredCount, 1u) << "Win32 error " << window.lastError;
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 2u);
}

TEST(RHIThreadTest, WindowsStopServicesSentMessagesWhileDrainingActiveAndQueuedWork)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded, 1);
    RHIGatedWindowMessage gate(thread, window);
    thread.Dispatch(std::bind_front(&RHIGatedWindowMessage::Run, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    thread.Dispatch(std::bind_front(&RHIMessageWindow::Send, &window));
    gate.Open();
    thread.Stop();
    EXPECT_EQ(window.deliveredCount, 2u) << "Win32 error " << window.lastError;
}

TEST(RHIThreadTest, WindowsLongWaitDoesNotMarkWindowThreadHung)
{
    // Sent-message-only waits let Windows report the window thread as hung, even while
    // it services sent messages; switching back to the app then ghosts its window.
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded);
    const bool hung = thread.Invoke(&IsHungAfterTimeout, window.handle);
    thread.Stop();
    EXPECT_FALSE(hung);
}

TEST(RHIThreadTest, WindowsWaitDoesNotSpinOnQueuedPostedMessages)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    const UINT postedMessage = WM_APP + 32;
    ASSERT_TRUE(PostMessageW(window.handle, postedMessage, 0, 0));
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded);
    const uint64_t before = GetCurrentThreadCPUTime100ns();
    thread.Invoke(&SleepDuringWait);
    const uint64_t spent = GetCurrentThreadCPUTime100ns() - before;
    thread.Stop();
    // The wait leaves posted messages queued; waking for them again would busy-wait.
    EXPECT_LT(spent, 1000000u) << "CPU time in 100 ns units during a 500 ms wait";
    MSG message{};
    EXPECT_TRUE(PeekMessageW(&message, window.handle, postedMessage, postedMessage, PM_REMOVE));
}
#endif

namespace
{
// Counts Destroy calls; it owns no backend object.
class LateReleaseResource : public RHIResource
{
public:
    LateReleaseResource() : RHIResource(RHIResourceType::eSampler) {}

    uint32_t destroyCalls{0};

protected:
    void Destroy() override
    {
        ++destroyCalls;
    }
};

class LateReleaseContext : public TestContext
{
public:
    explicit LateReleaseContext(uint32_t& destructions) :
        TestContext(RHICommandContextType::eGraphics), m_destructions(destructions)
    {}

    ~LateReleaseContext() override
    {
        ++m_destructions;
    }

private:
    uint32_t& m_destructions;
};

// Starts and destroys an executor over a fake backend. The returned runtime has closed cleanup admission.
RHIDeviceRuntimePtr RetireExecutor(RHIExecutionMode mode)
{
    RHICommandListExecutor* executor = ZEN_NEW() RHICommandListExecutor(ZEN_NEW() TestRHI(), mode);
    RHIDeviceRuntimePtr     runtime(&executor->GetRuntime());

    executor->Destroy();

    ZEN_DELETE(executor);

    return runtime;
}

class RHILateReleaseTest : public testing::TestWithParam<RHIExecutionMode>
{
protected:
    void SetUp() override
    {
        m_strict = GetRHIStrictTeardownChecks();

        message.clear();
        SetRHIMessageCallback(Capture);
    }

    void TearDown() override
    {
        SetRHIStrictTeardownChecks(m_strict);

        SetRHIMessageCallback(nullptr);
    }

    bool m_strict{true};

    static void Capture(const RHIMessage& diagnostic) noexcept
    {
        if (diagnostic.severity == RHIMessageSeverity::eError)
        {
            const std::lock_guard<std::mutex> lock(messageMutex);
            message.assign(diagnostic.text);
        }
    }
    static inline std::mutex  messageMutex;
    static inline std::string message;
};

using RHILateReleaseDeathTest = RHILateReleaseTest;
} // namespace

TEST_P(RHILateReleaseTest, NonStrictReleaseAfterExecutorDestructionIsLoggedAndLeaked)
{
    SetRHIStrictTeardownChecks(false);

    LateReleaseResource* late = ZEN_NEW() LateReleaseResource();

    late->BindRuntime(RetireExecutor(GetParam()).Get());

    // Admission rejects the release before Destroy, so nothing reaches the retired backend.
    EXPECT_EQ(late->ReleaseReference(), 0u);

    EXPECT_EQ(late->destroyCalls, 0u);

    EXPECT_NE(message.find("Resource released after RHI cleanup admission closed"), std::string::npos);

    // Another backend has its own open admission, and its unstarted worker releases inline.
    TestRHI              backend;
    LateReleaseResource* current = backend.Bind(ZEN_NEW() LateReleaseResource());

    EXPECT_EQ(current->ReleaseReference(), 0u);

    EXPECT_EQ(current->destroyCalls, 1u);

    ZEN_DELETE(current);

    ZEN_DELETE(late);
}

TEST_P(RHILateReleaseTest, NonStrictContextReleaseIsLoggedWithoutRunningItsDestructor)
{
    SetRHIStrictTeardownChecks(false);

    uint32_t destructions    = 0;

    LateReleaseContext* late = ZEN_NEW() LateReleaseContext(destructions);

    late->AddRef();

    late->BindRuntime(RetireExecutor(GetParam()).Get());

    EXPECT_EQ(late->Release(), 0u);

    EXPECT_EQ(destructions, 0u);

    EXPECT_NE(message.find("Context released after RHI cleanup admission closed"), std::string::npos);

    // This fake context owns no backend state; reclaim the deliberate test leak.
    ZEN_DELETE(late);

    EXPECT_EQ(destructions, 1u);
}

TEST_P(RHILateReleaseTest, FinalizerClosesExternalAdmissionUntilRestart)
{
    RHIThread thread;

    thread.Start(GetParam());

    uint32_t cleanupCalls = 0;

    thread.Stop([&thread, &cleanupCalls] {
        EXPECT_TRUE(thread.IsCurrentThread());

        EXPECT_TRUE(thread.DispatchCleanup([&cleanupCalls] { ++cleanupCalls; }));

        thread.Invoke([&cleanupCalls] { ++cleanupCalls; });

        std::thread external([&thread] { EXPECT_FALSE(thread.DispatchCleanup([] {})); });

        external.join();
    });

    EXPECT_EQ(cleanupCalls, 2u);

    thread.Stop();

    EXPECT_FALSE(thread.IsCurrentThread());

    EXPECT_FALSE(thread.DispatchCleanup([] {}));

    EXPECT_FALSE(thread.TryDispatch([] {}));

    uint32_t cancellations = 0;

    EXPECT_FALSE(
        thread.Dispatch([] { ADD_FAILURE() << "Closed admission executed a task"; }, [&cancellations] { ++cancellations; }));

    EXPECT_EQ(cancellations, 1u);

    thread.Start(GetParam());

    EXPECT_EQ(thread.Invoke([] { return 42; }), 42);

    thread.Stop();

    // Stopping a worker without retiring a backend preserves standalone inline use.
    EXPECT_TRUE(thread.DispatchCleanup([] {}));

    EXPECT_TRUE(thread.Dispatch([] {}));

    EXPECT_EQ(thread.Invoke([] { return std::this_thread::get_id(); }), std::this_thread::get_id());
}

TEST_P(RHILateReleaseDeathTest, StrictReleaseAfterExecutorDestructionStops)
{
    // Only the death-test child creates the executor.
    EXPECT_DEATH(
        {
            SetRHIStrictTeardownChecks(true);

            LateReleaseResource* late = ZEN_NEW() LateReleaseResource();

            late->BindRuntime(RetireExecutor(GetParam()).Get());

            late->ReleaseReference();
        },
        "Resource released after RHI cleanup admission closed");
}

TEST_P(RHILateReleaseDeathTest, StrictContextReleaseAfterExecutorDestructionStops)
{
    EXPECT_DEATH(
        {
            SetRHIStrictTeardownChecks(true);

            uint32_t            destructions = 0;
            LateReleaseContext* late         = ZEN_NEW() LateReleaseContext(destructions);

            late->AddRef();

            late->BindRuntime(RetireExecutor(GetParam()).Get());

            late->Release();
        },
        "Context released after RHI cleanup admission closed");
}

TEST_P(RHILateReleaseDeathTest, SynchronousInvocationAfterExecutorDestructionStops)
{
    EXPECT_DEATH(
        { RetireExecutor(GetParam())->thread.Invoke([] {}); }, "Synchronous RHI invocation after cleanup admission closed");
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         RHILateReleaseTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         RHILateReleaseDeathTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

namespace
{
void CountLateCleanup(const RHIThread& thread, std::atomic<uint32_t>& executed)
{
    EXPECT_TRUE(thread.IsCurrentThread());
    ++executed;
}

void LateCleanupNoop() {}

void AccumulateBenchmarkWork(uint64_t& counter, const std::array<uint64_t, 16>& payload)
{
    counter += payload[0];
}

void ProduceLateCleanup(RHIThread&             thread,
                        std::atomic<bool>&     stop,
                        std::atomic<bool>&     started,
                        std::atomic<uint32_t>& accepted,
                        std::atomic<uint32_t>& executed)
{
    started = true;
    do
    {
        if (thread.DispatchCleanup(std::bind_front(&CountLateCleanup, std::cref(thread), std::ref(executed))))
        {
            ++accepted;
        }
        thread.Invoke(&LateCleanupNoop);
    } while (!stop.load());
}
} // namespace

TEST(RHIThreadTest, StopWithoutFinalizerRunsLateCleanupExactlyOnce)
{
    RHIThread thread;
    for (uint32_t iteration = 0; iteration < 1000; ++iteration)
    {
        std::atomic<bool>     stop{false}, started{false};
        std::atomic<uint32_t> accepted{0}, executed{0};
        thread.Start(RHIExecutionMode::eThreaded);
        std::thread producer(&ProduceLateCleanup, std::ref(thread), std::ref(stop), std::ref(started), std::ref(accepted),
                             std::ref(executed));
        while (!started.load())
        {
            std::this_thread::yield();
        }
        thread.Stop();
        stop = true;
        producer.join();
        EXPECT_EQ(executed.load(), accepted.load());
    }
}

// Explicit opt-in benchmark: timings are recorded for comparison, never asserted.
TEST(RHIThreadTest, DISABLED_QueueThroughputBenchmark)
{
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded, 1024);
    uint64_t                 executed = 0;
    std::array<uint64_t, 16> payload{};
    payload[0]                                      = 1;
    const std::function<void()>                 job = std::bind_front(&AccumulateBenchmarkWork, std::ref(executed), payload);
    const std::chrono::steady_clock::time_point dispatchStart = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 100000; ++i)
    {
        ASSERT_TRUE(thread.Dispatch(job));
    }
    thread.Flush();
    const std::chrono::steady_clock::time_point dispatchEnd = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 10000; ++i)
    {
        thread.Invoke(&LateCleanupNoop);
    }
    const std::chrono::steady_clock::time_point invokeEnd = std::chrono::steady_clock::now();
    thread.Stop();
    EXPECT_EQ(executed, 100000u);
    printf("RVFIX_BENCH dispatch_ms=%.3f invoke_ms=%.3f\n",
           std::chrono::duration<double, std::milli>(dispatchEnd - dispatchStart).count(),
           std::chrono::duration<double, std::milli>(invokeEnd - dispatchEnd).count());
}

TEST_F(RHIExecutorTest, CancelledQueuedBatchCompletesItsTicketAfterTaskFailure)
{
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    RHIThreadEvent    entered;
    RHIThreadEvent    release;

    executor->GetRuntime().thread.Dispatch([&entered, &release] {
        entered.Signal();
        release.Wait();
    });
    entered.Wait();

    executor->GetRuntime().thread.Dispatch([this] {
        executor->GetRuntime().thread.Fail(
            MakeRHIError(RHIErrorCode::eBackendFailure, "injected batch task failure", __FILE__, __LINE__));
    });
    const RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);
    release.Signal();

    ASSERT_TRUE(ticket.IsValid());
    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eFatal);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 0u);
}

TEST_F(RHIExecutorTest, MissingPresentationListDeclinesSynchronousFrameAndAllowsRetry)
{
    TestViewport viewport;

    const SmallVector<RHICommandContextType, 1> queues{RHICommandContextType::eGraphics};

    TestOwnedSchedule rejected(*executor, queues);

    rejected.lists[0]->Draw(3, 1, 0, 0);

    // The executor creates its first presentation list on demand; that creation fails.
    rhi->failContextCreation    = true;

    const RHIBatchResult result = executor->SubmitGroups(rejected.groups, rejected.state, &viewport);

    EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(rhi->submissionAttempts, 0u);

    EXPECT_EQ(viewport.preparePresents, 0u);

    EXPECT_FALSE(executor->AreSubmissionsBlocked());

    EXPECT_TRUE(result.cause.IsFailure());

    EXPECT_EQ(viewport.presents, 0u);

    rhi->failContextCreation = false;

    rejected.lists[0]->Draw(3, 1, 0, 0);

    const RHIBatchResult retry = executor->SubmitGroups(rejected.groups, {}, &viewport);

    EXPECT_EQ(retry.submission, RHISubmissionResult::eSuccess);

    EXPECT_EQ(viewport.presents, 1u);

    // Retained batches reference the stack viewport; release them before it leaves scope.
    rejected.lists.clear();

    viewport.ReleaseForTeardown();

    executor->Destroy();
}

class FailingTranslationRHI : public TestRHI
{
public:
    RHISubmissionResult SubmitCommandLists(std::span<RHICommandList*>) override
    {
        return RHISubmissionResult::eRejected;
    }

    RHIError GetLastSubmissionError() const override
    {
        return {RHIErrorCode::eOutOfDeviceMemory, -2, "injected vkEndCommandBuffer", __FILE__, 73, 19};
    }
};

TEST(RHIThreadProductionTest, FrameTicketPreservesFinalizationCauseAndBlocksNewResources)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHICommandListExecutor executor(ZEN_NEW() FailingTranslationRHI(), mode);

        RHICommandListPtr commands(RHICommandList::Create(executor.GetCommandContext(RHICommandContextType::eGraphics)));

        commands->Draw(3, 1, 0, 0);

        const RHISubmissionTicket ticket = executor.SubmitFrame(*commands, nullptr);

        ASSERT_TRUE(ticket.IsValid());

        const RHIBatchResult& result = ticket.Wait();

        EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);

        EXPECT_EQ(result.cause.nativeCode, -2);

        EXPECT_EQ(result.cause.line, 73u);

        EXPECT_EQ(result.cause.resourceId, 19u);

        EXPECT_STREQ(executor.GetTerminalError().operation, "injected vkEndCommandBuffer");

        EXPECT_EQ(executor.GetTerminalError().nativeCode, -2);

        EXPECT_EQ(executor.WaitForCompletion(RHICommandContextType::eGraphics, 1, 0).outcome, RHIWaitOutcome::eFailed);

        EXPECT_EQ(executor.CreateBuffer({}), nullptr);

        commands.reset();

        executor.Destroy();
    }
}

TEST(RHIThreadProductionTest, FailedWorkerCompletesQueuedFrameTiming)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHICommandListExecutor executor(ZEN_NEW() TestRHI(), mode);
        RHIGPUFrameTimingPtr   timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        executor.GetRuntime().thread.Dispatch([&executor] {
            executor.GetRuntime().thread.Fail(
                MakeRHIError(RHIErrorCode::eBackendFailure, "injected timing task failure", __FILE__, __LINE__));
        });
        executor.BeginGPUFrameTiming(timing);
        executor.EndGPUFrameTiming(timing, true);
        executor.FlushRHIThread();

        EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eError);
        EXPECT_TRUE(executor.AreSubmissionsBlocked());
    }
}

TEST(RHISubmissionPointTest, ExactGroupsRetainIndependentLogicalTimelines)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute, RHICommandContextType::eTransfer};
    RefCountPtr<RHISubmissionState>             state = MakeRefCountPtr<RHISubmissionState>(queues);
    RHISubmissionPoint                          compute{queues[0], 0, state, 0};
    RHISubmissionPoint                          transfer{queues[1], 0, state, 1};
    RHISubmissionDependency                     resolved;
    EXPECT_EQ(compute.Resolve(resolved), RHISubmissionPointStatus::ePending);
    EXPECT_FALSE(RHISubmissionStateTestAccess::Accept(*state, 0, {queues[0], 91}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Queue(*state));
    EXPECT_FALSE(RHISubmissionStateTestAccess::Queue(*state));
    EXPECT_FALSE(RHISubmissionStateTestAccess::Accept(*state, 0, {queues[1], 91}));
    EXPECT_FALSE(RHISubmissionStateTestAccess::Accept(*state, 0, {queues[0], RHISubmissionDependency::kLatestSubmitted}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*state, 0, {queues[0], 91}));
    EXPECT_FALSE(RHISubmissionStateTestAccess::FinishSubmission(*state));
    EXPECT_EQ(transfer.Resolve(resolved), RHISubmissionPointStatus::ePending);
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*state, 1, {queues[1], 4}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::FinishSubmission(*state));
    state.Reset();
    ExpectPoint(compute, queues[0], 91);
    ExpectPoint(transfer, queues[1], 4);
    EXPECT_FALSE((RHISubmissionPoint{queues[0], RHISubmissionDependency::kLatestSubmitted}).IsValid());
}

TEST_F(RHIExecutorTest, QueuedConsumerResolvesExactProducerBeforeUnrelatedSubmissions)
{
    rhi->submissionQueueCapabilities.asyncSubmissionDependencies = true;
    const RHICommandContextType     producerQueue                = RHICommandContextType::eAsyncCompute;
    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(producerQueue));
    RHICommandListPtr               producer(RHICommandList::Create(executor->GetCommandContext(producerQueue)));
    RHICommandListPtr consumer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    ArmGate();
    producer->Dispatch(1, 1, 1);
    const RHISubmissionTicket first = executor->SubmitFrame(*producer, nullptr, state);
    ASSERT_TRUE(first.IsValid());
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    RHISubmissionPoint      point{producerQueue, 0, state, 0};
    RHISubmissionDependency resolved;
    EXPECT_EQ(point.Resolve(resolved), RHISubmissionPointStatus::ePending);
    // Queue another compute submission before the consumer; it must not change the point.
    producer->Dispatch(1, 1, 1);
    const RHISubmissionTicket unrelated = executor->SubmitFrame(*producer, nullptr);
    consumer->Dispatch(1, 1, 1);
    const RHISubmissionTicket last = executor->SubmitFrame(*consumer, nullptr, {}, MakeVecView(point));
    state.Reset();
    gate.Open();
    ASSERT_EQ(first.Wait().submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(unrelated.Wait().submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(last.Wait().submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(rhi->submitted[1], 2u);
    ExpectPoint(point, producerQueue, 1);
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, 1u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
}

TEST_F(RHIExecutorTest, FailedExactProducerStopsQueuedConsumerBeforeNativeExecution)
{
    const RHICommandContextType     queue = RHICommandContextType::eAsyncCompute;
    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queue));
    RHICommandListPtr               producer(RHICommandList::Create(executor->GetCommandContext(queue)));
    RHICommandListPtr consumer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    rhi->failSubmissionAt = 1;
    ArmGate();
    producer->Dispatch(1, 1, 1);
    const RHISubmissionTicket first = executor->SubmitFrame(*producer, nullptr, state);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const RHISubmissionPoint point{queue, 0, state, 0};
    consumer->Draw(3, 1, 0, 0);
    const RHISubmissionTicket second = executor->SubmitFrame(*consumer, nullptr, {}, MakeVecView(point));
    ASSERT_TRUE(second.IsValid());
    gate.Open();
    EXPECT_EQ(first.Wait().submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(second.Wait().submission, RHISubmissionResult::eFatal);
    RHISubmissionDependency resolved;
    EXPECT_EQ(point.Resolve(resolved), RHISubmissionPointStatus::eFailed);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(rhi->graphics.drawCount, 0u);
}

TEST(RHICompletionSetTest, ComparesOnlyCorrespondingQueueTimelines)
{
    RHICompletionSet required{{100, 5, 9}};
    required.Extend(RHICommandContextType::eAsyncCompute, 3);
    required.Extend(RHICompletionSet{{99, 7, 8}});
    EXPECT_EQ(required.Get(RHICommandContextType::eGraphics), 100u);
    EXPECT_EQ(required.Get(RHICommandContextType::eAsyncCompute), 7u);
    EXPECT_FALSE(required.IsCompleteAt(RHICompletionSet{{1000, 6, 1000}}));
    EXPECT_TRUE(required.IsCompleteAt(RHICompletionSet{{100, 7, 9}}));
    required.Reset();
    EXPECT_TRUE(required.IsCompleteAt({}));
}

#include "RHIGPUFrameExecutorTests.inl"

#include "RHIScheduledSubmissionTests.inl"

TEST(RHIPresentModeTest, ParsesConfigurationNames)
{
    RHIPresentMode mode = RHIPresentMode::eDefault;
    EXPECT_TRUE(ParseRHIPresentMode("fifo_relaxed", mode));
    EXPECT_EQ(mode, RHIPresentMode::eFifoRelaxed);
    EXPECT_TRUE(ParseRHIPresentMode("immediate", mode));
    EXPECT_EQ(mode, RHIPresentMode::eImmediate);
    EXPECT_TRUE(ParseRHIPresentMode("default", mode));
    EXPECT_EQ(mode, RHIPresentMode::eDefault);
    EXPECT_FALSE(ParseRHIPresentMode("vsync", mode));
    EXPECT_EQ(mode, RHIPresentMode::eDefault);
}

TEST_F(RHIExecutorTest, ParameterSnapshotsPreserveOffsetsArrayMetadataAndBindlessResources)
{
    RHITextureCreateInfo textureInfo{};
    textureInfo.format = DataFormat::eR8G8B8A8UNORM;
    textureInfo.type   = RHITextureType::e2D;
    textureInfo.width = textureInfo.height = 8;
    RHITexture*         texture            = executor->CreateTexture(textureInfo);
    RHIBufferCreateInfo bufferInfo{};
    bufferInfo.size     = 64;
    RHIBuffer*  buffer  = executor->CreateBuffer(bufferInfo);
    RHISampler* sampler = executor->CreateSampler({});
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(sampler, nullptr);

    RHIBatchedShaderParameters source;

    RHIShaderResourceDescriptor value{};

    value.set     = 2;

    value.binding = 5;

    source.AddValueParam(value, std::array<uint32_t, 2>{7, 11});

    value.set     = 4;

    value.binding = 9;

    source.AddValueParam(value, uint8_t(0xEF));

    RHIShaderResourceDescriptor resource{};

    resource.set     = 3;

    resource.binding = 6;

    resource.type    = RHIShaderResourceType::eSamplerWithTexture;

    source.AddResourceParam(resource, texture->GetDefaultView(), sampler, 17);

    resource.bindless = true;

    resource.type     = RHIShaderResourceType::eStorageBuffer;

    source.AddResourceParam(resource, buffer, nullptr, 23);

    RHICommandList storage;

    RHICommandSetShaderParameters command(storage, source.GetView());

    source.Reset();

    source.AddValueParam(value, uint64_t(0)); // Reuse and overwrite the original byte storage.

    const RHIShaderParameterView& snapshot          = command.parameters;

    std::span<const RHIShaderValueParameter> values = snapshot.GetValueParams();

    ASSERT_EQ(values.size(), 2u);

    EXPECT_EQ(values[0].set, 2u);

    EXPECT_EQ(values[0].binding, 5u);

    EXPECT_EQ(values[0].byteOffset, 0u);

    EXPECT_EQ(values[1].set, 4u);

    EXPECT_EQ(values[1].binding, 9u);

    EXPECT_EQ(values[1].byteOffset, 8u);

    std::array<uint32_t, 2> result;

    std::span<const uint8_t> bytes = snapshot.GetValueBytes(values[0]);

    ASSERT_EQ(bytes.size(), sizeof(result));

    std::memcpy(result.data(), bytes.data(), bytes.size());

    EXPECT_EQ(result, (std::array<uint32_t, 2>{7, 11}));

    ASSERT_EQ(snapshot.GetValueBytes(values[1]).size(), 1u);

    EXPECT_EQ(snapshot.GetValueBytes(values[1])[0], 0xEF);

    std::span<const RHIShaderResourceParameter> resources = snapshot.GetResourceParams();

    ASSERT_EQ(resources.size(), 1u);

    EXPECT_EQ(resources[0].set, 3u);

    EXPECT_EQ(resources[0].binding, 6u);

    EXPECT_EQ(resources[0].arrayIndex, 17u);

    EXPECT_EQ(resources[0].resourceType, RHIShaderResourceType::eSamplerWithTexture);

    EXPECT_EQ(resources[0].pResource, texture->GetDefaultView());

    EXPECT_EQ(resources[0].pAuxResource, sampler);

    std::span<const RHIShaderResourceParameter> bindless = snapshot.GetBindlessParams();

    ASSERT_EQ(bindless.size(), 1u);

    EXPECT_EQ(bindless[0].set, 3u);

    EXPECT_EQ(bindless[0].binding, 6u);

    EXPECT_EQ(bindless[0].arrayIndex, 23u);

    EXPECT_EQ(bindless[0].resourceType, RHIShaderResourceType::eStorageBuffer);

    EXPECT_EQ(bindless[0].pResource, buffer);

    EXPECT_EQ(bindless[0].pAuxResource, nullptr);

    source.Reset();

    storage.Reset();

    executor->DestroyTexture(texture);

    executor->DestroyBuffer(buffer);

    executor->DestroySampler(sampler);
}
