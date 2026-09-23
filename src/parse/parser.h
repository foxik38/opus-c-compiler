// parser.h - state and interfaces shared by the parser's source files.
//
//   scope.c       symbol tables and object creation
//   decl.c        declarations, declarators, struct/union/enum, functions
//   init.c        initializers for locals and globals
//   expr.c        expressions and their semantic rules
//   stmt.c        statements
//   const_eval.c  constant expressions
//   check.c       diagnostics that need whole-construct analysis
#pragma once

#include "parse/parse.h"
#include "support/hashmap.h"
#include "support/vec.h"

typedef VEC(Obj *) ObjVec;

// ---------------------------------------------------------------------------
// Scopes (scope.c)
// ---------------------------------------------------------------------------

// What an ordinary identifier denotes.
typedef struct {
  Obj *var;
  Type *type_def;
  Type *enum_ty;
  int64_t enum_val;
  Token *tok; // declaration, for "previous declaration" notes
} VarScope;

typedef struct Scope Scope;
struct Scope {
  Scope *next;
  HashMap vars; // ordinary identifiers
  HashMap tags; // struct/union/enum tags
  ObjVec locals; // variables declared here, in order (for -Wunused-variable)
};

void scope_reset(void);
void enter_scope(void);
void leave_scope(void);
bool at_file_scope(void);
VarScope *push_scope(char *name);
VarScope *find_var(const Token *tok);
VarScope *find_var_in_current_scope(const Token *tok);
Type *find_tag(const Token *tok);
Type *find_tag_in_current_scope(const Token *tok);
void push_tag(const Token *tok, Type *ty);

Obj *new_lvar(char *name, Type *ty, Token *tok);
Obj *new_gvar(char *name, Type *ty);
Obj *new_anon_gvar(Type *ty);
Obj *new_temp(Type *ty); // compiler-generated local
Node *new_string_literal(Token *tok);
char *unique_name(const char *prefix);
int new_label(void);

// ---------------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------------

// Attributes collected from [[...]] and __attribute__((...)).
typedef struct {
  bool deprecated;
  char *deprecated_msg;
  bool nodiscard;
  char *nodiscard_msg;
  bool maybe_unused;
  bool noreturn;
  bool fallthrough;
  bool packed;
  int aligned;
} Attributes;

// Storage class, function specifiers and attributes of a declaration.
typedef struct {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tls;
  bool is_constexpr;
  bool is_register;
  bool is_auto;     // C23 type inference: auto x = expr;
  bool is_noreturn; // _Noreturn
  int align;
  Attributes attrs;
} VarAttr;

typedef struct {
  Obj *globals;
  Obj *current_fn;
  Node *gotos;  // unresolved gotos of the current function
  Node *labels; // labels of the current function
  int brk_label;
  int cont_label;
  Node *current_switch;
  int loop_depth;  // for register-promotion weights
  int unevaluated; // inside sizeof/typeof/alignof/_Generic association
  int addr_of;     // parsing the operand of unary &
  Program *prog;
} ParserState;

extern ParserState P;

// ---------------------------------------------------------------------------
// Declarations (decl.c)
// ---------------------------------------------------------------------------

// What a declarator declared besides its type.
typedef struct {
  Token *name;     // nullptr for abstract declarators
  Token *name_pos; // where the name is (or would be)
  Attributes attrs;
} DeclInfo;

bool is_typename(Token *tok);
Type *declspec(Token **rest, Token *tok, VarAttr *attr);
// info == nullptr parses an abstract declarator (no identifier allowed).
Type *declarator(Token **rest, Token *tok, Type *ty, DeclInfo *info);
Type *typename_(Token **rest, Token *tok);
Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
Token *parse_attributes(Token *tok, Attributes *attrs);
Token *skip_gnu_attributes(Token *tok, Attributes *attrs);
Token *static_assertion(Token *tok);
Token *global_declaration(Token *tok);

// ---------------------------------------------------------------------------
// Initializers (init.c)
// ---------------------------------------------------------------------------

Node *lvar_initializer(Token **rest, Token *tok, Obj *var);
void gvar_initializer(Token **rest, Token *tok, Obj *var);
// Type of a declaration with an incomplete array type, completed by its initializer.
Type *initializer_type(Token *tok, Type *ty);

// ---------------------------------------------------------------------------
// Expressions (expr.c)
// ---------------------------------------------------------------------------

Node *expr(Token **rest, Token *tok);
Node *assign(Token **rest, Token *tok);
void note_discarded(Node *e);
Node *conditional(Token **rest, Token *tok);
int64_t const_expr(Token **rest, Token *tok);

Node *new_node(NodeKind kind, Token *tok);
Node *new_unary(NodeKind kind, Node *expr, Token *tok);
Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok);
Node *new_num(int64_t val, Type *ty, Token *tok);
Node *new_var_node(Obj *var, Token *tok);
Node *new_cast(Node *expr, Type *ty);
Node *rvalue(Node *node);
Node *to_condition(Node *node, Token *tok); // scalar check + -Wparentheses
Node *convert_for_assign(Node *expr, Type *ty, Token *tok, const char *context);
Node *new_assign(Node *lhs, Node *rhs, Token *tok);
Node *new_add(Node *lhs, Node *rhs, Token *tok);
Node *new_deref(Node *expr, Token *tok);
Node *member_access(Node *lhs, Token *name);
bool is_null_pointer_constant(Node *node);
bool is_lvalue(Node *node);
bool has_side_effects(Node *node);

// ---------------------------------------------------------------------------
// Statements (stmt.c)
// ---------------------------------------------------------------------------

Node *compound_stmt(Token **rest, Token *tok);

// ---------------------------------------------------------------------------
// Constants (const_eval.c)
// ---------------------------------------------------------------------------

// Evaluates a constant that may be an address: result = &label + value.
int64_t eval_reloc(Node *node, char **label);

// ---------------------------------------------------------------------------
// Checks (check.c)
// ---------------------------------------------------------------------------

// arg_toks[i]: first token of argument i, where format warnings point.
void check_format_call(Node *call, const char *name, Token **arg_toks);
bool stmt_falls_through(Node *node);
bool is_noreturn_call(Node *node);
void check_unused_locals(Scope *scope);
