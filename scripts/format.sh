#!/usr/bin/env bash
# format.sh [--check]  — used by CI (Linux) and git-bash.
# Prefer a pinned major when available (CI sets CLANG_FORMAT=clang-format-18).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CHECK=0
case "${1:-}" in
  --check|-n|--dry-run) CHECK=1 ;;
esac

CF="${CLANG_FORMAT:-}"
if [[ -z "$CF" ]] || ! command -v "$CF" >/dev/null 2>&1; then
  CF=""
  # Prefer pinned majors first so local matches CI when multiple are installed.
  for c in clang-format-18 clang-format-17 clang-format-16 clang-format-15 clang-format; do
    if command -v "$c" >/dev/null 2>&1; then CF="$c"; break; fi
  done
fi
if [[ -z "$CF" ]] || ! command -v "$CF" >/dev/null 2>&1; then
  echo "clang-format not found (want clang-format-18 for CI parity)" >&2
  exit 2
fi

echo "clang-format: $($CF --version | head -n1)"

mapfile -t FILES < <(find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | sort)
if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No source files under src/"
  exit 1
fi

if [[ "$CHECK" -eq 1 ]]; then
  fail=0
  tmpdir=$(mktemp -d)
  trap 'rm -rf "$tmpdir"' EXIT
  for f in "${FILES[@]}"; do
    out="$tmpdir/$(echo "$f" | tr '/\\' '__')"
    # Always materialize formatted output and diff — more reliable than --dry-run --Werror
    # (which can fail for non-format reasons while hiding stderr).
    if ! "$CF" --style=file "$f" >"$out" 2>"$tmpdir/cf.err"; then
      echo "NEED FORMAT: $f (clang-format failed)"
      cat "$tmpdir/cf.err" >&2 || true
      fail=1
      continue
    fi
    if ! diff -u "$f" "$out" >"$tmpdir/diff.out"; then
      echo "NEED FORMAT: $f"
      # Cap diff noise but always show the hunk so CI logs are actionable.
      head -n 200 "$tmpdir/diff.out" || true
      fail=1
    fi
  done
  if [[ "$fail" -ne 0 ]]; then
    echo "Format check FAILED (${#FILES[@]} files). Run: scripts/format.sh"
    exit 1
  fi
  echo "Format check OK (${#FILES[@]} files) via $CF"
  exit 0
fi

for f in "${FILES[@]}"; do
  "$CF" -i --style=file "$f"
done
echo "Formatted ${#FILES[@]} files via $CF"
