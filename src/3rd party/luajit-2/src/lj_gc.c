/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"

#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_vm.h"

#if LJ_TARGET_X86ORX64 && defined(_MSC_VER)
#include <intrin.h>
#define gc_prefetch(p) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
#define gc_prefetch(p) __builtin_prefetch((p), 0, 3)
#else
#define gc_prefetch(p) UNUSED(p)
#endif

#if LJ_TARGET_X86ORX64 && defined(_MSC_VER)
static LJ_AINLINE uint64_t gc_cycle_counter(void)
{
  _mm_lfence();
  return (uint64_t)__rdtsc();
}
#elif LJ_TARGET_X86ORX64 && (defined(__GNUC__) || defined(__clang__))
static LJ_AINLINE uint64_t gc_cycle_counter(void)
{
  __asm__ __volatile__("lfence" ::: "memory");
  return (uint64_t)__builtin_ia32_rdtsc();
}
#else
static LJ_AINLINE uint64_t gc_cycle_counter(void)
{
  return 0;
}
#endif

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPSTRMAX	32
#define GCSWEEPSTREMPTYMAX	256
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

#define UDATA_BITMAP_WORDS (LJ_UDATA_REGISTRY_PAGE_SIZE / 64)

static LJ_AINLINE MSize gc_highbit64(uint64_t value)
{
#if defined(_MSC_VER) && defined(_M_X64)
  unsigned long index;
  _BitScanReverse64(&index, value);
  return (MSize)index;
#elif defined(__GNUC__) || defined(__clang__)
  return (MSize)(63u - (MSize)__builtin_clzll((unsigned long long)value));
#else
  MSize index = 0;
  while (value >>= 1) index++;
  return index;
#endif
}

static LJ_AINLINE MSize gc_popcount64(uint64_t value)
{
  /* Branch-free software popcount keeps telemetry cheap on all supported
  ** targets and avoids introducing a new CPU feature dependency. */
  value = value - ((value >> 1) & UINT64_C(0x5555555555555555));
  value = (value & UINT64_C(0x3333333333333333)) +
          ((value >> 2) & UINT64_C(0x3333333333333333));
  value = (value + (value >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
  return (MSize)((value * UINT64_C(0x0101010101010101)) >> 56);
}

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lua_assert(iswhite(o) && !isdead(g, o));
  white2gray(o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCudata *ud = gco2ud(o);
    GCtab *mt = tabref(ud->metatable);
    lj_udata_registry_mark(g, ud);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    gc_markobj(g, tabref(ud->env));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lua_assert(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  g->gc.preatomic_stage = 0;
  lj_udata_registry_newcycle(g);
  setmref(g->gc.sweepstrptr, NULL);
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
#if LJ_HASFFI
  if (ctype_ctsG(g)) gc_markobj(g, ctype_ctsG(g)->finalizer);
#endif
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

/* Retire the pointer-bearing part of one userdata registry slot. The bitmaps
** are cleared in bulk after a word has been processed, which removes several
** read/modify/write operations from the dense finalizer path. */
static LJ_AINLINE void gc_udata_registry_detach_ptrs(UDataRegistryPage *page,
                                                     MSize slot, GCudata *ud)
{
  MSize word = slot >> 6;
  uint64_t mask = (uint64_t)1u << (slot & 63u);

  lua_assert(slot < page->used && page->live > 0 &&
             (page->occupied_bits[word] & mask) != 0 &&
             gcref(page->refs[slot]) == obj2gco(ud));

  setgcrefnull(page->refs[slot]);
  setgcrefnull(page->metatables[slot]);
  setmref(ud->registry_page, NULL);
}

/* Clear all registry bookkeeping for the slots detached from one 64-bit word.
** Registry pages are append-only, so mt_group_index for retired slots is never
** consulted again. The authoritative occupancy/group bitmaps are updated here. */
static LJ_AINLINE void gc_udata_registry_commit_word(UDataRegistryPage *page,
                                                     MSize word,
                                                     uint64_t detached)
{
  MSize group;
  MSize count;
  if (!detached)
    return;

  for (group = 0; group < page->mt_group_count; group++)
    page->mt_group_bits[group][word] &= ~detached;
  page->occupied_bits[word] &= ~detached;
  page->marked_bits[word] &= ~detached;
  page->metatable_bits[word] &= ~detached;
  page->finalizer_bits[word] &= ~detached;
  count = gc_popcount64(detached);
  lua_assert(page->live >= count);
  page->live -= count;
}

/* Unlink a contiguous run from the creation-ordered userdata root list with two
** pointer updates instead of two updates per object. Candidates are visited in
** exact root-list order (newest to oldest), so dense GAMMA finalizer bursts tend
** to collapse into long runs. */
static LJ_AINLINE void gc_udata_root_unlink_run(GCobj *run_prev,
                                                GCobj *run_tail)
{
  GCobj *next = gcnext(run_tail);
  lua_assert(run_prev != NULL);
  setgcrefr(run_prev->gch.nextgc, run_tail->gch.nextgc);
  if (next != NULL) {
    lua_assert(next->gch.gct == ~LJ_TUDATA);
    setgcref(gco2ud(next)->rootprev, run_prev);
  }
}

/* A just-separated userdata must keep its metatable/environment alive until
** __gc runs. Specialize the stock gc_mark() userdata branch: the registry slot
** has already been detached, so there is no reason to revisit registry state. */
static LJ_AINLINE void gc_remark_finalized_udata(global_State *g, GCudata *ud,
                                                  GCtab **last_env)
{
  GCobj *o = obj2gco(ud);
  GCtab *env = tabref(ud->env);

  makewhite(g, o);  /* It may carry the previous cycle's white bits. */
  white2gray(o);
  gray2black(o);  /* Userdata are never placed on the gray list. */

  /* Metatables with a live __gc are marked once per page/group while candidate
  ** masks are built. Environments are arbitrary per userdata, but luabind
  ** workloads overwhelmingly reuse the same environment; suppress consecutive
  ** duplicate mark checks without assuming that they are globally identical. */
  if (env != *last_env) {
    gc_markobj(g, env);
    *last_env = env;
  }
}

/* Separate userdata objects to be finalized to mmudata list. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  UDataRegistryPage *page = mref(g->gc.udata_registry_tail,
                                 UDataRegistryPage);
  GCobj *batch_head = NULL;
  GCobj *batch_tail = NULL;
  GCobj *unlink_run_prev = NULL;
  GCobj *unlink_run_tail = NULL;
  GCtab *last_finalizer_env = NULL;
  MSize visited = 0;
  MSize finalizable = 0;
  MSize pages = 0;

  /* LuaJIT 2.1 performs this selection as one stop-the-world walk. GAMMA's
  ** luabind-heavy heap makes the per-object constant cost dominate, so do as
  ** much work as possible with four 64-bit words per registry page:
  **
  **  - compute dead/finalizer-eligible slots from liveness bitmaps;
  **  - resolve __gc once per page-local metatable group (not once per word);
  **  - touch GCudata headers only for slots with a current __gc;
  **  - build one linear finalizer batch and splice it into mmudata once.
  **
  ** No candidate is moved before atomic, and __gc is still resolved from the
  ** current metatable at this atomic commit. This preserves LuaJIT/Lua 5.1
  ** resurrection, weak-table and runtime-metatable semantics. */
  while (page != NULL) {
    UDataRegistryPage *prev = mref(page->prev, UDataRegistryPage);
    uint64_t eligible_words[UDATA_BITMAP_WORDS];
    uint64_t grouped_words[UDATA_BITMAP_WORDS] = { 0, 0, 0, 0 };
    uint64_t candidate_words[UDATA_BITMAP_WORDS] = { 0, 0, 0, 0 };
    MSize word;
    MSize group;
    int has_eligible = 0;

    pages++;

    /* Build all four eligibility words first. Besides avoiding repeated state
    ** tests, this lets the metatable pass below execute exactly once per group
    ** for the whole 256-slot page. */
    for (word = 0; word < UDATA_BITMAP_WORDS; word++) {
      uint64_t occupied = page->occupied_bits[word];
      uint64_t eligible = 0;

      if (occupied) {
        if (all) {
          eligible = occupied & page->metatable_bits[word];
        } else {
          uint64_t marked = page->marked_bits_epoch == g->gc.udata_mark_epoch ?
            page->marked_bits[word] : 0;
          eligible = occupied & ~marked & page->metatable_bits[word] &
            page->finalizer_bits[word];
        }
      }

      eligible_words[word] = eligible;
      if (eligible) {
        has_eligible = 1;
        visited += gc_popcount64(eligible);
      }
    }

    if (has_eligible) {
      /* Resolve each grouped metatable once for the entire page. The mutator is
      ** stopped throughout atomic(), so one result is valid for all four words. */
      for (group = 0; group < page->mt_group_count; group++) {
        int intersects = 0;
        int has_finalizer = 0;

        for (word = 0; word < UDATA_BITMAP_WORDS; word++) {
          uint64_t bits = eligible_words[word] & page->mt_group_bits[group][word];
          grouped_words[word] |= bits;
          if (bits)
            intersects = 1;
        }

        if (!intersects || gcrefu(page->mt_group_refs[group]) == 0)
          continue;

        {
          GCtab *mt = tabref(page->mt_group_refs[group]);
          has_finalizer = lj_meta_fastg(g, mt, MM_gc) != NULL;
          if (has_finalizer) {
            if (!all && g->gc.state == GCSatomic)
              gc_markobj(g, mt);
            for (word = 0; word < UDATA_BITMAP_WORDS; word++)
              candidate_words[word] |= eligible_words[word] & page->mt_group_bits[group][word];
          }
        }
      }

      /* Pages with more than 32 distinct metatables use the exact old per-slot
      ** lookup for overflow slots. This path is intentionally rare and keeps
      ** behavior exact instead of imposing a larger hot-page structure. */
      for (word = 0; word < UDATA_BITMAP_WORDS; word++) {
        uint64_t overflow = eligible_words[word] & ~grouped_words[word];
        while (overflow) {
          MSize bit = gc_highbit64(overflow);
          MSize i = (word << 6) + bit;
          uint64_t mask = (uint64_t)1u << bit;

          overflow &= ~mask;
          if (i < page->used && (page->occupied_bits[word] & mask) &&
              gcrefu(page->metatables[i]) != 0) {
            GCtab *mt = tabref(page->metatables[i]);
            if (lj_meta_fastg(g, mt, MM_gc) != NULL) {
              if (!all && g->gc.state == GCSatomic)
                gc_markobj(g, mt);
              candidate_words[word] |= mask;
            }
          }
        }
      }

      /* Process newest slots first. Registry pointers are retired per object,
      ** while page bitmaps and root-list links are committed in batches. */
      for (word = UDATA_BITMAP_WORDS; word-- > 0; ) {
        uint64_t candidates = candidate_words[word];
        uint64_t detached = 0;
        while (candidates) {
          MSize bit = gc_highbit64(candidates);
          MSize i = (word << 6) + bit;
          uint64_t mask = (uint64_t)1u << bit;
          GCobj *o;
          GCudata *ud;

          candidates &= ~mask;
          if (i >= page->used || !(page->occupied_bits[word] & mask))
            continue;

          o = gcref(page->refs[i]);
          gc_prefetch(o);
          ud = gco2ud(o);
          if (!(iswhite(o) || all))
            continue;

          if (isfinalized(ud)) {
            gc_udata_registry_detach_ptrs(page, i, ud);
            detached |= mask;
            continue;
          }

          m += sizeudata(ud);
          finalizable++;
          markfinalized(o);
          gc_udata_registry_detach_ptrs(page, i, ud);
          detached |= mask;

          /* Coalesce adjacent finalizable userdata in the root list. A gap
          ** means a live/non-finalizable userdata must remain linked, so flush
          ** the previous run before starting the next one. */
          if (unlink_run_tail == NULL) {
            unlink_run_prev = gcref(ud->rootprev);
          } else if (gcnext(unlink_run_tail) != o) {
            gc_udata_root_unlink_run(unlink_run_prev, unlink_run_tail);
            unlink_run_prev = gcref(ud->rootprev);
          }
          unlink_run_tail = o;
          setgcrefnull(ud->rootprev);

          if (batch_tail != NULL)
            setgcref(batch_tail->gch.nextgc, o);
          else
            batch_head = o;
          batch_tail = o;

          if (!all && g->gc.state == GCSatomic)
            gc_remark_finalized_udata(g, ud, &last_finalizer_env);
        }
        gc_udata_registry_commit_word(page, word, detached);
      }
    }

    lj_udata_registry_release(g, page);
    page = prev;
  }

  if (unlink_run_tail != NULL)
    gc_udata_root_unlink_run(unlink_run_prev, unlink_run_tail);

  /* Append the whole userdata batch to the existing circular finalizer queue in
  ** one splice. The batch itself was built newest->oldest, matching stock
  ** LuaJIT finalizer order. */
  if (batch_tail != NULL) {
    GCobj *old_tail = gcref(g->gc.mmudata);
    if (old_tail != NULL) {
      GCobj *old_head = gcnext(old_tail);
      setgcref(batch_tail->gch.nextgc, old_head);
      setgcref(old_tail->gch.nextgc, batch_head);
    } else {
      setgcref(batch_tail->gch.nextgc, batch_head);
    }
    setgcref(g->gc.mmudata, batch_tail);
  }

  if (!all && g->gc.state == GCSatomic) {
    g->gc.atomic_udata_visited = visited;
    g->gc.atomic_udata_finalizable = finalizable;
    g->gc.atomic_udata_pages = pages;
  }
  return m;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_markobj(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      CTState *cts = ctype_ctsG(g);
      if (cts && cts->finalizer == t) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
  }
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lua_assert(!tvisnil(&n->key));
	if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n->key);
	if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lua_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lua_assert(traceno != G2J(g)->cur.traceno);
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1; o < top; o++)
    gc_marktv(g, o);
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  lua_assert(isgray(o));
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lua_assert(0);
    return 0;
#endif
  }
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

/* Host-only pre-pass for the otherwise indivisible atomic phase. This does
** not flip whites, separate userdata, clear weak tables or run finalizers.
** It only performs marking that stock atomic() would perform again anyway.
** Keeping GCSatomic active preserves write barriers while the mutator runs
** between host slices. A final stock atomic() remains the correctness commit. */
int LJ_FASTCALL lj_gc_preatomic(lua_State *L, MSize worklimit)
{
  global_State *g = G(L);
  MSize work = 0;
  int32_t ostate;

  if (g->gc.state != GCSatomic || gcref(g->jit_L))
    return 0;
  if (worklimit < GCSTEPSIZE)
    worklimit = GCSTEPSIZE;

  ostate = g->vmstate;
  setvmstate(g, GC);
  for (;;) {
    switch (g->gc.preatomic_stage) {
    case 0:
      /* Finish any propagation left at the transition, then perform the first
      ** stock-atomic root/weak-table remark without draining it all. */
      if (gcref(g->gc.gray) == NULL) {
        gc_mark_uv(g);
        setgcrefr(g->gc.gray, g->gc.weak);
        setgcrefnull(g->gc.weak);
        lua_assert(!iswhite(obj2gco(mainthread(g))));
        gc_markobj(g, L);
        gc_traverse_curtrace(g);
        gc_mark_gcroot(g);
        g->gc.preatomic_stage = 1;
      }
      break;
    case 1:
      /* Drain the roots/weak-table pass. Then consume the current second-chance
      ** list exactly once, mirroring stock atomic(). Threads deliberately put
      ** themselves back on grayagain when traversed, so waiting for grayagain to
      ** become empty here would never converge. */
      if (gcref(g->gc.gray) == NULL) {
        setgcrefr(g->gc.gray, g->gc.grayagain);
        setgcrefnull(g->gc.grayagain);
        g->gc.preatomic_stage = 2;
      }
      break;
    case 2:
      if (gcref(g->gc.gray) == NULL)
        g->gc.preatomic_stage = 3;
      break;
    case 3:
      /* The VM may have executed since the previous host slice. Refresh direct
      ** roots once more. Do not recursively chase grayagain here: thread
      ** traversal requeues threads by design and stock atomic only consumes the
      ** second-chance snapshot once. The final stock atomic commit below still
      ** performs its complete canonical remark before white-flip/weak clearing. */
      gc_mark_uv(g);
      gc_markobj(g, L);
      gc_traverse_curtrace(g);
      gc_mark_gcroot(g);
      g->gc.preatomic_stage = 4;
      if (gcref(g->gc.gray) == NULL) {
        g->gc.preatomic_stage = 3;
        g->vmstate = ostate;
        return 1;
      }
      break;
    default:  /* Stage 4: drain roots added by the refresh probe. */
      if (gcref(g->gc.gray) == NULL) {
        g->gc.preatomic_stage = 3;
        g->vmstate = ostate;
        return 1;
      }
      break;
    }

    if (gcref(g->gc.gray) != NULL) {
      work += (MSize)propagatemark(g);
      if (work >= worklimit) {
        g->vmstate = ostate;
        return 0;
      }
    }
  }
}

/* -- Sweep phase --------------------------------------------------------- */

/* Try to shrink some common data structures. */
static void gc_shrink(global_State *g, lua_State *L)
{
  if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
    lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
  if (g->tmpbuf.sz > LJ_MIN_SBUF*2)
    lj_str_resizebuf(L, &g->tmpbuf, g->tmpbuf.sz >> 1);  /* Shrink temp buf. */
}

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), LJ_MAX_MEM, NULL)

/* Partial sweep of a GC list. Optionally reports the number of visited
** objects. The counter is used by the host-bounded string-table sweeper,
** where a hash-chain length is not a useful upper bound on wall time. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim, uint32_t *visited)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (visited) (*visited)++;
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lua_assert(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED));
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lua_assert(isdead(g, o) || ow == LJ_GC_SFIXED);
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return p;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

static LJ_AINLINE void gc_prefetch_value(cTValue *o)
{
  if (tvisgcv(o))
    gc_prefetch(gcV(o));
}

/* Clear collected entries from weak tables. */
static void gc_clearweak(global_State *g, GCobj *o)
{
  MSize table_count = 0;
  MSize slot_count = 0;
  while (o) {
    GCtab *t = gco2tab(o);
    GCobj *nextweak = gcref(t->gclist);
    if (nextweak)
      gc_prefetch(nextweak);
    table_count++;
    lua_assert((t->marked & LJ_GC_WEAK));
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      slot_count += asize;
      for (i = 0; i < asize; i++) {
	if (i + 16 < asize)
	  gc_prefetch_value(arrayslot(t, i + 16));
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1))
	  setnilV(tv);
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      slot_count += hmask + 1;
      for (i = 0; i <= hmask; i++) {
	if (i + 8 <= hmask) {
	  gc_prefetch_value(&node[i + 8].key);
	  gc_prefetch_value(&node[i + 8].val);
	}
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1)))
	  setnilV(&n->val);
      }
    }
    o = nextweak;
  }
  g->gc.atomic_weak_tables = table_count;
  g->gc.atomic_weak_slots = slot_count;
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  MSize oldt = g->gc.threshold;
  int errcode;
  TValue *top;
  lj_trace_abort(g);
  top = L->top;
  L->top = top+2;
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  copyTV(L, top, mo);
  setgcV(L, top+1, o, ~o->gch.gct);
  errcode = lj_vm_pcall(L, top+1, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

/* Finalize one userdata or cdata object from the mmudata list. */
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo;
  lua_assert(gcref(g->jit_L) == NULL);  /* Must not be called on trace. */
  /* Unchain from list of userdata to be finalized. */
  if (o == gcref(g->gc.mmudata))
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(gcref(g->gc.mmudata)->gch.nextgc, o->gch.nextgc);
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    setgcrefr(o->gch.nextgc, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      g->gc.nocdatafin = 0;
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  /* Add userdata back to the main userdata list and make it white. */
  lj_udata_link(g, gco2ud(o));
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L);
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *node = noderef(t->node);
    ptrdiff_t i;
    setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
    for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
      if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
	GCobj *o = gcV(&node[i].key);
	TValue tmp;
	makewhite(g, o);
	o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
	copyTV(L, &tmp, &node[i].val);
	setnilV(&node[i].val);
	gc_call_finalizer(g, L, &tmp, o);
      }
  }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i;
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.root);
  for (i = g->strmask; i != ~(MSize)0; i--)  /* Free all string hash chains. */
    gc_fullsweep(g, &g->strhash[i]);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;
  uint64_t started = gc_cycle_counter();
  uint64_t phase_started = started;
  uint64_t phase_finished;

  g->gc.atomic_total_cycles = 0;
  g->gc.atomic_mark_cycles = 0;
  g->gc.atomic_finalize_cycles = 0;
  g->gc.atomic_weak_cycles = 0;
  g->gc.atomic_udata_visited = 0;
  g->gc.atomic_udata_finalizable = 0;
  g->gc.atomic_udata_pages = 0;
  g->gc.atomic_weak_tables = 0;
  g->gc.atomic_weak_slots = 0;

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lua_assert(!iswhite(obj2gco(mainthread(g))));
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  phase_finished = gc_cycle_counter();
  g->gc.atomic_mark_cycles = phase_finished - phase_started;
  phase_started = phase_finished;

  /* Separation marks every newly queued userdata immediately, while its cold
  ** header is already resident. This preserves the stock mark-before-sweep
  ** invariant without a second pointer-chasing pass over mmudata. */
  udsize = lj_gc_separateudata(g, 0);  /* Separate and mark finalizable udata. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  phase_finished = gc_cycle_counter();
  g->gc.atomic_finalize_cycles = phase_finished - phase_started;
  phase_started = phase_finished;

  /* All marking done, clear weak tables. */
  gc_clearweak(g, gcref(g->gc.weak));

  phase_finished = gc_cycle_counter();
  g->gc.atomic_weak_cycles = phase_finished - phase_started;

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (MSize)udsize;  /* Initial estimate. */
  g->gc.atomic_total_cycles = gc_cycle_counter() - started;
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L, uint32_t sweepstr_object_limit)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
    g->gc.state = GCSatomic;  /* End of mark phase. */
    g->gc.preatomic_stage = 0;
    return 0;
  case GCSatomic:
    if (gcref(g->jit_L))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.preatomic_stage = 0;
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    setmref(g->gc.sweepstrptr, &g->strhash[0]);
    return 0;
  case GCSsweepstring: {
    MSize old = g->gc.total;
    uint32_t visited = 0;
    uint32_t buckets = 0;
    uint32_t empty_hops = 0;
    uint32_t object_limit = sweepstr_object_limit;

    /* Stock LuaJIT accounts string sweeping per hash bucket. That makes one
    ** bucket an unbounded wall-time unit: a collision-heavy bucket may contain
    ** thousands of GCstr objects. The engine's exact-step mode passes a small
    ** object limit and resumes from sweepstrptr on the next host slice. String
    ** table resizing is already disabled while state == GCSsweepstring, so the
    ** cursor remains stable between mutator frames. */
    if (mref(g->gc.sweepstrptr, GCRef) == NULL && g->gc.sweepstr <= g->strmask)
      setmref(g->gc.sweepstrptr, &g->strhash[g->gc.sweepstr]);

    for (;;) {
      uint32_t batch_visited = 0;
      uint32_t remaining;
      GCRef *cursor;
      GCRef *next;

      if (g->gc.sweepstr > g->strmask) {
        g->gc.state = GCSsweep;
        setmref(g->gc.sweepstrptr, NULL);
        break;
      }
      if (object_limit && visited >= object_limit)
        break;

      cursor = mref(g->gc.sweepstrptr, GCRef);
      remaining = object_limit ? object_limit - visited : LJ_MAX_MEM;
      next = gc_sweep(g, cursor, remaining, &batch_visited);
      visited += batch_visited;
      setmref(g->gc.sweepstrptr, next);

      if (gcref(*next) != NULL)  /* Object budget exhausted inside this chain. */
        break;

      g->gc.sweepstr++;
      buckets++;
      if (g->gc.sweepstr > g->strmask) {
        g->gc.state = GCSsweep;
        setmref(g->gc.sweepstrptr, NULL);
        break;
      }
      setmref(g->gc.sweepstrptr, &g->strhash[g->gc.sweepstr]);

      /* Preserve stock behavior outside the engine exact-step API: finish one
      ** full hash chain per onestep. Exact mode may skip a bounded run of empty
      ** buckets so a sparse string table does not waste a host/QPC call. */
      if (!object_limit)
        break;
      if (batch_visited == 0 && ++empty_hops >= GCSWEEPSTREMPTYMAX)
        break;
    }

    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    return (buckets ? buckets : 1) * GCSWEEPCOST;
    }
  case GCSsweep: {
    MSize old = g->gc.total;
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX, NULL));
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      gc_shrink(g, L);
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
	g->gc.state = GCSfinalize;
#if LJ_HASFFI
	g->gc.nocdatafin = 1;
#endif
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	g->gc.debt = 0;
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      if (gcref(g->jit_L))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one userdata object. */
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
#if LJ_HASFFI
    if (!g->gc.nocdatafin) lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
#endif
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    return 0;
  default:
    lua_assert(0);
    return 0;
  }
}

/* Compute the next allocation threshold for one incremental quantum. A host
** that explicitly advances the collector may reserve extra headroom so a burst
** of small allocations cannot immediately chain many automatic GC calls. */
static MSize gc_step_threshold(global_State *g, MSize minimum,
			       MSize stepheadroom)
{
  MSize room = stepheadroom > minimum ? stepheadroom : minimum;
  return room < LJ_MAX_MEM - g->gc.total ? g->gc.total + room : LJ_MAX_MEM;
}

/* Perform a limited amount of incremental GC work. The stock collector uses a
** 1 KiB accounting quantum. The engine may request a larger quantum while it
** owns the Lua state, so its frame budget controls actual collector throughput
** instead of merely repeating tiny 1 KiB steps. */
static int LJ_FASTCALL gc_step(lua_State *L, MSize stepsize,
			       MSize stepheadroom, int atomicmode)
{
  global_State *g = G(L);
  MSize lim;
  MSize host_stepcost = 0;
  int32_t ostate = g->vmstate;
  if (stepsize < GCSTEPSIZE)
    stepsize = GCSTEPSIZE;
  /* A host-managed automatic step may be entered after another thread already
  ** reached an unbounded phase. Keep atomic and arbitrary __gc callbacks at the
  ** host's serialized post-submit point. Engine-sized steps use mode 1: they
  ** yield on transitions, but may explicitly resume either phase. */
  if (atomicmode > 1 &&
      (g->gc.state == GCSatomic || g->gc.state == GCSfinalize)) {
    if (g->gc.total > g->gc.threshold)
      g->gc.debt += g->gc.total - g->gc.threshold;
    g->gc.threshold = gc_step_threshold(g, stepsize, stepheadroom);
    return g->gc.state == GCSatomic ? 2 : 3;
  }
  setvmstate(g, GC);
  lim = (stepsize/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    int oldstate = g->gc.state;
    MSize stepcost = (MSize)gc_onestep(L, atomicmode == 1 ? GCSWEEPSTRMAX : 0);
    lim -= stepcost;
    host_stepcost += stepcost;
    /* Atomic is the only long, indivisible incremental phase. Leave a valid
    ** collector boundary after propagation so an embedding host can schedule
    ** atomic at a serialized post-submit point. Mutator write barriers remain
    ** active in GCSatomic and atomic() re-marks roots and gray-again objects. */
    if (atomicmode && oldstate == GCSpropagate &&
	 g->gc.state == GCSatomic) {
      g->gc.threshold = gc_step_threshold(g, stepsize, stepheadroom);
      g->vmstate = ostate;
      return 2;  /* Reached, but deliberately did not execute, atomic. */
    }
    /* A userdata finalizer is arbitrary Lua/C code and GCFINALIZECOST is only
    ** accounting, not a real time bound. Automatic steps must never run it on a
    ** gameplay worker. An engine-sized call entering this phase executes at
    ** most one finalizer before giving the host a chance to check its budget. */
    if (atomicmode && oldstate != GCSfinalize &&
	 g->gc.state == GCSfinalize) {
      g->gc.threshold = gc_step_threshold(g, stepsize, stepheadroom);
      g->vmstate = ostate;
      return 3;  /* Reached finalize without invoking a callback. */
    }
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
    if (atomicmode == 1 && oldstate == GCSfinalize) {
      g->gc.threshold = gc_step_threshold(g, stepsize, stepheadroom);
      g->vmstate = ostate;
      return 3;  /* Invoked at most one finalizer callback. */
    }
    /* Engine exact-step calls are wall-time scheduled by the host. Return after
    ** every object-bounded string sweep batch, after the atomic commit and after
    ** one ordinary root-list sweep block. This makes the QPC budget observable
    ** at a real GC work boundary instead of a hash-bucket boundary. */
    if (atomicmode == 1 &&
        (oldstate == GCSatomic || oldstate == GCSsweep ||
         oldstate == GCSsweepstring)) {
      if (g->gc.debt > host_stepcost)
        g->gc.debt -= host_stepcost;
      else
        g->gc.debt = 0;
      g->gc.threshold = gc_step_threshold(g, stepsize, stepheadroom);
      g->vmstate = ostate;
      return -1;  /* Active cycle; host decides whether another slice fits. */
    }
  } while ((int32_t)lim > 0);
  if (g->gc.debt < stepsize) {
    g->gc.threshold = gc_step_threshold(g, stepsize, stepheadroom);
    g->vmstate = ostate;
    return -1;
  } else {
    g->gc.debt -= stepsize;
    g->gc.threshold = gc_step_threshold(g, 0, stepheadroom);
    g->vmstate = ostate;
    return 0;
  }
}

int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  /* With host management disabled this is the unmodified stock collector.
  ** When enabled, automatic allocation steps may perform bounded marking and
  ** sweeping on the calling thread, but atomic and finalizer callbacks are left
  ** to the serialized host point. Public lua_gc() entry points below retain
  ** their original semantics. */
  return gc_step(L, GCSTEPSIZE, g->gc.stepheadroom,
		 g->gc.hostmanaged ? 2 : 0);
}

/* Preserve the Lua 5.1 collectgarbage("step", n) contract even when the host
** enabled allocation headroom for automatic steps. */
int LJ_FASTCALL lj_gc_step_noheadroom(lua_State *L)
{
  return gc_step(L, GCSTEPSIZE, 0, 0);
}

/* Engine-only sized step. Automatic GC and lua_gc(LUA_GCSTEP, ...) continue
** to use the original 1 KiB accounting quantum above. */
int LJ_FASTCALL lj_gc_step_sized(lua_State *L, MSize stepsize,
				 MSize stepheadroom)
{
  return gc_step(L, stepsize, stepheadroom, 1);
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->jit_L));
  L->base = mref(G(L)->jit_base, TValue);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.preatomic_stage = 0;
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
    setmref(g->gc.sweepstrptr, &g->strhash[0]);
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L, 0);  /* Finish sweep. */
  lua_assert(g->gc.state == GCSfinalize || g->gc.state == GCSpause);
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L, 0); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(&uv->tv))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
}
#endif

/* -- Allocator ----------------------------------------------------------- */

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, MSize osz, MSize nsz)
{
  global_State *g = G(L);
  lua_assert((osz == 0) == (p == NULL));
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lua_assert((nsz == 0) == (p == NULL));
  lua_assert(checkptr32(p));
  g->gc.total = (g->gc.total - osz) + nsz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, MSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lua_assert(checkptr32(o));
  g->gc.total += size;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  return o;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}
