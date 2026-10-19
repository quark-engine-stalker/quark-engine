#include <windows.h>

#include <cstdio>
#include <cstdlib>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "luajit.h"
}

static double elapsed_ms(const LARGE_INTEGER& begin, const LARGE_INTEGER& end, const LARGE_INTEGER& frequency)
{
    return static_cast<double>(end.QuadPart - begin.QuadPart) * 1000.0 /
        static_cast<double>(frequency.QuadPart);
}

static int finalizer_calls = 0;
static int finalizer_order_errors = 0;
static int expected_finalizer_index = 0;

static int noop_finalizer(lua_State* state)
{
    ++finalizer_calls;
    if (expected_finalizer_index > 0)
    {
        const unsigned char actual = *static_cast<unsigned char*>(lua_touserdata(state, 1));
        const unsigned char expected = static_cast<unsigned char>(expected_finalizer_index--);
        if (actual != expected)
            ++finalizer_order_errors;
    }
    return 0;
}

int main(int argc, char** argv)
{
    const int userdata_count = argc > 1 ? std::atoi(argv[1]) : 1000000;
    const bool keep_alive = argc <= 2 || std::atoi(argv[2]) != 0;
    const bool add_finalizer = argc > 3 && std::atoi(argv[3]) != 0;
    const bool verify_finalizers = argc > 4 && std::atoi(argv[4]) != 0;
    const bool finalizer_required = argc <= 5 || std::atoi(argv[5]) != 0;
    if (userdata_count <= 0)
        return 2;

    lua_State* state = luaL_newstate();
    if (!state)
        return 3;

    lua_gc(state, LUA_GCSTOP, 0);
    if (add_finalizer)
    {
        luaL_newmetatable(state, "gc_userdata_page_smoke");
        lua_pushcfunction(state, noop_finalizer);
        lua_setfield(state, -2, "__gc");
        lua_pop(state, 1);
    }
    if (keep_alive)
        lua_createtable(state, userdata_count, 0);
    for (int index = 1; index <= userdata_count; ++index)
    {
        *static_cast<unsigned char*>(lua_newuserdata(state, 1)) = static_cast<unsigned char>(index);
        if (add_finalizer)
        {
            luaL_getmetatable(state, "gc_userdata_page_smoke");
            lua_setmetatable(state, -2);
        }
        if (!finalizer_required)
            luaJIT_setudatafinalizer(state, -1, 0);
        if (keep_alive)
            lua_rawseti(state, -2, index);
        else
            lua_pop(state, 1);
    }
    if (keep_alive)
        lua_setglobal(state, "live_userdata");

    luaJIT_setgchostmanaged(state, 1);
    lua_gc(state, LUA_GCRESTART, 0);

    luaJIT_GCStateInfo gc_state = {};
    int mark_steps = 0;
    for (;;)
    {
        luaJIT_getgcstate(state, &gc_state);
        if (gc_state.state == 2)
            break;
        luaJIT_gcstep(state, 1024u * 1024u, 8u * 1024u * 1024u);
        if (++mark_steps > 1000000)
        {
            std::fprintf(stderr, "collector did not reach atomic\n");
            lua_close(state);
            return 4;
        }
    }

    int preatomic_steps = 0;
    while (!luaJIT_gcpreatomic(state, 64u * 1024u))
    {
        if (++preatomic_steps > 1000000)
        {
            std::fprintf(stderr, "pre-atomic marking did not converge\n");
            lua_close(state);
            return 5;
        }
    }

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER begin = {};
    LARGE_INTEGER end = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    const int result = luaJIT_gcstep(state, 64u * 1024u, 8u * 1024u * 1024u);
    QueryPerformanceCounter(&end);

    luaJIT_GCAtomicInfoV2 atomic_info = {};
    luaJIT_getgcatomicinfo_v2(state, &atomic_info);
    luaJIT_getgcstate(state, &gc_state);
    std::printf("userdata=%d live=%d finalizer=%d required=%d atomic_ms=%.3f visited=%zu finalizable=%zu pages=%zu state=%d result=%d mark_steps=%d preatomic_steps=%d\n",
        userdata_count, keep_alive ? 1 : 0, add_finalizer ? 1 : 0, finalizer_required ? 1 : 0,
        elapsed_ms(begin, end, frequency), atomic_info.udata_visited, atomic_info.udata_finalizable,
        atomic_info.udata_pages, gc_state.state, result, mark_steps, preatomic_steps);

    if (verify_finalizers && add_finalizer && finalizer_required && !keep_alive)
    {
        expected_finalizer_index = userdata_count;
        int finalize_steps = 0;
        do
        {
            luaJIT_gcstep(state, 1024u * 1024u, 8u * 1024u * 1024u);
            luaJIT_getgcstate(state, &gc_state);
            if (++finalize_steps > userdata_count * 2 + 1000000)
            {
                std::fprintf(stderr, "collector did not finish finalizers\n");
                lua_close(state);
                return 6;
            }
        } while (gc_state.state != 0);
        std::printf("finalizer_calls=%d order_errors=%d remaining_expected=%d finalize_steps=%d\n",
            finalizer_calls, finalizer_order_errors, expected_finalizer_index, finalize_steps);
        if (finalizer_calls != userdata_count || finalizer_order_errors != 0 || expected_finalizer_index != 0)
        {
            lua_close(state);
            return 7;
        }
    }

    lua_close(state);
    return 0;
}
