// stmt.c - statements, functions and stack frames.
#include "codegen/cg.h"

// ---------------------------------------------------------------------------
// switch
// ---------------------------------------------------------------------------

static bool fits_imm32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

// Dense switches with many cases become a jump table (-o prod).
static bool gen_jump_table(Node *node) {
  int count = 0;
  int64_t min = INT64_MAX, max = INT64_MIN;
  for (Node *c = node->case_next; c; c = c->case_next) {
    if (c->case_begin != c->case_end)
      return false;
    count++;
    min = MIN(min, c->case_begin);
    max = MAX(max, c->case_end);
  }
  if (count < 4 || (uint64_t)(max - min) >= 4096 || (uint64_t)(max - min) >= (uint64_t)count * 3)
    return false;

  int size = node->cond->ty->size;
  if (size == 4)
    emit(node->cond->ty->is_unsigned ? "mov %%eax, %%eax" : "movslq %%eax, %%rax");
  if (min != 0) {
    if (fits_imm32(min)) {
      emit("sub $%lld, %%rax", (long long)min);
    } else {
      emit("movabs $%lld, %%rdi", (long long)min);
      emit("sub %%rdi, %%rax");
    }
  }
  int fallback = node->default_case ? node->default_case->label_id : node->brk_label;
  int table = new_cg_label();
  emit("cmp $%lld, %%rax", (long long)(max - min));
  emit("ja %s", parser_label(fallback));
  emit("lea .L.jt.%d(%%rip), %%rdx", table);
  emit("movslq (%%rdx,%%rax,4), %%rax");
  emit("add %%rdx, %%rax");
  emit("jmp *%%rax");

  emit_raw("\t.section .rodata");
  emit_raw("\t.p2align 2");
  emit_label(".L.jt.%d", table);
  for (int64_t v = min; v <= max; v++) {
    int target = fallback;
    for (Node *c = node->case_next; c; c = c->case_next)
      if (c->case_begin == v)
        target = c->label_id;
    emit_raw("\t.long .L%d-.L.jt.%d", target, table);
  }
  emit_raw("\t.text");
  cg.stats->jump_tables++;
  return true;
}

static void gen_switch(Node *node) {
  gen_expr(node->cond);
  if (!cg.optimize || !gen_jump_table(node)) {
    int size = node->cond->ty->size < 8 ? 4 : 8;
    const char *ax = reg_ax(size), *di = reg_di(size);
    for (Node *c = node->case_next; c; c = c->case_next) {
      if (c->case_begin == c->case_end) {
        if (fits_imm32(c->case_begin) || size == 4) {
          emit("cmp $%lld, %s", (long long)(size == 4 ? (int32_t)c->case_begin : c->case_begin), ax);
        } else {
          emit("movabs $%lld, %%rdi", (long long)c->case_begin);
          emit("cmp %%rdi, %%rax");
        }
        emit("je %s", parser_label(c->label_id));
        continue;
      }
      // Case range: (x - begin) <= (end - begin) as unsigned.
      emit("mov %s, %s", ax, di);
      emit("sub $%lld, %s", (long long)c->case_begin, di);
      emit("cmp $%lld, %s", (long long)(c->case_end - c->case_begin), di);
      emit("jbe %s", parser_label(c->label_id));
    }
    emit("jmp %s", parser_label(node->default_case ? node->default_case->label_id : node->brk_label));
  }
  gen_stmt(node->then);
  emit_label("%s", parser_label(node->brk_label));
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

static void gen_loop(Node *node) {
  if (node->init)
    gen_stmt(node->init);

  if (cg.optimize && node->cond) {
    // Rotated loop: the condition is tested at the bottom, one jump per iteration.
    const char *body = label_name(new_cg_label()), *test = label_name(new_cg_label());
    emit("jmp %s", test);
    emit_label("%s", body);
    gen_stmt(node->then);
    emit_label("%s", parser_label(node->cont_label));
    if (node->inc)
      gen_discard(node->inc);
    emit_label("%s", test);
    gen_branch(node->cond, body, true);
    emit_label("%s", parser_label(node->brk_label));
    return;
  }

  const char *begin = label_name(new_cg_label());
  emit_label("%s", begin);
  if (node->cond)
    gen_branch(node->cond, parser_label(node->brk_label), false);
  gen_stmt(node->then);
  emit_label("%s", parser_label(node->cont_label));
  if (node->inc)
    gen_discard(node->inc);
  emit("jmp %s", begin);
  emit_label("%s", parser_label(node->brk_label));
}

void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_IF: {
    const char *els = label_name(new_cg_label());
    gen_branch(node->cond, els, false);
    gen_stmt(node->then);
    if (node->els) {
      const char *end = label_name(new_cg_label());
      emit("jmp %s", end);
      emit_label("%s", els);
      gen_stmt(node->els);
      emit_label("%s", end);
    } else {
      emit_label("%s", els);
    }
    return;
  }
  case ND_FOR:
    gen_loop(node);
    return;
  case ND_DO: {
    const char *begin = label_name(new_cg_label());
    emit_label("%s", begin);
    gen_stmt(node->then);
    emit_label("%s", parser_label(node->cont_label));
    gen_branch(node->cond, begin, true);
    emit_label("%s", parser_label(node->brk_label));
    return;
  }
  case ND_SWITCH:
    gen_switch(node);
    return;
  case ND_CASE:
  case ND_LABEL:
    emit_label("%s", parser_label(node->label_id));
    gen_stmt(node->lhs);
    return;
  case ND_BLOCK:
    for (Node *s = node->body; s; s = s->next)
      gen_stmt(s);
    return;
  case ND_GOTO:
    emit("jmp %s", parser_label(node->label_id));
    return;
  case ND_RETURN:
    if (node->lhs) {
      if (cg.fn->ty->return_ty->kind == TY_VOID) {
        gen_discard(node->lhs);
      } else {
        gen_expr(node->lhs);
        gen_return_value(node->lhs->ty);
      }
    }
    emit("jmp .L.return.%s", cg.fn->name);
    return;
  case ND_EXPR_STMT:
    gen_discard(node->lhs);
    if (cg.depth != 0)
      ICE("unbalanced stack in expression statement");
    return;
  case ND_ASM:
    // User assembly is fenced off so that the peephole pass leaves it alone.
    emit_raw("#APP");
    emit_raw("\t%s", node->label_name);
    emit_raw("#NO_APP");
    return;
  case ND_UNREACHABLE:
    emit("ud2");
    return;
  default:
    ICE("unknown statement kind %d", node->kind);
  }
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

static int save_slot[NUM_CALLEE_SAVED];

static int slot_size(const Type *ty) {
  // Aggregates are padded to whole eightbytes so that register-sized
  // stores of struct arguments and return values stay inside the slot.
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION)
    return (int)align_to(ty->size, 8);
  return ty->size;
}

static int layout_frame(Obj *fn) {
  int offset = 0;
  for (int r = 0; r < NUM_CALLEE_SAVED; r++) {
    if (cg.used_regs & (1u << r)) {
      offset += 8;
      save_slot[r] = -offset;
    }
  }
  if (passed_in_memory(fn->ty->return_ty)) {
    fn->ret_ptr = NEW(Obj);
    offset += 8;
    fn->ret_ptr->offset = -offset;
  }
  for (Obj *var = fn->locals; var; var = var->next) {
    if (var->reg >= 0)
      continue;
    int align = MAX(var->align, var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION ? 8 : 1);
    offset = (int)align_to(offset + slot_size(var->ty), MIN(align, 16));
    var->offset = -offset;
  }
  return (int)align_to(offset, 16);
}

void gen_function(Obj *fn) {
  bool is_global = !fn->is_static && !fn->is_inline_def;
  emit_raw("\t.text");
  if (is_global)
    emit_raw("\t.globl %s", fn->name);
  emit_raw("\t.type %s, @function", fn->name);
  emit_label("%s", fn->name);

  cg.fn = fn;
  cg.depth = 0;
  cg.used_regs = 0;
  for (Obj *var = fn->locals; var; var = var->next)
    var->reg = -1;
  if (cg.optimize)
    promote_locals(fn);

  int stack_size = layout_frame(fn);
  emit("push %%rbp");
  emit("mov %%rsp, %%rbp");
  if (stack_size)
    emit("sub $%d, %%rsp", stack_size);
  for (int r = 0; r < NUM_CALLEE_SAVED; r++)
    if (cg.used_regs & (1u << r))
      emit("mov %s, %d(%%rbp)", callee_reg(r, 8), save_slot[r]);

  gen_params(fn);
  gen_stmt(fn->body);
  if (cg.depth != 0)
    ICE("unbalanced stack at end of '%s'", fn->name);

  // Falling off the end of main returns 0 (C23 5.1.2.3.4).
  if (strcmp(fn->name, "main") == 0)
    emit("xor %%eax, %%eax");

  emit_label(".L.return.%s", fn->name);
  for (int r = 0; r < NUM_CALLEE_SAVED; r++)
    if (cg.used_regs & (1u << r))
      emit("mov %d(%%rbp), %s", save_slot[r], callee_reg(r, 8));
  emit("mov %%rbp, %%rsp");
  emit("pop %%rbp");
  emit("ret");
  emit_raw("\t.size %s, .-%s", fn->name, fn->name);
}
