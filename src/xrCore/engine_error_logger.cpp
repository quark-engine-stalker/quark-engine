#include "stdafx.h"
#pragma hdrstop

#include "engine_error_logger.h"
#include "diagnostic_privacy.h"

#include <Windows.h>
#include <ShlObj.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>

#include "../3rd party/stackwalker/include/StackWalker.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace EngineErrorLogger
{
namespace
{
    constexpr unsigned long long kMaxLogSize = 16ull * 1024ull * 1024ull;
    constexpr unsigned int kStackFrameLimit = 32;
    constexpr unsigned int kBreadcrumbCount = 64;
    constexpr unsigned int kBreadcrumbsShown = 12;
    constexpr DWORD kCppException = 0xE06D7363;
    constexpr DWORD kNoMemory = 0xC0000017;

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
        Crash
    };

    struct __declspec(align(64)) BreadcrumbSlot
    {
        volatile LONG busy = 0;
        LONG64 sequence = 0;
        char message[160] = {};
    };

    // Static storage: a crash may come from heap corruption or a stack overflow.
    struct CrashWorkspace
    {
        CONTEXT context;
        char error[1024];
        char where[1536];
        char module[MAX_PATH + 128];
        char stack[8192];    // module+RVA frames, unwound without DbgHelp or heap
        char symbols[24576]; // the same stack with symbols, when DbgHelp succeeds
        char lua[8192];
        char machine[1024];
        char record[65536];
    };

    volatile LONG g_enabled = 1;
    volatile LONG64 g_breadcrumbSequence = 0;
    volatile LONG g_crashWorkspaceInUse = 0;
    volatile LONG g_sessionFiles = 0; // bit per category: already written by this process
    SRWLOCK g_writeLock = SRWLOCK_INIT;
    SRWLOCK g_symbolLock = SRWLOCK_INIT;
    char g_directory[MAX_PATH * 3] = {};
    wchar_t g_directoryWide[MAX_PATH] = {};
    DiagnosticPrivacy::Names g_privateNames;
    INIT_ONCE g_privacyOnce = INIT_ONCE_STATIC_INIT;
    HANDLE g_symbolProcess = nullptr; // unique DbgHelp session, process lifetime
    char g_buildIdentity[512] = {};
    LuaStackWriter g_luaStackWriter = nullptr;
    volatile DWORD g_luaThread = 0;
    thread_local LPCSTR g_testFilename = nullptr;
    thread_local bool g_lastWriteSucceeded = false;
    struct FailureCodeState { long result = 0, removedReason = 0; bool valid = false, hasRemovedReason = false; };
    thread_local FailureCodeState g_failureCode;
    char g_recordBuffer[65536] = {};
    BreadcrumbSlot g_breadcrumbs[kBreadcrumbCount] = {};
    CrashWorkspace g_crashWorkspace = {};
    thread_local bool g_insideLogger = false;
    thread_local bool g_insideCrashHandler = false;
    thread_local LPCSTR g_threadStage = nullptr;
    thread_local const void* g_threadStageCallback = nullptr;
    thread_local char g_threadName[64] = {};

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
        default: return nullptr;
        }
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
        append_format(buffer, capacity, used, "%-10s: ", label);
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
            _snprintf_s(output, capacity, _TRUNCATE, "PE timestamp 0x%08lX, size %lu",
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
                    "PDB {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} age %lu",
                    guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                    guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7], age);
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { copy_text(output, capacity, "<image metadata read failed>"); }
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
            _snprintf_s(output, capacity, _TRUNCATE, "0x%p (not a loaded module)", address);
    }

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
        if (_snwprintf_s(engine, _countof(engine), _TRUNCATE, L"%s\\QUARK-ENGINE", appData) < 0 || !ensure_subdirectory(engine)) return false;
        if (_snwprintf_s(error, _countof(error), _TRUNCATE, L"%s\\ERROR", engine) < 0 || !ensure_subdirectory(error)) return false;
        if (!WideCharToMultiByte(CP_UTF8, 0, error, -1, g_directory, int(sizeof(g_directory)), nullptr, nullptr)) return false;
        wcscpy_s(g_directoryWide, error);
        return true;
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
        case Category::Crash: return "CRASH";
        }
        return "ENGINE";
    }

    // One file per category and session: the first write of this process moves
    // the previous session's file to .previous, so records need no timestamps.
    HANDLE open_log_file(Category category, DWORD access)
    {
        if (!*g_directoryWide) return INVALID_HANDLE_VALUE;
        wchar_t name[96] = {}, path[MAX_PATH] = {}, previous[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, g_testFilename ? g_testFilename : category_name(category), -1, name, int(_countof(name)));
        if (_snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\%s%s",
                g_directoryWide, name, g_testFilename ? L"" : L"-LOG.log") < 0 ||
            _snwprintf_s(previous, _countof(previous), _TRUNCATE, L"%s.previous", path) < 0)
            return INVALID_HANDLE_VALUE;

        const LONG bit = 1L << int(category);
        bool rotate = !g_testFilename && !(InterlockedOr(&g_sessionFiles, bit) & bit);
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!rotate && GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            rotate = (ULONGLONG(attributes.nFileSizeHigh) << 32 | attributes.nFileSizeLow) >= kMaxLogSize;
        if (rotate)
            MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

        const auto create = [&]() { return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); };
        HANDLE file = create();
        if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND)
        {
            // The folder was deleted while the game was running: create it again.
            wchar_t parent[MAX_PATH] = {};
            wcscpy_s(parent, g_directoryWide);
            if (wchar_t* slash = wcsrchr(parent, L'\\'))
                *slash = 0;
            ensure_subdirectory(parent);
            ensure_subdirectory(g_directoryWide);
            file = create();
        }
        if (file == INVALID_HANDLE_VALUE)
            return file;
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(file, &size) && !size.QuadPart)
        {
            char header[768] = {}, process[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, process, DWORD(sizeof(process)));
            _snprintf_s(header, sizeof(header), _TRUNCATE, "QUARK ENGINE error log 2.1 | %s | %s\r\n",
                path_basename(process), safe_text(g_buildIdentity));
            write_handle_text(file, header);
        }
        SetFilePointerEx(file, LARGE_INTEGER{}, nullptr, FILE_END);
        return file;
    }

    // Lock-free append for paths that must not wait (busy logger, crash notes).
    void append_text_record(Category category, LPCSTR text)
    {
        HANDLE file = open_log_file(category, FILE_APPEND_DATA);
        if (file == INVALID_HANDLE_VALUE) return;
        write_handle_text(file, text);
        FlushFileBuffers(file);
        CloseHandle(file);
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
        LPCSTR values[] = { expression, description, argument0, argument1 };
        for (LPCSTR value : values)
        {
            if (contains_i(value, "out of memory") ||
                contains_i(value, "not enough memory") ||
                contains_i(value, "memory exhausted") ||
                contains_i(value, "memory request") ||
                contains_i(value, "bad_alloc"))
                return true;
        }
        return false;
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

        return Category::Engine;
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
        case kNoMemory: return "out of memory (virtual memory allocation failed)";
        case 0xC0000374: return "heap corruption (memory freed twice or written out of bounds)";
        case 0xC0000409: return "stack buffer overrun / fast-fail";
        default: return "unhandled structured exception";
        }
    }

    bool is_executable_protection(DWORD protection)
    {
        const DWORD base = protection & 0xff;
        return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
            base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    }

    // Windows and CRT modules only transport a failure (RaiseException, heap checks):
    // the record points at the first frame outside them.
    bool is_system_module(LPCSTR path)
    {
        static constexpr LPCSTR names[] = { "ntdll.dll", "kernelbase.dll", "kernel32.dll", "ucrtbase.dll",
            "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll" };
        LPCSTR name = path_basename(path);
        for (LPCSTR candidate : names)
            if (!_stricmp(name, candidate))
                return true;
        return false;
    }

    LPCSTR local_symbol_path()
    {
        static char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, DWORD(sizeof(path)));
        char* slash = strrchr(path, '\\');
        if (slash) *slash = 0;
        return *path ? path : ".";
    }

    void append_thread_context(char* output, size_t capacity, size_t& used)
    {
        append_format(output, capacity, used, "Thread    : %s\r\n", *g_threadName ? g_threadName : "unnamed");
        if (!g_threadStage)
            return;
        append_format(output, capacity, used, "Stage     : %s", g_threadStage);
        if (g_threadStageCallback)
        {
            char location[MAX_PATH + 32] = {};
            format_address_location(g_threadStageCallback, location, sizeof(location));
            append_format(output, capacity, used, " (callback %s)", location);
        }
        append_format(output, capacity, used, "\r\n");
    }

    void append_memory_facts(char* output, size_t capacity, size_t& used)
    {
        MEMORYSTATUSEX memory = {};
        memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory))
            return;
        PROCESS_MEMORY_COUNTERS_EX counters = {};
        const bool process = GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE;
        append_format(output, capacity, used, "Memory    : RAM free %llu MB, commit free %llu MB, game private %llu MB\r\n",
            memory.ullAvailPhys >> 20, memory.ullAvailPageFile >> 20,
            process ? static_cast<unsigned long long>(counters.PrivateUsage) >> 20 : 0ull);
    }

    void append_failure_code(char* output, size_t capacity, size_t& used)
    {
        if (!g_failureCode.valid)
            return;
        const unsigned long code = static_cast<unsigned long>(g_failureCode.result);
        LPCSTR name = result_name(g_failureCode.result);
        char decoded[1024] = {};
        wchar_t wide[512] = {};
        if (!name && FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, code, 0, wide, DWORD(_countof(wide)), nullptr))
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, decoded, sizeof(decoded), nullptr, nullptr);
        append_format(output, capacity, used, "HRESULT   : 0x%08lX (", code);
        append_single_line_text(output, capacity, used, name ? name : *decoded ? decoded : "unknown");
        append_format(output, capacity, used, ")\r\n");
        if (g_failureCode.hasRemovedReason)
        {
            LPCSTR reason = result_name(g_failureCode.removedReason);
            append_format(output, capacity, used, "Device    : removed, reason 0x%08lX (%s)\r\n",
                static_cast<unsigned long>(g_failureCode.removedReason), reason ? reason : "unknown");
        }
    }

    void append_lua_stack(char* output, size_t capacity, size_t& used)
    {
        if (!g_luaStackWriter || GetCurrentThreadId() != g_luaThread || used + 64 >= capacity)
            return;
        const size_t start = used;
        append_format(output, capacity, used, "Lua stack:\r\n");
        __try { g_luaStackWriter(output + used, capacity - used); }
        __except (EXCEPTION_EXECUTE_HANDLER) { output[used] = 0; }
        const size_t added = strnlen(output + used, capacity - used - 1);
        if (!added)
        {
            used = start;
            output[used] = 0;
            return;
        }
        used += added;
        output[used] = 0;
    }

    class LogStackWalker final : public StackWalker
    {
    public:
        LogStackWalker(char* output, size_t capacity, size_t& used, bool skipLogger)
            : StackWalker(StackWalker::RetrieveSymbol | StackWalker::RetrieveLine |
                StackWalker::RetrieveModuleInfo,
                local_symbol_path(), GetCurrentProcessId(), g_symbolProcess ? g_symbolProcess : GetCurrentProcess()),
              output_(output), capacity_(capacity), used_(used), skipping_(skipLogger)
        {
        }

        unsigned int frames() const { return frames_; }
        LPCSTR first_frame() const { return first_; }

    protected:
        void OnCallstackEntry(CallstackEntryType entryType, CallstackEntry& entry) override
        {
            if (entryType == lastEntry || entry.offset == 0 || frames_ >= kStackFrameLimit)
                return;

            LPCSTR symbol = *entry.undName ? entry.undName : entry.name;
            // A fatal record starts at the code that reported the error, not inside the reporter.
            if (skipping_ && (!strncmp(symbol, "EngineErrorLogger::", 19) ||
                !strncmp(symbol, "StackWalker::", 13) || !strncmp(symbol, "xrDebug::", 9)))
                return;
            skipping_ = false;

            LPCSTR module = *entry.loadedImageName ? path_basename(entry.loadedImageName) : path_basename(entry.moduleName);
            const unsigned long long rva = entry.baseOfImage && entry.offset >= entry.baseOfImage ?
                static_cast<unsigned long long>(entry.offset - entry.baseOfImage) : 0;
            char frame[1024] = {};
            size_t used = 0;
            if (*symbol)
            {
                append_format(frame, sizeof(frame), used, "%.160s", symbol);
                if (*entry.lineFileName && entry.lineNumber)
                {
                    char source[512] = {};
                    format_source_path(entry.lineFileName, source, sizeof(source));
                    append_format(frame, sizeof(frame), used, " (%s:%lu)", source, entry.lineNumber);
                }
                else // without a PDB this is the nearest export; the offset shows how near
                    append_format(frame, sizeof(frame), used, "+0x%llX", static_cast<unsigned long long>(entry.offsetFromSmybol));
                append_format(frame, sizeof(frame), used, " [%s+0x%llX]", module, rva);
            }
            else
                append_format(frame, sizeof(frame), used, "%s+0x%llX", module, rva);

            if (!frames_ || (systemFirst_ && !is_system_module(module)))
            {
                copy_text(first_, sizeof(first_), frame);
                systemFirst_ = is_system_module(module);
            }
            append_format(output_, capacity_, used_, "  #%02u %s\r\n", frames_++, frame);
        }

        void OnOutput(LPCSTR) override
        {
        }

    private:
        char* output_ = nullptr;
        size_t capacity_ = 0;
        size_t& used_;
        bool skipping_ = false;
        bool systemFirst_ = false;
        unsigned int frames_ = 0;
        char first_[1024] = {}; // the first frame outside Windows/CRT modules
    };

    unsigned int walk_stack(const CONTEXT* context, bool skipLogger, char* output, size_t capacity, size_t& used,
        char* first, size_t firstCapacity)
    {
        LogStackWalker walker(output, capacity, used, skipLogger);
        if (context)
        {
            CONTEXT copy = *context; // StackWalk64 modifies the context it walks
            walker.ShowCallstack(GetCurrentThread(), &copy);
        }
        else
            walker.ShowCallstack();
        if (first && walker.frames())
            copy_text(first, firstCapacity, walker.first_frame());
        return walker.frames();
    }

    // DbgHelp is serialized and never waited for: a crash must not block on it.
    unsigned int append_symbolized_stack(const CONTEXT* context, bool skipLogger, char* output, size_t capacity,
        size_t& used, char* first = nullptr, size_t firstCapacity = 0)
    {
        if (!TryBeginSymbolWork())
            return 0;
        const size_t start = used;
        unsigned int frames = 0;
        __try { frames = walk_stack(context, skipLogger, output, capacity, used, first, firstCapacity); }
        __except (EXCEPTION_EXECUTE_HANDLER) { frames = 0; }
        EndSymbolWork();
        if (!frames)
        {
            used = start;
            output[used] = 0;
        }
        return frames;
    }

    // Module+RVA frames straight from the unwind tables: no DbgHelp, no heap.
    // They identify the code with the matching PDB if symbolization fails.
    void append_raw_stack(CONTEXT& context, char* output, size_t capacity, size_t& used)
    {
#if defined(_M_X64)
        __try
        {
            for (unsigned int index = 0; index < kStackFrameLimit && context.Rip; ++index)
            {
                char location[MAX_PATH + 32] = {};
                format_address_location(reinterpret_cast<const void*>(context.Rip), location, sizeof(location));
                append_format(output, capacity, used, "  #%02u %s\r\n", index, location);

                const DWORD64 previousStack = context.Rsp;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (function)
                {
                    PVOID handlerData = nullptr;
                    DWORD64 establisherFrame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
                        &handlerData, &establisherFrame, nullptr);
                }
                else
                {
                    // Leaf function: the return address is on top of the stack.
                    context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                }
                if (context.Rsp <= previousStack)
                    break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
#endif
    }

    void append_machine_state(const EXCEPTION_POINTERS* pointers, char* output, size_t capacity, size_t& used)
    {
#if defined(_M_X64)
        if (pointers->ContextRecord)
        {
            const CONTEXT& c = *pointers->ContextRecord;
            append_format(output, capacity, used,
                "Registers :\r\n"
                "  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\r\n"
                "  RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\r\n"
                "  R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\r\n"
                "  R12=%016llX R13=%016llX R14=%016llX R15=%016llX\r\n",
                c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp,
                c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
        }
#endif
        const void* address = pointers->ExceptionRecord->ExceptionAddress;
        MEMORY_BASIC_INFORMATION memory = {};
        if (!address || !VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
            !is_executable_protection(memory.Protect))
            return;
        append_format(output, capacity, used, "Code      :");
        __try
        {
            const unsigned char* bytes = static_cast<const unsigned char*>(address);
            for (unsigned int index = 0; index < 16; ++index)
                append_format(output, capacity, used, " %02X", bytes[index]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        append_format(output, capacity, used, "\r\n");
    }

    // ".?AVbad_alloc@std@@" -> "std::bad_alloc"; template types stay decorated.
    void format_type_name(LPCSTR decorated, char* output, size_t capacity)
    {
        copy_text(output, capacity, decorated);
        if (strncmp(decorated, ".?A", 3) || !decorated[3] || strchr(decorated + 4, '?'))
            return;
        LPCSTR parts[8] = {};
        int lengths[8] = {};
        int count = 0;
        for (LPCSTR part = decorated + 4; *part && *part != '@' && count < 8; ++count)
        {
            LPCSTR end = strchr(part, '@');
            if (!end)
                return;
            parts[count] = part;
            lengths[count] = int(end - part);
            part = end + 1;
        }
        size_t used = 0;
        *output = 0;
        for (int index = count - 1; index >= 0; --index)
            append_format(output, capacity, used, "%s%.*s", index == count - 1 ? "" : "::", lengths[index], parts[index]);
    }

    void describe_cpp_exception(const EXCEPTION_RECORD& record, char* output, size_t capacity)
    {
        copy_text(output, capacity, "unhandled C++ exception");
#if defined(_M_X64)
        if (record.NumberParameters < 4)
            return;
        __try
        {
            // x64 MSVC throw: [1] object, [2] ThrowInfo, [3] image base. ThrowInfo
            // and the catchable type array hold RVAs from that image base.
            const uintptr_t base = record.ExceptionInformation[3];
            const int* throwInfo = reinterpret_cast<const int*>(record.ExceptionInformation[2]);
            const int* types = reinterpret_cast<const int*>(base + throwInfo[3]);
            LPCSTR first = nullptr;
            const std::exception* standard = nullptr;
            for (int index = 0; index < types[0] && index < 32; ++index)
            {
                // CatchableType: properties, type descriptor RVA, mdisp, pdisp, vdisp
                const int* type = reinterpret_cast<const int*>(base + types[1 + index]);
                LPCSTR name = reinterpret_cast<LPCSTR>(base + type[1] + 2 * sizeof(void*));
                if (!first)
                    first = name;
                if (!strcmp(name, ".?AVexception@std@@") && type[3] < 0)
                    standard = reinterpret_cast<const std::exception*>(
                        reinterpret_cast<const char*>(record.ExceptionInformation[1]) + type[2]);
            }
            char typeName[256] = {};
            if (first)
                format_type_name(first, typeName, sizeof(typeName));
            size_t used = 0;
            append_format(output, capacity, used, "unhandled C++ exception %s", *typeName ? typeName : "<unknown type>");
            if (standard)
            {
                append_format(output, capacity, used, ": ");
                append_single_line_text(output, capacity, used, standard->what());
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
#endif
    }

    void describe_exception(const EXCEPTION_RECORD& record, char* output, size_t capacity)
    {
        const DWORD code = record.ExceptionCode;
        if (code == kCppException)
        {
            describe_cpp_exception(record, output, capacity);
            return;
        }
        if (is_luajit_exception_code(code))
        {
            if ((code & 0xffu) == 4u)
                copy_text(output, capacity, "Lua out of memory outside a protected call");
            else
                _snprintf_s(output, capacity, _TRUNCATE, "Lua error outside a protected call (status %lu)", code & 0xffu);
            return;
        }

        size_t used = 0;
        append_format(output, capacity, used, "%s (0x%08lX)", exception_name(code), code);
        if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) || record.NumberParameters < 2)
            return;

        const ULONG_PTR operation = record.ExceptionInformation[0];
        const ULONG_PTR target = record.ExceptionInformation[1];
        append_format(output, capacity, used, ", %s at 0x%llX",
            operation == 0 ? "read" : operation == 1 ? "write" : operation == 8 ? "execute (DEP)" : "access",
            static_cast<unsigned long long>(target));
        MEMORY_BASIC_INFORMATION memory = {};
        if (target < 0x10000)
            append_format(output, capacity, used, " (null pointer)");
        else if (VirtualQuery(reinterpret_cast<LPCVOID>(target), &memory, sizeof(memory)))
        {
            if (memory.State == MEM_FREE)
                append_format(output, capacity, used, " (unallocated memory)");
            else if (memory.State == MEM_RESERVE)
                append_format(output, capacity, used, " (reserved, not committed memory)");
            else if (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))
                append_format(output, capacity, used, " (no-access page)");
        }
        if (code == EXCEPTION_IN_PAGE_ERROR && record.NumberParameters >= 3)
            append_format(output, capacity, used, ", I/O status 0x%08llX",
                static_cast<unsigned long long>(record.ExceptionInformation[2]));
    }

    void append_breadcrumbs(char* output, size_t capacity, size_t& used)
    {
        const LONG64 newest = InterlockedCompareExchange64(&g_breadcrumbSequence, 0, 0);
        if (!newest)
            return;
        append_format(output, capacity, used, "Recent events (oldest first):\r\n");
        for (LONG64 sequence = newest > kBreadcrumbsShown ? newest - kBreadcrumbsShown + 1 : 1; sequence <= newest; ++sequence)
        {
            BreadcrumbSlot& slot = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
            if (InterlockedCompareExchange(&slot.busy, 1, 0) != 0)
                continue;
            char message[sizeof(slot.message)] = {};
            if (slot.sequence == sequence)
                copy_text(message, sizeof(message), slot.message);
            InterlockedExchange(&slot.busy, 0);
            if (*message)
                append_format(output, capacity, used, "  %s\r\n", message);
        }
    }

    void append_record_head(char* output, size_t capacity, size_t& used, Category category, Severity severity,
        LPCSTR expression, LPCSTR description, LPCSTR argument0, LPCSTR argument1, LPCSTR file, int line, LPCSTR function)
    {
        char source[512] = {};
        format_source_path(file, source, sizeof(source));
        append_format(output, capacity, used, "\r\n==== %s [%s] ====\r\n",
            severity == Severity::Fatal ? "FATAL" : "ERROR", category_name(category));
        if (g_testFilename)
            append_format(output, capacity, used, "Note      : simulated by 'engine_error_log test', nothing failed\r\n");
        append_field(output, capacity, used, "Error", description);
        append_field(output, capacity, used, "Expression", expression);
        append_field(output, capacity, used, "Details", argument0);
        append_field(output, capacity, used, "Details", argument1);
        append_format(output, capacity, used, "Location  : %s:%d (%s)\r\n", source, line, safe_text(function));
        append_failure_code(output, capacity, used);
        append_thread_context(output, capacity, used);
    }

    void write_record(Category category, Severity severity, LPCSTR expression, LPCSTR description,
        LPCSTR argument0, LPCSTR argument1, LPCSTR file, int line, LPCSTR function, LPCSTR luaStack)
    {
        g_lastWriteSucceeded = false;
        if (!IsEnabled() || g_insideLogger)
            return;

        const DWORD preservedLastError = GetLastError();
        g_insideLogger = true;
        const bool fatal = severity == Severity::Fatal;

        // A fatal error never waits: the lock owner may be the thread that failed.
        if (fatal ? !TryAcquireSRWLockExclusive(&g_writeLock) : (AcquireSRWLockExclusive(&g_writeLock), false))
        {
            char record[4096] = {};
            size_t used = 0;
            append_record_head(record, sizeof(record), used, category, severity,
                expression, description, argument0, argument1, file, line, function);
            append_text_record(category, record);
            g_insideLogger = false;
            SetLastError(preservedLastError);
            return;
        }

        if (ensure_directory_unlocked())
        {
            char* record = g_recordBuffer;
            const size_t capacity = sizeof(g_recordBuffer);
            size_t used = 0;
            record[0] = 0;
            append_record_head(record, capacity, used, category, severity,
                expression, description, argument0, argument1, file, line, function);
            if (fatal || category == Category::Memory)
                append_memory_facts(record, capacity, used);
            if (luaStack && *luaStack)
                append_format(record, capacity, used, "Lua stack:\r\n%s", luaStack);
            else if (fatal)
                append_lua_stack(record, capacity, used);
            if (fatal)
            {
                append_format(record, capacity, used, "Native stack:\r\n");
                // DbgHelp allocates; under OOM the engine's new-handler would end the process first.
                if (category == Category::Memory || !append_symbolized_stack(nullptr, true, record, capacity, used))
                {
                    CONTEXT context = {};
                    RtlCaptureContext(&context);
                    append_raw_stack(context, record, capacity, used);
                }
                append_breadcrumbs(record, capacity, used);
            }

            HANDLE fileHandle = open_log_file(category, FILE_APPEND_DATA);
            if (fileHandle != INVALID_HANDLE_VALUE)
            {
                g_lastWriteSucceeded = write_handle_text(fileHandle, record);
                if (!FlushFileBuffers(fileHandle)) g_lastWriteSucceeded = false;
                CloseHandle(fileHandle);
            }
        }

        ReleaseSRWLockExclusive(&g_writeLock);
        g_insideLogger = false;
        SetLastError(preservedLastError);
    }

    void compose_crash_record(CrashWorkspace& workspace, bool complete)
    {
        char* record = workspace.record;
        const size_t capacity = sizeof(workspace.record);
        size_t used = 0;
        record[0] = 0;
        append_format(record, capacity, used, "\r\n==== CRASH%s ====\r\n",
            complete ? "" : " (the logger stopped before finishing this record)");
        append_field(record, capacity, used, "Error", workspace.error);
        append_field(record, capacity, used, "Where", workspace.where);
        append_field(record, capacity, used, "Module", workspace.module);
        append_thread_context(record, capacity, used);
        if (complete)
            append_memory_facts(record, capacity, used);
        append_format(record, capacity, used, "%s", workspace.lua);
        append_format(record, capacity, used, "Native stack:\r\n%s", *workspace.symbols ? workspace.symbols : workspace.stack);
        if (!complete)
            return;
        append_format(record, capacity, used, "%s", workspace.machine);
        append_breadcrumbs(record, capacity, used);
    }

    void report_crash(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace)
    {
        ZeroMemory(&workspace, sizeof(workspace));
        const EXCEPTION_RECORD& exception = *pointers->ExceptionRecord;
        const DWORD code = exception.ExceptionCode;
        describe_exception(exception, workspace.error, sizeof(workspace.error));
        format_address_location(exception.ExceptionAddress, workspace.where, sizeof(workspace.where));

        char modulePath[MAX_PATH] = {};
        unsigned long long offset = 0;
        HMODULE module = module_from_address(exception.ExceptionAddress, modulePath, sizeof(modulePath), offset);
        const bool systemFault = is_system_module(modulePath); // registers of RaiseException/heap checks say nothing
        if (module && module != GetModuleHandleA(nullptr) && !systemFault)
        {
            char identity[512] = {};
            format_image_identity(module, identity, sizeof(identity));
            _snprintf_s(workspace.module, sizeof(workspace.module), _TRUNCATE, "%s, %s", path_basename(modulePath), identity);
        }

        size_t used = 0;
        if (pointers->ContextRecord)
        {
            workspace.context = *pointers->ContextRecord;
            append_raw_stack(workspace.context, workspace.stack, sizeof(workspace.stack), used);
        }

        // The original failure reaches the disk before DbgHelp loads any PDB.
        compose_crash_record(workspace, false);
        if (!ensure_directory_unlocked())
            return;
        HANDLE file = open_log_file(Category::Crash, GENERIC_WRITE);
        if (file == INVALID_HANDLE_VALUE)
            return;
        const LARGE_INTEGER zero = {};
        LARGE_INTEGER start = {}, end = {}, size = {};
        SetFilePointerEx(file, zero, &start, FILE_CURRENT);
        write_handle_text(file, workspace.record);
        FlushFileBuffers(file);
        SetFilePointerEx(file, zero, &end, FILE_CURRENT);

        const bool constrained = code == EXCEPTION_STACK_OVERFLOW || code == kNoMemory ||
            (is_luajit_exception_code(code) && (code & 0xffu) == 4u);
        if (!constrained && pointers->ContextRecord)
        {
            used = 0;
            append_symbolized_stack(pointers->ContextRecord, false, workspace.symbols, sizeof(workspace.symbols),
                used, workspace.where, sizeof(workspace.where));
        }
        used = 0;
        append_lua_stack(workspace.lua, sizeof(workspace.lua), used);
        used = 0;
        if (!systemFault)
            append_machine_state(pointers, workspace.machine, sizeof(workspace.machine), used);
        compose_crash_record(workspace, true);

        // Replace the checkpoint, unless another thread appended after it.
        if (GetFileSizeEx(file, &size) && size.QuadPart == end.QuadPart)
            SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
        else
            SetFilePointerEx(file, zero, nullptr, FILE_END);
        write_handle_text(file, workspace.record);
        SetEndOfFile(file);
        FlushFileBuffers(file);
        CloseHandle(file);
    }

    void report_simultaneous_crash(EXCEPTION_POINTERS* pointers)
    {
        char error[512] = {}, where[MAX_PATH + 32] = {}, record[2048] = {};
        __try
        {
            describe_exception(*pointers->ExceptionRecord, error, sizeof(error));
            format_address_location(pointers->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        size_t used = 0;
        append_format(record, sizeof(record), used, "\r\n==== CRASH (second thread, while another crash was being recorded) ====\r\n");
        append_field(record, sizeof(record), used, "Error", error);
        append_field(record, sizeof(record), used, "Where", where);
        append_thread_context(record, sizeof(record), used);
        append_text_record(Category::Crash, record);
    }
}

void Initialize()
{
    const DWORD lastError = GetLastError();
    InitOnceExecuteOnce(&g_privacyOnce, initialize_privacy, nullptr, nullptr);
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

void SetThreadStage(LPCSTR stage, const void* callbackAddress)
{
    static thread_local bool stackReserved = false;
    if (!stackReserved)
    {
        // Leave stack for the crash handler after an overflow on this thread.
        const DWORD lastError = GetLastError();
        ULONG reserve = 64u * 1024u;
        SetThreadStackGuarantee(&reserve);
        stackReserved = true;
        SetLastError(lastError);
    }
    g_threadStage = stage;
    g_threadStageCallback = callbackAddress;
}

void ClearThreadStage()
{
    g_threadStage = nullptr;
    g_threadStageCallback = nullptr;
}

void SetThreadName(LPCSTR name)
{
    copy_text(g_threadName, sizeof(g_threadName), name);
}

void AddBreadcrumb(LPCSTR message)
{
    if (!IsEnabled() || !message)
        return;

    const LONG64 sequence = InterlockedIncrement64(&g_breadcrumbSequence);
    BreadcrumbSlot& slot = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
    if (InterlockedCompareExchange(&slot.busy, 1, 0) != 0)
        return;
    if (slot.sequence < sequence)
    {
        SanitizeText(message, slot.message, sizeof(slot.message));
        slot.sequence = sequence;
    }
    InterlockedExchange(&slot.busy, 0);
}

void SetLuaStackWriter(LuaStackWriter writer)
{
    g_luaThread = GetCurrentThreadId();
    g_luaStackWriter = writer;
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

    Category category = classify_source(file, expression, description, argument0, argument1);
    if (contains_script_failure(expression, description, argument0, argument1))
        category = contains_memory_failure(expression, description, argument0, argument1) ? Category::Memory : Category::Script;

    write_record(category, severity, expression, description, argument0, argument1, file, line, function, nullptr);

    if (severity == Severity::Fatal)
    {
        char source[512] = {}, event[160] = {};
        format_source_path(file, source, sizeof(source));
        _snprintf_s(event, sizeof(event), _TRUNCATE, "fatal error at %s:%d", source, line);
        AddBreadcrumb(event);
    }
}

void ReportUnhandledException(void* exceptionPointers)
{
    auto pointers = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    if (!IsEnabled() || !pointers || !pointers->ExceptionRecord || g_insideCrashHandler) return;
    const DWORD lastError = GetLastError();
    g_insideCrashHandler = true;
    if (InterlockedCompareExchange(&g_crashWorkspaceInUse, 1, 0) != 0)
        report_simultaneous_crash(pointers);
    else
    {
        const DWORD code = pointers->ExceptionRecord->ExceptionCode;
        __try { report_crash(pointers, g_crashWorkspace); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char note[256] = {};
            _snprintf_s(note, sizeof(note), _TRUNCATE,
                "\r\n==== CRASH 0x%08lX: the logger failed while recording it; a record above may be incomplete ====\r\n", code);
            append_text_record(Category::Crash, note);
        }
        InterlockedExchange(&g_crashWorkspaceInUse, 0);
    }
    g_insideCrashHandler = false;
    SetLastError(lastError);
}

void OpenLogFolder()
{
    static volatile LONG opened = 0;
    if (!*g_directoryWide || InterlockedExchange(&opened, 1))
        return;
    // A new process, not ShellExecute: no COM or shell extensions inside a failed process.
    wchar_t windows[MAX_PATH] = {}, explorer[MAX_PATH] = {}, command[MAX_PATH * 2] = {};
    if (!GetWindowsDirectoryW(windows, MAX_PATH) ||
        _snwprintf_s(explorer, _countof(explorer), _TRUNCATE, L"%s\\explorer.exe", windows) < 0 ||
        _snwprintf_s(command, _countof(command), _TRUNCATE, L"\"%s\" \"%s\"", explorer, g_directoryWide) < 0)
        return;
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(explorer, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
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
    FailureCodeScope code(result, removedReason, true);
    write_record(Category::Render, Severity::Error, expression, "DX11 API failed",
        nullptr, nullptr, file, line, function, nullptr);
    SetLastError(lastError);
}

bool ShouldReportScriptFailure(int status, bool recoveredModuleLoad, LPCSTR message)
{
    if (status == 0 || status == 1) return false; // success / yield
    if (!recoveredModuleLoad || (status != 2 && status != 3)) return true;
    // Never infer ownership from a filename. Keep unknown error values and
    // native OOM messages converted to LUA_ERRRUN.
    return !message || !*message || contains_memory_failure(nullptr, message, nullptr, nullptr);
}

void ReportScriptFailure(int status, LPCSTR script, LPCSTR message, LPCSTR traceback,
    LPCSTR file, int line, LPCSTR function, bool recoveredModuleLoad)
{
    if (!IsEnabled() || !ShouldReportScriptFailure(status, recoveredModuleLoad, message)) return;
    const DWORD lastError = GetLastError();
    const bool memoryFailure = status == 4 || contains_memory_failure(nullptr, message, nullptr, nullptr);
    char details[512] = {};
    if (script && *script)
        _snprintf_s(details, sizeof(details), _TRUNCATE, "chunk %s", path_basename(script));
    LPCSTR expression = "Lua script error";
    if (memoryFailure) expression = "Lua memory allocation failed";
    else if (status == 2) expression = "Lua runtime error";
    else if (status == 3) expression = "Lua chunk compilation failed";
    write_record(memoryFailure ? Category::Memory : Category::Script, Severity::Error, expression,
        message ? message : "Lua error value is not a string (not coerced during diagnostics)", details, nullptr,
        file, line, function, traceback);
    SetLastError(lastError);
}

bool WriteSelfTest()
{
    if (!IsEnabled() || g_insideLogger || g_testFilename) return false;
    Initialize();
    g_testFilename = "RENDER-LOG-SELFTEST.log";
    ReportRenderFailure(static_cast<long>(0x887A0005u), static_cast<long>(0x887A0006u),
        "SIMULATED ID3D11Device/CreateTexture2D; texture=gamedata/textures/ui/selftest.dds; "
        "install=Z:\\Private Test User\\Private GAMMA Install\\bin\\engine.exe; token=dummy-secret",
        "Layers/xrRenderDX10/diagnostic_selftest.cpp", 42, "EngineErrorLogger::WriteSelfTest");
    g_testFilename = nullptr;
    return g_lastWriteSucceeded;
}
}
