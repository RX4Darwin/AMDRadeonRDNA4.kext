#!/usr/bin/env bash
# Run `air2amdgcn coverage --codegen` over every .bc under a disall.py output directory, one process per module (an LLVM fatal error must not take the
# batch down). One JSON line per module on stdout; a module whose process died gets {"file":..., "crash":<exit status>}.
#   tools/air/coverage-all.sh AIRDIR [find-args...] > coverage.jsonl
set -u
cd "$(dirname "$0")/../.."
dir=$1; shift
find "$dir" -name '*.bc' "$@" -print0 | xargs -0 -n1 -P"$(nproc)" sh -c 'out=$(build/air/air2amdgcn coverage --codegen "$1" 2>/dev/null); rc=$?; if [ $rc -eq 0 ] && [ -n "$out" ]; then echo "$out"; else echo "{\"file\":\"$1\",\"crash\":$rc}"; fi' _
