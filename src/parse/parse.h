// parse.h - the parser and semantic analysis.
#pragma once

#include "parse/ast.h"

// Parses a preprocessed translation unit into a typed AST.
Program *parse(Token *tok);

// Whether evaluating the expression can have an observable effect.
bool has_side_effects(Node *node);

// Constant evaluation, also used by the optimizer.
bool is_const_expr(Node *node);
int64_t eval_int(Node *node);
double eval_double(Node *node);
