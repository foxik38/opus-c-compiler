// licm.c - loop-invariant code motion (-o prod).
//
// Address and integer arithmetic that does not change while a loop runs is
// computed once before the loop into a fresh temporary:
//
//   for (j = 0; j < n; j++)          t = &c[i];
//     c[i][j] += x;          =>      for (j = 0; j < n; j++)
//                                      t[j] += x;
//
// An expression is invariant if it only combines constants, addresses of
// variables and locals that the loop never writes (and whose address is never
// taken). Only operations that cannot trap are moved (no division, no memory
// reads), so evaluating them when the loop body would not have run is safe.
#include "opt/opt.h"
#include "parse/parse.h"
#include "support/vec.h"

typedef VEC(Obj *) ObjList;

typedef struct {
  Obj *fn;
  OptStats *stats;
} Licm;

static bool contains(const ObjList *l, const Obj *v) {
  for (size_t i = 0; i < l->len; i++)
    if (l->data[i] == v)
      return true;
  return false;
}

// Collects the variables a subtree may write (assigned, or address taken).
static void collect_writes(Node *n, ObjList *w) {
  if (!n)
    return;
  if ((n->kind == ND_ASSIGN || n->kind == ND_ADDR) && n->lhs->kind == ND_VAR && !contains(w, n->lhs->var))
    vec_push(w, n->lhs->var);
  collect_writes(n->lhs, w);
  collect_writes(n->rhs, w);
  collect_writes(n->cond, w);
  collect_writes(n->then, w);
  collect_writes(n->els, w);
  collect_writes(n->init, w);
  collect_writes(n->inc, w);
  for (Node *s = n->body; s; s = s->next)
    collect_writes(s, w);
  for (Node *a = n->args; a; a = a->next)
    collect_writes(a, w);
}

// A jump from outside into the loop would skip the hoisted computations.
static bool has_foreign_label(Node *n, bool in_switch) {
  if (!n)
    return false;
  if (n->kind == ND_LABEL || (n->kind == ND_CASE && !in_switch))
    return true;
  if (n->kind == ND_SWITCH)
    in_switch = true;
  for (Node *s = n->body; s; s = s->next)
    if (has_foreign_label(s, in_switch))
      return true;
  return has_foreign_label(n->then, in_switch) || has_foreign_label(n->els, in_switch) ||
         has_foreign_label(n->init, in_switch) ||
         ((n->kind == ND_CASE || n->kind == ND_LABEL) && has_foreign_label(n->lhs, in_switch));
}

static bool is_int_or_ptr(const Type *ty) { return is_integer(ty) || ty->kind == TY_PTR; }

static bool is_invariant(Node *n, const ObjList *writes) {
  switch (n->kind) {
  case ND_NUM:
    return is_int_or_ptr(n->ty);
  case ND_VAR:
    return n->var->is_local && !n->var->addr_taken && !contains(writes, n->var) && is_int_or_ptr(n->ty) &&
           !n->ty->is_volatile;
  case ND_ADDR:
    if (n->lhs->kind == ND_VAR)
      return !n->lhs->var->is_tls;
    return n->lhs->kind == ND_DEREF && is_invariant(n->lhs->lhs, writes);
  case ND_CAST:
    return is_int_or_ptr(n->ty) && is_int_or_ptr(n->lhs->ty) && is_invariant(n->lhs, writes);
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_SHL: case ND_SHR: case ND_BITAND: case ND_BITOR:
  case ND_BITXOR:
    return is_int_or_ptr(n->ty) && is_invariant(n->lhs, writes) && is_invariant(n->rhs, writes);
  default:
    return false;
  }
}

// Worth a register: an actual computation involving a variable.
static bool worth_hoisting(Node *n) {
  if (n->kind != ND_ADD && n->kind != ND_SUB && n->kind != ND_MUL && n->kind != ND_SHL)
    return false;
  if (n->ty->size != 8)
    return false; // keep int arithmetic where it is (wrap-around semantics stay local)
  return true;
}

typedef struct {
  Licm *lx;
  const ObjList *writes;
  Node head; // hoisted assignments, chained through `next`
  Node *tail;
  uint32_t weight;
} Hoist;

static Node *hoist_expr(Hoist *h, Node *n);

static void hoist_children(Hoist *h, Node *n) {
  n->lhs = hoist_expr(h, n->lhs);
  n->rhs = hoist_expr(h, n->rhs);
  n->cond = hoist_expr(h, n->cond);
  n->then = hoist_expr(h, n->then);
  n->els = hoist_expr(h, n->els);
  Node head = {.next = n->args};
  for (Node *p = &head; p->next; p = p->next) {
    Node *next = p->next->next;
    p->next = hoist_expr(h, p->next);
    p->next->next = next;
  }
  n->args = head.next;
}

static Node *hoist_expr(Hoist *h, Node *n) {
  if (!n)
    return n;
  if (worth_hoisting(n) && is_invariant(n, h->writes)) {
    Obj *tmp = NEW(Obj);
    tmp->name = "";
    tmp->ty = n->ty;
    tmp->align = n->ty->align;
    tmp->reg = -1;
    tmp->is_local = true;
    tmp->weight = h->weight;
    tmp->next = h->lx->fn->locals;
    h->lx->fn->locals = tmp;

    Node *var = NEW(Node);
    var->kind = ND_VAR;
    var->tok = n->tok;
    var->var = tmp;
    var->ty = n->ty;

    Node *asg = NEW(Node);
    asg->kind = ND_ASSIGN;
    asg->tok = n->tok;
    asg->lhs = var;
    asg->rhs = n;
    asg->ty = n->ty;
    Node *stmt = NEW(Node);
    stmt->kind = ND_EXPR_STMT;
    stmt->tok = n->tok;
    stmt->lhs = asg;
    h->tail = h->tail->next = stmt;
    h->lx->stats->hoisted++;

    Node *use = NEW(Node);
    *use = *var;
    return use;
  }
  if (n->kind == ND_COMMA) {
    for (Node *c = n; c->kind == ND_COMMA; c = c->rhs) {
      c->lhs = hoist_expr(h, c->lhs);
      if (c->rhs->kind != ND_COMMA)
        c->rhs = hoist_expr(h, c->rhs);
    }
    return n;
  }
  hoist_children(h, n);
  return n;
}

static void hoist_stmt(Hoist *h, Node *n) {
  for (; n; n = n->next) {
    switch (n->kind) {
    case ND_EXPR_STMT: case ND_RETURN:
      n->lhs = hoist_expr(h, n->lhs);
      break;
    case ND_IF:
      n->cond = hoist_expr(h, n->cond);
      hoist_stmt(h, n->then);
      hoist_stmt(h, n->els);
      break;
    case ND_SWITCH:
      n->cond = hoist_expr(h, n->cond);
      hoist_stmt(h, n->then);
      break;
    case ND_FOR:
      hoist_stmt(h, n->init);
      n->cond = hoist_expr(h, n->cond);
      n->inc = hoist_expr(h, n->inc);
      hoist_stmt(h, n->then);
      break;
    case ND_DO:
      n->cond = hoist_expr(h, n->cond);
      hoist_stmt(h, n->then);
      break;
    case ND_BLOCK:
      hoist_stmt(h, n->body);
      break;
    case ND_CASE: case ND_LABEL:
      hoist_stmt(h, n->lhs);
      break;
    default:
      break;
    }
  }
}

static Node *process_stmt(Licm *lx, Node *n, int depth);

static void process_list(Licm *lx, Node **list, int depth) {
  Node head = {.next = *list};
  for (Node *p = &head; p->next; p = p->next) {
    Node *next = p->next->next;
    p->next = process_stmt(lx, p->next, depth);
    p->next->next = next;
  }
  *list = head.next;
}

// Returns the (possibly replaced) statement.
static Node *process_stmt(Licm *lx, Node *n, int depth) {
  switch (n->kind) {
  case ND_BLOCK:
    process_list(lx, &n->body, depth);
    return n;
  case ND_IF:
    n->then = process_stmt(lx, n->then, depth);
    if (n->els)
      n->els = process_stmt(lx, n->els, depth);
    return n;
  case ND_SWITCH:
    n->then = process_stmt(lx, n->then, depth);
    return n;
  case ND_CASE: case ND_LABEL:
    n->lhs = process_stmt(lx, n->lhs, depth);
    return n;
  case ND_FOR: case ND_DO:
    break;
  default:
    return n;
  }

  // Inner loops first: what they hoist may be invariant here as well.
  n->then = process_stmt(lx, n->then, depth + 1);
  if (has_foreign_label(n->then, false))
    return n;

  ObjList writes = {};
  collect_writes(n->then, &writes);
  collect_writes(n->cond, &writes);
  collect_writes(n->inc, &writes);

  Hoist h = {.lx = lx, .writes = &writes, .weight = 2u << MIN(3 * (depth + 1), 24)};
  h.tail = &h.head;
  n->cond = hoist_expr(&h, n->cond);
  n->inc = hoist_expr(&h, n->inc);
  hoist_stmt(&h, n->then);
  free(writes.data);
  if (!h.head.next)
    return n;

  // Result: { init; hoisted...; for (; cond; inc) body }
  Node *block = NEW(Node);
  block->kind = ND_BLOCK;
  block->tok = n->tok;
  Node *first = h.head.next;
  if (n->kind == ND_FOR && n->init) {
    Node *init = n->init;
    n->init = nullptr;
    init->next = first;
    first = init;
  }
  h.tail->next = n;
  n->next = nullptr; // the caller relinks the block into the statement list
  block->body = first;
  return block;
}

void hoist_loop_invariants(Program *prog, OptStats *stats) {
  for (Obj *fn = prog->globals; fn; fn = fn->next) {
    if (!fn->is_function || !fn->body)
      continue;
    Licm lx = {.fn = fn, .stats = stats};
    fn->body = process_stmt(&lx, fn->body, 0);
  }
}
