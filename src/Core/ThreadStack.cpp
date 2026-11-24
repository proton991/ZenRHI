#include "ThreadStack.h"
#include "ZenRHI/RHIDiagnostics.h"

#if defined(_WIN32) && defined(_M_X64)
#    include <Windows.h>
#    include <DbgHelp.h>
#    include <mutex>
#    include <cstring>
#    include <cstdio>

namespace
{
size_t CaptureFrames(CONTEXT& context, DWORD64 (&frames)[64])
{
    size_t count = 0;
    // A driver frame can lack unwind metadata or describe inaccessible stack memory.
    __try
    {
        while (context.Rip && count < 64)
        {
            frames[count++] = context.Rip;
            DWORD64           imageBase{};
            PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (entry)
            {
                PVOID   handler{};
                DWORD64 frame{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handler, &frame, nullptr);
            }
            else
            {
                context.Rip  = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += sizeof(DWORD64);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Retain the frames already captured.
    }
    return count;
}

class WorkerSymbols
{
public:
    WorkerSymbols()
    {
        // Use a private DbgHelp session so hosts can retain their own symbol search paths.
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &m_process, 0, FALSE,
                            DUPLICATE_SAME_ACCESS))
        {
            m_initialized = SymInitialize(m_process, nullptr, TRUE) != FALSE;
        }
    }
    ~WorkerSymbols()
    {
        if (m_initialized)
        {
            SymCleanup(m_process);
        }
        if (m_process)
        {
            CloseHandle(m_process);
        }
    }
    void Describe(DWORD64 address, char (&out)[512])
    {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        SYMBOL_INFO*              symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen               = MAX_SYM_NAME;
        DWORD64 displacement{};
        HMODULE module{};
        char    path[MAX_PATH]{};
        if (m_initialized && SymFromAddr(m_process, address, &displacement, symbol))
        {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD offset{};
            if (SymGetLineFromAddr64(m_process, address, &offset, &line))
            {
                std::snprintf(out, sizeof(out), "%s+0x%llx (%s:%lu)", symbol->Name, displacement, line.FileName,
                              line.LineNumber);
            }
            else
            {
                std::snprintf(out, sizeof(out), "%s+0x%llx", symbol->Name, displacement);
            }
        }
        else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(address), &module))
        {
            GetModuleFileNameA(module, path, MAX_PATH);
            const char* fileName = std::strrchr(path, '\\');
            std::snprintf(out, sizeof(out), "%s+0x%llx", fileName ? fileName + 1 : path,
                          address - reinterpret_cast<DWORD64>(module));
            FreeLibrary(module);
        }
        else
        {
            std::snprintf(out, sizeof(out), "0x%llx", address);
        }
    }

private:
    HANDLE m_process{};
    bool   m_initialized{};
};
} // namespace
#endif

namespace zen
{
void ReportWorkerStack(uint32_t threadID)
{
#if defined(_WIN32) && defined(_M_X64)
    DWORD64 frames[64]{};
    size_t  count  = 0;
    HANDLE  thread = threadID && threadID != GetCurrentThreadId()
                       ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadID)
                       : nullptr;
    if (thread)
    {
        if (GetProcessIdOfThread(thread) == GetCurrentProcessId() && SuspendThread(thread) != DWORD(-1))
        {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context))
            {
                count = CaptureFrames(context, frames);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    for (size_t i = 0; i < count; ++i)
    {
        char description[512]{};
        {
            static std::mutex           symbolMutex;
            std::lock_guard<std::mutex> lock(symbolMutex);
            static WorkerSymbols        symbols;
            symbols.Describe(frames[i], description);
        }
        ZEN_RHI_LOG_WARN("RHI worker stack [{}] {}", i, description);
    }
#else
    (void)threadID;
#endif
}
} // namespace zen
