// ast.h - types, symbols and the abstract syntax tree.
//
// The parser produces a fully typed AST: every expression node carries its
// C type and implicit conversions are explicit ND_CAST nodes. Arrays and
// functions used as values are wrapped in ND_ADDR (decay), so later stages
// never have to reason about lvalue conversion.
#pragma once

#include "preproc/token.h"

typedef struct Type Type;
typedef struct Member Member;
typedef struct Node Node;
typedef struct Obj Obj;
typedef struct Reloc Reloc;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef enum : uint8_t {
  TY_VOID,
  TY_BOOL,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_LLONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE, // represented like double (see README: limitations)
  TY_ENUM,
  TY_NULLPTR, // nullptr_t
  TY_PTR,
  TY_ARRAY,
  TY_FUNC,
  TY_STRUCT,
  TY_UNION,
} TypeKind;

struct Type {
  TypeKind kind;
  bool is_unsigned;
  bool is_const;
  bool is_volatile;
  bool is_restrict;
  bool is_atomic;
  int size;  // sizeof; negative while incomplete
  int align; // alignof

  // Unqualified original of a qualified copy (structs: the canonical type).
  Type *origin;

  // Pointer/array element type; underlying integer type of an enum.
  Type *base;
  int array_len; // -1 for an array of unknown size

  // Declarator information.
  Token *name;
  Token *name_pos;

  // Struct/union.
  Member *members;
  Token *tag;
  bool is_flexible; // ends in a flexible array member
  bool is_packed;
  Type *next_copy; // qualified copies, updated when the type is completed

  // Enum.
  bool is_fixed_enum; // enum with a C23 fixed underlying type
  bool is_defined;    // enumerator list seen

  // Function.
  Type *return_ty;
  Type *params; // linked through `next`
  bool is_variadic;
  Type *next;
  bool param_was_array; // parameter written with array syntax (adjusted to pointer)
};

struct Member {
  Member *next;
  Type *ty;
  Token *tok;  // for diagnostics
  Token *name; // nullptr for anonymous struct/union members and unnamed bit-fields
  int idx;
  int align;
  int offset;

  bool is_bitfield;
  int bit_offset;
  int bit_width;
};

extern Type *ty_void, *ty_bool, *ty_nullptr;
extern Type *ty_char, *ty_short, *ty_int, *ty_long, *ty_llong;
extern Type *ty_uchar, *ty_ushort, *ty_uint, *ty_ulong, *ty_ullong;
extern Type *ty_float, *ty_double, *ty_ldouble;

bool is_integer(const Type *ty);
bool is_flonum(const Type *ty);
bool is_numeric(const Type *ty);
bool is_scalar(const Type *ty);
bool is_pointer_like(const Type *ty); // pointer or nullptr_t
bool is_aggregate(const Type *ty);    // struct, union, array
bool is_complete(const Type *ty);
bool is_signed_integer(const Type *ty);

Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *array_of(Type *base, int len);
Type *func_type(Type *return_ty);
Type *enum_type(void);
Type *struct_type(TypeKind kind);
Type *qualified(Type *ty, bool is_const, bool is_volatile);
Type *unqualified(Type *ty);
void complete_struct(Type *ty); // propagates layout to qualified copies

// Integer promotion and the usual arithmetic conversions (C23 6.3.1).
Type *integer_promote(Type *ty);
Type *common_type(Type *a, Type *b);
Type *enum_underlying(Type *ty);
int integer_rank(const Type *ty);

bool types_compatible(Type *a, Type *b);
char *type_name(const Type *ty);

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

// A relocation in a global's initializer: *(offset) = &label + addend.
struct Reloc {
  Reloc *next;
  int offset;
  char *label;
  int64_t addend;
};

// A variable or function.
struct Obj {
  Obj *next;
  char *name;
  Type *ty;
  Token *tok;
  bool is_local;
  int align;

  // Local variables.
  int offset;        // offset from %rbp
  int reg;           // callee-saved register it lives in (-1: memory)
  bool addr_taken;   // address escapes: must live in memory
  bool is_param;
  bool is_register;  // declared 'register'
  bool maybe_unused; // [[maybe_unused]]
  bool has_init;     // declared with an initializer
  int refs;          // references (for -Wunused-variable)
  int lhs_refs;      // references that were only the target of '='
  int writes;        // assignments, increments, address escapes (for -Wuninitialized)
  Token *first_read; // first evaluated read (for -Wuninitialized)
  uint32_t weight;   // loop-weighted use count, drives register promotion

  // Globals and functions.
  bool is_function;
  bool is_definition;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tentative;
  bool is_tls;
  bool is_referenced;
  bool is_constexpr;
  bool is_string_literal;
  char *init_data;
  Reloc *rel;

  // Value of a constexpr object of scalar type.
  bool has_const_value;
  int64_t const_ival;
  double const_fval;

  // Functions.
  bool is_noreturn;
  bool is_inline_def; // every declaration is 'inline' without 'extern': no external symbol
  Obj *params;        // linked through next_param
  Obj *next_param;
  Node *body;
  Obj *locals;
  Obj *va_area;   // register save area of a variadic function
  Obj *ret_ptr;   // saved hidden pointer for struct returns in memory
  int stack_size;
  Obj *func_name; // lazily created __func__

  // Attributes.
  bool is_deprecated;
  char *deprecated_msg;
  bool is_nodiscard;
  char *nodiscard_msg;
};

// ---------------------------------------------------------------------------
// Abstract syntax tree
// ---------------------------------------------------------------------------

typedef enum : uint8_t {
  // Expressions.
  ND_NUM,       // integer or floating constant
  ND_VAR,       // variable or function designator
  ND_ADD,
  ND_SUB,
  ND_MUL,
  ND_DIV,
  ND_MOD,
  ND_BITAND,
  ND_BITOR,
  ND_BITXOR,
  ND_SHL,
  ND_SHR,
  ND_EQ,
  ND_NE,
  ND_LT,
  ND_LE,
  ND_NEG,
  ND_NOT,
  ND_BITNOT,
  ND_LOGAND,
  ND_LOGOR,
  ND_ASSIGN,
  ND_COND,      // cond ? then : els
  ND_COMMA,
  ND_MEMBER,    // lhs.member
  ND_ADDR,      // &lhs (also array/function decay)
  ND_DEREF,     // *lhs
  ND_CAST,
  ND_FUNCALL,
  ND_MEMZERO,   // zero-initialize var
  ND_VA_START,  // __builtin_va_start(lhs)
  ND_VA_ARG,    // address of the next variadic argument of type ty->base
  ND_OVERFLOW,  // __builtin_{add,sub,mul}_overflow: op in `val`, result pointer in `cond`
  ND_NOP,       // expression with no effect (value 0)

  // Statements.
  ND_BLOCK,
  ND_EXPR_STMT,
  ND_IF,
  ND_FOR,       // also while
  ND_DO,
  ND_SWITCH,
  ND_CASE,
  ND_GOTO,      // also break and continue
  ND_LABEL,
  ND_RETURN,
  ND_UNREACHABLE,
  ND_ASM,       // basic asm("...") statement, text in `label_name`
} NodeKind;

struct Node {
  NodeKind kind;
  bool in_parens;   // written inside parentheses (for -Wparentheses)
  bool is_implicit; // implicit conversion inserted by the compiler
  bool is_compound_literal;
  Type *ty;
  Token *tok;
  Node *next;

  Node *lhs;
  Node *rhs;

  // Control flow.
  Node *cond;
  Node *then;
  Node *els;
  Node *init;
  Node *inc;
  int brk_label;
  int cont_label;

  Node *body; // block

  Member *member;

  // Function call.
  Type *func_ty;
  Node *args;
  Obj *ret_buffer; // temporary receiving a returned struct

  // Labels (goto targets, case labels, loop labels).
  char *label_name;
  int label_id;
  Node *goto_next;

  // Switch.
  Node *case_next;
  Node *default_case;
  int64_t case_begin;
  int64_t case_end;

  Obj *var;

  // Constants.
  int64_t val;
  double fval;
};

typedef struct {
  Obj *globals; // variables and functions, in definition order
  int functions;
  int variables;
} Program;
