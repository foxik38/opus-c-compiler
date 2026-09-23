// stmt.c - statements (C23 6.8) and the translation-unit entry point.
#include "parse/parser.h"

static Node *stmt(Token **rest, Token *tok);

static Node *empty_block(Token *tok) { return new_node(ND_BLOCK, tok); }

// A label may be followed by a declaration or the end of a block (C23).
static Node *labeled_body(Token **rest, Token *tok) {
  if (tok_equal(tok, "}") || is_typename(tok)) {
    *rest = tok;
    return empty_block(tok);
  }
  return stmt(rest, tok);
}

// "if (x);" is almost always a typo.
static void check_empty_body(Token *body, Token *rparen, const char *what, bool same_line_only) {
  if (!tok_equal(body, ";"))
    return;
  if (same_line_only && body->line != rparen->line)
    return;
  warn_tok(W_EMPTY_BODY, body, "%s has an empty body; did you mean to put a statement here?", what);
}

static Obj *local_storage_of(Node *n);

// Returns the local variable a pointer expression points into, if any.
static Obj *address_of_local(Node *ptr) {
  switch (ptr->kind) {
  case ND_ADDR:
    return local_storage_of(ptr->lhs);
  case ND_ADD: case ND_SUB:
    return ptr->ty->kind == TY_PTR ? address_of_local(ptr->lhs) : nullptr;
  case ND_CAST:
    return is_pointer_like(ptr->lhs->ty) ? address_of_local(ptr->lhs) : nullptr;
  default:
    return nullptr;
  }
}

static Obj *local_storage_of(Node *n) {
  switch (n->kind) {
  case ND_VAR:
    return n->var->is_local && n->var->name[0] ? n->var : nullptr;
  case ND_MEMBER:
    return local_storage_of(n->lhs);
  case ND_DEREF:
    return address_of_local(n->lhs);
  default:
    return nullptr;
  }
}

static Node *return_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_RETURN, tok);
  Obj *fn = P.current_fn;
  Type *ret = fn->ty->return_ty;
  if (tok_consume(rest, tok->next, ";")) {
    if (ret->kind != TY_VOID)
      error_tok(tok, "non-void function '%s' should return a value", fn->name);
    return node;
  }

  Token *expr_tok = tok->next;
  Node *e = expr(&tok, tok->next);
  *rest = tok_skip(tok, ";");

  if (ret->kind == TY_VOID) {
    if (e->ty->kind != TY_VOID)
      error_tok(expr_tok, "void function '%s' should not return a value", fn->name);
    node->lhs = e;
    return node;
  }

  e = rvalue(e);
  Obj *local = address_of_local(e);
  if (local)
    warn_tok(W_RETURN_LOCAL_ADDR, expr_tok, "address of stack memory associated with local variable '%s' returned",
             local->name);
  node->lhs = convert_for_assign(e, unqualified(ret), expr_tok, "returning");
  return node;
}

static Node *if_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_IF, tok);
  tok = tok_skip(tok->next, "(");
  Token *cond_tok = tok;
  node->cond = to_condition(expr(&tok, tok), cond_tok);
  Token *rparen = tok;
  tok = tok_skip(tok, ")");
  check_empty_body(tok, rparen, "'if' statement", false);
  node->then = stmt(&tok, tok);
  if (tok_equal(tok, "else")) {
    check_empty_body(tok->next, tok, "'else' clause", true);
    node->els = stmt(&tok, tok->next);
  }
  *rest = tok;
  return node;
}

static Node *switch_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_SWITCH, tok);
  tok = tok_skip(tok->next, "(");
  Token *cond_tok = tok;
  Node *cond = rvalue(expr(&tok, tok));
  if (!is_integer(cond->ty))
    error_tok(cond_tok, "statement requires expression of integer type ('%s' invalid)", type_name(cond->ty));
  Type *pty = integer_promote(cond->ty);
  if (cond->ty != pty) {
    Node *c = new_cast(cond, pty);
    c->is_implicit = true;
    cond = c;
  }
  node->cond = cond;
  tok = tok_skip(tok, ")");

  Node *saved_switch = P.current_switch;
  int saved_brk = P.brk_label;
  P.current_switch = node;
  P.brk_label = node->brk_label = new_label();

  node->then = stmt(rest, tok);

  P.current_switch = saved_switch;
  P.brk_label = saved_brk;
  return node;
}

static int64_t case_value(Token **rest, Token *tok, Type *ty) {
  Token *start = tok;
  Node *v = conditional(rest, tok);
  if (!is_integer(v->ty) || !is_const_int_expr(v))
    error_tok(start, "case label does not reduce to an integer constant");
  int64_t val = eval_int(v);
  // Convert to the promoted type of the controlling expression.
  if (ty->size == 4)
    val = ty->is_unsigned ? (int64_t)(uint32_t)val : (int64_t)(int32_t)val;
  return val;
}

static Node *case_stmt(Token **rest, Token *tok) {
  Node *sw = P.current_switch;
  if (!sw)
    error_tok(tok, "'case' statement not in switch statement");

  Node *node = new_node(ND_CASE, tok);
  Type *ty = sw->cond->ty;
  int64_t begin = case_value(&tok, tok->next, ty);
  int64_t end = begin;
  if (tok_equal(tok, "...")) // GNU case range
    end = case_value(&tok, tok->next, ty);
  if (end < begin)
    warn_tok(W_OVERFLOW, node->tok, "empty case range specified");
  tok = tok_skip(tok, ":");

  for (Node *c = sw->case_next; c; c = c->case_next) {
    if (begin <= c->case_end && c->case_begin <= end) {
      error_tok_nofatal(node->tok, "duplicate case value '%lld'", (long long)begin);
      note_tok(c->tok, "previous case defined here");
      diag_abort();
    }
  }

  node->label_id = new_label();
  node->case_begin = begin;
  node->case_end = end;
  node->lhs = labeled_body(rest, tok);
  node->case_next = sw->case_next;
  sw->case_next = node;
  return node;
}

static Node *default_stmt(Token **rest, Token *tok) {
  Node *sw = P.current_switch;
  if (!sw)
    error_tok(tok, "'default' statement not in switch statement");
  if (sw->default_case) {
    error_tok_nofatal(tok, "multiple default labels in one switch");
    note_tok(sw->default_case->tok, "previous default label is here");
    diag_abort();
  }
  Node *node = new_node(ND_CASE, tok);
  tok = tok_skip(tok->next, ":");
  node->label_id = new_label();
  node->lhs = labeled_body(rest, tok);
  sw->default_case = node;
  return node;
}

// Loop bodies set the break/continue targets and the loop depth.
typedef struct {
  int brk, cont;
} LoopSave;

static LoopSave enter_loop(Node *node) {
  LoopSave s = {P.brk_label, P.cont_label};
  P.brk_label = node->brk_label = new_label();
  P.cont_label = node->cont_label = new_label();
  P.loop_depth++;
  return s;
}

static void leave_loop(LoopSave s) {
  P.brk_label = s.brk;
  P.cont_label = s.cont;
  P.loop_depth--;
}

static Node *for_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_FOR, tok);
  tok = tok_skip(tok->next, "(");
  enter_scope();
  LoopSave save = enter_loop(node);

  if (is_typename(tok)) {
    VarAttr attr = {};
    Type *basety = declspec(&tok, tok, &attr);
    if (attr.is_typedef || attr.is_extern)
      error_tok(tok, "declaration of non-local variable in 'for' loop");
    node->init = declaration(&tok, tok, basety, &attr);
  } else if (tok_equal(tok, ";")) {
    tok = tok->next;
  } else {
    Node *e = expr(&tok, tok);
    node->init = new_unary(ND_EXPR_STMT, e, e->tok);
    tok = tok_skip(tok, ";");
  }

  if (!tok_equal(tok, ";")) {
    Token *cond_tok = tok;
    node->cond = to_condition(expr(&tok, tok), cond_tok);
  }
  tok = tok_skip(tok, ";");

  if (!tok_equal(tok, ")"))
    node->inc = expr(&tok, tok);
  Token *rparen = tok;
  tok = tok_skip(tok, ")");
  check_empty_body(tok, rparen, "'for' loop", true);

  node->then = stmt(rest, tok);
  leave_loop(save);
  leave_scope();
  return node;
}

static Node *while_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_FOR, tok);
  tok = tok_skip(tok->next, "(");
  LoopSave save = enter_loop(node);
  Token *cond_tok = tok;
  node->cond = to_condition(expr(&tok, tok), cond_tok);
  Token *rparen = tok;
  tok = tok_skip(tok, ")");
  check_empty_body(tok, rparen, "'while' loop", true);
  node->then = stmt(rest, tok);
  leave_loop(save);
  return node;
}

static Node *do_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_DO, tok);
  LoopSave save = enter_loop(node);
  node->then = stmt(&tok, tok->next);
  if (!tok_equal(tok, "while"))
    error_tok(tok, "expected 'while' in do/while loop");
  tok = tok_skip(tok->next, "(");
  Token *cond_tok = tok;
  node->cond = to_condition(expr(&tok, tok), cond_tok);
  tok = tok_skip(tok, ")");
  *rest = tok_skip(tok, ";");
  leave_loop(save);
  return node;
}

static Node *asm_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_ASM, tok);
  tok = tok->next;
  while (tok_equal(tok, "volatile") || tok_equal(tok, "__volatile__") || tok_equal(tok, "inline"))
    tok = tok->next;
  tok = tok_skip(tok, "(");
  if (tok->kind != TK_STR)
    error_tok(tok, "expected string literal in asm statement");
  node->label_name = tok->str;
  tok = tok->next;
  if (tok_equal(tok, ":"))
    error_tok(tok, "extended asm with operands is not supported by occ");
  tok = tok_skip(tok, ")");
  *rest = tok_skip(tok, ";");
  return node;
}

// Warns about expression statements whose value is silently discarded.
static void check_discarded_value(Node *e) {
  if (e->kind == ND_CAST && e->ty->kind == TY_VOID)
    return; // explicitly discarded

  Node *call = e;
  if (call->kind == ND_FUNCALL && call->lhs->kind == ND_ADDR && call->lhs->lhs->kind == ND_VAR) {
    Obj *fn = call->lhs->lhs->var;
    if (fn->is_nodiscard)
      warn_tok(W_UNUSED_RESULT, call->lhs->tok, "ignoring return value of function declared with 'nodiscard' attribute%s%s",
               fn->nodiscard_msg ? ": " : "", fn->nodiscard_msg ? fn->nodiscard_msg : "");
    return;
  }

  if (!has_side_effects(e) && e->ty->kind != TY_VOID && e->kind != ND_NOP)
    warn_tok(W_UNUSED_VALUE, e->tok, "expression result unused");
}

static Node *expr_stmt(Token **rest, Token *tok) {
  Node *e = expr(&tok, tok);
  *rest = tok_skip(tok, ";");
  check_discarded_value(e);
  return new_unary(ND_EXPR_STMT, e, e->tok);
}

static Node *stmt(Token **rest, Token *tok) {
  Attributes attrs = {};
  tok = parse_attributes(tok, &attrs);

  if (tok_equal(tok, "return"))
    return return_stmt(rest, tok);
  if (tok_equal(tok, "if"))
    return if_stmt(rest, tok);
  if (tok_equal(tok, "switch"))
    return switch_stmt(rest, tok);
  if (tok_equal(tok, "case"))
    return case_stmt(rest, tok);
  if (tok_equal(tok, "default"))
    return default_stmt(rest, tok);
  if (tok_equal(tok, "for"))
    return for_stmt(rest, tok);
  if (tok_equal(tok, "while"))
    return while_stmt(rest, tok);
  if (tok_equal(tok, "do"))
    return do_stmt(rest, tok);
  if (tok_equal(tok, "asm") || tok_equal(tok, "__asm__") || tok_equal(tok, "__asm"))
    return asm_stmt(rest, tok);

  if (tok_equal(tok, "goto")) {
    if (tok_equal(tok->next, "*"))
      error_tok(tok, "computed goto is not supported by occ");
    if (tok->next->kind != TK_IDENT)
      error_tok(tok->next, "expected identifier after 'goto'");
    Node *node = new_node(ND_GOTO, tok);
    node->label_name = tok_text(tok->next);
    node->goto_next = P.gotos;
    P.gotos = node;
    *rest = tok_skip(tok->next->next, ";");
    return node;
  }

  if (tok_equal(tok, "break") || tok_equal(tok, "continue")) {
    bool is_break = tok_equal(tok, "break");
    int target = is_break ? P.brk_label : P.cont_label;
    if (!target)
      error_tok(tok, "'%s' statement not in loop%s", is_break ? "break" : "continue",
                is_break ? " or switch statement" : " statement");
    Node *node = new_node(ND_GOTO, tok);
    node->label_id = target;
    *rest = tok_skip(tok->next, ";");
    return node;
  }

  if (tok->kind == TK_IDENT && tok_equal(tok->next, ":")) {
    Node *node = new_node(ND_LABEL, tok);
    node->label_name = tok_text(tok);
    for (Node *l = P.labels; l; l = l->goto_next) {
      if (strcmp(l->label_name, node->label_name) == 0) {
        error_tok_nofatal(tok, "redefinition of label '%s'", node->label_name);
        note_tok(l->tok, "previous definition is here");
        diag_abort();
      }
    }
    node->label_id = new_label();
    node->goto_next = P.labels;
    P.labels = node;
    Attributes label_attrs = {};
    tok = parse_attributes(tok->next->next, &label_attrs);
    node->lhs = labeled_body(rest, tok);
    return node;
  }

  if (tok_equal(tok, "{"))
    return compound_stmt(rest, tok->next);

  if (tok_equal(tok, ";")) {
    *rest = tok->next;
    return empty_block(tok);
  }

  if (is_typename(tok))
    error_tok(tok, "a declaration is not a statement; wrap it in braces");

  return expr_stmt(rest, tok);
}

Node *compound_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_BLOCK, tok);
  Node head = {};
  Node *cur = &head;

  enter_scope();
  while (!tok_equal(tok, "}")) {
    if (tok->kind == TK_EOF)
      error_tok(tok, "expected '}' before end of file");

    if (tok_equal(tok, "static_assert") || tok_equal(tok, "_Static_assert")) {
      tok = static_assertion(tok);
      continue;
    }

    Attributes attrs = {};
    Token *attr_start = tok;
    tok = parse_attributes(tok, &attrs);
    if (is_typename(tok) && !tok_equal(tok->next, ":")) {
      VarAttr attr = {.attrs = attrs};
      Type *basety = declspec(&tok, tok, &attr);
      if (tok_consume(&tok, tok, ";"))
        continue; // struct/union/enum declaration
      cur = cur->next = declaration(&tok, tok, basety, &attr);
      continue;
    }
    cur = cur->next = stmt(&tok, attr_start);
  }
  leave_scope();

  node->body = head.next;
  *rest = tok->next;
  return node;
}

// ---------------------------------------------------------------------------
// Translation unit
// ---------------------------------------------------------------------------

static void finish_translation_unit(Program *prog) {
  for (Obj *var = P.globals; var; var = var->next) {
    bool from_user = var->tok && var->tok->file && !var->tok->file->is_system;
    if (var->is_function) {
      prog->functions += var->is_definition && from_user;
      if (var->is_static && var->is_definition && !var->is_referenced && !var->is_inline)
        warn_tok(W_UNUSED_FUNCTION, var->tok, "unused function '%s'", var->name);
      if (var->is_static && !var->is_definition && var->is_referenced)
        warn_tok(W_UNUSED_FUNCTION, var->tok, "function '%s' has internal linkage but is not defined",
                 var->name);
      continue;
    }
    if (!var->is_definition)
      continue;
    prog->variables += from_user && var->name[0] != '.';
    // A tentative array of unknown size becomes a one-element array (C23 6.9.2).
    if (var->ty->kind == TY_ARRAY && var->ty->array_len < 0) {
      var->ty = array_of(var->ty->base, 1);
      var->align = MAX(var->align, var->ty->align);
    }
    if (var->is_static && var->tok && !var->is_referenced && !var->maybe_unused && !var->ty->is_const &&
        !strchr(var->name, '.'))
      warn_tok(W_UNUSED_VARIABLE, var->tok, "unused variable '%s'", var->name);
  }
}

Program *parse(Token *tok) {
  scope_reset();
  P = (ParserState){};
  Program *prog = NEW(Program);
  P.prog = prog;

  while (tok->kind != TK_EOF)
    tok = global_declaration(tok);

  prog->globals = P.globals;
  finish_translation_unit(prog);
  return prog;
}
