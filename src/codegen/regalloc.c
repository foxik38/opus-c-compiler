// regalloc.c - keeps hot scalar locals in registers (-o prod).
//
// A local qualifies if its address is never taken. Candidates are ranked by
// a use count weighted by loop depth (8x per nesting level).
//
//   * integers and pointers get the callee-saved %rbx, %r12-%r15: they
//     survive calls, so the only cost is a save/restore in the prologue;
//   * float and double get %xmm12-%xmm15. The ABI has no callee-saved XMM
//     registers, so these are written back to the variable's stack slot
//     before every call and reloaded afterwards.
#include "codegen/cg.h"

static const char *const gp_names[NUM_CALLEE_SAVED][4] = {
    {"%bl", "%bx", "%ebx", "%rbx"},     {"%r12b", "%r12w", "%r12d", "%r12"},
    {"%r13b", "%r13w", "%r13d", "%r13"}, {"%r14b", "%r14w", "%r14d", "%r14"},
    {"%r15b", "%r15w", "%r15d", "%r15"},
};

static const char *const xmm_names[NUM_XMM_VARS] = {"%xmm12", "%xmm13", "%xmm14", "%xmm15"};

const char *callee_reg(int reg, int size) {
  int col = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
  return gp_names[reg][col];
}

bool is_xmm_var(const Obj *var) { return var->is_local && var->reg >= NUM_CALLEE_SAVED; }
bool is_gp_reg_var(const Obj *var) { return var->is_local && var->reg >= 0 && var->reg < NUM_CALLEE_SAVED; }
const char *xmm_var_reg(const Obj *var) { return xmm_names[var->reg - NUM_CALLEE_SAVED]; }

static bool is_candidate(Obj *fn, Obj *var, bool fp) {
  if (var->addr_taken || var->ty->is_volatile || var == fn->va_area)
    return false;
  bool ok = fp ? (var->ty->kind == TY_FLOAT || var->ty->kind == TY_DOUBLE)
               : (is_integer(var->ty) || var->ty->kind == TY_PTR);
  // A register costs a save/restore pair; single uses are not worth it.
  return ok && var->weight >= 2;
}

// Picks the `n` heaviest candidates and assigns registers first_reg, first_reg+1, ...
static int assign(Obj *fn, bool fp, int n, int first_reg) {
  Obj *best[NUM_CALLEE_SAVED > NUM_XMM_VARS ? NUM_CALLEE_SAVED : NUM_XMM_VARS] = {};
  for (Obj *var = fn->locals; var; var = var->next) {
    if (!is_candidate(fn, var, fp))
      continue;
    for (int i = 0; i < n; i++) {
      if (!best[i] || var->weight > best[i]->weight) {
        for (int j = n - 1; j > i; j--)
          best[j] = best[j - 1];
        best[i] = var;
        break;
      }
    }
  }
  int count = 0;
  for (int i = 0; i < n && best[i]; i++, count++)
    best[i]->reg = first_reg + i;
  return count;
}

void promote_locals(Obj *fn) {
  int gp = assign(fn, false, NUM_CALLEE_SAVED, 0);
  for (int i = 0; i < gp; i++)
    cg.used_regs |= 1u << i;
  int xmm = assign(fn, true, NUM_XMM_VARS, NUM_CALLEE_SAVED);
  cg.stats->promoted_vars += gp + xmm;
}

void spill_xmm_vars(bool reload) {
  for (Obj *var = cg.fn->locals; var; var = var->next) {
    if (!is_xmm_var(var))
      continue;
    const char *mov = var->ty->kind == TY_FLOAT ? "movss" : "movsd";
    if (reload)
      emit("%s %d(%%rbp), %s", mov, var->offset, xmm_var_reg(var));
    else
      emit("%s %s, %d(%%rbp)", mov, xmm_var_reg(var), var->offset);
  }
}
