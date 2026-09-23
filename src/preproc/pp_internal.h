// pp_internal.h - interfaces shared between the preprocessor's source files.
#pragma once

#include "preproc/preproc.h"

extern PreprocStats pp_stats;

// Token list utilities (macro.c).
Token *pp_append(Token *a, Token *b);
Token *pp_copy_line(Token **rest, Token *tok);
Token *pp_tokenize_text(const char *text, const Token *tmpl);
Token *pp_new_str_token(const char *str, const Token *tmpl);

// Macros (macro.c).
void macro_reset(const PreprocOptions *opts);
Token *macro_define(Token *tok);
void macro_undef(Token *name);
bool macro_is_defined(const Token *name);
bool macro_try_expand(Token **rest, Token *tok);
// Fully macro-expands an EOF-terminated list (no directives).
Token *pp_expand(Token *tok);
void macro_set_file_hooks(char *(*base_file)(void));

// Directives (preprocess.c).
bool pp_has_include(Token *tok, Token **rest);
bool pp_has_embed_resource(Token *tok, Token **rest, int *status);

// #if expression evaluation (pp_eval.c).
bool pp_eval_condition(Token *line, const Token *directive);
