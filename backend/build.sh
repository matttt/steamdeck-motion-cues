#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)

cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/build"
cmake --build "$SCRIPT_DIR/build"

mkdir -p "$SCRIPT_DIR/out"
mkdir -p "$SCRIPT_DIR/../bin"
cp "$SCRIPT_DIR/build/SteamDeckMotionCues" "$SCRIPT_DIR/out/"
cp "$SCRIPT_DIR/build/SteamDeckMotionCues" "$SCRIPT_DIR/../bin/"
