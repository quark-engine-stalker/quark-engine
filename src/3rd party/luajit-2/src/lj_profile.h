/*
** Low-overhead profiling.
** Copyright (C) 2005-2016 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_PROFILE_H
#define _LJ_PROFILE_H

#include "lj_obj.h"

#if LJ_HASPROFILE

LJ_FUNC void LJ_FASTCALL lj_profile_interpreter(lua_State *L);
/* QUARK host sampler (luajit_hostprof.h): allocation samples. */
extern int lj_hostprof_alloc_on;
LJ_FUNC void lj_hostprof_alloc(global_State *g, uint64_t size);
LJ_FUNC int lj_profile_active(void);
#if !LJ_PROFILE_SIGPROF
LJ_FUNC void LJ_FASTCALL lj_profile_hook_enter(global_State *g);
LJ_FUNC void LJ_FASTCALL lj_profile_hook_leave(global_State *g);
LJ_FUNC int lj_profile_lock(void);
LJ_FUNC void lj_profile_unlock(void);
#endif

#endif

#endif
