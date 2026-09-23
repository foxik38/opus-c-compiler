// opt.h - machine-independent optimizations on the typed AST (-o prod).
#pragma once

#include "parse/ast.h"

typedef struct {
  int folded;     // constant subexpressions evaluated at compile time
  int simplified; // algebraic identities and strength reductions applied
  int branches;   // conditional branches with constant conditions removed
  int hoisted;    // loop-invariant computations moved out of loops
} OptStats;

void optimize_program(Program *prog, OptStats *stats);
void hoist_loop_invariants(Program *prog, OptStats *stats); // licm.c
