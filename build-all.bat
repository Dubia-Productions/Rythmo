@echo off
setlocal EnableExtensions
rem Build a portable Windows folder (no installer). Run from any directory.
rem Requirements: Visual Studio C++ tools, CMake, Qt 6.5+ for the chosen
rem architecture, and vcpkg. Expat uses the dynamic CRT to match Qt's MSVC build.

rem Resolve the script folder without a trailing backslash. A quoted Windows
rem argument ending in backslash can escape its closing quote before CMake sees it.
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if /I not "%ARCH%"=="x64" if /I not "%ARCH%"=="ARM64" (
  echo Usage: %~nx0 [x64^|ARM64]
  exit /b 2
)
if not defined QT_ROOT (
  echo Set QT_ROOT to the Qt installation prefix for this architecture.
  echo Example: set QT_ROOT=C:\Qt\6.12.0\msvc2022_64
  exit /b 2
)
if not defined VCPKG_ROOT (
  echo Set VCPKG_ROOT to your vcpkg checkout.
  exit /b 2
)
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo vcpkg.exe not found under "%VCPKG_ROOT%".
  exit /b 2
)

set "TRIPLET=x64-windows"
if /I "%ARCH%"=="ARM64" set "TRIPLET=arm64-windows"
set "LOCAL_ROOT=%RYTHMO_BUILD_ROOT%"
if not defined LOCAL_ROOT set "LOCAL_ROOT=%LOCALAPPDATA%\Rythmo"
set "BUILD=%LOCAL_ROOT%\build\release-windows-%ARCH%"
set "DIST=%LOCAL_ROOT%\dist\windows-%ARCH%"
set "TARGET_DIST=%ROOT%\dist\Rythmo-by-Dubia-Productions-windows-%ARCH%-v8"
set "TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
set "CRYPTO_BACKUP=%BUILD%\libcrypto-3-x64.dll"
if exist "%DIST%\libcrypto-3-x64.dll" if not exist "%BUILD%" mkdir "%BUILD%"
if exist "%DIST%\libcrypto-3-x64.dll" copy /y "%DIST%\libcrypto-3-x64.dll" "%CRYPTO_BACKUP%" >nul

if not exist "%VCPKG_ROOT%\installed\%TRIPLET%\share\expat\expat-config.cmake" (
  "%VCPKG_ROOT%\vcpkg.exe" install expat:%TRIPLET%
  if errorlevel 1 exit /b %errorlevel%
)

cmake -S "%ROOT%" -B "%BUILD%" -A %ARCH% ^
  -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
  -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
  -DCMAKE_PREFIX_PATH="%QT_ROOT%" ^
  -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD%" --config Release --parallel
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir "%BUILD%" -C Release --output-on-failure
if errorlevel 1 exit /b %errorlevel%

if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"
if exist "%CRYPTO_BACKUP%" copy /y "%CRYPTO_BACKUP%" "%DIST%\libcrypto-3-x64.dll" >nul
copy /y "%BUILD%\Release\Rythmo.exe" "%DIST%\Rythmo.exe" >nul
if errorlevel 1 exit /b %errorlevel%
if not exist "%VCPKG_ROOT%\installed\%TRIPLET%\bin\*.dll" (
  echo Expat runtime DLLs not found under "%VCPKG_ROOT%\installed\%TRIPLET%\bin".
  exit /b 2
)
copy /y "%VCPKG_ROOT%\installed\%TRIPLET%\bin\*.dll" "%DIST%\" >nul
if errorlevel 1 exit /b %errorlevel%

set "DEPLOY=%QT_ROOT%\bin\windeployqt.exe"
if not exist "%DEPLOY%" (
  echo Qt deploy tool missing: "%DEPLOY%"
  exit /b 2
)
set "QTDIR=%QT_ROOT%"
set "PATH=%QT_ROOT%\bin;%PATH%"
if exist "%QT_ROOT%\bin\qtenv2.bat" call "%QT_ROOT%\bin\qtenv2.bat"
if errorlevel 1 exit /b %errorlevel%
"%DEPLOY%" --release --no-translations --compiler-runtime "%DIST%\Rythmo.exe"
if errorlevel 1 exit /b %errorlevel%
rem Qt Multimedia uses the Windows media backend. Keep its Schannel plugin and
rem drop Qt's optional OpenSSL plugin; FFmpeg is shipped separately below.
for /r "%DIST%" %%F in (qopensslbackend.dll) do if exist "%%~fF" del /q "%%~fF"
if not exist "%DIST%\Qt6Core.dll" (
  echo Qt deployment did not copy Qt6Core.dll into "%DIST%".
  exit /b 2
)
if not exist "%DIST%\platforms\qwindows.dll" (
  echo Qt deployment did not copy the Windows platform plugin.
  exit /b 2
)
rem Pin QLibraryInfo's plugin root to this portable folder. The app also
rem overrides inherited QT_PLUGIN_PATH before constructing QApplication.
(
  echo [Paths]
  echo Prefix=.
  echo Plugins=.
) > "%DIST%\qt.conf"
if not exist "%DIST%\qt.conf" (
  echo Failed to create qt.conf in the portable folder.
  exit /b 2
)

(
  echo Rythmo by Dubia Productions - Windows %ARCH%
  echo.
  echo Launch Rythmo.exe directly. This folder is portable and has no installer.
  echo Keep the DLLs and Qt plugin folders beside the executable.
  echo Open a .detx file from the Fichier menu. Video files remain external.
  echo FFmpeg decodes all codecs included in the shipped FFmpeg build.
) > "%DIST%\README-portable.txt"

set "FFMPEG_SOURCE="
for /f "delims=" %%F in ('where ffmpeg.exe 2^>nul') do if not defined FFMPEG_SOURCE set "FFMPEG_SOURCE=%%F"
if defined FFMPEG_SOURCE copy /y "%FFMPEG_SOURCE%" "%DIST%\ffmpeg.exe" >nul
if not defined FFMPEG_SOURCE echo FFmpeg not found in PATH. Add ffmpeg.exe beside Rythmo.exe for video decoding.

where powershell.exe >nul 2>nul
if not errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; Compress-Archive -Path '%DIST%\*' -DestinationPath '%LOCAL_ROOT%\dist\Rythmo-by-Dubia-Productions-windows-%ARCH%-portable.zip' -Force"
  if errorlevel 1 exit /b 1
)
echo Portable output: %DIST%\Rythmo.exe
echo No installer was created.
echo Updating requested distribution folder: %TARGET_DIST%
robocopy "%DIST%" "%TARGET_DIST%" /E /R:2 /W:2
if errorlevel 8 exit /b 1
echo Updated distribution folder: %TARGET_DIST%
exit /b 0
