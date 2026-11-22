#!/bin/sh
# Build and run the tests on this machine. Usage: sh tests/run.sh (from the project folder)
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}"
c++ -std=c++17 -O2 -Wall -Wextra -I../../framework -o "$OUT/percseq_plugin_tests" tests/test_plugin.cpp -lpthread
MPC_PLUGIN_FOLDER="$(pwd)" "$OUT/percseq_plugin_tests"
