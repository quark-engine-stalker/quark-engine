#include "stdafx.h"
#pragma hdrstop

#include "engine_error_logger.h"
#include "diagnostic_privacy.h"

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
    constexpr unsigned int kDetailedStackFrameLimit = 32;

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
        volatile LONG busy = 0;
        LONG64 sequence = 0;
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
        char stack[24576] = {};
        char stackScan[2048] = {};
        char breadcrumbs[8192] = {};
        char systemInfo[1024] = {};
        char modules[1024] = {};
    };

    volatile LONG g_enabled = 1;
    volatile LONG64 g_processStartTick = 0;
    volatile LONG64 g_breadcrumbSequence = 0;
    volatile LONG g_frameNumber = 0;
    volatile LONG g_crashWorkspaceInUse = 0;
    SRWLOCK g_writeLock = SRWLOCK_INIT;
    SRWLOCK g_symbolLock = SRWLOCK_INIT;
    char g_directory[MAX_PATH * 3] = {};
    wchar_t g_directoryWide[MAX_PATH] = {};
    volatile LONG64 g_incidentSequence = 0;
    volatile LONG64 g_droppedBreadcrumbs = 0;
    DiagnosticPrivacy::Names g_privateNames;
    INIT_ONCE g_privacyOnce = INIT_ONCE_STATIC_INIT;
    HANDLE g_symbolProcess = nullptr; // unique DbgHelp session, process lifetime
    char g_buildIdentity[512] = {};
    thread_local char g_incidentId[128] = {};
    thread_local LPCSTR g_testFilename = nullptr;
    thread_local bool g_lastWriteSucceeded = false;
    struct FailureCodeState { long result = 0, removedReason = 0; bool valid = false, hasRemovedReason = false; };
    thread_local FailureCodeState g_failureCode;
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

    BOOL CALLBACK initialize_privacy(PINIT_ONCE, PVOID, PVOID*)
    {
        wchar_t value[256] = {};
        GetEnvironmentVariableW(L"USERNAME", value, DWORD(_countof(value)));
        WideCharToMultiByte(CP_UTF8, 0, value, -1, g_privateNames.user, sizeof(g_privateNames.user), nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, value, -1, g_privateNames.userAnsi, sizeof(g_privateNames.userAnsi), nullptr, nullptr);
        ZeroMemory(value, sizeof(value));
        GetEnvironmentVariableW(L"COMPUTERNAME", value, DWORD(_countof(value)));
        WideCharToMultiByte(CP_UTF8, 0, value, -1, g_privateNames.computer, sizeof(g_privateNames.computer), nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, value, -1, g_privateNames.computerAnsi, sizeof(g_privateNames.computerAnsi), nullptr, nullptr);
        // DbgHelp keys sessions by handle value. A unique real handle keeps an
        // overlay/profiler's GetCurrentProcess() symbol session independent.
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
            &g_symbolProcess, 0, FALSE, DUPLICATE_SAME_ACCESS);
        return TRUE;
    }

    void begin_incident()
    {
        SYSTEMTIME utc = {};
        GetSystemTime(&utc);
        _snprintf_s(g_incidentId, sizeof(g_incidentId), _TRUNCATE,
            "%04u%02u%02uT%02u%02u%02u.%03uZ-P%lu-T%lu-Q%llu",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
            GetCurrentProcessId(), GetCurrentThreadId(),
            static_cast<unsigned long long>(InterlockedIncrement64(&g_incidentSequence)));
    }

    LPCSTR result_name(long result)
    {
        switch (static_cast<unsigned long>(result))
        {
        case 0: return "S_OK";
        case 0x80004005: return "E_FAIL";
        case 0x8007000E: return "E_OUTOFMEMORY";
        case 0x80070057: return "E_INVALIDARG";
        case 0x80070005: return "E_ACCESSDENIED";
        case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
        case 0x887A0002: return "DXGI_ERROR_NOT_FOUND";
        case 0x887A0004: return "DXGI_ERROR_UNSUPPORTED";
        case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
        case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG";
        case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET";
        case 0x887A000A: return "DXGI_ERROR_WAS_STILL_DRAWING";
        case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case 0x887A0022: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        default: return "unmapped HRESULT";
        }
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
        size_t limit = used + 4096;
        if (limit > capacity) limit = capacity;
        append_single_line_text(buffer, limit, used, value);
        if (used + 1 >= limit) append_format(buffer, capacity, used, " <truncated>");
        append_format(buffer, capacity, used, "\r\n");
    }

    struct PrivateFileSink
    {
        HANDLE file;
        char buffer[2048] = {};
        DWORD used = 0;
        bool ok = true;
        void flush()
        {
            DWORD offset = 0;
            while (ok && offset < used)
            {
                DWORD written = 0;
                ok = WriteFile(file, buffer + offset, used - offset, &written, nullptr) && written != 0;
                offset += written;
            }
            used = 0;
        }
        void put(char c) { if (used == sizeof(buffer)) flush(); buffer[used++] = c; }
    };

    bool write_handle_text(HANDLE file, LPCSTR text)
    {
        if (file == INVALID_HANDLE_VALUE || !text) return false;
        PrivateFileSink sink{file};
        DiagnosticPrivacy::sanitize(text, sink, g_privateNames);
        sink.flush();
        return sink.ok;
    }

    // No PDB path is copied. RSDS GUID+age identifies the matching symbols even
    // when no PDB is installed, and does not depend on an ASLR load address.
    void format_image_identity(HMODULE module, char* output, size_t capacity)
    {
        copy_text(output, capacity, "<image identity unavailable>");
        if (!module) return;
        __try
        {
            const auto base = reinterpret_cast<const unsigned char*>(module);
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 1024 * 1024) return;
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return;
            const DWORD size = nt->OptionalHeader.SizeOfImage;
            _snprintf_s(output, capacity, _TRUNCATE, "PE timestamp=0x%08lX image_size=%lu",
                nt->FileHeader.TimeDateStamp, size);
            const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            if (!directory.VirtualAddress || directory.VirtualAddress >= size || directory.Size > size - directory.VirtualAddress) return;
            const auto entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
            for (DWORD index = 0; index < directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++index)
            {
                const auto& entry = entries[index];
                if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < 24 ||
                    entry.AddressOfRawData >= size || entry.SizeOfData > size - entry.AddressOfRawData) continue;
                const auto data = base + entry.AddressOfRawData;
                if (memcmp(data, "RSDS", 4)) continue;
                GUID guid = {}; DWORD age = 0;
                memcpy(&guid, data + 4, sizeof(guid)); memcpy(&age, data + 20, sizeof(age));
                _snprintf_s(output, capacity, _TRUNCATE,
                    "PDB={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} age=%lu",
                    guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                    guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7], age);
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { copy_text(output, capacity, "<image metadata read failed>"); }
    }

    void append_record_context(char* output, size_t capacity, size_t& used)
    {
        char process[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, process, DWORD(sizeof(process)));
        append_format(output, capacity, used,
            "Logger       : QUARK v2.0 / UTC\r\n"
            "Incident ID  : %s\r\n"
            "Executable   : %s\r\n"
            "Build        : %s\r\n",
            safe_text(g_incidentId), path_basename(process), safe_text(g_buildIdentity));
        if (g_testFilename)
            append_format(output, capacity, used, "SELFTEST     : SIMULATED FAILURE; the game did not crash\r\n");
        if (!g_failureCode.valid) return;
        const unsigned long code = static_cast<unsigned long>(g_failureCode.result);
        const char* name = result_name(g_failureCode.result);
        append_format(output, capacity, used, "HRESULT      : 0x%08lX (%s)\r\n", code, name);
        if (!strcmp(name, "unmapped HRESULT"))
        {
            wchar_t wide[512] = {}; char decoded[1536] = {};
            if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, code, 0, wide, DWORD(_countof(wide)), nullptr) &&
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, decoded, sizeof(decoded), nullptr, nullptr))
                append_field(output, capacity, used, "System text", decoded);
        }
        if (g_failureCode.hasRemovedReason)
            append_format(output, capacity, used, "Device reason: 0x%08lX (%s)\r\n",
                static_cast<unsigned long>(g_failureCode.removedReason), result_name(g_failureCode.removedReason));
    }

    void append_memory_facts(char* output, size_t capacity, size_t& used)
    {
        MEMORYSTATUSEX memory = {}; memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
            append_format(output, capacity, used, "Memory       : RAM_available=%llu MB commit_available=%llu MB\r\n",
                memory.ullAvailPhys / (1024ull * 1024ull), memory.ullAvailPageFile / (1024ull * 1024ull));
        PROCESS_MEMORY_COUNTERS_EX counters = {};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            append_format(output, capacity, used, "Private bytes: %llu MB\r\n",
                static_cast<unsigned long long>(counters.PrivateUsage) / (1024ull * 1024ull));
    }

    HMODULE module_from_address(const void* address, char* path, size_t pathCapacity, unsigned long long& offset);
    void format_address_location(const void* address, char* output, size_t capacity);

    bool ensure_subdirectory(const wchar_t* path)
    {
        if (!path || !*path) return false;
        return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    bool ensure_directory_unlocked()
    {
        if (*g_directoryWide) return true;
        wchar_t appData[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, DWORD(_countof(appData)));
        if (!length || length >= _countof(appData))
            if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, appData))) return false;
        wchar_t engine[MAX_PATH] = {}, error[MAX_PATH] = {};
        if (_snwprintf_s(engine, _countof(engine), _TRUNCATE, L"%s\\QUARK ENGINE", appData) < 0 || !ensure_subdirectory(engine)) return false;
        if (_snwprintf_s(error, _countof(error), _TRUNCATE, L"%s\\ERROR", engine) < 0 || !ensure_subdirectory(error)) return false;
        if (!WideCharToMultiByte(CP_UTF8, 0, error, -1, g_directory, int(sizeof(g_directory)), nullptr, nullptr)) return false;
        wcscpy_s(g_directoryWide, error);
        return true;
    }

    LPCSTR category_name(Category category);

    HANDLE open_category_file(Category category, DWORD flags, bool rotate)
    {
        if (!*g_directoryWide) return INVALID_HANDLE_VALUE;
        wchar_t name[96] = {}, path[MAX_PATH] = {};
        const char* stem = g_testFilename ? g_testFilename : category_name(category);
        MultiByteToWideChar(CP_UTF8, 0, stem, -1, name, int(_countof(name)));
        if (_snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\%s%s",
            g_directoryWide, name, g_testFilename ? L"" : L"-LOG.log") < 0) return INVALID_HANDLE_VALUE;
        if (rotate)
        {
            WIN32_FILE_ATTRIBUTE_DATA attributes = {};
            if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            {
                ULARGE_INTEGER size = {}; size.HighPart = attributes.nFileSizeHigh; size.LowPart = attributes.nFileSizeLow;
                if (size.QuadPart >= kMaxLogSize)
                {
                    wchar_t previous[MAX_PATH] = {};
                    if (_snwprintf_s(previous, _countof(previous), _TRUNCATE, L"%s.previous", path) >= 0)
                        MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                }
            }
        }
        return CreateFileW(path, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | flags, nullptr);
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
        case Origin::ThirdPartyNative: return "EXTERNAL_MODULE_CONTEXT";
        case Origin::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
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

        HANDLE fileHandle = open_category_file(category, 0, false);
        if (fileHandle == INVALID_HANDLE_VALUE)
            return;

        SYSTEMTIME localTime = {};
        GetSystemTime(&localTime);

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
        append_record_context(record, sizeof(record), used);
        append_format(record, sizeof(record), used, "Origin hint : %s\r\n", origin_name(origin));
        append_format(record, sizeof(record), used,
            "Context     : pid=%lu thread=%lu frame=%ld uptime=%llu ms\r\n",
            GetCurrentProcessId(), GetCurrentThreadId(),
            InterlockedCompareExchange(&g_frameNumber, 0, 0), process_uptime_ms());
        append_format(record, sizeof(record), used,
            "Capture      : minimal nonblocking record; extended diagnostics may follow with same ID\r\n");
        if (*g_threadStage.stage)
            append_format(record, sizeof(record), used, "Stage       : %s @ %s:%d (%s)\r\n",
                g_threadStage.stage, stageSource, g_threadStage.line, safe_text(g_threadStage.function));
        append_field(record, sizeof(record), used, "Expression", expression);
        append_field(record, sizeof(record), used, "Message", description);
        append_format(record, sizeof(record), used, "Location    : %s:%d (%s)\r\n\r\n",
            sourcePath, line, safe_text(function));

        // Coalesce the minimal record into one append, including concurrent
        // secondary crashes, rather than interleaving many streamed chunks.
        char safeRecord[8192] = {};
        DiagnosticPrivacy::Buffer sanitized{safeRecord, sizeof(safeRecord)};
        DiagnosticPrivacy::sanitize(record, sanitized, g_privateNames);
        sanitized.finish();
        DWORD bytesWritten = 0;
        WriteFile(fileHandle, safeRecord, DWORD(strlen(safeRecord)), &bytesWritten, nullptr);
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
        g_lastWriteSucceeded = false;
        if (!IsEnabled() || g_insideLogger)
            return;

        const DWORD preservedLastError = GetLastError();
        g_insideLogger = true;

        const bool lockAcquired = severity != Severity::Error ?
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

        HANDLE fileHandle = open_category_file(category, 0, true);

        if (fileHandle != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME localTime = {};
            GetSystemTime(&localTime);

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
            append_record_context(g_recordBuffer, sizeof(g_recordBuffer), used);
            if (category == Category::Memory || (g_failureCode.valid && static_cast<unsigned long>(g_failureCode.result) == 0x8007000E))
                append_memory_facts(g_recordBuffer, sizeof(g_recordBuffer), used);
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "Origin hint : %s\r\n", origin_name(origin));
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
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "Location    : %s:%d (%s)\r\n",
                sourcePath, line, safe_text(function));
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Expression", expression);
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Message", description);
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Details", argument0);
            append_field(g_recordBuffer, sizeof(g_recordBuffer), used, "Context info", argument1);

            if (extraDetails && *extraDetails)
                append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "Diagnostics:\r\n%s\r\n", extraDetails);
            if (includeNativeStack)
                append_native_stack(g_recordBuffer, sizeof(g_recordBuffer), used);
            append_format(g_recordBuffer, sizeof(g_recordBuffer), used, "\r\n");

            DWORD bytesWritten = 0;
            g_lastWriteSucceeded = write_handle_text(fileHandle, g_recordBuffer);
            if (!FlushFileBuffers(fileHandle)) g_lastWriteSucceeded = false;
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
        case 0xE06D7363: return "unhandled MSVC C++ exception";
        case 0xC0000017: return "virtual memory allocation failed";
        case 0xC0000374: return "heap corruption detected";
        case 0xC0000409: return "security/fast-fail termination";
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
        if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.Type != MEM_IMAGE)
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

    LPCSTR local_symbol_path()
    {
        static char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, DWORD(sizeof(path)));
        char* slash = strrchr(path, '\\');
        if (slash) *slash = 0;
        return *path ? path : ".";
    }

    class CrashStackWalker final : public StackWalker
    {
    public:
        CrashStackWalker(char* output, size_t capacity, size_t& used,
            char* primarySource, size_t primarySourceCapacity,
            char* primaryFunction, size_t primaryFunctionCapacity, DWORD& primaryLine)
            : StackWalker(StackWalker::RetrieveSymbol | StackWalker::RetrieveLine |
                StackWalker::RetrieveModuleInfo,
                local_symbol_path(), GetCurrentProcessId(), g_symbolProcess ? g_symbolProcess : GetCurrentProcess()),
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

        void OnDbgHelpErr(LPCSTR operation, DWORD error, DWORD64 address) override
        {
            if (symbolErrors_++ < 8)
                append_format(output_, capacity_, used_,
                    "  [symbols] %s failed: Win32=%lu address=0x%llX (PDB may be absent/unmatched)\r\n",
                    safe_text(operation), error, static_cast<unsigned long long>(address));
        }

        void OnLoadModule(LPCSTR, LPCSTR module, DWORD64, DWORD, DWORD result,
            LPCSTR symbolType, LPCSTR, ULONGLONG) override
        {
            if (result && symbolErrors_++ < 8)
                append_format(output_, capacity_, used_, "  [symbols] module=%s load_error=%lu type=%s\r\n",
                    path_basename(module), result, safe_text(symbolType));
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
        unsigned int symbolErrors_ = 0;
    };

    bool walk_crash_stack(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace, size_t& used)
    {
        CONTEXT context = *pointers->ContextRecord;
        CrashStackWalker walker(workspace.stack, sizeof(workspace.stack), used,
            workspace.primarySource, sizeof(workspace.primarySource),
            workspace.primaryFunction, sizeof(workspace.primaryFunction), workspace.primaryLine);
        if (!walker.ShowCallstack(GetCurrentThread(), &context))
            append_format(workspace.stack, sizeof(workspace.stack), used,
                "  Unwind incomplete; module+RVA and matching PDB are still usable.\r\n");
        return walker.frame_count() != 0;
    }

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

        if (!TryBeginSymbolWork())
        {
            append_format(workspace.stack, sizeof(workspace.stack), used,
                "  Symbols skipped: DbgHelp is busy (no blocking in crash handler).\r\n");
            return false;
        }
        bool hasFrames = false;
        __try { hasFrames = walk_crash_stack(pointers, workspace, used); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            append_format(workspace.stack, sizeof(workspace.stack), used,
                "  Symbolization failed inside DbgHelp; original crash checkpoint retained.\r\n");
        }
        EndSymbolWork();
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
            // Copy through the OS rather than dereferencing memory outside C++
            // object lifetimes (including ASan stack red zones). Never emit raw
            // words: only module RVAs for executable image candidates.
            uintptr_t words[192] = {};
            SIZE_T copied = 0;
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(stackPointer),
                words, wordsToScan * sizeof(uintptr_t), &copied);
            for (size_t index = 0; index < copied / sizeof(uintptr_t) && found < 8; ++index)
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
        if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
            memory.Type != MEM_IMAGE || !is_executable_protection(memory.Protect))
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


        if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2)
        {
            const uintptr_t target = static_cast<uintptr_t>(record->ExceptionInformation[1]);
            const ULONG_PTR operation = record->ExceptionInformation[0];
            append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                "  Access        : %s target=0x%llX\r\n",
                operation == 0 ? "read" : operation == 1 ? "write" : operation == 8 ? "execute/DEP" : "unknown",
                static_cast<unsigned long long>(target));
            if (target < 0x10000)
                append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                    "  Observation   : target is below 64 KiB (near-null); object ownership is not established\r\n");
            MEMORY_BASIC_INFORMATION memory = {};
            if (VirtualQuery(reinterpret_cast<const void*>(target), &memory, sizeof(memory)))
                append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                    "  Target region : state=0x%lX protection=0x%lX type=0x%lX size=%llu bytes\r\n",
                    memory.State, memory.Protect, memory.Type, static_cast<unsigned long long>(memory.RegionSize));
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
                "  RIP=%016llX RSP=%016llX RBP=%016llX\r\n"
                "  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\r\n"
                "  RSI=%016llX RDI=%016llX R8=%016llX R9=%016llX\r\n"
                "  R10=%016llX R11=%016llX R12=%016llX R13=%016llX R14=%016llX R15=%016llX\r\n",
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
        // Nested records are metadata only, bounded and protected by the outer SEH guard.
        EXCEPTION_RECORD* nested = record->ExceptionRecord;
        for (unsigned index = 0; nested && index < 4; ++index)
        {
            char location[256] = {};
            format_address_location(nested->ExceptionAddress, location, sizeof(location));
            append_format(workspace.exceptionDetails, sizeof(workspace.exceptionDetails), used,
                "  Nested[%u]     : 0x%08lX flags=0x%lX at %s\r\n", index,
                nested->ExceptionCode, nested->ExceptionFlags, safe_text(location));
            if (nested == nested->ExceptionRecord || nested == record) break;
            nested = nested->ExceptionRecord;
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
        const LONG64 oldest = newest > 16 ? newest - 16 + 1 : 1;
        unsigned int written = 0;

        for (LONG64 sequence = oldest; sequence <= newest; ++sequence)
        {
            BreadcrumbSlot& slot = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
            if (InterlockedCompareExchange(&slot.busy, 1, 0) != 0) continue;
            if (slot.sequence != sequence) { InterlockedExchange(&slot.busy, 0); continue; }
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
            InterlockedExchange(&slot.busy, 0);

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
        const LONG64 dropped = InterlockedCompareExchange64(&g_droppedBreadcrumbs, 0, 0);
        if (dropped) append_format(workspace.breadcrumbs, sizeof(workspace.breadcrumbs), used,
            "  History incomplete: %llu busy writes skipped\r\n", static_cast<unsigned long long>(dropped));

    }

    void append_fault_module_identity(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
    {
        if (!pointers || !pointers->ExceptionRecord) return;
        char path[MAX_PATH] = {}, identity[512] = {}; unsigned long long offset = 0;
        HMODULE module = module_from_address(pointers->ExceptionRecord->ExceptionAddress, path, sizeof(path), offset);
        if (!module || module == GetModuleHandleA(nullptr)) return; // main build is already identified
        format_image_identity(module, identity, sizeof(identity));
        size_t used = 0;
        append_format(workspace.modules, sizeof(workspace.modules), used, "Fault module  : %s / %s\r\n",
            path_basename(path), identity);
    }

    void make_crash_id(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
    {
        copy_text(workspace.crashId, sizeof(workspace.crashId), g_incidentId);
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

        HANDLE file = open_category_file(category, FILE_FLAG_WRITE_THROUGH, true);
        if (file != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME localTime = {};
            GetSystemTime(&localTime);

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
                "Origin hint   : %s\r\n"
                "Process       : %s (pid=%lu)\r\n"
                "Context       : thread=%lu frame=%ld uptime=%llu ms\r\n",
                localTime.wYear, localTime.wMonth, localTime.wDay,
                localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds,
                category_name(category), origin_name(origin),
                *processPath ? path_basename(processPath) : "<unknown>", GetCurrentProcessId(), GetCurrentThreadId(),
                InterlockedCompareExchange(&g_frameNumber, 0, 0), process_uptime_ms());
            append_record_context(header, sizeof(header), used);
            append_field(header, sizeof(header), used, "Message", description);
            append_format(header, sizeof(header), used,
                "Fault location: %s\r\n",
                safe_text(workspace.faultLocation));


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
    const DWORD lastError = GetLastError();
    InitOnceExecuteOnce(&g_privacyOnce, initialize_privacy, nullptr, nullptr);
    process_uptime_ms();
    AcquireSRWLockExclusive(&g_writeLock);
    ensure_directory_unlocked();
    if (!*g_buildIdentity) format_image_identity(GetModuleHandleA(nullptr), g_buildIdentity, sizeof(g_buildIdentity));
    ReleaseSRWLockExclusive(&g_writeLock);
    SetLastError(lastError);
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
    static thread_local bool stackReserved = false;
    if (!stackReserved)
    {
        const DWORD lastError = GetLastError();
        ULONG reserve = 64u * 1024u;
        stackReserved = SetThreadStackGuarantee(&reserve) != FALSE;
        SetLastError(lastError);
    }

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
    if (InterlockedCompareExchange(&slot.busy, 1, 0) != 0)
    { InterlockedIncrement64(&g_droppedBreadcrumbs); return; }
    if (slot.sequence > sequence)
    { InterlockedExchange(&slot.busy, 0); InterlockedIncrement64(&g_droppedBreadcrumbs); return; }
    slot.tick = GetTickCount64();
    slot.threadId = GetCurrentThreadId();
    copy_text(slot.category, sizeof(slot.category), category);
    SanitizeText(message, slot.message, sizeof(slot.message));
    format_source_path(file, slot.file, sizeof(slot.file));
    copy_text(slot.function, sizeof(slot.function), function);
    slot.line = line;
    slot.sequence = sequence;
    InterlockedExchange(&slot.busy, 0);
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

    begin_incident();
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

void report_unhandled_impl(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
{
    ZeroMemory(&workspace, sizeof(workspace));
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
        LPCSTR operation = pointers->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute/DEP" : "unknown";
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
    // Save the original failure before allocating symbol helpers or loading PDBs.
    write_emergency_record(Category::Crash, Severity::Crash, origin, expression, description,
        workspace.faultLocation, 0, "original exception checkpoint (before symbolization)");
    append_exception_details(pointers, workspace);
    const bool constrained = code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000017 ||
        (luaJitException && (code & 0xffu) == 4u);
    const bool hasSymbolizedStack = !constrained && append_symbolized_crash_stack(pointers, workspace);
    if (constrained) copy_text(workspace.stack, sizeof(workspace.stack),
        "Symbols skipped for stack-overflow/OOM; original context and module RVAs retained.\r\n");
    if (!hasSymbolizedStack)
        append_stack_scan(pointers, workspace);
    append_breadcrumb_history(workspace);
    append_fault_module_identity(pointers, workspace);

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

    if (crashCategory == Category::Memory)
    {
        size_t used = 0;
        append_memory_facts(workspace.systemInfo, sizeof(workspace.systemInfo), used);
    }
    write_crash_record(crashCategory, origin, expression, description, workspace);

}

void ReportUnhandledException(void* exceptionPointers)
{
    if (!IsEnabled() || !exceptionPointers || g_insideCrashHandler) return;
    const DWORD lastError = GetLastError();
    g_insideCrashHandler = true;
    begin_incident();
    if (InterlockedCompareExchange(&g_crashWorkspaceInUse, 1, 0) != 0)
    {
        __try
        {
            auto pointers = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
            char expression[160] = {}, location[256] = {};
            const auto record = pointers->ExceptionRecord;
            _snprintf_s(expression, sizeof(expression), _TRUNCATE, "simultaneous SEH 0x%08lX",
                record ? record->ExceptionCode : 0);
            if (record) format_address_location(record->ExceptionAddress, location, sizeof(location));
            write_emergency_record(Category::Crash, Severity::Crash, Origin::Unknown, expression,
                "workspace busy; original code/location retained without waiting",
                location, 0, "simultaneous exception checkpoint");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            write_emergency_record(Category::Crash, Severity::Crash, Origin::Unknown,
                "simultaneous exception", "workspace busy and exception metadata unreadable",
                __FILE__, __LINE__, __FUNCTION__);
        }
    }
    else
    {
        __try { report_unhandled_impl(static_cast<EXCEPTION_POINTERS*>(exceptionPointers), g_crashWorkspace); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            write_emergency_record(Category::Crash, Severity::Crash, Origin::Unknown,
                "logger secondary fault", "extended diagnostics failed; consult original exception checkpoint",
                __FILE__, __LINE__, __FUNCTION__);
        }
        InterlockedExchange(&g_crashWorkspaceInUse, 0);
    }
    g_insideCrashHandler = false;
    SetLastError(lastError);
}


bool TryBeginSymbolWork() { return TryAcquireSRWLockExclusive(&g_symbolLock) != FALSE; }
void EndSymbolWork() { ReleaseSRWLockExclusive(&g_symbolLock); }

size_t SanitizeText(LPCSTR text, char* output, size_t capacity)
{
    const DWORD lastError = GetLastError();
    InitOnceExecuteOnce(&g_privacyOnce, initialize_privacy, nullptr, nullptr);
    DiagnosticPrivacy::Buffer sink{output, capacity};
    DiagnosticPrivacy::sanitize(text, sink, g_privateNames);
    sink.finish();
    SetLastError(lastError);
    return sink.length;
}

FailureCodeScope::FailureCodeScope(long result, long removedReason, bool hasRemovedReason)
    : previousResult_(g_failureCode.result), previousRemovedReason_(g_failureCode.removedReason),
      previousValid_(g_failureCode.valid), previousHasRemovedReason_(g_failureCode.hasRemovedReason)
{
    g_failureCode = {result, removedReason, true, hasRemovedReason};
}
FailureCodeScope::~FailureCodeScope()
{
    g_failureCode = {previousResult_, previousRemovedReason_, previousValid_, previousHasRemovedReason_};
}

void ReportRenderFailure(long result, long removedReason, LPCSTR expression, LPCSTR file, int line, LPCSTR function)
{
    if (!IsEnabled()) return;
    const DWORD lastError = GetLastError();
    begin_incident();
    FailureCodeScope code(result, removedReason, true);
    write_record(Category::Render, Severity::Error, Origin::Unknown, expression,
        "DX11 API failed",
        nullptr, nullptr, file, line, function, nullptr, nullptr, false);
    SetLastError(lastError);
}

bool ShouldReportScriptFailure(int status, bool recoveredModuleLoad, LPCSTR message)
{
    if (status == 0 || status == 1) return false; // success / yield
    if (!recoveredModuleLoad || (status != 2 && status != 3)) return true;
    // Never infer ownership from a filename or the SCRIPT_OR_CONTENT hint.
    // Keep unknown error values and native OOM messages converted to LUA_ERRRUN.
    return !message || !*message || contains_memory_failure(nullptr, message, nullptr, nullptr) ||
        contains_i(message, "not enough memory") || contains_i(message, "memory exhausted");
}

void ReportScriptFailure(int status, LPCSTR script, LPCSTR message, LPCSTR traceback,
    LPCSTR file, int line, LPCSTR function, bool recoveredModuleLoad)
{
    if (!IsEnabled() || !ShouldReportScriptFailure(status, recoveredModuleLoad, message)) return;
    const DWORD lastError = GetLastError();
    begin_incident();
    const bool memoryFailure = status == 4 || contains_memory_failure(nullptr, message, nullptr, nullptr) ||
        contains_i(message, "not enough memory") || contains_i(message, "memory exhausted");
    char details[1024] = {};
    _snprintf_s(details, sizeof(details), _TRUNCATE, "Lua status=%d; chunk=%s", status, path_basename(script));
    LPCSTR expression = "Lua script error";
    if (memoryFailure) expression = "Lua memory allocation failed";
    else if (status == 2) expression = "Lua runtime error";
    else if (status == 3) expression = "Lua chunk compilation failed";
    write_record(memoryFailure ? Category::Memory : Category::Script, Severity::Error,
        Origin::ScriptContent, expression,
        message ? message : "Lua error value is not a string (not coerced during diagnostics)", details, nullptr,
        file, line, function, nullptr, traceback, false);
    SetLastError(lastError);
}

bool WriteSelfTest()
{
    if (!IsEnabled() || g_insideLogger || g_testFilename) return false;
    Initialize();
    g_testFilename = "RENDER-LOG-SELFTEST-v2.log";
    ReportRenderFailure(static_cast<long>(0x887A0005u), static_cast<long>(0x887A0006u),
        "SIMULATED ID3D11Device/CreateTexture2D; texture=gamedata/textures/ui/selftest.dds; "
        "install=Z:\\Private Test User\\Private GAMMA Install\\bin\\engine.exe; token=dummy-secret",
        "Layers/xrRenderDX10/diagnostic_selftest.cpp", 42, "EngineErrorLogger::WriteSelfTest");
    g_testFilename = nullptr;
    return g_lastWriteSucceeded;
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
