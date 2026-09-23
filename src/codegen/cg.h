// cg.h - state and helpers shared by the code generator's source files.
//
//   emit.c      instruction buffer and output
//   expr.c      expressions (a stack machine over %rax/%xmm0)
//   stmt.c      statements, functions, prologue/epilogue
//   call.c      System V calling convention: calls, parameters, varargs
//   data.c      global variables and constants
//   regalloc.c  promotion of scalar locals to callee-saved registers (-o prod)
//   peephole.c  local instruction-level cleanups (-o prod)
#pragma once

#include "codegen/codegen.h"
#include "support/vec.h"

typedef struct {
  bool optimize;
  Obj *fn;          // function being generated
  int depth;        // 8-byte slots currently pushed (keeps calls 16-byte aligned)
  int label_seq;    // codegen-local labels (.L.cg.N)
  uint8_t used_regs; // callee-saved registers used by the current function
  StrVec lines;
  CodegenStats *stats;
} CodegenState;

extern CodegenState cg;

// emit.c
[[gnu::format(printf, 1, 2)]] void emit(const char *fmt, ...);       // instruction
[[gnu::format(printf, 1, 2)]] void emit_label(const char *fmt, ...); // "name:"
[[gnu::format(printf, 1, 2)]] void emit_raw(const char *fmt, ...);   // directive
int new_cg_label(void);
void flush_lines(FILE *out);

// Register names by size (1, 2, 4, 8 bytes).
const char *reg_ax(int size);
const char *reg_di(int size);
const char *arg_reg(int idx, int size);

// Stack machine primitives (expr.c).
void push(void);
void pop(const char *reg);
void pushf(void);
void popf(int xmm);

// expr.c
void gen_expr(Node *node);
void gen_addr(Node *node);
void gen_discard(Node *node);                    // evaluate for side effects only
void gen_branch(Node *cond, const char *label, bool jump_if); // jump to label if cond == jump_if
const char *label_name(int cg_label);                         // ".L.cg.N"
char *parser_label(int id);                                   // ".LN"
void load(Type *ty);
void store(Type *ty);
void copy_struct(int size); // (%rax) -> (%rdi), size bytes
void cast(Type *from, Type *to);
char *symbol_ref(Obj *var); // operand for a global: "name(%rip)"
bool is_local_symbol(Obj *var);

// call.c
typedef enum : uint8_t { CLASS_INTEGER, CLASS_SSE } EightbyteClass;
bool passed_in_memory(Type *ty);
int eightbyte_count(Type *ty);
EightbyteClass eightbyte_class(Type *ty, int idx);
void gen_funcall(Node *node);
void gen_params(Obj *fn);
void gen_va_start(Node *node);
void gen_va_arg(Node *node);
void gen_return_value(Type *ty); // move the value in %rax into the return registers
void load_eightbyte(int offset, int size, const char *reg64, int xmm, EightbyteClass cls);

// stmt.c
void gen_stmt(Node *node);
void gen_function(Obj *fn);

// data.c
void emit_data(Program *prog);

// regalloc.c
enum { NUM_CALLEE_SAVED = 5 };
void promote_locals(Obj *fn);
const char *callee_reg(int reg, int size);

// peephole.c
int peephole(StrVec *lines);
