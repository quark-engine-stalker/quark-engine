#ifndef engine_error_loggerH
#define engine_error_loggerH
#pragma once

namespace EngineErrorLogger
{
    enum class Severity : unsigned char
    {
        Error,
        Fatal,
        Crash
    };

    class XRCORE_API ExternalContentScope
    {
    public:
        ExternalContentScope();
        ~ExternalContentScope();

        ExternalContentScope(const ExternalContentScope&) = delete;
        ExternalContentScope& operator=(const ExternalContentScope&) = delete;
    };

    XRCORE_API void Initialize();
    XRCORE_API void SetEnabled(bool enabled);
    XRCORE_API bool IsEnabled();
    XRCORE_API LPCSTR GetDirectory();

    // Lightweight, allocation-free crash context. Calls only update thread-local
    // state and are safe to use on hot frame paths.
    XRCORE_API void SetThreadStage(
        LPCSTR stage,
        LPCSTR file,
        int line,
        LPCSTR function,
        const void* callbackAddress = nullptr
    );
    XRCORE_API void ClearThreadStage();
    XRCORE_API void SetFrameNumber(unsigned int frameNumber);

    // Bounded, non-blocking in-memory event history. It is appended to the category log on crash,
    // but does not touch the disk during normal gameplay. Native source paths are
    // normalized to repository-relative names before they enter persistent state.
    XRCORE_API void AddBreadcrumb(
        LPCSTR category,
        LPCSTR message,
        LPCSTR file,
        int line,
        LPCSTR function
    );

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
    // A separate filename and explicit SIMULATED marker, never a live failure.
    XRCORE_API bool WriteSelfTest();
    XRCORE_API void ReportUnhandledException(void* exceptionPointers);
    XRCORE_API void __cdecl ReportExplicit(LPCSTR file, int line, LPCSTR function, LPCSTR format, ...);

}

#define QUARK_ENGINE_ERROR(...) \
    ::EngineErrorLogger::ReportExplicit(__FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define QUARK_DIAGNOSTIC_STAGE(stage) \
    ::EngineErrorLogger::SetThreadStage((stage), __FILE__, __LINE__, __FUNCTION__, nullptr)

#define QUARK_DIAGNOSTIC_STAGE_ADDRESS(stage, address) \
    ::EngineErrorLogger::SetThreadStage((stage), __FILE__, __LINE__, __FUNCTION__, reinterpret_cast<const void*>(address))

#define QUARK_DIAGNOSTIC_CLEAR_STAGE() \
    ::EngineErrorLogger::ClearThreadStage()

#define QUARK_DIAGNOSTIC_BREADCRUMB(category, message) \
    ::EngineErrorLogger::AddBreadcrumb((category), (message), __FILE__, __LINE__, __FUNCTION__)

#endif // engine_error_loggerH
