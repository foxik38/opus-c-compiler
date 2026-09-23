// codegen.h - x86-64 (System V AMD64 ABI) assembly generation.
#pragma once

#include "parse/ast.h"

typedef struct {
  int instructions;     // instructions emitted (after peephole)
  int peephole_removed; // instructions removed by the peephole optimizer
  int promoted_vars;    // locals kept in callee-saved registers
  int jump_tables;      // switch statements lowered to jump tables
} CodegenStats;

// Writes GNU assembler (AT&T syntax) for the program to `out`.
void codegen(Program *prog, FILE *out, bool optimize, CodegenStats *stats);
