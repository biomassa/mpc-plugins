#!/bin/sh
# Build and run the unit tests on this machine. Usage: sh tests/run.sh (from the project folder)
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}"
c++ -std=c++17 -O2 -Wall -Wextra -I../../framework -o "$OUT/scalaseq_engine_tests" tests/test_engine.cpp
"$OUT/scalaseq_engine_tests"
if [ -f tests/test_plugin.cpp ]; then
  c++ -std=c++17 -O2 -Wall -Wextra -I../../framework -o "$OUT/scalaseq_plugin_tests" tests/test_plugin.cpp -lpthread
  MPC_PLUGIN_FOLDER="$(pwd)" "$OUT/scalaseq_plugin_tests"
fi
