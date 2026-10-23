#!/bin/sh
# Build and run the platform-independent unit tests with any C11 compiler.
set -e
cd "$(dirname "$0")/.."
${CC:-gcc} -std=c11 -Wall -Wextra -Wpedantic -Wshadow -O1 -g -fsanitize=address,undefined \
  -Isrc/core src/core/*.c tests/test_core.c -o tests/test_core
./tests/test_core
