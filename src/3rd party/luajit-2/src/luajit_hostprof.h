/*
** QUARK: the engine's script sampler (MODS-INFO of QUARK-PANEL), on the profile timer of lj_profile.c.
**
** Time samples come from the timer thread about every interval_ms, allocation samples every ~alloc_mean bytes the VM
** allocates (random distances, so periodic work cannot alias them). Both are taken only while *gate is nonzero: the
** host keeps it nonzero, a new value for every call, while Lua runs a call of the host, so nothing is sampled between
** calls and a sample whose call ended before the VM reached the hook is dropped. The callback runs on the Lua thread
** in the hook, with other hooks off: it may walk the stack (lua_getstack, lua_getinfo) but must not call Lua.
** time_samples: timer ticks since the last callback; alloc_bytes: bytes the allocation samples stand for; vmstate as
** in jit.profile ('N' compiled, 'I' interpreted, 'C' a C function, 'G' garbage collector, 'J' JIT compiler).
*/

#ifndef _LUAJIT_HOSTPROF_H
#define _LUAJIT_HOSTPROF_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*luaJIT_hostprof_callback)(void *data, lua_State *L, int time_samples, unsigned long long alloc_bytes,
					 int vmstate);

/* 0 when jit.profile or jit.allocprof of a script holds the timer. jit.profile.start stops the host's sampler. */
LUA_API int luaJIT_hostprof_start(lua_State *L, int interval_ms, unsigned int alloc_mean, volatile long *gate,
				  luaJIT_hostprof_callback cb, void *data);
LUA_API void luaJIT_hostprof_stop(lua_State *L);
LUA_API int luaJIT_hostprof_active(void);

#ifdef __cplusplus
}
#endif

#endif
