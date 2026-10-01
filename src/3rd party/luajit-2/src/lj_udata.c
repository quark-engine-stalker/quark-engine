/*
** Userdata handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include <string.h>

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_udata.h"

#define UDATA_BITMAP_WORD(slot) ((MSize)(slot) >> 6)
#define UDATA_BITMAP_MASK(slot) ((uint64_t)1u << ((MSize)(slot) & 63u))
#define UDATA_BITMAP_WORDS (LJ_UDATA_REGISTRY_PAGE_SIZE / 64)

static LJ_AINLINE void udata_registry_clear_mt_group(UDataRegistryPage *page,
                                                     MSize slot)
{
  uint8_t group = page->mt_group_index[slot];
  if (group < LJ_UDATA_METATABLE_GROUPS)
    page->mt_group_bits[group][UDATA_BITMAP_WORD(slot)] &=
      ~UDATA_BITMAP_MASK(slot);
  page->mt_group_index[slot] = LJ_UDATA_MTGROUP_NONE;
}

static LJ_AINLINE int udata_registry_group_empty(const UDataRegistryPage *page,
                                                 MSize group)
{
  MSize word;
  for (word = 0; word < UDATA_BITMAP_WORDS; word++)
    if (page->mt_group_bits[group][word] != 0)
      return 0;
  return 1;
}

static uint8_t udata_registry_mt_group(UDataRegistryPage *page, GCtab *mt)
{
  MSize group;
  MSize reusable = LJ_UDATA_METATABLE_GROUPS;

  for (group = 0; group < page->mt_group_count; group++) {
    if (!udata_registry_group_empty(page, group)) {
      if (gcref(page->mt_group_refs[group]) == obj2gco(mt))
        return (uint8_t)group;
    } else if (reusable == LJ_UDATA_METATABLE_GROUPS) {
      reusable = group;
    }
  }

  if (reusable != LJ_UDATA_METATABLE_GROUPS) {
    setgcref(page->mt_group_refs[reusable], obj2gco(mt));
    return (uint8_t)reusable;
  }

  if (page->mt_group_count < LJ_UDATA_METATABLE_GROUPS) {
    group = page->mt_group_count++;
    setgcref(page->mt_group_refs[group], obj2gco(mt));
    return (uint8_t)group;
  }

  return LJ_UDATA_MTGROUP_NONE;
}

static LJ_AINLINE int udata_registry_slot_active(const UDataRegistryPage *page, MSize slot)
{
  return (page->occupied_bits[UDATA_BITMAP_WORD(slot)] &
          UDATA_BITMAP_MASK(slot)) != 0;
}

static LJ_AINLINE void udata_registry_clear_active(UDataRegistryPage *page, MSize slot)
{
  MSize word = UDATA_BITMAP_WORD(slot);
  uint64_t mask = UDATA_BITMAP_MASK(slot);
  udata_registry_clear_mt_group(page, slot);
  page->occupied_bits[word] &= ~mask;
  page->marked_bits[word] &= ~mask;
  page->metatable_bits[word] &= ~mask;
  page->finalizer_bits[word] &= ~mask;
}

static UDataRegistryPage *udata_registry_prepare(lua_State *L)
{
  global_State *g = G(L);
  UDataRegistryPage *page = mref(g->gc.udata_registry_tail,
					 UDataRegistryPage);
  if (page == NULL || page->used == LJ_UDATA_REGISTRY_PAGE_SIZE) {
    UDataRegistryPage *prev = page;
    page = lj_mem_newt(L, sizeof(UDataRegistryPage), UDataRegistryPage);
    setmref(page->prev, prev);
    setmref(page->next, NULL);
    page->used = 0;
    page->live = 0;
    page->pending_sweep = 0;
    memset(page->occupied_bits, 0, sizeof(page->occupied_bits));
    memset(page->marked_bits, 0, sizeof(page->marked_bits));
    memset(page->metatable_bits, 0, sizeof(page->metatable_bits));
    memset(page->finalizer_bits, 0, sizeof(page->finalizer_bits));
    memset(page->mt_group_bits, 0, sizeof(page->mt_group_bits));
    memset(page->mt_group_refs, 0, sizeof(page->mt_group_refs));
    memset(page->mt_group_index, LJ_UDATA_MTGROUP_NONE,
           sizeof(page->mt_group_index));
    page->mt_group_count = 0;
    memset(page->mt_group_pad, 0, sizeof(page->mt_group_pad));
    page->marked_bits_epoch = 0;
    if (prev != NULL)
      setmref(prev->next, page);
    else
      setmref(g->gc.udata_registry_head, page);
    setmref(g->gc.udata_registry_tail, page);
  }
  return page;
}

void lj_udata_link(global_State *g, GCudata *ud)
{
  GCobj *next = gcref(mainthread(g)->nextgc);
  setgcref(ud->rootprev, obj2gco(mainthread(g)));
  setgcrefr(ud->nextgc, mainthread(g)->nextgc);
  if (next != NULL) {
    lua_assert(next->gch.gct == ~LJ_TUDATA);
    setgcref(gco2ud(next)->rootprev, obj2gco(ud));
  }
  setgcref(mainthread(g)->nextgc, obj2gco(ud));
}

void lj_udata_unlink(global_State *g, GCudata *ud)
{
  GCobj *prev = gcref(ud->rootprev);
  GCobj *next = gcnext(obj2gco(ud));
  lua_assert(prev != NULL && gcnext(prev) == obj2gco(ud));
  setgcrefr(prev->gch.nextgc, ud->nextgc);
  if (next != NULL) {
    lua_assert(next->gch.gct == ~LJ_TUDATA);
    setgcrefr(gco2ud(next)->rootprev, ud->rootprev);
  }
  setgcrefnull(ud->rootprev);
}

void lj_udata_registry_remove(global_State *g, GCudata *ud)
{
  UDataRegistryPage *page = mref(ud->registry_page, UDataRegistryPage);
  MSize slot;
  UNUSED(g);
  if (page == NULL)
    return;
  slot = ud->registry_slot;
  lua_assert(slot < page->used);
  if (udata_registry_slot_active(page, slot)) {
    lua_assert(gcrefu(page->refs[slot]) != 0 &&
               gcref(page->refs[slot]) == obj2gco(ud) && page->live > 0);
    udata_registry_clear_active(page, slot);
    setgcrefnull(page->refs[slot]);
    setgcrefnull(page->metatables[slot]);
    page->live--;
  } else {
    /* A dead registry slot may be retired before sweep. Keep the page pinned
    ** until sweep reaches the object and drops this pending-sweep pin. */
    lua_assert(page->pending_sweep > 0);
    setgcrefnull(page->refs[slot]);
    setgcrefnull(page->metatables[slot]);
    page->pending_sweep--;
  }
  setmref(ud->registry_page, NULL);
}

void lj_udata_registry_retire(UDataRegistryPage *page, MSize slot)
{
  lua_assert(slot < page->used && udata_registry_slot_active(page, slot) &&
             gcrefu(page->refs[slot]) != 0 && page->live > 0);
  udata_registry_clear_active(page, slot);
  setgcrefnull(page->refs[slot]);
  setgcrefnull(page->metatables[slot]);
  page->live--;
  page->pending_sweep++;
}

void lj_udata_registry_release(global_State *g, UDataRegistryPage *page)
{
  UDataRegistryPage *prev;
  UDataRegistryPage *next;
  if (page == NULL || page->live != 0 || page->pending_sweep != 0)
    return;
  prev = mref(page->prev, UDataRegistryPage);
  next = mref(page->next, UDataRegistryPage);
  if (prev != NULL)
    setmref(prev->next, next);
  else
    setmref(g->gc.udata_registry_head, next);
  if (next != NULL)
    setmref(next->prev, prev);
  else
    setmref(g->gc.udata_registry_tail, prev);
  lj_mem_freet(g, page);
}

void lj_udata_registry_freeall(global_State *g)
{
  UDataRegistryPage *page = mref(g->gc.udata_registry_head,
					 UDataRegistryPage);
  while (page != NULL) {
    UDataRegistryPage *next = mref(page->next, UDataRegistryPage);
    lua_assert(page->live == 0 && page->pending_sweep == 0);
    lj_mem_freet(g, page);
    page = next;
  }
  setmref(g->gc.udata_registry_head, NULL);
  setmref(g->gc.udata_registry_tail, NULL);
}

void lj_udata_registry_newcycle(global_State *g)
{
  if (++g->gc.udata_mark_epoch == 0) {
    UDataRegistryPage *page = mref(g->gc.udata_registry_head,
                                           UDataRegistryPage);
    /* Generation zero is reserved for pages with no valid mark bitmap. A 32-bit
    ** epoch makes this reset effectively unreachable in gameplay, while keeping
    ** exact correctness if a VM ever survives more than four billion cycles. */
    while (page != NULL) {
      memset(page->marked_bits, 0, sizeof(page->marked_bits));
      page->marked_bits_epoch = 0;
      page = mref(page->next, UDataRegistryPage);
    }
    g->gc.udata_mark_epoch = 1;
  }
}

void lj_udata_registry_mark(global_State *g, GCudata *ud)
{
  UDataRegistryPage *page = mref(ud->registry_page, UDataRegistryPage);
  if (page != NULL) {
    MSize slot = ud->registry_slot;
    MSize word = UDATA_BITMAP_WORD(slot);
    uint64_t mask = UDATA_BITMAP_MASK(slot);
    lua_assert(slot < page->used && udata_registry_slot_active(page, slot) &&
               gcref(page->refs[slot]) == obj2gco(ud));
    if (page->marked_bits_epoch != g->gc.udata_mark_epoch) {
      memset(page->marked_bits, 0, sizeof(page->marked_bits));
      page->marked_bits_epoch = g->gc.udata_mark_epoch;
    }
    page->marked_bits[word] |= mask;
  }
}

void lj_udata_setmetatable(GCudata *ud, GCtab *mt)
{
  UDataRegistryPage *page = mref(ud->registry_page, UDataRegistryPage);
  setgcref(ud->metatable, obj2gco(mt));
  if (page != NULL) {
    MSize slot = ud->registry_slot;
    MSize word = UDATA_BITMAP_WORD(slot);
    uint64_t mask = UDATA_BITMAP_MASK(slot);
    lua_assert(slot < page->used &&
               gcref(page->refs[slot]) == obj2gco(ud));

    /* A userdata may change metatables at runtime. Keep grouping exact rather
    ** than treating a creation-time metatable as immutable. */
    udata_registry_clear_mt_group(page, slot);
    setgcref(page->metatables[slot], obj2gco(mt));
    if (mt) {
      uint8_t group = udata_registry_mt_group(page, mt);
      page->metatable_bits[word] |= mask;
      page->mt_group_index[slot] = group;
      if (group < LJ_UDATA_METATABLE_GROUPS)
        page->mt_group_bits[group][word] |= mask;
    } else {
      page->metatable_bits[word] &= ~mask;
    }
  }
}

void lj_udata_setfinalizerrequired(GCudata *ud, int required)
{
  UDataRegistryPage *page = mref(ud->registry_page, UDataRegistryPage);
  if (page != NULL) {
    MSize slot = ud->registry_slot;
    MSize word = UDATA_BITMAP_WORD(slot);
    uint64_t mask = UDATA_BITMAP_MASK(slot);
    lua_assert(slot < page->used &&
	       gcref(page->refs[slot]) == obj2gco(ud));
    if (required)
      page->finalizer_bits[word] |= mask;
    else
      page->finalizer_bits[word] &= ~mask;
  }
}

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env)
{
  UDataRegistryPage *page = udata_registry_prepare(L);
  GCudata *ud = lj_mem_newt(L, sizeof(GCudata) + sz, GCudata);
  global_State *g = G(L);
  MSize slot = page->used++;
  g->gc.udata_alloc_serial++;
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefnull(ud->metatable);
  setgcref(ud->env, obj2gco(env));
  setmref(ud->registry_page, page);
  ud->registry_slot = (uint16_t)slot;
  setgcref(page->refs[slot], obj2gco(ud));
  setgcrefnull(page->metatables[slot]);
  page->mt_group_index[slot] = LJ_UDATA_MTGROUP_NONE;
  /* New userdata is conservatively finalizable until the embedding layer
  ** explicitly certifies a borrowed/no-op wrapper. */
  page->occupied_bits[UDATA_BITMAP_WORD(slot)] |= UDATA_BITMAP_MASK(slot);
  page->finalizer_bits[UDATA_BITMAP_WORD(slot)] |= UDATA_BITMAP_MASK(slot);
  page->live++;
  /* Chain to userdata list (after main thread). */
  lj_udata_link(g, ud);
  return ud;
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
  UDataRegistryPage *page = mref(ud->registry_page, UDataRegistryPage);
  GCobj *next = gcnext(obj2gco(ud));
  /* gc_sweep has already removed ud from the root list. Repair the reverse
  ** link of the next userdata before releasing the object. */
  if (next != NULL) {
    lua_assert(next->gch.gct == ~LJ_TUDATA);
    setgcrefr(gco2ud(next)->rootprev, ud->rootprev);
  }
  lj_udata_registry_remove(g, ud);
  lj_udata_registry_release(g, page);
  lj_mem_free(g, ud, sizeudata(ud));
}

