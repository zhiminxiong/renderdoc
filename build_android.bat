@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  Build the RenderDocCmd Android APKs for both arm64-v8a (64-bit) and
REM  armeabi-v7a (32-bit) in one go.
REM
REM  Usage:
REM     build_android.bat            build both ABIs
REM     build_android.bat arm64      build only 64-bit
REM     build_android.bat arm32      build only 32-bit
REM     build_android.bat clean      wipe build dirs first, then build both
REM
REM  The default tool paths below were recovered from the previous successful
REM  build on this machine. If any real environment variable of the same name
REM  is already set it takes priority; otherwise these defaults are used.
REM ===========================================================================

REM ------------------------- user-configurable -------------------------------
if not defined JAVA_HOME        set "JAVA_HOME=D:\jdk-23.0.2"
if not defined ANDROID_HOME     set "ANDROID_HOME=C:\Users\alexxiong\AppData\Local\Android\Sdk"
if not defined ANDROID_NDK_HOME set "ANDROID_NDK_HOME=D:\NDK\android-ndk-r21e"

REM full path to cmake.exe and ninja.exe used previously
if not defined CMAKE            set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
if not defined NINJA            set "NINJA=D:\OpenHarmonySDK\12\native\build-tools\cmake\bin\ninja.exe"

if not defined GENERATOR        set "GENERATOR=Ninja"
REM leave BUILD_TYPE empty to match the previous configuration; set to Release/Debug to override
if not defined BUILD_TYPE       set "BUILD_TYPE="
REM ---------------------------------------------------------------------------

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUTDIR=%ROOT%\out-android"

REM ---- sanity checks ----
set "FATAL="
if not exist "%JAVA_HOME%\bin\java.exe" (
  echo [ERROR] JDK not found at "%JAVA_HOME%". Fix JAVA_HOME at the top of this script.
  set "FATAL=1"
)
if not exist "%ANDROID_HOME%\build-tools" (
  echo [ERROR] Android SDK not found at "%ANDROID_HOME%". Fix ANDROID_HOME.
  set "FATAL=1"
)
if not exist "%ANDROID_NDK_HOME%\build\cmake\android.toolchain.cmake" (
  echo [ERROR] Android NDK not found at "%ANDROID_NDK_HOME%". Fix ANDROID_NDK_HOME.
  set "FATAL=1"
)
if not exist "%CMAKE%" (
  echo [ERROR] cmake not found at "%CMAKE%". Fix CMAKE.
  set "FATAL=1"
)
if not exist "%NINJA%" (
  echo [ERROR] ninja not found at "%NINJA%". Fix NINJA.
  set "FATAL=1"
)
if defined FATAL goto :fail

REM make sure ninja's folder is on PATH too (some steps look it up by name)
for %%I in ("%NINJA%") do set "NINJA_DIR=%%~dpI"
set "PATH=%NINJA_DIR%;%PATH%"

echo(
echo ============================================================
echo  JAVA_HOME        = %JAVA_HOME%
echo  ANDROID_HOME     = %ANDROID_HOME%
echo  ANDROID_NDK_HOME = %ANDROID_NDK_HOME%
echo  cmake            = %CMAKE%
echo  ninja            = %NINJA%
echo  Generator        = %GENERATOR%
if defined BUILD_TYPE echo  Build type       = %BUILD_TYPE%
echo ============================================================
echo(

REM ---- parse argument ----
set "ARG=%~1"
set "DO_CLEAN="
set "BUILD64=1"
set "BUILD32=1"

if /I "%ARG%"=="clean" set "DO_CLEAN=1"
if /I "%ARG%"=="arm64" ( set "BUILD64=1" & set "BUILD32=" )
if /I "%ARG%"=="arm32" ( set "BUILD64=" & set "BUILD32=1" )

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

if defined BUILD64 (
  call :build arm64-v8a arm64 || goto :fail
)
if defined BUILD32 (
  call :build armeabi-v7a arm32 || goto :fail
)

echo(
echo ============================================================
echo  Build finished. APKs collected in:
echo    %OUTDIR%
echo ------------------------------------------------------------
dir /b "%OUTDIR%\*.apk"
echo ============================================================
endlocal
exit /b 0

REM ===========================================================================
REM  :build  <ANDROID_ABI>  <tag>
REM ===========================================================================
:build
set "ABI=%~1"
set "TAG=%~2"
set "BDIR=%ROOT%\build-android-%TAG%"
set "PKG=org.renderdoc.renderdoccmd.%TAG%"
set "APK=%BDIR%\bin\%PKG%.apk"

echo(
echo ---- Building %ABI% (%TAG%) --------------------------------

if defined DO_CLEAN (
  if exist "%BDIR%" (
    echo   cleaning %BDIR%
    rmdir /s /q "%BDIR%"
  )
)

if not exist "%BDIR%" mkdir "%BDIR%"

set "BUILDTYPE_ARG="
if defined BUILD_TYPE set "BUILDTYPE_ARG=-DCMAKE_BUILD_TYPE=%BUILD_TYPE%"

echo   configuring...
"%CMAKE%" -S "%ROOT%" -B "%BDIR%" -G "%GENERATOR%" ^
    "-DCMAKE_MAKE_PROGRAM=%NINJA%" ^
    -DBUILD_ANDROID=On ^
    -DANDROID_ABI=%ABI% ^
    %BUILDTYPE_ARG%
if errorlevel 1 (
  echo [ERROR] CMake configure failed for %ABI%
  exit /b 1
)

echo   building apk target...
"%CMAKE%" --build "%BDIR%" --target apk
if errorlevel 1 (
  echo [ERROR] Build failed for %ABI%
  exit /b 1
)

if not exist "%APK%" (
  echo [ERROR] Expected APK not found: %APK%
  exit /b 1
)

copy /y "%APK%" "%OUTDIR%\" >nul
echo   OK -^> %OUTDIR%\%PKG%.apk
exit /b 0

:fail
echo(
echo [FAILED] See the errors above.
endlocal
exit /b 1
