# Contributing to occ

Thanks for your interest! Bug reports are the most valuable contribution: a C program that occ
compiles differently from GCC is exactly what the project wants to know about.

## Reporting a bug

Open an issue with the **Bug report** form and include:

- the smallest C program that shows the problem (ideally without undefined behavior —
  `gcc -fsanitize=undefined` is a quick check),
- the occ command line and the optimization level (`-o none` or `-o prod`),
- what occ printed or produced, and what GCC or Clang does instead,
- your distribution and `occ --version`.

## Making a change

```sh
make                          # build ./occ
make test                     # every runtime and diagnostic test, at -o none and -o prod
tests/run.sh --reference      # check that the tests themselves agree with GCC
make selfhost                 # occ must still compile itself to an identical fixpoint
make fuzz FUZZ_COUNT=200      # differential fuzzing against GCC
```

All of these must pass before a pull request is merged; CI runs them too.

Guidelines:

- **Every bug fix comes with a regression test** in `tests/cases` (runtime behavior, validated
  with `tests/run.sh --reference`) or `tests/diag` (diagnostics). The diagnostic tests are strict:
  every warning occ prints must be expected by an `expect-warning` marker on its line.
- **Match the surrounding code**: C23, two-space indentation, comments that explain *why*.
  The source is organized by pipeline stage (`src/preproc`, `src/parse`, `src/opt`,
  `src/codegen`, `src/driver`); `THINKPROC.md` explains the design.
- **Keep changes focused.** One fix or feature per pull request, with a commit message that says
  what changed and why.

## Scope

occ targets x86-64 Linux with glibc, by design. Features on the "not yet" list in the README
(VLAs, `_BitInt`, `_Complex`, extended `asm`) are welcome if they come with tests.
