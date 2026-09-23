// toolchain.h - the external assembler and linker, and process helpers.
#pragma once

#include "support/timer.h"
#include "support/vec.h"

typedef struct {
  int exit_code; // -1 if the process could not be started or was killed
  Duration time; // CPU time of the child (user + system) and wall time
  char *output;  // captured stdout + stderr
} ProcResult;

// Runs a program (searched in PATH), capturing its output.
bool run_process(char *const *argv, ProcResult *res);
char *join_argv(char *const *argv);

// Builds the command line to assemble `src` into `obj`.
StrVec assemble_command(const char *src, const char *obj);

typedef struct {
  StrVec objects;
  StrVec libs;
  StrVec lib_dirs;
  const char *output;
  bool is_static;
  bool export_dynamic; // -rdynamic: symbols visible to dlopen()ed libraries
} LinkJob;

// Builds the linker command line. Uses ld directly with the C runtime
// start files; falls back to the system C compiler driver if they are not
// found. *driver_name receives "ld" or "cc" for display.
StrVec link_command(const LinkJob *job, const char **driver_name);

// Directory with occ's own headers (stddef.h, stdarg.h, ...).
char *find_occ_include_dir(void);
// Existing system header directories in search order.
StrVec system_include_dirs(void);

char *make_temp_dir(void);
void remove_temp_dir(const char *dir);
long file_size(const char *path);
