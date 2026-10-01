/*
** Trace management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_trace_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_gdbjit.h"
#include "lj_record.h"
#include "lj_asm.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_target.h"

/* -- Error handling ------------------------------------------------------ */

/* Synchronous abort with error message. */
void lj_trace_err(jit_State *J, TraceError e)
{
  setnilV(&J->errinfo);  /* No error info. */
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* Synchronous abort with error message and error info. */
void lj_trace_err_info(jit_State *J, TraceError e)
{
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* -- Trace management ---------------------------------------------------- */

/* The current trace is first assembled in J->cur. The variable length
** arrays point to shared, growable buffers (J->irbuf etc.). When trace
** recording ends successfully, the current trace and its data structures
** are copied to a new (compact) GCtrace object.
*/

/* Find a free trace number. */
static TraceNo trace_findfree(jit_State *J)
{
  MSize osz, lim;
  if (J->freetrace == 0)
    J->freetrace = 1;
  for (; J->freetrace < J->sizetrace; J->freetrace++)
    if (traceref(J, J->freetrace) == NULL)
      return J->freetrace++;
  /* Need to grow trace array. */
  lim = (MSize)J->param[JIT_P_maxtrace] + 1;
  if (lim < 2) lim = 2; else if (lim > 65535) lim = 65535;
  osz = J->sizetrace;
  if (osz >= lim)
    return 0;  /* Too many traces. */
  lj_mem_growvec(J->L, J->trace, J->sizetrace, lim, GCRef);
  for (; osz < J->sizetrace; osz++)
    setgcrefnull(J->trace[osz]);
  return J->freetrace;
}

#define TRACE_APPENDVEC(field, szfield, tp) \
  T->field = (tp *)p; \
  memcpy(p, J->cur.field, J->cur.szfield*sizeof(tp)); \
  p += J->cur.szfield*sizeof(tp);

#ifdef LUAJIT_USE_PERFTOOLS
/*
** Create symbol table of JIT-compiled code. For use with Linux perf tools.
** Example usage:
**   perf record -f -e cycles luajit test.lua
**   perf report -s symbol
**   rm perf.data /tmp/perf-*.map
*/
#include <stdio.h>
#include <unistd.h>

static void perftools_addtrace(GCtrace *T)
{
  static FILE *fp;
  GCproto *pt = &gcref(T->startpt)->pt;
  const BCIns *startpc = mref(T->startpc, const BCIns);
  const char *name = proto_chunknamestr(pt);
  BCLine lineno;
  if (name[0] == '@' || name[0] == '=')
    name++;
  else
    name = "(string)";
  lua_assert(startpc >= proto_bc(pt) && startpc < proto_bc(pt) + pt->sizebc);
  lineno = lj_debug_line(pt, proto_bcpos(pt, startpc));
  if (!fp) {
    char fname[40];
    sprintf(fname, "/tmp/perf-%d.map", getpid());
    if (!(fp = fopen(fname, "w"))) return;
    setlinebuf(fp);
  }
  fprintf(fp, "%lx %x TRACE_%d::%s:%u\n",
	  (long)T->mcode, T->szmcode, T->traceno, name, lineno);
}
#endif

/* Allocate space for copy of trace. */
static GCtrace *trace_save_alloc(jit_State *J)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (J->cur.nins-J->cur.nk)*sizeof(IRIns);
  size_t sz = sztr + szins +
	      J->cur.nsnap*sizeof(SnapShot) +
	      J->cur.nsnapmap*sizeof(SnapEntry);
  return lj_mem_newt(J->L, (MSize)sz, GCtrace);
}

/* Save current trace by copying and compacting it. */
static void trace_save(jit_State *J, GCtrace *T)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (J->cur.nins-J->cur.nk)*sizeof(IRIns);
  char *p = (char *)T + sztr;
  memcpy(T, &J->cur, sizeof(GCtrace));
  setgcrefr(T->nextgc, J2G(J)->gc.root);
  setgcrefp(J2G(J)->gc.root, T);
  newwhite(J2G(J), T);
  T->gct = ~LJ_TTRACE;
  T->ir = (IRIns *)p - J->cur.nk;
  memcpy(p, J->cur.ir+J->cur.nk, szins);
  p += szins;
  TRACE_APPENDVEC(snap, nsnap, SnapShot)
  TRACE_APPENDVEC(snapmap, nsnapmap, SnapEntry)
  J->cur.traceno = 0;
  setgcrefp(J->trace[T->traceno], T);
  lj_gc_barriertrace(J2G(J), T->traceno);
  lj_gdbjit_addtrace(J, T);
#ifdef LUAJIT_USE_PERFTOOLS
  perftools_addtrace(T);
#endif
}

void LJ_FASTCALL lj_trace_free(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  if (T->traceno) {
    lj_gdbjit_deltrace(J, T);
    if (T->traceno < J->freetrace)
      J->freetrace = T->traceno;
    setgcrefnull(J->trace[T->traceno]);
  }
  lj_mem_free(g, T,
    ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
    T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry));
}

/* Re-enable compiling a prototype by unpatching any modified bytecode. */
void lj_trace_reenableproto(GCproto *pt)
{
  if ((pt->flags & PROTO_ILOOP)) {
    BCIns *bc = proto_bc(pt);
    BCPos i, sizebc = pt->sizebc;;
    pt->flags &= ~PROTO_ILOOP;
    if (bc_op(bc[0]) == BC_IFUNCF)
      setbc_op(&bc[0], BC_FUNCF);
    for (i = 1; i < sizebc; i++) {
      BCOp op = bc_op(bc[i]);
      if (op == BC_IFORL || op == BC_IITERL || op == BC_ILOOP)
	setbc_op(&bc[i], (int)op+(int)BC_LOOP-(int)BC_ILOOP);
    }
  }
}

/* Unpatch the bytecode modified by a root trace. */
static void trace_unpatch(jit_State *J, GCtrace *T)
{
  BCOp op = bc_op(T->startins);
  BCIns *pc = mref(T->startpc, BCIns);
  UNUSED(J);
  if (op == BC_JMP)
    return;  /* No need to unpatch branches in parent traces (yet). */
  switch (bc_op(*pc)) {
  case BC_JFORL:
    lua_assert(traceref(J, bc_d(*pc)) == T);
    *pc = T->startins;
    pc += bc_j(T->startins);
    lua_assert(bc_op(*pc) == BC_JFORI);
    setbc_op(pc, BC_FORI);
    break;
  case BC_JITERL:
  case BC_JLOOP:
    lua_assert(op == BC_ITERL || op == BC_LOOP || bc_isret(op));
    *pc = T->startins;
    break;
  case BC_JFUNCF:
    lua_assert(op == BC_FUNCF);
    *pc = T->startins;
    break;
  default:  /* Already unpatched. */
    break;
  }
}

/* Flush a root trace. */
static void trace_flushroot(jit_State *J, GCtrace *T)
{
  GCproto *pt = &gcref(T->startpt)->pt;
  lua_assert(T->root == 0 && pt != NULL);
  /* Unlink root trace from chain anchored in prototype. */
  if (pt->trace == T->traceno) {  /* Trace is first in chain. Easy. */
    pt->trace = T->nextroot;
unpatch:
    /* Unpatch modified bytecode only if the trace has not been flushed. */
    trace_unpatch(J, T);
  } else if (pt->trace) {  /* Otherwise search in chain of root traces. */
    GCtrace *T2 = traceref(J, pt->trace);
    if (T2) {
      for (; T2->nextroot; T2 = traceref(J, T2->nextroot))
	if (T2->nextroot == T->traceno) {
	  T2->nextroot = T->nextroot;  /* Unlink from chain. */
	  goto unpatch;
	}
    }
  }
}

/* Flush a trace. Only root traces are considered. */
void lj_trace_flush(jit_State *J, TraceNo traceno)
{
  if (traceno > 0 && traceno < J->sizetrace) {
    GCtrace *T = traceref(J, traceno);
    if (T && T->root == 0)
      trace_flushroot(J, T);
  }
}

/* Flush all traces associated with a prototype. */
void lj_trace_flushproto(global_State *g, GCproto *pt)
{
  jit_State *J = G2J(g);
  uint32_t i;
  while (pt->trace != 0)
    trace_flushroot(J, traceref(J, pt->trace));
  /* Explicit per-prototype JIT mode changes must also remove finite NYI
  ** cooldowns, so re-enabled or runtime-replaced mod code gets a clean retry. */
  for (i = 0; i < JIT_ROOT_NYI_SLOTS; i++) {
    JitRootNyiPenalty *penalty = &J->root_nyi_penalty[i];
    if (penalty->used && mref(penalty->pt, GCproto) == pt)
      memset(penalty, 0, sizeof(*penalty));
  }
}

/* Flush all traces. */
int lj_trace_flushall(lua_State *L)
{
  jit_State *J = L2J(L);
  ptrdiff_t i;
  if ((J2G(J)->hookmask & HOOK_GC))
    return 1;
  J->stats_trace_flushes++;
  for (i = (ptrdiff_t)J->sizetrace-1; i > 0; i--) {
    GCtrace *T = traceref(J, i);
    if (T) {
      if (T->root == 0)
	trace_flushroot(J, T);
      lj_gdbjit_deltrace(J, T);
      T->traceno = 0;
      setgcrefnull(J->trace[i]);
    }
  }
  J->cur.traceno = 0;
  J->freetrace = 0;
  /* Clear penalty caches tied to trace numbers/exits. */
  memset(J->penalty, 0, sizeof(J->penalty));
  memset(J->side_nyi_penalty, 0, sizeof(J->side_nyi_penalty));
  J->side_nyi_penalty_slot = 0;
  memset(J->root_nyi_penalty, 0, sizeof(J->root_nyi_penalty));
  J->root_nyi_penalty_slot = 0;
  /* Free the whole machine code and invalidate all exit stub groups. */
  lj_mcode_free(J);
  memset(J->exitstubgroup, 0, sizeof(J->exitstubgroup));
  lj_vmevent_send(L, TRACE,
    setstrV(L, L->top++, lj_str_newlit(L, "flush"));
  );
  return 0;
}

/* Initialize JIT compiler state. */
void lj_trace_initstate(global_State *g)
{
  jit_State *J = G2J(g);
  TValue *tv;
  /* Initialize SIMD constants. */
  tv = LJ_KSIMD(J, LJ_KSIMD_ABS);
  tv[0].u64 = U64x(7fffffff,ffffffff);
  tv[1].u64 = U64x(7fffffff,ffffffff);
  tv = LJ_KSIMD(J, LJ_KSIMD_NEG);
  tv[0].u64 = U64x(80000000,00000000);
  tv[1].u64 = U64x(80000000,00000000);
}

/* Free everything associated with the JIT compiler state. */
void lj_trace_freestate(global_State *g)
{
  jit_State *J = G2J(g);
#ifdef LUA_USE_ASSERT
  {  /* This assumes all traces have already been freed. */
    ptrdiff_t i;
    for (i = 1; i < (ptrdiff_t)J->sizetrace; i++)
      lua_assert(i == (ptrdiff_t)J->cur.traceno || traceref(J, i) == NULL);
  }
#endif
  lj_mcode_free(J);
  lj_ir_k64_freeall(J);
  lj_mem_freevec(g, J->snapmapbuf, J->sizesnapmap, SnapEntry);
  lj_mem_freevec(g, J->snapbuf, J->sizesnap, SnapShot);
  lj_mem_freevec(g, J->irbuf + J->irbotlim, J->irtoplim - J->irbotlim, IRIns);
  lj_mem_freevec(g, J->trace, J->sizetrace, GCRef);
}

/* -- Penalties and blacklisting ------------------------------------------ */

/* Blacklist a bytecode instruction. */
static void blacklist_pc(jit_State *J, GCproto *pt, BCIns *pc)
{
  setbc_op(pc, (int)bc_op(*pc)+(int)BC_ILOOP-(int)BC_LOOP);
  pt->flags |= PROTO_ILOOP;
  J->stats_blacklisted_sites++;
}

/* Penalize a bytecode instruction. */
static void penalty_pc(jit_State *J, GCproto *pt, BCIns *pc, TraceError e)
{
  uint32_t i, val = PENALTY_MIN;
  for (i = 0; i < PENALTY_SLOTS; i++)
    if (mref(J->penalty[i].pc, const BCIns) == pc) {  /* Cache slot found? */
      /* First try to bump its hotcount several times. */
      val = ((uint32_t)J->penalty[i].val << 1) +
	    LJ_PRNG_BITS(J, PENALTY_RNDBITS);
      if (val > PENALTY_MAX) {
	blacklist_pc(J, pt, pc);  /* Blacklist it, if that didn't help. */
	return;
      }
      goto setpenalty;
    }
  /* Assign a new penalty cache slot. */
  i = J->penaltyslot;
  J->penaltyslot = (J->penaltyslot + 1) & (PENALTY_SLOTS-1);
  setmref(J->penalty[i].pc, pc);
setpenalty:
  J->penalty[i].val = (uint16_t)val;
  J->penalty[i].reason = e;
  hotcount_set(J2GG(J), pc+1, val);
}

static void trace_clear_root_nyi(jit_State *J, GCproto *pt, const BCIns *pc);

/* -- Trace compiler state machine ---------------------------------------- */

/* Start tracing. */
static void trace_start(jit_State *J)
{
  lua_State *L;
  TraceNo traceno;

  if ((J->pt->flags & PROTO_NOJIT)) {  /* JIT disabled for this proto? */
    if (J->parent == 0) {
      /* Lazy bytecode patching to disable hotcount events. */
      lua_assert(bc_op(*J->pc) == BC_FORL || bc_op(*J->pc) == BC_ITERL ||
		 bc_op(*J->pc) == BC_LOOP || bc_op(*J->pc) == BC_FUNCF);
      setbc_op(J->pc, (int)bc_op(*J->pc)+(int)BC_ILOOP-(int)BC_LOOP);
      J->pt->flags |= PROTO_ILOOP;
    }
    J->state = LJ_TRACE_IDLE;  /* Silently ignored. */
    return;
  }

  /* Get a new trace number. */
  J->stats_trace_starts++;
  if (J->parent)
    J->stats_side_trace_starts++;
  else
    J->stats_root_trace_starts++;
  traceno = trace_findfree(J);
  if (LJ_UNLIKELY(traceno == 0)) {  /* No free trace? */
    lua_assert((J2G(J)->hookmask & HOOK_GC) == 0);
    J->stats_maxtrace_flushes++;
    lj_trace_flushall(J->L);
    J->state = LJ_TRACE_IDLE;  /* Silently ignored. */
    return;
  }
  setgcrefp(J->trace[traceno], &J->cur);

  /* Setup enough of the current trace to be able to send the vmevent. */
  memset(&J->cur, 0, sizeof(GCtrace));
  J->cur.traceno = traceno;
  J->cur.nins = J->cur.nk = REF_BASE;
  J->cur.ir = J->irbuf;
  J->cur.snap = J->snapbuf;
  J->cur.snapmap = J->snapmapbuf;
  J->mergesnap = 0;
  J->needsnap = 0;
  J->bcskip = 0;
  J->guardemit.irt = 0;
  J->postproc = LJ_POST_NONE;
  lj_resetsplit(J);
  setgcref(J->cur.startpt, obj2gco(J->pt));

  L = J->L;
  lj_vmevent_send(L, TRACE,
    setstrV(L, L->top++, lj_str_newlit(L, "start"));
    setintV(L->top++, traceno);
    setfuncV(L, L->top++, J->fn);
    setintV(L->top++, proto_bcpos(J->pt, J->pc));
    if (J->parent) {
      setintV(L->top++, J->parent);
      setintV(L->top++, J->exitno);
    }
  );
  lj_record_setup(J);
}

/* Stop tracing. */
static void trace_stop(jit_State *J)
{
  BCIns *pc = mref(J->cur.startpc, BCIns);
  BCOp op = bc_op(J->cur.startins);
  GCproto *pt = &gcref(J->cur.startpt)->pt;
  TraceNo traceno = J->cur.traceno;
  GCtrace *T = trace_save_alloc(J);  /* Do this first. May throw OOM. */
  lua_State *L;

  switch (op) {
  case BC_FORL:
    setbc_op(pc+bc_j(J->cur.startins), BC_JFORI);  /* Patch FORI, too. */
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
    /* Patch bytecode of starting instruction in root trace. */
    setbc_op(pc, (int)op+(int)BC_JLOOP-(int)BC_LOOP);
    setbc_d(pc, traceno);
  addroot:
    /* Add to root trace chain in prototype. */
    J->cur.nextroot = pt->trace;
    pt->trace = (TraceNo1)traceno;
    break;
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    *pc = BCINS_AD(BC_JLOOP, J->cur.snap[0].nslots, traceno);
    goto addroot;
  case BC_JMP:
    /* Patch exit branch in parent to side trace entry. */
    lua_assert(J->parent != 0 && J->cur.root != 0);
    lj_asm_patchexit(J, traceref(J, J->parent), J->exitno, J->cur.mcode);
    /* Avoid compiling a side trace twice (stack resizing uses parent exit). */
    {
      SnapShot *snap = &traceref(J, J->parent)->snap[J->exitno];
      snap->count = SNAPCOUNT_DONE;
      if (J->cur.topslot > snap->topslot) snap->topslot = J->cur.topslot;
    }
    /* Add to side trace chain in root trace. */
    {
      GCtrace *root = traceref(J, J->cur.root);
      root->nchild++;
      J->cur.nextside = root->nextside;
      root->nextside = (TraceNo1)traceno;
    }
    break;
  default:
    lua_assert(0);
    break;
  }

  /* Commit new mcode only after all patching is done. */
  lj_mcode_commit(J, J->cur.mcode);
  J->postproc = LJ_POST_NONE;
  if (J->parent == 0)
    trace_clear_root_nyi(J, &gcref(J->cur.startpt)->pt, mref(J->cur.startpc, const BCIns));
  trace_save(J, T);
  J->stats_trace_compiled++;

  L = J->L;
  lj_vmevent_send(L, TRACE,
    setstrV(L, L->top++, lj_str_newlit(L, "stop"));
    setintV(L->top++, traceno);
  );
}

/* Start a new root trace for down-recursion. */
static int trace_downrec(jit_State *J)
{
  /* Restart recording at the return instruction. */
  lua_assert(J->pt != NULL);
  lua_assert(bc_isret(bc_op(*J->pc)));
  if (bc_op(*J->pc) == BC_RETM)
    return 0;  /* NYI: down-recursion with RETM. */
  J->parent = 0;
  J->exitno = 0;
  J->state = LJ_TRACE_RECORD;
  trace_start(J);
  return 1;
}

static int trace_error_is_nyi(TraceError e)
{
  switch (e) {
  case LJ_TRERR_NYIBC:
  case LJ_TRERR_NYICF:
  case LJ_TRERR_NYIFF:
  case LJ_TRERR_NYIFFU:
  case LJ_TRERR_NYIRETL:
  case LJ_TRERR_NYITMIX:
  case LJ_TRERR_NYICONV:
  case LJ_TRERR_NYICALL:
  case LJ_TRERR_NYIIR:
  case LJ_TRERR_NYIPHI:
  case LJ_TRERR_NYICOAL:
    return 1;
  default:
    return 0;
  }
}

static void trace_record_abort_site(jit_State *J, TraceError e)
{
  GCproto *pt;
  const BCIns *pc;
  const char *source;
  size_t source_len;
  BCLine line;
  uint32_t i, slot = JIT_ABORT_SITE_SLOTS, minslot = 0;
  uint64_t mincount = ~(uint64_t)0;
  JitAbortSiteStat *site;

  /* Prefer the instruction that actually raised the NYI. Fall back to the
  ** trace start only when the recorder no longer has a valid current PC. */
  pt = J->pt;
  pc = J->pc;
  if (pt == NULL || pc == NULL || pc < proto_bc(pt) || pc >= proto_bc(pt) + pt->sizebc) {
    if (!gcref(J->cur.startpt) || !mref(J->cur.startpc, BCIns))
      return;
    pt = &gcref(J->cur.startpt)->pt;
    pc = mref(J->cur.startpc, const BCIns);
    if (pc < proto_bc(pt) || pc >= proto_bc(pt) + pt->sizebc)
      return;
  }
  source = proto_chunknamestr(pt);
  line = lj_debug_line(pt, proto_bcpos(pt, pc));
  if (source[0] == '@' || source[0] == '=')
    source++;
  source_len = strlen(source);
  if (source_len >= JIT_ABORT_SOURCE_LEN)
    source_len = JIT_ABORT_SOURCE_LEN - 1;

  for (i = 0; i < JIT_ABORT_SITE_SLOTS; i++) {
    site = &J->stats_abort_sites[i];
    if (site->used && site->line == (uint32_t)line &&
        site->reason == (uint16_t)e && site->side_trace == (uint8_t)(J->parent != 0) &&
        strncmp(site->source, source, source_len) == 0 && site->source[source_len] == '\0') {
      site->count++;
      return;
    }
    if (!site->used && slot == JIT_ABORT_SITE_SLOTS)
      slot = i;
    if (site->used && site->count < mincount) {
      mincount = site->count;
      minslot = i;
    }
  }

  /* Fixed-size Space-Saving table: no Lua/allocator activity in an abort path. */
  if (slot == JIT_ABORT_SITE_SLOTS)
    slot = minslot;
  site = &J->stats_abort_sites[slot];
  {
    uint64_t inherited = site->used ? site->count : 0;
    memset(site, 0, sizeof(*site));
    memcpy(site->source, source, source_len);
    site->source[source_len] = '\0';
    site->count = inherited + 1;
    site->line = (uint32_t)line;
    site->reason = (uint16_t)e;
    site->side_trace = (uint8_t)(J->parent != 0);
    site->used = 1;
  }
}

static void trace_penalize_side_nyi(jit_State *J)
{
  uint32_t i, slot = JIT_SIDE_NYI_SLOTS;
  JitSideNyiPenalty *penalty;
  GCtrace *parent;
  SnapShot *snap;

  if (!J->parent || J->parent >= J->sizetrace)
    return;
  parent = traceref(J, J->parent);
  if (!parent || J->exitno >= parent->nsnap)
    return;
  snap = &parent->snap[J->exitno];

  for (i = 0; i < JIT_SIDE_NYI_SLOTS; i++) {
    penalty = &J->side_nyi_penalty[i];
    if (penalty->used && penalty->parent == J->parent && penalty->exitno == J->exitno) {
      slot = i;
      break;
    }
  }
  if (slot == JIT_SIDE_NYI_SLOTS) {
    slot = J->side_nyi_penalty_slot++ & (JIT_SIDE_NYI_SLOTS-1);
    penalty = &J->side_nyi_penalty[slot];
    memset(penalty, 0, sizeof(*penalty));
    penalty->parent = J->parent;
    penalty->exitno = J->exitno;
    penalty->used = 1;
  } else {
    penalty = &J->side_nyi_penalty[slot];
  }

  if (penalty->failures < 0xffffu)
    penalty->failures++;
  if (penalty->failures >= 4u) {
    if (snap->count != SNAPCOUNT_DONE) {
      snap->count = SNAPCOUNT_DONE;
      J->stats_side_nyi_disabled++;
    }
  } else {
    /* Do not retry at every occurrence after an NYI side-trace abort. Re-arm the
    ** ordinary hotexit threshold; correctness stays in the interpreter. */
    snap->count = 0;
  }
}

/* Adaptive cooldown for root traces which repeatedly abort at a structural NYI.
** Unlike the stock bytecode penalty/blacklist this is finite and does not alter
** bytecode. Runtime-dependent mod code therefore gets periodic retry chances. */
static uint32_t trace_root_nyi_signature(GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  uint32_t signature = (uint32_t)pt->sizebc * 16777619u;
  signature ^= (uint32_t)pt->sizekgc * 2246822519u;
  signature ^= (uint32_t)pt->firstline * 3266489917u;
  signature ^= (uint32_t)pt->numline * 668265263u;
  signature ^= ((uint32_t)pt->framesize << 24) ^ ((uint32_t)pt->numparams << 16);
  if (pt->sizebc) {
    signature ^= (uint32_t)bc[0] * 374761393u;
    signature ^= (uint32_t)bc[pt->sizebc-1] * 1274126177u;
  }
  return signature;
}

static JitRootNyiPenalty *trace_root_nyi_penalty(jit_State *J, GCproto *pt,
                                                 const BCIns *pc, int create)
{
  /* Four-way set associative table. GAMMA can have hundreds of concurrently hot
  ** NYI sites; a linear 64/128-entry penalty cache thrashes on that workload.
  ** This keeps lookup constant-time while staying allocation-free. The signature
  ** prevents allocator address reuse from inheriting a stale prototype's cooldown;
  ** the stored MRefs are compared only as addresses and are never dereferenced. */
  enum { ROOT_NYI_WAYS = 4, ROOT_NYI_BUCKETS = JIT_ROOT_NYI_SLOTS / ROOT_NYI_WAYS };
  uint32_t signature = trace_root_nyi_signature(pt);
  uint32_t hash = (u32ptr(pt) >> 4) ^ ((u32ptr(pc) >> 2) * 2654435761u) ^ signature;
  uint32_t base = (hash & (ROOT_NYI_BUCKETS-1)) * ROOT_NYI_WAYS;
  uint32_t i, empty = JIT_ROOT_NYI_SLOTS;
  JitRootNyiPenalty *slot;
  for (i = 0; i < ROOT_NYI_WAYS; i++) {
    slot = &J->root_nyi_penalty[base+i];
    if (slot->used) {
      if (slot->signature == signature && mref(slot->pt, GCproto) == pt &&
          mref(slot->pc, const BCIns) == pc)
        return slot;
    } else if (empty == JIT_ROOT_NYI_SLOTS) {
      empty = base+i;
    }
  }
  if (!create) return NULL;
  i = empty != JIT_ROOT_NYI_SLOTS ? empty :
      base + (J->root_nyi_penalty_slot++ & (ROOT_NYI_WAYS-1));
  slot = &J->root_nyi_penalty[i];
  memset(slot, 0, sizeof(*slot));
  setmref(slot->pt, pt);
  setmref(slot->pc, pc);
  slot->signature = signature;
  slot->used = 1;
  return slot;
}

static void trace_penalize_root_nyi(jit_State *J)
{
  GCproto *pt = &gcref(J->cur.startpt)->pt;
  const BCIns *pc = mref(J->cur.startpc, const BCIns);
  JitRootNyiPenalty *penalty = trace_root_nyi_penalty(J, pt, pc, 1);
  uint32_t shift;
  if (penalty->failures < 0xffffu)
    penalty->failures++;
  shift = penalty->failures > 15u ? 15u : penalty->failures;
  penalty->cooldown = (uint16_t)(1u << shift);  /* 2, 4, ... 32768 hot events. */
  J->stats_root_nyi_backoffs++;
}

static int trace_defer_root_nyi(jit_State *J, GCproto *pt, const BCIns *pc)
{
  JitRootNyiPenalty *penalty = trace_root_nyi_penalty(J, pt, pc, 0);
  if (penalty && penalty->cooldown) {
    penalty->cooldown--;
    J->stats_root_nyi_suppressed++;
    return 1;
  }
  return 0;
}

static void trace_clear_root_nyi(jit_State *J, GCproto *pt, const BCIns *pc)
{
  JitRootNyiPenalty *penalty = trace_root_nyi_penalty(J, pt, pc, 0);
  if (penalty)
    memset(penalty, 0, sizeof(*penalty));
}

/* Abort tracing. */
static int trace_abort(jit_State *J)
{
  lua_State *L = J->L;
  TraceError e = LJ_TRERR_RECERR;
  TraceNo traceno;

  J->postproc = LJ_POST_NONE;
  lj_mcode_abort(J);
  if (tvisnumber(L->top-1))
    e = (TraceError)numberVint(L->top-1);
  if (e == LJ_TRERR_MCODELM) {
    J->stats_mcode_retries++;
    L->top--;  /* Remove error object */
    J->state = LJ_TRACE_ASM;
    return 1;  /* Retry ASM with new MCode area. */
  }

  J->stats_trace_aborts++;
  if (J->parent)
    J->stats_side_trace_aborts++;
  else
    J->stats_root_trace_aborts++;
  if (e == LJ_TRERR_MCODEAL || e == LJ_TRERR_MCODEOV)
    J->stats_abort_mcode++;
  if (e == LJ_TRERR_BLACKL)
    J->stats_abort_blacklist++;
  if (trace_error_is_nyi(e)) {
    J->stats_abort_nyi++;
    trace_record_abort_site(J, e);
    if (J->parent)
      trace_penalize_side_nyi(J);
    else
      trace_penalize_root_nyi(J);
  }

  /* Penalize or blacklist starting bytecode instruction. Structural NYI roots
  ** use the finite engine-side cooldown above instead: GAMMA has far more than
  ** PENALTY_SLOTS simultaneously hot NYI sites, which made the stock cache thrash,
  ** while permanent bytecode blacklisting is undesirable for runtime-patched mod
  ** code. Non-NYI aborts retain LuaJIT's original penalty policy. */
  if (J->parent == 0 && !trace_error_is_nyi(e) && !bc_isret(bc_op(J->cur.startins)))
    penalty_pc(J, &gcref(J->cur.startpt)->pt, mref(J->cur.startpc, BCIns), e);

  /* Is there anything to abort? */
  traceno = J->cur.traceno;
  if (traceno) {
    ptrdiff_t errobj = savestack(L, L->top-1);  /* Stack may be resized. */
    J->cur.link = 0;
    J->cur.linktype = LJ_TRLINK_NONE;
    lj_vmevent_send(L, TRACE,
      cTValue *bot = tvref(L->stack);
      cTValue *frame;
      const BCIns *pc;
      BCPos pos = 0;
      setstrV(L, L->top++, lj_str_newlit(L, "abort"));
      setintV(L->top++, traceno);
      /* Find original Lua function call to generate a better error message. */
      for (frame = J->L->base-1, pc = J->pc; ; frame = frame_prev(frame)) {
	if (isluafunc(frame_func(frame))) {
	  pos = proto_bcpos(funcproto(frame_func(frame)), pc);
	  break;
	} else if (frame_prev(frame) <= bot) {
	  break;
	} else if (frame_iscont(frame)) {
	  pc = frame_contpc(frame) - 1;
	} else {
	  pc = frame_pc(frame) - 1;
	}
      }
      setfuncV(L, L->top++, frame_func(frame));
      setintV(L->top++, pos);
      copyTV(L, L->top++, restorestack(L, errobj));
      copyTV(L, L->top++, &J->errinfo);
    );
    /* Drop aborted trace after the vmevent (which may still access it). */
    setgcrefnull(J->trace[traceno]);
    if (traceno < J->freetrace)
      J->freetrace = traceno;
    J->cur.traceno = 0;
  }
  L->top--;  /* Remove error object */
  if (e == LJ_TRERR_DOWNREC)
    return trace_downrec(J);
  else if (e == LJ_TRERR_MCODEAL)
    lj_trace_flushall(L);
  return 0;
}

/* Perform pending re-patch of a bytecode instruction. */
static LJ_AINLINE void trace_pendpatch(jit_State *J, int force)
{
  if (LJ_UNLIKELY(J->patchpc)) {
    if (force || J->bcskip == 0) {
      *J->patchpc = J->patchins;
      J->patchpc = NULL;
    } else {
      J->bcskip = 0;
    }
  }
}

/* State machine for the trace compiler. Protected callback. */
static TValue *trace_state(lua_State *L, lua_CFunction dummy, void *ud)
{
  jit_State *J = (jit_State *)ud;
  UNUSED(dummy);
  do {
  retry:
    switch (J->state) {
    case LJ_TRACE_START:
      J->state = LJ_TRACE_RECORD;  /* trace_start() may change state. */
      trace_start(J);
      lj_dispatch_update(J2G(J));
      break;

    case LJ_TRACE_RECORD:
      trace_pendpatch(J, 0);
      setvmstate(J2G(J), RECORD);
      lj_vmevent_send_(L, RECORD,
	/* Save/restore state for trace recorder. */
	TValue savetv = J2G(J)->tmptv;
	TValue savetv2 = J2G(J)->tmptv2;
	TraceNo parent = J->parent;
	ExitNo exitno = J->exitno;
	setintV(L->top++, J->cur.traceno);
	setfuncV(L, L->top++, J->fn);
	setintV(L->top++, J->pt ? (int32_t)proto_bcpos(J->pt, J->pc) : -1);
	setintV(L->top++, J->framedepth);
      ,
	J2G(J)->tmptv = savetv;
	J2G(J)->tmptv2 = savetv2;
	J->parent = parent;
	J->exitno = exitno;
      );
      lj_record_ins(J);
      break;

    case LJ_TRACE_END:
      trace_pendpatch(J, 1);
      J->loopref = 0;
      if ((J->flags & JIT_F_OPT_LOOP) &&
	  J->cur.link == J->cur.traceno && J->framedepth + J->retdepth == 0) {
	setvmstate(J2G(J), OPT);
	lj_opt_dce(J);
	if (lj_opt_loop(J)) {  /* Loop optimization failed? */
	  J->cur.link = 0;
	  J->cur.linktype = LJ_TRLINK_NONE;
	  J->loopref = J->cur.nins;
	  J->state = LJ_TRACE_RECORD;  /* Try to continue recording. */
	  break;
	}
	J->loopref = J->chain[IR_LOOP];  /* Needed by assembler. */
      }
      lj_opt_split(J);
      lj_opt_sink(J);
      if (!J->loopref) J->cur.snap[J->cur.nsnap-1].count = SNAPCOUNT_DONE;
      J->state = LJ_TRACE_ASM;
      break;

    case LJ_TRACE_ASM:
      setvmstate(J2G(J), ASM);
      lj_asm_trace(J, &J->cur);
      trace_stop(J);
      setvmstate(J2G(J), INTERP);
      J->state = LJ_TRACE_IDLE;
      lj_dispatch_update(J2G(J));
      return NULL;

    default:  /* Trace aborted asynchronously. */
      setintV(L->top++, (int32_t)LJ_TRERR_RECERR);
      /* fallthrough */
    case LJ_TRACE_ERR:
      trace_pendpatch(J, 1);
      if (trace_abort(J))
	goto retry;
      setvmstate(J2G(J), INTERP);
      J->state = LJ_TRACE_IDLE;
      lj_dispatch_update(J2G(J));
      return NULL;
    }
  } while (J->state > LJ_TRACE_RECORD);
  return NULL;
}

/* -- Event handling ------------------------------------------------------ */

/* A bytecode instruction is about to be executed. Record it. */
void lj_trace_ins(jit_State *J, const BCIns *pc)
{
  /* Note: J->L must already be set. pc is the true bytecode PC here. */
  J->pc = pc;
  J->fn = curr_func(J->L);
  J->pt = isluafunc(J->fn) ? funcproto(J->fn) : NULL;
  while (lj_vm_cpcall(J->L, NULL, (void *)J, trace_state) != 0)
    J->state = LJ_TRACE_ERR;
}

/* A hotcount triggered. Start recording a root trace. */
void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc)
{
  /* Note: pc is the interpreter bytecode PC here. It's offset by 1. */
  ERRNO_SAVE
  /* Reset hotcount. */
  hotcount_set(J2GG(J), pc, J->param[JIT_P_hotloop]*HOTCOUNT_LOOP);
  /* Only start a new trace if not recording or inside __gc call or vmevent. */
  if (J->state == LJ_TRACE_IDLE &&
      !(J2G(J)->hookmask & (HOOK_GC|HOOK_VMEVENT))) {
    GCfunc *fn = curr_func(J->L);
    GCproto *pt = isluafunc(fn) ? funcproto(fn) : NULL;
    if (pt && trace_defer_root_nyi(J, pt, pc-1)) {
      ERRNO_RESTORE
      return;
    }
    J->parent = 0;  /* Root trace. */
    J->exitno = 0;
    J->state = LJ_TRACE_START;
    lj_trace_ins(J, pc-1);
  }
  ERRNO_RESTORE
}

/* Check for a hot side exit. If yes, start recording a side trace. */
static void trace_hotside(jit_State *J, const BCIns *pc)
{
  SnapShot *snap = &traceref(J, J->parent)->snap[J->exitno];
  if (!(J2G(J)->hookmask & (HOOK_GC|HOOK_VMEVENT)) &&
      snap->count != SNAPCOUNT_DONE &&
      ++snap->count >= J->param[JIT_P_hotexit]) {
    lua_assert(J->state == LJ_TRACE_IDLE);
    /* J->parent is non-zero for a side trace. */
    J->state = LJ_TRACE_START;
    lj_trace_ins(J, pc);
  }
}

/* Tiny struct to pass data to protected call. */
typedef struct ExitDataCP {
  jit_State *J;
  void *exptr;		/* Pointer to exit state. */
  const BCIns *pc;	/* Restart interpreter at this PC. */
} ExitDataCP;

/* Need to protect lj_snap_restore because it may throw. */
static TValue *trace_exit_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  ExitDataCP *exd = (ExitDataCP *)ud;
  /* Always catch error here and don't call error function. */
  cframe_errfunc(L->cframe) = 0;
  cframe_nres(L->cframe) = -2*LUAI_MAXSTACK*(int)sizeof(TValue);
  exd->pc = lj_snap_restore(exd->J, exd->exptr);
  UNUSED(dummy);
  return NULL;
}

#ifndef LUAJIT_DISABLE_VMEVENT
/* Push all registers from exit state. */
static void trace_exit_regs(lua_State *L, ExitState *ex)
{
  int32_t i;
  setintV(L->top++, RID_NUM_GPR);
  setintV(L->top++, RID_NUM_FPR);
  for (i = 0; i < RID_NUM_GPR; i++) {
    if (sizeof(ex->gpr[i]) == sizeof(int32_t))
      setintV(L->top++, (int32_t)ex->gpr[i]);
    else
      setnumV(L->top++, (lua_Number)ex->gpr[i]);
  }
#if !LJ_SOFTFP
  for (i = 0; i < RID_NUM_FPR; i++) {
    setnumV(L->top, ex->fpr[i]);
    if (LJ_UNLIKELY(tvisnan(L->top)))
      setnanV(L->top);
    L->top++;
  }
#endif
}
#endif

#ifdef EXITSTATE_PCREG
/* Determine trace number from pc of exit instruction. */
static TraceNo trace_exit_find(jit_State *J, MCode *pc)
{
  TraceNo traceno;
  for (traceno = 1; traceno < J->sizetrace; traceno++) {
    GCtrace *T = traceref(J, traceno);
    if (T && pc >= T->mcode && pc < (MCode *)((char *)T->mcode + T->szmcode))
      return traceno;
  }
  lua_assert(0);
  return 0;
}
#endif

/* A trace exited. Restore interpreter state. */
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr)
{
  ERRNO_SAVE
  lua_State *L = J->L;
  ExitState *ex = (ExitState *)exptr;
  ExitDataCP exd;
  int errcode;
  const BCIns *pc;
  void *cf;
  GCtrace *T;
#ifdef EXITSTATE_PCREG
  J->parent = trace_exit_find(J, (MCode *)(intptr_t)ex->gpr[EXITSTATE_PCREG]);
#endif
  T = traceref(J, J->parent); UNUSED(T);
#ifdef EXITSTATE_CHECKEXIT
  if (J->exitno == T->nsnap) {  /* Treat stack check like a parent exit. */
    lua_assert(T->root != 0);
    J->exitno = T->ir[REF_BASE].op2;
    J->parent = T->ir[REF_BASE].op1;
    T = traceref(J, J->parent);
  }
#endif
  lua_assert(T != NULL && J->exitno < T->nsnap);
  exd.J = J;
  exd.exptr = exptr;
  errcode = lj_vm_cpcall(L, NULL, &exd, trace_exit_cp);
  if (errcode)
    return -errcode;  /* Return negated error code. */

  lj_vmevent_send(L, TEXIT,
    lj_state_checkstack(L, 4+RID_NUM_GPR+RID_NUM_FPR+LUA_MINSTACK);
    setintV(L->top++, J->parent);
    setintV(L->top++, J->exitno);
    trace_exit_regs(L, ex);
  );

  pc = exd.pc;
  cf = cframe_raw(L->cframe);
  setcframe_pc(cf, pc);
  if (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize) {
    if (!(G(L)->hookmask & HOOK_GC))
      lj_gc_step(L);  /* Exited because of GC: drive GC forward. */
  } else if ((J->flags & JIT_F_ON)) {
    trace_hotside(J, pc);
  }
  if (bc_op(*pc) == BC_JLOOP) {
    BCIns *retpc = &traceref(J, bc_d(*pc))->startins;
    if (bc_isret(bc_op(*retpc))) {
      if (J->state == LJ_TRACE_RECORD) {
	J->patchins = *pc;
	J->patchpc = (BCIns *)pc;
	*J->patchpc = *retpc;
	J->bcskip = 1;
      } else {
	pc = retpc;
	setcframe_pc(cf, pc);
      }
    }
  }
  /* Return MULTRES or 0. */
  ERRNO_RESTORE
  switch (bc_op(*pc)) {
  case BC_CALLM: case BC_CALLMT:
    return (int)((BCReg)(L->top - L->base) - bc_a(*pc) - bc_c(*pc));
  case BC_RETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc) - bc_d(*pc));
  case BC_TSETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc));
  default:
    if (bc_op(*pc) >= BC_FUNCF)
      return (int)((BCReg)(L->top - L->base) + 1);
    return 0;
  }
}

#endif
