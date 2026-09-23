// pipeline.h - the compilation pipeline: preprocess, compile, assemble, link.
#pragma once

#include "driver/cli.h"

// Runs the whole pipeline; returns the process exit status.
int run_pipeline(Options *opts);
