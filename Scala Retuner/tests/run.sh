#!/bin/sh
# Build and run the unit tests on this machine. Usage: sh tests/run.sh (from the project folder)
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/scala_retuner_tests"
c++ -std=c++17 -O2 -Wall -Wextra -I../../framework -o "$OUT" tests/test_retuner.cpp -lpthread
MPC_PLUGIN_FOLDER="$(pwd)" "$OUT"
