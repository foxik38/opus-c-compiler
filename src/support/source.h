// source.h - source files loaded into memory.
#pragma once

#include "support/common.h"

typedef struct SourceFile {
  char *path;         // path the file was opened with
  char *display_name; // name used in diagnostics and __FILE__ (#line may change it)
  char *contents;     // NUL-terminated, line continuations already spliced
  int id;
  bool is_system;     // found in a system include directory: warnings suppressed
  int include_index;  // index in the include search list it was found at (-1: none)
  int depth;          // #include nesting depth of the most recent inclusion
} SourceFile;

SourceFile *source_new(const char *path, char *contents);
// Reads a whole file; "-" means stdin. Returns nullptr on failure (errno set).
char *read_file(const char *path, size_t *out_len);
int source_count(void);
