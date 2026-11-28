#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/RHIThread.h"
#include "ZenFoundation/Utils/Errors.h"
#include <algorithm>
#include "ThreadStack.h"
#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif

namespace zen
{
namespace
{
class StallMonitor
{
public:
    StallMonitor(const char* operation, const RHIThreadActivity& activity) :
        m_operation(operation),
        m_activity(activity),
        m_interval(operation ? activity.stallReportMS.load(std::memory_order_relaxed) : 0),
        m_fatal(operation ? activity.stallFatalMS.load(std::memory_order_relaxed) : 0)
    {
#if defined(ZEN_WIN32)
        if (IsDebuggerPresent())
        {
            m_interval = m_fatal = std::chrono::milliseconds(0);
        }
#endif
        if (m_fatal.count() > 0 && (m_interval.count() <= 0 || m_interval > m_fatal))
        {
            m_interval = m_fatal;
        }
        if (m_interval.count() > 0)
        {
            m_start     = Clock::now();
            m_next      = m_start + m_interval;
            m_completed = m_activity.completed.load(std::memory_order_relaxed);
        }
    }

    uint32_t SliceMS() const
    {
        return m_interval.count() > 0 ? uint32_t(std::min<int64_t>(250, m_interval.count())) : UINT32_MAX;
    }

    void Check(bool finished)
    {
        if (m_interval.count() > 0 && (!finished || m_reported))
        {
            const Clock::time_point now = Clock::now();
            const int64_t           ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start).count();
            if (finished && m_reported)
            {
                ZEN_RHI_LOG_INFO("RHI stall recovered: {} after {} ms", m_operation, ms);
            }
            else if (!finished && (now >= m_next || (m_fatal.count() > 0 && ms >= m_fatal.count())))
            {
                const uint64_t completed = m_activity.completed.load(std::memory_order_relaxed) - m_completed;
                const int64_t  started   = m_activity.taskStartedMS.load(std::memory_order_relaxed);
                const int64_t  nowMS = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                ZEN_RHI_LOG_WARN("RHI stall: {} waited {} ms, worker={}, completed={}, queued={}, task_ms={}", m_operation, ms,
                                 completed ? "progressing"
                                 : started ? "executing"
                                           : "idle",
                                 completed, m_activity.queued.load(), started ? std::max<int64_t>(0, nowMS - started) : 0);
                if (!m_reported && m_activity.stallStackCapture.load(std::memory_order_relaxed))
                {
                    ReportWorkerStack(m_activity.threadID.load(std::memory_order_relaxed));
                }
                ZEN_RHI_VERIFY_MSG_F(m_fatal.count() <= 0 || ms < m_fatal.count(), "Fatal RHI stall in {} after {} ms",
                                     m_operation, ms);
                m_reported = true;
                m_interval = std::min(m_interval, std::chrono::milliseconds(30000)) * 2;
                m_next     = now + m_interval;
            }
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    const char*               m_operation;
    const RHIThreadActivity&  m_activity;
    std::chrono::milliseconds m_interval, m_fatal;
    Clock::time_point         m_start{}, m_next{};
    uint64_t                  m_completed{};
    bool                      m_reported{};
};

#if defined(ZEN_WIN32)
void ServiceSentMessages()
{
    // PeekMessage dispatches sent messages itself. Never remove or dispatch posted
    // messages here, including WM_QUIT, input and application events. Keep the peek
    // unfiltered: PM_QS_SENDMESSAGE-only peeks make Windows report the thread as hung.
    MSG message{};

    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
}

void WaitForRHIObject(HANDLE handle, StallMonitor& monitor)
{
    // The wait wakes only for messages that arrive after the queue was last examined.
    ServiceSentMessages();

    bool completed = false;

    while (!completed)
    {
        // Waiting on QS_SENDMESSAGE alone makes Windows report the thread as hung after
        // five seconds, even if it pumps elsewhere; DWM then ghosts its windows and
        // takes their input. Without MWMO_INPUTAVAILABLE, posted messages left queued
        // above do not wake this wait again.
        const DWORD result = MsgWaitForMultipleObjectsEx(1, &handle, monitor.SliceMS(), QS_ALLINPUT, 0);

        if (result == WAIT_OBJECT_0)
        {
            completed = true;
        }
        else if (result == WAIT_OBJECT_0 + 1)
        {
            ServiceSentMessages();
        }
        else if (result != WAIT_TIMEOUT)
        {
            ZEN_RHI_VERIFY_MSG_F(false, "RHI object wait failed: {}", GetLastError());
        }
        monitor.Check(completed);
    }
}
#endif
} // namespace

RHIThreadEvent::RHIThreadEvent(bool signaled)
{
#if defined(ZEN_WIN32)
    m_handle = CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr);

    if (m_handle == nullptr)
    {
        ZEN_RHI_VERIFY_MSG_F(false, "Cannot create RHI event: {}", GetLastError());
    }
#else
    m_signaled = signaled;
#endif
}

RHIThreadEvent::~RHIThreadEvent()
{
#if defined(ZEN_WIN32)
    CloseHandle(m_handle);
#endif
}

void RHIThreadEvent::Signal()
{
#if defined(ZEN_WIN32)
    ZEN_RHI_VERIFY(SetEvent(m_handle));
#else
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_signaled = true;
    }

    m_available.notify_all();
#endif
}

void RHIThreadEvent::Reset()
{
#if defined(ZEN_WIN32)
    ZEN_RHI_VERIFY(ResetEvent(m_handle));
#else
    std::lock_guard<std::mutex> lock(m_mutex);

    m_signaled = false;
#endif
}

void RHIThreadEvent::Wait() const
{
    static const RHIThreadActivity unwatched;
    Wait(nullptr, unwatched);
}

void RHIThreadEvent::Wait(const char* operation, const RHIThreadActivity& activity) const
{
    StallMonitor monitor(operation, activity);
#if defined(ZEN_WIN32)
    WaitForRHIObject(m_handle, monitor);
#else
    std::unique_lock<std::mutex> lock(m_mutex);

    while (!m_signaled)
    {
        if (monitor.SliceMS() == UINT32_MAX)
        {
            m_available.wait(lock);
        }
        else
        {
            m_available.wait_for(lock, std::chrono::milliseconds(monitor.SliceMS()));
            if (!m_signaled)
            {
                lock.unlock();
                monitor.Check(false);
                lock.lock();
            }
        }
    }
    lock.unlock();
    monitor.Check(true);
#endif
}

thread_local RHIThread* RHIThread::s_current = nullptr;

void RHIThread::SetStallReports(std::chrono::milliseconds report, std::chrono::milliseconds fatal, bool captureStack)
{
    m_activity.stallReportMS.store(report.count(), std::memory_order_relaxed);
    m_activity.stallFatalMS.store(fatal.count(), std::memory_order_relaxed);
    m_activity.stallStackCapture.store(captureStack, std::memory_order_relaxed);
}

RHIThread::~RHIThread()
{
    Stop();
}

void RHIThread::Start(RHIExecutionMode mode, size_t capacity)
{
    ZEN_RHI_VERIFY_MSG(!m_worker.joinable(), "The RHI thread is already running");

    m_capacity      = std::max(size_t(1), capacity);

    m_stopping      = false;

    m_cleanupClosed = false;

    m_failure       = {};

    m_deviceLost.store(false, std::memory_order_release);

    m_taskFailed.store(false, std::memory_order_release);

    m_space.Signal();

    m_threaded.store(mode == RHIExecutionMode::eThreaded, std::memory_order_release);

    if (m_threaded.load(std::memory_order_acquire))
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_workerRunning = true;
        }
#if !defined(ZEN_WIN32)
        m_workerExited.Reset();
#endif
        m_worker = std::thread(&RHIThread::Run, this);
    }
}

void RHIThread::Stop(std::function<void()> finalizer)
{
    const bool retiresBackend = static_cast<bool>(finalizer);

    if (m_worker.joinable())
    {
        ZEN_RHI_VERIFY(s_current != this);
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            m_stopping  = true;

            m_finalizer = std::move(finalizer);

            m_space.Signal();
        }

        m_available.notify_all();
#if defined(ZEN_WIN32)
        StallMonitor monitor("RHI shutdown", m_activity);
        WaitForRHIObject(m_worker.native_handle(), monitor);
#else
        m_workerExited.Wait("RHI shutdown", m_activity);
#endif
        m_worker.join();
    }
    else if (retiresBackend)
    {
        // Inline mode closes admission at the same boundary as the worker.
        m_cleanupClosed.store(true, std::memory_order_release);

        ExecuteFinalizer(std::move(finalizer));
    }

    // A standalone inline backend can follow an executor once it reopens admission.
    // The finalizer must destroy every native resource before returning.
    m_threaded.store(false, std::memory_order_release);
}

void RHIThread::CloseCleanupAdmission()
{
    m_cleanupClosed.store(true, std::memory_order_release);
}

void RHIThread::ExecuteFinalizer(std::function<void()> finalizer)
{
    // Releases from the finalizer are RHI-thread work, as they are on the worker.
    RHIThread* previous = s_current;

    s_current           = this;

    Task task{std::move(finalizer), {}, true};

    ExecuteTask(task);

    s_current = previous;
}

bool RHIThread::Dispatch(std::function<void()> task, std::function<void()> cancelled)
{
    return static_cast<bool>(DispatchChecked(std::move(task), std::move(cancelled)));
}

bool RHIThread::DispatchCleanup(std::function<void()> task)
{
    return static_cast<bool>(Enqueue({std::move(task), {}, true}, true));
}

bool RHIThread::TryDispatch(std::function<void()> task)
{
    return static_cast<bool>(DispatchChecked(std::move(task), {}, false));
}

RHIJobAdmissionResult RHIThread::DispatchChecked(std::function<void()> task, std::function<void()> cancelled, bool waitForSpace)
{
    return Enqueue({std::move(task), std::move(cancelled), false}, waitForSpace);
}

void RHIThread::Fail(RHIError error)
{
    ZEN_RHI_VERIFY(error.IsFailure());
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_failure.IsFailure())
        {
            m_failure = error;
        }

        m_taskFailed.store(true, std::memory_order_release);

        if (error.code == RHIErrorCode::eDeviceLost)
        {
            m_deviceLost.store(true, std::memory_order_release);
        }

        m_space.Signal();
    }

    m_available.notify_all();
}

RHIError RHIThread::GetFailure() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_failure;
}

RHIJobAdmissionResult RHIThread::Enqueue(Task task, bool waitForSpace)
{
    RHIJobAdmissionResult result;

    if (s_current == this || !m_threaded.load(std::memory_order_acquire))
    {
        if (!task.cleanup && HasTaskFailure())
        {
            result = {RHIJobAdmission::eFailed, GetFailure()};
        }
        else if (s_current == this || !m_cleanupClosed.load(std::memory_order_acquire))
        {
            result.admission = RHIJobAdmission::eAccepted;

            ExecuteTask(task);
        }
    }
    else
    {
        bool runInline = false;
        result         = Push(task, waitForSpace, runInline);
        if (runInline)
        {
            RHIThread* previous = s_current;
            s_current           = this;
            ExecuteTask(task);
            s_current = previous;
        }
    }

    if (!result)
    {
        if (!result.error.IsFailure())
        {
            result.error = MakeRHIError(RHIErrorCode::eCancelled, "RHI job admission", __FILE__, __LINE__);
        }

        if (task.cancelled)
        {
            Task cancellation{std::move(task.cancelled), {}, true};

            ExecuteTask(cancellation);
        }
    }

    return result;
}

RHIJobAdmissionResult RHIThread::Push(Task& task, bool waitForSpace, bool& outRunInline)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    while (waitForSpace && !task.cleanup && !m_stopping && !m_taskFailed && m_tasks.Size() >= m_capacity)
    {
        lock.unlock();

        m_space.Wait("RHI queue space", m_activity);

        lock.lock();
    }

    RHIJobAdmissionResult result;

    if (!task.cleanup && m_taskFailed)
    {
        result = {RHIJobAdmission::eFailed, m_failure};
    }
    else if (task.cleanup ? !m_cleanupClosed : !m_stopping)
    {
        result.admission =
            task.cleanup || m_tasks.Size() < m_capacity ? RHIJobAdmission::eAccepted : RHIJobAdmission::eQueueFull;
    }

    outRunInline = result && !m_workerRunning;
    // Cleanup bypasses capacity so final releases cannot deadlock a draining worker.
    if (result && !outRunInline)
    {
        m_tasks.Push(std::move(task));
        m_activity.queued.store(m_tasks.Size(), std::memory_order_relaxed);

        if (m_tasks.Size() >= m_capacity && !m_stopping && !m_taskFailed)
        {
            m_space.Reset();
        }
    }

    lock.unlock();

    if (result)
    {
        m_available.notify_one();
    }

    return result;
}

void RHIThread::ExecuteTask(Task& task) noexcept
{
    if (task.cleanup || !HasTaskFailure())
    {
        task.execute();
    }
    else if (task.cancelled)
    {
        task.cancelled();
    }
}

bool RHIThread::HasTaskFailure() const
{
    return m_taskFailed.load(std::memory_order_acquire);
}

void RHIThread::Fence() {}

void RHIThread::Flush()
{
    Invoke(&RHIThread::Fence);
}

bool RHIThread::IsThreaded() const
{
    return m_threaded.load(std::memory_order_acquire);
}

bool RHIThread::IsCurrentThread() const
{
    return s_current == this
        || (!m_threaded.load(std::memory_order_acquire) && !m_cleanupClosed.load(std::memory_order_acquire));
}

void RHIThread::CheckOwnership() const
{
    ZEN_RHI_VERIFY_MSG(IsCurrentThread(), "Native RHI work must run on the RHI thread");
}

void RHIThread::Run()
{
    s_current = this;
#if defined(ZEN_WIN32)
    m_activity.threadID.store(GetCurrentThreadId());
    SetThreadDescription(GetCurrentThread(), L"RHIThread");
#endif
    bool finished = false;

    while (!finished)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);

            while (m_tasks.Empty() && !m_stopping)
            {
                RHIWorkerTimer idle(RHIWorkerTime::eIdle);
                m_available.wait(lock);
            }

            finished = m_stopping && m_tasks.Empty();

            if (finished)
            {
                m_cleanupClosed = static_cast<bool>(m_finalizer);
                m_workerRunning = false;

                task            = {std::move(m_finalizer), {}, true};
            }
            else
            {
                std::optional<Task> pending = m_tasks.TryPop();
                m_activity.queued.store(m_tasks.Size(), std::memory_order_relaxed);

                task = std::move(*pending);

                // Cleanup can exceed capacity. Wake producers only once a slot exists, or a
                // waiting Dispatch would spin on a signaled event while the queue is full.
                if (m_tasks.Size() < m_capacity)
                {
                    m_space.Signal();
                }
            }
        }

        if (task.execute)
        {
            m_activity.taskStartedMS.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
            ExecuteTask(task);
            m_activity.completed.fetch_add(1, std::memory_order_relaxed);
            m_activity.taskStartedMS.store(0, std::memory_order_relaxed);
        }
    }

    m_activity.threadID.store(0);
    s_current = nullptr;
#if !defined(ZEN_WIN32)
    m_workerExited.Signal();
#endif
}
} // namespace zen
