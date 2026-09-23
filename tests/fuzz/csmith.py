#!/usr/bin/env python3
"""Differential testing with Csmith-generated programs.

    tests/fuzz/csmith.py [--count N] [--seed S] [--csmith PATH] [--occ ./occ] [--ref gcc]

Csmith (https://github.com/csmith-project/csmith) generates large random C
programs that are free of undefined behavior and print a checksum of their
global state. Each program is built with the reference compiler and with occ
at -o none and -o prod; any difference in output is a miscompilation, and the
program is saved to tests/out/csmith-fail-<seed>-<mode>.c.

Programs the reference build cannot finish within the time limit are skipped
(Csmith occasionally generates very long-running loops).
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

TIMEOUT = 10  # seconds per program run


def find_include(csmith):
    """csmith.h lives in <prefix>/include/csmith next to <prefix>/bin/csmith."""
    prefix = os.path.dirname(os.path.dirname(os.path.realpath(csmith)))
    for d in (os.path.join(prefix, "include", "csmith"), "/usr/include/csmith", "/usr/local/include/csmith"):
        if os.path.exists(os.path.join(d, "csmith.h")):
            return d
    sys.exit("csmith.py: cannot find csmith.h (use --include)")


def run(cmd, timeout=60, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--csmith", default=shutil.which("csmith") or "csmith")
    ap.add_argument("--include", default=None, help="directory containing csmith.h")
    ap.add_argument("--occ", default="./occ")
    ap.add_argument("--ref", default="gcc")
    ap.add_argument("--modes", default="none,prod")
    args = ap.parse_args()

    if not shutil.which(args.csmith) and not os.path.exists(args.csmith):
        sys.exit("csmith.py: csmith not found (apt install csmith, or pass --csmith)")
    include = args.include or find_include(args.csmith)
    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    print(f"csmith differential testing, first seed {seed}")

    failures = skipped = 0
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "p.c")
        for n in range(args.count):
            s = seed + n
            gen = run([args.csmith, "--seed", str(s), "--output", src], cwd=tmp)  # it writes platform.info
            if gen is None or gen.returncode != 0:
                print(f"[{s}] csmith failed")
                continue

            ref_exe = os.path.join(tmp, "ref")
            r = run([args.ref, "-w", "-O0", "-I", include, "-o", ref_exe, src])
            if r is None or r.returncode != 0:
                print(f"[{s}] reference compiler failed")
                continue
            ref = run([ref_exe], timeout=TIMEOUT)
            if ref is None:
                skipped += 1
                continue

            for mode in args.modes.split(","):
                exe = os.path.join(tmp, f"occ-{mode}")
                r = run([args.occ, "-q", "-w", "-o", mode, "-I", include, "-n", exe, src], timeout=120)
                if r is None:
                    got = "occ timed out"
                elif r.returncode != 0:
                    got = r.stderr
                else:
                    out = run([exe], timeout=TIMEOUT * 3)
                    got = "program timed out" if out is None else out.stdout
                if got != ref.stdout:
                    failures += 1
                    os.makedirs("tests/out", exist_ok=True)
                    keep = f"tests/out/csmith-fail-{s}-{mode}.c"
                    shutil.copy(src, keep)
                    detail = got.strip().splitlines()[:3] if got else ["(no output)"]
                    print(f"[{s}] MISMATCH at -o {mode}: saved {keep}")
                    for line in detail:
                        print(f"      {line}")
                    break
            if (n + 1) % 10 == 0:
                print(f"  {n + 1} programs, {failures} failures, {skipped} skipped")
    print(f"done: {args.count} programs, {failures} failures, {skipped} skipped (reference timed out)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
