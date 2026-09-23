// main.c - occ, the Opus C Compiler.
#include "driver/cli.h"
#include "driver/pipeline.h"

int main(int argc, char **argv) {
  Options opts = parse_args(argc, argv);
  return run_pipeline(&opts);
}
