# Security policy

occ is a compiler: it reads source code and runs the system assembler and linker. Please report
problems that could matter for security privately, through GitHub's
[private vulnerability reporting](https://github.com/foxik38/opus-c-compiler/security/advisories/new),
rather than in a public issue. That includes:

- miscompilation that silently removes or changes a security check in correct C code,
- crashes or memory corruption in occ itself when compiling untrusted input,
- unsafe handling of temporary files or of the commands occ runs.

Please include a minimal reproducer and the occ version (`occ --version`).
Only the latest version on `main` receives fixes.
