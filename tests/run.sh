#!/usr/bin/env bash
# run.sh - occ test runner.
#
#   tests/run.sh [./occ]            run every test with occ at -o none and -o prod
#   tests/run.sh --reference [cc]   run the runtime tests with a reference compiler
#                                   (validates the tests themselves; default: gcc)
#   FILTER=struct tests/run.sh      only tests whose name contains "struct"
#
# Runtime tests (tests/cases/*.c) are self-checking programs that print "OK".
# A test may declare extra sources with "// extra: file.c" and expected
# output with a sibling "<name>.expected" file. Tests using C23 features
# the reference compiler lacks are marked "// occ-only".
#
# Diagnostic tests (tests/diag/*.c) list what occ must report:
#   // expect-warning: <text>    stderr must contain "warning: <text>"
#   // expect-error: <text>      stderr must contain "error: <text>"; build fails
#   // expect-clean              no warnings at all
# and nothing more: a warning is only accepted on a line whose expect-warning
# marker is a prefix of its message.
set -u
shopt -s nullglob

cd "$(dirname "$0")/.."
ROOT=$PWD
OUT="$ROOT/tests/out"
mkdir -p "$OUT"

REFERENCE=""
OCC="./occ"
if [[ "${1:-}" == "--reference" ]]; then
  REFERENCE="${2:-gcc}"
elif [[ -n "${1:-}" ]]; then
  OCC="$1"
fi
FILTER="${FILTER:-}"

if [[ -t 1 ]]; then
  GREEN=$'\033[32m' RED=$'\033[31m' DIM=$'\033[2m' BOLD=$'\033[1m' RESET=$'\033[0m'
else
  GREEN="" RED="" DIM="" BOLD="" RESET=""
fi

pass=0
fail=0
failed_names=()

report() { # name status detail
  if [[ "$2" == ok ]]; then
    pass=$((pass + 1))
    printf "  ${GREEN}PASS${RESET} %s\n" "$1"
  else
    fail=$((fail + 1))
    failed_names+=("$1")
    printf "  ${RED}FAIL${RESET} %s\n" "$1"
    [[ -n "${3:-}" ]] && printf "%s\n" "$3" | head -30 | sed 's/^/       /'
  fi
}

extra_sources() { # file
  sed -n 's|^// extra: *||p' "$1" | while read -r f; do printf '%s ' "$(dirname "$1")/$f"; done
}

run_case() { # file mode
  local src="$1" mode="$2" name
  name="$(basename "$src" .c)"
  local exe="$OUT/$name-$mode"
  local log
  local extra
  extra="$(extra_sources "$src")"

  if [[ -n "$REFERENCE" ]]; then
    # shellcheck disable=SC2086
    log=$("$REFERENCE" -std=c2x -w -O1 -I tests -o "$exe" "$src" $extra -lm 2>&1) || {
      report "$name [$REFERENCE]" fail "$log"
      return
    }
  else
    # shellcheck disable=SC2086
    log=$("$OCC" -q -w --no-color -o "$mode" -I tests -n "$exe" "$src" $extra 2>&1) || {
      report "$name [$mode]" fail "$log"
      return
    }
  fi

  local out status
  out=$(cd "$(dirname "$src")" && timeout 10 "$exe" arg1 arg2 2>&1)
  status=$?
  local label="$name [${REFERENCE:-$mode}]"
  if [[ $status -ne 0 ]]; then
    report "$label" fail "exit status $status"$'\n'"$out"
    return
  fi
  if [[ -f "${src%.c}.expected" ]]; then
    local diff_out
    diff_out=$(diff <(printf '%s\n' "$out") "${src%.c}.expected") || {
      report "$label" fail "$diff_out"
      return
    }
  elif [[ "$out" != *OK* ]]; then
    report "$label" fail "$out"
    return
  fi
  report "$label" ok
}

run_diag() { # file
  local src="$1" name
  name="diag/$(basename "$src" .c)"
  local log status
  log=$("$OCC" -q --no-color -S -n /dev/null "$src" 2>&1)
  status=$?

  local problems=""
  local expect_error=0
  while IFS= read -r line; do
    local kind="${line%%:*}" text="${line#*: }"
    case "$kind" in
      expect-warning) [[ "$log" == *"warning: $text"* ]] || problems+="missing warning: $text"$'\n' ;;
      expect-error)
        expect_error=1
        [[ "$log" == *"error: $text"* ]] || problems+="missing error: $text"$'\n'
        ;;
      expect-clean) [[ "$log" != *"warning:"* ]] || problems+="unexpected warnings"$'\n' ;;
    esac
  done < <(sed -n 's|^.*// \(expect-[a-z]*\(: .*\)\{0,1\}\)$|\1|p' "$src")

  # Conversely, every warning must be expected on the line it points at.
  while IFS= read -r w; do
    local wline="${w#"$src":}"
    wline="${wline%%:*}"
    local wtext="${w#*: warning: }"
    wtext="${wtext% \[-W*\]}"
    local marker
    marker=$(sed -n "${wline}s|^.*// expect-warning: ||p" "$src")
    [[ -n "$marker" && "$wtext" == "$marker"* ]] ||
      problems+="unexpected warning on line $wline: $wtext"$'\n'
  done < <(grep "^$src:[0-9]*:[0-9]*: warning: " <<<"$log")

  if [[ $expect_error -eq 1 && $status -eq 0 ]]; then
    problems+="compilation succeeded but an error was expected"$'\n'
  elif [[ $expect_error -eq 0 && $status -ne 0 ]]; then
    problems+="compilation failed"$'\n'
  fi
  if [[ -n "$problems" ]]; then
    report "$name" fail "$problems$log"
  else
    report "$name" ok
  fi
}

printf "${BOLD}Runtime tests${RESET} ${DIM}(tests/cases)${RESET}\n"
for src in tests/cases/*.c; do
  [[ -n "$FILTER" && "$src" != *"$FILTER"* ]] && continue
  grep -q '^// part-of:' "$src" && continue # helper source of another test
  if [[ -n "$REFERENCE" ]]; then
    grep -q '^// occ-only' "$src" && continue
    run_case "$src" ref
  else
    run_case "$src" none
    run_case "$src" prod
  fi
done

if [[ -z "$REFERENCE" ]]; then
  printf "${BOLD}Diagnostic tests${RESET} ${DIM}(tests/diag)${RESET}\n"
  for src in tests/diag/*.c; do
    [[ -n "$FILTER" && "$src" != *"$FILTER"* ]] && continue
    run_diag "$src"
  done
fi

printf "\n${BOLD}%d passed${RESET}, " "$pass"
if [[ $fail -eq 0 ]]; then
  printf "${GREEN}0 failed${RESET}\n"
else
  printf "${RED}%d failed${RESET}: %s\n" "$fail" "${failed_names[*]}"
  exit 1
fi
