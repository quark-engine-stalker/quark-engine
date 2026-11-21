#include "stdafx.h"
#pragma hdrstop

#include "engine_error_logger.h"
#include "diagnostic_privacy.h"

#include <Windows.h>
#include <ShlObj.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <typeinfo>
#include <winevt.h>

#include "../3rd party/stackwalker/include/StackWalker.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "advapi32.lib")

// vcruntime: the exception record and context of the exception the running catch block handles.
extern "C" void** __cdecl __current_exception();
extern "C" void** __cdecl __current_exception_context();

namespace EngineErrorLogger
{
namespace
{
    constexpr unsigned int kStackFrameLimit = 32;
    constexpr unsigned int kOtherThreadFrames = 12; // crash records: the other engine threads
    constexpr unsigned int kBreadcrumbCount = 128;
    constexpr unsigned int kBreadcrumbsShown = 32;  // lines in a record, a run of similar lines takes three
    constexpr unsigned int kThreadSlots = 128;
    constexpr unsigned int kGameLogTailFatal = 24;
    constexpr DWORD kCppException = 0xE06D7363;
    constexpr DWORD kNoMemory = 0xC0000017;
    constexpr DWORD kHeapCorruption = 0xC0000374;
    constexpr DWORD kHelperWaitMs = 20000;
    constexpr DWORD kSessionMagic = 0x51534553; // "SESQ"
    enum : DWORD { kSessionRunning = 1, kSessionRecorded = 2 };

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
        char threads[65536]; // stacks of the other engine threads
        char gameLog[65536]; // the player's xray log, so it need not be sent separately
        char record[196608];
    };

    volatile LONG g_enabled = 1;
    volatile LONG64 g_breadcrumbSequence = 0;
    volatile LONG g_crashWorkspaceInUse = 0;
    SRWLOCK g_writeLock = SRWLOCK_INIT;
    SRWLOCK g_symbolLock = SRWLOCK_INIT;
    char g_directory[MAX_PATH * 3] = {};
    wchar_t g_directoryWide[MAX_PATH] = {};
    DiagnosticPrivacy::Names g_privateNames;
    INIT_ONCE g_privacyOnce = INIT_ONCE_STATIC_INIT;
    HANDLE g_symbolProcess = nullptr; // unique DbgHelp session, process lifetime
    char g_buildIdentity[512] = {};
    LuaStackWriter g_luaStackWriter = nullptr;
    GameLogWriter g_gameLogWriter = nullptr;
    thread_local bool g_selfTest = false;
    uint64_t g_errorSignatures[128] = {};
    size_t g_nextErrorSignature = 0;

    struct ScriptLoadFailure { char chunk[160], error[768]; unsigned int count; };
    SRWLOCK g_scriptLoadLock = SRWLOCK_INIT;
    ScriptLoadFailure g_scriptLoads[16] = {};
    size_t g_scriptLoadCursor = 0;
    thread_local bool g_lastWriteSucceeded = false;
    struct FailureCodeState { long result = 0, removedReason = 0; bool valid = false, hasRemovedReason = false; };
    thread_local FailureCodeState g_failureCode;
    char g_recordBuffer[131072] = {};
    BreadcrumbSlot g_breadcrumbs[kBreadcrumbCount] = {};
    CrashWorkspace g_crashWorkspace = {};
    thread_local bool g_insideLogger = false;
    thread_local bool g_insideCrashHandler = false;

    // A thread's crash context, read under SEH.
    struct ThreadState
    {
        DWORD id = 0;
        LPCSTR stage = nullptr;
        const void* stageCallback = nullptr;
        LPCSTR objectAction = nullptr;
        const void* object = nullptr;
        ObjectDescriber describer = nullptr;
        const void* stackBase = nullptr;
        char name[64] = {};
    };
    thread_local ThreadState g_thread;
    ThreadState* volatile g_threads[kThreadSlots] = {};
    struct ThreadRegistration
    {
        bool registered = false;
        ~ThreadRegistration()
        {
            for (ThreadState* volatile& slot : g_threads)
                InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&slot), nullptr, &g_thread);
        }
    };
    thread_local ThreadRegistration g_threadRegistration;

    // What the game is: level and save (set by the game).
    char g_contextLevel[64] = {};
    char g_contextSave[192] = {};

    // The symbols of a thread out of stack are walked by a helper thread made at startup: the failing
    // thread may have no stack left or hold a lock.
    struct HelperJob
    {
        bool ok; const CONTEXT* context; char* output; size_t capacity; size_t* used; char* first; size_t firstCapacity;
    };
    HANDLE g_helperRequest = nullptr, g_helperDone = nullptr;
    SRWLOCK g_helperLock = SRWLOCK_INIT;
    HelperJob g_helperJob = {};

    // A fault caught by luabind: the game goes on, so the helper finishes the record. The catch block only
    // copies the frames and the text it alone can give; DbgHelp (the first PDB load takes up to seconds)
    // never stalls the game. One record at a time: the helper owns the job while caughtBusy is set.
    struct CaughtJob
    {
        char title[512];
        char error[1024];
        char context[16384]; // thread, game, memory, Lua stack
        char machine[1024];  // registers, code bytes
        char events[8192];
        char record[81920];
        DWORD64 frames[kStackFrameLimit];
        unsigned int count;
    };
    CaughtJob g_caught = {};
    volatile LONG g_caughtBusy = 0;
    HANDLE g_caughtRequest = nullptr;

    // How the previous process ended, for the deaths Windows does not hand to the process (fast-fail).
    struct SessionMarker { DWORD magic, pid; ULONGLONG created; DWORD state; };
    SessionMarker g_session = {};
    SessionMarker g_previousSession = {};
    wchar_t g_sessionPath[MAX_PATH] = {};
    INIT_ONCE g_sessionOnce = INIT_ONCE_STATIC_INIT;

    unsigned char g_threadStack[1u << 20]; // another thread's stack, copied while it is suspended (crash records)

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

    bool log_path(wchar_t (&path)[MAX_PATH], const wchar_t* stem, unsigned long number)
    {
        return (number == 1
            ? _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\%s.log", g_directoryWide, stem)
            : _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\%s-%lu.log", g_directoryWide, stem, number)) >= 0;
    }

    unsigned long next_log_number(const wchar_t* stem)
    {
        wchar_t pattern[MAX_PATH] = {};
        if (_snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%s\\%s*.log", g_directoryWide, stem) < 0)
            return 1;

        WIN32_FIND_DATAW entry = {};
        HANDLE files = FindFirstFileW(pattern, &entry);
        if (files == INVALID_HANDLE_VALUE)
            return 1;

        unsigned long newest = 0;
        const size_t stemLength = wcslen(stem);
        do
        {
            if (_wcsnicmp(entry.cFileName, stem, stemLength) != 0)
                continue;
            const wchar_t* suffix = entry.cFileName + stemLength;
            if (_wcsicmp(suffix, L".log") == 0)
                newest = (std::max)(newest, 1ul);
            else if (suffix[0] == L'-' && suffix[1] >= L'1' && suffix[1] <= L'9')
            {
                wchar_t* end = nullptr;
                const unsigned long number = wcstoul(suffix + 1, &end, 10);
                if (number >= 2 && number < ULONG_MAX && _wcsicmp(end, L".log") == 0)
                    newest = (std::max)(newest, number);
            }
        } while (FindNextFileW(files, &entry));
        FindClose(files);
        return newest + 1;
    }

    HANDLE open_log_file(Category category)
    {
        if (!*g_directoryWide)
            return INVALID_HANDLE_VALUE;

        wchar_t stem[64] = {}, path[MAX_PATH] = {};
        if (g_selfTest)
            wcscpy_s(stem, L"RENDER-LOG-SELFTEST");
        else if (_snwprintf_s(stem, _countof(stem), _TRUNCATE, L"%hs-LOG", category_name(category)) < 0)
            return INVALID_HANDLE_VALUE;

        unsigned long number = g_selfTest ? 1 : next_log_number(stem);
        bool recreated = false;
        HANDLE file = INVALID_HANDLE_VALUE;
        while (number && log_path(path, stem, number))
        {
            file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                g_selfTest ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
                break;

            const DWORD error = GetLastError();
            if (!g_selfTest && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
            {
                ++number; // Another writer reserved this number first.
                continue;
            }
            if (error != ERROR_PATH_NOT_FOUND || recreated)
                return INVALID_HANDLE_VALUE;

            wchar_t parent[MAX_PATH] = {};
            wcscpy_s(parent, g_directoryWide);
            if (wchar_t* slash = wcsrchr(parent, L'\\'))
                *slash = 0;
            if (!ensure_subdirectory(parent) || !ensure_subdirectory(g_directoryWide))
                return INVALID_HANDLE_VALUE;
            recreated = true;
            number = g_selfTest ? 1 : next_log_number(stem);
        }
        if (file == INVALID_HANDLE_VALUE)
            return file;

        char header[768] = {}, process[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, process, DWORD(sizeof(process)));
        _snprintf_s(header, sizeof(header), _TRUNCATE, "QUARK-ENGINE LOG 2.3 | %s | %s\r\n",
            path_basename(process), safe_text(g_buildIdentity));
        write_handle_text(file, header);
        return file;
    }

    bool append_text_record(Category category, LPCSTR text)
    {
        HANDLE file = open_log_file(category);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        const bool written = write_handle_text(file, text) && FlushFileBuffers(file);
        CloseHandle(file);
        return written;
    }

    bool contains_script_failure(LPCSTR expression, LPCSTR description)
    {
        return contains_i(expression, "lua error:") || contains_i(description, "lua error:");
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

    Category classify_source(LPCSTR file, LPCSTR expression, LPCSTR description, LPCSTR argument0, LPCSTR argument1)
    {
        if (contains_memory_failure(expression, description, argument0, argument1))
            return Category::Memory;

        char source[512] = {};
        format_source_path(file, source, sizeof(source));
        LPCSTR name = path_basename(source);

        if (find_i(source, "Layers/") == source || contains_i(name, "dx11") || contains_i(name, "d3d11") ||
            contains_i(name, "shader") || contains_i(name, "seqrender") || find_i(source, "xrEngine/render/") == source)
            return Category::Render;
        // Debug.fatal puts the Lua error text in Details, after the Lua call site.
        if (contains_script_failure(expression, description) ||
            (find_i(source, "xrServerEntities/") == source && contains_script_failure(argument0, argument1)))
            return Category::Script;
        if (find_i(source, "xrPhysics/") == source || find_i(name, "ph") == name || contains_i(name, "physics"))
            return Category::Physics;
        if (find_i(source, "xrSound/") == source || contains_i(name, "sound_"))
            return Category::Sound;
        if (find_i(source, "xrNetServer/") == source || contains_i(name, "net_") || contains_i(name, "network"))
            return Category::Network;
        if (find_i(source, "xrCDB/") == source || contains_i(name, "collide"))
            return Category::Collision;
        if (contains_i(name, "xrmemory") || contains_i(name, "allocator"))
            return Category::Memory;
        if (find_i(source, "xrCore/") == source)
            return Category::Core;
        if (find_i(source, "xrGame/ai/") == source || contains_i(name, "ai_") ||
            contains_i(name, "alife") || contains_i(name, "path_manager") || contains_i(name, "movement_manager") ||
            contains_i(name, "graph_engine"))
            return Category::AI;
        if (find_i(source, "xrGame/") == source || find_i(source, "xrServerEntities/") == source ||
            find_i(source, "xrParticles/") == source)
            return Category::Game;
        return Category::Engine;
    }

    Category crash_category(DWORD code)
    {
        if (is_luajit_exception_code(code))
            return (code & 0xffu) == 4u ? Category::Memory : Category::Script;
        return code == kNoMemory ? Category::Memory : Category::Crash;
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

    // The object text is made by the game's describer at record time; a broken object only loses this line.
    void describe_object(const ThreadState& state, char* output, size_t capacity)
    {
        *output = 0;
        if (!state.object || !state.describer)
            return;
        __try { state.describer(state.object, output, capacity); }
        __except (EXCEPTION_EXECUTE_HANDLER) { copy_text(output, capacity, "<object unreadable>"); }
        output[capacity - 1] = 0;
    }

    // state is a copy; object is the describer's text, made while the state was current.
    void append_thread_state(const ThreadState& state, LPCSTR object, char* output, size_t capacity, size_t& used,
        LPCSTR callbackSymbol = nullptr)
    {
        if (state.stage)
        {
            append_format(output, capacity, used, "Stage     : ");
            append_single_line_text(output, capacity, used, state.stage);
            if (state.stageCallback)
            {
                char location[MAX_PATH + 32] = {};
                format_address_location(state.stageCallback, location, sizeof(location));
                if (callbackSymbol && *callbackSymbol)
                    append_format(output, capacity, used, " (callback %s [%s])", callbackSymbol, location);
                else
                    append_format(output, capacity, used, " (callback %s)", location);
            }
            append_format(output, capacity, used, "\r\n");
        }
        if (object && *object)
        {
            append_format(output, capacity, used, "Object    : ");
            append_single_line_text(output, capacity, used, object);
            if (state.objectAction)
                append_format(output, capacity, used, " (%s)", state.objectAction);
            append_format(output, capacity, used, "\r\n");
        }
    }

    void append_thread_context(char* output, size_t capacity, size_t& used)
    {
        append_format(output, capacity, used, "Thread    : %s\r\n", *g_thread.name ? g_thread.name : "unnamed");
        char object[256] = {};
        describe_object(g_thread, object, sizeof(object));
        append_thread_state(g_thread, object, output, capacity, used);
    }

    void append_game_context(char* output, size_t capacity, size_t& used)
    {
        if (*g_contextLevel || *g_contextSave)
        {
            append_format(output, capacity, used, "Game      : level %s", *g_contextLevel ? g_contextLevel : "<none>");
            if (*g_contextSave)
                append_format(output, capacity, used, ", save \"%s\"", g_contextSave);
            append_format(output, capacity, used, "\r\n");
        }
    }

    void register_thread()
    {
        if (g_threadRegistration.registered)
            return;
        g_threadRegistration.registered = true;
        g_thread.id = GetCurrentThreadId();
        g_thread.stackBase = reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase;
        for (ThreadState* volatile& slot : g_threads)
            if (!InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&slot), &g_thread, nullptr))
                return;
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
        if (!g_luaStackWriter || used + 64 >= capacity) // the writer itself knows which threads may walk the VM
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

    // Protected module-load errors can precede the crash by an entire level load.
    // Keep original order and collapse repeats independently of the ordinary game log.
    void remember_script_load_failure(LPCSTR script, LPCSTR message)
    {
        char chunk[sizeof(ScriptLoadFailure::chunk)] = {}, error[sizeof(ScriptLoadFailure::error)] = {};
        copy_text(chunk, sizeof(chunk), path_basename(script));
        SanitizeText(message, error, sizeof(error));
        AcquireSRWLockExclusive(&g_scriptLoadLock);
        for (auto& issue : g_scriptLoads)
        {
            if (issue.count && !strcmp(issue.chunk, chunk) && !strcmp(issue.error, error))
            {
                if (issue.count != UINT_MAX)
                    ++issue.count;
                ReleaseSRWLockExclusive(&g_scriptLoadLock);
                return;
            }
        }
        auto& issue = g_scriptLoads[g_scriptLoadCursor++ % _countof(g_scriptLoads)];
        copy_text(issue.chunk, sizeof(issue.chunk), chunk);
        copy_text(issue.error, sizeof(issue.error), error);
        issue.count = 1;
        ReleaseSRWLockExclusive(&g_scriptLoadLock);
    }

    void append_script_load_failures(char* output, size_t capacity, size_t& used)
    {
        if (!TryAcquireSRWLockShared(&g_scriptLoadLock))
            return;
        if (g_scriptLoadCursor)
        {
            append_format(output, capacity, used, "Earlier Lua module-load errors (game continued):\r\n");
            const auto count = (std::min)(g_scriptLoadCursor, _countof(g_scriptLoads));
            for (size_t index = g_scriptLoadCursor - count; index < g_scriptLoadCursor; ++index)
            {
                const auto& issue = g_scriptLoads[index % _countof(g_scriptLoads)];
                append_format(output, capacity, used, "  ");
                if (!contains_i(issue.error, issue.chunk))
                    append_format(output, capacity, used, "%s | ", issue.chunk);
                append_single_line_text(output, capacity, used, issue.error);
                if (issue.count > 1)
                    append_format(output, capacity, used, " (%u occurrences)", issue.count);
                append_format(output, capacity, used, "\r\n");
            }
        }
        ReleaseSRWLockShared(&g_scriptLoadLock);
    }

    // Serialized by g_writeLock. Keep one copy of the same continuing failure per session.
    uint64_t error_signature(Category category, LPCSTR file, int line, LPCSTR expression, LPCSTR description,
        LPCSTR argument0, LPCSTR argument1, LPCSTR luaStack)
    {
        uint64_t hash = 14695981039346656037ull;
        const auto add = [&hash](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        add(static_cast<unsigned int>(category));
        add(line);
        if (g_failureCode.valid)
        {
            add(g_failureCode.result);
            if (g_failureCode.hasRemovedReason)
                add(g_failureCode.removedReason);
        }
        const LPCSTR fields[] = {file, expression, description, argument0, argument1, luaStack};
        for (LPCSTR text : fields)
        {
            if (text)
                while (*text)
                    add(static_cast<unsigned char>(*text++));
            add(0);
        }
        return hash ? hash : 1;
    }

    void append_game_log(char* output, size_t capacity, size_t& used, unsigned int tail)
    {
        if (!g_gameLogWriter || used + 256 >= capacity)
            return;
        const size_t start = used;
        append_format(output, capacity, used, "Game log (xray log of this session):\r\n");
        __try { g_gameLogWriter(output + used, capacity - used, tail); }
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

    // One walker can walk several threads: DbgHelp loads the modules once.
    class LogStackWalker final : public StackWalker
    {
    public:
        LogStackWalker()
            : StackWalker(StackWalker::RetrieveSymbol | StackWalker::RetrieveLine |
                StackWalker::RetrieveModuleInfo,
                local_symbol_path(), GetCurrentProcessId(), g_symbolProcess ? g_symbolProcess : GetCurrentProcess())
        {
        }

        void begin(char* output, size_t capacity, size_t* used, bool skipLogger, unsigned int limit)
        {
            output_ = output; capacity_ = capacity; used_ = used;
            skipping_ = skipLogger; limit_ = limit;
            systemFirst_ = false; frames_ = 0; repeats_ = 0; *first_ = 0; *last_ = 0;
        }

        unsigned int frames() const { return frames_; }
        LPCSTR first_frame() const { return first_; }

        // Symbol, source line and module of one code address (a crash Windows recorded in its event log).
        // Without a line (no PDB) a frame shows how far shown is from the nearest export, as a walk does.
        void describe_address(DWORD64 address, char* output, size_t capacity, DWORD64 shown = 0)
        {
            LoadModules();
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512] = {};
            auto symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            const HANDLE process = g_symbolProcess ? g_symbolProcess : GetCurrentProcess();
            *output = 0;
            if (!SymFromAddr(process, address, &displacement, symbol))
                return;
            size_t used = 0;
            append_format(output, capacity, used, "%.160s", symbol->Name);
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line) && line.FileName)
            {
                char source[512] = {};
                format_source_path(line.FileName, source, sizeof(source));
                append_format(output, capacity, used, " (%s:%lu)", source, line.LineNumber);
            }
            else if (shown >= symbol->Address)
                append_format(output, capacity, used, "+0x%llX", static_cast<unsigned long long>(shown - symbol->Address));
        }

        // A frame copied earlier, printed as a walk prints it. lookup resolves the symbol: a return address
        // minus one, so the line is the call's.
        void describe_frame(DWORD64 address, DWORD64 lookup, char* output, size_t capacity)
        {
            char location[MAX_PATH + 32] = {}, symbol[768] = {};
            format_address_location(reinterpret_cast<const void*>(address), location, sizeof(location));
            describe_address(lookup, symbol, sizeof(symbol), address);
            if (*symbol)
                _snprintf_s(output, capacity, _TRUNCATE, "%s [%s]", symbol, location);
            else
                copy_text(output, capacity, location);
        }

    protected:
        void OnCallstackEntry(CallstackEntryType entryType, CallstackEntry& entry) override
        {
            if (output_ && entryType == lastEntry)
                flush_repeats();
            if (!output_ || entryType == lastEntry || entry.offset == 0 || frames_ >= limit_)
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
            // A recursion repeats one frame: one line for the run, so the frames that started it still fit.
            if (frames_ && !strcmp(frame, last_))
            {
                ++repeats_;
                return;
            }
            flush_repeats();
            copy_text(last_, sizeof(last_), frame);
            append_format(output_, capacity_, *used_, "  #%02u %s\r\n", frames_++, frame);
        }

        void flush_repeats()
        {
            if (repeats_)
                append_format(output_, capacity_, *used_, "      ... the frame above %u more times\r\n", repeats_);
            repeats_ = 0;
        }

        void OnOutput(LPCSTR) override
        {
        }

    private:
        char* output_ = nullptr;
        size_t capacity_ = 0;
        size_t* used_ = nullptr;
        bool skipping_ = false;
        bool systemFirst_ = false;
        unsigned int limit_ = kStackFrameLimit;
        unsigned int frames_ = 0;
        unsigned int repeats_ = 0;
        char first_[1024] = {}; // the first frame outside Windows/CRT modules
        char last_[1024] = {};
    };

    unsigned int walk_stack(const CONTEXT* context, bool skipLogger, char* output, size_t capacity, size_t& used,
        char* first, size_t firstCapacity)
    {
        LogStackWalker walker;
        walker.begin(output, capacity, &used, skipLogger, kStackFrameLimit);
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

    // Code addresses straight from the unwind tables: no DbgHelp, no heap.
    unsigned int capture_frames(CONTEXT& context, DWORD64* frames, unsigned int limit)
    {
        unsigned int count = 0;
#if defined(_M_X64)
        __try
        {
            while (count < limit && context.Rip)
            {
                frames[count++] = context.Rip;
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
        return count;
    }

    // Captured frames as a walk prints them: symbols when walker is given (the caller holds the symbol
    // lock), module+RVA otherwise, a recursion as one run; where gets the first frame outside Windows/CRT.
    void append_frames(LogStackWalker* walker, const DWORD64* frames, unsigned int count, char* output, size_t capacity,
        size_t& used, char* where = nullptr, size_t whereCapacity = 0)
    {
        char last[1024] = {};
        unsigned int printed = 0, repeats = 0;
        bool systemFirst = false;
        for (unsigned int index = 0; index < count; ++index)
        {
            const void* address = reinterpret_cast<const void*>(frames[index]);
            char frame[1024] = {}, module[MAX_PATH] = {};
            unsigned long long offset = 0;
            if (walker)
                walker->describe_frame(frames[index], index ? frames[index] - 1 : frames[index], frame, sizeof(frame));
            else
                format_address_location(address, frame, sizeof(frame));
            module_from_address(address, module, sizeof(module), offset);
            if (where && (!index || (systemFirst && !is_system_module(module))))
            {
                copy_text(where, whereCapacity, frame);
                systemFirst = is_system_module(module);
            }
            if (printed && !strcmp(frame, last))
            {
                ++repeats;
                continue;
            }
            if (repeats)
                append_format(output, capacity, used, "      ... the frame above %u more times\r\n", repeats);
            repeats = 0;
            copy_text(last, sizeof(last), frame);
            append_format(output, capacity, used, "  #%02u %s\r\n", printed++, frame);
        }
        if (repeats)
            append_format(output, capacity, used, "      ... the frame above %u more times\r\n", repeats);
    }

    void symbolize_frames(const DWORD64* frames, unsigned int count, char* output, size_t capacity, size_t& used,
        char* where, size_t whereCapacity)
    {
        LogStackWalker walker;
        append_frames(&walker, frames, count, output, capacity, used, where, whereCapacity);
    }

    // Module+RVA frames: they identify the code with the matching PDB if symbolization fails.
    void append_raw_stack(CONTEXT& context, char* output, size_t capacity, size_t& used)
    {
        DWORD64 frames[kStackFrameLimit] = {};
        append_frames(nullptr, frames, capture_frames(context, frames, kStackFrameLimit), output, capacity, used);
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

    bool read_breadcrumb(LONG64 sequence, char* message, size_t capacity)
    {
        BreadcrumbSlot& slot = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
        *message = 0;
        if (InterlockedCompareExchange(&slot.busy, 1, 0) != 0)
            return false;
        if (slot.sequence == sequence)
            copy_text(message, capacity, slot.message);
        InterlockedExchange(&slot.busy, 0);
        return *message != 0;
    }

    // Lines of one kind ("! Fallback to default bump map: <texture>") differ only after their first ':'.
    bool similar_breadcrumbs(LPCSTR left, LPCSTR right)
    {
        if (!strchr(left, ':'))
            return !strcmp(left, right);
        // Up to the first ':', digits apart: "fallback #7: a" and "fallback #8: b" are one kind.
        for (;; ++left, ++right)
        {
            while (*left >= '0' && *left <= '9') ++left;
            while (*right >= '0' && *right <= '9') ++right;
            if (*left != *right)
                return false;
            if (*left == ':')
                return true;
        }
    }

    void append_breadcrumb_run(char* output, size_t capacity, size_t& used, LPCSTR first, LPCSTR last, unsigned int count)
    {
        append_format(output, capacity, used, "  %s\r\n", first);
        if (count > 2)
            append_format(output, capacity, used, "  ... %u more like it ...\r\n", count - 2);
        if (count > 1)
            append_format(output, capacity, used, "  %s\r\n", last);
    }

    // A run of similar lines shows its first and last line, so a burst of warnings does not push the
    // events before it out of the record.
    void append_breadcrumbs(char* output, size_t capacity, size_t& used)
    {
        const LONG64 newest = InterlockedCompareExchange64(&g_breadcrumbSequence, 0, 0);
        if (!newest)
            return;
        const LONG64 oldest = newest > kBreadcrumbCount ? newest - kBreadcrumbCount + 1 : 1;
        char line[sizeof(BreadcrumbSlot::message)] = {}, run[sizeof(line)] = {};
        LONG64 start = newest + 1;
        unsigned int lines = 0, runLength = 0;
        for (LONG64 sequence = newest; sequence >= oldest; --sequence)
        {
            if (!read_breadcrumb(sequence, line, sizeof(line)))
                continue;
            const bool same = runLength && similar_breadcrumbs(run, line);
            const unsigned int added = same ? (runLength < 3 ? 1u : 0u) : 1u;
            if (lines + added > kBreadcrumbsShown)
                break;
            lines += added;
            runLength = same ? runLength + 1 : 1;
            if (!same)
                copy_text(run, sizeof(run), line);
            start = sequence;
        }

        append_format(output, capacity, used, "Recent events (oldest first):\r\n");
        char last[sizeof(line)] = {};
        runLength = 0;
        for (LONG64 sequence = start; sequence <= newest; ++sequence)
        {
            if (!read_breadcrumb(sequence, line, sizeof(line)))
                continue;
            if (runLength && similar_breadcrumbs(run, line))
            {
                ++runLength;
                copy_text(last, sizeof(last), line);
                continue;
            }
            if (runLength)
                append_breadcrumb_run(output, capacity, used, run, last, runLength);
            copy_text(run, sizeof(run), line);
            runLength = 1;
        }
        if (runLength)
            append_breadcrumb_run(output, capacity, used, run, last, runLength);
    }

    void write_session_state(DWORD state)
    {
        if (!*g_sessionPath || !g_session.magic)
            return;
        g_session.state = state;
        HANDLE file = CreateFileW(g_sessionPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(file, &g_session, sizeof(g_session), &written, nullptr);
        CloseHandle(file);
    }

    void __cdecl clean_session_exit()
    {
        // A caught fault's record may still be on the helper thread: let it reach the disk.
        for (int waited = 0; waited < 5000 && InterlockedCompareExchange(&g_caughtBusy, 0, 0); waited += 10)
            Sleep(10);
        if (*g_sessionPath && g_session.state == kSessionRunning)
            DeleteFileW(g_sessionPath);
    }

    // The CRASH CAUGHT record from what the catch block copied, symbols resolved here.
    void finish_caught_record()
    {
        CaughtJob& job = g_caught;
        char stack[24576] = {}, where[1536] = {};
        size_t stackUsed = 0;
        bool symbols = false;
        for (int attempt = 0; attempt < 1000 && !(symbols = TryBeginSymbolWork()); ++attempt)
            Sleep(10); // a crash or a fatal walks a stack now; this record is not in a hurry
        if (symbols)
        {
            __try { symbolize_frames(job.frames, job.count, stack, sizeof(stack), stackUsed, where, sizeof(where)); }
            __except (EXCEPTION_EXECUTE_HANDLER) { stackUsed = 0; *stack = 0; *where = 0; }
            EndSymbolWork();
        }
        if (!stackUsed)
            append_frames(nullptr, job.frames, job.count, stack, sizeof(stack), stackUsed, where, sizeof(where));

        size_t used = 0;
        char* record = job.record;
        const size_t capacity = sizeof(job.record);
        append_format(record, capacity, used, "\r\n==== CRASH CAUGHT (the game went on: %s) ====\r\n",
            job.title);
        append_field(record, capacity, used, "Error", job.error);
        append_field(record, capacity, used, "Where", where);
        append_format(record, capacity, used, "%sNative stack:\r\n%s%s%s", job.context, stack, job.machine, job.events);
        append_script_load_failures(record, capacity, used);
        append_game_log(record, capacity, used, kGameLogTailFatal);
        append_text_record(Category::Crash, record);
    }

    DWORD WINAPI helper_thread(LPVOID requests)
    {
        g_insideCrashHandler = true; // a failure in here must not start another crash record
        for (;;)
        {
            const DWORD signaled = WaitForMultipleObjects(2, static_cast<HANDLE*>(requests), FALSE, INFINITE);
            if (signaled == WAIT_OBJECT_0)
            {
                __try
                {
                    const HelperJob& job = g_helperJob;
                    g_helperJob.ok = append_symbolized_stack(job.context, false, job.output, job.capacity, *job.used,
                        job.first, job.firstCapacity) != 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_helperJob.ok = false; }
                SetEvent(g_helperDone);
            }
            else if (signaled == WAIT_OBJECT_0 + 1)
            {
                __try { finish_caught_record(); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                InterlockedExchange(&g_caughtBusy, 0);
            }
            else
                return 0;
        }
    }

    // The failing thread waits; the helper walks its stack.
    bool run_on_helper(const HelperJob& job)
    {
        if (!g_helperRequest)
            return false;
        AcquireSRWLockExclusive(&g_helperLock);
        g_helperJob = job;
        ResetEvent(g_helperDone);
        SetEvent(g_helperRequest);
        const bool ok = WaitForSingleObject(g_helperDone, kHelperWaitMs) == WAIT_OBJECT_0 && g_helperJob.ok;
        ReleaseSRWLockExclusive(&g_helperLock);
        return ok;
    }

    void append_record_head(char* output, size_t capacity, size_t& used, Category category, Severity severity,
        LPCSTR expression, LPCSTR description, LPCSTR argument0, LPCSTR argument1, LPCSTR file, int line, LPCSTR function)
    {
        char source[512] = {};
        format_source_path(file, source, sizeof(source));
        append_format(output, capacity, used, "\r\n==== %s [%s] ====\r\n",
            severity == Severity::Fatal ? "FATAL" : "ERROR", category_name(category));
        if (g_selfTest)
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
            // Reserve half of the emergency record for the game-log tail.
            append_record_head(record, sizeof(record) / 2, used, category, severity,
                expression, description, argument0, argument1, file, line, function);
            append_script_load_failures(record, sizeof(record), used);
            g_lastWriteSucceeded = append_text_record(category, record);
            g_insideLogger = false;
            SetLastError(preservedLastError);
            return;
        }

        const uint64_t signature = !fatal && !g_selfTest
            ? error_signature(category, file, line, expression, description, argument0, argument1, luaStack)
            : 0;
        if (signature)
        {
            for (const uint64_t previous : g_errorSignatures)
                if (previous == signature)
                {
                    ReleaseSRWLockExclusive(&g_writeLock);
                    g_insideLogger = false;
                    SetLastError(preservedLastError);
                    return;
                }
        }
        if (ensure_directory_unlocked())
        {
            char* record = g_recordBuffer;
            const size_t capacity = sizeof(g_recordBuffer);
            size_t used = 0;
            record[0] = 0;
            append_record_head(record, capacity, used, category, severity,
                expression, description, argument0, argument1, file, line, function);
            append_game_context(record, capacity, used);
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
                append_script_load_failures(record, capacity, used);
                append_game_log(record, capacity, used, kGameLogTailFatal);
            }
            else if (category == Category::Render || category == Category::Memory)
                append_game_log(record, capacity, used, category == Category::Memory ? kGameLogTailFatal : 0);

            g_lastWriteSucceeded = append_text_record(category, record);
            if (signature && g_lastWriteSucceeded)
                g_errorSignatures[g_nextErrorSignature++ % _countof(g_errorSignatures)] = signature;
        }

        ReleaseSRWLockExclusive(&g_writeLock);
        g_insideLogger = false;
        SetLastError(preservedLastError);
    }

    void append_other_threads(char* output, size_t capacity, size_t& used);

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
        {
            append_game_context(record, capacity, used);
            append_memory_facts(record, capacity, used);
        }
        append_format(record, capacity, used, "%s", workspace.lua);
        append_format(record, capacity, used, "Native stack:\r\n%s", *workspace.symbols ? workspace.symbols : workspace.stack);
        if (complete)
        {
            append_format(record, capacity, used, "%s", workspace.machine);
            append_breadcrumbs(record, capacity, used);
            append_format(record, capacity, used, "%s", workspace.threads);
        }
        append_script_load_failures(record, capacity, used);
        append_format(record, capacity, used, "%s", workspace.gameLog);
    }

    void report_crash(EXCEPTION_POINTERS* pointers, CrashWorkspace& workspace, bool& checkpoint)
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

        // Keep the original fault and the game log on disk before walking optional stacks.
        used = 0;
        append_game_log(workspace.gameLog, sizeof(workspace.gameLog), used, kGameLogTailFatal);
        compose_crash_record(workspace, false);
        if (!ensure_directory_unlocked())
            return;
        HANDLE file = open_log_file(crash_category(code));
        if (file == INVALID_HANDLE_VALUE)
            return;
        __try
        {
            LARGE_INTEGER start = {};
            checkpoint = SetFilePointerEx(file, LARGE_INTEGER{}, &start, FILE_CURRENT) &&
                write_handle_text(file, workspace.record) && FlushFileBuffers(file);
            if (checkpoint)
                write_session_state(kSessionRecorded);

            const bool constrained = code == EXCEPTION_STACK_OVERFLOW || code == kNoMemory ||
                (is_luajit_exception_code(code) && (code & 0xffu) == 4u);
            if (!constrained && pointers->ContextRecord)
            {
                used = 0;
                append_symbolized_stack(pointers->ContextRecord, false, workspace.symbols, sizeof(workspace.symbols),
                    used, workspace.where, sizeof(workspace.where));
            }
            else if (pointers->ContextRecord)
            {
                // Out of stack or memory here: the helper thread walks this thread's stack with symbols.
                used = 0;
                HelperJob job = {};
                job.context = pointers->ContextRecord; job.output = workspace.symbols; job.capacity = sizeof(workspace.symbols);
                job.used = &used; job.first = workspace.where; job.firstCapacity = sizeof(workspace.where);
                if (!run_on_helper(job))
                    workspace.symbols[0] = 0;
            }
            used = 0;
            append_lua_stack(workspace.lua, sizeof(workspace.lua), used);
            used = 0;
            if (!systemFault)
                append_machine_state(pointers, workspace.machine, sizeof(workspace.machine), used);
            used = 0;
            if (!constrained && code != kHeapCorruption) // a broken heap: no more DbgHelp work than the main stack
                append_other_threads(workspace.threads, sizeof(workspace.threads), used);
            if (!*workspace.gameLog)
            {
                // The history lock may have been busy when we wrote the checkpoint.
                used = 0;
                append_game_log(workspace.gameLog, sizeof(workspace.gameLog), used, kGameLogTailFatal);
            }
            compose_crash_record(workspace, true);

            // This incident owns the file. Keep the flushed checkpoint if optional diagnostics faulted.
            if (SetFilePointerEx(file, start, nullptr, FILE_BEGIN) && write_handle_text(file, workspace.record) &&
                SetEndOfFile(file) && FlushFileBuffers(file) && !checkpoint)
                write_session_state(kSessionRecorded);
        }
        __finally { CloseHandle(file); }
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
        append_text_record(crash_category(pointers->ExceptionRecord->ExceptionCode), record);
    }

    // A thread's stack as it was at the suspension: DbgHelp walks this copy after the thread runs again.
    struct StackCopy { DWORD64 low, high; const unsigned char* data; };

    BOOL __stdcall read_stack_copy(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read, LPVOID user)
    {
        const StackCopy& copy = *static_cast<const StackCopy*>(user);
        if (address >= copy.low && address + size <= copy.high)
        {
            memcpy(buffer, copy.data + (address - copy.low), size);
            if (read) *read = size;
            return TRUE;
        }
        SIZE_T done = 0;
        const BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &done);
        if (read) *read = DWORD(done);
        return ok;
    }

    // While the thread is suspended: no heap, no locks, nothing it could be holding.
    bool suspend_and_copy(HANDLE thread, ThreadState* live, ThreadState& state, char* object, size_t objectCapacity,
        CONTEXT& context, StackCopy& copy)
    {
        if (SuspendThread(thread) == DWORD(-1))
            return false;
        bool ok = false;
        __try
        {
            state = *live;
            describe_object(state, object, objectCapacity);
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context))
            {
                const DWORD64 low = context.Rsp, high = reinterpret_cast<DWORD64>(state.stackBase);
                const DWORD64 size = high > low ? high - low : 0;
                copy = { low, low + (size < sizeof(g_threadStack) ? size : sizeof(g_threadStack)), g_threadStack };
                memcpy(g_threadStack, reinterpret_cast<const void*>(low), size_t(copy.high - copy.low));
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        ResumeThread(thread);
        return ok;
    }

    bool ends_with(LPCSTR text, LPCSTR tail)
    {
        const size_t length = text ? strlen(text) : 0, tailLength = strlen(tail);
        return length >= tailLength && !strcmp(text + length - tailLength, tail);
    }

    // A thread parked in a kernel wait with no work of its own: one name in the "Idle" line, no stack.
    bool is_parked(LogStackWalker& walker, const ThreadState& state, const CONTEXT& context)
    {
        if (state.stage && !ends_with(state.stage, "/idle"))
            return false;
        char top[768] = {};
        walker.describe_address(context.Rip, top, sizeof(top));
        return (!strncmp(top, "Zw", 2) || !strncmp(top, "Nt", 2)) &&
            (strstr(top, "Wait") || strstr(top, "Delay") || strstr(top, "Remove"));
    }

    void append_other_thread(LogStackWalker& walker, ThreadState* live, char* output, size_t capacity, size_t& used,
        char* idle, size_t idleCapacity, size_t& idleUsed)
    {
        ThreadState state = {};
        __try { state.id = live->id; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (!state.id || state.id == GetCurrentThreadId())
            return;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, state.id);
        if (!thread)
            return;
        char object[256] = {};
        CONTEXT context = {};
        StackCopy copy = {};
        const bool copied = suspend_and_copy(thread, live, state, object, sizeof(object), context, copy);
        state.name[sizeof(state.name) - 1] = 0;
        if (copied && is_parked(walker, state, context))
        {
            append_format(idle, idleCapacity, idleUsed, "%s%s", idleUsed ? ", " : "", *state.name ? state.name : "unnamed");
            CloseHandle(thread);
            return;
        }
        char callback[768] = {};
        if (state.stageCallback)
            walker.describe_address(reinterpret_cast<DWORD64>(state.stageCallback), callback, sizeof(callback));
        append_format(output, capacity, used, "-- %s\r\n", *state.name ? state.name : "unnamed");
        append_thread_state(state, object, output, capacity, used, callback);
        if (copied)
        {
            walker.begin(output, capacity, &used, false, kOtherThreadFrames);
            __try { walker.ShowCallstack(thread, &context, &read_stack_copy, &copy); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        CloseHandle(thread);
    }

    void walk_registered_threads(LogStackWalker& walker, char* output, size_t capacity, size_t& used)
    {
        char idle[2048] = {};
        size_t idleUsed = 0;
        for (ThreadState* volatile& slot : g_threads)
        {
            ThreadState* live = slot;
            __try
            {
                if (live)
                    append_other_thread(walker, live, output, capacity, used, idle, sizeof(idle), idleUsed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (idleUsed)
            append_format(output, capacity, used, "Idle      : %s\r\n", idle);
    }

    void walk_other_threads(char* output, size_t capacity, size_t& used)
    {
        LogStackWalker walker;
        walk_registered_threads(walker, output, capacity, used);
    }

    // Include a few frames from each of the other engine threads.
    void append_other_threads(char* output, size_t capacity, size_t& used)
    {
        if (!TryBeginSymbolWork())
            return;
        const size_t start = used;
        append_format(output, capacity, used, "Other threads:\r\n");
        const size_t header = used;
        __try { walk_other_threads(output, capacity, used); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        EndSymbolWork();
        if (used == header)
        {
            used = start;
            output[used] = 0;
        }
    }

    // Event data as text: strings as they are, numbers in hex, the way Application Error writes them.
    void event_value_text(const EVT_VARIANT& value, char* output, size_t capacity)
    {
        *output = 0;
        switch (value.Type & EVT_VARIANT_TYPE_MASK)
        {
        case EvtVarTypeString:
            if (value.StringVal)
                WideCharToMultiByte(CP_UTF8, 0, value.StringVal, -1, output, int(capacity), nullptr, nullptr);
            break;
        case EvtVarTypeUInt16: _snprintf_s(output, capacity, _TRUNCATE, "%x", value.UInt16Val); break;
        case EvtVarTypeUInt32: case EvtVarTypeHexInt32: _snprintf_s(output, capacity, _TRUNCATE, "%lx", value.UInt32Val); break;
        case EvtVarTypeUInt64: case EvtVarTypeHexInt64: case EvtVarTypeFileTime:
            _snprintf_s(output, capacity, _TRUNCATE, "%llx", value.UInt64Val); break;
        }
    }

    bool hex_equals(LPCSTR text, ULONGLONG value)
    {
        if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            text += 2;
        char* end = nullptr;
        return *text && _strtoui64(text, &end, 16) == value && end && !*end;
    }

    // Application Error writes the process id and creation time in decimal on Windows 11, in hex before.
    bool number_equals(LPCSTR text, ULONGLONG value)
    {
        char* end = nullptr;
        return hex_equals(text, value) || (*text && _strtoui64(text, &end, 10) == value && end && !*end);
    }

    struct PreviousCrash
    {
        char values[16][256];
        DWORD count;
    };

    // Match the pid and creation time to exclude crashes of other processes.
    bool find_previous_event(const SessionMarker& previous, PreviousCrash& found)
    {
        const EVT_HANDLE query = EvtQuery(nullptr, L"Application",
            L"*[System[EventID=1000 and TimeCreated[timediff(@SystemTime) <= 1209600000]]]",
            EvtQueryChannelPath | EvtQueryReverseDirection);
        if (!query)
            return false;
        const EVT_HANDLE userContext = EvtCreateRenderContext(0, nullptr, EvtRenderContextUser);
        static BYTE buffer[65536];
        bool matched = false;
        EVT_HANDLE events[16] = {};
        DWORD returned = 0;
        for (int batch = 0; !matched && batch < 32 && EvtNext(query, 16, events, 0, 0, &returned); ++batch)
        {
            for (DWORD index = 0; index < returned; ++index)
            {
                DWORD size = 0, count = 0;
                if (!matched && EvtRender(userContext, events[index], EvtRenderEventValues, sizeof(buffer), buffer, &size, &count))
                {
                    const auto values = reinterpret_cast<const EVT_VARIANT*>(buffer);
                    found.count = count < 16 ? count : 16;
                    bool pid = false, created = false;
                    for (DWORD value = 0; value < found.count; ++value)
                    {
                        event_value_text(values[value], found.values[value], sizeof(found.values[value]));
                        pid = pid || number_equals(found.values[value], previous.pid);
                        created = created || number_equals(found.values[value], previous.created);
                    }
                    matched = pid && created;
                }
                EvtClose(events[index]);
            }
        }
        EvtClose(userContext);
        EvtClose(query);
        return matched;
    }

    void describe_previous_address(DWORD64 address, char* output, size_t capacity)
    {
        LogStackWalker walker;
        walker.describe_address(address, output, capacity);
    }

    // Only a crash Windows recorded: an end from outside (Task Manager, power loss) or a closed hang is no
    // bug report. Returns false when there is nothing to write.
    bool compose_previous_session_record(const PreviousCrash& event, char* record, size_t capacity)
    {
        size_t used = 0;
        if (event.count >= 8)
        {
            // AppName, AppVersion, AppTimeStamp, ModuleName, ModuleVersion, ModuleTimeStamp, ExceptionCode, FaultingOffset
            const auto& v = event.values;
            const DWORD code = DWORD(_strtoui64(v[6], nullptr, 16));
            const ULONGLONG offset = _strtoui64(v[7], nullptr, 16);
            append_format(record, capacity, used,
                "\r\n==== CRASH (previous session: Windows ended the game without letting the engine record it) ====\r\n");
            char error[256] = {};
            _snprintf_s(error, sizeof(error), _TRUNCATE, "%s (0x%08lX)", exception_name(code), code);
            append_field(record, capacity, used, "Error", error);
            char where[1024] = {};
            size_t whereUsed = 0;
            append_format(where, sizeof(where), whereUsed, "%s+0x%llX", v[3], offset);
            // The same build as now: the address resolves with this session's symbols.
            char process[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, process, DWORD(sizeof(process)));
            const auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            if (!_stricmp(v[3], path_basename(process)) && hex_equals(v[5], nt->FileHeader.TimeDateStamp) &&
                offset < nt->OptionalHeader.SizeOfImage && TryBeginSymbolWork())
            {
                char symbol[768] = {};
                __try { describe_previous_address(reinterpret_cast<DWORD64>(base) + offset, symbol, sizeof(symbol)); }
                __except (EXCEPTION_EXECUTE_HANDLER) { *symbol = 0; }
                EndSymbolWork();
                if (*symbol)
                {
                    whereUsed = 0;
                    append_format(where, sizeof(where), whereUsed, "%s [%s+0x%llX]", symbol, v[3], offset);
                }
            }
            append_field(record, capacity, used, "Where", where);
            char module[512] = {};
            _snprintf_s(module, sizeof(module), _TRUNCATE, "%s, PE timestamp 0x%s", v[3], v[5]); // its version looks like an IP to the sanitizer
            append_field(record, capacity, used, "Module", module);
            append_field(record, capacity, used, "Note", code == 0xC0000409 ?
                "a fast-fail: the C runtime or Windows found broken state (a stack buffer overrun, an invalid "
                "parameter, a failed security check) and ended the process at once, past every crash handler" :
                "the engine's crash handler did not run: another program replaced it, or the process died inside it");
            return true;
        }
        return false;
    }

    DWORD WINAPI previous_session_thread(LPVOID)
    {
        // Windows Error Reporting writes its event a few seconds after the process died.
        static PreviousCrash event = {};
        bool found = false;
        for (int attempt = 0; attempt < 3 && !(found = find_previous_event(g_previousSession, event)); ++attempt)
            Sleep(3000);
        static char record[8192];
        if (!found || !compose_previous_session_record(event, record, sizeof(record)))
            return 0;
        const DWORD code = DWORD(_strtoui64(event.values[6], nullptr, 16));
        append_text_record(crash_category(code), record);
        return 0;
    }

    BOOL CALLBACK start_session(PINIT_ONCE, PVOID, PVOID*)
    {
        wchar_t appData[MAX_PATH] = {};
        if (!*g_directoryWide)
            return TRUE;
        wcscpy_s(appData, g_directoryWide);
        if (wchar_t* slash = wcsrchr(appData, L'\\'))
            *slash = 0; // ...\QUARK-ENGINE: the marker is no log, it stays out of the ERROR folder
        if (_snwprintf_s(g_sessionPath, _countof(g_sessionPath), _TRUNCATE, L"%s\\error-session.bin", appData) < 0)
            return TRUE;

        HANDLE file = CreateFileW(g_sessionPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD read = 0;
            if (!ReadFile(file, &g_previousSession, sizeof(g_previousSession), &read, nullptr) || read != sizeof(g_previousSession) ||
                g_previousSession.magic != kSessionMagic)
                g_previousSession = {};
            CloseHandle(file);
        }

        FILETIME created = {}, exited = {}, kernel = {}, user = {};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        g_session = { kSessionMagic, GetCurrentProcessId(),
            ULONGLONG(created.dwHighDateTime) << 32 | created.dwLowDateTime, kSessionRunning };
        write_session_state(kSessionRunning);
        atexit(clean_session_exit);

        if (g_previousSession.magic && g_previousSession.state != kSessionRecorded)
            if (HANDLE thread = CreateThread(nullptr, 0, previous_session_thread, nullptr, 0, nullptr))
                CloseHandle(thread);

        // Created now: at the time of a crash a new thread could wait on the loader lock the failing thread holds.
        static HANDLE requests[2] = { CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr) };
        g_helperDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = requests[0] && requests[1] && g_helperDone ?
            CreateThread(nullptr, 256 * 1024, helper_thread, requests, 0, nullptr) : nullptr;
        if (thread)
        {
            g_helperRequest = requests[0];
            g_caughtRequest = requests[1];
            CloseHandle(thread);
        }
        return TRUE;
    }
}

void Initialize()
{
    const DWORD lastError = GetLastError();
    InitOnceExecuteOnce(&g_privacyOnce, initialize_privacy, nullptr, nullptr);
    AcquireSRWLockExclusive(&g_writeLock);
    ensure_directory_unlocked();
    if (!*g_buildIdentity)
    {
        format_image_identity(GetModuleHandleA(nullptr), g_buildIdentity, sizeof(g_buildIdentity));
        if (char* age = strstr(g_buildIdentity, "} age ")) // the header names the build by its PDB GUID alone
            age[1] = 0;
    }
    ReleaseSRWLockExclusive(&g_writeLock);
    if (IsEnabled())
        InitOnceExecuteOnce(&g_sessionOnce, start_session, nullptr, nullptr);
    SetLastError(lastError);
}

ObjectScope::ObjectScope(LPCSTR action, const void* object, ObjectDescriber describer)
    : previousAction_(g_thread.objectAction), previousObject_(g_thread.object), previousDescriber_(g_thread.describer)
{
    g_thread.objectAction = action;
    g_thread.describer = describer;
    g_thread.object = object;
}

ObjectScope::~ObjectScope()
{
    g_thread.object = previousObject_;
    g_thread.describer = previousDescriber_;
    g_thread.objectAction = previousAction_;
}

void SetContext(Context item, LPCSTR value)
{
    switch (item)
    {
    case Context::Level: copy_text(g_contextLevel, sizeof(g_contextLevel), value); break;
    case Context::Save: copy_text(g_contextSave, sizeof(g_contextSave), value); break;
    }
}

void ReportCaughtException(LPCSTR where)
{
    if (!IsEnabled() || g_insideLogger || g_insideCrashHandler)
        return;
    // vcruntime keeps the exception a catch block handles; on x64 the block runs above the faulting
    // frames, so their stack is still there to copy.
    const auto record = static_cast<EXCEPTION_RECORD*>(*__current_exception());
    const auto context = static_cast<CONTEXT*>(*__current_exception_context());
    if (!record || !context)
        return;
    const DWORD code = record->ExceptionCode;
    if (code == kCppException || is_luajit_exception_code(code))
        return; // a thrown error (luabind::error, a Lua error) is the code's own way of failing
    // The helper still writes the previous one: the same fault in a loop, as a rule.
    if (!g_caughtRequest || InterlockedCompareExchange(&g_caughtBusy, 1, 0) != 0)
        return;
    static volatile LONG reported = 0;
    if (InterlockedIncrement(&reported) > 5)
    {
        InterlockedExchange(&g_caughtBusy, 0);
        return; // a fault that keeps coming back: the first records tell it
    }
    const DWORD lastError = GetLastError();
    g_insideLogger = true;
    CaughtJob& job = g_caught;
    copy_text(job.title, sizeof(job.title), safe_text(where));
    describe_exception(*record, job.error, sizeof(job.error));
    size_t used = 0;
    job.context[0] = job.machine[0] = job.events[0] = 0;
    append_thread_context(job.context, sizeof(job.context), used);
    append_game_context(job.context, sizeof(job.context), used);
    append_memory_facts(job.context, sizeof(job.context), used);
    append_lua_stack(job.context, sizeof(job.context), used);
    used = 0;
    EXCEPTION_POINTERS pointers = { record, context };
    append_machine_state(&pointers, job.machine, sizeof(job.machine), used);
    used = 0;
    append_breadcrumbs(job.events, sizeof(job.events), used);
    CONTEXT copy = *context;
    job.count = capture_frames(copy, job.frames, kStackFrameLimit);
    g_insideLogger = false;
    SetEvent(g_caughtRequest);
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
        register_thread();
        SetLastError(lastError);
    }
    g_thread.stage = stage;
    g_thread.stageCallback = callbackAddress;
}

void ClearThreadStage()
{
    g_thread.stage = nullptr;
    g_thread.stageCallback = nullptr;
}

void SetThreadName(LPCSTR name)
{
    copy_text(g_thread.name, sizeof(g_thread.name), name);
    register_thread();
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
    g_luaStackWriter = writer;
}

void SetGameLogWriter(GameLogWriter writer)
{
    g_gameLogWriter = writer;
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

    const Category category = classify_source(file, expression, description, argument0, argument1);

    write_record(category, severity, expression, description, argument0, argument1, file, line, function, nullptr);

    if (severity == Severity::Fatal)
    {
        if (g_lastWriteSucceeded)
            write_session_state(kSessionRecorded);
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
        bool checkpoint = false;
        __try { report_crash(pointers, g_crashWorkspace, checkpoint); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char note[256] = {};
            _snprintf_s(note, sizeof(note), _TRUNCATE,
                "\r\n==== CRASH 0x%08lX: the logger failed while recording it; a record above may be incomplete ====\r\n", code);
            if (!checkpoint && append_text_record(crash_category(code), note))
                write_session_state(kSessionRecorded);
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
    if (!IsEnabled() || status == 0 || status == 1) return;
    const DWORD lastError = GetLastError();
    if (!ShouldReportScriptFailure(status, recoveredModuleLoad, message))
    {
        remember_script_load_failure(script, message);
        SetLastError(lastError);
        return;
    }
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
    if (!IsEnabled() || g_insideLogger || g_selfTest) return false;
    Initialize();
    g_selfTest = true;
    ReportRenderFailure(static_cast<long>(0x887A0005u), static_cast<long>(0x887A0006u),
        "SIMULATED ID3D11Device/CreateTexture2D; texture=gamedata/textures/ui/selftest.dds; "
        "install=Z:\\Private Test User\\Private GAMMA Install\\bin\\engine.exe; token=dummy-secret",
        "Layers/xrRenderDX10/diagnostic_selftest.cpp", 42, "EngineErrorLogger::WriteSelfTest");
    g_selfTest = false;
    return g_lastWriteSucceeded;
}
}
