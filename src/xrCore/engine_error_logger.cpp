#include "stdafx.h"
#pragma hdrstop

#include "engine_error_logger.h"

#include <Windows.h>
#include <ShlObj.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <winternl.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../3rd party/stackwalker/include/StackWalker.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace EngineErrorLogger
{
namespace
{
    constexpr unsigned long long kMaxLogSize = 16ull * 1024ull * 1024ull;
    constexpr USHORT kFatalStackFrameCount = 32;
    constexpr unsigned int kBreadcrumbCount = 96;
    constexpr unsigned int kDetailedStackFrameLimit = 96;

    enum class Category : u8
    {
        AI,
        Render,
        Physics,
        Sound,
        Network,
        Memory,
        Collision,
        Core,
        Game,
        Engine,
        Script,
        Fatal,
        Crash
    };

    enum class Origin : u8
    {
        EngineNative,
        EngineNativeDuringScriptContent,
        ScriptContent,
        ThirdPartyNative,
        Unknown
    };

    struct ThreadStageState
    {
        char stage[128] = {};
        char file[260] = {};
        char function[160] = {};
        int line = 0;
        const void* callbackAddress = nullptr;
        unsigned long long changedTick = 0;
    };

    struct __declspec(align(64)) BreadcrumbSlot
    {
        volatile LONG64 sequence = 0;
        unsigned long long tick = 0;
        DWORD threadId = 0;
        char category[32] = {};
        char message[160] = {};
        char file[160] = {};
        char function[128] = {};
        int line = 0;
    };

    struct CrashWorkspace
    {
        char crashId[96] = {};
        char faultModule[MAX_PATH] = {};
        char faultLocation[1536] = {};
        char primarySource[1024] = {};
        char primaryFunction[1024] = {};
        DWORD primaryLine = 0;
        char exceptionDetails[12288] = {};
        char stack[49152] = {};
        char stackScan[16384] = {};
        char breadcrumbs[32768] = {};
        char systemInfo[16384] = {};
        char modules[65536] = {};
    };

    volatile LONG g_enabled = 1;
    volatile LONG64 g_processStartTick = 0;
    volatile LONG64 g_breadcrumbSequence = 0;
    volatile LONG g_frameNumber = 0;
    volatile LONG g_crashWorkspaceInUse = 0;
    SRWLOCK g_writeLock = SRWLOCK_INIT;
    SRWLOCK g_symbolLock = SRWLOCK_INIT;
    char g_directory[MAX_PATH] = {};
    char g_recordBuffer[65536] = {};
    BreadcrumbSlot g_breadcrumbs[kBreadcrumbCount] = {};
    CrashWorkspace g_crashWorkspace = {};
    thread_local bool g_insideLogger = false;
    thread_local bool g_insideCrashHandler = false;
    thread_local unsigned int g_externalContentDepth = 0;
    thread_local ThreadStageState g_threadStage = {};
    thread_local LPCSTR g_threadStageSourceStage = nullptr;
    thread_local LPCSTR g_threadStageSourceFile = nullptr;
    thread_local LPCSTR g_threadStageSourceFunction = nullptr;
    thread_local int g_threadStageSourceLine = 0;

    bool ascii_equal_i(char left, char right)
    {
        if (left >= 'A' && left <= 'Z')
            left = char(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z')
            right = char(right - 'A' + 'a');
        return left == right;
    }

    LPCSTR find_i(LPCSTR text, LPCSTR token)
    {
        if (!text || !token || !*token)
            return nullptr;

        for (LPCSTR current = text; *current; ++current)
        {
            LPCSTR source = current;
            LPCSTR pattern = token;
            while (*source && *pattern && ascii_equal_i(*source, *pattern))
            {
                ++source;
                ++pattern;
            }
            if (!*pattern)
                return current;
        }
        return nullptr;
    }

    bool contains_i(LPCSTR text, LPCSTR token)
    {
        return find_i(text, token) != nullptr;
    }

    bool is_luajit_exception_code(DWORD code)
    {
        // LuaJIT/Windows uses 0xE24C4A00 | lua_error_code for its internal
        // SEH unwinding. If one reaches the process crash handler, KERNELBASE!
        // RaiseException is only the transport and must not be classified as a
        // third-party native fault.
        return (code & 0xffffff00u) == 0xe24c4a00u;
    }

    LPCSTR safe_text(LPCSTR text)
    {
        return text && *text ? text : "<none>";
    }

    void copy_text(char* destination, size_t capacity, LPCSTR source)
    {
        if (!destination || capacity == 0)
            return;
        if (!source)
            source = "";
        strncpy_s(destination, capacity, source, _TRUNCATE);
    }

    unsigned long long process_uptime_ms()
    {
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        LONG64 start = InterlockedCompareExchange64(&g_processStartTick, 0, 0);
        if (!start)
        {
            const LONG64 previous = InterlockedCompareExchange64(&g_processStartTick, now, 0);
            start = previous ? previous : now;
        }
        return static_cast<unsigned long long>(now - start);
    }

    LPCSTR path_basename(LPCSTR path)
    {
        if (!path || !*path)
            return "<unknown>";

        LPCSTR slash = strrchr(path, '\\');
        LPCSTR forwardSlash = strrchr(path, '/');
        if (!slash || (forwardSlash && forwardSlash > slash))
            slash = forwardSlash;
        return slash ? slash + 1 : path;
    }

    void format_source_path(LPCSTR path, char* output, size_t capacity)
    {
        if (!output || capacity == 0)
            return;
        if (!path || !*path)
        {
            copy_text(output, capacity, "<unknown>");
            return;
        }

        size_t written = 0;
        for (LPCSTR source = path; *source && written + 1 < capacity; ++source)
            output[written++] = *source == '\\' ? '/' : *source;
        output[written] = 0;

        LPCSTR relative = find_i(output, "/src/");
        if (relative)
        {
            relative += sizeof("/src/") - 1;
        }
        else if (find_i(output, "src/") == output)
        {
            relative = output + sizeof("src/") - 1;
        }
        else
        {
            static constexpr LPCSTR sourceRoots[] = {
                "xrCore/", "xrEngine/", "xrGame/", "xrServerEntities/",
                "xrPhysics/", "xrSound/", "xrCDB/", "xrNetServer/",
                "xrParticles/", "xrCPU_Pipe/", "Layers/", "Include/",
                "3rd party/", "sdk/"
            };

            for (LPCSTR root : sourceRoots)
            {
                for (LPCSTR candidate = find_i(output, root); candidate; candidate = find_i(candidate + 1, root))
                {
                    if (candidate == output || candidate[-1] == '/')
                    {
                        if (!relative || candidate < relative)
                            relative = candidate;
                        break;
                    }
                }
            }
        }

        const bool absolute = written >= 3 &&
            ((output[0] >= 'A' && output[0] <= 'Z') || (output[0] >= 'a' && output[0] <= 'z')) &&
            output[1] == ':' && output[2] == '/';
        const bool uncPath = written >= 2 && output[0] == '/' && output[1] == '/';
        if (!relative && (absolute || uncPath))
            relative = path_basename(output);
        if (!relative)
            relative = output;

        while (relative[0] == '.' && relative[1] == '/')
            relative += 2;
        while (relative[0] == '.' && relative[1] == '.' && relative[2] == '/')
            relative += 3;

        if (relative != output)
            memmove(output, relative, strlen(relative) + 1);
        if (!*output)
            copy_text(output, capacity, "<unknown>");
    }

    void append_format(char* buffer, size_t capacity, size_t& used, LPCSTR format, ...)
    {
        if (!buffer || used >= capacity - 1)
            return;

        va_list args;
        va_start(args, format);
        const int result = _vsnprintf_s(buffer + used, capacity - used, _TRUNCATE, format, args);
        va_end(args);

        if (result < 0)
            used = strlen(buffer);
        else
            used += size_t(result);
    }

    void append_single_line_text(char* buffer, size_t capacity, size_t& used, LPCSTR text)
    {
        if (!buffer || !text || used >= capacity - 1)
            return;

        bool pendingSpace = false;
        for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
            *cursor && used < capacity - 1; ++cursor)
        {
            const unsigned char value = *cursor;
            if (value < 0x20 || value == 0x7f)
            {
                pendingSpace = used != 0 && buffer[used - 1] != ' ';
                continue;
            }

            if (pendingSpace && used < capacity - 1)
                buffer[used++] = ' ';
            pendingSpace = false;
            buffer[used++] = static_cast<char>(value);
        }
        buffer[used] = 0;
    }

    void append_field(char* buffer, size_t capacity, size_t& used, LPCSTR label, LPCSTR value)
    {
        if (!value || !*value)
            return;
        append_format(buffer, capacity, used, "%-12s: ", label);
        append_single_line_text(buffer, capacity, used, value);
        append_format(buffer, capacity, used, "\r\n");
    }

    HMODULE module_from_address(const void* address, char* path, size_t pathCapacity, unsigned long long& offset);
    void format_address_location(const void* address, char* output, size_t capacity);

    bool ensure_subdirectory(LPCSTR path)
    {
        if (!path || !*path)
            return false;
        if (CreateDirectoryA(path, nullptr))
            return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }

    bool ensure_directory_unlocked()
    {
        if (*g_directory)
            return true;

        char appData[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableA("APPDATA", appData, DWORD(sizeof(appData)));
        if (!length || length >= sizeof(appData))
        {
            *appData = 0;
            if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, appData)))
                return false;
        }

        char engineDirectory[MAX_PATH] = {};
        if (_snprintf_s(engineDirectory, sizeof(engineDirectory), _TRUNCATE,
            "%s\\QUARK ENGINE", appData) < 0)
            return false;
        if (!ensure_subdirectory(engineDirectory))
            return false;

        if (_snprintf_s(g_directory, sizeof(g_directory), _TRUNCATE,
            "%s\\ERROR", engineDirectory) < 0)
        {
            *g_directory = 0;
            return false;
        }
        if (!ensure_subdirectory(g_directory))
        {
            *g_directory = 0;
            return false;
        }

        return true;
    }

    bool is_third_party_source(LPCSTR file)
    {
        if (!file || !*file)
            return false;

        return contains_i(file, "3rd party") ||
            contains_i(file, "third_party") ||
            contains_i(file, "\\luajit-") ||
            contains_i(file, "/luajit-") ||
            contains_i(file, "reshade");
    }

    bool contains_script_failure(
        LPCSTR expression,
        LPCSTR description,
        LPCSTR argument0,
        LPCSTR argument1
    )
    {
        LPCSTR values[] = { expression, description, argument0, argument1 };
        for (LPCSTR value : values)
        {
            if (contains_i(value, "lua error") ||
                contains_i(value, "[lua]") ||
                contains_i(value, "gamedata\\scripts") ||
                contains_i(value, "gamedata/scripts") ||
                contains_i(value, ".script:") ||
                contains_i(value, "luabind"))
                return true;
        }
        return false;
    }

    bool contains_memory_failure(LPCSTR expression, LPCSTR description, LPCSTR argument0, LPCSTR argument1)
    {
        return contains_i(expression, "out of memory") ||
            contains_i(description, "out of memory") ||
            contains_i(argument0, "out of memory") ||
            contains_i(argument1, "out of memory") ||
            contains_i(expression, "bad_alloc") ||
            contains_i(description, "bad_alloc") ||
            contains_i(argument0, "memory request");
    }

    Category classify_source(
        LPCSTR file,
        LPCSTR expression,
        LPCSTR description,
        LPCSTR argument0,
        LPCSTR argument1
    )
    {
        if (contains_memory_failure(expression, description, argument0, argument1))
            return Category::Memory;

        if (contains_i(file, "xrgame\\ai\\") || contains_i(file, "xrgame/ai/") ||
            contains_i(file, "ai_") || contains_i(file, "alife") ||
            contains_i(file, "stalker") || contains_i(file, "monster") ||
            contains_i(file, "path_manager") || contains_i(file, "movement_manager") ||
            contains_i(file, "graph_engine"))
            return Category::AI;

        if (contains_i(file, "xrrender") || contains_i(file, "\\layers\\") ||
            contains_i(file, "dx10") || contains_i(file, "dx11") ||
            contains_i(file, "d3d11") || contains_i(file, "shader") ||
            contains_i(file, "seqrender") || contains_i(file, "render/"))
            return Category::Render;

        if (contains_i(file, "xrphysics") || contains_i(file, "phworld") ||
            contains_i(file, "phobject") || contains_i(file, "phshell") ||
            contains_i(file, "physics"))
            return Category::Physics;

        if (contains_i(file, "xrsound") || contains_i(file, "soundrender") ||
            contains_i(file, "sound_"))
            return Category::Sound;

        if (contains_i(file, "xrnetserver") || contains_i(file, "gamespy") ||
            contains_i(file, "net_") || contains_i(file, "network"))
            return Category::Network;

        if (contains_i(file, "xrcdb") || contains_i(file, "cdb") ||
            contains_i(file, "collide"))
            return Category::Collision;

        if (contains_i(file, "xrmemory") || contains_i(file, "allocator") ||
            contains_i(file, "memory"))
            return Category::Memory;

        if (contains_i(file, "xrcore"))
            return Category::Core;

        if (contains_i(file, "xrgame") || contains_i(file, "xrserverentities") ||
            contains_i(file, "xrparticles"))
            return Category::Game;

        if (contains_i(file, "xrengine"))
            return Category::Engine;

        return Category::Engine;
    }

    LPCSTR category_name(Category category)
    {
        switch (category)
        {
        case Category::AI: return "AI";
        case Category::Render: return "RENDER";
        case Category::Physics: return "PHYSICS";
        case Category::Sound: return "SOUND";
        case Category::Network: return "NETWORK";
        case Category::Memory: return "MEMORY";
        case Category::Collision: return "COLLISION";
        case Category::Core: return "CORE";
        case Category::Game: return "GAME";
        case Category::Engine: return "ENGINE";
        case Category::Script: return "SCRIPT";
        case Category::Fatal: return "FATAL";
        case Category::Crash: return "CRASH";
        }
        return "ENGINE";
    }

    LPCSTR severity_name(Severity severity)
    {
        switch (severity)
        {
        case Severity::Error: return "ERROR";
        case Severity::Fatal: return "FATAL";
        case Severity::Crash: return "CRASH";
        }
        return "ERROR";
    }

    LPCSTR origin_name(Origin origin)
    {
        switch (origin)
        {
        case Origin::EngineNative: return "ENGINE_NATIVE";
        case Origin::EngineNativeDuringScriptContent: return "ENGINE_NATIVE_DURING_SCRIPT_OR_CONTENT";
        case Origin::ScriptContent: return "SCRIPT_OR_CONTENT";
        case Origin::ThirdPartyNative: return "THIRD_PARTY_NATIVE";
        case Origin::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    void truncate_if_needed(LPCSTR path)
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes))
            return;

        ULARGE_INTEGER size = {};
        size.HighPart = attributes.nFileSizeHigh;
        size.LowPart = attributes.nFileSizeLow;
        if (size.QuadPart < kMaxLogSize)
            return;

        HANDLE file = CreateFileA(path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;

        static constexpr char resetNotice[] =
            "============================================================\r\n"
            "QUARK ENGINE LOG RESET: previous content exceeded 16 MB.\r\n"
            "============================================================\r\n\r\n";
        DWORD bytesWritten = 0;
        WriteFile(file, resetNotice, DWORD(sizeof(resetNotice) - 1), &bytesWritten, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }

    void append_native_stack(char* record, size_t capacity, size_t& used)
    {
        void* frames[kFatalStackFrameCount] = {};
        const USHORT frameCount = CaptureStackBackTrace(3, kFatalStackFrameCount, frames, nullptr);
        if (!frameCount)
            return;

        append_format(record, capacity, used, "Native stack  :\r\n");
        for (USHORT index = 0; index < frameCount; ++index)
        {
            MEMORY_BASIC_INFORMATION memory = {};
            HMODULE module = nullptr;
            char modulePath[MAX_PATH] = {};
            unsigned long long offset = 0;

            if (VirtualQuery(frames[index], &memory, sizeof(memory)))
            {
                module = static_cast<HMODULE>(memory.AllocationBase);
                if (module)
                {
                    GetModuleFileNameA(module, modulePath, DWORD(sizeof(modulePath)));
                    offset = static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(frames[index]) - reinterpret_cast<uintptr_t>(module));
                }
            }

            if (*modulePath)
            {
                append_format(record, capacity, used, "  #%02u %s+0x%llX\r\n",
                    unsigned(index), path_basename(modulePath), offset);
            }
            else
            {
                append_format(record, capacity, used, "  #%02u [%p]\r\n", unsigned(index), frames[index]);
            }
        }
    }

    void write_emergency_record(
        Category category,
        Severity severity,
        Origin origin,
        LPCSTR expression,
        LPCSTR description,
        LPCSTR file,
        int line,
        LPCSTR function
    )
    {
        if (!*g_directory)
            return;

        char logPath[MAX_PATH] = {};
        _snprintf_s(logPath, sizeof(logPath), _TRUNCATE, "%s\\%s-LOG.log", g_directory, category_name(category));

        HANDLE fileHandle = CreateFileA(logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
            return;

        SYSTEMTIME localTime = {};
        GetLocalTime(&localTime);

        char record[4096] = {};
        char sourcePath[512] = {};
        char stageSource[512] = {};
        format_source_path(file, sourcePath, sizeof(sourcePath));
        format_source_path(g_threadStage.file, stageSource, sizeof(stageSource));

        size_t used = 0;
        append_format(record, sizeof(record), used,
            "------------------------------------------------------------\r\n"
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s / %s\r\n",
            localTime.wYear, localTime.wMonth, localTime.wDay,
            localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds,
            category_name(category), severity_name(severity));
        append_format(record, sizeof(record), used, "Origin      : %s\r\n", origin_name(origin));
        append_format(record, sizeof(record), used,
            "Context     : pid=%lu thread=%lu frame=%ld uptime=%llu ms\r\n",
            GetCurrentProcessId(), GetCurrentThreadId(),
            InterlockedCompareExchange(&g_frameNumber, 0, 0), process_uptime_ms());
        append_format(record, sizeof(record), used,
            "Logger      : emergency write; primary logger lock unavailable\r\n");
        if (*g_threadStage.stage)
            append_format(record, sizeof(record), used, "Stage       : %s @ %s:%d (%s)\r\n",
                g_threadStage.stage, stageSource, g_threadStage.line, safe_text(g_threadStage.function));
        append_field(record, sizeof(record), used, "Expression", expression);
        append_field(record, sizeof(record), used, "Message", description);
        append_format(record, sizeof(record), used, "Location    : %s:%d (%s)\r\n\r\n",
            sourcePath, line, safe_text(function));

        DWORD bytesWritten = 0;
        WriteFile(fileHandle, record, DWORD(used), &bytesWritten, nullptr);
        FlushFileBuffers(fileHandle);
        CloseHandle(fileHandle);
    }

    void write_record(
        Category category,
        Severity severity,
        Origin origin,
        LPCSTR expression,
        LPCSTR description,
        LPCSTR argument0,
        LPCSTR argument1,
        LPCSTR file,
        int line,
        LPCSTR function,
        LPCSTR module,
        LPCSTR extraDetails,
        bool includeNativeStack
    )
    {
        if (!IsEnabled() || g_insideLogger)
            return;

        const DWORD preservedLastError = GetLastError();
        g_insideLogger = true;

        const bool lockAcquired = severity == Severity::Crash ?
            TryAcquireSRWLockExclusive(&g_writeLock) != FALSE :
            (AcquireSRWLockExclusive(&g_writeLock), true);
        if (!lockAcquired)
        {
            write_emergency_record(category, severity, origin, expression, description, file, line, function);
            g_insideLogger = false;
            SetLastError(preservedLastError);
            return;
        }

        if (!ensure_directory_unlocked())
        {
            ReleaseSRWLockExclusive(&g_writeLock);
            g_insideLogger = false;
            SetLastError(preservedLastError);
            return;
        }

        char logPath[MAX_PATH] = {};
        _snprintf_s(logPath, sizeof(logPath), _TRUNCATE, "%s\\%s-LOG.log", g_directory, category_name(category));
        truncate_if_needed(logPath);

        HANDLE fileHandle = CreateFileA(logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);

        if (fileHandle != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME localTime = {};
            GetLocalTime(&localTime);

            char sourcePath[512] = {};
            char stageSource[512] = {};
            char callbackLocation[512] = {};
            format_source_path(file, sourcePath, sizeof(sourcePath));
            format_source_path(g_threadStage.file, stageSource, sizeof(stageSource));
            format_address_location(g_threadStage.callbackAddress, callbackLocation, sizeof(callbackLocation));

            ZeroMemory(g_recordBuffer, sizeof(g_recordBuffer));
            size_t used = 0;
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "------------------------------------------------------------\r\n");
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used,
                "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s / %s\r\n",
                localTime.wYear, localTime.wMonth, localTime.wDay,
                localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds,
                category_name(category), severity_name(severity));
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "Origin      : %s\r\n", origin_name(origin));
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used,
                "Context     : pid=%lu thread=%lu frame=%ld uptime=%llu ms\r\n",
                GetCurrentProcessId(), GetCurrentThreadId(),
                InterlockedCompareExchange(&g_frameNumber, 0, 0), process_uptime_ms());
            if (*g_threadStage.stage)
            {
                append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "Stage       : %s @ %s:%d (%s)\r\n",
                    g_threadStage.stage, stageSource, g_threadStage.line, safe_text(g_threadStage.function));
                if (g_threadStage.callbackAddress)
                    append_format(g_recordBuffer, sizeof(g_recordBuffer), used,
                        "Callback    : %s\r\n", callbackLocation);
            }
            if (module && *module)
                append_format(g_recordBuffer, sizeof(g_recordBuffer), used,
                    "Module      : %s\r\n", path_basename(module));
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Expression", expression);
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Message", description);
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Details", argument0);
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Context info", argument1);
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "Location    : %s:%d (%s)\r\n",
                sourcePath, line, safe_text(function));
            if (extraDetails && *extraDetails)
                append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "Crash diagnostics:\r\n%s\r\n", extraDetails);
            if (includeNativeStack)
                append_native_stack(g_recordBuffer, sizeof(g_recordBuffer), used);
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "\r\n");

            DWORD bytesWritten = 0;
            WriteFile(fileHandle, g_recordBuffer, DWORD(used), &bytesWritten, nullptr);
            FlushFileBuffers(fileHandle);
            CloseHandle(fileHandle);
        }

        ReleaseSRWLockExclusive(&g_writeLock);
        g_insideLogger = false;
        SetLastError(preservedLastError);
    }

    LPCSTR exception_name(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "floating-point denormal operand";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point divide by zero";
        case EXCEPTION_FLT_INEXACT_RESULT: return "floating-point inexact result";
        case EXCEPTION_FLT_INVALID_OPERATION: return "invalid floating-point operation";
        case EXCEPTION_FLT_OVERFLOW: return "floating-point overflow";
        case EXCEPTION_FLT_STACK_CHECK: return "floating-point stack check";
        case EXCEPTION_FLT_UNDERFLOW: return "floating-point underflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_INT_OVERFLOW: return "integer overflow";
        case EXCEPTION_INVALID_DISPOSITION: return "invalid exception disposition";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "non-continuable exception";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_SINGLE_STEP: return "single step";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        default: return "unhandled structured exception";
        }
    }

    bool is_engine_module(HMODULE module, LPCSTR modulePath)
    {
        if (!module)
            return false;

        if (module == GetModuleHandleA(nullptr))
            return true;

        return contains_i(modulePath, "xrcore") || contains_i(modulePath, "xrengine") ||
            contains_i(modulePath, "xrgame") || contains_i(modulePath, "xrrender") ||
            contains_i(modulePath, "xrphysics") || contains_i(modulePath, "xrcdb") ||
            contains_i(modulePath, "xrsound") || contains_i(modulePath, "xrnetserver") ||
            contains_i(modulePath, "xrserverentities") || contains_i(modulePath, "xrcpu_pipe") ||
            contains_i(modulePath, "xrparticles");
    }

    HMODULE module_from_address(const void* address, char* path, size_t pathCapacity, unsigned long long& offset)
    {
        if (path && pathCapacity)
            *path = 0;
        offset = 0;
        if (!address)
            return nullptr;

        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(address, &memory, sizeof(memory)))
            return nullptr;

        HMODULE module = static_cast<HMODULE>(memory.AllocationBase);
        if (!module)
            return nullptr;

        if (path && pathCapacity)
            GetModuleFileNameA(module, path, DWORD(pathCapacity));
        offset = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
            reinterpret_cast<uintptr_t>(module));
        return module;
    }

    void format_address_location(const void* address, char* output, size_t capacity)
    {
        if (!output || capacity == 0)
            return;
        *output = 0;
        if (!address)
            return;

        char modulePath[MAX_PATH] = {};
        unsigned long long offset = 0;
        if (module_from_address(address, modulePath, sizeof(modulePath), offset) && *modulePath)
            _snprintf_s(output, capacity, _TRUNCATE, "%s+0x%llX", path_basename(modulePath), offset);
        else
            _snprintf_s(output, capacity, _TRUNCATE, "%p", address);
    }

    void format_thread_name(char* output, size_t capacity)
    {
        if (!output || capacity == 0)
            return;
        *output = 0;

        using get_thread_description_fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel)
            return;
        auto getThreadDescription = reinterpret_cast<get_thread_description_fn>(
            GetProcAddress(kernel, "GetThreadDescription"));
        if (!getThreadDescription)
            return;

        PWSTR description = nullptr;
        if (SUCCEEDED(getThreadDescription(GetCurrentThread(), &description)) && description)
        {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, output, int(capacity), nullptr, nullptr);
            LocalFree(description);
        }
    }

    class CrashStackWalker final : public StackWalker
    {
    public:
        CrashStackWalker(char* output, size_t capacity, size_t& used,
            char* primarySource, size_t primarySourceCapacity,
            char* primaryFunction, size_t primaryFunctionCapacity, DWORD& primaryLine)
            : StackWalker(StackWalker::RetrieveSymbol | StackWalker::RetrieveLine |
                StackWalker::RetrieveModuleInfo | StackWalker::SymBuildPath,
                nullptr, GetCurrentProcessId(), GetCurrentProcess()),
              output_(output), capacity_(capacity), used_(used),
              primarySource_(primarySource), primarySourceCapacity_(primarySourceCapacity),
              primaryFunction_(primaryFunction), primaryFunctionCapacity_(primaryFunctionCapacity),
              primaryLine_(primaryLine)
        {
        }

    protected:
        void OnCallstackEntry(CallstackEntryType entryType, CallstackEntry& entry) override
        {
            if (entryType == lastEntry || entry.offset == 0 || frameIndex_ >= kDetailedStackFrameLimit)
                return;

            LPCSTR moduleName = *entry.moduleName ? path_basename(entry.moduleName) : path_basename(entry.loadedImageName);
            LPCSTR symbolName = *entry.undFullName ? entry.undFullName :
                (*entry.undName ? entry.undName : (*entry.name ? entry.name : "<symbol unavailable>"));
            const unsigned long long moduleOffset = entry.baseOfImage && entry.offset >= entry.baseOfImage ?
                static_cast<unsigned long long>(entry.offset - entry.baseOfImage) : 0;

            append_format(output_, capacity_, used_, "  #%02u %s+0x%llX!%s+0x%llX",
                frameIndex_, safe_text(moduleName), moduleOffset, safe_text(symbolName),
                static_cast<unsigned long long>(entry.offsetFromSmybol));
            if (*entry.lineFileName && entry.lineNumber)
            {
                char sourcePath[1024] = {};
                format_source_path(entry.lineFileName, sourcePath, sizeof(sourcePath));
                append_format(output_, capacity_, used_, " (%s:%lu", sourcePath, entry.lineNumber);
                if (entry.offsetFromLine)
                    append_format(output_, capacity_, used_, "+0x%lX", entry.offsetFromLine);
                append_format(output_, capacity_, used_, ")");
            }
            append_format(output_, capacity_, used_, "\r\n");

            if (!capturedPrimary_)
            {
                capturedPrimary_ = true;
                copy_text(primaryFunction_, primaryFunctionCapacity_, symbolName);
                if (*entry.lineFileName)
                    format_source_path(entry.lineFileName, primarySource_, primarySourceCapacity_);
                primaryLine_ = entry.lineNumber;
            }
            ++frameIndex_;
        }

        void OnDbgHelpErr(LPCSTR, DWORD, DWORD64) override
        {
        }

        void OnOutput(LPCSTR) override
        {
        }

    public:
        unsigned int frame_count() const
        {
            return frameIndex_;
        }

    private:
        char* output_ = nullptr;
        size_t capacity_ = 0;
        size_t& used_;
        char* primarySource_ = nullptr;
        size_t primarySourceCapacity_ = 0;
        char* primaryFunction_ = nullptr;
        size_t primaryFunctionCapacity_ = 0;
        DWORD& primaryLine_;
        unsigned int frameIndex_ = 0;
        bool capturedPrimary_ = false;
    };

    bool append_symbolized_crash_stack(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
    {
        size_t used = 0;
        append_format(workspace.stack, sizeof(workspace.stack), used,
            "Symbolized crash stack (faulting context):\r\n");

        if (!pointers || !pointers->ContextRecord)
        {
            append_format(workspace.stack, sizeof(workspace.stack), used,
                "  Context record is unavailable.\r\n");
            return false;
        }

        if (!TryAcquireSRWLockExclusive(&g_symbolLock))
        {
            append_format(workspace.stack, sizeof(workspace.stack), used,
                "  Symbol engine lock is unavailable; another thread may have failed inside DbgHelp.\r\n");
            return false;
        }

        CONTEXT context = *pointers->ContextRecord;
        CrashStackWalker walker(workspace.stack, sizeof(workspace.stack), used,
            workspace.primarySource, sizeof(workspace.primarySource),
            workspace.primaryFunction, sizeof(workspace.primaryFunction), workspace.primaryLine);
        const BOOL result = walker.ShowCallstack(GetCurrentThread(), &context);
        if (!result)
        {
            append_format(workspace.stack, sizeof(workspace.stack), used,
                "  StackWalker could not unwind this context. Use the matching PDB and minidump.\r\n");
        }
        const bool hasFrames = walker.frame_count() != 0;
        ReleaseSRWLockExclusive(&g_symbolLock);
        return hasFrames;
    }

    bool is_executable_protection(DWORD protection)
    {
        const DWORD base = protection & 0xff;
        return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
            base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    }

    void append_stack_scan(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
    {
        size_t used = 0;
        append_format(workspace.stackScan, sizeof(workspace.stackScan), used,
            "Raw stack return-address candidates (fallback):\r\n");

        if (!pointers || !pointers->ContextRecord)
        {
            append_format(workspace.stackScan, sizeof(workspace.stackScan), used, "  <context unavailable>\r\n");
            return;
        }

#if defined(_M_X64)
        const uintptr_t stackPointer = static_cast<uintptr_t>(pointers->ContextRecord->Rsp);
#elif defined(_M_IX86)
        const uintptr_t stackPointer = static_cast<uintptr_t>(pointers->ContextRecord->Esp);
#else
        const uintptr_t stackPointer = 0;
#endif
        if (!stackPointer)
        {
            append_format(workspace.stackScan, sizeof(workspace.stackScan), used, "  <stack pointer unavailable>\r\n");
            return;
        }

        MEMORY_BASIC_INFORMATION stackMemory = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(stackPointer), &stackMemory, sizeof(stackMemory)) ||
            stackMemory.State != MEM_COMMIT)
        {
            append_format(workspace.stackScan, sizeof(workspace.stackScan), used, "  <stack memory is not readable>\r\n");
            return;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(stackMemory.BaseAddress) + stackMemory.RegionSize;
        const size_t availableWords = regionEnd > stackPointer ?
            (regionEnd - stackPointer) / sizeof(uintptr_t) : 0;
        const size_t wordsToScan = availableWords < 192 ? availableWords : 192;
        unsigned int found = 0;

        __try
        {
            const uintptr_t* words = reinterpret_cast<const uintptr_t*>(stackPointer);
            for (size_t index = 0; index < wordsToScan && found < 40; ++index)
            {
                const uintptr_t candidate = words[index];
                if (!candidate)
                    continue;

                MEMORY_BASIC_INFORMATION candidateMemory = {};
                if (!VirtualQuery(reinterpret_cast<const void*>(candidate), &candidateMemory, sizeof(candidateMemory)) ||
                    candidateMemory.State != MEM_COMMIT || !is_executable_protection(candidateMemory.Protect))
                    continue;

                char modulePath[MAX_PATH] = {};
                unsigned long long moduleOffset = 0;
                module_from_address(reinterpret_cast<const void*>(candidate), modulePath, sizeof(modulePath), moduleOffset);
                append_format(workspace.stackScan, sizeof(workspace.stackScan), used,
                    "  RSP+0x%04llX -> %s+0x%llX\r\n",
                    static_cast<unsigned long long>(index * sizeof(uintptr_t)),
                    *modulePath ? path_basename(modulePath) : "<unknown module>", moduleOffset);
                ++found;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            append_format(workspace.stackScan, sizeof(workspace.stackScan), used,
                "  <stack scan stopped by memory fault>\r\n");
        }

        if (!found)
            append_format(workspace.stackScan, sizeof(workspace.stackScan), used, "  <no executable candidates found>\r\n");
    }

    void append_instruction_bytes(const void* address, char* output, size_t capacity, size_t& used)
    {
        append_format(output, capacity, used, "Instruction bytes: ");
        if (!address)
        {
            append_format(output, capacity, used, "<unavailable>\r\n");
            return;
        }

        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT)
        {
            append_format(output, capacity, used, "<unreadable>\r\n");
            return;
        }

        __try
        {
            const unsigned char* bytes = static_cast<const unsigned char*>(address);
            for (unsigned int index = 0; index < 24; ++index)
                append_format(output, capacity, used, "%02X ", bytes[index]);
            append_format(output, capacity, used, "\r\n");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            append_format(output, capacity, used, "<read fault>\r\n");
        }
    }

    void append_exception_details(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
    {
        size_t used = 0;
        if (!pointers || !pointers->ExceptionRecord)
            return;

        EXCEPTION_RECORD* record = pointers->ExceptionRecord;
        append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
            "  Code          : 0x%08lX\r\n"
            "  Flags         : 0x%08lX\r\n"
            "  Address       : %p\r\n"
            "  Parameters    : %lu\r\n",
            record->ExceptionCode, record->ExceptionFlags, record->ExceptionAddress,
            record->NumberParameters);

        const DWORD parameterCount = record->NumberParameters < EXCEPTION_MAXIMUM_PARAMETERS ?
            record->NumberParameters : EXCEPTION_MAXIMUM_PARAMETERS;
        for (DWORD index = 0; index < parameterCount; ++index)
        {
            append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                "  Parameter[%lu] : 0x%p\r\n", index,
                reinterpret_cast<void*>(record->ExceptionInformation[index]));
        }

        if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2)
        {
            const uintptr_t target = static_cast<uintptr_t>(record->ExceptionInformation[1]);
            if (target < 0x10000)
            {
                append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                    "  Access pattern: near-null address 0x%llX; probable null object/member access.\r\n",
                    static_cast<unsigned long long>(target));
            }
        }
        if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && record->NumberParameters >= 3)
        {
            append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                "  In-page status: 0x%08llX\r\n",
                static_cast<unsigned long long>(record->ExceptionInformation[2]));
        }

        if (pointers->ContextRecord)
        {
#if defined(_M_X64)
            const CONTEXT& context = *pointers->ContextRecord;
            append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                "  RIP           : 0x%016llX\r\n"
                "  RSP           : 0x%016llX\r\n"
                "  RBP           : 0x%016llX\r\n"
                "  RAX           : 0x%016llX\r\n"
                "  RBX           : 0x%016llX\r\n"
                "  RCX           : 0x%016llX\r\n"
                "  RDX           : 0x%016llX\r\n"
                "  RSI           : 0x%016llX\r\n"
                "  RDI           : 0x%016llX\r\n"
                "  R8            : 0x%016llX\r\n"
                "  R9            : 0x%016llX\r\n"
                "  R10           : 0x%016llX\r\n"
                "  R11           : 0x%016llX\r\n"
                "  R12           : 0x%016llX\r\n"
                "  R13           : 0x%016llX\r\n"
                "  R14           : 0x%016llX\r\n"
                "  R15           : 0x%016llX\r\n",
                context.Rip, context.Rsp, context.Rbp, context.Rax, context.Rbx,
                context.Rcx, context.Rdx, context.Rsi, context.Rdi, context.R8,
                context.R9, context.R10, context.R11, context.R12, context.R13,
                context.R14, context.R15);
#elif defined(_M_IX86)
            const CONTEXT& context = *pointers->ContextRecord;
            append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                "  EIP           : 0x%08lX\r\n"
                "  ESP           : 0x%08lX\r\n"
                "  EBP           : 0x%08lX\r\n"
                "  EAX           : 0x%08lX\r\n"
                "  EBX           : 0x%08lX\r\n"
                "  ECX           : 0x%08lX\r\n"
                "  EDX           : 0x%08lX\r\n"
                "  ESI           : 0x%08lX\r\n"
                "  EDI           : 0x%08lX\r\n",
                context.Eip, context.Esp, context.Ebp, context.Eax, context.Ebx,
                context.Ecx, context.Edx, context.Esi, context.Edi);
#endif
        }
        append_instruction_bytes(record->ExceptionAddress, workspace.exceptionDetails,
            sizeof(workspace.exceptionDetails), used);
    }

    void append_breadcrumb_history(CrashWorkspace& workspace)
    {
        size_t used = 0;
        append_format(workspace.breadcrumbs, sizeof(workspace.breadcrumbs), used,
            "Recent engine breadcrumbs (oldest to newest):\r\n");

        const LONG64 newest = InterlockedCompareExchange64(&g_breadcrumbSequence, 0, 0);
        const LONG64 oldest = newest > LONG64(kBreadcrumbCount) ? newest - LONG64(kBreadcrumbCount) + 1 : 1;
        unsigned int written = 0;

        for (LONG64 sequence = oldest; sequence <= newest; ++sequence)
        {
            BreadcrumbSlot& slot = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
            const LONG64 before = InterlockedCompareExchange64(&slot.sequence, 0, 0);
            if (before != sequence)
                continue;

            const unsigned long long tick = slot.tick;
            const DWORD threadId = slot.threadId;
            const int line = slot.line;
            char category[sizeof(slot.category)] = {};
            char message[sizeof(slot.message)] = {};
            char file[sizeof(slot.file)] = {};
            char function[sizeof(slot.function)] = {};
            copy_text(category, sizeof(category), slot.category);
            copy_text(message, sizeof(message), slot.message);
            copy_text(file, sizeof(file), slot.file);
            copy_text(function, sizeof(function), slot.function);
            MemoryBarrier();
            if (InterlockedCompareExchange64(&slot.sequence, 0, 0) != sequence)
                continue;

            const LONG64 start = InterlockedCompareExchange64(&g_processStartTick, 0, 0);
            const unsigned long long relative = start && tick >= static_cast<unsigned long long>(start) ?
                tick - static_cast<unsigned long long>(start) : 0;
            append_format(workspace.breadcrumbs, sizeof(workspace.breadcrumbs), used,
                "  +%10llu ms [T%lu] %-12s %s | %s:%d (%s)\r\n",
                relative, threadId, safe_text(category), safe_text(message),
                safe_text(file), line, safe_text(function));
            ++written;
        }

        if (!written)
            append_format(workspace.breadcrumbs, sizeof(workspace.breadcrumbs), used, "  <none recorded>\r\n");
    }

    void append_system_info(CrashWorkspace& workspace)
    {
        size_t used = 0;
        char threadName[256] = {};
        format_thread_name(threadName, sizeof(threadName));

        append_format(workspace.systemInfo, sizeof(workspace.systemInfo), used,
            "Runtime environment:\r\n");
        if (*threadName)
            append_format(workspace.systemInfo, sizeof(workspace.systemInfo), used,
                "  Thread name    : %s\r\n", threadName);
        append_format(workspace.systemInfo, sizeof(workspace.systemInfo), used,
            "  Debugger       : %s\r\n"
            "  Logger build   : %s %s\r\n",
            IsDebuggerPresent() ? "attached" : "not attached", __DATE__, __TIME__);

        MEMORYSTATUSEX memoryStatus = {};
        memoryStatus.dwLength = sizeof(memoryStatus);
        if (GlobalMemoryStatusEx(&memoryStatus))
        {
            append_format(workspace.systemInfo, sizeof(workspace.systemInfo), used,
                "  Physical RAM   : %llu MB total / %llu MB available\r\n"
                "  Memory load    : %lu%%\r\n",
                memoryStatus.ullTotalPhys / (1024ull * 1024ull),
                memoryStatus.ullAvailPhys / (1024ull * 1024ull), memoryStatus.dwMemoryLoad);
        }

        PROCESS_MEMORY_COUNTERS_EX counters = {};
        if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        {
            append_format(workspace.systemInfo, sizeof(workspace.systemInfo), used,
                "  Working set    : %llu MB\r\n"
                "  Private bytes  : %llu MB\r\n"
                "  Page faults    : %lu\r\n",
                static_cast<unsigned long long>(counters.WorkingSetSize) / (1024ull * 1024ull),
                static_cast<unsigned long long>(counters.PrivateUsage) / (1024ull * 1024ull),
                counters.PageFaultCount);
        }

        SYSTEM_INFO system = {};
        GetNativeSystemInfo(&system);
        append_format(workspace.systemInfo, sizeof(workspace.systemInfo), used,
            "  Logical CPUs   : %lu\r\n", system.dwNumberOfProcessors);

        using rtl_get_version_fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto rtlGetVersion = ntdll ? reinterpret_cast<rtl_get_version_fn>(
            GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        if (rtlGetVersion)
        {
            RTL_OSVERSIONINFOW version = {};
            version.dwOSVersionInfoSize = sizeof(version);
            if (rtlGetVersion(&version) == 0)
            {
                append_format(workspace.systemInfo, sizeof(workspace.systemInfo), used,
                    "  Windows        : %lu.%lu build %lu\r\n",
                    version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
            }
        }
    }

    void append_loaded_modules(CrashWorkspace& workspace)
    {
        size_t used = 0;
        append_format(workspace.modules, sizeof(workspace.modules), used,
            "Loaded modules (base, end, size, name):\r\n");

        HMODULE modules[512] = {};
        DWORD bytesNeeded = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytesNeeded))
        {
            append_format(workspace.modules, sizeof(workspace.modules), used,
                "  EnumProcessModules failed: %lu\r\n", GetLastError());
            return;
        }

        const DWORD discoveredModules = bytesNeeded / sizeof(HMODULE);
        const DWORD moduleCount = discoveredModules < DWORD(_countof(modules)) ?
            discoveredModules : DWORD(_countof(modules));
        for (DWORD index = 0; index < moduleCount; ++index)
        {
            MODULEINFO info = {};
            char path[MAX_PATH] = {};
            GetModuleInformation(GetCurrentProcess(), modules[index], &info, sizeof(info));
            GetModuleFileNameExA(GetCurrentProcess(), modules[index], path, DWORD(sizeof(path)));
            const uintptr_t base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
            append_format(workspace.modules, sizeof(workspace.modules), used,
                "  %p - %p  %8lu KB  %s\r\n",
                info.lpBaseOfDll, reinterpret_cast<void*>(base + info.SizeOfImage),
                info.SizeOfImage / 1024, *path ? path_basename(path) : "<unknown>");
        }
        if (bytesNeeded > sizeof(modules))
            append_format(workspace.modules, sizeof(workspace.modules), used,
                "  <module list truncated: %lu bytes required>\r\n", bytesNeeded);
    }

    void make_crash_id(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
    {
        SYSTEMTIME localTime = {};
        GetLocalTime(&localTime);
        const DWORD code = pointers && pointers->ExceptionRecord ?
            pointers->ExceptionRecord->ExceptionCode : 0;

        _snprintf_s(workspace.crashId, sizeof(workspace.crashId), _TRUNCATE,
            "%04u%02u%02u-%02u%02u%02u-%03u-P%lu-T%lu-C%08lX",
            localTime.wYear, localTime.wMonth, localTime.wDay,
            localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds,
            GetCurrentProcessId(), GetCurrentThreadId(), code);
    }

    void write_handle_text(HANDLE file, LPCSTR text)
    {
        if (file == INVALID_HANDLE_VALUE || !text || !*text)
            return;

        const char* cursor = text;
        size_t remaining = strlen(text);
        while (remaining)
        {
            const DWORD chunk = remaining > MAXDWORD ? MAXDWORD : static_cast<DWORD>(remaining);
            DWORD bytesWritten = 0;
            if (!WriteFile(file, cursor, chunk, &bytesWritten, nullptr) || bytesWritten == 0)
                break;
            cursor += bytesWritten;
            remaining -= bytesWritten;
        }
    }

    void write_crash_record(
        Category category,
        Origin origin,
        LPCSTR expression,
        LPCSTR description,
        CrashWorkspace& workspace)
    {
        if (g_insideLogger)
            return;

        const DWORD preservedLastError = GetLastError();
        g_insideLogger = true;

        if (!TryAcquireSRWLockExclusive(&g_writeLock))
        {
            write_emergency_record(category, Severity::Crash, origin,
                expression, description,
                *workspace.primarySource ? workspace.primarySource : workspace.faultModule,
                static_cast<int>(workspace.primaryLine),
                *workspace.primaryFunction ? workspace.primaryFunction : "UnhandledFilter");
            g_insideLogger = false;
            SetLastError(preservedLastError);
            return;
        }

        if (!ensure_directory_unlocked())
        {
            ReleaseSRWLockExclusive(&g_writeLock);
            g_insideLogger = false;
            SetLastError(preservedLastError);
            return;
        }

        char logPath[MAX_PATH] = {};
        _snprintf_s(logPath, sizeof(logPath), _TRUNCATE,
            "%s\\%s-LOG.log", g_directory, category_name(category));
        truncate_if_needed(logPath);

        HANDLE file = CreateFileA(logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME localTime = {};
            GetLocalTime(&localTime);

            char processPath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, processPath, DWORD(sizeof(processPath)));
            char stageSource[512] = {};
            char callbackLocation[512] = {};
            format_source_path(g_threadStage.file, stageSource, sizeof(stageSource));
            format_address_location(g_threadStage.callbackAddress, callbackLocation, sizeof(callbackLocation));

            char header[12288] = {};
            size_t used = 0;
            append_format(header, sizeof(header), used,
                "============================================================\r\n"
                "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s / CRASH\r\n"
                "Crash ID      : %s\r\n"
                "Origin        : %s\r\n"
                "Process       : %s (pid=%lu)\r\n"
                "Context       : thread=%lu frame=%ld uptime=%llu ms\r\n",
                localTime.wYear, localTime.wMonth, localTime.wDay,
                localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds,
                category_name(category), workspace.crashId, origin_name(origin),
                *processPath ? path_basename(processPath) : "<unknown>", GetCurrentProcessId(), GetCurrentThreadId(),
                InterlockedCompareExchange(&g_frameNumber, 0, 0), process_uptime_ms());
            append_field(header, sizeof(header), used, "Message", description);
            append_format(header, sizeof(header), used,
                "Fault location: %s\r\n",
                safe_text(workspace.faultLocation));

            if (*workspace.primarySource)
                append_format(header, sizeof(header), used,
                    "Top source     : %s:%lu\r\n", workspace.primarySource, workspace.primaryLine);
            if (*g_threadStage.stage)
            {
                append_format(header, sizeof(header), used,
                    "Thread stage   : %s\r\n", g_threadStage.stage);
                append_format(header, sizeof(header), used,
                    "Stage source   : %s:%d (%s)\r\n",
                    stageSource, g_threadStage.line,
                    safe_text(g_threadStage.function));
                if (g_threadStage.callbackAddress)
                    append_format(header, sizeof(header), used,
                        "Stage callback : %s\r\n", callbackLocation);
            }
            append_format(header, sizeof(header), used,
                "Matching PDB  : keep the PDB from this exact build next to the EXE.\r\n"
                "============================================================\r\n\r\n");

            write_handle_text(file, header);
            write_handle_text(file, "Exception and registers:\r\n");
            write_handle_text(file, workspace.exceptionDetails);
            write_handle_text(file, "\r\n");
            write_handle_text(file, workspace.stack);
            if (*workspace.stackScan)
            {
                write_handle_text(file, "\r\n");
                write_handle_text(file, workspace.stackScan);
            }
            write_handle_text(file, "\r\n");
            write_handle_text(file, workspace.breadcrumbs);
            write_handle_text(file, "\r\n");
            write_handle_text(file, workspace.systemInfo);
            write_handle_text(file, "\r\n");
            write_handle_text(file, workspace.modules);
            write_handle_text(file, "\r\n");
            FlushFileBuffers(file);
            CloseHandle(file);
        }

        ReleaseSRWLockExclusive(&g_writeLock);
        g_insideLogger = false;
        SetLastError(preservedLastError);
    }

}

ExternalContentScope::ExternalContentScope()
{
    ++g_externalContentDepth;
}

ExternalContentScope::~ExternalContentScope()
{
    if (g_externalContentDepth)
        --g_externalContentDepth;
}

void Initialize()
{
    process_uptime_ms();
    AcquireSRWLockExclusive(&g_writeLock);
    ensure_directory_unlocked();
    ReleaseSRWLockExclusive(&g_writeLock);
}

void SetEnabled(bool enabled)
{
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    if (enabled)
        Initialize();
}

bool IsEnabled()
{
    return InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
}

LPCSTR GetDirectory()
{
    Initialize();
    return *g_directory ? g_directory : "<unavailable>";
}

void SetThreadStage(LPCSTR stage, LPCSTR file, int line, LPCSTR function, const void* callbackAddress)
{
    if (!IsEnabled())
        return;

    // Hot job callbacks repeatedly publish the same literal source location.
    // Keep the already-copied strings and update only volatile crash context.
    if (stage == g_threadStageSourceStage && file == g_threadStageSourceFile &&
        function == g_threadStageSourceFunction && line == g_threadStageSourceLine)
    {
        g_threadStage.callbackAddress = callbackAddress;
        g_threadStage.changedTick = GetTickCount64();
        return;
    }

    copy_text(g_threadStage.stage, sizeof(g_threadStage.stage), stage);
    format_source_path(file, g_threadStage.file, sizeof(g_threadStage.file));
    copy_text(g_threadStage.function, sizeof(g_threadStage.function), function);
    g_threadStage.line = line;
    g_threadStage.callbackAddress = callbackAddress;
    g_threadStage.changedTick = GetTickCount64();
    g_threadStageSourceStage = stage;
    g_threadStageSourceFile = file;
    g_threadStageSourceFunction = function;
    g_threadStageSourceLine = line;
}

void ClearThreadStage()
{
    ZeroMemory(&g_threadStage, sizeof(g_threadStage));
    g_threadStageSourceStage = nullptr;
    g_threadStageSourceFile = nullptr;
    g_threadStageSourceFunction = nullptr;
    g_threadStageSourceLine = 0;
}

void SetFrameNumber(unsigned int frameNumber)
{
    InterlockedExchange(&g_frameNumber, static_cast<LONG>(frameNumber));
}

void AddBreadcrumb(LPCSTR category, LPCSTR message, LPCSTR file, int line, LPCSTR function)
{
    if (!IsEnabled())
        return;

    const LONG64 sequence = InterlockedIncrement64(&g_breadcrumbSequence);
    BreadcrumbSlot& slot = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
    InterlockedExchange64(&slot.sequence, -sequence);
    slot.tick = GetTickCount64();
    slot.threadId = GetCurrentThreadId();
    copy_text(slot.category, sizeof(slot.category), category);
    copy_text(slot.message, sizeof(slot.message), message);
    format_source_path(file, slot.file, sizeof(slot.file));
    copy_text(slot.function, sizeof(slot.function), function);
    slot.line = line;
    MemoryBarrier();
    InterlockedExchange64(&slot.sequence, sequence);
}

void Report(
    Severity severity,
    LPCSTR expression,
    LPCSTR description,
    LPCSTR argument0,
    LPCSTR argument1,
    LPCSTR file,
    int line,
    LPCSTR function
)
{
    if (!IsEnabled())
        return;

    const bool memoryFailure = contains_memory_failure(expression, description, argument0, argument1);
    const bool scriptContent = contains_script_failure(expression, description, argument0, argument1);
    const bool externalContentContext = g_externalContentDepth != 0;
    const bool thirdPartySource = is_third_party_source(file);

    Origin origin = Origin::EngineNative;
    Category primaryCategory = classify_source(file, expression, description, argument0, argument1);

    if (scriptContent)
    {
        origin = Origin::ScriptContent;
        primaryCategory = memoryFailure ? Category::Memory : Category::Script;
    }
    else if (externalContentContext)
    {
        origin = Origin::EngineNativeDuringScriptContent;
    }
    else if (thirdPartySource)
    {
        origin = Origin::ThirdPartyNative;
    }

    if (severity == Severity::Fatal)
        AddBreadcrumb("fatal", safe_text(description), file, line, function);

    write_record(primaryCategory, severity, origin, expression, description,
        argument0, argument1, file, line, function, nullptr, nullptr,
        severity == Severity::Fatal);
}

void ReportUnhandledException(void* exceptionPointers)
{
    if (!IsEnabled() || !exceptionPointers || g_insideCrashHandler || g_insideLogger)
        return;

    EXCEPTION_POINTERS* pointers = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    if (!pointers->ExceptionRecord)
        return;

    g_insideCrashHandler = true;
    if (InterlockedCompareExchange(&g_crashWorkspaceInUse, 1, 0) != 0)
    {
        write_emergency_record(Category::Crash, Severity::Crash, Origin::Unknown,
            "simultaneous unhandled exception", "crash workspace is already in use",
            __FILE__, __LINE__, __FUNCTION__);
        g_insideCrashHandler = false;
        return;
    }

    CrashWorkspace& workspace = g_crashWorkspace;
    ZeroMemory(&workspace, sizeof(workspace));

    AcquireSRWLockExclusive(&g_writeLock);
    const bool directoryReady = ensure_directory_unlocked();
    ReleaseSRWLockExclusive(&g_writeLock);
    if (!directoryReady)
    {
        InterlockedExchange(&g_crashWorkspaceInUse, 0);
        g_insideCrashHandler = false;
        return;
    }

    const void* exceptionAddress = pointers->ExceptionRecord->ExceptionAddress;
    unsigned long long moduleOffset = 0;
    HMODULE module = module_from_address(exceptionAddress, workspace.faultModule,
        sizeof(workspace.faultModule), moduleOffset);

    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    const bool luaJitException = is_luajit_exception_code(code);
    const bool engineModule = is_engine_module(module, workspace.faultModule);
    Origin origin = Origin::Unknown;
    if (luaJitException)
        origin = Origin::ScriptContent;
    else if (engineModule)
        origin = g_externalContentDepth ? Origin::EngineNativeDuringScriptContent : Origin::EngineNative;
    else if (module)
        origin = Origin::ThirdPartyNative;
    else if (g_externalContentDepth)
        origin = Origin::ScriptContent;

    char expression[256] = {};
    _snprintf_s(expression, sizeof(expression), _TRUNCATE,
        "SEH 0x%08lX at %p", code, exceptionAddress);

    char description[512] = {};
    if (luaJitException)
    {
        const DWORD luaErrorCode = code & 0xffu;
        _snprintf_s(description, sizeof(description), _TRUNCATE,
            luaErrorCode == 4u ? "LuaJIT memory error escaped protected call" :
            "LuaJIT error escaped protected call (code=%lu)", luaErrorCode);
    }
    else if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        pointers->ExceptionRecord->NumberParameters >= 2)
    {
        LPCSTR operation = "execute";
        if (pointers->ExceptionRecord->ExceptionInformation[0] == 0)
            operation = "read";
        else if (pointers->ExceptionRecord->ExceptionInformation[0] == 1)
            operation = "write";

        _snprintf_s(description, sizeof(description), _TRUNCATE,
            "%s while attempting to %s address %p", exception_name(code), operation,
            reinterpret_cast<void*>(pointers->ExceptionRecord->ExceptionInformation[1]));
    }
    else
    {
        _snprintf_s(description, sizeof(description), _TRUNCATE, "%s", exception_name(code));
    }

    _snprintf_s(workspace.faultLocation, sizeof(workspace.faultLocation), _TRUNCATE,
        "%s+0x%llX [%p]", *workspace.faultModule ? path_basename(workspace.faultModule) : "<unknown>",
        moduleOffset, exceptionAddress);

    make_crash_id(pointers, workspace);
    append_exception_details(pointers, workspace);
    const bool hasSymbolizedStack = append_symbolized_crash_stack(pointers, workspace);
    if (!hasSymbolizedStack)
        append_stack_scan(pointers, workspace);
    append_breadcrumb_history(workspace);
    append_system_info(workspace);
    append_loaded_modules(workspace);

    if (*workspace.primaryFunction)
    {
        if (*workspace.primarySource)
        {
            _snprintf_s(workspace.faultLocation, sizeof(workspace.faultLocation), _TRUNCATE,
                "%s+0x%llX!%s [%p] at %s:%lu",
                *workspace.faultModule ? path_basename(workspace.faultModule) : "<unknown>",
                moduleOffset, workspace.primaryFunction, exceptionAddress,
                workspace.primarySource, workspace.primaryLine);
        }
        else
        {
            _snprintf_s(workspace.faultLocation, sizeof(workspace.faultLocation), _TRUNCATE,
                "%s+0x%llX!%s [%p]",
                *workspace.faultModule ? path_basename(workspace.faultModule) : "<unknown>",
                moduleOffset, workspace.primaryFunction, exceptionAddress);
        }
    }

    Category crashCategory = Category::Crash;
    const bool luaJitMemoryFailure = luaJitException && (code & 0xffu) == 4u;
    if (luaJitMemoryFailure)
    {
        crashCategory = Category::Memory;
    }
    else if (origin == Origin::ScriptContent)
    {
        crashCategory = Category::Script;
    }
    else if (engineModule && *workspace.primarySource)
    {
        crashCategory = classify_source(workspace.primarySource,
            expression, description, nullptr, nullptr);
    }
    else if (engineModule && *g_threadStage.stage)
    {
        crashCategory = classify_source(g_threadStage.stage,
            expression, description, nullptr, nullptr);
    }
    else if (engineModule)
    {
        crashCategory = classify_source(workspace.faultModule,
            expression, description, nullptr, nullptr);
    }

    write_crash_record(crashCategory, origin, expression, description, workspace);

    InterlockedExchange(&g_crashWorkspaceInUse, 0);
    g_insideCrashHandler = false;
}

void __cdecl ReportExplicit(LPCSTR file, int line, LPCSTR function, LPCSTR format, ...)
{
    char message[4096] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    Report(Severity::Error, nullptr, message, nullptr, nullptr, file, line, function);
}
}
