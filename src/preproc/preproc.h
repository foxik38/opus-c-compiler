// preproc.h - the C preprocessor (translation phases 3-6).
#pragma once

#include "preproc/token.h"
#include "support/vec.h"

typedef struct {
  StrVec quote_dirs;  // -iquote: searched only by #include "..."
  StrVec user_dirs;   // -I
  StrVec system_dirs; // occ's own headers, then the host's system headers
  StrVec defines;     // -D NAME[=VALUE]
  StrVec undefines;   // -U NAME
  bool optimize;      // defines __OPTIMIZE__
} PreprocOptions;

typedef struct {
  int tokens;       // tokens handed to the parser
  int files;        // distinct files read
  int expansions;   // macro expansions performed
} PreprocStats;

// Resets all preprocessor state (macros, include cache) for a new translation unit.
void preproc_init(const PreprocOptions *opts);
// Preprocesses a file (translation phases 1-4) and returns the tokens.
Token *preprocess_file(const char *path, PreprocStats *stats);
// Makes preprocessed tokens parser-ready (phases 5-7): recognizes keywords,
// converts numbers and concatenates adjacent string literals.
void preproc_finalize(Token *tok);
// Writes preprocessed tokens as text (the -E output).
void print_tokens(FILE *out, Token *tok);
