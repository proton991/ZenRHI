#include "ZenRHI/RHIThread.h"
#include "ZenRHI/RHIDeviceDesc.h"
#include "ZenRHI/RHIGPUFrameTiming.h"
#include "ZenRHI/RHICommandListExecutor.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>
#if defined(ZEN_WIN32)
#    include <windows.h>
#endif

using namespace zen;

TEST(RHIThreadTest, FifoNestedInvokeAndDrain)
{
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded, 2);
    HeapVector<uint32_t> order;
    for (uint32_t i = 0; i < 32; ++i)
    {
        thread.Dispatch([&order, i] { order.push_back(i); });
    }
    const std::thread::id worker = thread.Invoke([] { return std::this_thread::get_id(); });
    EXPECT_NE(worker, std::this_thread::get_id());
    EXPECT_EQ(thread.Invoke([&thread] { return thread.Invoke([] { return 17; }); }), 17);
    RHIResult<int> checked = thread.InvokeChecked([] { return 23; });

    ASSERT_TRUE(checked);

    EXPECT_EQ(checked.GetValue(), 23);
    thread.Stop();
    ASSERT_EQ(order.size(), 32u);
    for (uint32_t i = 0; i < 32; ++i)
    {
        EXPECT_EQ(order[i], i);
    }
}

namespace
{
std::atomic<bool> observedStall{};
std::atomic<bool> observedStack{};
std::atomic<bool> observedRecovery{};
void              CaptureStall(const RHIMessage& diagnostic) noexcept
{
    if ((diagnostic.text.starts_with("RHI stall: synchronous RHI call")
         || diagnostic.text.starts_with("RHI stall: RHI shutdown"))
        && diagnostic.text.find("worker=executing") != std::string_view::npos)
    {
        observedStall.store(true);
    }
    if (diagnostic.text.starts_with("RHI worker stack [0]"))
    {
        observedStack.store(true);
    }
    if (diagnostic.text.starts_with("RHI stall recovered: synchronous RHI call")
        || diagnostic.text.starts_with("RHI stall recovered: RHI shutdown"))
    {
        observedRecovery.store(true);
    }
}
bool WaitForStallReport()
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!observedStall.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return observedStall.load();
}
} // namespace

TEST(RHIThreadTest, ReportsBlockedInvocationAndResumes)
{
#if defined(ZEN_WIN32)
    if (IsDebuggerPresent())
    {
        GTEST_SKIP() << "The RHI watchdog suppresses reports while a debugger is attached";
    }
#endif
    const RHIMessageCallback callback = GetRHIMessageCallback();
    SetRHIMessageCallback(CaptureStall);
    observedStall.store(false);
    observedStack.store(false);
    observedRecovery.store(false);
    RHIThread thread;
    thread.SetStallReports(std::chrono::milliseconds(10), std::chrono::milliseconds(0), true);
    thread.Start(RHIExecutionMode::eThreaded);
    EXPECT_TRUE(thread.Invoke(WaitForStallReport));
    EXPECT_TRUE(observedRecovery.load());
    observedStall.store(false);
    observedRecovery.store(false);
    thread.Stop([] { WaitForStallReport(); });
    EXPECT_TRUE(observedStall.load());
    SetRHIMessageCallback(callback);
    EXPECT_TRUE(observedRecovery.load());
#if defined(_WIN32) && defined(_M_X64)
    EXPECT_TRUE(observedStack.load());
#endif
}

TEST(RHIDiagnosticsTest, ReportsAnEmptyDefaultView)
{
    const RHIMessageCallback previous = GetRHIMessageCallback();
    SetRHIMessageCallback(nullptr);
    testing::internal::CaptureStderr();
    ReportRHIMessage(RHIMessageSeverity::eInfo, {});
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "[info] \n");
    SetRHIMessageCallback(previous);
}

TEST(RHIThreadTest, InlineUsesCallingThread)
{
    RHIThread thread;
    thread.Start(RHIExecutionMode::eInline);
    EXPECT_EQ(thread.Invoke([] { return std::this_thread::get_id(); }), std::this_thread::get_id());
    thread.Stop();
}

TEST(RHIThreadDeathTest, ExplicitFatalThresholdEndsABlockedInvocation)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            RHIThread thread;
            thread.SetStallReports(std::chrono::milliseconds(0), std::chrono::milliseconds(30), false);
            thread.Start(RHIExecutionMode::eThreaded);
            thread.Invoke([] { std::this_thread::sleep_for(std::chrono::seconds(2)); });
            thread.Stop();
        },
        "Fatal RHI stall in synchronous RHI call");
}

TEST(RHIGPUMemoryBudgetTest, PressureUsesDeviceLocalBudgetAndHandlesUnavailableTelemetry)
{
    RHIGPUMemoryStats stats;
    stats.heapCount = 2;
    stats.heaps[0]  = {1000, 950, 1000, false};
    stats.heaps[1]  = {1000, 900, 1000, true};
    EXPECT_FALSE(stats.IsUnderPressure());

    stats.budgetAvailable = true;
    EXPECT_TRUE(stats.IsUnderPressure());
    stats.heaps[1].usageBytes = 899;
    EXPECT_FALSE(stats.IsUnderPressure());
    stats.heaps[1].budgetBytes = 0;
    EXPECT_FALSE(stats.IsUnderPressure());
}

TEST(RHIThreadProductionTest, TaskFailureCancelsNormalWorkAndPreservesCleanup)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHIThread thread;
        thread.Start(mode);
        uint32_t cancellations = 0;
        uint32_t cleanups      = 0;
        uint32_t normal        = 0;
        thread.Dispatch([&thread] {
            thread.Fail(MakeRHIError(RHIErrorCode::eBackendFailure, "injected task failure", __FILE__, __LINE__));
        });
        thread.Dispatch([&normal] { ++normal; }, [&cancellations] { ++cancellations; });
        EXPECT_TRUE(thread.DispatchCleanup([&cleanups] { ++cleanups; }));
        thread.Stop([&cleanups] { ++cleanups; });
        EXPECT_TRUE(thread.HasTaskFailure());
        EXPECT_EQ(cancellations, 1u);
        EXPECT_EQ(cleanups, 2u);
        EXPECT_EQ(normal, 0u);
    }
}

TEST(RHIThreadProductionTest, ShutdownDrainsLateCleanupBeforeTheFinalizer)
{
    RHIThread            thread;
    RHIThreadEvent       entered;
    RHIThreadEvent       release;
    HeapVector<uint32_t> order;
    thread.Start(RHIExecutionMode::eThreaded);
    thread.Dispatch([&entered, &release] {
        entered.Signal();
        release.Wait();
    });
    entered.Wait();

    std::thread stopper([&thread, &order] { thread.Stop([&order] { order.push_back(2); }); });

    while (!thread.IsStopping())
    {
        std::this_thread::yield();
    }

    uint32_t cancellations = 0;
    EXPECT_FALSE(thread.Dispatch([] {}, [&cancellations] { ++cancellations; }));
    EXPECT_TRUE(thread.DispatchCleanup([&order] { order.push_back(1); }));
    release.Signal();
    stopper.join();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 2u);
    EXPECT_EQ(cancellations, 1u);
}



namespace
{
struct QueueStallGate
{
    std::promise<void>            waiting;
    RHIThreadEvent                resume;
    static inline QueueStallGate* active{};
    static void                   OnMessage(const RHIMessage& diagnostic) noexcept
    {
        if (diagnostic.text.starts_with("RHI stall: RHI queue space") && !active->reported.exchange(true))
        {
            active->waiting.set_value();
            active->resume.Wait();
        }
    }
    std::atomic<bool> reported{};
};
} // namespace

TEST(RHIThreadProductionTest, LateCleanupKeepsShutdownWakeVisibleToFullQueueProducer)
{
    using namespace std::chrono_literals;
    RHIThread      thread;
    RHIThreadEvent entered, release;
    QueueStallGate gate;
    QueueStallGate::active            = &gate;
    const RHIMessageCallback callback = GetRHIMessageCallback();
    thread.SetStallReports(5ms, 0ms, true);
    SetRHIMessageCallback(QueueStallGate::OnMessage);
    thread.Start(RHIExecutionMode::eThreaded, 1);
    thread.Dispatch([&entered, &release] {
        entered.Signal();
        release.Wait();
    });
    entered.Wait();
    thread.Dispatch([] {});
    std::future<RHIJobAdmissionResult> producer =
        std::async(std::launch::async, [&thread] { return thread.DispatchChecked([] {}); });
    const bool  reached = gate.waiting.get_future().wait_for(2s) == std::future_status::ready;
    std::thread stopper([&thread] { thread.Stop(); });
    while (!thread.IsStopping())
    {
        std::this_thread::yield();
    }
    EXPECT_TRUE(thread.DispatchCleanup([] {}));
    gate.resume.Signal();
    const bool woke = producer.wait_for(1s) == std::future_status::ready;
    release.Signal();
    stopper.join();
    EXPECT_EQ(producer.get().admission, RHIJobAdmission::eClosed);
    SetRHIMessageCallback(callback);
    QueueStallGate::active = nullptr;
    if (!reached)
    {
        GTEST_SKIP() << "Stall diagnostics unavailable";
    }
    EXPECT_TRUE(woke);
}

TEST(RHIThreadProductionTest, FullQueueFailureWakesProducersAndPreservesTheFirstCause)
{
    RHIThread thread;

    thread.Start(RHIExecutionMode::eThreaded, 1);

    RHIThreadEvent entered;

    RHIThreadEvent release;

    ASSERT_TRUE(thread.Dispatch([&entered, &release] {
        entered.Signal();

        release.Wait();
    }));

    entered.Wait();

    uint32_t cancelled = 0;

    uint32_t executed  = 0;

    EXPECT_TRUE(thread.Dispatch([&executed] { ++executed; }, [&cancelled] { ++cancelled; }));

    EXPECT_EQ(thread.DispatchChecked([] {}, {}, false).admission, RHIJobAdmission::eQueueFull);

    RHIJobAdmissionResult producerResult;

    RHIThreadEvent producerStarted;

    std::thread producer([&thread, &producerResult, &producerStarted] {
        producerStarted.Signal();

        producerResult = thread.DispatchChecked([] {});
    });

    producerStarted.Wait();

    thread.Fail({RHIErrorCode::eOutOfDeviceMemory, -2, "first failure", __FILE__, 7, 91});

    thread.Fail({RHIErrorCode::eDeviceLost, -4, "later device loss", __FILE__, 8});

    producer.join();

    EXPECT_EQ(producerResult.admission, RHIJobAdmission::eFailed);

    EXPECT_EQ(producerResult.error.resourceId, 91u);

    EXPECT_TRUE(thread.HasDeviceLoss());

    EXPECT_STREQ(thread.GetFailure().operation, "first failure");

    release.Signal();

    thread.Flush();

    EXPECT_EQ(cancelled, 1u);

    EXPECT_EQ(executed, 0u);

    thread.Stop();
}

TEST(RHIThreadProductionTest, CheckedInvocationReturnsMoveOnlyValuesAndCancellationWithoutWaiting)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHIThread thread;

        thread.Start(mode);

        RHIResult<UniquePtr<int>> value = thread.InvokeChecked([] { return MakeUnique<int>(17); });

        ASSERT_TRUE(value);

        EXPECT_EQ(*value.GetValue(), 17);

        EXPECT_TRUE(thread.InvokeChecked([] {}));

        thread.Fail({RHIErrorCode::eBackendFailure, -99, "invoke cancellation", __FILE__, 12});

        uint32_t calls           = 0;

        RHIResult<int> cancelled = thread.InvokeChecked([&calls] {
            ++calls;
            return 42;
        });

        EXPECT_FALSE(cancelled);

        EXPECT_EQ(cancelled.GetError().nativeCode, -99);

        EXPECT_EQ(calls, 0u);

        EXPECT_TRUE(thread.DispatchCleanup([&calls] { ++calls; }));

        thread.Stop([] {});

        EXPECT_EQ(calls, 1u);

        EXPECT_EQ(thread.DispatchChecked([] {}).admission, RHIJobAdmission::eFailed);
    }
}
