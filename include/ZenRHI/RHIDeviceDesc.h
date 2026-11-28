#pragma once
#include <cstdint>
#include <chrono>
#include <span>
#include <memory>
#include <vector>
#include <string>
#include <string_view>
#include "RHICommon.h"
#include "RHIDiagnostics.h"

namespace zen
{
// Swapchain present-mode request. eDefault follows the viewport's VSync flag: FIFO when VSync
// is on; IMMEDIATE, then MAILBOX, then FIFO when it is off. An explicit mode falls back to
// FIFO, the only mode every surface supports.
enum class RHIPresentMode : uint8_t
{
    eDefault,
    eFifo,
    eFifoRelaxed,
    eMailbox,
    eImmediate
};

// Accepts "default", "fifo", "fifo_relaxed", "mailbox" and "immediate".
inline bool ParseRHIPresentMode(std::string_view name, RHIPresentMode& outMode)
{
    bool valid = true;

    if (name == "default")
    {
        outMode = RHIPresentMode::eDefault;
    }
    else if (name == "fifo")
    {
        outMode = RHIPresentMode::eFifo;
    }
    else if (name == "fifo_relaxed")
    {
        outMode = RHIPresentMode::eFifoRelaxed;
    }
    else if (name == "mailbox")
    {
        outMode = RHIPresentMode::eMailbox;
    }
    else if (name == "immediate")
    {
        outMode = RHIPresentMode::eImmediate;
    }
    else
    {
        valid = false;
    }

    return valid;
}

enum class RHIValidationMode : uint8_t
{
    eOff,
    eRequested,
    eRequired
};

// Creation parameters. Each backend keeps its own copy, so later changes to a description
// do not reach a device that already exists.
class RHIDeviceDesc
{
public:
    // Diagnostic opt-out, set before device creation. Otherwise RT follows device capabilities.
    void SetRayTracingDisabled(bool disabled)
    {
        m_rayTracingDisabled = disabled;
    }

    bool RayTracingDisabled() const
    {
        return m_rayTracingDisabled;
    }

    // Diagnostic device-memory limit in bytes, set before device creation: each device-local heap
    // larger than this is capped to it, so allocations past it fail and memory budgets report the
    // smaller device (an 8 GB card on a larger one). Zero uses the device's own heaps.
    void SetDeviceMemoryLimit(uint64_t bytes)
    {
        m_deviceMemoryLimit = bytes;
    }

    uint64_t DeviceMemoryLimit() const
    {
        return m_deviceMemoryLimit;
    }

    // Diagnostic adapter choice for cross-vendor runs. A valid device whose name contains
    // this text wins over the scored choice; empty uses the scored choice.
    void SetPreferredDevice(std::string_view name)
    {
        m_preferredDevice = name;
    }

    const std::string& PreferredDevice() const
    {
        return m_preferredDevice;
    }

    void SetGPUProfilerMarkers(bool enabled)
    {
        m_gpuProfilerMarkers = enabled;
    }

    bool GPUProfilerMarkers() const
    {
        return m_gpuProfilerMarkers;
    }

    // Opt-in compiler statistics, set before device creation. Never enabled for normal timing runs.
    void SetPipelineStatistics(bool enabled)
    {
        m_pipelineStatistics = enabled;
    }

    bool PipelineStatistics() const
    {
        return m_pipelineStatistics;
    }

    // Supply a snapshot previously returned by this SDK. Copies of the descriptor share the immutable bytes.
    void SetPipelineCacheSeed(std::vector<uint8_t> bytes)
    {
        m_pipelineCacheSeed = bytes.empty() ? nullptr : std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    }

    std::span<const uint8_t> PipelineCacheSeed() const
    {
        return m_pipelineCacheSeed ? std::span<const uint8_t>(*m_pipelineCacheSeed) : std::span<const uint8_t>{};
    }

    void SetStallReportThreshold(std::chrono::milliseconds threshold)
    {
        m_stallReportThreshold = threshold;
    }

    std::chrono::milliseconds StallReportThreshold() const
    {
        return m_stallReportThreshold;
    }

    void SetStallFatalThreshold(std::chrono::milliseconds threshold)
    {
        m_stallFatalThreshold = threshold;
    }

    std::chrono::milliseconds StallFatalThreshold() const
    {
        return m_stallFatalThreshold;
    }

    void SetStallStackCapture(bool enabled)
    {
        m_stallStackCapture = enabled;
    }

    bool StallStackCapture() const
    {
        return m_stallStackCapture;
    }

    void SetDeviceLossDiagnostics(bool enabled)
    {
        m_deviceLossDiagnostics = enabled;
    }

    bool DeviceLossDiagnostics() const
    {
        return m_deviceLossDiagnostics;
    }

    void SetGPUMemoryStats(bool enabled)
    {
        m_gpuMemoryStats = enabled;
    }

    bool GPUMemoryStats() const
    {
        return m_gpuMemoryStats;
    }

    void SetValidationEnabled(bool enabled)
    {
        SetValidationMode(enabled ? RHIValidationMode::eRequested : RHIValidationMode::eOff);
    }

    bool ValidationEnabled() const
    {
        return m_validationMode != RHIValidationMode::eOff;
    }

    void SetValidationMode(RHIValidationMode mode)
    {
        m_validationMode = mode;
    }

    RHIValidationMode ValidationMode() const
    {
        return m_validationMode;
    }

    void SetDebugPrintfEnabled(bool enabled)
    {
        m_debugPrintfEnabled = enabled;
    }

    bool DebugPrintfEnabled() const
    {
        return m_debugPrintfEnabled;
    }

    void SetWorkerTimingEnabled(bool enabled)
    {
        m_workerTimingEnabled = enabled;
    }
    bool WorkerTimingEnabled() const
    {
        return m_workerTimingEnabled;
    }

    void SetExecutionCountersEnabled(bool enabled)
    {
        m_executionCountersEnabled = enabled;
    }

    bool ExecutionCountersEnabled() const
    {
        return m_executionCountersEnabled;
    }

    void SetRobustBufferAccessEnabled(bool enabled)
    {
        m_robustBufferAccessEnabled = enabled;
    }

    bool RobustBufferAccessEnabled() const
    {
        return m_robustBufferAccessEnabled;
    }

    // Every swapchain the device creates or recreates presents with this mode.
    void SetPresentMode(RHIPresentMode mode)
    {
        m_presentMode = mode;
    }

    RHIPresentMode PresentMode() const
    {
        return m_presentMode;
    }

    // Read when the backend initializes. Every heap needs at least one slot. Requested counts
    // are upper bounds; QueryGPUInfo reports the capacities fitted to the selected device.
    bool SetBindlessHeapCapacities(const RHIBindlessHeapCapacities& capacities)
    {
        bool valid = true;

        for (uint32_t count : capacities.slots)
        {
            valid = valid && count != 0;
        }

        if (valid)
        {
            m_bindlessHeapCapacities = capacities;
        }

        return valid;
    }

    const RHIBindlessHeapCapacities& BindlessHeapCapacities() const
    {
        return m_bindlessHeapCapacities;
    }

    using SurfaceExtensions = std::span<const char* const> (*)();
    SurfaceExtensions                                      surfaceExtensions{};

private:
    std::shared_ptr<const std::vector<uint8_t>> m_pipelineCacheSeed;
    RHIPresentMode                              m_presentMode{RHIPresentMode::eDefault};
    RHIBindlessHeapCapacities                   m_bindlessHeapCapacities;
#if defined(NDEBUG)
    RHIValidationMode m_validationMode{RHIValidationMode::eOff};
#else
    RHIValidationMode m_validationMode{RHIValidationMode::eRequested};
#endif
    bool                      m_workerTimingEnabled{};
    bool                      m_executionCountersEnabled{true};
    bool                      m_debugPrintfEnabled{false};
    bool                      m_robustBufferAccessEnabled{false};
    bool                      m_rayTracingDisabled{false};
    uint64_t                  m_deviceMemoryLimit{0};
    std::string               m_preferredDevice;
    bool                      m_gpuProfilerMarkers{false};
    bool                      m_pipelineStatistics{false};
    bool                      m_gpuMemoryStats{false};
    std::chrono::milliseconds m_stallReportThreshold{10000};
    std::chrono::milliseconds m_stallFatalThreshold{};
    bool                      m_stallStackCapture{true};
    bool                      m_deviceLossDiagnostics{false};
};
} // namespace zen
