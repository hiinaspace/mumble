@echo off
:: package_portable_win.bat
:: Produces a self-contained portable zip of the Mumble client from a local build.
::
:: Usage:
::   Run via PowerShell from the repo root:
::     powershell -NonInteractive -Command "& 'scripts\package_portable_win.bat'"
::
:: Override defaults by setting env vars before calling:
::   set BUILD_DIR=S:\code\mumble\build
::   set ZIP_OUT=C:\somewhere\mumble-test.zip

setlocal enabledelayedexpansion

:: ── Configurable paths ───────────────────────────────────────────────────────
if not defined BUILD_DIR set "BUILD_DIR=S:\code\mumble\build"
if not defined ZIP_OUT   set "ZIP_OUT=S:\code\mumble\mumble-hrtf-test-win64.zip"
set "DIST_DIR=%TEMP%\mumble_portable_dist"

:: ── Find VC143 redist dir (picks the highest-versioned subdir) ───────────────
set "VC_REDIST_BASE=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC"
set "VC_REDIST="
for /d %%V in ("%VC_REDIST_BASE%\1*") do set "VC_REDIST=%%V\x64\Microsoft.VC143.CRT"
if not defined VC_REDIST (
    echo ERROR: No VC143 redist found under %VC_REDIST_BASE%
    exit /b 1
)
if not exist "%VC_REDIST%\msvcp140.dll" (
    echo ERROR: msvcp140.dll not found in: %VC_REDIST%
    exit /b 1
)

echo Build dir : %BUILD_DIR%
echo Staging   : %DIST_DIR%
echo VC redist : %VC_REDIST%
echo Output    : %ZIP_OUT%
echo.

:: ── Stage ────────────────────────────────────────────────────────────────────
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

echo [1/3] cmake --install ...
cmake --install "%BUILD_DIR%" --prefix "%DIST_DIR%"
if errorlevel 1 ( echo FAILED: cmake install & exit /b 1 )

:: ── VC runtime DLLs ──────────────────────────────────────────────────────────
echo [2/3] Copying VC runtime DLLs ...
for %%F in (msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll) do (
    copy "%VC_REDIST%\%%F" "%DIST_DIR%\" > nul
    if errorlevel 1 ( echo FAILED: could not copy %%F & exit /b 1 )
)

:: ── Zip ──────────────────────────────────────────────────────────────────────
echo [3/3] Creating zip ...
if exist "%ZIP_OUT%" del "%ZIP_OUT%"
7z a -mx=5 "%ZIP_OUT%" "%DIST_DIR%\*"
if errorlevel 1 ( echo FAILED: 7z & exit /b 1 )

echo.
echo Done: %ZIP_OUT%
