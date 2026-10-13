#!/usr/bin/env bash
# Linux build (verification host; the Windows build lives in build_win.bat).
#
# Expects:
#   deps/onnxruntime-linux-x64-<ver>/   ONNX Runtime release tarball
#   deps/sentencepiece-<ver>/           sentencepiece built static
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

ORT_DIR=${ORT_DIR:-$(ls -d "$ROOT"/deps/onnxruntime-linux-x64-* 2>/dev/null | head -1)}
SPM_DIR=${SPM_DIR:-$(ls -d "$ROOT"/deps/sentencepiece-* 2>/dev/null | grep -v '\.tgz' | head -1)}
SPM_LIB=${SPM_LIB:-$SPM_DIR/build/src/libsentencepiece.a}

SRCS=("$ROOT/src/npz.cpp" "$ROOT/src/audio.cpp" "$ROOT/src/spm.cpp"
      "$ROOT/src/normalize_fa.cpp" "$ROOT/src/g2p.cpp" "$ROOT/src/dict.cpp"
      "$ROOT/src/chunk.cpp"
      "$ROOT/src/ort_util.cpp" "$ROOT/src/rng.cpp" "$ROOT/src/tts.cpp")

mkdir -p build
g++ -std=c++17 -O2 -DNDEBUG \
  -I "$ORT_DIR/include" -I "$SPM_DIR/src" -I "$ROOT/src" \
  "${SRCS[@]}" "$ROOT/src/main_cli.cpp" \
  -o "$ROOT/build/farsi-tts" \
  "$SPM_LIB" -L "$ORT_DIR/lib" -lonnxruntime -lpthread -ldl \
  -Wl,-rpath,"$ORT_DIR/lib"

g++ -std=c++17 -O2 -DNDEBUG \
  -I "$ORT_DIR/include" -I "$SPM_DIR/src" -I "$ROOT/src" \
  "${SRCS[@]}" "$ROOT/src/server_main.cpp" \
  -o "$ROOT/build/farsi-tts-server" \
  "$SPM_LIB" -L "$ORT_DIR/lib" -lonnxruntime -lpthread -ldl \
  -Wl,-rpath,"$ORT_DIR/lib"

echo "built build/farsi-tts and build/farsi-tts-server"
