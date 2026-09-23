// expr.c - expression code generation.
//
// Expressions are compiled as a stack machine: every expression leaves its
// value in %rax (integers, pointers, aggregate addresses) or %xmm0 (floating
// point); intermediate values are pushed on the machine stack.
//
// Invariant: a value of an integer type narrower than 8 bytes is valid in
// the low 32 bits of %rax, sign- or zero-extended from its own width; the
// upper 32 bits are unspecified. 32-bit instructions are used for such types.
//
// With -o prod some shapes get better code: operands that are constants or
// variables are used directly instead of going through the stack, and
// conditions compile to compare-and-branch.
#include <math.h>

#include "codegen/cg.h"
#include "parse/parse.h"

void push(void) {
  emit("push %%rax");
  cg.depth++;
}

void pop(const char *reg) {
  emit("pop %s", reg);
  cg.depth--;
}

void pushf(void) {
  emit("sub $8, %%rsp");
  emit("movsd %%xmm0, (%%rsp)");
  cg.depth++;
}

void popf(int xmm) {
  emit("movsd (%%rsp), %%xmm%d", xmm);
  emit("add $8, %%rsp");
  cg.depth--;
}

static bool fits_imm32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

bool is_local_symbol(Obj *var) { return var->is_static || var->is_definition; }

char *symbol_ref(Obj *var) { return format("%s(%%rip)", var->name); }

static bool is_int_or_ptr(const Type *ty) { return is_integer(ty) || is_pointer_like(ty); }

// ---------------------------------------------------------------------------
// Addresses, loads and stores
// ---------------------------------------------------------------------------

// Memory operand for a variable that needs no address computation, or nullptr.
static char *var_operand(Obj *var) {
  if (var->is_local)
    return var->reg >= 0 ? nullptr : format("%d(%%rbp)", var->offset);
  if (var->is_tls || !is_local_symbol(var))
    return nullptr;
  return symbol_ref(var);
}

void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->var;
    if (var->is_local) {
      if (var->reg >= 0)
        ICE("address of register-promoted variable '%s'", var->name);
      emit("lea %d(%%rbp), %%rax", var->offset);
    } else if (var->is_tls) {
      emit("mov %%fs:0, %%rax");
      emit("lea %s@tpoff(%%rax), %%rax", var->name);
    } else if (is_local_symbol(var)) {
      emit("lea %s(%%rip), %%rax", var->name);
    } else {
      emit("mov %s@GOTPCREL(%%rip), %%rax", var->name);
    }
    return;
  }
  case ND_DEREF:
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_discard(node->lhs);
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    if (node->member->offset)
      emit("add $%d, %%rax", node->member->offset);
    return;
  case ND_FUNCALL:
  case ND_ASSIGN:
  case ND_COND:
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) {
      gen_expr(node); // aggregate values are represented by their address
      return;
    }
    break;
  default:
    break;
  }
  ICE("expression is not an lvalue (node kind %d)", node->kind);
}

// Loads an integer of type ty from memory operand src into a register.
static void load_int(const Type *ty, const char *src, const char *r32, const char *r64) {
  bool zext = ty->is_unsigned || ty->kind == TY_BOOL;
  switch (ty->size) {
  case 1: emit("%s %s, %s", zext ? "movzbl" : "movsbl", src, r32); return;
  case 2: emit("%s %s, %s", zext ? "movzwl" : "movswl", src, r32); return;
  case 4: emit("mov %s, %s", src, r32); return;
  default: emit("mov %s, %s", src, r64); return;
  }
}

// Loads the value at (%rax) of type ty.
void load(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY: case TY_STRUCT: case TY_UNION: case TY_FUNC: case TY_VOID:
    return; // the address is the value
  case TY_FLOAT:
    emit("movss (%%rax), %%xmm0");
    return;
  case TY_DOUBLE: case TY_LDOUBLE:
    emit("movsd (%%rax), %%xmm0");
    return;
  default:
    load_int(ty, "(%rax)", "%eax", "%rax");
  }
}

void copy_struct(int size) {
  if (size > 64) {
    emit("mov %%rdi, %%r8");
    emit("mov %%rax, %%rsi");
    emit("mov $%d, %%ecx", size);
    emit("rep movsb");
    emit("mov %%r8, %%rdi");
    return;
  }
  int i = 0;
  for (; i + 8 <= size; i += 8) {
    emit("mov %d(%%rax), %%r8", i);
    emit("mov %%r8, %d(%%rdi)", i);
  }
  for (; i + 4 <= size; i += 4) {
    emit("mov %d(%%rax), %%r8d", i);
    emit("mov %%r8d, %d(%%rdi)", i);
  }
  for (; i < size; i++) {
    emit("movb %d(%%rax), %%r8b", i);
    emit("movb %%r8b, %d(%%rdi)", i);
  }
}

// Stores %rax/%xmm0 to the address on top of the stack.
void store(Type *ty) {
  pop("%rdi");
  switch (ty->kind) {
  case TY_STRUCT: case TY_UNION:
    copy_struct(ty->size);
    emit("mov %%rdi, %%rax");
    return;
  case TY_FLOAT:
    emit("movss %%xmm0, (%%rdi)");
    return;
  case TY_DOUBLE: case TY_LDOUBLE:
    emit("movsd %%xmm0, (%%rdi)");
    return;
  default:
    emit("mov %s, (%%rdi)", reg_ax(ty->size));
  }
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

typedef enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 } TypeId;

static TypeId type_id(const Type *ty) {
  switch (ty->kind) {
  case TY_BOOL: return U8;
  case TY_FLOAT: return F32;
  case TY_DOUBLE: case TY_LDOUBLE: return F64;
  case TY_PTR: case TY_NULLPTR: return U64;
  default: break;
  }
  int k = ty->size == 1 ? 0 : ty->size == 2 ? 1 : ty->size == 4 ? 2 : 3;
  return (TypeId)(k + (ty->is_unsigned ? 4 : 0));
}

static int id_size(TypeId id) { return id == F32 ? 4 : id == F64 ? 8 : 1 << (id & 3); }
static bool id_unsigned(TypeId id) { return id >= U8 && id <= U64; }

static void int_to_int(TypeId from, TypeId to) {
  int fs = id_size(from), ts = id_size(to);
  bool fu = id_unsigned(from), tu = id_unsigned(to);
  if (ts == 1) {
    if (fs != 1 || fu != tu)
      emit(tu ? "movzbl %%al, %%eax" : "movsbl %%al, %%eax");
  } else if (ts == 2) {
    bool fits = (fs == 1 && (fu || !tu)) || (fs == 2 && fu == tu);
    if (!fits)
      emit(tu ? "movzwl %%ax, %%eax" : "movswl %%ax, %%eax");
  } else if (ts == 8 && fs < 8) {
    if (fs == 4 && fu)
      emit("mov %%eax, %%eax");
    else
      emit("movslq %%eax, %%rax");
  }
}

static void u64_to_fp(bool to_float) {
  const char *cvt = to_float ? "cvtsi2ssq" : "cvtsi2sdq";
  const char *add = to_float ? "addss" : "addsd";
  int neg = new_cg_label(), end = new_cg_label();
  emit("test %%rax, %%rax");
  emit("js .L.cg.%d", neg);
  emit("pxor %%xmm0, %%xmm0");
  emit("%s %%rax, %%xmm0", cvt);
  emit("jmp .L.cg.%d", end);
  emit_label(".L.cg.%d", neg);
  // Halve (keeping the low bit for correct rounding), convert, double.
  emit("mov %%rax, %%rdi");
  emit("and $1, %%eax");
  emit("shr %%rdi");
  emit("or %%rax, %%rdi");
  emit("pxor %%xmm0, %%xmm0");
  emit("%s %%rdi, %%xmm0", cvt);
  emit("%s %%xmm0, %%xmm0", add);
  emit_label(".L.cg.%d", end);
}

static void fp_to_u64(bool from_float) {
  const char *cvt = from_float ? "cvttss2siq" : "cvttsd2siq";
  int big = new_cg_label(), end = new_cg_label();
  if (from_float) {
    emit("mov $0x5f000000, %%eax"); // 2^63 as float
    emit("movd %%eax, %%xmm1");
    emit("ucomiss %%xmm1, %%xmm0");
  } else {
    emit("movabs $0x43e0000000000000, %%rax"); // 2^63 as double
    emit("movq %%rax, %%xmm1");
    emit("ucomisd %%xmm1, %%xmm0");
  }
  emit("jae .L.cg.%d", big);
  emit("%s %%xmm0, %%rax", cvt);
  emit("jmp .L.cg.%d", end);
  emit_label(".L.cg.%d", big);
  emit(from_float ? "subss %%xmm1, %%xmm0" : "subsd %%xmm1, %%xmm0");
  emit("%s %%xmm0, %%rax", cvt);
  emit("btc $63, %%rax");
  emit_label(".L.cg.%d", end);
}

// Sets ZF according to whether the value of type ty is zero.
static void cmp_zero(const Type *ty) {
  if (ty->kind == TY_FLOAT) {
    emit("xorps %%xmm1, %%xmm1");
    emit("ucomiss %%xmm1, %%xmm0");
  } else if (is_flonum(ty)) {
    emit("xorpd %%xmm1, %%xmm1");
    emit("ucomisd %%xmm1, %%xmm0");
  } else if (ty->size <= 4 && ty->kind != TY_PTR) {
    emit("cmp $0, %%eax");
  } else {
    emit("cmp $0, %%rax");
  }
}

static void to_bool(const Type *from) {
  cmp_zero(from);
  emit("setne %%al");
  if (is_flonum(from)) { // NaN compares unordered but is true
    emit("setp %%dl");
    emit("or %%dl, %%al");
  }
  emit("movzbl %%al, %%eax");
}

void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return;
  if (to->kind == TY_BOOL) {
    if (from->kind != TY_BOOL)
      to_bool(from);
    return;
  }
  TypeId f = type_id(from), t = type_id(to);
  if (f == t)
    return;

  if (f <= U64 && t <= U64) {
    int_to_int(f, t);
    return;
  }

  if (t == F32 || t == F64) {
    bool to_float = t == F32;
    switch (f) {
    case F32: emit("cvtss2sd %%xmm0, %%xmm0"); return;
    case F64: emit("cvtsd2ss %%xmm0, %%xmm0"); return;
    case U64: u64_to_fp(to_float); return;
    case U32:
      emit("mov %%eax, %%eax");
      emit("%s %%rax, %%xmm0", to_float ? "cvtsi2ssq" : "cvtsi2sdq");
      return;
    case I64:
      emit("%s %%rax, %%xmm0", to_float ? "cvtsi2ssq" : "cvtsi2sdq");
      return;
    default: // narrower integers are valid as 32-bit signed values
      emit("%s %%eax, %%xmm0", to_float ? "cvtsi2ssl" : "cvtsi2sdl");
      return;
    }
  }

  // Floating point to integer.
  bool from_float = f == F32;
  const char *sfx = from_float ? "ss" : "sd";
  switch (t) {
  case U64:
    fp_to_u64(from_float);
    return;
  case I64: case U32:
    emit("cvtt%s2siq %%xmm0, %%rax", sfx);
    return;
  default:
    emit("cvtt%s2sil %%xmm0, %%eax", sfx);
    int_to_int(I32, t);
    return;
  }
}

// ---------------------------------------------------------------------------
// Operands used directly by -o prod
// ---------------------------------------------------------------------------

// Returns an operand that can be the source of an integer instruction of
// the given size without evaluating any code, or nullptr.
static char *simple_operand(Node *n, int size) {
  if (!cg.optimize)
    return nullptr;
  if (n->kind == ND_NUM && is_int_or_ptr(n->ty)) {
    if (size == 4 && n->val >= INT32_MIN && n->val <= UINT32_MAX)
      return format("$%d", (int32_t)n->val);
    if (size == 8 && fits_imm32(n->val))
      return format("$%lld", (long long)n->val);
    return nullptr;
  }
  if (n->kind == ND_VAR && is_int_or_ptr(n->ty) && n->ty->size == size && !n->ty->is_volatile) {
    if (n->var->is_local && n->var->reg >= 0)
      return (char *)callee_reg(n->var->reg, size);
    return var_operand(n->var);
  }
  return nullptr;
}

static void gen_num(Node *node) {
  if (node->ty->kind == TY_FLOAT) {
    float f = (float)node->fval;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    if (bits == 0 && cg.optimize) {
      emit("xorps %%xmm0, %%xmm0");
      return;
    }
    emit("mov $%u, %%eax", bits);
    emit("movd %%eax, %%xmm0");
    return;
  }
  if (is_flonum(node->ty)) {
    uint64_t bits;
    memcpy(&bits, &node->fval, 8);
    if (bits == 0 && cg.optimize) {
      emit("xorpd %%xmm0, %%xmm0");
      return;
    }
    emit("movabs $%llu, %%rax", (unsigned long long)bits);
    emit("movq %%rax, %%xmm0");
    return;
  }

  int64_t v = node->val;
  if (v == 0 && cg.optimize)
    emit("xor %%eax, %%eax");
  else if (node->ty->size <= 4)
    emit("mov $%d, %%eax", (int32_t)v);
  else if (fits_imm32(v))
    emit("mov $%lld, %%rax", (long long)v);
  else
    emit("movabs $%lld, %%rax", (long long)v);
}

// ---------------------------------------------------------------------------
// Bit-fields
// ---------------------------------------------------------------------------

static void extract_bitfield(Member *m) {
  emit("shl $%d, %%rax", 64 - m->bit_width - m->bit_offset);
  emit("%s $%d, %%rax", m->ty->is_unsigned || m->ty->kind == TY_BOOL ? "shr" : "sar", 64 - m->bit_width);
}

static void gen_bitfield_store(Node *node) {
  Member *m = node->lhs->member;
  gen_addr(node->lhs);
  push();
  gen_expr(node->rhs);
  emit("mov %%rax, %%r8"); // the assigned value

  uint64_t mask = m->bit_width == 64 ? ~0ULL : (1ULL << m->bit_width) - 1;
  emit("mov %%rax, %%rdi");
  emit("movabs $%llu, %%r9", (unsigned long long)mask);
  emit("and %%r9, %%rdi");
  emit("shl $%d, %%rdi", m->bit_offset);

  emit("mov (%%rsp), %%rax");
  Type unit = *m->ty;
  unit.is_unsigned = true;
  load_int(&unit, "(%rax)", "%eax", "%rax");
  emit("movabs $%llu, %%r9", (unsigned long long)~(mask << m->bit_offset));
  emit("and %%r9, %%rax");
  emit("or %%rdi, %%rax");
  store(m->ty);

  // The value of the assignment is the value stored in the bit-field.
  emit("mov %%r8, %%rax");
  emit("shl $%d, %%rax", 64 - m->bit_width);
  emit("%s $%d, %%rax", m->ty->is_unsigned || m->ty->kind == TY_BOOL ? "shr" : "sar", 64 - m->bit_width);
}

// ---------------------------------------------------------------------------
// Assignment
// ---------------------------------------------------------------------------

static void store_to_operand(Type *ty, const char *dst) {
  if (ty->kind == TY_FLOAT)
    emit("movss %%xmm0, %s", dst);
  else if (is_flonum(ty))
    emit("movsd %%xmm0, %s", dst);
  else
    emit("mov %s, %s", reg_ax(ty->size), dst);
}

static void gen_assign(Node *node) {
  Node *lhs = node->lhs;
  if (lhs->kind == ND_VAR && lhs->var->is_local && lhs->var->reg >= 0) {
    gen_expr(node->rhs);
    emit("mov %%rax, %s", callee_reg(lhs->var->reg, 8));
    return;
  }
  if (lhs->kind == ND_MEMBER && lhs->member->is_bitfield) {
    gen_bitfield_store(node);
    return;
  }
  if (cg.optimize && lhs->kind == ND_VAR && is_scalar(lhs->ty)) {
    char *dst = var_operand(lhs->var);
    if (dst) {
      gen_expr(node->rhs);
      store_to_operand(lhs->ty, dst);
      return;
    }
  }
  gen_addr(lhs);
  push();
  gen_expr(node->rhs);
  store(node->ty);
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

const char *label_name(int id) { return format(".L.cg.%d", id); }

char *parser_label(int id) { return format(".L%d", id); }

static void gen_float_binary(Node *node) {
  gen_expr(node->rhs);
  pushf();
  gen_expr(node->lhs);
  popf(1);

  const char *sz = node->lhs->ty->kind == TY_FLOAT ? "ss" : "sd";
  switch (node->kind) {
  case ND_ADD: emit("add%s %%xmm1, %%xmm0", sz); return;
  case ND_SUB: emit("sub%s %%xmm1, %%xmm0", sz); return;
  case ND_MUL: emit("mul%s %%xmm1, %%xmm0", sz); return;
  case ND_DIV: emit("div%s %%xmm1, %%xmm0", sz); return;
  case ND_EQ:
    emit("ucomi%s %%xmm1, %%xmm0", sz);
    emit("sete %%al");
    emit("setnp %%dl");
    emit("and %%dl, %%al");
    break;
  case ND_NE:
    emit("ucomi%s %%xmm1, %%xmm0", sz);
    emit("setne %%al");
    emit("setp %%dl");
    emit("or %%dl, %%al");
    break;
  case ND_LT:
    emit("ucomi%s %%xmm0, %%xmm1", sz);
    emit("seta %%al");
    break;
  case ND_LE:
    emit("ucomi%s %%xmm0, %%xmm1", sz);
    emit("setae %%al");
    break;
  default:
    ICE("invalid floating-point operator");
  }
  emit("movzbl %%al, %%eax");
}

// Evaluates lhs into %rax and returns the operand holding rhs.
static const char *gen_operands(Node *node, int size) {
  const char *src = simple_operand(node->rhs, size);
  if (src) {
    gen_expr(node->lhs);
    return src;
  }
  gen_expr(node->rhs);
  push();
  gen_expr(node->lhs);
  pop("%rdi");
  return reg_di(size);
}

static const char *condition_code(NodeKind kind, bool is_unsigned) {
  switch (kind) {
  case ND_EQ: return "e";
  case ND_NE: return "ne";
  case ND_LT: return is_unsigned ? "b" : "l";
  case ND_LE: return is_unsigned ? "be" : "le";
  default: ICE_UNREACHABLE();
  }
}

static const char *negated_code(NodeKind kind, bool is_unsigned) {
  switch (kind) {
  case ND_EQ: return "ne";
  case ND_NE: return "e";
  case ND_LT: return is_unsigned ? "ae" : "ge";
  case ND_LE: return is_unsigned ? "a" : "g";
  default: ICE_UNREACHABLE();
  }
}

static bool is_int_compare(Node *node) {
  return (node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) &&
         is_int_or_ptr(node->lhs->ty);
}

// Emits "cmp" for an integer comparison node.
static void gen_compare(Node *node) {
  int size = node->lhs->ty->size < 8 && node->lhs->ty->kind != TY_PTR ? 4 : 8;
  // Compare a register variable directly: cmp $5, %ebx.
  if (cg.optimize && node->lhs->kind == ND_VAR && node->lhs->var->is_local && node->lhs->var->reg >= 0 &&
      node->lhs->ty->size == size) {
    const char *src = simple_operand(node->rhs, size);
    if (src) {
      emit("cmp %s, %s", src, callee_reg(node->lhs->var->reg, size));
      return;
    }
  }
  const char *src = gen_operands(node, size);
  emit("cmp %s, %s", src, reg_ax(size));
}

static void gen_shift(Node *node) {
  int size = node->ty->size < 8 ? 4 : 8;
  const char *ax = reg_ax(size);
  const char *op = node->kind == ND_SHL ? "shl" : node->ty->is_unsigned ? "shr" : "sar";
  if (cg.optimize && node->rhs->kind == ND_NUM) {
    gen_expr(node->lhs);
    emit("%s $%lld, %s", op, (long long)(node->rhs->val & (size * 8 - 1)), ax);
    return;
  }
  gen_expr(node->rhs);
  push();
  gen_expr(node->lhs);
  pop("%rcx");
  emit("%s %%cl, %s", op, ax);
}

static void gen_divmod(Node *node, const char *src, int size) {
  if (src[0] != '%') { // idiv/div take no immediates; use a register
    emit("mov %s, %s", src, reg_di(size));
    src = reg_di(size);
  }
  if (node->ty->is_unsigned) {
    emit("xor %%edx, %%edx");
    emit("div %s", src);
  } else {
    emit(size == 8 ? "cqo" : "cdq");
    emit("idiv %s", src);
  }
  if (node->kind == ND_MOD)
    emit(size == 8 ? "mov %%rdx, %%rax" : "mov %%edx, %%eax");
}

static void gen_binary(Node *node) {
  if (is_flonum(node->lhs->ty)) {
    gen_float_binary(node);
    return;
  }
  if (node->kind == ND_SHL || node->kind == ND_SHR) {
    gen_shift(node);
    return;
  }
  if (is_int_compare(node)) {
    gen_compare(node);
    bool u = node->lhs->ty->is_unsigned || is_pointer_like(node->lhs->ty);
    emit("set%s %%al", condition_code(node->kind, u));
    emit("movzbl %%al, %%eax");
    return;
  }

  int size = node->ty->size < 8 && node->ty->kind != TY_PTR ? 4 : 8;
  const char *src = gen_operands(node, size);
  const char *ax = reg_ax(size);
  switch (node->kind) {
  case ND_ADD: emit("add %s, %s", src, ax); return;
  case ND_SUB: emit("sub %s, %s", src, ax); return;
  case ND_MUL: emit("imul %s, %s", src, ax); return;
  case ND_DIV: case ND_MOD: gen_divmod(node, src, size); return;
  case ND_BITAND: emit("and %s, %s", src, ax); return;
  case ND_BITOR: emit("or %s, %s", src, ax); return;
  case ND_BITXOR: emit("xor %s, %s", src, ax); return;
  default: ICE("unknown binary operator %d", node->kind);
  }
}

void gen_branch(Node *cond, const char *label, bool jump_if) {
  if (cg.optimize) {
    switch (cond->kind) {
    case ND_LOGAND:
      if (!jump_if) {
        gen_branch(cond->lhs, label, false);
        gen_branch(cond->rhs, label, false);
      } else {
        const char *skip = label_name(new_cg_label());
        gen_branch(cond->lhs, skip, false);
        gen_branch(cond->rhs, label, true);
        emit_label("%s", skip);
      }
      return;
    case ND_LOGOR:
      if (jump_if) {
        gen_branch(cond->lhs, label, true);
        gen_branch(cond->rhs, label, true);
      } else {
        const char *skip = label_name(new_cg_label());
        gen_branch(cond->lhs, skip, true);
        gen_branch(cond->rhs, label, false);
        emit_label("%s", skip);
      }
      return;
    case ND_NOT:
      gen_branch(cond->lhs, label, !jump_if);
      return;
    case ND_NUM:
      if (is_integer(cond->ty)) {
        if ((cond->val != 0) == jump_if)
          emit("jmp %s", label);
        return;
      }
      break;
    default:
      if (is_int_compare(cond)) {
        gen_compare(cond);
        bool u = cond->lhs->ty->is_unsigned || is_pointer_like(cond->lhs->ty);
        emit("j%s %s", jump_if ? condition_code(cond->kind, u) : negated_code(cond->kind, u), label);
        return;
      }
      break;
    }
  }

  gen_expr(cond);
  if (is_flonum(cond->ty)) {
    to_bool(cond->ty);
    emit("cmp $0, %%eax");
  } else {
    cmp_zero(cond->ty);
  }
  emit("%s %s", jump_if ? "jne" : "je", label);
}

static void gen_logical(Node *node) {
  const char *other = label_name(new_cg_label()), *end = label_name(new_cg_label());
  bool is_and = node->kind == ND_LOGAND;
  gen_branch(node->lhs, other, !is_and);
  gen_branch(node->rhs, other, !is_and);
  emit("mov $%d, %%eax", is_and ? 1 : 0);
  emit("jmp %s", end);
  emit_label("%s", other);
  emit("mov $%d, %%eax", is_and ? 0 : 1);
  emit_label("%s", end);
}

static void gen_overflow(Node *node) {
  Type *rt = node->cond->ty->base;
  gen_expr(node->cond);
  push();
  gen_expr(node->rhs);
  push();
  gen_expr(node->lhs);
  pop("%rdi");

  bool u = rt->is_unsigned;
  switch (node->val) {
  case ND_ADD:
    emit("add %%rdi, %%rax");
    emit(u ? "setc %%cl" : "seto %%cl");
    break;
  case ND_SUB:
    emit("sub %%rdi, %%rax");
    emit(u ? "setc %%cl" : "seto %%cl");
    break;
  default:
    if (u) {
      emit("mul %%rdi");
      emit("seto %%cl");
    } else {
      emit("imul %%rdi, %%rax");
      emit("seto %%cl");
    }
    break;
  }
  // The exact result must also fit the (possibly narrower) result type.
  if (rt->size < 8) {
    if (u) {
      emit("mov %%rax, %%rdx");
      emit("shr $%d, %%rdx", rt->size * 8);
      emit("test %%rdx, %%rdx");
    } else {
      static const char *sext[] = {[1] = "movsbq %al, %rdx", [2] = "movswq %ax, %rdx",
                                   [4] = "movslq %eax, %rdx"};
      emit("%s", sext[rt->size]);
      emit("cmp %%rax, %%rdx");
    }
    emit("setne %%dl");
    emit("or %%dl, %%cl");
  }
  pop("%rdi");
  emit("mov %s, (%%rdi)", reg_ax(rt->size));
  emit("movzbl %%cl, %%eax");
}

static void gen_memzero(Obj *var) {
  int size = var->ty->size;
  if (cg.optimize && size <= 64) {
    int i = 0;
    for (; i + 8 <= size; i += 8)
      emit("movq $0, %d(%%rbp)", var->offset + i);
    for (; i + 4 <= size; i += 4)
      emit("movl $0, %d(%%rbp)", var->offset + i);
    for (; i < size; i++)
      emit("movb $0, %d(%%rbp)", var->offset + i);
    return;
  }
  emit("lea %d(%%rbp), %%rdi", var->offset);
  emit("mov $%d, %%ecx", size);
  emit("xor %%eax, %%eax");
  emit("rep stosb");
}

void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NOP:
    return;
  case ND_NUM:
    gen_num(node);
    return;

  case ND_VAR: {
    Obj *var = node->var;
    if (var->is_local && var->reg >= 0) {
      emit("mov %s, %%rax", callee_reg(var->reg, 8));
      return;
    }
    if (cg.optimize && is_scalar(node->ty)) {
      char *src = var_operand(var);
      if (src) {
        if (node->ty->kind == TY_FLOAT)
          emit("movss %s, %%xmm0", src);
        else if (is_flonum(node->ty))
          emit("movsd %s, %%xmm0", src);
        else
          load_int(node->ty, src, "%eax", "%rax");
        return;
      }
    }
    gen_addr(node);
    load(node->ty);
    return;
  }

  case ND_MEMBER:
    gen_addr(node);
    load(node->ty);
    if (node->member->is_bitfield)
      extract_bitfield(node->member);
    return;

  case ND_DEREF:
    gen_expr(node->lhs);
    load(node->ty);
    return;

  case ND_ADDR:
    gen_addr(node->lhs);
    return;

  case ND_ASSIGN:
    gen_assign(node);
    return;

  case ND_COMMA:
    // Walk right-leaning chains iteratively (long initializer lists).
    while (node->kind == ND_COMMA) {
      gen_discard(node->lhs);
      node = node->rhs;
    }
    gen_expr(node);
    return;

  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;

  case ND_MEMZERO:
    gen_memzero(node->var);
    return;

  case ND_COND: {
    const char *els = label_name(new_cg_label()), *end = label_name(new_cg_label());
    gen_branch(node->cond, els, false);
    gen_expr(node->then);
    emit("jmp %s", end);
    emit_label("%s", els);
    gen_expr(node->els);
    emit_label("%s", end);
    return;
  }

  case ND_NOT:
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    emit("sete %%al");
    if (is_flonum(node->lhs->ty)) {
      emit("setnp %%dl");
      emit("and %%dl, %%al");
    }
    emit("movzbl %%al, %%eax");
    return;

  case ND_NEG:
    gen_expr(node->lhs);
    if (node->ty->kind == TY_FLOAT) {
      emit("mov $0x80000000, %%eax");
      emit("movd %%eax, %%xmm1");
      emit("xorps %%xmm1, %%xmm0");
    } else if (is_flonum(node->ty)) {
      emit("movabs $0x8000000000000000, %%rax");
      emit("movq %%rax, %%xmm1");
      emit("xorpd %%xmm1, %%xmm0");
    } else {
      emit("neg %s", reg_ax(node->ty->size < 8 ? 4 : 8));
    }
    return;

  case ND_BITNOT:
    gen_expr(node->lhs);
    emit("not %s", reg_ax(node->ty->size < 8 ? 4 : 8));
    return;

  case ND_LOGAND:
  case ND_LOGOR:
    gen_logical(node);
    return;

  case ND_FUNCALL:
    gen_funcall(node);
    return;

  case ND_VA_START:
    gen_va_start(node);
    return;

  case ND_VA_ARG:
    gen_va_arg(node);
    return;

  case ND_OVERFLOW:
    gen_overflow(node);
    return;

  case ND_UNREACHABLE:
    emit("ud2");
    return;

  default:
    gen_binary(node);
    return;
  }
}

// ---------------------------------------------------------------------------
// Expressions evaluated only for their side effects
// ---------------------------------------------------------------------------

static bool same_var(Node *a, Node *b) { return a->kind == ND_VAR && b->kind == ND_VAR && a->var == b->var; }

static Node *strip_same_repr_casts(Node *n) {
  while (n->kind == ND_CAST && n->is_implicit && is_int_or_ptr(n->ty) && is_int_or_ptr(n->lhs->ty) &&
         n->ty->size == n->lhs->ty->size && n->ty->kind != TY_BOOL)
    n = n->lhs;
  return n;
}

// "x = x + c" / "x = x - c" on an int, long or pointer variable in place.
static bool gen_update_in_place(Node *node) {
  if (node->kind != ND_ASSIGN || node->lhs->kind != ND_VAR)
    return false;
  Node *lhs = node->lhs;
  Type *ty = lhs->ty;
  if (!is_int_or_ptr(ty) || ty->size < 4 || ty->is_volatile)
    return false;
  Node *rhs = strip_same_repr_casts(node->rhs);
  if ((rhs->kind != ND_ADD && rhs->kind != ND_SUB) || !same_var(rhs->lhs, lhs) || rhs->ty->size != ty->size)
    return false;
  const char *src = simple_operand(rhs->rhs, ty->size);
  if (!src)
    return false;

  const char *op = rhs->kind == ND_ADD ? "add" : "sub";
  Obj *var = lhs->var;
  if (var->is_local && var->reg >= 0) {
    emit("%s %s, %s", op, src, callee_reg(var->reg, ty->size));
    return true;
  }
  char *dst = var_operand(var);
  if (!dst || src[0] != '$')
    return false; // memory-to-memory is not encodable
  emit("%s%s %s, %s", op, ty->size == 8 ? "q" : "l", src, dst);
  return true;
}

void gen_discard(Node *node) {
  if (cg.optimize) {
    // The value of a post-increment ("(x += 1) - 1") is not needed here.
    for (;;) {
      Node *n = strip_same_repr_casts(node);
      if ((n->kind == ND_ADD || n->kind == ND_SUB) && n->rhs->kind == ND_NUM && has_side_effects(n->lhs)) {
        node = n->lhs;
        continue;
      }
      if (n->kind == ND_CAST && n->ty->kind == TY_VOID) {
        node = n->lhs;
        continue;
      }
      break;
    }
    if (node->kind == ND_COMMA) {
      gen_discard(node->lhs);
      gen_discard(node->rhs);
      return;
    }
    if (gen_update_in_place(node))
      return;
  }
  gen_expr(node);
}
