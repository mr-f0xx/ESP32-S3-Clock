#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror tests/clock_layout_test.cpp -o "$out"
"$out"
