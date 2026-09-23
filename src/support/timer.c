// timer.c - stopwatch built on clock_gettime.
#include "support/timer.h"

#include <time.h>

static double clock_ms(clockid_t id) {
  struct timespec ts;
  clock_gettime(id, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

Stopwatch stopwatch_start(void) {
  return (Stopwatch){
      .cpu_start = clock_ms(CLOCK_PROCESS_CPUTIME_ID),
      .wall_start = clock_ms(CLOCK_MONOTONIC),
  };
}

Duration stopwatch_elapsed(const Stopwatch *sw) {
  return (Duration){
      .cpu_ms = clock_ms(CLOCK_PROCESS_CPUTIME_ID) - sw->cpu_start,
      .wall_ms = clock_ms(CLOCK_MONOTONIC) - sw->wall_start,
  };
}
