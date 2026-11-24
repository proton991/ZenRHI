namespace
{
struct TestQueueFlushOrder
{
    explicit TestQueueFlushOrder(TestRHI& backend) : backend(backend) {}

    void BeforeSubmission()
    {
        // Vulkan drains its distinct logical wrappers in queue-enum order, including
        // wrappers that share a native queue. Reproduce that behavior at the fake boundary.
        std::stable_sort(backend.pendingCommandLists.begin(), backend.pendingCommandLists.end(),
                         [](const RHICommandList* first, const RHICommandList* second) {
                             return first->GetContext()->GetContextType() < second->GetContext()->GetContextType();
                         });

        for (const RHICommandList* list : backend.pendingCommandLists)
        {
            queues.push_back(list->GetContext()->GetContextType());
        }
    }

    TestRHI&                          backend;
    HeapVector<RHICommandContextType> queues;
};

class RejectedPresentViewport : public TestViewport
{
public:
    RHIStatus PrepareForPresentChecked(RHICommandList* commands) override
    {
        return rejectPreparation ? RHIStatus{error} : TestViewport::PrepareForPresentChecked(commands);
    }

    RHIPresentResult PresentChecked() override
    {
        return {RHISurfaceOutcome::eFailed, RHIPresentAcceptance::eRejected, error, false};
    }

    bool     rejectPreparation{};
    RHIError error = MakeRHIError(RHIErrorCode::eOutOfDeviceMemory, "test presentation", __FILE__, __LINE__);
};

class RHIScheduledSubmissionTest : public testing::TestWithParam<std::tuple<RHIExecutionMode, bool>>
{
protected:
    void SetUp() override
    {
        destroyed.clear();
        GRHIFrameState.Init(3);
        rhi                                                          = ZEN_NEW() TestRHI();
        rhi->submissionQueueCapabilities.asyncSubmissionDependencies = true;
        rhi->submissionQueueCapabilities                             = DistinctComputeQueues();
        rhi->submissionQueueCapabilities.queueIds[2]                 = std::get<1>(GetParam()) ? 1 : 2;
        executor    = ZEN_NEW() RHICommandListExecutor(rhi, std::get<0>(GetParam()));
        GDynamicRHI = executor;
    }

    void TearDown() override
    {
        if (rejectedSurface)
        {
            rejectedSurface->ReleaseForTeardown();
        }
        viewport.ReleaseForTeardown();
        executor->Destroy();
        ZEN_DELETE(executor);
        GDynamicRHI = nullptr;
    }

    RHIResourcePtr<RHIBuffer> Buffer()
    {
        RHIBufferCreateInfo info;
        info.size = 64;
        return RHIResourcePtr<RHIBuffer>(executor->CreateBuffer(info), false);
    }

    void VerifyQueueSubmissionOrder(std::span<const RHICommandContextType> queues, uint32_t expectedSubmissions)
    {
        TestOwnedSchedule   schedule(*executor, queues);
        TestQueueFlushOrder flushOrder(*rhi);
        rhi->beforeSubmission = std::bind_front(&TestQueueFlushOrder::BeforeSubmission, &flushOrder);

        HeapVector<RHIResourcePtr<RHIBuffer>> buffers;
        buffers.push_back(Buffer());
        TestBuffer* input = static_cast<TestBuffer*>(buffers[0].Get());
        for (size_t i = 0; i < input->bytes.size(); ++i)
        {
            input->bytes[i] = uint8_t(i + 7);
        }
        for (uint32_t i = 0; i < queues.size(); ++i)
        {
            buffers.push_back(Buffer());
            schedule.lists[i]->CopyBuffer(buffers[i].Get(), buffers[i + 1].Get(), {0, 0, 64});
            if (i != 0)
            {
                schedule.Depends(i, i - 1);
            }
        }

        const RHIBatchResult result = executor->SubmitGroups(schedule.groups, schedule.state);
        rhi->beforeSubmission       = {};

        ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);
        ASSERT_EQ(result.groups.size(), queues.size());
        ASSERT_EQ(flushOrder.queues.size(), queues.size());
        std::array<uint64_t, RHICompletionSet::kQueueCount> serials{};
        for (uint32_t i = 0; i < queues.size(); ++i)
        {
            const uint64_t serial = ++serials[ToUnderlying(queues[i])];
            EXPECT_EQ(flushOrder.queues[i], queues[i]);
            EXPECT_EQ(result.groups[i].accepted.queue, queues[i]);
            EXPECT_EQ(result.groups[i].accepted.serial, serial);
            EXPECT_EQ(static_cast<TestBuffer*>(buffers[i + 1].Get())->bytes, input->bytes);

            RHISubmissionDependency accepted;
            EXPECT_EQ(schedule.state->Resolve(i, accepted), RHISubmissionPointStatus::eAccepted);
            EXPECT_EQ(accepted.queue, queues[i]);
            EXPECT_EQ(accepted.serial, serial);
        }
        EXPECT_EQ(rhi->submissionAttempts, expectedSubmissions);
        EXPECT_EQ(rhi->gpuDependencies.size(), std::get<1>(GetParam()) ? 0u : expectedSubmissions - 1);
        EXPECT_EQ(result.requiredSerials.Get(RHICommandContextType::eAsyncCompute), serials[1]);
        EXPECT_EQ(result.requiredSerials.Get(RHICommandContextType::eTransfer), serials[2]);
        EXPECT_TRUE(rhi->submissionWaits.empty());
    }

    TestRHI*                rhi{nullptr};
    RHICommandListExecutor* executor{nullptr};
    TestViewport            viewport;
    std::optional<RejectedPresentViewport> rejectedSurface;
};

TEST_P(RHIScheduledSubmissionTest, ReadyGroupsSubmitInOrderAndRetainExactLogicalSerials)
{
    const SmallVector<RHICommandContextType, 4> queues{RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute,
                                                       RHICommandContextType::eGraphics, RHICommandContextType::eTransfer};
    TestOwnedSchedule                           schedule(*executor, queues);
    RHIResourcePtr<RHIBuffer>                   prefix = Buffer(), source = Buffer(), produced = Buffer(), graphics = Buffer(),
                                                transfer = Buffer();
    TestBuffer*                                 input    = static_cast<TestBuffer*>(source.Get());
    for (size_t i = 0; i < input->bytes.size(); ++i)
    {
        input->bytes[i] = uint8_t(i + 7);
    }
    schedule.lists[0]->ClearBuffer(prefix.Get(), 0, 64);
    schedule.lists[1]->CopyBuffer(source.Get(), produced.Get(), {0, 0, 64});
    schedule.lists[2]->CopyBuffer(produced.Get(), graphics.Get(), {0, 0, 64});
    schedule.lists[3]->CopyBuffer(produced.Get(), transfer.Get(), {0, 0, 64});
    schedule.Depends(2, 1);
    schedule.Depends(3, 1);
    const RHIBatchResult result = executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(result.groups.size(), 4u);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.groups[1].accepted.serial, 1u);
    EXPECT_EQ(result.groups[2].accepted.serial, 2u);
    EXPECT_EQ(result.groups[3].accepted.serial, 1u);
    EXPECT_EQ(result.groups[3].accepted.queue, RHICommandContextType::eTransfer);
    EXPECT_EQ(ToValue(GetRHIThread().Invoke(&RHIFrameState::GetFrameNumber, &GRHIFrameState)), 1u);
    EXPECT_EQ(viewport.presents, 1u);
    ASSERT_EQ(rhi->gpuDependencies.size(), std::get<1>(GetParam()) ? 1u : 2u);
    EXPECT_EQ(rhi->gpuDependencies[0].consumer, RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, 1u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    for (const RHICommandListPtr& list : schedule.lists)
    {
        EXPECT_EQ(list->GetCommandCount(), 0u);
    }
    EXPECT_EQ(static_cast<TestBuffer*>(graphics.Get())->bytes, input->bytes);
    EXPECT_EQ(static_cast<TestBuffer*>(transfer.Get())->bytes, input->bytes);
}

TEST_P(RHIScheduledSubmissionTest, SameQueueRunsKeepEveryGroupSerialAndEarlierProducerWait)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eGraphics, RHICommandContextType::eGraphics,
                                            RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics,
                                            RHICommandContextType::eGraphics};

    TestOwnedSchedule schedule(*executor, MakeVecView(queues, 5));

    for (RHICommandListPtr& list : schedule.lists)
    {
        list->Dispatch(1, 1, 1);
    }

    schedule.Depends(1, 0);

    schedule.Depends(2, 0);

    schedule.Depends(4, 2);

    schedule.Depends(4, 3);

    const RHIBatchResult result = executor->SubmitGroups(schedule.groups, schedule.state);

    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);

    ASSERT_EQ(result.groups.size(), 5u);

    const uint64_t serials[] = {1, 2, 1, 3, 4};

    for (uint32_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(result.groups[i].accepted.queue, queues[i]);

        EXPECT_EQ(result.groups[i].accepted.serial, serials[i]);

        RHISubmissionDependency accepted;

        EXPECT_EQ(schedule.state->Resolve(i, accepted), RHISubmissionPointStatus::eAccepted);

        EXPECT_EQ(accepted.serial, serials[i]);
    }

    EXPECT_EQ(rhi->submissionAttempts, 3u);

    ASSERT_EQ(rhi->gpuDependencies.size(), 2u);

    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, 1u);

    EXPECT_EQ(rhi->gpuDependencies[0].producer.queue, RHICommandContextType::eGraphics);

    EXPECT_EQ(rhi->gpuDependencies[1].producer.queue, RHICommandContextType::eAsyncCompute);
}

TEST_P(RHIScheduledSubmissionTest, AliasedNativeQueueRunsRetainDistinctLogicalTimelines)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eAsyncCompute, RHICommandContextType::eTransfer,
                                            RHICommandContextType::eTransfer, RHICommandContextType::eAsyncCompute};

    VerifyQueueSubmissionOrder(MakeVecView(queues, 4), 3);
}

TEST_P(RHIScheduledSubmissionTest, AliasedNativeQueuesPreserveTransferBeforeCompute)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eTransfer, RHICommandContextType::eAsyncCompute};

    VerifyQueueSubmissionOrder(MakeVecView(queues, 2), 2);
}

TEST_P(RHIScheduledSubmissionTest, AliasedNativeQueueRejectionPreservesAcceptedPrefix)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eTransfer, RHICommandContextType::eAsyncCompute,
                                            RHICommandContextType::eTransfer};
    TestOwnedSchedule           schedule(*executor, MakeVecView(queues, 3));
    TestQueueFlushOrder         flushOrder(*rhi);
    rhi->beforeSubmission            = std::bind_front(&TestQueueFlushOrder::BeforeSubmission, &flushOrder);
    RHIResourcePtr<RHIBuffer> source = Buffer();
    RHIResourcePtr<RHIBuffer> buffer = Buffer();
    TestBuffer*               output = static_cast<TestBuffer*>(buffer.Get());
    TestBuffer*               input  = static_cast<TestBuffer*>(source.Get());
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(0));
    std::fill(output->bytes.begin(), output->bytes.end(), uint8_t(7));

    for (uint32_t i = 0; i < 3; ++i)
    {
        schedule.lists[i]->CopyBuffer(source.Get(), buffer.Get(), {0, i * 16, 16});
        if (i != 0)
        {
            schedule.Depends(i, i - 1);
        }
    }
    rhi->failSubmissionAt       = 2;
    const RHIBatchResult result = executor->SubmitGroups(schedule.groups, schedule.state);
    rhi->beforeSubmission       = {};

    ASSERT_EQ(result.submission, RHISubmissionResult::eFatal);
    ASSERT_EQ(result.groups.size(), 3u);
    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(result.groups[0].accepted.queue, RHICommandContextType::eTransfer);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.groups[1].accepted.serial, 0u);
    EXPECT_EQ(result.groups[2].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(result.groups[2].accepted.serial, 0u);
    EXPECT_EQ(result.requiredSerials.Get(RHICommandContextType::eTransfer), 1u);
    EXPECT_EQ(result.requiredSerials.Get(RHICommandContextType::eAsyncCompute), 0u);
    for (size_t i = 0; i < output->bytes.size(); ++i)
    {
        EXPECT_EQ(output->bytes[i], i < 16 ? 0u : 7u);
    }
    EXPECT_EQ(rhi->submissionAttempts, 2u);
    ASSERT_EQ(flushOrder.queues.size(), 2u);
    EXPECT_EQ(flushOrder.queues[0], RHICommandContextType::eTransfer);
    EXPECT_EQ(flushOrder.queues[1], RHICommandContextType::eAsyncCompute);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
}

TEST_P(RHIScheduledSubmissionTest, RejectedFirstRunRejectsEveryGroupAndCanRetry)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eGraphics, RHICommandContextType::eGraphics};

    TestOwnedSchedule rejected(*executor, MakeVecView(queues, 2));

    for (RHICommandListPtr& list : rejected.lists)
    {
        list->Dispatch(1, 1, 1);
    }

    rejected.Depends(1, 0);

    rhi->failSubmissionAt       = 1;

    const RHIBatchResult failed = executor->SubmitGroups(rejected.groups, rejected.state);

    ASSERT_EQ(failed.submission, RHISubmissionResult::eRejected);

    ASSERT_EQ(failed.groups.size(), 2u);

    for (const RHISubmissionGroupResult& group : failed.groups)
    {
        EXPECT_EQ(group.submission, RHISubmissionResult::eRejected);

        EXPECT_EQ(group.accepted.serial, 0u);
    }

    EXPECT_EQ(rhi->submissionAttempts, 1u);

    EXPECT_EQ(rhi->graphics.dispatchCount, 0u);

    EXPECT_FALSE(executor->AreSubmissionsBlocked());

    TestOwnedSchedule retry(*executor, MakeVecView(queues, 2));

    for (RHICommandListPtr& list : retry.lists)
    {
        list->Dispatch(1, 1, 1);
    }

    retry.Depends(1, 0);

    const RHIBatchResult accepted = executor->SubmitGroups(retry.groups, retry.state);

    ASSERT_EQ(accepted.submission, RHISubmissionResult::eSuccess);

    EXPECT_EQ(accepted.groups[0].accepted.serial, 1u);

    EXPECT_EQ(accepted.groups[1].accepted.serial, 2u);

    EXPECT_EQ(rhi->submissionAttempts, 2u);
}

TEST_P(RHIScheduledSubmissionTest, RejectedLaterRunKeepsEarlierAcceptedResultsAndStopsItsConsumers)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eGraphics, RHICommandContextType::eGraphics,
                                            RHICommandContextType::eAsyncCompute, RHICommandContextType::eAsyncCompute,
                                            RHICommandContextType::eGraphics};

    TestOwnedSchedule schedule(*executor, MakeVecView(queues, 5));

    for (uint32_t i = 0; i < 5; ++i)
    {
        schedule.lists[i]->Dispatch(1, 1, 1);

        if (i != 0)
        {
            schedule.Depends(i, i - 1);
        }
    }

    rhi->failSubmissionAt       = 2;

    const RHIBatchResult result = executor->SubmitGroups(schedule.groups, schedule.state);

    ASSERT_EQ(result.submission, RHISubmissionResult::eFatal);

    ASSERT_EQ(result.groups.size(), 5u);

    for (uint32_t i = 0; i < 2; ++i)
    {
        EXPECT_EQ(result.groups[i].submission, RHISubmissionResult::eSuccess);

        EXPECT_EQ(result.groups[i].accepted.serial, i + 1);

        EXPECT_EQ(result.groups[i + 2].submission, RHISubmissionResult::eFatal);

        EXPECT_EQ(result.groups[i + 2].accepted.serial, 0u);
    }

    EXPECT_EQ(result.groups[4].submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(rhi->graphics.dispatchCount, 2u);

    EXPECT_EQ(rhi->compute.dispatchCount, 0u);

    EXPECT_EQ(rhi->submissionAttempts, 2u);

    EXPECT_TRUE(executor->AreSubmissionsBlocked());
}

TEST_P(RHIScheduledSubmissionTest, InvalidScheduleDoesNotDetachAnyCommands)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute};
    TestOwnedSchedule                           schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(0, 1);
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, nullptr, schedule.state).IsValid());
    schedule.groups[0].predecessors.clear();
    schedule.groups[1].commands = schedule.lists[0].get();
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, nullptr, schedule.state).IsValid());
    EXPECT_FALSE(schedule.state->IsQueued());
    EXPECT_EQ(schedule.lists[0]->GetCommandCount(), 1u);
    EXPECT_EQ(schedule.lists[1]->GetCommandCount(), 1u);
    EXPECT_EQ(rhi->submissionAttempts, 0u);
}

TEST_P(RHIScheduledSubmissionTest, AcceptedPrefixFailureStopsRemainingGroupsAndPresentation)
{
    const SmallVector<RHICommandContextType, 3> queues{RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics,
                                                       RHICommandContextType::eTransfer};
    TestOwnedSchedule                           schedule(*executor, queues);
    for (RHICommandListPtr& list : schedule.lists)
    {
        list->Dispatch(1, 1, 1);
    }
    schedule.Depends(1, 0);
    schedule.Depends(2, 1);
    rhi->failSubmissionAt       = 2;
    const RHIBatchResult result = executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    ASSERT_EQ(result.groups.size(), 3u);
    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.groups[2].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 2u);
    EXPECT_EQ(viewport.preparePresents, 0u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    RHISubmissionDependency point;
    EXPECT_EQ(schedule.state->Resolve(0, point), RHISubmissionPointStatus::eFailed);
}

TEST_P(RHIScheduledSubmissionTest, StandaloneRejectionCanRetryWithoutAdvancingFrame)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics};
    TestOwnedSchedule                           rejected(*executor, queues);
    rejected.lists[0]->Dispatch(1, 1, 1);
    rejected.lists[1]->Dispatch(1, 1, 1);
    rejected.Depends(1, 0);
    rhi->failSubmissionAt = 1;
    EXPECT_EQ(executor->SubmitGroups(rejected.groups, rejected.state).submission, RHISubmissionResult::eRejected);
    EXPECT_FALSE(executor->AreSubmissionsBlocked());
    TestOwnedSchedule retry(*executor, queues);
    retry.lists[0]->Dispatch(1, 1, 1);
    retry.lists[1]->Dispatch(1, 1, 1);
    retry.Depends(1, 0);
    EXPECT_EQ(executor->SubmitGroups(retry.groups, retry.state).submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(ToValue(GetRHIThread().Invoke(&RHIFrameState::GetFrameNumber, &GRHIFrameState)), 0u);
}

TEST_P(RHIScheduledSubmissionTest, ComputeWithoutGraphicsJoinProtectsRetirementAndRecycling)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute};
    RHIResourcePtr<RHIBuffer>                   resource = Buffer();
    const uint64_t                              id       = resource->GetStableId();
    TestOwnedSchedule                           first(*executor, queues);
    first.lists[0]->Dispatch(1, 1, 1);
    first.lists[1]->ClearBuffer(resource.Get(), 0, 64);
    const RHIBatchResult submitted = executor->SubmitFrame(first.groups, nullptr, first.state).Wait();
    ASSERT_EQ(submitted.submission, RHISubmissionResult::eSuccess);
    resource.Reset();
    GetRHIThread().Invoke([this] { rhi->completed[0] = rhi->submitted[0]; });
    executor->FlushRHIThread();
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_TRUE(rhi->gpuDependencies.empty());
    GetRHIThread().Invoke([this] { rhi->completed[1] = rhi->submitted[1]; });
    executor->FlushRHIThread();
    EXPECT_TRUE(destroyed.contains(id));
    const SmallVector<RHICommandContextType, 3> reordered{
        RHICommandContextType::eTransfer, RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics};
    TestOwnedSchedule next(*executor, reordered);
    for (RHICommandListPtr& list : next.lists)
    {
        list->Dispatch(1, 1, 1);
    }
    const RHIBatchResult recycled = executor->SubmitFrame(next.groups, nullptr, next.state).Wait();
    ASSERT_EQ(recycled.submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(recycled.groups.size(), 3u);
    for (size_t i = 0; i < reordered.size(); ++i)
    {
        EXPECT_EQ(recycled.groups[i].accepted.queue, reordered[i]);
        EXPECT_EQ(next.lists[i]->GetContext()->GetContextType(), reordered[i]);
    }
}

INSTANTIATE_TEST_SUITE_P(InlineThreadedAndAliases,
                         RHIScheduledSubmissionTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded),
                                          testing::Bool()));

TEST_P(RHIScheduledSubmissionTest, PresentRejectionPreservesAcceptedRendering)
{
    RejectedPresentViewport&                    surface = rejectedSurface.emplace();
    const SmallVector<RHICommandContextType, 1> queues{RHICommandContextType::eGraphics};
    TestOwnedSchedule                           schedule(*executor, queues);
    schedule.lists[0]->Draw(3, 1, 0, 0);
    const RHIBatchResult result = executor->SubmitFrame(schedule.groups, &surface, schedule.state).Wait();
    EXPECT_EQ(result.presentation.error.code, surface.error.code);
    EXPECT_FALSE(result.presented);
    EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
    RHISubmissionDependency point;
    EXPECT_EQ(schedule.state->Resolve(0, point), RHISubmissionPointStatus::eAccepted);
    EXPECT_FALSE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(ToValue(GRHIFrameState.GetFrameNumber()), 1u);
    RHISubmissionTicket next = executor->SubmitFrame(schedule.groups, &viewport);
    EXPECT_TRUE(next.IsValid());
    if (next.IsValid())
    {
        EXPECT_TRUE(next.Wait().presented);
    }
}

TEST_P(RHIScheduledSubmissionTest, GroupPreservesPresentPreparationCause)
{
    RejectedPresentViewport& surface = rejectedSurface.emplace();
    surface.rejectPreparation        = true;
    const SmallVector<RHICommandContextType, 1> queues{RHICommandContextType::eGraphics};
    TestOwnedSchedule                           schedule(*executor, queues);
    schedule.lists[0]->Draw(3, 1, 0, 0);
    const RHIBatchResult result = executor->SubmitFrame(schedule.groups, &surface, schedule.state).Wait();
    ASSERT_EQ(result.groups.size(), 1u);
    EXPECT_EQ(result.cause.code, surface.error.code);
    EXPECT_EQ(result.groups[0].cause.code, surface.error.code);
}

TEST_P(RHIScheduledSubmissionTest, PresentationRejectionAfterComputeIsFatalAndRetainsOwners)
{
    const SmallVector<RHICommandContextType, 1> queues{RHICommandContextType::eAsyncCompute};
    TestOwnedSchedule                           schedule(*executor, queues);
    RHIResourcePtr<RHIBuffer>                   resource = Buffer();
    const uint64_t                              id       = resource->GetStableId();
    schedule.lists[0]->ClearBuffer(resource.Get(), 0, 64);
    viewport.recordPresentCommands = true;
    rhi->failSubmissionAt          = 2;
    const RHIBatchResult result    = executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.requiredSerials.Get(queues[0]), 1u);
    EXPECT_EQ(viewport.preparePresents, 1u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_FALSE(schedule.state->IsSubmissionFinished());
    resource.Reset();
    executor->WaitDeviceIdle();
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, nullptr).IsValid());
    schedule.lists.clear();
    viewport.ReleaseForTeardown();
    executor->Destroy();
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_P(RHIScheduledSubmissionTest, FirstGroupWithAcceptedWorkCannotBeRetried)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics};
    TestOwnedSchedule                           schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(1, 0);
    rhi->failSubmissionAt       = 1;
    rhi->submitBeforeFailure    = true;
    const RHIBatchResult result = executor->SubmitGroups(schedule.groups, schedule.state);
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.requiredSerials.Get(queues[0]), 1u);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(rhi->graphics.dispatchCount, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
}

TEST_P(RHIScheduledSubmissionTest, CompletionQueryFailurePreservesAcceptedComputeSerial)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics};
    TestOwnedSchedule                           schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(1, 0);
    rhi->beforeSubmission = [this] {
        rhi->failProgressQuery = true;
    };
    const RHIBatchResult result = executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    GetRHIThread().Invoke([this] { rhi->failProgressQuery = false; });
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.requiredSerials.Get(queues[0]), 1u);
    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
}

TEST_P(RHIScheduledSubmissionTest, FirstFrameGroupRejectionBlocksSubsequentFrames)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics};
    TestOwnedSchedule                           schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(1, 0);
    rhi->failSubmissionAt       = 1;
    const RHIBatchResult result = executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(result.requiredSerials.Get(queues[0]), 0u);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(viewport.preparePresents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, &viewport).IsValid());
    const RHIBatchResult blocked = executor->SubmitGroups(schedule.groups, {});
    EXPECT_EQ(blocked.cause.code, executor->GetTerminalError().code);
}

TEST_F(RHIExecutorTest, ShutdownRejectsNewGroupsBeforeTakingOwnership)
{
    executor->Destroy();
    const HeapVector<RHICommandContextType> queues;
    RefCountPtr<RHISubmissionState>         state  = MakeRefCountPtr<RHISubmissionState>(queues);
    const RHIBatchResult                    result = executor->SubmitGroups({}, state);
    EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(result.cause.code, RHIErrorCode::eCancelled);
    EXPECT_FALSE(state->IsQueued());
}
} // namespace
