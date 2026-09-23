// opt.h - machine-independent optimizations on the typed AST (-o prod).
#pragma once

#include "parse/ast.h"

typedef struct {
  int folded;     // constant subexpressions evaluated at compile time
  int simplified; // algebraic identities and strength reductions applied
  int branches;   // conditional branches with constant conditions removed
} OptStats;

void optimize_program(Program *prog, OptStats *stats);
