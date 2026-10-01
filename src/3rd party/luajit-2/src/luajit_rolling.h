/*
** LuaJIT -- a Just-In-Time Compiler for Lua. https://luajit.org/
**
** Copyright (C) 2005-2026 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: https://www.opensource.org/licenses/mit-license.php ]
*/

#ifndef _LUAJIT_H
#define _LUAJIT_H

#include "lua.h"

#define LUAJIT_VERSION		"LuaJIT 2.0.ROLLING"
#define LUAJIT_VERSION_NUM	20099  /* Deprecated. */
#define LUAJIT_VERSION_SYM	luaJIT_version_2_0_ROLLING
#define LUAJIT_COPYRIGHT	"Copyright (C) 2005-2026 Mike Pall"
#define LUAJIT_URL		"https://luajit.org/"

/* Modes for luaJIT_setmode. */
#define LUAJIT_MODE_MASK	0x00ff

enum {
  LUAJIT_MODE_ENGINE,		/* Set mode for whole JIT engine. */
  LUAJIT_MODE_DEBUG,		/* Set debug mode (idx = level). */

  LUAJIT_MODE_FUNC,		/* Change mode for a function. */
  LUAJIT_MODE_ALLFUNC,		/* Recurse into subroutine protos. */
  LUAJIT_MODE_ALLSUBFUNC,	/* Change only the subroutines. */

  LUAJIT_MODE_TRACE,		/* Flush a compiled trace. */

  LUAJIT_MODE_WRAPCFUNC = 0x10,	/* Set wrapper mode for C function calls. */

  LUAJIT_MODE_MAX
};

/* Flags or'ed in to the mode. */
#define LUAJIT_MODE_OFF		0x0000	/* Turn feature off. */
#define LUAJIT_MODE_ON		0x0100	/* Turn feature on. */
#define LUAJIT_MODE_FLUSH	0x0200	/* Flush JIT-compiled code. */

/* LuaJIT public C API. */

/* Engine-specific GC telemetry API. */
typedef struct luaJIT_GCStateInfo {
  size_t total;
  size_t threshold;
  size_t debt;
  int state;
  int active;
} luaJIT_GCStateInfo;

LUA_API void luaJIT_getgcstate(lua_State *L, luaJIT_GCStateInfo *info);

/* Separate extension structure: keep luaJIT_GCStateInfo ABI-stable for native
** plugins built against an earlier engine SDK. */
typedef struct luaJIT_GCAtomicInfo {
  unsigned long long total_cycles;
  unsigned long long mark_cycles;
  unsigned long long finalize_cycles;
  unsigned long long weak_cycles;
  size_t udata_visited;
  size_t weak_tables;
  size_t weak_slots;
} luaJIT_GCAtomicInfo;

LUA_API void luaJIT_getgcatomicinfo(lua_State *L, luaJIT_GCAtomicInfo *info);

/* Extended atomic telemetry. This is a separate ABI so native plugins built
** against luaJIT_GCAtomicInfo keep their original structure size. */
typedef struct luaJIT_GCAtomicInfoV2 {
  unsigned long long total_cycles;
  unsigned long long mark_cycles;
  unsigned long long finalize_cycles;
  unsigned long long weak_cycles;
  size_t udata_visited;
  size_t udata_finalizable;
  size_t udata_pages;
  size_t weak_tables;
  size_t weak_slots;
} luaJIT_GCAtomicInfoV2;

LUA_API void luaJIT_getgcatomicinfo_v2(lua_State *L, luaJIT_GCAtomicInfoV2 *info);

/* Monotonic allocation telemetry used by the embedding host to start a GC
** cycle before a very large userdata-finalizer population can accumulate.
** This is diagnostic/scheduling state only and does not alter GC semantics. */
typedef struct luaJIT_GCPressureInfo {
  unsigned long long udata_alloc_serial;
} luaJIT_GCPressureInfo;

LUA_API void luaJIT_getgcpressureinfo(lua_State *L, luaJIT_GCPressureInfo *info);

/* When enabled, allocation-driven steps stop at atomic and finalizer boundaries
** so the embedding host can resume them at a serialized point. Explicit
** lua_gc() and collectgarbage() calls keep their Lua 5.1 behaviour. */
LUA_API void luaJIT_setgchostmanaged(lua_State *L, int enabled);

/* Mark a full userdata as requiring (enabled != 0) or not requiring its
** metatable __gc callback. The opt-out is for embedding-owned borrowed
** wrappers whose destructor is proven to be empty; ordinary Lua userdata is
** conservative by default. */
LUA_API void luaJIT_setudatafinalizer(lua_State *L, int idx, int enabled);

/* Advance one bounded incremental GC quantum and reserve allocation headroom
** before LuaJIT may trigger another automatic quantum. Returns 2 at the atomic
** boundary and 3 at the finalizer boundary or after at most one finalizer, so
** the host may check its time budget. lua_gc() keeps its Lua 5.1 behaviour. */
LUA_API int luaJIT_gcstep(lua_State *L, size_t work_bytes,
			  size_t allocation_headroom);

/* Perform extra marking work while stopped at GCSatomic without entering the
** indivisible atomic commit. Returns non-zero when the pre-pass is quiescent. */
LUA_API int luaJIT_gcpreatomic(lua_State *L, size_t work_bytes);

/* Engine-specific JIT cache/trace telemetry. Counters are cumulative for the
** current VM and can be reset without flushing compiled code. */
typedef struct luaJIT_JITStats {
  size_t mcode_reserved_bytes;
  size_t trace_mcode_bytes;
  size_t mcode_limit_bytes;
  unsigned int active_traces;
  unsigned int max_traces;
  unsigned int mcode_area_kb;
  unsigned long long trace_starts;
  unsigned long long root_trace_starts;
  unsigned long long side_trace_starts;
  unsigned long long trace_compiled;
  unsigned long long trace_aborts;
  unsigned long long root_trace_aborts;
  unsigned long long side_trace_aborts;
  unsigned long long mcode_retries;
  unsigned long long trace_flushes;
  unsigned long long maxtrace_flushes;
  unsigned long long mcode_limit_hits;
  unsigned long long mcode_alloc_failures;
  unsigned long long abort_mcode;
  unsigned long long abort_nyi;
  unsigned long long abort_blacklist;
  unsigned long long blacklisted_sites;
  unsigned long long side_nyi_disabled;
  unsigned long long root_nyi_backoffs;
  unsigned long long root_nyi_suppressed;
} luaJIT_JITStats;

#define LUAJIT_JIT_ABORT_SOURCE_LEN 96
typedef struct luaJIT_JITAbortSite {
  unsigned long long count;
  unsigned int line;
  unsigned int reason;
  unsigned int side_trace;
  char source[LUAJIT_JIT_ABORT_SOURCE_LEN];
} luaJIT_JITAbortSite;

LUA_API void luaJIT_getjitstats(lua_State *L, luaJIT_JITStats *stats);
LUA_API unsigned int luaJIT_getjitabortsites(lua_State *L, luaJIT_JITAbortSite *sites,
                                              unsigned int capacity);
LUA_API void luaJIT_resetjitstats(lua_State *L);
/* Set cache limits without loading jit.opt. Intended for VM startup before
** gameplay scripts have had a chance to compile traces. */
LUA_API void luaJIT_setjitcache(lua_State *L, unsigned int max_traces,
                                unsigned int mcode_area_kb,
                                unsigned int max_mcode_kb);

/* Control the JIT engine. */
LUA_API int luaJIT_setmode(lua_State *L, int idx, int mode);

/* Enforce (dynamic) linker error for version mismatches. Call from main. */
LUA_API void LUAJIT_VERSION_SYM(void);

#error "DO NOT USE luajit_rolling.h -- only include build-generated luajit.h"
#endif
