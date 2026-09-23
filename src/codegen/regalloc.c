// regalloc.c - keeps hot scalar locals in callee-saved registers (-o prod).
//
// A local qualifies if its address is never taken and it holds an integer
// or pointer. Candidates are ranked by a use count weighted by loop depth
// (8x per nesting level) and the best five get %rbx and %r12-%r15. Callee-
// saved registers survive calls, so no spilling is ever needed; the price is
// one save and one restore per register in the prologue and epilogue.
#include "codegen/cg.h"

static const char *const names[NUM_CALLEE_SAVED][4] = {
    {"%bl", "%bx", "%ebx", "%rbx"},     {"%r12b", "%r12w", "%r12d", "%r12"},
    {"%r13b", "%r13w", "%r13d", "%r13"}, {"%r14b", "%r14w", "%r14d", "%r14"},
    {"%r15b", "%r15w", "%r15d", "%r15"},
};

const char *callee_reg(int reg, int size) {
  int col = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
  return names[reg][col];
}

static bool is_candidate(Obj *fn, Obj *var) {
  if (var->addr_taken || var->ty->is_volatile || var == fn->va_area)
    return false;
  if (!is_integer(var->ty) && var->ty->kind != TY_PTR)
    return false;
  // A register costs a save/restore pair; single uses are not worth it.
  return var->weight >= 2;
}

void promote_locals(Obj *fn) {
  Obj *best[NUM_CALLEE_SAVED] = {};
  for (Obj *var = fn->locals; var; var = var->next) {
    if (!is_candidate(fn, var))
      continue;
    // Insertion into a small sorted array (heaviest first).
    for (int i = 0; i < NUM_CALLEE_SAVED; i++) {
      if (!best[i] || var->weight > best[i]->weight) {
        for (int j = NUM_CALLEE_SAVED - 1; j > i; j--)
          best[j] = best[j - 1];
        best[i] = var;
        break;
      }
    }
  }
  for (int i = 0; i < NUM_CALLEE_SAVED && best[i]; i++) {
    best[i]->reg = i;
    cg.used_regs |= 1u << i;
    cg.stats->promoted_vars++;
  }
}
