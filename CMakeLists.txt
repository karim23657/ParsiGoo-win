cmake_minimum_required(VERSION 3.16)
project(farsi_tts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MINGW)
  # Windows 7 ships the old msvcrt printf, which has no %zu/%lld — mingw's own
  # ISO-C99 printf keeps the format strings portable across both worlds.
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#   ORT_DIR : ONNX Runtime directory (contains include/ and lib/)
#             For Windows 7 use ONNX Runtime 1.14 (the last Win7 release).
#   SPM_DIR : sentencepiece source tree (contains src/ and build/src/)
#             build it once:  cmake -B build -DSPM_ENABLE_SHARED=OFF . && cmake --build build
# ---------------------------------------------------------------------------
set(ORT_DIR "" CACHE PATH "ONNX Runtime install dir (include/ + lib/)")
set(SPM_DIR "" CACHE PATH "sentencepiece source dir (src/ + build/src/)")

find_path(ORT_INCLUDE onnxruntime_c_api.h PATHS "${ORT_DIR}/include" NO_DEFAULT_PATH)
find_path(ORT_INCLUDE onnxruntime_c_api.h)
find_library(ORT_LIB onnxruntime PATHS "${ORT_DIR}/lib" NO_DEFAULT_PATH)
find_library(ORT_LIB onnxruntime)

find_path(SPM_INCLUDE sentencepiece_processor.h PATHS "${SPM_DIR}/src" NO_DEFAULT_PATH)
find_path(SPM_INCLUDE sentencepiece_processor.h)
find_library(SPM_LIB sentencepiece PATHS "${SPM_DIR}/build/src" NO_DEFAULT_PATH)
find_library(SPM_LIB sentencepiece)

if(NOT ORT_INCLUDE OR NOT ORT_LIB)
  message(FATAL_ERROR "ONNX Runtime not found — set -DORT_DIR=<dir with include/ and lib/>")
endif()
if(NOT SPM_INCLUDE OR NOT SPM_LIB)
  message(FATAL_ERROR "sentencepiece not found — set -DSPM_DIR=<sentencepiece source dir>")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
add_library(tts_core STATIC
  src/npz.cpp
  src/audio.cpp
  src/spm.cpp
  src/normalize_fa.cpp
  src/g2p.cpp
  src/dict.cpp
  src/chunk.cpp
  src/ort_util.cpp
  src/rng.cpp
  src/tts.cpp
)
target_include_directories(tts_core PUBLIC src ${ORT_INCLUDE} ${SPM_INCLUDE})
target_link_libraries(tts_core PUBLIC ${ORT_LIB} ${SPM_LIB} Threads::Threads)

add_executable(farsi-tts src/main_cli.cpp)
target_link_libraries(farsi-tts PRIVATE tts_core)
if(WIN32)
  # --play hands the finished WAV to the sound card (winmm)
  target_link_libraries(farsi-tts PRIVATE winmm)
endif()

add_executable(farsi-tts-server src/server_main.cpp)
target_link_libraries(farsi-tts-server PRIVATE tts_core)

if(WIN32)
  # gui.rc embeds the Common Controls v6 manifest (themed controls, DPI aware)
  enable_language(RC)
  add_executable(farsi-tts-gui WIN32 src/gui_win32.cpp cmake/gui.rc)
  # wininet: the model-download tab (models.json -> huggingface.co)
  target_link_libraries(farsi-tts-gui PRIVATE tts_core comctl32 comdlg32 winmm shell32 wininet)
  if(MSVC)
    # Common Controls v6 + Windows 7 compatibility manifest
    target_link_options(farsi-tts-gui PRIVATE
      "/MANIFEST:EMBED"
      "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/cmake/win7.manifest")
  endif()
endif()

if(MSVC)
  # Persian wide-string literals in gui_win32.cpp are UTF-8
  foreach(t farsi-tts farsi-tts-server farsi-tts-gui)
    if(TARGET ${t})
      target_compile_options(${t} PRIVATE /utf-8)
    endif()
  endforeach()
  # Windows 7 target + static CRT option (see docs/win7.md)
  if(WIN7_STATIC_CRT)
    foreach(t tts_core farsi-tts farsi-tts-server farsi-tts-gui)
      if(TARGET ${t})
        target_compile_options(${t} PRIVATE /MT)
        set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
      endif()
    endforeach()
  endif()
endif()
