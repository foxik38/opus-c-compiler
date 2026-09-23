// cli.h - command-line options of the occ driver.
#pragma once

#include "support/vec.h"

typedef enum { OPT_NONE, OPT_PROD } OptLevel;

// Where the pipeline stops.
typedef enum {
  STOP_PREPROCESS, // -E
  STOP_ASSEMBLY,   // -S
  STOP_OBJECT,     // -c
  STOP_EXECUTABLE,
} StopAfter;

typedef struct {
  OptLevel opt;
  StopAfter stop;
  char *name;       // -n: output name (default derived from the first input)
  StrVec inputs;    // .c / .s / .o / .a files
  StrVec include_dirs;
  StrVec quote_dirs;
  StrVec defines;
  StrVec undefines;
  StrVec libs;      // -l
  StrVec lib_dirs;  // -L
  StrVec run_args;  // arguments after "--" for --run
  bool quiet;       // -q: no pipeline status, diagnostics only
  bool verbose;     // -v: show external commands
  bool keep;        // --keep: keep intermediate files next to the output
  bool run;         // --run: execute the program after building it
  bool color;
  bool werror;
  bool no_warnings; // -w
  bool is_static;   // --static
  bool export_dynamic; // -rdynamic
} Options;

// Parses argv; exits with a message on invalid usage.
Options parse_args(int argc, char **argv);
