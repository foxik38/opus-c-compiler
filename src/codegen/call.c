// call.c - the System V AMD64 calling convention.
//
// Arguments: integers and pointers in %rdi %rsi %rdx %rcx %r8 %r9, floating
// point in %xmm0-7, the rest on the stack. A struct of at most 16 bytes is
// split into "eightbytes" that are passed in SSE or integer registers; larger
// ones are copied onto the stack. Structs larger than 16 bytes are returned
// through a hidden pointer passed in %rdi.
#include "codegen/cg.h"

enum { MAX_GP = 6, MAX_FP = 8 };

static const char *gp64[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

static bool is_aggregate_type(const Type *ty) { return ty->kind == TY_STRUCT || ty->kind == TY_UNION; }

bool passed_in_memory(Type *ty) { return is_aggregate_type(ty) && ty->size > 16; }

int eightbyte_count(Type *ty) { return (ty->size + 7) / 8; }

// True if every scalar overlapping [lo, hi) is floating point.
static bool only_floats(Type *ty, int lo, int hi, int offset) {
  if (is_aggregate_type(ty)) {
    for (Member *m = ty->members; m; m = m->next)
      if (!only_floats(m->ty, lo, hi, offset + m->offset))
        return false;
    return true;
  }
  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < ty->array_len; i++)
      if (!only_floats(ty->base, lo, hi, offset + i * ty->base->size))
        return false;
    return true;
  }
  return offset + ty->size <= lo || hi <= offset || ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE;
}

EightbyteClass eightbyte_class(Type *ty, int idx) {
  return only_floats(ty, idx * 8, idx * 8 + 8, 0) ? CLASS_SSE : CLASS_INTEGER;
}

static int eightbyte_bytes(Type *ty, int idx) { return MIN(8, ty->size - idx * 8); }

static void count_classes(Type *ty, int *gp, int *fp) {
  *gp = *fp = 0;
  for (int i = 0; i < eightbyte_count(ty); i++) {
    if (eightbyte_class(ty, i) == CLASS_SSE)
      (*fp)++;
    else
      (*gp)++;
  }
}

// Integer destination registers for partial eightbyte loads.
typedef struct {
  const char *r64, *r32, *r8;
} IntReg;

static const IntReg REG_RAX = {"%rax", "%eax", "%al"};
static const IntReg REG_RDX = {"%rdx", "%edx", "%dl"};
static const IntReg REG_RDI = {"%rdi", "%edi", "%dil"};

// Loads `bytes` (1..8) bytes at offset(base) without reading past them.
static void load_int_bytes(const char *base, int offset, int bytes, IntReg r) {
  if (bytes == 8) {
    emit("mov %d(%s), %s", offset, base, r.r64);
  } else if (bytes == 4) {
    emit("mov %d(%s), %s", offset, base, r.r32);
  } else {
    emit("xor %s, %s", r.r32, r.r32);
    for (int i = bytes - 1; i >= 0; i--) {
      emit("shl $8, %s", r.r64);
      emit("mov %d(%s), %s", offset + i, base, r.r8);
    }
  }
}

static void load_sse_bytes(const char *base, int offset, int bytes, int xmm) {
  emit("%s %d(%s), %%xmm%d", bytes == 4 ? "movss" : "movsd", offset, base, xmm);
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

typedef struct {
  Node *arg;
  bool on_stack;
  bool deferred; // simple argument loaded straight into its register
} ArgInfo;

static bool is_simple_arg(Node *arg) {
  if (!cg.optimize)
    return false;
  switch (arg->kind) {
  case ND_NUM:
    return is_scalar(arg->ty);
  case ND_VAR: {
    Obj *var = arg->var;
    if (!is_scalar(arg->ty) || var->is_tls)
      return false;
    return var->is_local || is_local_symbol(var);
  }
  case ND_ADDR: {
    Node *v = arg->lhs;
    return v->kind == ND_VAR && !v->var->is_tls && (v->var->is_local ? v->var->reg < 0 : is_local_symbol(v->var));
  }
  default:
    return false;
  }
}

static void load_simple_arg(Node *arg, int gp, int fp) {
  Type *ty = arg->ty;
  if (is_flonum(ty)) {
    if (arg->kind == ND_NUM) {
      // Build the bit pattern in %rax: other %xmm registers may be live.
      if (ty->kind == TY_FLOAT) {
        float f = (float)arg->fval;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        emit("mov $%u, %%eax", bits);
        emit("movd %%eax, %%xmm%d", fp);
      } else {
        uint64_t bits;
        memcpy(&bits, &arg->fval, 8);
        emit("movabs $%llu, %%rax", (unsigned long long)bits);
        emit("movq %%rax, %%xmm%d", fp);
      }
      return;
    }
    Obj *var = arg->var;
    char *src = var->is_local ? format("%d(%%rbp)", var->offset) : symbol_ref(var);
    emit("%s %s, %%xmm%d", ty->kind == TY_FLOAT ? "movss" : "movsd", src, fp);
    return;
  }

  const char *r32 = arg_reg(gp, 4), *r64 = arg_reg(gp, 8);
  switch (arg->kind) {
  case ND_NUM:
    if (arg->val == 0)
      emit("xor %s, %s", r32, r32);
    else if (ty->size <= 4)
      emit("mov $%d, %s", (int32_t)arg->val, r32);
    else if (arg->val >= INT32_MIN && arg->val <= INT32_MAX)
      emit("mov $%lld, %s", (long long)arg->val, r64);
    else
      emit("movabs $%lld, %s", (long long)arg->val, r64);
    return;
  case ND_VAR: {
    Obj *var = arg->var;
    if (var->is_local && var->reg >= 0) {
      emit("mov %s, %s", callee_reg(var->reg, 8), r64);
      return;
    }
    char *src = var->is_local ? format("%d(%%rbp)", var->offset) : symbol_ref(var);
    bool zext = ty->is_unsigned || ty->kind == TY_BOOL;
    switch (ty->size) {
    case 1: emit("%s %s, %s", zext ? "movzbl" : "movsbl", src, r32); return;
    case 2: emit("%s %s, %s", zext ? "movzwl" : "movswl", src, r32); return;
    case 4: emit("mov %s, %s", src, r32); return;
    default: emit("mov %s, %s", src, r64); return;
    }
  }
  default: { // ND_ADDR of a variable
    Obj *var = arg->lhs->var;
    if (var->is_local)
      emit("lea %d(%%rbp), %s", var->offset, r64);
    else
      emit("lea %s, %s", symbol_ref(var), r64);
    return;
  }
  }
}

// Pushes an argument value (scalar, or a struct's eightbytes last-first).
static void push_reg_arg(Node *arg) {
  Type *ty = arg->ty;
  gen_expr(arg);
  if (is_flonum(ty)) {
    pushf();
    return;
  }
  if (!is_aggregate_type(ty)) {
    push();
    return;
  }
  // %rax holds the struct's address; push its eightbytes in reverse.
  for (int i = eightbyte_count(ty) - 1; i >= 0; i--) {
    int bytes = eightbyte_bytes(ty, i);
    if (eightbyte_class(ty, i) == CLASS_SSE) {
      load_sse_bytes("%rax", i * 8, bytes, 0);
      pushf();
    } else {
      load_int_bytes("%rax", i * 8, bytes, REG_RDI);
      emit("push %%rdi");
      cg.depth++;
    }
  }
}

static void push_stack_arg(Node *arg) {
  Type *ty = arg->ty;
  gen_expr(arg);
  if (is_aggregate_type(ty)) {
    int size = (int)align_to(ty->size, 8);
    emit("sub $%d, %%rsp", size);
    cg.depth += size / 8;
    emit("mov %%rsp, %%rdi");
    copy_struct(ty->size);
  } else if (is_flonum(ty)) {
    pushf();
  } else {
    push();
  }
}

static Obj *direct_callee(Node *node) {
  Node *fn = node->lhs;
  if (fn->kind == ND_ADDR && fn->lhs->kind == ND_VAR && fn->lhs->var->is_function)
    return fn->lhs->var;
  return nullptr;
}

void gen_funcall(Node *node) {
  Type *ret = node->ty;
  bool ret_in_mem = passed_in_memory(ret);

  int nargs = 0;
  for (Node *a = node->args; a; a = a->next)
    nargs++;
  ArgInfo *info = xcalloc((size_t)MAX(nargs, 1), sizeof(ArgInfo));

  // Classify arguments.
  int gp = ret_in_mem ? 1 : 0, fp = 0, stack_slots = 0;
  int i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    info[i].arg = a;
    Type *ty = a->ty;
    if (is_aggregate_type(ty)) {
      int ng, nf;
      count_classes(ty, &ng, &nf);
      if (passed_in_memory(ty) || gp + ng > MAX_GP || fp + nf > MAX_FP) {
        info[i].on_stack = true;
        stack_slots += (int)align_to(ty->size, 8) / 8;
      } else {
        gp += ng;
        fp += nf;
      }
    } else if (is_flonum(ty)) {
      if (fp < MAX_FP)
        fp++;
      else
        info[i].on_stack = true, stack_slots++;
    } else {
      if (gp < MAX_GP)
        gp++;
      else
        info[i].on_stack = true, stack_slots++;
    }
    info[i].deferred = !info[i].on_stack && !is_aggregate_type(ty) && is_simple_arg(a);
  }
  int total_fp = fp;

  // The stack must be 16-byte aligned at the call instruction.
  bool pad = (cg.depth + stack_slots) % 2 == 1;
  if (pad) {
    emit("sub $8, %%rsp");
    cg.depth++;
  }

  for (i = nargs - 1; i >= 0; i--)
    if (info[i].on_stack)
      push_stack_arg(info[i].arg);
  for (i = nargs - 1; i >= 0; i--)
    if (!info[i].on_stack && !info[i].deferred)
      push_reg_arg(info[i].arg);

  Obj *callee = direct_callee(node);
  if (!callee) {
    gen_expr(node->lhs);
    emit("mov %%rax, %%r10");
  }

  // Move register arguments into place.
  gp = ret_in_mem ? 1 : 0;
  fp = 0;
  for (i = 0; i < nargs; i++) {
    ArgInfo *ai = &info[i];
    if (ai->on_stack)
      continue;
    Type *ty = ai->arg->ty;
    if (ai->deferred) {
      if (is_flonum(ty))
        fp++;
      else
        gp++;
      continue;
    }
    if (is_aggregate_type(ty)) {
      for (int k = 0; k < eightbyte_count(ty); k++) {
        if (eightbyte_class(ty, k) == CLASS_SSE)
          popf(fp++);
        else
          pop(gp64[gp++]);
      }
    } else if (is_flonum(ty)) {
      popf(fp++);
    } else {
      pop(gp64[gp++]);
    }
  }

  // Simple arguments go last: loading them clobbers no argument register.
  // Floating-point constants use %xmm0 as scratch, so they are loaded in
  // descending register order.
  gp = ret_in_mem ? 1 : 0;
  fp = 0;
  typedef struct {
    Node *arg;
    int gp, fp;
  } Deferred;
  Deferred *later = xcalloc((size_t)MAX(nargs, 1), sizeof(Deferred));
  int nlater = 0;
  for (i = 0; i < nargs; i++) {
    ArgInfo *ai = &info[i];
    if (ai->on_stack)
      continue;
    Type *ty = ai->arg->ty;
    if (ai->deferred)
      later[nlater++] = (Deferred){ai->arg, gp, fp};
    if (is_aggregate_type(ty)) {
      int ng, nf;
      count_classes(ty, &ng, &nf);
      gp += ng;
      fp += nf;
    } else if (is_flonum(ty)) {
      fp++;
    } else {
      gp++;
    }
  }
  for (int k = nlater - 1; k >= 0; k--)
    load_simple_arg(later[k].arg, later[k].gp, later[k].fp);
  free(later);
  free(info);

  if (ret_in_mem)
    emit("lea %d(%%rbp), %%rdi", node->ret_buffer->offset);
  if (node->func_ty->is_variadic) {
    if (total_fp == 0 && cg.optimize)
      emit("xor %%eax, %%eax");
    else
      emit("mov $%d, %%eax", total_fp);
  }

  if (!callee)
    emit("call *%%r10");
  else if (is_local_symbol(callee))
    emit("call %s", callee->name);
  else
    emit("call %s@PLT", callee->name);

  int cleanup = (stack_slots + pad) * 8;
  if (cleanup)
    emit("add $%d, %%rsp", cleanup);
  cg.depth -= stack_slots + pad;

  // Normalize narrow return values (the ABI leaves the upper bits undefined).
  switch (ret->kind) {
  case TY_BOOL:
    emit("movzbl %%al, %%eax");
    return;
  case TY_CHAR:
    emit(ret->is_unsigned ? "movzbl %%al, %%eax" : "movsbl %%al, %%eax");
    return;
  case TY_SHORT:
    emit(ret->is_unsigned ? "movzwl %%ax, %%eax" : "movswl %%ax, %%eax");
    return;
  default:
    break;
  }

  if (!is_aggregate_type(ret))
    return;
  int off = node->ret_buffer->offset;
  if (!ret_in_mem) {
    int gi = 0, si = 0;
    for (int k = 0; k < eightbyte_count(ret); k++) {
      if (eightbyte_class(ret, k) == CLASS_SSE)
        emit("movsd %%xmm%d, %d(%%rbp)", si++, off + k * 8);
      else
        emit("mov %s, %d(%%rbp)", gi++ ? "%rdx" : "%rax", off + k * 8);
    }
  }
  emit("lea %d(%%rbp), %%rax", off);
}

// ---------------------------------------------------------------------------
// Function entry
// ---------------------------------------------------------------------------

static int va_gp_offset, va_fp_offset, va_overflow_offset;

void gen_params(Obj *fn) {
  int gp = 0, fp = 0, stack_off = 16;
  if (passed_in_memory(fn->ty->return_ty)) {
    emit("mov %%rdi, %d(%%rbp)", fn->ret_ptr->offset);
    gp = 1;
  }

  if (fn->va_area) {
    int base = fn->va_area->offset;
    for (int i = 0; i < MAX_GP; i++)
      emit("mov %s, %d(%%rbp)", gp64[i], base + i * 8);
    for (int i = 0; i < MAX_FP; i++)
      emit("movsd %%xmm%d, %d(%%rbp)", i, base + 48 + i * 16);
  }

  for (Obj *p = fn->params; p; p = p->next_param) {
    Type *ty = p->ty;
    if (is_aggregate_type(ty)) {
      int ng, nf;
      count_classes(ty, &ng, &nf);
      if (passed_in_memory(ty) || gp + ng > MAX_GP || fp + nf > MAX_FP) {
        p->offset = stack_off;
        stack_off += (int)align_to(ty->size, 8);
        continue;
      }
      for (int k = 0; k < eightbyte_count(ty); k++) {
        if (eightbyte_class(ty, k) == CLASS_SSE)
          emit("movsd %%xmm%d, %d(%%rbp)", fp++, p->offset + k * 8);
        else
          emit("mov %s, %d(%%rbp)", gp64[gp++], p->offset + k * 8);
      }
      continue;
    }

    if (is_flonum(ty)) {
      if (fp >= MAX_FP) {
        p->offset = stack_off;
        stack_off += 8;
        continue;
      }
      emit("%s %%xmm%d, %d(%%rbp)", ty->kind == TY_FLOAT ? "movss" : "movsd", fp++, p->offset);
      continue;
    }

    bool zext = ty->is_unsigned || ty->kind == TY_BOOL;
    if (gp >= MAX_GP) {
      if (p->reg >= 0) {
        char *src = format("%d(%%rbp)", stack_off);
        const char *r32 = callee_reg(p->reg, 4), *r64 = callee_reg(p->reg, 8);
        switch (ty->size) {
        case 1: emit("%s %s, %s", zext ? "movzbl" : "movsbl", src, r32); break;
        case 2: emit("%s %s, %s", zext ? "movzwl" : "movswl", src, r32); break;
        case 4: emit("mov %s, %s", src, r32); break;
        default: emit("mov %s, %s", src, r64); break;
        }
      } else {
        p->offset = stack_off;
      }
      stack_off += 8;
      continue;
    }

    if (p->reg >= 0) {
      const char *r32 = callee_reg(p->reg, 4), *r64 = callee_reg(p->reg, 8);
      switch (ty->size) {
      case 1: emit("%s %s, %s", zext ? "movzbl" : "movsbl", arg_reg(gp, 1), r32); break;
      case 2: emit("%s %s, %s", zext ? "movzwl" : "movswl", arg_reg(gp, 2), r32); break;
      case 4: emit("mov %s, %s", arg_reg(gp, 4), r32); break;
      default: emit("mov %s, %s", arg_reg(gp, 8), r64); break;
      }
    } else {
      emit("mov %s, %d(%%rbp)", arg_reg(gp, ty->size), p->offset);
    }
    gp++;
  }

  va_gp_offset = gp * 8;
  va_fp_offset = 48 + fp * 16;
  va_overflow_offset = stack_off;
}

// ---------------------------------------------------------------------------
// Variadic functions
// ---------------------------------------------------------------------------

void gen_va_start(Node *node) {
  gen_expr(node->lhs);
  emit("movl $%d, (%%rax)", va_gp_offset);
  emit("movl $%d, 4(%%rax)", va_fp_offset);
  emit("lea %d(%%rbp), %%rdx", va_overflow_offset);
  emit("mov %%rdx, 8(%%rax)");
  emit("lea %d(%%rbp), %%rdx", cg.fn->va_area->offset);
  emit("mov %%rdx, 16(%%rax)");
}

// Leaves the address of the next argument of type node->ty->base in %rax.
void gen_va_arg(Node *node) {
  Type *ty = node->ty->base;
  gen_expr(node->lhs);
  emit("mov %%rax, %%rcx");

  int overflow = new_cg_label(), done = new_cg_label();
  bool in_regs = !passed_in_memory(ty);
  if (in_regs && is_aggregate_type(ty)) {
    int ng, nf;
    count_classes(ty, &ng, &nf);
    if (ng && nf)
      ICE("va_arg of a struct mixing integer and floating-point fields is not supported");
    if (nf > 1)
      ICE("va_arg of a struct with two floating-point eightbytes is not supported");
  }

  if (in_regs) {
    bool sse = is_flonum(ty) || (is_aggregate_type(ty) && eightbyte_class(ty, 0) == CLASS_SSE);
    if (sse) {
      emit("cmpl $%d, 4(%%rcx)", 176 - 16);
      emit("ja .L.cg.%d", overflow);
      emit("movl 4(%%rcx), %%edx");
      emit("mov 16(%%rcx), %%rax");
      emit("add %%rdx, %%rax");
      emit("addl $16, 4(%%rcx)");
    } else {
      int slots = is_aggregate_type(ty) ? eightbyte_count(ty) : 1;
      emit("cmpl $%d, (%%rcx)", 48 - 8 * slots);
      emit("ja .L.cg.%d", overflow);
      emit("movl (%%rcx), %%edx");
      emit("mov 16(%%rcx), %%rax");
      emit("add %%rdx, %%rax");
      emit("addl $%d, (%%rcx)", 8 * slots);
    }
    emit("jmp .L.cg.%d", done);
  }

  emit_label(".L.cg.%d", overflow);
  emit("mov 8(%%rcx), %%rax");
  if (ty->align > 8) {
    emit("add $%d, %%rax", ty->align - 1);
    emit("and $%d, %%rax", -ty->align);
  }
  emit("lea %d(%%rax), %%rdx", (int)align_to(ty->size, 8));
  emit("mov %%rdx, 8(%%rcx)");
  emit_label(".L.cg.%d", done);
}

// ---------------------------------------------------------------------------
// Returns
// ---------------------------------------------------------------------------

void gen_return_value(Type *ty) {
  if (!is_aggregate_type(ty))
    return; // scalars are already in %rax / %xmm0

  if (passed_in_memory(ty)) {
    emit("mov %d(%%rbp), %%rdi", cg.fn->ret_ptr->offset);
    copy_struct(ty->size);
    emit("mov %%rdi, %%rax");
    return;
  }

  emit("mov %%rax, %%rcx");
  int gi = 0, si = 0;
  for (int k = 0; k < eightbyte_count(ty); k++) {
    int bytes = eightbyte_bytes(ty, k);
    if (eightbyte_class(ty, k) == CLASS_SSE)
      load_sse_bytes("%rcx", k * 8, bytes, si++);
    else
      load_int_bytes("%rcx", k * 8, bytes, gi++ ? REG_RDX : REG_RAX);
  }
}
