@echo off
rem CardiacVisualizer one-command dev launcher (like "npm run dev").
rem Usage:
rem   dev.bat                 - build (if needed) + run dashboard (synthetic demo)
rem   dev.bat --live [hint]   - build + run dashboard in LIVE BLE mode
rem   dev.bat --beats N       - build + run dashboard demo with N beats
rem   dev.bat --test          - build + run core tests (no window)
rem   dev.bat --configure     - force CMake re-configure, then build + run
rem   dev.bat --clean         - delete the build directory
rem   dev.bat --help          - show this help
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set "ROOT=%CD%"
rem build2 is the live "MinGW Makefiles" cache. The old .\build folder holds a
rem stale "Unix Makefiles" cache with no make.exe, so building against it fails
rem with "No rule to make target" - which is what broke `dev.bat` for local runs.
set "BUILD_DIR=%ROOT%\build2"

rem ---- locate cmake: portable .tools first, PATH fallback ----
set "CMAKE_EXE="
for /f "delims=" %%C in ('dir /b /s "%ROOT%\.tools\cmake.exe" 2^>nul') do (
  if not defined CMAKE_EXE set "CMAKE_EXE=%%C"
)
if not defined CMAKE_EXE (
  where cmake >nul 2>nul
  if !errorlevel! equ 0 (
    for /f "delims=" %%C in ('where cmake 2^>nul') do (
      if not defined CMAKE_EXE set "CMAKE_EXE=%%C"
    )
  )
)
if not defined CMAKE_EXE (
  echo [dev] ERROR: cmake not found. Expected portable toolchain under .tools\ ^(see README^).
  exit /b 1
)

rem ---- locate llvm-mingw bin (for MinGW Makefiles generator) ----
set "LLVM_BIN="
for /f "delims=" %%G in ('dir /b /s "%ROOT%\.tools\clang++.exe" 2^>nul') do (
  if not defined LLVM_BIN set "LLVM_BIN=%%~dpG"
)
if defined LLVM_BIN set "PATH=!LLVM_BIN!;!PATH!"
set "CTEST_EXE=!CMAKE_EXE:cmake.exe=ctest.exe!"

set "MODE=run"
set "FORCE_CONFIGURE="
set "APP_ARGS="
:parse
if "%~1"=="" goto doneparse
if "%~1"=="--test" ( set "MODE=test" & shift & goto parse )
if "%~1"=="--import" ( set "MODE=import" & shift & goto parse )
if "%~1"=="--configure" ( set "FORCE_CONFIGURE=1" & shift & goto parse )
if "%~1"=="--clean" ( set "MODE=clean" & shift & goto parse )
if "%~1"=="--help" ( set "MODE=help" & shift & goto parse )
if "%~1"=="-h" ( set "MODE=help" & shift & goto parse )
set "APP_ARGS=%APP_ARGS% %1"
shift
goto parse
:doneparse

if "%MODE%"=="help" (
  echo Usage: dev.bat [--live [name_hint]] [--beats N] [--session ^<id^>] [--no-autoload] [--test] [--import] [--configure] [--clean] [--help]
  echo   default: build + import CHF2DB data + run dashboard ^(auto-loads chf2db-chf207^).
  echo   --import: build + import ./data into exports\cardiac_history.db, then exit.
  echo   --test runs core tests instead. --no-autoload starts on the synthetic demo.
  exit /b 0
)
if "%MODE%"=="clean" (
  echo [dev] Removing %BUILD_DIR% ...
  rmdir /s /q "%BUILD_DIR%"
  exit /b 0
)

rem ---- configure on first run (or when forced) ----
if defined FORCE_CONFIGURE (
  echo [dev] Forcing CMake re-configure ...
  if defined LLVM_BIN (
    set "CC=!LLVM_BIN!clang.exe"
    set "CXX=!LLVM_BIN!clang++.exe"
  )
  "%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCARDIAC_BUILD_DASHBOARD=ON
  if errorlevel 1 ( echo [dev] Configure FAILED. & exit /b 1 )
) else if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo [dev] First run: configuring CMake ...
  if defined LLVM_BIN (
    set "CC=!LLVM_BIN!clang.exe"
    set "CXX=!LLVM_BIN!clang++.exe"
  )
  "%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCARDIAC_BUILD_DASHBOARD=ON
  if errorlevel 1 ( echo [dev] Configure FAILED. & exit /b 1 )
)

rem ---- build ----
echo [dev] Building ...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
  echo [dev] Build FAILED. Please check errors above.
  exit /b 1
)

if "%MODE%"=="test" (
  echo [dev] Running core tests ...
  "%BUILD_DIR%\cardiac_core_tests.exe"
  exit /b !errorlevel!
)

rem ---- import CHF2DB IBI data into the runtime SQLite history DB ----
rem Idempotent (INSERT OR REPLACE). Non-fatal: the dashboard still runs if the
rem data folder or importer is unavailable.
if exist "%BUILD_DIR%\cardiac_import_chf2db.exe" (
  if exist "%ROOT%\data" (
    echo [dev] Importing CHF2DB IBI data into exports\cardiac_history.db ...
    "%BUILD_DIR%\cardiac_import_chf2db.exe" "%ROOT%\data" "%ROOT%\exports\cardiac_history.db"
    if errorlevel 1 echo [dev] WARNING: import reported an issue ^(continuing^).
  ) else (
    echo [dev] No .\data folder found; skipping CHF2DB import.
  )
) else (
  echo [dev] Importer not built ^(SQLite3 missing?^); skipping CHF2DB import.
)

if "%MODE%"=="import" (
  echo [dev] Import-only mode; not launching the dashboard.
  exit /b 0
)

rem ---- run dashboard (blocking, like "npm run dev") ----
echo [dev] Starting Cardiac Dashboard%APP_ARGS% ...
"%BUILD_DIR%\cardiac_dashboard_app.exe"%APP_ARGS%
exit /b !errorlevel!
