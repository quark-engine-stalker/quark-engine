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
    // lines. It is registered on the Lua VM thread and called only on that
    // thread, under SEH, by fatal and crash records.
    using LuaStackWriter = void (*)(char* output, size_t capacity);
    XRCORE_API void SetLuaStackWriter(LuaStackWriter writer);

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
