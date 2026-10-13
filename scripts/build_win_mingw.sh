#!/usr/bin/env bash
# Cross-compile the Windows x64 binaries with MinGW-w64.
#
#   apt-get install -y g++-mingw-w64-x86-64
#   scripts/build_win_mingw.sh
#
# Needs:
#   deps/ort_win/onnxruntime-win-x64-1.14.1/   ONNX Runtime Windows release
#   deps/sentencepiece-<ver>/                   sentencepiece built for Windows
#                                               (scripts/build_spm_mingw.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

ORT_DIR=${ORT_DIR:-$(ls -d "$ROOT"/deps/ort_win/onnxruntime-win-x64-* | head -1)}
SPM_DIR=${SPM_DIR:-$(ls -d "$ROOT"/deps/sentencepiece-* | grep -v '\.tgz' | head -1)}
SPM_BUILD=${SPM_BUILD:-$SPM_DIR/build-mingw}
# the mingw build drops the static lib under src/
SPM_LIB="$SPM_BUILD/libsentencepiece.a"
[ -f "$SPM_LIB" ] || SPM_LIB="$SPM_BUILD/src/libsentencepiece.a"
OUT=${OUT:-$ROOT/build-win}

CXX=x86_64-w64-mingw32-g++
mkdir -p "$OUT"

CORE_SRC=("$ROOT/src/npz.cpp" "$ROOT/src/audio.cpp" "$ROOT/src/spm.cpp"
          "$ROOT/src/normalize_fa.cpp" "$ROOT/src/g2p.cpp" "$ROOT/src/dict.cpp"
          "$ROOT/src/chunk.cpp"
          "$ROOT/src/ort_util.cpp" "$ROOT/src/rng.cpp" "$ROOT/src/tts.cpp")

CXXFLAGS=(-std=c++17 -O2 -DNDEBUG -Wall
          -I "$ORT_DIR/include" -I "$SPM_DIR/src" -I "$ROOT/src"
          -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++
          # Windows 7 ships the old msvcrt printf, which has no %zu/%lld — use
          # mingw's own ISO-C99 printf so every format string is portable.
          -D__USE_MINGW_ANSI_STDIO=1
          # onnxruntime_c_api.h hardcodes MSVC's `_stdcall` for _WIN32, which
          # MinGW g++ rejects (it only knows __stdcall)
          -D_stdcall=__stdcall)

cp -f "$ORT_DIR/lib/onnxruntime.dll" "$OUT/"

echo ">> patching onnxruntime.dll for Windows 7 (api-set imports -> w7shim.dll)"
python3 "$ROOT/scripts/patch_ort_win7.py" "$OUT/onnxruntime.dll" \
  --shim-out "$OUT/shim"

echo ">> w7shim.dll"
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc \
  -I "$OUT/shim" \
  "$ROOT/src/win7shim/win7shim.c" "$OUT/shim/win7shim_gen.S" \
  "$OUT/shim/win7shim.def" \
  -o "$OUT/w7shim.dll" -Wl,--dynamicbase,--nxcompat

echo ">> api-set stand-in: api-ms-win-core-localization-l1-2-0.dll"
python3 "$ROOT/scripts/gen_apiset_shim.py" \
  api-ms-win-core-localization-l1-2-0.dll --out "$OUT/nls"
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc \
  -I "$OUT/nls" \
  "$ROOT/src/win7shim/win7shim.c" "$OUT/nls/win7shim_gen.S" \
  "$OUT/nls/win7shim.def" \
  -o "$OUT/api-ms-win-core-localization-l1-2-0.dll" \
  -Wl,--dynamicbase,--nxcompat

echo ">> w7shim-test.exe (diagnostic + unit test)"
x86_64-w64-mingw32-gcc -O2 -I "$OUT/shim" \
  "$ROOT/src/win7shim/test_w7shim.c" "$OUT/shim/win7shim_gen.S" \
  -o "$OUT/w7shim-test.exe"

echo ">> VC runtime (app-local, Microsoft redistributable)"
if [ -d "$ROOT/deps/vcruntime" ]; then
  cp -f "$ROOT"/deps/vcruntime/*.dll "$OUT/"
fi

# onnxruntime.dll is deliberately *not* linked in (-lonnxruntime): it is
# loaded at run time by ort_util.cpp so that a problem inside the DLL can
# never take the process down before main() runs (Windows 7: 0xc0000005).
echo ">> CLI"
"$CXX" "${CXXFLAGS[@]}" "${CORE_SRC[@]}" "$ROOT/src/main_cli.cpp" \
  -o "$OUT/farsi-tts.exe" \
  "$SPM_LIB" -lws2_32 -lwinmm

echo ">> server"
"$CXX" "${CXXFLAGS[@]}" "${CORE_SRC[@]}" "$ROOT/src/server_main.cpp" \
  -o "$OUT/farsi-tts-server.exe" \
  "$SPM_LIB" -lws2_32

echo ">> GUI"
# gui.rc embeds the Common Controls v6 manifest: themed controls + DPI aware
x86_64-w64-mingw32-windres -O coff -I "$ROOT/cmake" "$ROOT/cmake/gui.rc" "$OUT/gui_res.o"
"$CXX" "${CXXFLAGS[@]}" -mwindows -municode "${CORE_SRC[@]}" "$ROOT/src/gui_win32.cpp" \
  "$OUT/gui_res.o" \
  -o "$OUT/farsi-tts-gui.exe" \
  "$SPM_LIB" \
  -lcomctl32 -lcomdlg32 -lwinmm -lshell32 -lole32 -luuid -lwininet

echo "built in $OUT:"
ls -la "$OUT"