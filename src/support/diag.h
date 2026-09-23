// diag.h - diagnostics: errors, warnings and notes with source excerpts.
//
// Diagnostics are buffered while a pipeline stage runs and flushed by the
// driver right after the stage's status line, so the report reads top-down.
// A fatal error unwinds to the recovery point registered by the driver.
#pragma once

#include <setjmp.h>

#include "support/source.h"

typedef enum { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE } DiagLevel;

// X(id, flag-name, description)
#define WARNING_LIST(X)                                                                            \
  X(W_UNUSED_VARIABLE, "unused-variable", "local variable is never used")                          \
  X(W_UNUSED_FUNCTION, "unused-function", "static function is never used")                         \
  X(W_UNUSED_VALUE, "unused-value", "expression result is computed and discarded")                 \
  X(W_UNUSED_RESULT, "unused-result", "result of a [[nodiscard]] function is ignored")             \
  X(W_UNINITIALIZED, "uninitialized", "variable is read but never written (UB)")                   \
  X(W_PARENTHESES, "parentheses", "assignment used as a truth value")                              \
  X(W_EMPTY_BODY, "empty-body", "empty body of if/while/for (stray semicolon)")                    \
  X(W_DIV_BY_ZERO, "div-by-zero", "integer division by constant zero (UB)")                        \
  X(W_SHIFT_COUNT, "shift-count", "shift count negative or >= width of type (UB)")                 \
  X(W_OVERFLOW, "overflow", "constant overflows or changes value on conversion")                   \
  X(W_ARRAY_BOUNDS, "array-bounds", "constant index outside of array bounds (UB)")                 \
  X(W_NULL_DEREFERENCE, "null-dereference", "dereference of a null pointer constant (UB)")         \
  X(W_RETURN_LOCAL_ADDR, "return-local-addr", "returning address of a local variable (UB)")        \
  X(W_RETURN_TYPE, "return-type", "control reaches end of non-void function (UB)")                 \
  X(W_FORMAT, "format", "printf/scanf format does not match arguments (UB)")                       \
  X(W_SIGN_COMPARE, "sign-compare", "comparison between signed and unsigned integers")             \
  X(W_INT_CONVERSION, "int-conversion", "implicit conversion between pointer and integer")         \
  X(W_INCOMPATIBLE_POINTER, "incompatible-pointer-types", "implicit conversion between "           \
                                                          "incompatible pointer types")            \
  X(W_DISCARDED_QUALIFIERS, "discarded-qualifiers", "implicit conversion drops const/volatile")    \
  X(W_STRING_COMPARE, "string-compare", "comparing against a string literal with == or !=")        \
  X(W_SIZEOF_ARRAY_ARGUMENT, "sizeof-array-argument", "sizeof applied to an array parameter")      \
  X(W_DEPRECATED, "deprecated", "use of deprecated, obsolescent or unsafe features")               \
  X(W_MULTICHAR, "multichar", "multi-character character constant")                                \
  X(W_MAIN, "main", "suspicious declaration of main")                                              \
  X(W_MACRO_REDEFINED, "macro-redefined", "macro redefined with a different body")                 \
  X(W_CPP, "cpp", "#warning directive")                                                            \
  X(W_UNSUPPORTED, "unsupported", "feature accepted but only approximated by occ")

typedef enum {
#define X(id, name, desc) id,
  WARNING_LIST(X)
#undef X
  W_COUNT,
  W_NONE = W_COUNT, // used for errors and notes
} WarningId;

// A location inside a source file.
typedef struct {
  SourceFile *file;
  const char *pos; // points into file->contents
  int len;         // length of the highlighted range
  int line;        // presumed line number (after #line)
} SrcLoc;

void diag_configure(bool color, bool werror, bool suppress_warnings);
bool diag_use_color(void);
// Enables or disables a warning by its flag name (e.g. "unused-variable").
bool diag_set_warning(const char *name, bool enabled);
bool diag_warning_enabled(WarningId id);
void diag_print_warning_list(FILE *out);

// Low-level reporting entry point. loc may be nullptr for location-less messages.
// Returns false if the diagnostic was suppressed.
bool diag_vreport(DiagLevel level, WarningId wid, const SrcLoc *loc, const char *fmt, va_list ap);
[[gnu::format(printf, 4, 5)]] bool diag_report(DiagLevel level, WarningId wid, const SrcLoc *loc,
                                               const char *fmt, ...);

// Location-less error that aborts the current stage.
[[noreturn, gnu::format(printf, 1, 2)]] void fatal(const char *fmt, ...);

// Unwinds to the active recovery point (or exits if there is none).
[[noreturn]] void diag_abort(void);
void diag_set_recovery(jmp_buf *jb);

int diag_error_count(void);
int diag_warning_count(void);
// Writes buffered diagnostics to stderr.
void diag_flush(void);
