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

    // Lock-free in-memory event history. It is appended to the category log on crash,
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
