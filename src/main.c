// main.c - occ, the C23 compiler.
#include "driver/cli.h"
#include "driver/pipeline.h"

int main(int argc, char **argv) {
  Options opts = parse_args(argc, argv);
  return run_pipeline(&opts);
}
