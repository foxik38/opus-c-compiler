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

// A memory operand: disp(base, index, scale) or sym+disp(%rip).
typedef struct {
  const char *base;
  const char *index;
  int scale;
  int64_t disp;
  const char *sym;
} Mem;

static void lvalue_mem(Node *lv, Mem *m);
static void pointer_mem(Node *p, Mem *m);
static void lea_mem(const Mem *m);
static bool gen_cast_of_var(Node *node);

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
    if (cg.optimize) {
      Mem m;
      pointer_mem(node->lhs, &m);
      lea_mem(&m);
      return;
    }
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_discard(node->lhs);
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    if (cg.optimize) {
      Mem m;
      lvalue_mem(node, &m);
      lea_mem(&m);
      return;
    }
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
//
// Constants are used directly even at -o none: nothing may be pushed across
// a call that can return twice (setjmp(env) == 0).
static char *simple_operand(Node *n, int size) {
  if (!cg.optimize && n->kind != ND_NUM)
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
// Temporaries and addressing modes (-o prod)
// ---------------------------------------------------------------------------

// Up to four intermediate values live in scratch registers instead of on the
// stack. A temporary may only be live while a side-effect-free expression is
// evaluated: such code never calls functions, copies structs or stores
// bit-fields, the only other users of these registers.
enum { MAX_TEMPS = 4 };

static const char *const int_temps[MAX_TEMPS][4] = {
    {"%r11b", "%r11w", "%r11d", "%r11"},
    {"%r10b", "%r10w", "%r10d", "%r10"},
    {"%r9b", "%r9w", "%r9d", "%r9"},
    {"%r8b", "%r8w", "%r8d", "%r8"},
};
static const char *const fp_temps[MAX_TEMPS] = {"%xmm8", "%xmm9", "%xmm10", "%xmm11"};

static const char *int_temp(int t, int size) {
  return int_temps[t][size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3];
}

static bool can_use_temp(Node *evaluated_later) {
  return cg.optimize && cg.temp_depth < MAX_TEMPS && !has_side_effects(evaluated_later);
}

// Moves %rax (or %xmm0) into a new temporary and returns its index.
static int save_temp(bool fp) {
  int t = cg.temp_depth++;
  if (fp)
    emit("movapd %%xmm0, %s", fp_temps[t]);
  else
    emit("mov %%rax, %s", int_temp(t, 8));
  return t;
}

static void release_temp(void) { cg.temp_depth--; }

static char *mem_str(const Mem *m) {
  if (m->sym)
    return m->disp ? format("%s%+lld(%%rip)", m->sym, (long long)m->disp) : format("%s(%%rip)", m->sym);
  const char *disp = m->disp ? format("%lld", (long long)m->disp) : "";
  if (m->index)
    return format("%s(%s,%s,%d)", disp, m->base, m->index, m->scale);
  return format("%s(%s)", disp, m->base);
}

// Leaves the value of a in %rax and of b in %rdi (both 8 bytes wide).
static void gen_pair(Node *a, Node *b) {
  const char *src = simple_operand(b, 8);
  if (src) {
    gen_expr(a);
    emit("mov %s, %%rdi", src);
    return;
  }
  if (can_use_temp(a)) {
    gen_expr(b);
    int t = save_temp(false);
    gen_expr(a);
    emit("mov %s, %%rdi", int_temp(t, 8));
    release_temp();
    return;
  }
  gen_expr(b);
  push();
  gen_expr(a);
  pop("%rdi");
}

// Computes as little as needed of the address of lvalue `lv` and describes
// the rest as an addressing mode.
static void lvalue_mem(Node *lv, Mem *m) {
  *m = (Mem){};
  switch (lv->kind) {
  case ND_VAR: {
    Obj *var = lv->var;
    if (var->is_local && var->reg < 0) {
      m->base = "%rbp";
      m->disp = var->offset;
      return;
    }
    if (!var->is_local && !var->is_tls && is_local_symbol(var)) {
      m->sym = var->name;
      return;
    }
    break;
  }
  case ND_MEMBER:
    lvalue_mem(lv->lhs, m);
    m->disp += lv->member->offset;
    return;
  case ND_DEREF:
    pointer_mem(lv->lhs, m);
    return;
  default:
    break;
  }
  gen_addr(lv);
  m->base = "%rax";
}

// Describes the object a pointer expression points to.
static void pointer_mem(Node *p, Mem *m) {
  *m = (Mem){};
  if (p->kind == ND_ADDR) {
    lvalue_mem(p->lhs, m);
    return;
  }
  if (p->kind == ND_VAR && p->var->is_local && p->var->reg >= 0) {
    m->base = callee_reg(p->var->reg, 8);
    return;
  }
  if (p->kind == ND_ADD && p->ty->kind == TY_PTR) {
    Node *base = p->lhs, *off = p->rhs;
    if (off->kind == ND_NUM && fits_imm32(off->val)) {
      pointer_mem(base, m);
      m->disp += off->val;
      return;
    }

    // base + index * scale
    int scale = 1;
    Node *idx = off;
    if (off->kind == ND_SHL && off->rhs->kind == ND_NUM && off->rhs->val >= 0 && off->rhs->val <= 3) {
      scale = 1 << off->rhs->val;
      idx = off->lhs;
    } else if (off->kind == ND_MUL && off->rhs->kind == ND_NUM &&
               (off->rhs->val == 2 || off->rhs->val == 4 || off->rhs->val == 8)) {
      scale = (int)off->rhs->val;
      idx = off->lhs;
    }
    m->scale = scale;
    // An index held in a register variable is used as is.
    const char *reg_index = idx->kind == ND_VAR && is_gp_reg_var(idx->var) && idx->ty->size == 8
                                ? callee_reg(idx->var->reg, 8)
                                : nullptr;
    if (base->kind == ND_ADDR && base->lhs->kind == ND_VAR && base->lhs->var->is_local &&
        base->lhs->var->reg < 0) {
      if (!reg_index)
        gen_expr(idx);
      m->base = "%rbp";
      m->index = reg_index ? reg_index : "%rax";
      m->disp = base->lhs->var->offset;
      return;
    }
    if (base->kind == ND_VAR && is_gp_reg_var(base->var)) {
      if (!reg_index)
        gen_expr(idx);
      m->base = callee_reg(base->var->reg, 8);
      m->index = reg_index ? reg_index : "%rax";
      return;
    }
    if (reg_index || can_use_temp(base)) {
      // Index first, kept in a temporary while the base address is formed.
      int t = -1;
      if (!reg_index) {
        gen_expr(idx);
        t = save_temp(false);
      }
      if (base->kind == ND_ADDR) {
        lvalue_mem(base->lhs, m);
      } else {
        gen_expr(base);
        *m = (Mem){.base = "%rax"};
      }
      if (t >= 0)
        release_temp(); // read by the instruction using this operand
      if (m->index || m->sym) {
        emit("lea %s, %%rax", mem_str(m));
        *m = (Mem){.base = "%rax"};
      }
      m->index = reg_index ? reg_index : int_temp(t, 8);
      m->scale = scale;
      return;
    }
    gen_pair(base, idx);
    m->base = "%rax";
    m->index = "%rdi";
    return;
  }
  gen_expr(p);
  m->base = "%rax";
}

static bool is_plain_rax(const Mem *m) { return !m->sym && !m->index && !m->disp && !strcmp(m->base, "%rax"); }

static void lea_mem(const Mem *m) {
  if (!is_plain_rax(m))
    emit("lea %s, %%rax", mem_str(m));
}

static void load_mem(Type *ty, const Mem *m) {
  switch (ty->kind) {
  case TY_ARRAY: case TY_STRUCT: case TY_UNION: case TY_FUNC: case TY_VOID:
    lea_mem(m);
    return;
  case TY_FLOAT:
    emit("movss %s, %%xmm0", mem_str(m));
    return;
  case TY_DOUBLE: case TY_LDOUBLE:
    emit("movsd %s, %%xmm0", mem_str(m));
    return;
  default:
    load_int(ty, mem_str(m), "%eax", "%rax");
  }
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

static const char *size_suffix(int size) {
  return size == 1 ? "b" : size == 2 ? "w" : size == 4 ? "l" : "q";
}

// Value first, then the address: "a[i] = x" needs no stack traffic.
// `need_value` is false in statement context, where %rax is dead afterwards.
static bool gen_assign_via_temp(Node *node, bool need_value) {
  Node *lhs = node->lhs;
  if (!cg.optimize || !is_scalar(lhs->ty) || has_side_effects(lhs) || cg.temp_depth >= MAX_TEMPS)
    return false;

  // Constant stores need no register at all: movl $1, (%rbx,%rax,4).
  Node *rhs = node->rhs;
  if (!need_value && rhs->kind == ND_NUM && is_int_or_ptr(rhs->ty) &&
      (fits_imm32(rhs->val) || (lhs->ty->size <= 4 && rhs->val <= UINT32_MAX))) {
    Mem m;
    lvalue_mem(lhs, &m);
    int size = lhs->ty->size;
    int64_t v = size == 1 ? (int8_t)rhs->val : size == 2 ? (int16_t)rhs->val : size == 4 ? (int32_t)rhs->val
                                                                                          : rhs->val;
    emit("mov%s $%lld, %s", size_suffix(size), (long long)v, mem_str(&m));
    return true;
  }

  bool fp = is_flonum(lhs->ty);
  gen_expr(rhs);
  int t = save_temp(fp);
  Mem m;
  lvalue_mem(lhs, &m);
  if (fp) {
    emit("%s %s, %s", lhs->ty->kind == TY_FLOAT ? "movss" : "movsd", fp_temps[t], mem_str(&m));
    if (need_value)
      emit("movapd %s, %%xmm0", fp_temps[t]);
  } else {
    emit("mov %s, %s", int_temp(t, lhs->ty->size), mem_str(&m));
    if (need_value)
      emit("mov %s, %%rax", int_temp(t, 8));
  }
  release_temp();
  return true;
}

static void gen_assign(Node *node, bool need_value) {
  Node *lhs = node->lhs;
  if (lhs->kind == ND_VAR && is_xmm_var(lhs->var)) {
    gen_expr(node->rhs);
    emit("movapd %%xmm0, %s", xmm_var_reg(lhs->var));
    return;
  }
  if (lhs->kind == ND_VAR && is_gp_reg_var(lhs->var)) {
    gen_expr(node->rhs);
    emit("mov %%rax, %s", callee_reg(lhs->var->reg, 8));
    return;
  }
  if (lhs->kind == ND_MEMBER && lhs->member->is_bitfield) {
    gen_bitfield_store(node);
    return;
  }
  // Stores to plain variables evaluate the value first and need no pushed
  // address. Besides being shorter, this keeps "x = setjmp(env)" working.
  if (lhs->kind == ND_VAR && is_scalar(lhs->ty)) {
    char *dst = var_operand(lhs->var);
    if (dst) {
      gen_expr(node->rhs);
      store_to_operand(lhs->ty, dst);
      return;
    }
  }
  if (gen_assign_via_temp(node, need_value))
    return;
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

// Evaluates a floating-point operand pair: lhs into %xmm0, rhs into %xmm1.
// Evaluates lhs into %xmm0 and returns an operand holding rhs: %xmm1, a
// register variable, a temporary or a memory operand.
static const char *gen_float_operands(Node *node) {
  Node *lhs = node->lhs, *rhs = node->rhs;
  bool is_float = lhs->ty->kind == TY_FLOAT;
  if (cg.optimize) {
    if (rhs->kind == ND_VAR && is_xmm_var(rhs->var)) {
      gen_expr(lhs);
      return xmm_var_reg(rhs->var);
    }
    char *src = rhs->kind == ND_VAR ? var_operand(rhs->var) : nullptr;
    if (src) {
      gen_expr(lhs);
      return src;
    }
    if (rhs->kind == ND_NUM) {
      gen_expr(lhs);
      if (is_float) {
        float f = (float)rhs->fval;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        emit("mov $%u, %%eax", bits);
        emit("movd %%eax, %%xmm1");
      } else {
        uint64_t bits;
        memcpy(&bits, &rhs->fval, 8);
        emit("movabs $%llu, %%rax", (unsigned long long)bits);
        emit("movq %%rax, %%xmm1");
      }
      return "%xmm1";
    }
    if (can_use_temp(lhs)) {
      gen_expr(rhs);
      int t = save_temp(true);
      gen_expr(lhs);
      release_temp(); // read by the very next instruction
      return fp_temps[t];
    }
  }
  gen_expr(rhs);
  pushf();
  gen_expr(lhs);
  popf(1);
  return "%xmm1";
}

static bool is_float_compare(Node *node) {
  return (node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) &&
         is_flonum(node->lhs->ty);
}

// Emits ucomiss/ucomisd for a floating-point comparison. For < and <= the
// operands are swapped so that "above" conditions reject NaN (unordered).
static void gen_float_compare(Node *node) {
  const char *sz = node->lhs->ty->kind == TY_FLOAT ? "ss" : "sd";
  const char *r = gen_float_operands(node);
  if (node->kind == ND_EQ || node->kind == ND_NE) {
    emit("ucomi%s %s, %%xmm0", sz, r);
    return;
  }
  if (r[0] != '%') { // the second operand of ucomis* must be a register
    emit("mov%s %s, %%xmm1", sz, r);
    r = "%xmm1";
  }
  emit("ucomi%s %%xmm0, %s", sz, r);
}

static void gen_float_binary(Node *node) {
  const char *sz = node->lhs->ty->kind == TY_FLOAT ? "ss" : "sd";
  if (is_float_compare(node)) {
    gen_float_compare(node);
    switch (node->kind) {
    case ND_EQ:
      emit("sete %%al");
      emit("setnp %%dl");
      emit("and %%dl, %%al");
      break;
    case ND_NE:
      emit("setne %%al");
      emit("setp %%dl");
      emit("or %%dl, %%al");
      break;
    case ND_LT:
      emit("seta %%al");
      break;
    default:
      emit("setae %%al");
      break;
    }
    emit("movzbl %%al, %%eax");
    return;
  }

  const char *r = gen_float_operands(node);
  switch (node->kind) {
  case ND_ADD: emit("add%s %s, %%xmm0", sz, r); return;
  case ND_SUB: emit("sub%s %s, %%xmm0", sz, r); return;
  case ND_MUL: emit("mul%s %s, %%xmm0", sz, r); return;
  case ND_DIV: emit("div%s %s, %%xmm0", sz, r); return;
  default: ICE("invalid floating-point operator");
  }
}

// Conditional jump on a floating-point comparison, NaN-correct.
static void gen_float_branch(Node *cond, const char *label, bool jump_if) {
  gen_float_compare(cond);
  switch (cond->kind) {
  case ND_LT:
    emit("%s %s", jump_if ? "ja" : "jbe", label);
    return;
  case ND_LE:
    emit("%s %s", jump_if ? "jae" : "jb", label);
    return;
  default: {
    // Equal means ZF=1 and PF=0 (PF=1: unordered).
    bool jump_on_equal = (cond->kind == ND_EQ) == jump_if;
    if (jump_on_equal) {
      const char *skip = label_name(new_cg_label());
      emit("jp %s", skip);
      emit("je %s", label);
      emit_label("%s", skip);
    } else {
      emit("jne %s", label);
      emit("jp %s", label);
    }
    return;
  }
  }
}

// Evaluates lhs into %rax and returns the operand holding rhs.
static const char *gen_operands(Node *node, int size) {
  const char *src = simple_operand(node->rhs, size);
  if (src) {
    gen_expr(node->lhs);
    return src;
  }
  if (cg.optimize) {
    // A simple left operand is loaded after the right one is computed.
    const char *l = simple_operand(node->lhs, size);
    if (l) {
      gen_expr(node->rhs);
      emit("mov %%rax, %%rdi");
      emit("mov %s, %s", l, reg_ax(size));
      return reg_di(size);
    }
    if (can_use_temp(node->lhs)) {
      gen_expr(node->rhs);
      int t = save_temp(false);
      gen_expr(node->lhs);
      release_temp(); // the register is read by the very next instruction
      return int_temp(t, size);
    }
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
  if (can_use_temp(node->lhs)) {
    gen_expr(node->rhs);
    int t = save_temp(false);
    gen_expr(node->lhs);
    emit("mov %s, %%rcx", int_temp(t, 8));
    release_temp();
  } else {
    gen_expr(node->rhs);
    push();
    gen_expr(node->lhs);
    pop("%rcx");
  }
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
      if (is_float_compare(cond)) {
        gen_float_branch(cond, label, jump_if);
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
    if (is_xmm_var(var)) {
      emit("movapd %s, %%xmm0", xmm_var_reg(var));
      return;
    }
    if (is_gp_reg_var(var)) {
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
    if (cg.optimize) {
      Mem m;
      lvalue_mem(node, &m);
      load_mem(node->ty, &m);
    } else {
      gen_addr(node);
      load(node->ty);
    }
    if (node->member->is_bitfield)
      extract_bitfield(node->member);
    return;

  case ND_DEREF:
    if (cg.optimize) {
      Mem m;
      pointer_mem(node->lhs, &m);
      load_mem(node->ty, &m);
      return;
    }
    gen_expr(node->lhs);
    load(node->ty);
    return;

  case ND_ADDR:
    gen_addr(node->lhs);
    return;

  case ND_ASSIGN:
    gen_assign(node, true);
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
    if (cg.optimize && gen_cast_of_var(node))
      return;
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
  const char *op = rhs->kind == ND_ADD ? "add" : "sub";
  Obj *var = lhs->var;
  const char *src = simple_operand(rhs->rhs, ty->size);
  if (!src) {
    if (!is_gp_reg_var(var))
      return false;
    gen_expr(rhs->rhs); // "sum += a[i]" with sum in a register
    emit("%s %s, %s", op, reg_ax(ty->size), callee_reg(var->reg, ty->size));
    return true;
  }

  if (is_gp_reg_var(var)) {
    emit("%s %s, %s", op, src, callee_reg(var->reg, ty->size));
    return true;
  }
  char *dst = var_operand(var);
  if (!dst || src[0] != '$')
    return false; // memory-to-memory is not encodable
  emit("%s%s %s, %s", op, ty->size == 8 ? "q" : "l", src, dst);
  return true;
}

// "r = 0", "r = s", "r = mem" for a register variable, as a statement.
static bool gen_assign_simple_to_reg(Node *node) {
  if (node->kind != ND_ASSIGN || node->lhs->kind != ND_VAR)
    return false;
  if (is_xmm_var(node->lhs->var) && node->rhs->kind == ND_VAR && is_xmm_var(node->rhs->var)) {
    emit("movapd %s, %s", xmm_var_reg(node->rhs->var), xmm_var_reg(node->lhs->var));
    return true;
  }
  if (!is_gp_reg_var(node->lhs->var))
    return false;
  Obj *var = node->lhs->var;
  Node *rhs = node->rhs;
  int size = var->ty->size;
  if (rhs->kind == ND_NUM && is_int_or_ptr(rhs->ty)) {
    if (rhs->val == 0)
      emit("xor %s, %s", callee_reg(var->reg, 4), callee_reg(var->reg, 4));
    else if (size <= 4)
      emit("mov $%d, %s", (int32_t)rhs->val, callee_reg(var->reg, 4));
    else if (fits_imm32(rhs->val))
      emit("mov $%lld, %s", (long long)rhs->val, callee_reg(var->reg, 8));
    else
      emit("movabs $%lld, %s", (long long)rhs->val, callee_reg(var->reg, 8));
    return true;
  }
  if (rhs->kind == ND_VAR && is_int_or_ptr(rhs->ty) && rhs->ty->size == size && !rhs->ty->is_volatile) {
    if (is_gp_reg_var(rhs->var)) {
      emit("mov %s, %s", callee_reg(rhs->var->reg, 8), callee_reg(var->reg, 8));
      return true;
    }
    char *src = var_operand(rhs->var);
    if (src) {
      load_int(var->ty, src, callee_reg(var->reg, 4), callee_reg(var->reg, 8));
      return true;
    }
  }
  return false;
}

// Integer widening of a variable straight from where it lives:
// "movslq %ebx, %rax" instead of "mov %rbx, %rax; movslq %eax, %rax".
static bool gen_cast_of_var(Node *node) {
  Node *v = node->lhs;
  if (v->kind != ND_VAR || !is_integer(v->ty) || !is_int_or_ptr(node->ty) || node->ty->kind == TY_BOOL ||
      v->ty->is_volatile || node->ty->size < v->ty->size)
    return false;
  const char *src = simple_operand(v, v->ty->size);
  if (!src || src[0] == '$')
    return false;
  if (node->ty->size == 8 && v->ty->size == 4) {
    if (v->ty->is_unsigned)
      emit("mov %s, %%eax", src);
    else
      emit("movslq %s, %%rax", src);
    return true;
  }
  if (v->ty->size < 4 && node->ty->size == 4) {
    load_int(v->ty, src, "%eax", "%rax"); // sign/zero extends to 32 bits
    return true;
  }
  return false;
}

// Structural equality of side-effect-free lvalues and address expressions.
static bool same_tree(Node *a, Node *b) {
  if (!a || !b)
    return a == b;
  if (a->kind != b->kind || a->ty->size != b->ty->size)
    return false;
  switch (a->kind) {
  case ND_VAR:
    return a->var == b->var;
  case ND_NUM:
    return a->val == b->val;
  case ND_MEMBER:
    return a->member == b->member && same_tree(a->lhs, b->lhs);
  case ND_DEREF: case ND_ADDR: case ND_CAST:
    return a->ty->is_unsigned == b->ty->is_unsigned && same_tree(a->lhs, b->lhs);
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_SHL:
    return same_tree(a->lhs, b->lhs) && same_tree(a->rhs, b->rhs);
  default:
    return false;
  }
}

// "L = L op e" on an integer or floating lvalue in memory: op straight into memory.
static bool gen_read_modify_write(Node *node) {
  if (node->kind != ND_ASSIGN)
    return false;
  Node *lhs = node->lhs;
  Type *ty = lhs->ty;
  if (has_side_effects(lhs) || (lhs->kind == ND_MEMBER && lhs->member->is_bitfield) ||
      (lhs->kind == ND_VAR && is_gp_reg_var(lhs->var)) || cg.temp_depth >= MAX_TEMPS)
    return false;
  Node *rhs = strip_same_repr_casts(node->rhs);

  if (is_flonum(ty)) {
    if (rhs->ty->kind != ty->kind)
      return false;
    static const char *ops[] = {[ND_ADD] = "add", [ND_SUB] = "sub", [ND_MUL] = "mul", [ND_DIV] = "div"};
    if (rhs->kind > ND_DIV || !ops[rhs->kind] || !same_tree(rhs->lhs, lhs))
      return false;
    const char *sz = ty->kind == TY_FLOAT ? "ss" : "sd";
    if (lhs->kind == ND_VAR && is_xmm_var(lhs->var)) {
      gen_expr(rhs->rhs);
      emit("%s%s %%xmm0, %s", ops[rhs->kind], sz, xmm_var_reg(lhs->var));
      return true;
    }
    gen_expr(rhs->rhs);
    int t = save_temp(true);
    Mem m;
    lvalue_mem(lhs, &m);
    char *mem = mem_str(&m);
    emit("mov%s %s, %%xmm0", sz, mem);
    emit("%s%s %s, %%xmm0", ops[rhs->kind], sz, fp_temps[t]);
    emit("mov%s %%xmm0, %s", sz, mem);
    release_temp();
    return true;
  }

  if (!is_int_or_ptr(ty) || ty->size < 4 || rhs->ty->size != ty->size || !same_tree(rhs->lhs, lhs))
    return false;
  const char *op;
  switch (rhs->kind) {
  case ND_ADD: op = "add"; break;
  case ND_SUB: op = "sub"; break;
  case ND_BITAND: op = "and"; break;
  case ND_BITOR: op = "or"; break;
  case ND_BITXOR: op = "xor"; break;
  default: return false;
  }
  const char *suffix = ty->size == 8 ? "q" : "l";
  const char *src = simple_operand(rhs->rhs, ty->size);
  if (src && src[0] == '$') {
    Mem m;
    lvalue_mem(lhs, &m);
    emit("%s%s %s, %s", op, suffix, src, mem_str(&m));
    return true;
  }
  gen_expr(rhs->rhs);
  int t = save_temp(false);
  Mem m;
  lvalue_mem(lhs, &m);
  emit("%s %s, %s", op, int_temp(t, ty->size), mem_str(&m));
  release_temp();
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
    if (gen_update_in_place(node) || gen_read_modify_write(node) || gen_assign_simple_to_reg(node))
      return;
    if (node->kind == ND_ASSIGN) {
      gen_assign(node, false);
      return;
    }
  }
  gen_expr(node);
}
