// timer.h - CPU and wall-clock time measurement for pipeline stages.
#pragma once

#include "support/common.h"

typedef struct {
  double cpu_ms;  // CPU time (user + system) consumed
  double wall_ms; // elapsed real time
} Duration;

typedef struct {
  double cpu_start;
  double wall_start;
} Stopwatch;

Stopwatch stopwatch_start(void);
Duration stopwatch_elapsed(const Stopwatch *sw);
