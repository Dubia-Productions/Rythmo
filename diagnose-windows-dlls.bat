@echo off
setlocal EnableExtensions
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
set "DIST=%ROOT%\dist\windows-x64"
set "APP=%DIST%\Rythmo.exe"
set "LOG=%ROOT%\diagnose-qt-plugins.log"

echo === Rythmo portable DLL diagnostic ===
echo Project: "%ROOT%"
echo App:     "%APP%"
echo QT_ROOT=%QT_ROOT%
echo QTDIR=%QTDIR%
echo QT_PLUGIN_PATH=%QT_PLUGIN_PATH%
echo QT_QPA_PLATFORM_PLUGIN_PATH=%QT_QPA_PLATFORM_PLUGIN_PATH%
echo.

if not exist "%APP%" (
  echo ERROR: portable executable not found. Run build-all.bat x64 first.
  exit /b 2
)

echo === Portable Qt configuration ===
if exist "%DIST%\qt.conf" (type "%DIST%\qt.conf") else echo ERROR: qt.conf is missing.
if exist "%DIST%\tls\qopensslbackend.dll" (echo ERROR: OpenSSL plugin is present in dist.) else echo OpenSSL plugin is absent from dist\tls.
echo.

echo === Direct DLL imports of libcrypto ===
where dumpbin.exe >nul 2>nul
if errorlevel 1 (
  echo dumpbin.exe not found. Run this file from the Visual Studio 2022 Developer Command Prompt.
) else (
  for /r "%DIST%" %%F in (*.dll *.exe) do call :scan "%%~fF"
)
echo.

if defined QT_ROOT (
  echo === OpenSSL plugins in Qt installation ===
  for /r "%QT_ROOT%" %%F in (qopensslbackend.dll) do if exist "%%~fF" echo %%~fF
  echo.
)

echo === Launching with Qt plugin diagnostics ===
echo If the app window opens, close it. If a missing-DLL dialog appears, close it too.
set "QT_DEBUG_PLUGINS=1"
pushd "%DIST%"
"%APP%" >"%LOG%" 2>&1
set "APP_EXIT=%ERRORLEVEL%"
popd
set "QT_DEBUG_PLUGINS="
echo App exit code: %APP_EXIT%
echo Qt diagnostic log: "%LOG%"
echo Attach that log if the missing libcrypto dialog still appears.
exit /b 0

:scan
dumpbin.exe /nologo /dependents "%~1" 2>nul | findstr.exe /i "libcrypto" >nul
if not errorlevel 1 echo IMPORTS libcrypto: %~1
exit /b 0
