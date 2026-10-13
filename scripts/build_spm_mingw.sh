#!/usr/bin/env bash
# Cross-compile sentencepiece to a static Windows x64 library.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SPM_DIR=${SPM_DIR:-$(ls -d "$ROOT"/deps/sentencepiece-* | grep -v '\.tgz' | head -1)}
BUILD="$SPM_DIR/build-mingw"

cat > "$BUILD.toolchain.cmake" 2>/dev/null || true
mkdir -p "$BUILD"
cat > "$BUILD/toolchain.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

cmake -S "$SPM_DIR" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSPM_ENABLE_SHARED=OFF \
  -DSPM_ENABLE_TCMALLOC=OFF \
  > /tmp/spm_mingw_cmake.log 2>&1
cmake --build "$BUILD" --parallel 4 > /tmp/spm_mingw_make.log 2>&1
echo "built $BUILD/libsentencepiece.a"
ls -la "$BUILD"/libsentencepiece.a