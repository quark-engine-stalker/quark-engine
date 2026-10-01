/*
** Userdata handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_UDATA_H
#define _LJ_UDATA_H

#include "lj_obj.h"

LJ_FUNC GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env);
LJ_FUNC void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud);
LJ_FUNC void lj_udata_unlink(global_State *g, GCudata *ud);
LJ_FUNC void lj_udata_link(global_State *g, GCudata *ud);
LJ_FUNC void lj_udata_registry_remove(global_State *g, GCudata *ud);
LJ_FUNC void lj_udata_registry_release(global_State *g,
					UDataRegistryPage *page);
LJ_FUNC void lj_udata_registry_freeall(global_State *g);
LJ_FUNC void lj_udata_registry_newcycle(global_State *g);
LJ_FUNC void lj_udata_registry_mark(global_State *g, GCudata *ud);
LJ_FUNC void lj_udata_registry_retire(UDataRegistryPage *page, MSize slot);
LJ_FUNC void lj_udata_setmetatable(GCudata *ud, GCtab *mt);
LJ_FUNC void lj_udata_setfinalizerrequired(GCudata *ud, int required);

#endif
