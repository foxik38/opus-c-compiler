// report.h - the pipeline status display.
//
//   occ 1.0.0 · x86-64 Linux · C23 · opt=prod
//   hello.c
//     preprocess   OK     0.61 ms cpu    0.63 ms wall   63 files, 6382 tokens
//     compile      OK     ...
//       ├ parse           1.20 ms
//       └ codegen         0.62 ms
//   ✓ built ./hello in 11.6 ms
#pragma once

#include "support/timer.h"

typedef enum { STATUS_OK, STATUS_WARN, STATUS_ERR } StageStatus;

void report_init(bool enabled, bool color, bool animate);
bool report_enabled(void);
void report_header(const char *opt_level, const char *target);
void report_file(const char *path);
void report_stage_begin(const char *name); // live spinner until report_stage()
void report_stage(const char *name, StageStatus status, Duration time, const char *detail);
void report_substage(const char *name, bool last, Duration time, const char *detail);
void report_command(const char *cmdline); // -v
void report_summary(bool ok, const char *what, Duration total, int errors, int warnings);
void report_note(const char *fmt, ...);
char *format_size(long bytes);
