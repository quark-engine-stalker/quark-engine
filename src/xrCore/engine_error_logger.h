#ifndef engine_error_loggerH
#define engine_error_loggerH
#pragma once

namespace EngineErrorLogger
{
    enum class Severity : unsigned char
    {
        Error,
        Fatal
    };

    XRCORE_API void Initialize();
    XRCORE_API void SetEnabled(bool enabled);
    XRCORE_API bool IsEnabled();
    XRCORE_API LPCSTR GetDirectory();

    // Allocation-free crash context of the calling thread, safe on hot frame
    // paths. stage must be a string literal: only the pointer is stored.
    XRCORE_API void SetThreadStage(LPCSTR stage, const void* callbackAddress = nullptr);
    XRCORE_API void ClearThreadStage();
    XRCORE_API void SetThreadName(LPCSTR name);

    // Bounded in-memory history (game-log warnings, loading, saving). It is
    // printed in fatal and crash records and never touches the disk by itself.
    XRCORE_API void AddBreadcrumb(LPCSTR message);

    // Writes the running Lua call stack as "  #NN source:line (function)\r\n"
    // lines. Fatal and crash records call it on the failing thread, under SEH;
    // it writes nothing unless that thread may walk the VM.
    using LuaStackWriter = void (*)(char* output, size_t capacity);
    XRCORE_API void SetLuaStackWriter(LuaStackWriter writer);

    // The Lua stack of whatever script runs now, asked from another thread (hang records): the sampler
    // arms a hook, waits up to timeoutMs for the VM to reach it and names the thread it ran on.
    using LuaHangSampler = bool (*)(char* output, size_t capacity, unsigned timeoutMs, DWORD* threadId);
    XRCORE_API void SetLuaHangSampler(LuaHangSampler sampler);

    // The object the calling thread works on ("Object: wpn_pm1117 [wpn_pm] id 1117 (UpdateCL)").
    // Hot paths: the scope stores two pointers and a literal; the describer formats the object only
    // when a record is written, under SEH, and must not allocate or lock.
    using ObjectDescriber = void (*)(const void* object, char* output, size_t capacity);
    class XRCORE_API ObjectScope
    {
    public:
        ObjectScope(LPCSTR action, const void* object, ObjectDescriber describer);
        ~ObjectScope();
        ObjectScope(const ObjectScope&) = delete;
        ObjectScope& operator=(const ObjectScope&) = delete;
    private:
        LPCSTR previousAction_;
        const void* previousObject_;
        ObjectDescriber previousDescriber_;
    };

    // The "Game" line of fatal, crash and hang records.
    enum class Context : unsigned char { Level, Save };
    XRCORE_API void SetContext(Context item, LPCSTR value);

    // The freeze watchdog: threadId has not finished a frame for frozenMs (one record per freeze).
    XRCORE_API void ReportHang(DWORD threadId, unsigned frozenMs, bool loading);
    XRCORE_API void ReportHangEnd(unsigned frozenMs);
    // From inside a catch(...) that goes on (luabind turns a native fault into a Lua error, "Busy
    // Hands"): a hardware fault is recorded with the stack where it happened; thrown errors are skipped.
    XRCORE_API void ReportCaughtException(LPCSTR where);
    // Crash and fatal minidumps (CRASH-DUMP.dmp); -no_crash_dump turns them off.
    XRCORE_API void SetDumpsEnabled(bool enabled);

    XRCORE_API void Report(
        Severity severity,
        LPCSTR expression,
        LPCSTR description,
        LPCSTR argument0,
        LPCSTR argument1,
        LPCSTR file,
        int line,
        LPCSTR function
    );

    // Shared DbgHelp gate: callers must keep construction, walking and destruction
    // inside this gate. A crash never waits for a failed symbol worker.
    XRCORE_API bool TryBeginSymbolWork();
    XRCORE_API void EndSymbolWork();

    // Returns required UTF-8 byte count (excluding NUL). Does not allocate.
    XRCORE_API size_t SanitizeText(LPCSTR text, char* output, size_t capacity);

    class XRCORE_API FailureCodeScope
    {
    public:
        explicit FailureCodeScope(long result, long removedReason = 0, bool hasRemovedReason = false);
        ~FailureCodeScope();
        FailureCodeScope(const FailureCodeScope&) = delete;
        FailureCodeScope& operator=(const FailureCodeScope&) = delete;
    private:
        long previousResult_, previousRemovedReason_;
        bool previousValid_, previousHasRemovedReason_;
    };

    XRCORE_API void ReportRenderFailure(long result, long removedReason,
        LPCSTR expression, LPCSTR file, int line, LPCSTR function);
    // A protected module load can reject content and continue. Keep its normal
    // game-log output, but reserve ERROR files for critical/unknown failures.
    // OOM and failures outside this explicitly recoverable path remain visible.
    XRCORE_API bool ShouldReportScriptFailure(int status, bool recoveredModuleLoad,
        LPCSTR message);
    XRCORE_API void ReportScriptFailure(int status, LPCSTR script,
        LPCSTR message, LPCSTR traceback, LPCSTR file = nullptr, int line = 0,
        LPCSTR function = nullptr, bool recoveredModuleLoad = false);
    // Opens the ERROR folder in Explorer once per process, instead of an error dialog.
    XRCORE_API void OpenLogFolder();
    // A separate filename and explicit SIMULATED marker, never a live failure.
    XRCORE_API bool WriteSelfTest();
    XRCORE_API void ReportUnhandledException(void* exceptionPointers);
}

#define QUARK_DIAGNOSTIC_STAGE(stage) \
    ::EngineErrorLogger::SetThreadStage(stage)

#define QUARK_DIAGNOSTIC_STAGE_ADDRESS(stage, address) \
    ::EngineErrorLogger::SetThreadStage((stage), reinterpret_cast<const void*>(address))

#define QUARK_DIAGNOSTIC_CLEAR_STAGE() \
    ::EngineErrorLogger::ClearThreadStage()

#define QUARK_DIAGNOSTIC_BREADCRUMB(message) \
    ::EngineErrorLogger::AddBreadcrumb(message)

#endif // engine_error_loggerH
