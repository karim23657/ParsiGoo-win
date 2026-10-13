@echo off
REM ---------------------------------------------------------------
REM  Build the native engine on Windows (Visual Studio 2019 or 2022).
REM
REM  Windows 7 target: use the v142 toolset and the Windows 7.1 SDK
REM  (see docs\win7.md).  Set the paths below first.
REM
REM    set ORT_DIR=C:\deps\onnxruntime-win-x64-1.14.1
REM    set SPM_DIR=C:\deps\sentencepiece        (source tree, built)
REM ---------------------------------------------------------------
setlocal

if "%ORT_DIR%"=="" set ORT_DIR=C:\deps\onnxruntime-win-x64-1.14.1
if "%SPM_DIR%"=="" set SPM_DIR=C:\deps\sentencepiece

cmake -S . -B build-win -G "Visual Studio 16 2019" -A x64 ^
  -DORT_DIR="%ORT_DIR%" -DSPM_DIR="%SPM_DIR%" -DWIN7_STATIC_CRT=ON
if errorlevel 1 goto :err

cmake --build build-win --config Release --parallel
if errorlevel 1 goto :err

echo.
echo Built:
echo   build-win\Release\farsi-tts.exe
echo   build-win\Release\farsi-tts-server.exe
echo   build-win\Release\farsi-tts-gui.exe
echo.
echo Copy the engine next to the model folder and run prepare_win7_onnx.py first:
echo   python scripts\prepare_win7_onnx.py model\onnx
goto :eof

:err
echo BUILD FAILED
exit /b 1
