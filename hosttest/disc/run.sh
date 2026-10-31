#!/bin/sh
set -eu
cd "$(dirname "$0")"
bin=$(mktemp)
trap 'rm -f "$bin"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-function \
    -Ifake -fsanitize=address,undefined -g test.c -o "$bin"
# LeakSanitizer cannot run in some hosted/ptraced environments.
ASAN_OPTIONS=detect_leaks=0 "$bin"
